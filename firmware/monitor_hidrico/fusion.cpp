// ---------------------------------------------------------------------------
// fusion.cpp — Implementación de la lógica de fusión y de la alarma.
// ---------------------------------------------------------------------------
#include "fusion.h"

static float limitar01(float x) { return constrain(x, 0.0f, 1.0f); }

// ----------------------------- Ventana móvil -------------------------------
void VentanaNivel::agregar(float nivel_cm) {
  _buf[_idx] = nivel_cm;
  _idx = (_idx + 1) % VENTANA_MUESTRAS;
  if (_idx == 0) _llena = true;
}

float VentanaNivel::tasaDescenso() const {
  if (!_llena) return 0.0f;
  // Con el buffer lleno, _idx apunta a la muestra más antigua
  float antiguo  = _buf[_idx];
  float reciente = _buf[(_idx - 1 + VENTANA_MUESTRAS) % VENTANA_MUESTRAS];
  // Entre la primera y la última muestra hay VENTANA_MUESTRAS − 1 intervalos
  return (antiguo - reciente) / (VENTANA_MUESTRAS - 1);
}

// ----------------------------- Índices -------------------------------------
float calcularIndiceEvaporacion(float temp_c, float hum_pct, uint16_t ldr) {
  // Si el DHT22 nunca ha dado una lectura válida, su factor no suma
  float fT = isnan(temp_c)  ? 0.0f : limitar01((temp_c - EVAP_T_MIN_C) / EVAP_T_RANGO_C);
  float fH = isnan(hum_pct) ? 0.0f : limitar01((100.0f - hum_pct) / 100.0f);
  float fR = limitar01(ldr / 4095.0f);
  return (fT + fH + fR) / 3.0f;
}

float calcularRiesgo(float nivel_norm, float tasa_norm, float evaporacion) {
  return limitar01(W1_NIVEL * (1.0f - nivel_norm)
                 + W2_TASA  * tasa_norm
                 + W3_EVAP  * evaporacion);
}

CausaDescenso estimarCausa(float tasa_norm, float evaporacion) {
  if (tasa_norm >= CAUSA_TASA_FUGA && evaporacion < CAUSA_EVAP_BAJA) return CAUSA_FUGA;
  if (tasa_norm >= CAUSA_TASA_MIN  && evaporacion >= CAUSA_EVAP_ALTA) return CAUSA_EVAPORACION;
  return CAUSA_NINGUNA;
}

ResultadoFusion fusionar(const Lectura& l, VentanaNivel& ventana) {
  ResultadoFusion r;
  ventana.agregar(l.nivel_cm);
  r.nivel_norm  = limitar01(l.nivel_cm / TANQUE_ALTURA_CM);
  r.tasa_cm     = ventana.tasaDescenso();
  r.tasa_norm   = limitar01(r.tasa_cm / TASA_MAX_CM);
  r.evaporacion = calcularIndiceEvaporacion(l.temp_c, l.hum_pct, l.ldr);
  r.riesgo      = calcularRiesgo(r.nivel_norm, r.tasa_norm, r.evaporacion);
  r.causa       = estimarCausa(r.tasa_norm, r.evaporacion);
  return r;
}

// ----------------------------- Máquina de alarma ---------------------------
EstadoAlarma MaquinaAlarma::estadoSinAlerta(float riesgo) const {
  return riesgo > UMBRAL_PRECAUCION ? PRECAUCION : NORMAL;
}

bool MaquinaAlarma::actualizar(float riesgo, bool ventanaLlena) {
  EstadoAlarma anterior = _estado;
  _muestrasAltas = (riesgo > UMBRAL_ALERTA) ? _muestrasAltas + 1 : 0;

  switch (_estado) {
    case NORMAL:
    case PRECAUCION:
      // No se alerta hasta tener la ventana llena: sin ella la tasa no es confiable
      if (ventanaLlena && _muestrasAltas >= MUESTRAS_PERSISTENCIA) _estado = ALERTA;
      else _estado = estadoSinAlerta(riesgo);
      break;
    case ALERTA:
    case SILENCIADA:
      // Histéresis: sale de alerta (o se rearma) solo cuando el riesgo baja claramente
      if (riesgo < UMBRAL_ALERTA - HISTERESIS) _estado = estadoSinAlerta(riesgo);
      break;
  }
  return _estado != anterior;
}

bool MaquinaAlarma::silenciar() {
  if (_estado != ALERTA) return false;
  _estado = SILENCIADA;
  return true;
}

bool MaquinaAlarma::rearmar() {
  if (_estado != SILENCIADA) return false;
  _estado = ALERTA;
  return true;
}

// ----------------------------- Nombres -------------------------------------
const char* nombreEstado(EstadoAlarma e) {
  switch (e) {
    case NORMAL:     return "NORMAL";
    case PRECAUCION: return "PRECAUCION";
    case ALERTA:     return "ALERTA";
    case SILENCIADA: return "SILENCIADA";
  }
  return "?";
}

const char* nombreCausa(CausaDescenso c) {
  switch (c) {
    case CAUSA_FUGA:        return "FUGA";
    case CAUSA_EVAPORACION: return "EVAPORACION";
    default:                return "NINGUNA";
  }
}
