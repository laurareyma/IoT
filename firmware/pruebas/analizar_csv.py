#!/usr/bin/env python3
"""
analizar_csv.py — Estadísticas y gráficas a partir del log serie del nodo.

Uso:
    1. Abrir el Monitor Serie (115200 baudios), copiar la salida y guardarla
       como un archivo .csv (las líneas que no son de datos se ignoran).
    2. python3 analizar_csv.py prueba.csv [--desde SEG] [--hasta SEG] [--ref-nivel CM]

Salida:
    - Tabla en Markdown (media, máximo, mínimo, desviación estándar por variable)
      lista para pegar en la wiki (página 04, Resultados y Análisis).
    - Conteo de muestras por estado de la alarma y transiciones con su tiempo.
    - Periodo real entre muestras.
    - Si matplotlib está instalado: prueba_variables.png y prueba_riesgo.png.

Solo requiere la biblioteca estándar de Python 3; matplotlib es opcional.
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

COLUMNAS = ["t_ms", "nivel_cm", "temp_c", "hum_pct", "presion_hpa", "ldr",
            "evaporacion", "tasa_cm", "riesgo", "estado", "causa"]
VARIABLES = [  # (columna, nombre, unidad, decimales)
    ("nivel_cm", "Nivel", "cm", 2),
    ("temp_c", "Temperatura", "°C", 2),
    ("hum_pct", "Humedad", "%HR", 2),
    ("presion_hpa", "Presión", "hPa", 2),
    ("ldr", "LDR", "cuentas", 0),
    ("evaporacion", "Evaporación", "índice", 3),
    ("riesgo", "Riesgo", "índice", 3),
]
UMBRAL_PRECAUCION, UMBRAL_ALERTA = 0.40, 0.65


def leer(ruta):
    """Devuelve una lista de diccionarios con las líneas de datos válidas."""
    filas = []
    with open(ruta, encoding="utf-8", errors="replace") as f:
        for linea in f:
            partes = next(csv.reader([linea.strip()]))
            if len(partes) != len(COLUMNAS) or not partes[0].isdigit():
                continue  # encabezado, mensajes [EVENTO], avisos, etc.
            fila = dict(zip(COLUMNAS, partes))
            try:
                for col in COLUMNAS[:-2]:
                    fila[col] = float(fila[col])
            except ValueError:
                continue
            filas.append(fila)
    return filas


def numero(v):
    return v == v  # descarta NaN


def tabla_estadisticas(filas, ref_nivel):
    print("| Variable | Unidad | n | Media | Máximo | Mínimo | σ |" +
          (" Referencia | Error medio |" if ref_nivel is not None else ""))
    print("|---|---|---|---|---|---|---|" + ("---|---|" if ref_nivel is not None else ""))
    for col, nombre, unidad, d in VARIABLES:
        valores = [f[col] for f in filas if numero(f[col])]
        if not valores:
            continue
        media = statistics.fmean(valores)
        sigma = statistics.stdev(valores) if len(valores) > 1 else 0.0
        linea = (f"| {nombre} | {unidad} | {len(valores)} | {media:.{d}f} | "
                 f"{max(valores):.{d}f} | {min(valores):.{d}f} | {sigma:.{d + 1}f} |")
        if ref_nivel is not None:
            linea += (f" {ref_nivel:.1f} | {media - ref_nivel:+.2f} |" if col == "nivel_cm" else " — | — |")
        print(linea)


def resumen_estados(filas):
    t0 = filas[0]["t_ms"]
    conteo = {}
    for f in filas:
        conteo[f["estado"]] = conteo.get(f["estado"], 0) + 1
    print("\n| Estado | Muestras | % |\n|---|---|---|")
    for estado, n in sorted(conteo.items(), key=lambda x: -x[1]):
        print(f"| {estado} | {n} | {100 * n / len(filas):.1f} |")

    print("\nTransiciones:")
    anterior = filas[0]["estado"]
    for f in filas[1:]:
        if f["estado"] != anterior:
            print(f"  t = {(f['t_ms'] - t0) / 1000:7.1f} s  {anterior} -> {f['estado']}"
                  f"  (riesgo {f['riesgo']:.3f}, causa {f['causa']})")
            anterior = f["estado"]


def resumen_periodo(filas):
    periodos = [(b["t_ms"] - a["t_ms"]) for a, b in zip(filas, filas[1:])]
    if not periodos:
        return
    print(f"\nPeriodo entre muestras: media {statistics.fmean(periodos):.0f} ms, "
          f"mín {min(periodos):.0f} ms, máx {max(periodos):.0f} ms "
          f"(incluye el tiempo de lectura de los sensores, no solo el del timer)")


def graficar(filas, base):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n(matplotlib no está instalado: se omiten las gráficas. "
              "Instalar con: python3 -m pip install matplotlib)")
        return
    t0 = filas[0]["t_ms"]
    t = [(f["t_ms"] - t0) / 1000 for f in filas]

    fig, ejes = plt.subplots(4, 1, figsize=(9, 9), sharex=True)
    for eje, (col, nombre, unidad) in zip(ejes, [("nivel_cm", "Nivel", "cm"),
                                                ("temp_c", "Temperatura", "°C"),
                                                ("hum_pct", "Humedad", "%HR"),
                                                ("evaporacion", "Evaporación", "índice 0–1")]):
        eje.plot(t, [f[col] for f in filas], linewidth=1.5)
        eje.set_ylabel(f"{nombre}\n({unidad})")
        eje.grid(alpha=0.3)
    ejes[-1].set_xlabel("Tiempo (s)")
    fig.suptitle("Variables medidas")
    fig.tight_layout()
    fig.savefig(f"{base}_variables.png", dpi=150)

    fig, eje = plt.subplots(figsize=(9, 4))
    eje.plot(t, [f["riesgo"] for f in filas], linewidth=1.8, label="Riesgo")
    eje.axhline(UMBRAL_PRECAUCION, linestyle="--", color="#c77700", label="Precaución (0.40)")
    eje.axhline(UMBRAL_ALERTA, linestyle="--", color="#c62828", label="Alerta (0.65)")
    en_alerta = [f["estado"] in ("ALERTA", "SILENCIADA") for f in filas]
    eje.fill_between(t, 0, 1, where=en_alerta, color="#c62828", alpha=0.08,
                     step="post", label="Alarma activa")
    eje.set_ylim(0, 1)
    eje.set_xlabel("Tiempo (s)")
    eje.set_ylabel("Riesgo (0–1)")
    eje.grid(alpha=0.3)
    eje.legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(f"{base}_riesgo.png", dpi=150)
    print(f"\nGráficas: {base}_variables.png, {base}_riesgo.png")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv")
    p.add_argument("--desde", type=float, default=None, help="segundos desde la primera muestra")
    p.add_argument("--hasta", type=float, default=None)
    p.add_argument("--ref-nivel", type=float, default=None, help="nivel real medido con regla (cm)")
    a = p.parse_args()

    filas = leer(a.csv)
    if not filas:
        sys.exit("No se encontraron líneas de datos en el archivo.")
    t0 = filas[0]["t_ms"]
    if a.desde is not None:
        filas = [f for f in filas if (f["t_ms"] - t0) / 1000 >= a.desde]
    if a.hasta is not None:
        filas = [f for f in filas if (f["t_ms"] - t0) / 1000 <= a.hasta]
    if not filas:
        sys.exit("No quedan muestras en el intervalo pedido.")

    print(f"Archivo: {a.csv} — {len(filas)} muestras, "
          f"{(filas[-1]['t_ms'] - filas[0]['t_ms']) / 1000:.0f} s\n")
    tabla_estadisticas(filas, a.ref_nivel)
    resumen_estados(filas)
    resumen_periodo(filas)
    graficar(filas, str(Path(a.csv).with_suffix("")))


if __name__ == "__main__":
    main()
