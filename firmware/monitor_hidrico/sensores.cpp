// ---------------------------------------------------------------------------
// sensores.cpp — Implementación del módulo de adquisición.
// ---------------------------------------------------------------------------
#include "sensores.h"
#include "config.h"
#include "recursos.h"
#include <DHT.h>
#include <Adafruit_BMP280.h>

static DHT             dht(PIN_DHT, DHTTYPE);
static Adafruit_BMP280 bmp;
static bool            bmpPresente = false;

// Últimos valores válidos (se reusan cuando un sensor falla)
static float ultNivel   = TANQUE_ALTURA_CM;  // hasta la primera lectura, se asume lleno
static float ultTemp    = NAN;
static float ultHum     = NAN;
static float ultPresion = NAN;
static int   fallosNivelSeguidos = 0;

bool sensoresIniciar() {
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
  analogReadResolution(12);

  dht.begin();

  xSemaphoreTake(mtxI2C, portMAX_DELAY);
  bmpPresente = bmp.begin(BMP280_ADDR);
  if (bmpPresente) {
    // Modo normal, sobremuestreo moderado: suficiente para variables lentas
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
  }
  xSemaphoreGive(mtxI2C);
  return bmpPresente;
}

// Distancia sensor–superficie en cm, o NAN si no hubo eco.
static float leerDistanciaCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // 30 ms de timeout ≈ 5 m ida y vuelta, más que el rango del sensor
  unsigned long duracion = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duracion == 0) return NAN;

  float d = duracion * 0.0343f / 2.0f;  // velocidad del sonido ≈ 343 m/s
  if (d < 2.0f || d > 400.0f) return NAN;  // fuera del rango de la hoja de datos
  return d;
}

void sensoresLeer(Lectura& l) {
  l.t = millis();

  // ---- Nivel ----
  float d = leerDistanciaCM();
  if (isnan(d)) {
    fallosNivelSeguidos++;
    l.nivel_ok = false;
    // Falla segura: tras varias lecturas sin eco se asume tanque vacío,
    // para que el sistema alerte en lugar de quedarse con un nivel viejo.
    if (fallosNivelSeguidos >= FALLOS_NIVEL_MAX) ultNivel = 0.0f;
  } else {
    fallosNivelSeguidos = 0;
    l.nivel_ok = true;
    ultNivel = constrain(TANQUE_ALTURA_CM - d, 0.0f, TANQUE_ALTURA_CM);
  }
  l.nivel_cm = ultNivel;

  // ---- DHT22 ----
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  l.dht_ok = !(isnan(t) || isnan(h));
  if (l.dht_ok) { ultTemp = t; ultHum = h; }
  l.temp_c  = ultTemp;
  l.hum_pct = ultHum;

  // ---- BMP280 (bus I2C compartido con el LCD) ----
  float p = NAN;
  if (bmpPresente) {
    xSemaphoreTake(mtxI2C, portMAX_DELAY);
    p = bmp.readPressure() / 100.0f;
    xSemaphoreGive(mtxI2C);
  }
  l.bmp_ok = !isnan(p) && p > 300.0f && p < 1100.0f;
  if (l.bmp_ok) ultPresion = p;
  l.presion_hpa = ultPresion;

  // ---- LDR ----
  l.ldr = analogRead(PIN_LDR);
}
