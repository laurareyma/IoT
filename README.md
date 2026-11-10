# Monitor hídrico IoT — Sabana Centro

Código del sistema IoT de bajo costo para vigilar el nivel de agua de tanques comunitarios en Sabana Centro (Cundinamarca) y alertar del riesgo de desabastecimiento.

Internet de las Cosas 2026-2, Universidad de La Sabana. Equipo: Laura Vanesa Reyes Martínez y Ana María Cordero Escobar.

**Documentación completa:** [wiki del proyecto](https://github.com/laurareyma/IoT/wiki) (diseño, diagramas UML, protocolo de pruebas, resultados y declaración de uso de IA).

```
ESP32 (sensores, fusión, alarma, tablero local)
   |  MQTT  sabana/<nodo>/...
   v
Raspberry Pi: Mosquitto -> gateway.py -> SQLite
                               |-> Ubidots (MQTT + TLS): tablero global
                               |-> Módulo de IA: regresión + Gemini
                               '-> Telegram: avisos de la alarma
```

## Carpetas

| Carpeta | Contenido | Cómo usarlo |
|---|---|---|
| [`firmware/monitor_hidrico/`](firmware/monitor_hidrico) | Firmware del ESP32: medición con ISR y tareas FreeRTOS, lógica de fusión, alarma física, LCD, tablero web local con acceso restringido y cliente MQTT | [README del firmware](firmware/monitor_hidrico/README.md) |
| [`firmware/pruebas/`](firmware/pruebas) | 25 pruebas unitarias de la lógica de fusión y de la máquina de alarma, en el computador | `c++ -std=c++17 -I stub -I ../monitor_hidrico test_fusion.cpp ../monitor_hidrico/fusion.cpp -o test_fusion && ./test_fusion` |
| [`gateway/`](gateway) | Programa del gateway (Raspberry Pi): SQLite, envío a Ubidots, módulo de IA, avisos por Telegram y orden de silenciar; configuración de Mosquitto y servicio de systemd | [README del gateway](gateway/README.md) · pruebas: `python3 test_gateway.py` (24) |
| `Anexos/` | Material de apoyo de la documentación | — |

## Secretos

Las credenciales nunca se suben al repositorio: `firmware/monitor_hidrico/secrets.h` (Wi-Fi, usuarios del tablero y del broker) y `gateway/gateway.conf` (claves de Mosquitto, token de Ubidots, clave de Gemini y token del bot de Telegram) están en `.gitignore`. Cada carpeta tiene una plantilla (`secrets.h.example`, `gateway.conf.example`).
