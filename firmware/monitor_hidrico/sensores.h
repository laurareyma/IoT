// ---------------------------------------------------------------------------
// sensores.h — Módulo de adquisición: HC-SR04, DHT22, BMP280 y LDR.
// Solo se llama desde TareaMedicion (núcleo 0).
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"

// Inicializa pines y sensores. Devuelve false si el BMP280 no responde.
bool sensoresIniciar();

// Lee todos los sensores. Si un sensor falla, reusa su último valor válido y
// marca el campo *_ok correspondiente en false.
void sensoresLeer(Lectura& l);
