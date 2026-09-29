// ===========================================================================
// Sistema IoT de Monitoreo Hídrico — Challenge 2
// Internet de las Cosas 2026-2 · Universidad de La Sabana
//
// Arquitectura (wiki, páginas 02c, 02e y 02f):
//   ISR del timer  --notifica-->  TareaMedicion (núcleo 0)
//   TareaMedicion  --cola qLecturas-->  TareaFusionAlerta (núcleo 1)
//   TareaFusionAlerta: fusión, alarma, LCD/LEDs/buzzer, histórico, SSE
//   Servidor web (tarea de AsyncTCP): tablero, API y silenciar alarma
//   loop(): solo vigila el Wi-Fi
//
// La alerta física no depende de la red: si el Wi-Fi cae, todo lo local sigue.
//
// Librerías: ESP Async WebServer y Async TCP (ESP32Async), DHT sensor library,
// Adafruit BMP280, Adafruit Unified Sensor, LiquidCrystal I2C.
// Antes de compilar: copiar secrets.h.example como secrets.h.
// ===========================================================================
#include <Wire.h>
#include "config.h"
#include "tipos.h"
#include "recursos.h"
#include "sistema.h"
#include "sensores.h"
#include "fusion.h"
#include "actuacion.h"
#include "historico.h"
#include "servidor.h"

// ----------------------------- Recursos compartidos ------------------------
SemaphoreHandle_t mtxI2C;
SemaphoreHandle_t mtxEstado;
static QueueHandle_t qLecturas;
static TaskHandle_t  hTareaMedicion;
static hw_timer_t*   timerMedicion = nullptr;

EstadoSistema estado = {};
DiagMedicion  diag   = {0, UINT32_MAX, 0, 0, 0};

static MaquinaAlarma alarma;
static VentanaNivel  ventana;

static const int     LARGO_COLA  = 4;
static const uint32_t TICK_FUSION_MS = 250;  // refresco de LCD, buzzer y órdenes del tablero

// ----------------------------- ISR -----------------------------------------
// Solo notifica a la tarea: leer el DHT22 o el bus I2C dentro de una ISR
// bloquearía las demás interrupciones y dispararía el watchdog.
void IRAM_ATTR onTimerMedicion() {
  BaseType_t despertarTarea = pdFALSE;
  vTaskNotifyGiveFromISR(hTareaMedicion, &despertarTarea);
  portYIELD_FROM_ISR(despertarTarea);
}

static void iniciarTimer() {
  const uint64_t periodo_us = (uint64_t)PERIODO_MEDICION_MS * 1000ULL;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  timerMedicion = timerBegin(1000000);                 // 1 MHz: 1 tick = 1 µs
  timerAttachInterrupt(timerMedicion, &onTimerMedicion);
  timerAlarm(timerMedicion, periodo_us, true, 0);      // recarga automática
#else
  timerMedicion = timerBegin(0, 80, true);             // 80 MHz / 80 = 1 MHz
  timerAttachInterrupt(timerMedicion, &onTimerMedicion, true);
  timerAlarmWrite(timerMedicion, periodo_us, true);
  timerAlarmEnable(timerMedicion);
#endif
}

// ----------------------------- Tarea de medición (núcleo 0) ----------------
static void TareaMedicion(void*) {
  uint32_t anterior_us = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // espera la ISR

    uint32_t ahora_us = micros();
    if (anterior_us != 0) {
      uint32_t periodo = ahora_us - anterior_us;
      diag.ultimo_us = periodo;
      if (periodo < diag.min_us) diag.min_us = periodo;
      if (periodo > diag.max_us) diag.max_us = periodo;
    }
    anterior_us = ahora_us;

    Lectura l;
    sensoresLeer(l);

    // Si la cola está llena se descarta la lectura más antigua
    if (xQueueSend(qLecturas, &l, 0) != pdTRUE) {
      Lectura descartada;
      xQueueReceive(qLecturas, &descartada, 0);
      xQueueSend(qLecturas, &l, 0);
      diag.perdidas++;
    }
    diag.muestras++;
  }
}

// ----------------------------- Tarea de fusión y alerta (núcleo 1) ---------
static void registrarFallasSensor(const Lectura& l, const Lectura& previa, bool primera) {
  // Solo se registra cuando el sensor pasa de bien a mal, no cada lectura fallida
  if (!l.nivel_ok && (primera || previa.nivel_ok)) eventoAgregar(l.t, "HC-SR04 sin eco");
  if (!l.dht_ok   && (primera || previa.dht_ok))   eventoAgregar(l.t, "DHT22 sin lectura valida");
  if (!l.bmp_ok   && (primera || previa.bmp_ok))   eventoAgregar(l.t, "BMP280 sin lectura valida");
}

static void procesarLectura(const Lectura& l) {
  ResultadoFusion f = fusionar(l, ventana);

  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  registrarFallasSensor(l, estado.lectura, estado.muestras == 0);
  EstadoAlarma antes = alarma.estado();
  bool cambio = alarma.actualizar(f.riesgo, ventana.llena());
  EstadoAlarma ahora = alarma.estado();

  estado.lectura      = l;
  estado.fusion       = f;
  estado.alarma       = ahora;
  estado.ventanaLlena = ventana.llena();
  estado.muestras++;

  Registro r = {l.t, l.nivel_cm, l.temp_c, l.hum_pct, l.presion_hpa,
                f.evaporacion, f.riesgo, (uint8_t)ahora};
  historicoAgregar(r);

  if (cambio) {
    if (ahora == ALERTA) eventoAgregar(l.t, "ALERTA: riesgo %.2f", f.riesgo);
    else if (antes == SILENCIADA) eventoAgregar(l.t, "Rearme automatico: riesgo %.2f", f.riesgo);
    else eventoAgregar(l.t, "Estado %s: riesgo %.2f", nombreEstado(ahora), f.riesgo);
  }
  xSemaphoreGive(mtxEstado);

  // Línea CSV por el puerto serie para registrar datos de las pruebas
  Serial.printf("%lu,%.1f,%.1f,%.1f,%.1f,%u,%.3f,%.2f,%.3f,%s,%s\n",
                (unsigned long)l.t, l.nivel_cm, l.temp_c, l.hum_pct, l.presion_hpa, l.ldr,
                f.evaporacion, f.tasa_cm, f.riesgo, nombreEstado(ahora), nombreCausa(f.causa));

  servidorPublicarLectura();
  if (cambio && ahora == ALERTA) servidorPublicarAlerta();
}

static void TareaFusionAlerta(void*) {
  Lectura l;
  EstadoSistema copia;
  for (;;) {
    if (xQueueReceive(qLecturas, &l, pdMS_TO_TICKS(TICK_FUSION_MS)) == pdTRUE) {
      procesarLectura(l);
    }
    // Se actualiza la actuación en cada tick aunque no haya lectura nueva, para
    // rotar el LCD y aplicar en < 250 ms una orden de silenciar del tablero.
    xSemaphoreTake(mtxEstado, portMAX_DELAY);
    copia = estado;
    xSemaphoreGive(mtxEstado);
    actuacionActualizar(copia, wifiConectado(), wifiIP());
  }
}

// ----------------------------- Acciones desde el tablero -------------------
bool alarmaSilenciar(const IPAddress& cliente) {
  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  bool ok = alarma.silenciar();
  if (ok) {
    estado.alarma = alarma.estado();
    eventoAgregar(millis(), "Silenciada desde %s", cliente.toString().c_str());
  }
  xSemaphoreGive(mtxEstado);
  return ok;
}

bool alarmaRearmar(const IPAddress& cliente) {
  xSemaphoreTake(mtxEstado, portMAX_DELAY);
  bool ok = alarma.rearmar();
  if (ok) {
    estado.alarma = alarma.estado();
    eventoAgregar(millis(), "Rearmada desde %s", cliente.toString().c_str());
  }
  xSemaphoreGive(mtxEstado);
  return ok;
}

// ----------------------------- setup / loop --------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("== Sistema IoT de Monitoreo Hidrico - Challenge 2 ==");

  mtxI2C    = xSemaphoreCreateMutex();
  mtxEstado = xSemaphoreCreateMutex();
  qLecturas = xQueueCreate(LARGO_COLA, sizeof(Lectura));

  Wire.begin(PIN_SDA, PIN_SCL);
  actuacionIniciar();
  if (!sensoresIniciar()) {
    Serial.println("ERROR: no se detecto el BMP280 (revisar I2C, direccion 0x76)");
    eventoAgregar(millis(), "BMP280 no detectado al iniciar");
  }

  servidorIniciar();

  xTaskCreatePinnedToCore(TareaMedicion, "medicion", 4096, nullptr,
                          PRIO_MEDICION, &hTareaMedicion, CORE_MEDICION);
  xTaskCreatePinnedToCore(TareaFusionAlerta, "fusion", 6144, nullptr,
                          PRIO_FUSION, nullptr, CORE_FUSION);

  Serial.println("t_ms,nivel_cm,temp_c,hum_pct,presion_hpa,ldr,evaporacion,tasa_cm,riesgo,estado,causa");
  iniciarTimer();
  xTaskNotifyGive(hTareaMedicion);  // primera medición sin esperar el periodo
}

void loop() {
  servidorMantener();
  vTaskDelay(pdMS_TO_TICKS(1000));
}
