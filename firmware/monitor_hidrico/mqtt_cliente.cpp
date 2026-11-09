// ---------------------------------------------------------------------------
// mqtt_cliente.cpp — Cliente MQTT del nodo (Challenge 3).
//
// Base: primera versión de Ana María Cordero (cliente PubSubClient, reintento
// de conexión cada 5 s y mensaje JSON con la medición). Esta versión:
//   - corre en su propia tarea FreeRTOS (TareaMQTT, núcleo 0, prioridad 1),
//     como exige la restricción 1 del enunciado para la transmisión;
//   - usa los temas de la wiki (02e, sección 4) y el mismo JSON de /api/estado;
//   - avisa su desconexión con un Last Will y recibe las órdenes del gateway.
//
// Temas (<nodo> = NODO_ID en config.h):
//   sabana/<nodo>/lectura     ESP32 -> gateway  JSON de cada medición (cada 5 s)
//   sabana/<nodo>/alerta      ESP32 -> gateway  cambio de estado de la alarma, retenido
//   sabana/<nodo>/conexion    ESP32 -> gateway  "en_linea"; "desconectado" como Last Will
//   sabana/<nodo>/cmd/alarma  gateway -> ESP32  "silenciar" o "rearmar"
//
// QoS: PubSubClient solo publica con QoS 0. Para no perder alertas, cada
// cambio de estado espera en la cola qMQTT hasta que se publica con la
// conexión activa, y se publica retenido: el gateway recibe el último estado
// aunque se conecte después. El Last Will y la suscripción a las órdenes usan
// QoS 1. Una lectura perdida no se reintenta: la siguiente la reemplaza.
// ---------------------------------------------------------------------------
#include "mqtt_cliente.h"
#include "config.h"
#include "secrets.h"
#include "recursos.h"
#include "sistema.h"
#include "servidor.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>

// Cliente TCP sobre Wi-Fi y cliente MQTT que lo usa.
static WiFiClient   clienteWiFi;
static PubSubClient clienteMQTT(clienteWiFi);

static TaskHandle_t  hTareaMQTT = nullptr;
static QueueHandle_t qMQTT      = nullptr;  // cambios de alarma pendientes de publicar
static volatile bool conectado  = false;    // copia del estado para otras tareas

// Un cambio de estado de la alarma, tal como se envía al gateway.
struct CambioAlarma {
  uint32_t      t;
  EstadoAlarma  estado;
  float         riesgo;
  CausaDescenso causa;
  OrigenCambio  origen;
};

static char temaLectura[48];
static char temaAlerta[48];
static char temaConexion[48];
static char temaCmd[48];
static char idCliente[40];

// Print sobre un arreglo fijo: arma el JSON de la lectura sin memoria dinámica.
class SalidaBuffer : public Print {
 public:
  char   buf[MQTT_TAM_MENSAJE];
  size_t n     = 0;
  bool   lleno = false;
  void reiniciar() { n = 0; lleno = false; buf[0] = '\0'; }
  size_t write(uint8_t c) override {
    if (n + 1 >= sizeof(buf)) { lleno = true; return 0; }
    buf[n++] = (char)c;
    buf[n]   = '\0';
    return 1;
  }
};

static const char* nombreOrigen(OrigenCambio o) {
  switch (o) {
    case ORIGEN_TABLERO_LOCAL: return "tablero_local";
    case ORIGEN_UBIDOTS:       return "ubidots";
    default:                   return "automatico";
  }
}

// ----------------------------- Publicación ---------------------------------
static bool publicarCambio(const CambioAlarma& c) {
  char riesgo[12];
  if (isnan(c.riesgo)) strcpy(riesgo, "null");
  else snprintf(riesgo, sizeof(riesgo), "%.3f", c.riesgo);

  char json[192];
  snprintf(json, sizeof(json),
           "{\"nodo\":\"%s\",\"t\":%lu,\"estado\":\"%s\",\"riesgo\":%s,"
           "\"causa\":\"%s\",\"origen\":\"%s\"}",
           NODO_ID, (unsigned long)c.t, nombreEstado(c.estado), riesgo,
           nombreCausa(c.causa), nombreOrigen(c.origen));

  bool ok = clienteMQTT.publish(temaAlerta, json, true);  // retenido
  Serial.printf("MQTT: alerta %s %s\n", nombreEstado(c.estado), ok ? "publicada" : "NO publicada");
  return ok;
}

static void publicarLectura() {
  static SalidaBuffer json;  // estático: no ocupa la pila de la tarea
  json.reiniciar();

  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  escribirEstadoJSON(json, NODO_ID);
  xSemaphoreGive(mtxEstado);

  if (json.lleno) {
    Serial.println("MQTT: lectura demasiado larga, no se publica (subir MQTT_TAM_MENSAJE)");
    return;
  }
  if (!clienteMQTT.publish(temaLectura, json.buf)) {  // QoS 0, no retenido
    Serial.println("MQTT: error al publicar la lectura");
  }
}

// ----------------------------- Órdenes del gateway -------------------------
static void alRecibirOrden(char* tema, byte* datos, unsigned int largo) {
  if (strcmp(tema, temaCmd) != 0) return;

  char orden[16];
  unsigned int n = largo < sizeof(orden) - 1 ? largo : sizeof(orden) - 1;
  memcpy(orden, datos, n);
  orden[n] = '\0';

  bool ok;
  if      (strcmp(orden, "silenciar") == 0) ok = alarmaSilenciarRemoto();
  else if (strcmp(orden, "rearmar") == 0)   ok = alarmaRearmarRemoto();
  else {
    Serial.printf("MQTT: orden desconocida '%s'\n", orden);
    return;
  }
  // Una orden que no aplica (p. ej., silenciar sin ALERTA) se rechaza sin
  // efecto; por eso un mensaje QoS 1 duplicado no causa problemas.
  Serial.printf("MQTT: orden '%s' %s\n", orden, ok ? "aplicada" : "rechazada");
}

// ----------------------------- Conexión ------------------------------------
static bool conectarBroker() {
  Serial.printf("MQTT: conectando a %s:%d... ", MQTT_HOST, MQTT_PORT);

  // Last Will: si el nodo se cae sin avisar, el broker publica "desconectado".
  bool ok;
#if defined(MQTT_USER) && defined(MQTT_PASS)
  ok = clienteMQTT.connect(idCliente, MQTT_USER, MQTT_PASS,
                           temaConexion, 1, true, "desconectado");
#else
  // Sin MQTT_USER/MQTT_PASS en secrets.h: conexión sin usuario (solo para
  // pruebas; exige allow_anonymous true en Mosquitto).
  ok = clienteMQTT.connect(idCliente, temaConexion, 1, true, "desconectado");
#endif

  if (!ok) {
    // -2: el broker no responde (IP, puerto o "listener 1883" en Mosquitto)
    //  4: usuario o contraseña incorrectos · 5: no autorizado
    Serial.printf("fallo, estado %d\n", clienteMQTT.state());
    return false;
  }

  clienteMQTT.publish(temaConexion, "en_linea", true);
  clienteMQTT.subscribe(temaCmd, 1);
  Serial.println("conectado");
  return true;
}

// ----------------------------- Tarea ---------------------------------------
static void TareaMQTT(void*) {
  uint32_t ultimoIntento = 0;
  bool     primerIntento = true;

  for (;;) {
    // Espera el aviso de una lectura nueva, o MQTT_TICK_MS para atender las
    // órdenes y mantener viva la conexión.
    bool hayLectura = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MQTT_TICK_MS)) > 0;

    if (WiFi.status() != WL_CONNECTED) {  // loop() se encarga de reconectar el Wi-Fi
      conectado = false;
      continue;
    }

    if (!clienteMQTT.connected()) {
      conectado = false;
      if (primerIntento || millis() - ultimoIntento >= MQTT_REINTENTO_MS) {
        primerIntento = false;
        ultimoIntento = millis();
        conectarBroker();
      }
      if (!clienteMQTT.connected()) continue;
    }
    conectado = true;

    clienteMQTT.loop();  // recibe órdenes y envía el keep-alive

    // Primero los cambios de alarma pendientes, en orden. Si uno falla, queda
    // en la cola y se reintenta en la siguiente vuelta.
    CambioAlarma c;
    while (xQueuePeek(qMQTT, &c, 0) == pdTRUE) {
      if (!publicarCambio(c)) break;
      xQueueReceive(qMQTT, &c, 0);
    }

    if (hayLectura) publicarLectura();
  }
}

// ----------------------------- API pública ---------------------------------
void mqttIniciar() {
  snprintf(temaLectura,  sizeof(temaLectura),  "sabana/%s/lectura",    NODO_ID);
  snprintf(temaAlerta,   sizeof(temaAlerta),   "sabana/%s/alerta",     NODO_ID);
  snprintf(temaConexion, sizeof(temaConexion), "sabana/%s/conexion",   NODO_ID);
  snprintf(temaCmd,      sizeof(temaCmd),      "sabana/%s/cmd/alarma", NODO_ID);
  snprintf(idCliente,    sizeof(idCliente),    "esp32_%s",             NODO_ID);

  clienteMQTT.setServer(MQTT_HOST, MQTT_PORT);
  clienteMQTT.setBufferSize(MQTT_TAM_MENSAJE + 64);  // + encabezado y tema
  clienteMQTT.setKeepAlive(MQTT_KEEPALIVE_S);        // Last Will en ~1.5 × keep-alive
  clienteMQTT.setSocketTimeout(5);                   // no quedarse 15 s esperando al broker
  clienteMQTT.setCallback(alRecibirOrden);

  qMQTT = xQueueCreate(MQTT_LARGO_COLA, sizeof(CambioAlarma));
  xTaskCreatePinnedToCore(TareaMQTT, "mqtt", 6144, nullptr,
                          PRIO_MQTT, &hTareaMQTT, CORE_MQTT);
}

void mqttAvisarLectura() {
  if (hTareaMQTT) xTaskNotifyGive(hTareaMQTT);
}

void mqttAvisarCambioAlarma(uint32_t t, EstadoAlarma estado, float riesgo,
                            CausaDescenso causa, OrigenCambio origen) {
  if (!qMQTT) return;
  CambioAlarma c = {t, estado, riesgo, causa, origen};
  if (xQueueSend(qMQTT, &c, 0) != pdTRUE) {
    // Cola llena: el broker lleva mucho tiempo caído. Se descarta el cambio
    // más antiguo; el más reciente es el que refleja el estado actual.
    CambioAlarma viejo;
    xQueueReceive(qMQTT, &viejo, 0);
    xQueueSend(qMQTT, &c, 0);
  }
}

bool mqttConectado() { return conectado; }
