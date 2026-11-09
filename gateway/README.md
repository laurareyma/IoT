# Gateway — Raspberry Pi (Challenge 3)

Programa del gateway IoT: recibe por MQTT los datos del nodo ESP32, los guarda en SQLite, los promedia por minuto y los envía a Ubidots, ejecuta el módulo de IA y reenvía al ESP32 la orden de silenciar que llega desde el tablero global.

Diseño: wiki, páginas [02e, sección 5](https://github.com/laurareyma/IoT/wiki/02e-Tablero-Web-y-Seguridad#5-gateway-sqlite-procesamiento-y-envío-a-la-nube) y [02c, sección 4](https://github.com/laurareyma/IoT/wiki/02c-Arquitectura-y-Logica-de-Fusion#4-módulo-de-inteligencia-artificial).

```
ESP32 --MQTT--> Mosquitto --> gateway.py --> SQLite (lecturas, eventos, predicciones, pendientes)
                                         \--> Ubidots (MQTT + TLS, puerto 8883)
Ubidots (interruptor "silenciar") --> gateway.py --> sabana/<nodo>/cmd/alarma --> ESP32
```

## Archivos

| Archivo | Contenido |
|---|---|
| `gateway.py` | Programa del gateway |
| `schema.sql` | Esquema de SQLite (tablas `lecturas`, `eventos`, `predicciones`, `pendientes`). `gateway.py` lo aplica al arrancar; también sirve para crear la base a mano: `sqlite3 gateway.db < schema.sql` |
| `test_gateway.py` | 19 pruebas de la lógica (pronóstico, plantilla, validación de Gemini, SQLite, mensajes de Ubidots). No necesitan red ni `paho-mqtt` |
| `gateway.conf.example` | Plantilla de configuración. Copiar como `gateway.conf` (está en `.gitignore`) |
| `requirements.txt` | Única dependencia externa: `paho-mqtt`. La llamada a Gemini usa `urllib` (biblioteca estándar) contra la API REST, así que no hacen falta `requests` ni `google-genai` |
| `mosquitto/sabana.conf`, `mosquitto/acl` | Configuración del broker local: escucha en la red, usuarios y lista de control de acceso |
| `gateway.service` | Servicio de systemd para que el gateway arranque con la Raspberry Pi |

## Instalación en la Raspberry Pi

```bash
# 1. Mosquitto y Python
sudo apt update && sudo apt install -y mosquitto mosquitto-clients python3-paho-mqtt sqlite3

# 2. Configurar el broker (usuarios: gateway y uno por nodo, con su NODO_ID)
sudo cp mosquitto/sabana.conf /etc/mosquitto/conf.d/
sudo cp mosquitto/acl /etc/mosquitto/acl
sudo mosquitto_passwd -c /etc/mosquitto/passwd gateway
sudo mosquitto_passwd /etc/mosquitto/passwd tanque01
sudo chown mosquitto: /etc/mosquitto/passwd /etc/mosquitto/acl
sudo systemctl restart mosquitto

# 3. Gateway
mkdir -p /home/pi/gateway && cp gateway.py schema.sql gateway.conf.example /home/pi/gateway/
cd /home/pi/gateway && cp gateway.conf.example gateway.conf
nano gateway.conf          # clave de Mosquitto, token de Ubidots y clave de Gemini
python3 gateway.py --config gateway.conf      # prueba en primer plano (Ctrl+C para salir)

# 4. Que arranque solo
sudo cp gateway.service /etc/systemd/system/
sudo systemctl enable --now gateway
journalctl -u gateway -f   # ver el registro
```

En el ESP32, `secrets.h` debe tener `MQTT_HOST` = IP de la Raspberry Pi, `MQTT_USER "tanque01"` y la misma contraseña que se creó con `mosquitto_passwd`. El usuario debe ser igual a `NODO_ID`, porque la ACL limita cada nodo a sus propios temas (`%u`).

## Qué hace

| Mensaje recibido | Acción |
|---|---|
| `sabana/<nodo>/lectura` | Se guarda en `lecturas` de inmediato (CD-06). Cada minuto se promedian y se envían a Ubidots: `nivel_pct`, `temp_c`, `hum_pct`, `presion_hpa`, `evaporacion`, `riesgo` |
| `sabana/<nodo>/alerta` | Fila en `eventos` y `estado_alarma` a Ubidots de inmediato (0 NORMAL, 1 PRECAUCION, 2 ALERTA, 3 SILENCIADA) |
| `sabana/<nodo>/conexion` | Fila en `eventos` y `nodo_en_linea` (1/0) a Ubidots |
| Ubidots `/v1.6/devices/<nodo>/silenciar/lv` = 1 | Fila en `eventos` (origen ubidots), publica `silenciar` en `sabana/<nodo>/cmd/alarma` (QoS 1) y devuelve el interruptor a 0 |

- **Sin internet:** lo que va a Ubidots se guarda en `pendientes` y se envía en orden, con su marca de tiempo original, al volver la conexión (CD-07).
- **Mensajes retenidos** (último estado al reiniciar el gateway): actualizan Ubidots, pero no se registran como eventos nuevos.

## Módulo de IA (cada 5 min)

1. Promedios por minuto de `nivel_pct` de los últimos 15 min, **sin las lecturas en que el HC-SR04 no dio eco** (el firmware asume tanque vacío y eso falsearía el pronóstico).
2. Casos en este orden: nivel ≤ 20 % → 0 h · menos de 5 datos → "Tendencia no clara" · pendiente ≥ −0.5 %/h → −1 ("Nivel estable") · R² < 0.4 → "Tendencia no clara" · si no, horas = (nivel − 20) / |pendiente|.
3. En los casos con resultado nuevo, pide la recomendación a Gemini (REST, respuesta en JSON, 10 s como máximo). Si no responde, la respuesta no es válida o no hay clave, usa la plantilla y registra `plantilla` como modelo.
4. Guarda en `predicciones` y publica `horas_restantes` en Ubidots, con la recomendación, la prioridad y la confianza en el contexto.

La IA no envía nada al ESP32: no puede encender, apagar ni retrasar la alarma (CD-15). El nombre del modelo de Gemini se cambia en `gateway.conf` si Google lo retira.

## Pruebas

```bash
python3 test_gateway.py                       # lógica, sin red
mosquitto_sub -v -t 'sabana/#' -u gateway -P <clave>        # ver lo que publica el ESP32
mosquitto_pub -t sabana/tanque01/cmd/alarma -m silenciar -u gateway -P <clave>
sqlite3 /home/pi/gateway/gateway.db "SELECT hora_utc, nivel_pct, estado FROM lecturas ORDER BY id DESC LIMIT 5;"
sqlite3 /home/pi/gateway/gateway.db "SELECT hora_utc, horas_restantes, modelo, recomendacion FROM predicciones ORDER BY id DESC LIMIT 3;"
```
