// ---------------------------------------------------------------------------
// fusion.h — Lógica de fusión y máquina de estados de la alarma.
// Funciones sin acceso a hardware: se pueden probar con valores conocidos
// (escenarios C-03a a C-03e del protocolo de pruebas).
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"
#include "config.h"

// Ventana móvil (buffer circular) de niveles para calcular la tasa de descenso.
class VentanaNivel {
 public:
  void  agregar(float nivel_cm);
  bool  llena() const { return _llena; }
  // cm por muestra entre la muestra más antigua y la más reciente (positivo = bajando)
  float tasaDescenso() const;
 private:
  float _buf[VENTANA_MUESTRAS] = {0};
  int   _idx   = 0;
  bool  _llena = false;
};

// e = (fT + fH + fR) / 3, cada factor normalizado entre 0 y 1.
float calcularIndiceEvaporacion(float temp_c, float hum_pct, uint16_t ldr);

// riesgo = W1·(1 − n) + W2·t + W3·e, limitado a [0, 1].
float calcularRiesgo(float nivel_norm, float tasa_norm, float evaporacion);

CausaDescenso estimarCausa(float tasa_norm, float evaporacion);

// Aplica toda la fusión a una lectura.
ResultadoFusion fusionar(const Lectura& l, VentanaNivel& ventana);

// Máquina de estados: NORMAL -> PRECAUCION -> ALERTA -> SILENCIADA.
// Diagrama en la wiki, página 02f, sección 4.
class MaquinaAlarma {
 public:
  // Devuelve true si el estado cambió.
  bool actualizar(float riesgo, bool ventanaLlena);
  bool silenciar();   // solo desde ALERTA
  bool rearmar();     // solo desde SILENCIADA
  EstadoAlarma estado() const { return _estado; }
 private:
  EstadoAlarma _estado = NORMAL;
  int          _muestrasAltas = 0;
  EstadoAlarma estadoSinAlerta(float riesgo) const;
};
