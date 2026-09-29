// ---------------------------------------------------------------------------
// actuacion.h — Alerta in situ: LCD 16x2 I2C, semáforo de LEDs y buzzer.
// Funciona sin red: solo depende del estado calculado en el propio ESP32.
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"

void actuacionIniciar();

// Se llama cada ~250 ms desde TareaFusionAlerta. Rota las pantallas del LCD,
// actualiza el semáforo y hace sonar el buzzer de forma intermitente en ALERTA.
void actuacionActualizar(const EstadoSistema& e, bool wifiConectado, const char* ip);
