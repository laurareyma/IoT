-- schema.sql — Esquema de la base de datos SQLite del gateway.
-- Diseño: wiki, página 02e (sección 5.2). gateway.py lo aplica al arrancar
-- (CREATE ... IF NOT EXISTS: no borra datos existentes).
-- Para crear la base a mano:  sqlite3 gateway.db < schema.sql
CREATE TABLE IF NOT EXISTS lecturas (
    id              INTEGER PRIMARY KEY,
    nodo            TEXT    NOT NULL,
    ts_ms           INTEGER NOT NULL,   -- hora de llegada al gateway (UTC, ms)
    hora_utc        TEXT    NOT NULL,
    t_nodo          INTEGER,            -- millis() del ESP32 al medir
    nivel_cm        REAL,
    nivel_pct       REAL,
    tasa_cm_muestra REAL,
    temp_c          REAL,
    hum_pct         REAL,
    presion_hpa     REAL,
    ldr             INTEGER,
    evaporacion     REAL,
    riesgo          REAL,
    estado          TEXT,
    causa           TEXT,
    nivel_ok        INTEGER,            -- 0 si el HC-SR04 no dio eco (no se usa en la IA)
    dht_ok          INTEGER,
    bmp_ok          INTEGER
);
CREATE INDEX IF NOT EXISTS idx_lecturas_nodo_ts ON lecturas (nodo, ts_ms);

CREATE TABLE IF NOT EXISTS eventos (
    id       INTEGER PRIMARY KEY,
    nodo     TEXT    NOT NULL,
    ts_ms    INTEGER NOT NULL,
    hora_utc TEXT    NOT NULL,
    tipo     TEXT    NOT NULL,   -- alerta, silenciar, conexion
    origen   TEXT,               -- automatico, tablero_local, ubidots
    detalle  TEXT
);

CREATE TABLE IF NOT EXISTS predicciones (
    id              INTEGER PRIMARY KEY,
    nodo            TEXT    NOT NULL,
    ts_ms           INTEGER NOT NULL,
    hora_utc        TEXT    NOT NULL,
    horas_restantes REAL,
    pendiente_pct_h REAL,
    r2              REAL,
    confianza       TEXT,
    prioridad       TEXT,
    recomendacion   TEXT,
    modelo          TEXT                -- nombre del modelo de Gemini o "plantilla"
);

CREATE TABLE IF NOT EXISTS pendientes (
    id      INTEGER PRIMARY KEY,
    ts_ms   INTEGER NOT NULL,
    tema    TEXT    NOT NULL,
    mensaje TEXT    NOT NULL           -- JSON con la marca de tiempo original
);
