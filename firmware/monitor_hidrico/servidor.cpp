// ---------------------------------------------------------------------------
// servidor.cpp — Implementación del servidor web embebido.
// Librería: ESPAsyncWebServer + AsyncTCP (ESP32Async). Las peticiones se
// atienden en la tarea de AsyncTCP, sin bloquear la medición ni la alarma.
// ---------------------------------------------------------------------------
#include "servidor.h"
#include "mqtt_cliente.h"
#include "config.h"
#include "recursos.h"
#include "sistema.h"
#include "historico.h"
#include "tablero_html.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Falta secrets.h: copie secrets.h.example como secrets.h y complete los datos"
#endif

static AsyncWebServer                server(80);
static AsyncEventSource              events("/events");
static AsyncAuthenticationMiddleware autenticacion;

static char     ipTexto[16] = "0.0.0.0";
static uint32_t ultimoIntentoWifi = 0;
static const uint32_t REINTENTO_WIFI_MS = 10000;

// ----------------------------- Control de acceso ---------------------------
static bool ipPermitida(const IPAddress& ip) {
  uint32_t mascara = (uint32_t)WiFi.subnetMask();
  bool mismaSubred = ((uint32_t)ip & mascara) == ((uint32_t)WiFi.localIP() & mascara);
  if (!mismaSubred) return false;
  if (!FILTRAR_POR_LISTA) return true;
  for (const IPAddress& autorizada : IPS_AUTORIZADAS) {
    if (autorizada == ip) return true;
  }
  return false;
}

// ----------------------------- JSON ----------------------------------------
static void imprimirNum(Print& out, float v, int decimales) {
  if (isnan(v)) out.print("null");
  else out.print(v, decimales);
}

// Requiere mtxEstado tomado. También lo usa mqtt_cliente.cpp (mensaje `lectura`).
void escribirEstadoJSON(Print& out, const char* nodo) {
  const Lectura& l = estado.lectura;
  const ResultadoFusion& f = estado.fusion;
  out.print("{");
  if (nodo) out.printf("\"nodo\":\"%s\",", nodo);
  out.printf("\"uptime_ms\":%lu,\"t\":%lu,\"muestras\":%lu,",
             (unsigned long)millis(), (unsigned long)l.t, (unsigned long)estado.muestras);
  out.print("\"nivel_cm\":");        imprimirNum(out, l.nivel_cm, 1);
  out.print(",\"nivel_pct\":");      imprimirNum(out, f.nivel_norm * 100.0f, 1);
  out.print(",\"tasa_cm_muestra\":"); imprimirNum(out, f.tasa_cm, 2);
  out.print(",\"temp_c\":");         imprimirNum(out, l.temp_c, 1);
  out.print(",\"hum_pct\":");        imprimirNum(out, l.hum_pct, 1);
  out.print(",\"presion_hpa\":");    imprimirNum(out, l.presion_hpa, 1);
  out.printf(",\"ldr\":%u", l.ldr);
  out.print(",\"evaporacion\":");    imprimirNum(out, f.evaporacion, 3);
  out.print(",\"riesgo\":");         imprimirNum(out, f.riesgo, 3);
  out.printf(",\"estado\":\"%s\",\"alarma_silenciada\":%s,\"causa\":\"%s\"",
             nombreEstado(estado.alarma),
             estado.alarma == SILENCIADA ? "true" : "false",
             nombreCausa(f.causa));
  out.printf(",\"ventana_llena\":%s,\"sensores\":{\"nivel\":%s,\"dht22\":%s,\"bmp280\":%s}}",
             estado.ventanaLlena ? "true" : "false",
             l.nivel_ok ? "true" : "false",
             l.dht_ok ? "true" : "false",
             l.bmp_ok ? "true" : "false");
}

// Imprime en un String (para los eventos SSE).
class SalidaString : public Print {
 public:
  String s;
  size_t write(uint8_t c) override { s += (char)c; return 1; }
  size_t write(const uint8_t* b, size_t n) override { s.concat((const char*)b, n); return n; }
};

// ----------------------------- Manejadores ---------------------------------
static void enviarJSON(AsyncWebServerRequest* req, void (*escribir)(Print&, int), int n) {
  AsyncResponseStream* resp = req->beginResponseStream("application/json");
  resp->addHeader("Cache-Control", "no-store");
  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  escribir(*resp, n);
  xSemaphoreGive(mtxEstado);
  req->send(resp);
}

static void manejarAlarma(AsyncWebServerRequest* req, bool silenciar) {
  IPAddress cliente = req->client()->remoteIP();
  bool ok = silenciar ? alarmaSilenciar(cliente) : alarmaRearmar(cliente);
  if (!ok) {
    req->send(409, "application/json",
              silenciar ? "{\"error\":\"La alarma no esta en ALERTA\"}"
                        : "{\"error\":\"La alarma no esta SILENCIADA\"}");
    return;
  }
  servidorPublicarLectura();  // todos los tableros ven el cambio de inmediato
  enviarJSON(req, [](Print& out, int) { escribirEstadoJSON(out); }, 0);
}

static void configurarRutas() {
  // 1) Filtro por subred / lista de IPs, antes que la autenticación
  server.addMiddleware([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
    if (!ipPermitida(req->client()->remoteIP())) {
      req->send(403, "text/plain", "Dispositivo no autorizado");
      return;
    }
    next();
  });

  // 2) Autenticación HTTP Digest en todas las rutas, incluida /events
  autenticacion.setUsername(WEB_USER);
  autenticacion.setPassword(WEB_PASS);
  autenticacion.setRealm("Monitor hidrico");
  autenticacion.setAuthFailureMessage("Autenticacion requerida");
  autenticacion.setAuthType(AsyncAuthType::AUTH_DIGEST);
  autenticacion.generateHash();
  server.addMiddleware(&autenticacion);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html; charset=utf-8", TABLERO_HTML);
  });

  server.on("/api/estado", HTTP_GET, [](AsyncWebServerRequest* req) {
    enviarJSON(req, [](Print& out, int) { escribirEstadoJSON(out); }, 0);
  });

  server.on("/api/historico", HTTP_GET, [](AsyncWebServerRequest* req) {
    int n = HISTORICO_MUESTRAS;
    if (req->hasParam("n")) n = req->getParam("n")->value().toInt();
    enviarJSON(req, historicoEscribirJSON, n);
  });

  server.on("/api/eventos", HTTP_GET, [](AsyncWebServerRequest* req) {
    enviarJSON(req, [](Print& out, int) { eventosEscribirJSON(out); }, 0);
  });

  // Diagnóstico para las pruebas C-01, C-02 y R-05
  server.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* req) {
    enviarJSON(req, [](Print& out, int) {
      out.printf("{\"uptime_ms\":%lu,\"heap_libre\":%lu,\"heap_min\":%lu,\"rssi_dbm\":%d,"
                 "\"periodo_ultimo_us\":%lu,\"periodo_min_us\":%lu,\"periodo_max_us\":%lu,"
                 "\"muestras\":%lu,\"perdidas\":%lu,\"clientes_sse\":%u,\"mqtt_conectado\":%s}",
                 (unsigned long)millis(), (unsigned long)ESP.getFreeHeap(),
                 (unsigned long)ESP.getMinFreeHeap(), WiFi.RSSI(),
                 (unsigned long)diag.ultimo_us, (unsigned long)diag.min_us,
                 (unsigned long)diag.max_us, (unsigned long)diag.muestras,
                 (unsigned long)diag.perdidas, (unsigned)events.count(),
                 mqttConectado() ? "true" : "false");
    }, 0);
  });

  server.on("/api/alarma/silenciar", HTTP_POST, [](AsyncWebServerRequest* req) {
    manejarAlarma(req, true);
  });
  server.on("/api/alarma/rearmar", HTTP_POST, [](AsyncWebServerRequest* req) {
    manejarAlarma(req, false);
  });

  server.addHandler(&events);

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "No encontrado");
  });
}

// ----------------------------- Wi-Fi ---------------------------------------
static void alCambiarWifi(WiFiEvent_t evento) {
  if (evento == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    snprintf(ipTexto, sizeof(ipTexto), "%s", WiFi.localIP().toString().c_str());
    xSemaphoreTake(mtxEstado, portMAX_DELAY);
    eventoAgregar(millis(), "WiFi conectado, IP %s", ipTexto);
    xSemaphoreGive(mtxEstado);
  } else if (evento == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    xSemaphoreTake(mtxEstado, portMAX_DELAY);
    eventoAgregar(millis(), "WiFi desconectado");
    xSemaphoreGive(mtxEstado);
  }
}

void servidorIniciar() {
  WiFi.onEvent(alCambiarWifi);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NOMBRE_MDNS);
  WiFi.setSleep(false);          // menor latencia del tablero
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ultimoIntentoWifi = millis();

  if (MDNS.begin(NOMBRE_MDNS)) MDNS.addService("http", "tcp", 80);

  configurarRutas();
  server.begin();
}

void servidorMantener() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - ultimoIntentoWifi < REINTENTO_WIFI_MS) return;
  ultimoIntentoWifi = millis();
  WiFi.reconnect();
}

bool wifiConectado() { return WiFi.status() == WL_CONNECTED; }
const char* wifiIP() { return ipTexto; }

// ----------------------------- SSE -----------------------------------------
void servidorPublicarLectura() {
  if (events.count() == 0) return;
  SalidaString json;
  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  escribirEstadoJSON(json);
  xSemaphoreGive(mtxEstado);
  events.send(json.s.c_str(), "lectura", millis());
}

void servidorPublicarAlerta() {
  if (events.count() == 0) return;
  SalidaString json;
  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  escribirEstadoJSON(json);
  xSemaphoreGive(mtxEstado);
  events.send(json.s.c_str(), "alerta", millis());
}
