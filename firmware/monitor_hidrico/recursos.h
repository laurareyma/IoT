// ---------------------------------------------------------------------------
// recursos.h — Mutex compartidos entre tareas. Se crean en setup().
//   mtxI2C    : protege el bus I2C (BMP280 en núcleo 0, LCD en núcleo 1).
//   mtxEstado : protege el estado del sistema, el histórico y los eventos,
//               que escribe TareaFusionAlerta y lee el servidor web.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t mtxI2C;
extern SemaphoreHandle_t mtxEstado;
