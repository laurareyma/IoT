#!/usr/bin/env python3
"""
gateway.py — Programa del gateway IoT (Raspberry Pi), Challenge 3.

Recorrido de los datos (wiki, páginas 02e sección 5 y 02c sección 4):

  ESP32 --MQTT--> Mosquitto (local) --> este programa --> SQLite
                                                     \\--> Ubidots (MQTT + TLS)
  Ubidots (interruptor "silenciar") --> este programa --> sabana/<nodo>/cmd/alarma --> ESP32

Qué hace:
  1. Se suscribe a sabana/# en el broker local.
  2. Guarda cada mensaje en SQLite ANTES de cualquier otra cosa (CD-06):
     tablas lecturas, eventos, predicciones y pendientes.
  3. Cada minuto promedia las lecturas y las publica en Ubidots; los cambios
     de estado de la alarma y de conexión se publican de inmediato.
     Si no hay internet, los mensajes quedan en `pendientes` y se envían en
     orden, con su marca de tiempo original, al volver la conexión (CD-07).
  4. Cada 5 min ejecuta el módulo de IA: regresión lineal de las horas hasta
     el nivel crítico y recomendación de Gemini, con plantilla si falla.
     La IA no actúa sobre la alarma (CD-15).
  5. Reenvía al ESP32 la orden de silenciar que llega desde Ubidots.
  6. Avisa por Telegram (bot propio) cada cambio de estado de la alarma y la
     desconexión de un nodo. El plan de Ubidots no incluye la acción Telegram
     en sus eventos, por eso el aviso sale del gateway.

Uso:
  python3 gateway.py --config gateway.conf
  (schema.sql debe estar en la misma carpeta que gateway.py)

Dependencia externa: paho-mqtt (pip install -r requirements.txt). El resto
es biblioteca estándar de Python (sqlite3, urllib, json, ssl).
"""

import argparse
import configparser
import json
import logging
import math
import os
import queue
import signal
import sqlite3
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import List, Optional, Tuple

log = logging.getLogger("gateway")

# Estado de la alarma -> valor numérico de la variable `estado_alarma` en Ubidots
ESTADOS_UBIDOTS = {"NORMAL": 0, "PRECAUCION": 1, "ALERTA": 2, "SILENCIADA": 3}

# Variables que se promedian por minuto y se envían a Ubidots
VARIABLES_PROMEDIO = ["nivel_pct", "temp_c", "hum_pct", "presion_hpa", "evaporacion", "riesgo"]


# =============================================================================
# Configuración
# =============================================================================
@dataclass
class Config:
    # Broker local (Mosquitto)
    local_host: str = "localhost"
    local_port: int = 1883
    local_usuario: str = ""
    local_clave: str = ""
    # Ubidots
    ubidots_host: str = "industrial.api.ubidots.com"
    ubidots_port: int = 8883
    ubidots_token: str = ""
    ubidots_ca: str = ""  # vacío: certificados del sistema
    # Base de datos
    db_ruta: str = "gateway.db"
    # Nodos conocidos (se agregan solos al recibir mensajes de un nodo nuevo)
    nodos: Tuple[str, ...] = ("tanque01",)
    # Módulo de IA (02c, sección 4.2)
    nivel_critico_pct: float = 20.0
    ventana_min: int = 15            # prototipo: 15 min; en campo: 1440 (24 h)
    datos_minimos: int = 5
    periodo_ia_s: int = 300          # cada 5 min
    pendiente_estable: float = -0.5  # %/h; b >= este valor -> "Nivel estable"
    r2_alta: float = 0.7
    r2_media: float = 0.4
    gemini_clave: str = ""
    gemini_modelo: str = "gemini-2.5-flash"
    gemini_timeout_s: float = 10.0
    # Telegram (02e, sección 8.1). Vacíos = sin avisos.
    telegram_token: str = ""
    telegram_chat_id: str = ""


def cargar_config(ruta: str) -> Config:
    cp = configparser.ConfigParser()
    if not cp.read(ruta, encoding="utf-8"):
        raise SystemExit(f"No se encontró el archivo de configuración: {ruta}")
    c = Config()
    g = cp.get
    c.local_host = g("mosquitto", "host", fallback=c.local_host)
    c.local_port = cp.getint("mosquitto", "port", fallback=c.local_port)
    c.local_usuario = g("mosquitto", "usuario", fallback="")
    c.local_clave = g("mosquitto", "clave", fallback="")
    c.ubidots_host = g("ubidots", "host", fallback=c.ubidots_host)
    c.ubidots_port = cp.getint("ubidots", "port", fallback=c.ubidots_port)
    c.ubidots_token = g("ubidots", "token", fallback="")
    c.ubidots_ca = g("ubidots", "ca", fallback="")
    c.db_ruta = g("sqlite", "ruta", fallback=c.db_ruta)
    nodos = g("general", "nodos", fallback="tanque01")
    c.nodos = tuple(n.strip() for n in nodos.split(",") if n.strip())
    c.nivel_critico_pct = cp.getfloat("ia", "nivel_critico_pct", fallback=c.nivel_critico_pct)
    c.ventana_min = cp.getint("ia", "ventana_min", fallback=c.ventana_min)
    c.datos_minimos = cp.getint("ia", "datos_minimos", fallback=c.datos_minimos)
    c.periodo_ia_s = cp.getint("ia", "periodo_s", fallback=c.periodo_ia_s)
    c.gemini_clave = g("ia", "gemini_clave", fallback="")
    c.gemini_modelo = g("ia", "gemini_modelo", fallback=c.gemini_modelo)
    c.gemini_timeout_s = cp.getfloat("ia", "gemini_timeout_s", fallback=c.gemini_timeout_s)
    c.telegram_token = g("telegram", "token", fallback="").strip()
    c.telegram_chat_id = g("telegram", "chat_id", fallback="").strip()
    return c


def ahora_ms() -> int:
    return int(time.time() * 1000)


def iso_utc(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).isoformat(timespec="seconds")


# =============================================================================
# Base de datos SQLite (02e, sección 5.2)
# =============================================================================
# Esquema DDL en schema.sql, junto a este archivo (una sola fuente para el
# programa y para crear la base a mano con sqlite3).
RUTA_ESQUEMA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "schema.sql")


class BaseDatos:
    """Acceso a SQLite compartido entre los hilos de MQTT y el hilo principal."""

    def __init__(self, ruta: str):
        self.con = sqlite3.connect(ruta, check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock:
            # WAL: lecturas y escrituras concurrentes y menos riesgo de corrupción
            # de la microSD ante un corte de energía.
            self.con.execute("PRAGMA journal_mode=WAL")
            self.con.execute("PRAGMA synchronous=NORMAL")
            with open(RUTA_ESQUEMA, encoding="utf-8") as f:
                self.con.executescript(f.read())
            self.con.commit()

    def _ejecutar(self, sql: str, params=()):
        with self.lock:
            cur = self.con.execute(sql, params)
            self.con.commit()
            return cur

    def _consultar(self, sql: str, params=()):
        with self.lock:
            return self.con.execute(sql, params).fetchall()

    # ------------------------------------------------------------------ escritura
    def guardar_lectura(self, nodo: str, d: dict, ts: int) -> None:
        sensores = d.get("sensores") or {}

        def b(v):
            return None if v is None else int(bool(v))

        self._ejecutar(
            """INSERT INTO lecturas (nodo, ts_ms, hora_utc, t_nodo, nivel_cm, nivel_pct,
               tasa_cm_muestra, temp_c, hum_pct, presion_hpa, ldr, evaporacion, riesgo,
               estado, causa, nivel_ok, dht_ok, bmp_ok)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (nodo, ts, iso_utc(ts), d.get("t"), d.get("nivel_cm"), d.get("nivel_pct"),
             d.get("tasa_cm_muestra"), d.get("temp_c"), d.get("hum_pct"), d.get("presion_hpa"),
             d.get("ldr"), d.get("evaporacion"), d.get("riesgo"), d.get("estado"), d.get("causa"),
             b(sensores.get("nivel")), b(sensores.get("dht22")), b(sensores.get("bmp280"))))

    def guardar_evento(self, nodo: str, tipo: str, origen: str, detalle: str, ts: int) -> None:
        self._ejecutar(
            "INSERT INTO eventos (nodo, ts_ms, hora_utc, tipo, origen, detalle) VALUES (?,?,?,?,?,?)",
            (nodo, ts, iso_utc(ts), tipo, origen, detalle))

    def guardar_prediccion(self, nodo: str, p: "Pronostico", prioridad: str,
                           recomendacion: str, modelo: str, ts: int) -> None:
        self._ejecutar(
            """INSERT INTO predicciones (nodo, ts_ms, hora_utc, horas_restantes, pendiente_pct_h,
               r2, confianza, prioridad, recomendacion, modelo) VALUES (?,?,?,?,?,?,?,?,?,?)""",
            (nodo, ts, iso_utc(ts), p.horas_restantes, p.pendiente, p.r2, p.confianza,
             prioridad, recomendacion, modelo))

    def encolar_pendiente(self, tema: str, mensaje: str, ts: int) -> None:
        self._ejecutar("INSERT INTO pendientes (ts_ms, tema, mensaje) VALUES (?,?,?)",
                       (ts, tema, mensaje))

    def borrar_pendiente(self, id_: int) -> None:
        self._ejecutar("DELETE FROM pendientes WHERE id = ?", (id_,))

    # ------------------------------------------------------------------ lectura
    def pendientes(self, limite: int = 200):
        return self._consultar("SELECT id, tema, mensaje FROM pendientes ORDER BY id LIMIT ?",
                               (limite,))

    def contar_pendientes(self) -> int:
        return self._consultar("SELECT COUNT(*) FROM pendientes")[0][0]

    def promedio_minuto(self, nodo: str, inicio_ms: int) -> Optional[dict]:
        """Promedio de cada variable en el minuto [inicio, inicio + 60 s)."""
        cols = ", ".join(f"AVG({v}) AS {v}" for v in VARIABLES_PROMEDIO)
        fila = self._consultar(
            f"SELECT COUNT(*) AS n, {cols} FROM lecturas WHERE nodo = ? AND ts_ms >= ? AND ts_ms < ?",
            (nodo, inicio_ms, inicio_ms + 60_000))[0]
        if fila["n"] == 0:
            return None
        return {v: fila[v] for v in VARIABLES_PROMEDIO if fila[v] is not None}

    def niveles_por_minuto(self, nodo: str, desde_ms: int) -> List[Tuple[float, float]]:
        """(hora en h, nivel_pct promedio) de cada minuto, sin lecturas con falla del HC-SR04."""
        filas = self._consultar(
            """SELECT ts_ms / 60000 AS minuto, AVG(nivel_pct) AS nivel FROM lecturas
               WHERE nodo = ? AND ts_ms >= ? AND nivel_pct IS NOT NULL
                 AND (nivel_ok IS NULL OR nivel_ok = 1)
               GROUP BY minuto ORDER BY minuto""",
            (nodo, desde_ms))
        return [(f["minuto"] / 60.0, f["nivel"]) for f in filas]

    def ultima_lectura(self, nodo: str) -> Optional[sqlite3.Row]:
        filas = self._consultar(
            """SELECT * FROM lecturas WHERE nodo = ? AND nivel_pct IS NOT NULL
                 AND (nivel_ok IS NULL OR nivel_ok = 1)
               ORDER BY ts_ms DESC LIMIT 1""", (nodo,))
        return filas[0] if filas else None


# =============================================================================
# Módulo de IA: pronóstico (02c, sección 4.2)
# =============================================================================
@dataclass
class Pronostico:
    caso: int                      # 1 a 5, según la tabla de casos especiales
    horas_restantes: Optional[float]
    pendiente: Optional[float]     # %/h
    r2: Optional[float]
    confianza: str                 # alta, media, baja, no aplica
    texto: str                     # "Nivel estable", "Tendencia no clara", ...

    @property
    def consultar_gemini(self) -> bool:
        # Casos 2 y 4: se mantiene el último pronóstico y no se llama a Gemini
        return self.caso in (1, 3, 5)


def regresion_lineal(puntos: List[Tuple[float, float]]) -> Tuple[float, float, float]:
    """Mínimos cuadrados nivel = a + b·t. Devuelve (a, b, R²). b en %/h."""
    n = len(puntos)
    sx = sum(x for x, _ in puntos)
    sy = sum(y for _, y in puntos)
    mx, my = sx / n, sy / n
    sxx = sum((x - mx) ** 2 for x, _ in puntos)
    sxy = sum((x - mx) * (y - my) for x, y in puntos)
    if sxx == 0:
        return my, 0.0, 0.0
    b = sxy / sxx
    a = my - b * mx
    ss_tot = sum((y - my) ** 2 for _, y in puntos)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in puntos)
    r2 = 0.0 if ss_tot == 0 else max(0.0, 1.0 - ss_res / ss_tot)
    return a, b, r2


def pronosticar(puntos: List[Tuple[float, float]], nivel_actual: Optional[float],
                cfg: Config) -> Pronostico:
    """Aplica los casos de 02c §4.2 en orden; el primero que se cumple define el resultado."""
    # Caso 1: ya está en el nivel crítico o por debajo
    if nivel_actual is not None and nivel_actual <= cfg.nivel_critico_pct:
        return Pronostico(1, 0.0, None, None, "no aplica", "Nivel crítico alcanzado")
    # Caso 2: pocos datos
    if nivel_actual is None or len(puntos) < cfg.datos_minimos:
        return Pronostico(2, None, None, None, "baja", "Tendencia no clara")
    _, b, r2 = regresion_lineal(puntos)
    # Caso 3: el nivel no baja. Se revisa ANTES que el R²: con el nivel quieto la
    # recta es plana y el R² sale cercano a 0.
    if b >= cfg.pendiente_estable:
        return Pronostico(3, -1.0, b, r2, "no aplica", "Nivel estable")
    # Caso 4: descenso con confianza baja
    if r2 < cfg.r2_media:
        return Pronostico(4, None, b, r2, "baja", "Tendencia no clara")
    # Caso 5: descenso claro
    horas = (nivel_actual - cfg.nivel_critico_pct) / abs(b)
    confianza = "alta" if r2 >= cfg.r2_alta else "media"
    return Pronostico(5, round(horas, 2), b, r2, confianza, f"{horas:.1f} h hasta el nivel crítico")


# =============================================================================
# Módulo de IA: recomendación (02c, secciones 4.3 y 4.4)
# =============================================================================
PROMPT = """Eres un asistente de gestión del riesgo hídrico para un acueducto veredal de
Sabana Centro (Cundinamarca). Con estos datos medidos, escribe una
recomendación para la autoridad municipal.

Datos:
- Nivel actual: {nivel_pct} %   Nivel crítico: {nivel_critico} %
- Horas hasta el nivel crítico: {horas_restantes} (confianza {confianza})
- Estado de la alarma: {estado}   Causa probable: {causa}
- Temperatura: {temp_c} °C   Humedad: {hum_pct} %   Índice de evaporación: {evaporacion}

Reglas:
- Responde solo con JSON: {{"prioridad": "baja|media|alta", "recomendacion": "..."}}
- Máximo dos frases, en español sencillo.
- No inventes datos que no estén arriba.
- Si la confianza es media, di que el pronóstico puede cambiar.
- Si las horas son -1, el nivel está estable; si son 0, ya se alcanzó el nivel crítico."""


def _fmt(v, dec=1) -> str:
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "sin dato"
    return f"{v:.{dec}f}" if isinstance(v, float) else str(v)


def construir_prompt(p: Pronostico, ultima: dict, cfg: Config) -> str:
    # Solo datos ambientales: sin datos personales ni ubicación (Ley 1581 de 2012)
    return PROMPT.format(
        nivel_pct=_fmt(ultima.get("nivel_pct")), nivel_critico=_fmt(cfg.nivel_critico_pct, 0),
        horas_restantes=_fmt(p.horas_restantes), confianza=p.confianza,
        estado=ultima.get("estado") or "sin dato", causa=ultima.get("causa") or "sin dato",
        temp_c=_fmt(ultima.get("temp_c")), hum_pct=_fmt(ultima.get("hum_pct")),
        evaporacion=_fmt(ultima.get("evaporacion"), 2))


def validar_respuesta(texto: str) -> Optional[Tuple[str, str]]:
    """(prioridad, recomendacion) si la respuesta cumple el formato; si no, None."""
    texto = (texto or "").strip()
    if texto.startswith("```"):  # por si el modelo envuelve el JSON en un bloque de código
        texto = texto.strip("`")
        texto = texto[texto.find("{"):]
    try:
        d = json.loads(texto)
    except (ValueError, TypeError):
        return None
    if not isinstance(d, dict):
        return None
    prioridad = str(d.get("prioridad", "")).strip().lower()
    rec = str(d.get("recomendacion", "")).strip()
    if prioridad not in ("baja", "media", "alta") or not rec or len(rec) > 400:
        return None
    return prioridad, rec


def recomendacion_plantilla(p: Pronostico, causa: Optional[str]) -> Tuple[str, str]:
    """Recomendación si Gemini falla. Las filas se revisan en orden: el caso
    "nivel estable" (−1) va primero porque −1 es numéricamente menor que 6."""
    h = p.horas_restantes
    if h is None or h < 0 or h > 24:
        prioridad, texto = "baja", "Sin acción inmediata; seguir el monitoreo."
    elif h < 6:
        prioridad, texto = "alta", "Aplicar racionamiento de inmediato y coordinar el suministro alterno."
    else:
        prioridad, texto = "media", "Programar racionamiento y avisar a la comunidad."
    if causa == "FUGA":
        texto += " Revisar tuberías y válvulas: el descenso no se explica por el clima."
    return prioridad, texto


def consultar_gemini(prompt: str, cfg: Config) -> Optional[str]:
    """Llama a la API de Gemini (REST). Devuelve el texto o None si falla o tarda."""
    if not cfg.gemini_clave:
        return None
    url = (f"https://generativelanguage.googleapis.com/v1beta/models/"
           f"{cfg.gemini_modelo}:generateContent")
    cuerpo = {
        "contents": [{"parts": [{"text": prompt}]}],
        "generationConfig": {
            "temperature": 0.2,
            "responseMimeType": "application/json",
            "responseSchema": {
                "type": "OBJECT",
                "properties": {
                    "prioridad": {"type": "STRING"},  # baja | media | alta (se valida al recibir)
                    "recomendacion": {"type": "STRING"},
                },
                "required": ["prioridad", "recomendacion"],
            },
        },
    }
    req = urllib.request.Request(
        url, data=json.dumps(cuerpo).encode("utf-8"), method="POST",
        headers={"Content-Type": "application/json", "x-goog-api-key": cfg.gemini_clave})
    try:
        with urllib.request.urlopen(req, timeout=cfg.gemini_timeout_s) as r:
            datos = json.loads(r.read().decode("utf-8"))
        return datos["candidates"][0]["content"]["parts"][0]["text"]
    except (urllib.error.URLError, TimeoutError, OSError, ValueError, KeyError, IndexError) as e:
        log.warning("Gemini no respondió correctamente: %s", e)
        return None


def recomendar(p: Pronostico, ultima: dict, cfg: Config,
               llamar=consultar_gemini) -> Tuple[str, str, str]:
    """(prioridad, recomendacion, modelo). Usa Gemini y, si falla, la plantilla (CD-16)."""
    respuesta = llamar(construir_prompt(p, ultima, cfg), cfg)
    valida = validar_respuesta(respuesta) if respuesta else None
    if valida:
        return valida[0], valida[1], cfg.gemini_modelo
    prioridad, texto = recomendacion_plantilla(p, ultima.get("causa"))
    return prioridad, texto, "plantilla"


# =============================================================================
# Mensajes para Ubidots (02e, secciones 5.4 y 7)
# =============================================================================
def tema_ubidots(nodo: str) -> str:
    return f"/v1.6/devices/{nodo}"


def payload_promedio(prom: dict, ts: int) -> str:
    return json.dumps({k: {"value": round(v, 3), "timestamp": ts} for k, v in prom.items()})


def payload_estado(estado: str, causa: Optional[str], origen: Optional[str], ts: int) -> Optional[str]:
    if estado not in ESTADOS_UBIDOTS:
        return None
    ctx = {"estado": estado, "causa": causa or "", "origen": origen or ""}
    return json.dumps({"estado_alarma": {"value": ESTADOS_UBIDOTS[estado], "timestamp": ts,
                                         "context": ctx}})


def payload_pronostico(p: Pronostico, prioridad: str, recomendacion: str, ts: int) -> str:
    ctx = {"recomendacion": recomendacion, "prioridad": prioridad,
           "confianza": p.confianza, "texto": p.texto}
    return json.dumps({"horas_restantes": {"value": p.horas_restantes, "timestamp": ts,
                                           "context": ctx}})


# =============================================================================
# Avisos por Telegram (02e, sección 8.1)
# =============================================================================
TEXTO_ESTADO = {
    "ALERTA": "🚨 ALERTA en {nodo}: riesgo de desabastecimiento",
    "PRECAUCION": "⚠️ PRECAUCIÓN en {nodo}: el riesgo está subiendo",
    "SILENCIADA": "🔕 Alarma de {nodo} silenciada",
    "NORMAL": "✅ {nodo} volvió a la normalidad",
}
TEXTO_ORIGEN = {"tablero_local": "desde el tablero local",
                "ubidots": "desde el tablero global (Ubidots)"}


def texto_alerta(nodo: str, d: dict) -> Optional[str]:
    """Mensaje de Telegram para un cambio de estado de la alarma."""
    plantilla = TEXTO_ESTADO.get(d.get("estado", ""))
    if not plantilla:
        return None
    lineas = [plantilla.format(nodo=nodo)]
    riesgo = d.get("riesgo")
    if isinstance(riesgo, (int, float)):
        lineas.append(f"Riesgo: {riesgo:.2f}")
    causa = d.get("causa")
    if causa and causa != "NINGUNA":
        lineas.append(f"Causa probable: {causa}")
    if d.get("origen") in TEXTO_ORIGEN:
        lineas.append(f"Acción {TEXTO_ORIGEN[d['origen']]}")
    if d.get("estado") == "ALERTA":
        lineas.append("Revisar el tablero y, si aplica, silenciar la alarma.")
    return "\n".join(lineas)


def enviar_telegram(texto: str, cfg: Config, abrir=urllib.request.urlopen) -> bool:
    """Envía un mensaje con la API de bots de Telegram. True si Telegram lo aceptó."""
    url = f"https://api.telegram.org/bot{cfg.telegram_token}/sendMessage"
    datos = urllib.parse.urlencode({"chat_id": cfg.telegram_chat_id, "text": texto}).encode()
    try:
        with abrir(urllib.request.Request(url, data=datos, method="POST"), timeout=10) as r:
            return bool(json.loads(r.read().decode("utf-8")).get("ok"))
    except urllib.error.HTTPError as e:
        # 401: token mal copiado · 400 "chat not found": chat_id mal o el bot no
        # está en el grupo · 403: el bot fue bloqueado o expulsado del chat
        log.warning("Telegram rechazó el mensaje: HTTP %s", e.code)
    except (urllib.error.URLError, TimeoutError, OSError, ValueError) as e:
        log.warning("No se pudo enviar el mensaje de Telegram: %s", getattr(e, "reason", e))
    return False


class AvisosTelegram:
    """Envía los avisos en su propio hilo y en orden, para que una red lenta no
    retrase la recepción de mensajes MQTT. Si falla, reintenta 3 veces."""

    def __init__(self, cfg: Config, enviar=enviar_telegram):
        self.cfg = cfg
        self.enviar = enviar
        self.activo = bool(cfg.telegram_token and cfg.telegram_chat_id)
        self.cola: "queue.Queue[Optional[str]]" = queue.Queue(maxsize=50)
        self.hilo = threading.Thread(target=self._trabajar, name="telegram", daemon=True)
        if self.activo:
            self.hilo.start()

    def avisar(self, texto: Optional[str]) -> None:
        if not (self.activo and texto):
            return
        try:
            self.cola.put_nowait(texto)
        except queue.Full:
            log.warning("Cola de Telegram llena: se descarta un aviso")

    def detener(self) -> None:
        if self.activo:
            self.cola.put(None)
            self.hilo.join(timeout=15)

    def _trabajar(self) -> None:
        while True:
            texto = self.cola.get()
            if texto is None:
                return
            for intento in range(3):
                if self.enviar(texto, self.cfg):
                    log.info("Aviso de Telegram enviado: %s", texto.splitlines()[0])
                    break
                time.sleep(5 * (intento + 1))


# =============================================================================
# Gateway: MQTT local + Ubidots
# =============================================================================
def _crear_cliente(mqtt, client_id: str):
    """Cliente paho compatible con paho-mqtt 1.x y 2.x."""
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:
        return mqtt.Client(client_id=client_id)


class Gateway:
    def __init__(self, cfg: Config):
        import paho.mqtt.client as mqtt  # se importa aquí para poder probar la lógica sin paho

        self.mqtt = mqtt
        self.cfg = cfg
        self.db = BaseDatos(cfg.db_ruta)
        self.nodos = set(cfg.nodos)
        self.ubidots_ok = threading.Event()
        self.detener = threading.Event()
        self.ultimo_pronostico = {}  # nodo -> Pronostico válido más reciente
        self.avisos = AvisosTelegram(cfg)

        # ---- Broker local (Mosquitto)
        self.local = _crear_cliente(mqtt, "gateway_sabana")
        if cfg.local_usuario:
            self.local.username_pw_set(cfg.local_usuario, cfg.local_clave)
        self.local.on_connect = self._local_conectado
        self.local.on_message = self._local_mensaje
        self.local.reconnect_delay_set(1, 30)

        # ---- Ubidots (MQTT sobre TLS, usuario = token)
        self.ubi = _crear_cliente(mqtt, f"gateway_sabana_{int(time.time())}")
        self.ubi.username_pw_set(cfg.ubidots_token, "")
        self.ubi.tls_set(ca_certs=cfg.ubidots_ca or None, cert_reqs=ssl.CERT_REQUIRED)
        self.ubi.on_connect = self._ubi_conectado
        self.ubi.on_disconnect = self._ubi_desconectado
        self.ubi.on_message = self._ubi_mensaje
        self.ubi.reconnect_delay_set(2, 60)

    # ------------------------------------------------------------------ broker local
    def _local_conectado(self, client, userdata, flags, rc, *args):
        if int(getattr(rc, "value", rc)) != 0:
            log.error("Mosquitto rechazó la conexión (código %s): revisar usuario y contraseña", rc)
            return
        client.subscribe("sabana/#", qos=1)
        log.info("Conectado a Mosquitto en %s:%s; suscrito a sabana/#",
                 self.cfg.local_host, self.cfg.local_port)

    def _local_mensaje(self, client, userdata, msg):
        partes = msg.topic.split("/")
        if len(partes) < 3 or partes[0] != "sabana":
            return
        nodo, tipo = partes[1], "/".join(partes[2:])
        if tipo == "cmd/alarma":  # son las órdenes que publica este mismo programa
            return
        if nodo not in self.nodos:
            self.nodos.add(nodo)
            self._suscribir_control(nodo)
            log.info("Nodo nuevo: %s", nodo)
        texto = msg.payload.decode("utf-8", errors="replace")
        ts = ahora_ms()
        try:
            if tipo == "lectura":
                self.db.guardar_lectura(nodo, json.loads(texto), ts)  # CD-06: primero SQLite
            elif tipo == "alerta":
                self._procesar_alerta(nodo, json.loads(texto), ts, msg.retain)
            elif tipo == "conexion":
                self._procesar_conexion(nodo, texto.strip(), ts, msg.retain)
        except ValueError:
            log.warning("Mensaje inválido en %s: %.120s", msg.topic, texto)

    def _procesar_alerta(self, nodo: str, d: dict, ts: int, retenido: bool):
        estado = d.get("estado", "")
        # Un mensaje retenido es el último estado conocido (al reiniciar el
        # gateway): actualiza Ubidots pero no se registra como evento nuevo.
        if not retenido:
            self.db.guardar_evento(nodo, "alerta", d.get("origen"),
                                   f"{estado} riesgo={d.get('riesgo')} causa={d.get('causa')}", ts)
            self.avisos.avisar(texto_alerta(nodo, d))  # sin red, Telegram reintenta aparte
        p = payload_estado(estado, d.get("causa"), d.get("origen"), ts)
        if p:
            self.publicar_ubidots(nodo, p, ts)  # inmediato, sin esperar el promedio

    def _procesar_conexion(self, nodo: str, texto: str, ts: int, retenido: bool):
        en_linea = 1 if texto == "en_linea" else 0
        if not retenido:
            self.db.guardar_evento(nodo, "conexion", "automatico", texto, ts)
            if not en_linea:
                self.avisos.avisar(f"📡 {nodo} se desconectó: sin datos del nodo. "
                                   "Revisar la energía y el Wi-Fi.")
        self.publicar_ubidots(nodo, json.dumps({"nodo_en_linea": {"value": en_linea,
                                                                   "timestamp": ts}}), ts)

    # ------------------------------------------------------------------ Ubidots
    def _ubi_conectado(self, client, userdata, flags, rc, *args):
        if int(getattr(rc, "value", rc)) != 0:
            log.error("Ubidots rechazó la conexión (código %s): revisar el token", rc)
            return
        self.ubidots_ok.set()
        for nodo in self.nodos:
            self._suscribir_control(nodo)
        log.info("Conectado a Ubidots (%s)", self.cfg.ubidots_host)

    def _ubi_desconectado(self, client, userdata, *args):
        self.ubidots_ok.clear()
        log.warning("Desconectado de Ubidots; los mensajes quedan en la tabla pendientes")

    def _suscribir_control(self, nodo: str):
        if self.ubidots_ok.is_set():
            # /lv entrega solo el último valor de la variable de control
            self.ubi.subscribe(f"{tema_ubidots(nodo)}/silenciar/lv", qos=1)

    def _ubi_mensaje(self, client, userdata, msg):
        # Tema: /v1.6/devices/<nodo>/silenciar/lv  ·  contenido: "1.0" o "0.0"
        partes = msg.topic.strip("/").split("/")
        if len(partes) < 5 or partes[3] != "silenciar":
            return
        nodo = partes[2]
        try:
            valor = float(msg.payload.decode("utf-8"))
        except ValueError:
            return
        if valor < 0.5:
            return
        ts = ahora_ms()
        self.db.guardar_evento(nodo, "silenciar", "ubidots", "orden enviada al nodo", ts)
        self.local.publish(f"sabana/{nodo}/cmd/alarma", "silenciar", qos=1)
        # El interruptor vuelve a 0; el tablero mostrará SILENCIADA cuando el ESP32
        # confirme con su mensaje `alerta` (no con la orden).
        self.ubi.publish(tema_ubidots(nodo), json.dumps({"silenciar": 0}), qos=1)
        log.info("Orden silenciar reenviada a %s", nodo)

    def publicar_ubidots(self, nodo: str, mensaje: str, ts: int) -> None:
        """Publica en Ubidots o, si no hay conexión, guarda en `pendientes` (CD-07)."""
        tema = tema_ubidots(nodo)
        if self.ubidots_ok.is_set() and self.db.contar_pendientes() == 0:
            info = self.ubi.publish(tema, mensaje, qos=1)
            if info.rc == self.mqtt.MQTT_ERR_SUCCESS:
                return
        self.db.encolar_pendiente(tema, mensaje, ts)

    def reenviar_pendientes(self) -> None:
        if not self.ubidots_ok.is_set():
            return
        filas = self.db.pendientes()
        for f in filas:
            info = self.ubi.publish(f["tema"], f["mensaje"], qos=1)
            if info.rc != self.mqtt.MQTT_ERR_SUCCESS:
                return
            self.db.borrar_pendiente(f["id"])
        if filas:
            log.info("Reenviados %d mensajes pendientes a Ubidots", len(filas))

    # ------------------------------------------------------------------ tareas periódicas
    def enviar_promedios(self, inicio_minuto_ms: int) -> None:
        for nodo in sorted(self.nodos):
            prom = self.db.promedio_minuto(nodo, inicio_minuto_ms)
            if prom:
                self.publicar_ubidots(nodo, payload_promedio(prom, inicio_minuto_ms),
                                      inicio_minuto_ms)

    def ejecutar_ia(self) -> None:
        ts = ahora_ms()
        for nodo in sorted(self.nodos):
            ultima_fila = self.db.ultima_lectura(nodo)
            ultima = dict(ultima_fila) if ultima_fila else {}
            puntos = self.db.niveles_por_minuto(nodo, ts - self.cfg.ventana_min * 60_000)
            p = pronosticar(puntos, ultima.get("nivel_pct"), self.cfg)
            if not p.consultar_gemini:
                previo = self.ultimo_pronostico.get(nodo)
                log.info("IA %s: %s; se mantiene el último pronóstico (%s)", nodo, p.texto,
                         previo.texto if previo else "ninguno")
                continue
            prioridad, rec, modelo = recomendar(p, ultima, self.cfg)
            self.db.guardar_prediccion(nodo, p, prioridad, rec, modelo, ts)
            self.publicar_ubidots(nodo, payload_pronostico(p, prioridad, rec, ts), ts)
            self.ultimo_pronostico[nodo] = p
            log.info("IA %s: %s (horas=%s, modelo=%s, prioridad=%s)", nodo, p.texto,
                     p.horas_restantes, modelo, prioridad)

    # ------------------------------------------------------------------ ciclo principal
    def ejecutar(self) -> None:
        self.local.connect_async(self.cfg.local_host, self.cfg.local_port, keepalive=30)
        self.ubi.connect_async(self.cfg.ubidots_host, self.cfg.ubidots_port, keepalive=60)
        self.local.loop_start()
        self.ubi.loop_start()

        minuto_actual = ahora_ms() // 60_000
        proxima_ia = time.time() + self.cfg.periodo_ia_s
        try:
            while not self.detener.wait(1.0):
                m = ahora_ms() // 60_000
                if m != minuto_actual:            # se cerró un minuto: enviar su promedio
                    self.enviar_promedios(minuto_actual * 60_000)
                    minuto_actual = m
                self.reenviar_pendientes()
                if time.time() >= proxima_ia:
                    proxima_ia = time.time() + self.cfg.periodo_ia_s
                    try:
                        self.ejecutar_ia()
                    except Exception:  # la IA nunca debe detener el gateway (CD-16)
                        log.exception("Error en el módulo de IA")
        finally:
            self.avisos.detener()
            self.local.loop_stop()
            self.ubi.loop_stop()
            self.local.disconnect()
            self.ubi.disconnect()
            log.info("Gateway detenido")


def main():
    ap = argparse.ArgumentParser(description="Gateway IoT del monitor hídrico (Challenge 3)")
    ap.add_argument("--config", default="gateway.conf", help="archivo de configuración")
    ap.add_argument("-v", "--verbose", action="store_true", help="mostrar mensajes de depuración")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    cfg = cargar_config(args.config)
    if not cfg.ubidots_token:
        log.warning("Sin token de Ubidots: todo quedará en la tabla pendientes")
    if not cfg.gemini_clave:
        log.warning("Sin clave de Gemini: se usará siempre la recomendación de plantilla")
    if not (cfg.telegram_token and cfg.telegram_chat_id):
        log.warning("Sin token o chat_id de Telegram: no se enviarán avisos")
    gw = Gateway(cfg)
    signal.signal(signal.SIGINT, lambda *_: gw.detener.set())
    signal.signal(signal.SIGTERM, lambda *_: gw.detener.set())
    gw.ejecutar()


if __name__ == "__main__":
    main()
