// ---------------------------------------------------------------------------
// sistema.h — Estado global y acciones sobre la alarma. Implementado en
// monitor_hidrico.ino. Leer o escribir `estado` requiere tomar mtxEstado;
// las funciones alarmaSilenciar/alarmaRearmar lo toman internamente.
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"
#include <IPAddress.h>

extern EstadoSistema estado;
extern DiagMedicion  diag;

// Órdenes del tablero local (el registro de eventos anota la IP del cliente).
bool alarmaSilenciar(const IPAddress& cliente);
bool alarmaRearmar(const IPAddress& cliente);

// Órdenes del tablero global, recibidas por MQTT a través del gateway.
bool alarmaSilenciarRemoto();
bool alarmaRearmarRemoto();
