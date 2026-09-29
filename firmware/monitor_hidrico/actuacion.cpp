// ---------------------------------------------------------------------------
// actuacion.cpp — Implementación de la alerta in situ.
// ---------------------------------------------------------------------------
#include "actuacion.h"
#include "config.h"
#include "recursos.h"
#include <LiquidCrystal_I2C.h>

static LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

static const uint32_t MS_POR_PANTALLA = 2500;
static const uint32_t MS_BUZZER       = 500;   // periodo del pitido intermitente
static const int      PANTALLAS       = 4;

static char lineaActual[2][17] = {"", ""};

void actuacionIniciar() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  xSemaphoreTake(mtxI2C, portMAX_DELAY);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Monitoreo");
  lcd.setCursor(0, 1); lcd.print("Hidrico");
  xSemaphoreGive(mtxI2C);
}

// Escribe una línea solo si cambió, rellenando con espacios (evita parpadeo de lcd.clear()).
static void escribirLinea(int fila, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", texto);
  if (strcmp(buf, lineaActual[fila]) == 0) return;
  strcpy(lineaActual[fila], buf);
  xSemaphoreTake(mtxI2C, portMAX_DELAY);
  lcd.setCursor(0, fila);
  lcd.print(buf);
  xSemaphoreGive(mtxI2C);
}

static void actualizarSemaforo(EstadoAlarma estado) {
  bool rojo  = (estado == ALERTA || estado == SILENCIADA);
  bool ambar = (estado == PRECAUCION);
  digitalWrite(PIN_LED_R, rojo);
  digitalWrite(PIN_LED_Y, ambar);
  digitalWrite(PIN_LED_G, !rojo && !ambar);
}

static void actualizarBuzzer(EstadoAlarma estado) {
  bool encendido = (estado == ALERTA) && ((millis() / MS_BUZZER) % 2 == 0);
  digitalWrite(PIN_BUZZER, encendido);
}

static void actualizarLCD(const EstadoSistema& e, bool wifiConectado, const char* ip) {
  char l1[17], l2[17];
  int pantalla = (millis() / MS_POR_PANTALLA) % PANTALLAS;

  // En alerta, la pantalla de estado aparece una vez sí y una vez no
  bool enAlerta = (e.alarma == ALERTA || e.alarma == SILENCIADA);
  if (enAlerta && pantalla % 2 == 1) pantalla = 3;

  if (e.muestras == 0) {
    snprintf(l1, sizeof(l1), "Midiendo...");
    snprintf(l2, sizeof(l2), "%s", wifiConectado ? ip : "Sin WiFi");
  } else switch (pantalla) {
    case 0:
      snprintf(l1, sizeof(l1), "Nivel:%5.1f cm%s", e.lectura.nivel_cm, e.lectura.nivel_ok ? "" : "!");
      snprintf(l2, sizeof(l2), "Temp: %5.1f C%s", e.lectura.temp_c, e.lectura.dht_ok ? "" : "!");
      break;
    case 1:
      snprintf(l1, sizeof(l1), "Hum:  %5.1f %%%s", e.lectura.hum_pct, e.lectura.dht_ok ? "" : "!");
      snprintf(l2, sizeof(l2), "Pres: %5.0f hPa", e.lectura.presion_hpa);
      break;
    case 2:
      snprintf(l1, sizeof(l1), "Evap:   %4.2f", e.fusion.evaporacion);
      snprintf(l2, sizeof(l2), "Riesgo: %3.0f %%", e.fusion.riesgo * 100.0f);
      break;
    default:
      switch (e.alarma) {
        case ALERTA:     snprintf(l1, sizeof(l1), "!!! ALERTA !!!"); break;
        case SILENCIADA: snprintf(l1, sizeof(l1), "ALERTA SILENC."); break;
        case PRECAUCION: snprintf(l1, sizeof(l1), "PRECAUCION");     break;
        default:         snprintf(l1, sizeof(l1), "Estado: NORMAL"); break;
      }
      snprintf(l2, sizeof(l2), "%s", wifiConectado ? ip : "Sin WiFi");
      break;
  }
  escribirLinea(0, l1);
  escribirLinea(1, l2);
}

void actuacionActualizar(const EstadoSistema& e, bool wifiConectado, const char* ip) {
  actualizarSemaforo(e.alarma);
  actualizarBuzzer(e.alarma);
  actualizarLCD(e, wifiConectado, ip);
}
