#include <Wire.h>
#include <DHT.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <LiquidCrystal_I2C.h>

// ---------------------------------------------------------------------------
// CONFIGURACIÓN DE PINES
// ---------------------------------------------------------------------------
#define PIN_TRIG        5
#define PIN_ECHO        18
#define PIN_DHT         4
#define PIN_LDR         34   // ADC1 — no interfiere con WiFi/BLE apagados
#define PIN_LED_R       25
#define PIN_LED_G       26
#define PIN_LED_Y       33
#define PIN_RELAY_BUZZ  32

#define DHTTYPE DHT22

// I2C compartido por BME280 y OLED (SDA=21, SCL=22 por defecto en ESP32)
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C
// Dirección I2C del BMP180: 0x77

// ---------------------------------------------------------------------------
// PARÁMETROS DEL SISTEMA / CALIBRACIÓN
// ---------------------------------------------------------------------------
const float TANQUE_ALTURA_CM   = 100.0;  // altura total del tanque (ajustar a tu maqueta)
const float NIVEL_CRITICO_CM   = 20.0;   // nivel mínimo aceptable antes de considerar riesgo

const int   VENTANA_MUESTRAS   = 6;      // tamaño de la ventana móvil (buffer circular)
const unsigned long INTERVALO_MEDICION_MS = 5000; // en prototipo real: 10-15 min con deep-sleep

// Pesos de la lógica de fusión (ajustables/documentar en la Wiki cómo se calibraron)
const float W1_NIVEL = 0.5;
const float W2_TASA   = 0.3;
const float W3_EVAP   = 0.2;

const float UMBRAL_RIESGO = 0.65; // umbral que dispara la alerta (0.0 - 1.0)

// ---------------------------------------------------------------------------
// OBJETOS DE SENSORES / ACTUADORES
// ---------------------------------------------------------------------------
DHT dht(PIN_DHT, DHTTYPE);
Adafruit_BMP280 bmp;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------------------------------------------------------------------
// BUFFER CIRCULAR PARA VENTANA MÓVIL (nivel de agua, para tasa de descenso)
// ---------------------------------------------------------------------------
float bufferNivel[VENTANA_MUESTRAS];
int indiceBuffer = 0;
bool bufferLleno = false;

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("== Sistema IoT de Monitoreo Hidrico - Iniciando ==");

  // Pines de sensado
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  // Pines de actuación
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT);
  pinMode(PIN_RELAY_BUZZ, OUTPUT);
  digitalWrite(PIN_RELAY_BUZZ, LOW);

  // DHT22
  dht.begin();

  // I2C compartido por BMP280 + LCD
  Wire.begin(21, 22);

  // BMP280
  if (!bmp.begin(0x76)) {
    Serial.println("ERROR: No se detecto el BMP280. Verifica conexion I2C.");
  }

  // LCD 16x2
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Monitoreo");
  lcd.setCursor(0, 1);
  lcd.print("Hidrico");
  delay(2000);

 


  // Inicializar buffer
  for (int i = 0; i < VENTANA_MUESTRAS; i++) {
    bufferNivel[i] = -1;
  }

  delay(1000);
  Serial.println("Sistema listo.\n");
}

// ---------------------------------------------------------------------------
// LOOP PRINCIPAL
// ---------------------------------------------------------------------------
void loop() {
  // ---- 1. SENSADO ----
  float distanciaCM = leerDistanciaHCSR04();
  float nivelAguaCM = TANQUE_ALTURA_CM - distanciaCM;
  if (nivelAguaCM < 0) nivelAguaCM = 0;
  if (nivelAguaCM > TANQUE_ALTURA_CM) nivelAguaCM = TANQUE_ALTURA_CM;

  float temperatura = dht.readTemperature();
  float humedad     = dht.readHumidity();
  float presionHPa  = bmp.readPressure() / 100.0F;
  int   lecturaLDR  = analogRead(PIN_LDR); // 0-4095, proxy de radiación

  // Validar lecturas del DHT22 (puede fallar ocasionalmente)
  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("ADVERTENCIA: Lectura invalida del DHT22, se usa el ultimo valor valido.");
    temperatura = 20.0; // valor de respaldo conservador para no romper el calculo
    humedad = 60.0;
  }

  // ---- 2. ADQUISICIÓN: actualizar buffer circular (ventana móvil) ----
  bufferNivel[indiceBuffer] = nivelAguaCM;
  indiceBuffer = (indiceBuffer + 1) % VENTANA_MUESTRAS;
  if (indiceBuffer == 0) bufferLleno = true;

  // ---- 3. LÓGICA DE FUSIÓN ----
  float nivelNormalizado = nivelAguaCM / TANQUE_ALTURA_CM; // 0 (vacío) a 1 (lleno)
  float tasaDescenso = calcularTasaDescenso();               // cm/muestra, positivo = bajando
  float indiceEvaporacion = calcularIndiceEvaporacion(temperatura, humedad, lecturaLDR);

  // riesgo = w1*(1 - nivel) + w2*tasa_normalizada + w3*evaporacion
  float tasaNormalizada = constrain(tasaDescenso / 5.0, 0.0, 1.0); // normalizar a rango esperado
  float riesgo = W1_NIVEL * (1.0 - nivelNormalizado)
               + W2_TASA  * tasaNormalizada
               + W3_EVAP  * indiceEvaporacion;
  riesgo = constrain(riesgo, 0.0, 1.0);

  // ---- 4. DECISIÓN DE ALERTA ----
  bool alertaActiva = (riesgo > UMBRAL_RIESGO) && bufferLleno;

  // ---- 5. ACTUACIÓN IN SITU ----
  actualizarOLED(nivelAguaCM, temperatura, humedad, presionHPa, riesgo, alertaActiva);
  actualizarLEDSemaforo(riesgo);
  actualizarBuzzer(alertaActiva);

  // ---- LOG por consola (útil para depurar en Wokwi) ----
  Serial.println("---------------------------------------------");
  Serial.print("Nivel agua (cm): "); Serial.println(nivelAguaCM);
  Serial.print("Temp (C): ");        Serial.println(temperatura);
  Serial.print("Humedad (%): ");     Serial.println(humedad);
  Serial.print("Presion (hPa): ");   Serial.println(presionHPa);
  Serial.print("LDR (raw): ");       Serial.println(lecturaLDR);
  Serial.print("Tasa descenso: ");   Serial.println(tasaDescenso);
  Serial.print("Indice evap.: ");    Serial.println(indiceEvaporacion);
  Serial.print("RIESGO: ");          Serial.println(riesgo);
  Serial.print("ALERTA: ");          Serial.println(alertaActiva ? "SI" : "NO");

  delay(INTERVALO_MEDICION_MS);
}

// ---------------------------------------------------------------------------
// FUNCIÓN: leer distancia con HC-SR04 (cm)
// ---------------------------------------------------------------------------
float leerDistanciaHCSR04() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long duracion = pulseIn(PIN_ECHO, HIGH, 30000); // timeout 30ms (~5m máx)
  if (duracion == 0) {
    Serial.println("ADVERTENCIA: HC-SR04 sin eco (fuera de rango o error).");
    return TANQUE_ALTURA_CM; // asume tanque vacío por seguridad ante fallo de sensor
  }

  float distanciaCM = duracion * 0.0343 / 2.0;
  return distanciaCM;
}

// ---------------------------------------------------------------------------
// FUNCIÓN: calcular tasa de descenso a partir del buffer circular
// Retorna cm de descenso entre la muestra más antigua y la más reciente,
// normalizado por el número de muestras (aproxima cm por intervalo).
// ---------------------------------------------------------------------------
float calcularTasaDescenso() {
  if (!bufferLleno) return 0.0; // aún no hay suficiente historial

  int idxMasAntiguo = indiceBuffer; // por el wrap-around del buffer circular
  int idxMasReciente = (indiceBuffer - 1 + VENTANA_MUESTRAS) % VENTANA_MUESTRAS;

  float nivelAntiguo = bufferNivel[idxMasAntiguo];
  float nivelReciente = bufferNivel[idxMasReciente];

  float delta = nivelAntiguo - nivelReciente; // positivo si el nivel bajó
  return delta / VENTANA_MUESTRAS;
}

// ---------------------------------------------------------------------------
// FUNCIÓN: índice de evaporación potencial (versión simplificada tipo Hargreaves)
// Combina temperatura, humedad y proxy de radiación solar en un índice 0-1.
// NOTA: esta es una aproximación pedagógica, no el modelo Hargreaves completo;
// debe documentarse así en la Wiki (sección de supuestos y limitaciones).
// ---------------------------------------------------------------------------
float calcularIndiceEvaporacion(float tempC, float humedadPct, int lecturaLDR) {
  float factorTemp = constrain((tempC - 10.0) / 25.0, 0.0, 1.0);       // más calor = más evaporación
  float factorHumedad = constrain((100.0 - humedadPct) / 100.0, 0.0, 1.0); // menos humedad = más evaporación
  float factorRadiacion = constrain(lecturaLDR / 4095.0, 0.0, 1.0);    // más luz = más evaporación

  float indice = (factorTemp + factorHumedad + factorRadiacion) / 3.0;
  return constrain(indice, 0.0, 1.0);
}

// ------------------------------------------------------------
// FUNCIÓN: actualizar pantalla LCD 16x2
// ------------------------------------------------------------

void actualizarOLED(float nivel, float temp, float hum, float presion, float riesgo, bool alerta) {

  static unsigned long ultimaActualizacion = 0;
  static int pagina = 0;

  // Cambiar de pantalla cada 3 segundos
  if (millis() - ultimaActualizacion >= 3000) {
    ultimaActualizacion = millis();
    pagina++;

    if (pagina > 2) {
      pagina = 0;
    }

    lcd.clear();
  }

  // Página 1: Nivel y temperatura
  if (pagina == 0) {

    lcd.setCursor(0, 0);
    lcd.print("Nivel:");
    lcd.print(nivel, 1);
    lcd.print(" cm");

    lcd.setCursor(0, 1);
    lcd.print("Temp:");
    lcd.print(temp, 1);
    lcd.print(" C");
  }

  // Página 2: Humedad y presión
  else if (pagina == 1) {

    lcd.setCursor(0, 0);
    lcd.print("Hum:");
    lcd.print(hum, 1);
    lcd.print(" %");

    lcd.setCursor(0, 1);
    lcd.print("Pres:");
    lcd.print(presion, 0);
    lcd.print(" hPa");
  }

  // Página 3: Riesgo y estado
  else if (pagina == 2) {

    lcd.setCursor(0, 0);
    lcd.print("Riesgo:");
    lcd.print(riesgo * 100, 0);
    lcd.print("%");

    lcd.setCursor(0, 1);

    if (alerta) {
      lcd.print("!!! ALERTA !!!");
    }
    else if (riesgo > 0.4) {
      lcd.print("PRECAUCION");
    }
    else {
      lcd.print("Estado: NORMAL");
    }
  }
} 

// ---------------------------------------------------------------------------
// FUNCIÓN: LED semáforo (verde / ámbar / rojo) según nivel de riesgo
// ---------------------------------------------------------------------------
void actualizarLEDSemaforo(float riesgo) {
  if (riesgo > UMBRAL_RIESGO) {
    // Rojo
    digitalWrite(PIN_LED_R, HIGH);
    digitalWrite(PIN_LED_G, LOW);
    digitalWrite(PIN_LED_Y, LOW);
  } else if (riesgo > 0.4) {
    // Ámbar (rojo + verde)
    digitalWrite(PIN_LED_R, LOW);
    digitalWrite(PIN_LED_G, LOW);
    digitalWrite(PIN_LED_Y, HIGH);
  } else {
    // Verde
    digitalWrite(PIN_LED_R, LOW);
    digitalWrite(PIN_LED_G, HIGH);
    digitalWrite(PIN_LED_Y, LOW);
  }
}

// ---------------------------------------------------------------------------
// FUNCIÓN: activar/desactivar buzzer vía relé
// ---------------------------------------------------------------------------
void actualizarBuzzer(bool alerta) {
  digitalWrite(PIN_RELAY_BUZZ, alerta ? HIGH : LOW);
}