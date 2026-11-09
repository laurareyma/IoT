#!/usr/bin/env python3
"""
Pruebas de la lógica del gateway que no necesitan red ni hardware:
pronóstico (casos de la prueba IA-01), plantilla, validación de la respuesta
de Gemini (IA-03), SQLite (M-02, M-03) y mensajes para Ubidots.

Ejecutar:  python3 test_gateway.py
No necesita paho-mqtt ni conexión a internet.
"""
import json
import os
import random
import tempfile
import unittest

import gateway as gw


def serie(inicio, pendiente_por_min, n, ruido=0.0, semilla=1):
    """n promedios por minuto: (hora en h, nivel %)."""
    rnd = random.Random(semilla)
    return [(i / 60.0, inicio + pendiente_por_min * i + rnd.uniform(-ruido, ruido))
            for i in range(n)]


class PruebasPronostico(unittest.TestCase):
    """Prueba IA-01: casos de 02c §4.2 en orden."""

    def setUp(self):
        self.cfg = gw.Config()

    def test_a_bajo_el_critico_da_cero(self):
        p = gw.pronosticar(serie(25, -1, 15), 18.0, self.cfg)
        self.assertEqual((p.caso, p.horas_restantes), (1, 0.0))
        self.assertTrue(p.consultar_gemini)

    def test_b_pocos_datos_no_consulta_gemini(self):
        p = gw.pronosticar(serie(60, -2, 3), 56.0, self.cfg)
        self.assertEqual(p.caso, 2)
        self.assertEqual(p.texto, "Tendencia no clara")
        self.assertFalse(p.consultar_gemini)

    def test_c_nivel_quieto_con_ruido_es_estable(self):
        # Ruido de ±0.3 % del ultrasónico: R² bajo, pero debe salir "Nivel estable"
        p = gw.pronosticar(serie(70, 0.0, 15, ruido=0.3), 70.0, self.cfg)
        self.assertEqual((p.caso, p.horas_restantes, p.texto), (3, -1.0, "Nivel estable"))

    def test_d_datos_dispersos_no_consulta_gemini(self):
        puntos = [(i / 60.0, 60 - 0.05 * i + (8 if i % 2 else -8)) for i in range(15)]
        p = gw.pronosticar(puntos, 60.0, self.cfg)
        self.assertEqual(p.caso, 4)
        self.assertFalse(p.consultar_gemini)

    def test_e_descenso_constante_igual_al_calculo_manual(self):
        # 2 %/min = 120 %/h desde 60 %. Último promedio = 60 − 2·14 = 32 %.
        # Horas = (32 − 20) / 120 = 0.1 h
        puntos = serie(60, -2, 15)
        p = gw.pronosticar(puntos, 32.0, self.cfg)
        self.assertEqual(p.caso, 5)
        self.assertAlmostEqual(p.pendiente, -120.0, places=6)
        self.assertAlmostEqual(p.r2, 1.0, places=6)
        self.assertAlmostEqual(p.horas_restantes, 0.1, places=2)
        self.assertEqual(p.confianza, "alta")

    def test_regresion_conocida(self):
        a, b, r2 = gw.regresion_lineal([(0, 1), (1, 3), (2, 5)])
        self.assertAlmostEqual(a, 1.0)
        self.assertAlmostEqual(b, 2.0)
        self.assertAlmostEqual(r2, 1.0)


class PruebasRecomendacion(unittest.TestCase):
    """Plantilla (02c §4.4) y validación de la respuesta (IA-02, IA-03)."""

    def p(self, horas):
        return gw.Pronostico(5, horas, -1, 0.9, "alta", "")

    def test_plantilla_estable_es_baja_aunque_menos_uno_sea_menor_que_6(self):
        self.assertEqual(gw.recomendacion_plantilla(self.p(-1.0), "NINGUNA")[0], "baja")

    def test_plantilla_rangos(self):
        self.assertEqual(gw.recomendacion_plantilla(self.p(0.0), None)[0], "alta")
        self.assertEqual(gw.recomendacion_plantilla(self.p(5.9), None)[0], "alta")
        self.assertEqual(gw.recomendacion_plantilla(self.p(6.0), None)[0], "media")
        self.assertEqual(gw.recomendacion_plantilla(self.p(24.0), None)[0], "media")
        self.assertEqual(gw.recomendacion_plantilla(self.p(30.0), None)[0], "baja")

    def test_plantilla_fuga_agrega_texto(self):
        _, texto = gw.recomendacion_plantilla(self.p(10.0), "FUGA")
        self.assertIn("tuberías", texto)

    def test_validar_respuesta(self):
        ok = gw.validar_respuesta('{"prioridad": "Alta", "recomendacion": "Racionar hoy."}')
        self.assertEqual(ok, ("alta", "Racionar hoy."))
        envuelta = '```json\n{"prioridad":"media","recomendacion":"Avisar."}\n```'
        self.assertEqual(gw.validar_respuesta(envuelta), ("media", "Avisar."))
        self.assertIsNone(gw.validar_respuesta("Debe racionar el agua."))
        self.assertIsNone(gw.validar_respuesta('{"prioridad":"urgente","recomendacion":"x"}'))
        self.assertIsNone(gw.validar_respuesta('{"prioridad":"alta","recomendacion":""}'))

    def test_falla_de_gemini_usa_plantilla(self):
        cfg = gw.Config()
        p = self.p(3.0)
        sin_respuesta = gw.recomendar(p, {"causa": "EVAPORACION"}, cfg, llamar=lambda *_: None)
        self.assertEqual(sin_respuesta[2], "plantilla")
        mal_formato = gw.recomendar(p, {}, cfg, llamar=lambda *_: "no es json")
        self.assertEqual(mal_formato[2], "plantilla")
        buena = gw.recomendar(p, {}, cfg,
                              llamar=lambda *_: '{"prioridad":"alta","recomendacion":"Racionar."}')
        self.assertEqual(buena, ("alta", "Racionar.", cfg.gemini_modelo))

    def test_prompt_sin_datos_inventados(self):
        cfg = gw.Config()
        texto = gw.construir_prompt(self.p(3.0), {"nivel_pct": 32.0, "estado": "ALERTA",
                                                  "causa": "FUGA", "temp_c": None}, cfg)
        self.assertIn("Nivel actual: 32.0 %", texto)
        self.assertIn("Temperatura: sin dato", texto)
        self.assertIn('{"prioridad": "baja|media|alta"', texto)


class PruebasBaseDatos(unittest.TestCase):
    """M-02 (todo queda en SQLite) y M-03 (promedio por minuto)."""

    def setUp(self):
        fd, self.ruta = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        self.db = gw.BaseDatos(self.ruta)

    def tearDown(self):
        self.db.con.close()
        for sufijo in ("", "-wal", "-shm"):
            if os.path.exists(self.ruta + sufijo):
                os.remove(self.ruta + sufijo)

    def lectura(self, nivel, riesgo=0.5, nivel_ok=True, temp=None):
        return {"t": 1000, "nivel_pct": nivel, "riesgo": riesgo, "temp_c": temp,
                "estado": "NORMAL", "causa": "NINGUNA",
                "sensores": {"nivel": nivel_ok, "dht22": temp is not None, "bmp280": True}}

    def test_promedio_por_minuto(self):
        base = 1_700_000_040_000 // 60_000 * 60_000
        for i, nivel in enumerate([50, 52, 54, 56]):   # 4 lecturas del mismo minuto
            self.db.guardar_lectura("tanque01", self.lectura(nivel, temp=20.0 + i), base + i * 5000)
        self.db.guardar_lectura("tanque01", self.lectura(99), base + 60_000)  # minuto siguiente
        prom = self.db.promedio_minuto("tanque01", base)
        self.assertAlmostEqual(prom["nivel_pct"], 53.0)
        self.assertAlmostEqual(prom["temp_c"], 21.5)
        self.assertAlmostEqual(prom["riesgo"], 0.5)
        self.assertIsNone(self.db.promedio_minuto("tanque01", base - 60_000))

    def test_lecturas_sin_eco_no_entran_al_pronostico(self):
        base = 1_700_000_040_000 // 60_000 * 60_000
        self.db.guardar_lectura("tanque01", self.lectura(60), base)
        self.db.guardar_lectura("tanque01", self.lectura(0, nivel_ok=False), base + 60_000)
        puntos = self.db.niveles_por_minuto("tanque01", 0)
        self.assertEqual([round(n) for _, n in puntos], [60])
        self.assertEqual(self.db.ultima_lectura("tanque01")["nivel_pct"], 60)

    def test_pendientes_en_orden(self):
        for i in range(3):
            self.db.encolar_pendiente("/v1.6/devices/tanque01", json.dumps({"i": i}), i)
        filas = self.db.pendientes()
        self.assertEqual([json.loads(f["mensaje"])["i"] for f in filas], [0, 1, 2])
        self.db.borrar_pendiente(filas[0]["id"])
        self.assertEqual(self.db.contar_pendientes(), 2)

    def test_eventos_y_predicciones(self):
        self.db.guardar_evento("tanque01", "silenciar", "ubidots", "orden enviada", 1)
        p = gw.Pronostico(5, 3.5, -2.0, 0.8, "alta", "3.5 h")
        self.db.guardar_prediccion("tanque01", p, "alta", "Racionar.", "plantilla", 2)
        fila = self.db._consultar("SELECT * FROM predicciones")[0]
        self.assertEqual((fila["horas_restantes"], fila["modelo"]), (3.5, "plantilla"))


class PruebasUbidots(unittest.TestCase):
    def test_estado_alarma(self):
        d = json.loads(gw.payload_estado("SILENCIADA", "FUGA", "ubidots", 123))
        self.assertEqual(d["estado_alarma"]["value"], 3)
        self.assertEqual(d["estado_alarma"]["timestamp"], 123)
        self.assertIsNone(gw.payload_estado("DESCONOCIDO", None, None, 1))

    def test_promedio_lleva_marca_de_tiempo_original(self):
        d = json.loads(gw.payload_promedio({"nivel_pct": 53.0}, 1_700_000_000_000))
        self.assertEqual(d["nivel_pct"], {"value": 53.0, "timestamp": 1_700_000_000_000})

    def test_pronostico_con_recomendacion_en_contexto(self):
        p = gw.Pronostico(3, -1.0, 0.1, 0.1, "no aplica", "Nivel estable")
        d = json.loads(gw.payload_pronostico(p, "baja", "Seguir el monitoreo.", 5))
        self.assertEqual(d["horas_restantes"]["value"], -1.0)
        self.assertEqual(d["horas_restantes"]["context"]["recomendacion"], "Seguir el monitoreo.")


if __name__ == "__main__":
    unittest.main(verbosity=2)
