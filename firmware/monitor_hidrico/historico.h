// ---------------------------------------------------------------------------
// historico.h — Buffer circular con las últimas mediciones y registro de
// eventos (alertas, silenciado, fallas de sensor).
// IMPORTANTE: quien llama debe tener tomado mtxEstado.
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"

void historicoAgregar(const Registro& r);
int  historicoCantidad();

// Escribe las últimas n muestras (de la más antigua a la más reciente) como
// {"campos":[...],"datos":[[...],...]} para ahorrar memoria frente a objetos.
void historicoEscribirJSON(Print& out, int n);

void eventoAgregar(uint32_t t, const char* formato, ...);
void eventosEscribirJSON(Print& out);
