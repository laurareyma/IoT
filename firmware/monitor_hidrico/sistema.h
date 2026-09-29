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

bool alarmaSilenciar(const IPAddress& cliente);
bool alarmaRearmar(const IPAddress& cliente);
