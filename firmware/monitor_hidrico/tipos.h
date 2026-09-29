// ---------------------------------------------------------------------------
// tipos.h — Estructuras de datos compartidas entre módulos.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>

// Lectura cruda de los sensores. La produce TareaMedicion y viaja por la
// cola qLecturas hasta TareaFusionAlerta.
struct Lectura {
  uint32_t t;            // millis() al inicio de la medición
  float    nivel_cm;     // altura del agua
  float    temp_c;
  float    hum_pct;
  float    presion_hpa;
  uint16_t ldr;          // 0–4095
  bool     nivel_ok;     // false: sin eco, se reusó el último valor válido
  bool     dht_ok;
  bool     bmp_ok;
};

enum EstadoAlarma : uint8_t { NORMAL = 0, PRECAUCION, ALERTA, SILENCIADA };

enum CausaDescenso : uint8_t { CAUSA_NINGUNA = 0, CAUSA_FUGA, CAUSA_EVAPORACION };

// Resultado de la lógica de fusión para una lectura.
struct ResultadoFusion {
  float         nivel_norm;  // n: 0 vacío, 1 lleno
  float         tasa_cm;     // cm por muestra (positivo = bajando)
  float         tasa_norm;   // t: 0–1
  float         evaporacion; // e: 0–1
  float         riesgo;      // 0–1
  CausaDescenso causa;
};

// Registro compacto del histórico (~32 bytes).
struct Registro {
  uint32_t t;
  float    nivel_cm;
  float    temp_c;
  float    hum_pct;
  float    presion_hpa;
  float    evaporacion;
  float    riesgo;
  uint8_t  estado;
};

// Evento para el registro que muestra el tablero.
struct Evento {
  uint32_t t;
  char     texto[48];
};

// Estado completo que consulta el servidor web.
struct EstadoSistema {
  Lectura         lectura;
  ResultadoFusion fusion;
  EstadoAlarma    alarma;
  bool            ventanaLlena;
  uint32_t        muestras;
};

// Estadísticas del periodo real de medición (pruebas C-01 y C-02).
struct DiagMedicion {
  uint32_t ultimo_us;
  uint32_t min_us;
  uint32_t max_us;
  uint32_t muestras;
  uint32_t perdidas;   // lecturas descartadas por cola llena
};

const char* nombreEstado(EstadoAlarma e);
const char* nombreCausa(CausaDescenso c);
