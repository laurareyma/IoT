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

## Configurar Ubidots

Los nombres de los menús pueden variar un poco según el plan y la versión de Ubidots. Lo que importa es que las **etiquetas** (*labels*) coincidan exactamente con las de esta guía, porque el gateway publica con esos nombres.

### 1. Token

1. En Ubidots, menú del usuario (arriba a la derecha) → **API Credentials**.
2. Copiar el **Default token** en `gateway.conf`:
   ```ini
   [ubidots]
   token = BBUS-xxxxxxxxxxxxxxxx
   ```
3. Reiniciar el gateway (`sudo systemctl restart gateway`). En el registro (`journalctl -u gateway -f`) debe aparecer `Conectado a Ubidots`. Si dice `Ubidots rechazó la conexión`, el token está mal copiado.

El token da acceso a toda la cuenta: no se sube al repositorio (`gateway.conf` está en `.gitignore`) ni se muestra en el video.

### 2. Dispositivo y variables

El dispositivo `tanque01` y sus variables **se crean solos** la primera vez que el gateway publica (al minuto de recibir lecturas del ESP32). Para verificar: **Devices → Devices → tanque01**.

| Variable (label) | Qué es | La crea |
|---|---|---|
| `nivel_pct`, `temp_c`, `hum_pct`, `presion_hpa`, `evaporacion`, `riesgo` | Promedio de cada minuto | El gateway |
| `estado_alarma` | 0 NORMAL · 1 PRECAUCION · 2 ALERTA · 3 SILENCIADA | El gateway, con la primera alerta |
| `nodo_en_linea` | 1 = el ESP32 está reportando; 0 = se desconectó | El gateway, al conectarse el ESP32 |
| `horas_restantes` | Pronóstico de la IA (−1 = nivel estable; 0 = nivel crítico alcanzado). La recomendación, la prioridad y la confianza van en su *context* | El gateway, con el primer pronóstico (a los 5 min) |
| `silenciar` | Interruptor para silenciar la alarma física | **A mano** (paso 3) |

### 3. Variable de control `silenciar`

1. En **Devices → tanque01**, agregar una variable nueva de tipo **Raw**.
2. Nombre y label: `silenciar` (en minúsculas, exactamente así).
3. Darle valor inicial **0**.

El gateway se suscribe a esta variable. Cuando vale 1, envía `silenciar` al ESP32 y la devuelve a 0.

### 4. Tablero global

**Data → Dashboards → + Nuevo tablero** (nombre sugerido: *Monitor hídrico — tanque01*). Widgets (diseño en la wiki, 02e sección 7):

| Widget | Variable | Configuración |
|---|---|---|
| Indicador (*Indicator*) o métrica con colores | `estado_alarma` | Rangos de color: 0 verde, 1 ámbar, 2 rojo, 3 rojo (silenciada) |
| Métrica (*Metric*), uno por variable | `nivel_pct`, `temp_c`, `hum_pct`, `presion_hpa`, `evaporacion`, `riesgo` | Último valor, con unidades (%, °C, %HR, hPa) |
| Gráfica de línea (*Line chart*) | Las mismas | Rango: últimas 24 h. En la gráfica de `riesgo`, líneas de referencia en 0.40 y 0.65 si el widget lo permite |
| Indicador | `nodo_en_linea` | 1 = "En línea", 0 = "Sin conexión" |
| Métrica | `horas_restantes` | Título: "Horas hasta el nivel crítico" |
| Tabla de valores (*Values table*) | `horas_restantes` | Mostrar las columnas del *context* (`recomendacion`, `prioridad`, `confianza`). Si este widget no muestra el *context* en su plan, usar un widget *HTML Canvas* |
| Interruptor (*Switch*) | `silenciar` | Valor encendido = 1, apagado = 0 |

### 5. Notificación por Telegram (prueba N-03)

1. **Data → Events → + Crear evento**.
2. Disparador: variable `estado_alarma` del dispositivo `tanque01`, **igual a 2** (ALERTA), durante 0 minutos.
3. Acción: **Telegram**. Seguir las instrucciones de Ubidots para vincular el bot con el chat o grupo del personal autorizado.
4. Mensaje sugerido: `ALERTA en {{device.name}}: riesgo de desabastecimiento. Revisar el tablero.`
5. Opcional: acción de "vuelta a la normalidad" cuando `estado_alarma` vuelva a 0.

### 6. Acceso restringido (prueba N-05)

- No activar el enlace público del tablero (**Share → Public link** debe quedar apagado).
- Para dar acceso a otra persona, crearle un usuario en la cuenta (si el plan lo permite) en vez de compartir el enlace.

### 7. Límite de datos del plan

El gateway envía unos **8.600 datos por día**: 6 variables × 1.440 minutos, más los cambios de alarma, la conexión y 288 pronósticos. Revisen el límite diario del plan de Ubidots (en la configuración de la cuenta). Si no alcanza, se puede promediar cada 5 minutos: en `gateway.py`, función `ejecutar()`, cambiar el envío de cada minuto por uno cada 5 minutos y el promedio de `promedio_minuto()` a una ventana de 300 s.

### 8. Verificación rápida

1. Con el ESP32 encendido, en 1–2 minutos aparecen datos en las métricas y gráficas.
2. Provocar una ALERTA (escenario C-03c): `estado_alarma` pasa a 2 en menos de 10 s (N-02) y llega el mensaje de Telegram (N-03).
3. Activar el interruptor `silenciar`: el buzzer se apaga en menos de 5 s, el interruptor vuelve a 0 y `estado_alarma` pasa a 3 (N-04).
4. A los 5 min aparece `horas_restantes` con la recomendación de la IA.
5. Tomar capturas de cada paso para Resultados (04) y el video.

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
