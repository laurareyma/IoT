// ---------------------------------------------------------------------------
// config.h — Pines, parámetros de calibración y umbrales del nodo.
// Todo lo que cambia entre un punto de monitoreo y otro está aquí, para
// replicar el nodo sin tocar la lógica (restricción de escalabilidad).
// Justificación de cada valor: wiki, página 02c (sección 3.2).
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>

// ----------------------------- Pines ---------------------------------------
#define PIN_TRIG      5    // HC-SR04 TRIG
#define PIN_ECHO      18   // HC-SR04 ECHO (con divisor 1k/2k: 5 V -> 3.3 V)
#define PIN_DHT       4    // DHT22 DATA (pull-up 10k a 3.3 V)
#define PIN_LDR       34   // ADC1: el ADC2 no funciona con el Wi-Fi activo
#define PIN_LED_R     25
#define PIN_LED_G     26
#define PIN_LED_Y     33
#define PIN_BUZZER    32   // base del 2N2222A a través de 1k
#define PIN_SDA       21   // I2C compartido: BMP280 + LCD
#define PIN_SCL       22

#define DHTTYPE       DHT22
#define BMP280_ADDR   0x76
#define LCD_ADDR      0x27

// ----------------------------- Muestreo ------------------------------------
const uint32_t PERIODO_MEDICION_MS = 5000;  // prototipo; en campo: 10 min
const int      VENTANA_MUESTRAS    = 6;     // ventana móvil para la tasa de descenso
const int      FALLOS_NIVEL_MAX    = 3;     // lecturas sin eco antes de asumir tanque vacío

// ----------------------------- Tanque --------------------------------------
const float TANQUE_ALTURA_CM = 100.0;  // distancia sensor–fondo. AJUSTAR A LA MAQUETA
const float TASA_MAX_CM      = 5.0;    // cm por muestra que equivalen a t = 1

// ----------------------------- Índice de evaporación -----------------------
const float EVAP_T_MIN_C    = 10.0;  // fT = 0
const float EVAP_T_RANGO_C  = 25.0;  // fT = 1 en 35 °C

// ----------------------------- Lógica de fusión ----------------------------
const float W1_NIVEL = 0.5;
const float W2_TASA  = 0.3;
const float W3_EVAP  = 0.2;

const float UMBRAL_PRECAUCION     = 0.40;
const float UMBRAL_ALERTA         = 0.65;
const float HISTERESIS            = 0.05;  // rearme cuando riesgo < 0.60
const int   MUESTRAS_PERSISTENCIA = 3;     // muestras seguidas sobre el umbral para alertar

// Causa probable del descenso (se muestra en el tablero)
const float CAUSA_TASA_FUGA      = 0.5;  // t >= 0.5 y e < 0.4 -> posible fuga
const float CAUSA_EVAP_BAJA      = 0.4;
const float CAUSA_TASA_MIN       = 0.1;  // t >= 0.1 y e >= 0.6 -> evaporación
const float CAUSA_EVAP_ALTA      = 0.6;

// ----------------------------- Histórico -----------------------------------
const int HISTORICO_MUESTRAS = 360;  // 30 min con muestreo de 5 s
const int EVENTOS_MAX        = 20;

// ----------------------------- Red y acceso --------------------------------
#define NOMBRE_MDNS "monitor-hidrico"  // http://monitor-hidrico.local

// Filtro por IP. Siempre se exige que el cliente esté en la misma subred del
// ESP32. Si FILTRAR_POR_LISTA es true, además debe estar en IPS_AUTORIZADAS
// (se recomienda reservar esas IPs por DHCP en el router de la alcaldía).
const bool FILTRAR_POR_LISTA = false;
const IPAddress IPS_AUTORIZADAS[] = {
  IPAddress(192, 168, 1, 10),
  IPAddress(192, 168, 1, 11),
};

// ----------------------------- Tareas FreeRTOS -----------------------------
const int CORE_MEDICION = 0;
const int CORE_FUSION   = 1;
const int PRIO_MEDICION = 3;
const int PRIO_FUSION   = 2;
