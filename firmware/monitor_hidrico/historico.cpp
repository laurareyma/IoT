// ---------------------------------------------------------------------------
// historico.cpp — Implementación del histórico y del registro de eventos.
// 360 registros × 32 bytes ≈ 11.5 KB de RAM.
// ---------------------------------------------------------------------------
#include "historico.h"
#include "config.h"
#include <stdarg.h>

static Registro regs[HISTORICO_MUESTRAS];
static int      regIdx = 0;    // próxima posición a escribir
static int      regCant = 0;

static Evento   evs[EVENTOS_MAX];
static int      evIdx = 0;
static int      evCant = 0;

void historicoAgregar(const Registro& r) {
  regs[regIdx] = r;
  regIdx = (regIdx + 1) % HISTORICO_MUESTRAS;
  if (regCant < HISTORICO_MUESTRAS) regCant++;
}

int historicoCantidad() { return regCant; }

// Imprime un float o null si no es un número (JSON no admite NaN)
static void imprimirNum(Print& out, float v, int decimales) {
  if (isnan(v)) out.print("null");
  else out.print(v, decimales);
}

void historicoEscribirJSON(Print& out, int n) {
  if (n <= 0 || n > regCant) n = regCant;
  out.print("{\"campos\":[\"t\",\"nivel_cm\",\"temp_c\",\"hum_pct\",\"presion_hpa\","
            "\"evaporacion\",\"riesgo\",\"estado\"],\"datos\":[");
  int inicio = (regIdx - n + HISTORICO_MUESTRAS) % HISTORICO_MUESTRAS;
  for (int i = 0; i < n; i++) {
    const Registro& r = regs[(inicio + i) % HISTORICO_MUESTRAS];
    if (i) out.print(',');
    out.print('[');
    out.print(r.t);                      out.print(',');
    imprimirNum(out, r.nivel_cm, 1);     out.print(',');
    imprimirNum(out, r.temp_c, 1);       out.print(',');
    imprimirNum(out, r.hum_pct, 1);      out.print(',');
    imprimirNum(out, r.presion_hpa, 1);  out.print(',');
    imprimirNum(out, r.evaporacion, 3);  out.print(',');
    imprimirNum(out, r.riesgo, 3);       out.print(",\"");
    out.print(nombreEstado((EstadoAlarma)r.estado));
    out.print("\"]");
  }
  out.print("]}");
}

void eventoAgregar(uint32_t t, const char* formato, ...) {
  Evento& e = evs[evIdx];
  e.t = t;
  va_list args;
  va_start(args, formato);
  vsnprintf(e.texto, sizeof(e.texto), formato, args);
  va_end(args);
  evIdx = (evIdx + 1) % EVENTOS_MAX;
  if (evCant < EVENTOS_MAX) evCant++;
  Serial.printf("[EVENTO] %lu ms: %s\n", (unsigned long)t, e.texto);
}

void eventosEscribirJSON(Print& out) {
  // Del más reciente al más antiguo. Los textos los genera el firmware y no
  // contienen comillas, así que no hace falta escaparlos.
  out.print('[');
  for (int i = 0; i < evCant; i++) {
    const Evento& e = evs[(evIdx - 1 - i + EVENTOS_MAX) % EVENTOS_MAX];
    if (i) out.print(',');
    out.printf("{\"t\":%lu,\"texto\":\"%s\"}", (unsigned long)e.t, e.texto);
  }
  out.print(']');
}
