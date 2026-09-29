# Firmware — Monitor hídrico (Challenge 2)

Firmware del nodo ESP32: mide nivel de agua y variables meteorológicas, calcula el riesgo de desabastecimiento con lógica de fusión, activa la alarma física y sirve un tablero web en la WLAN de la alcaldía.

## Compilar y cargar

1. **Arduino IDE 2** con el core **esp32 by Espressif** (versión 3.x). Placa: *ESP32 Dev Module*.
2. Instalar las librerías desde el gestor de librerías:
   - `ESP Async WebServer` y `Async TCP` (autor **ESP32Async**)
   - `DHT sensor library` y `Adafruit Unified Sensor` (Adafruit)
   - `Adafruit BMP280 Library` (Adafruit)
   - `LiquidCrystal I2C` (Frank de Brabander)
3. Copiar `secrets.h.example` como `secrets.h` y completar la red Wi-Fi y las credenciales del tablero. `secrets.h` está en `.gitignore`.
4. Ajustar `TANQUE_ALTURA_CM` en `config.h` a la altura de la maqueta.
5. Compilar y cargar. En el Monitor Serie (115200 baudios) se ve la IP y una línea CSV por medición.
6. Desde un PC o celular conectado a la misma red, abrir `http://<IP del ESP32>` o `http://monitor-hidrico.local`.

## Módulos

| Archivo | Módulo | Responsabilidad |
|---|---|---|
| `monitor_hidrico.ino` | Principal | Crea mutex, cola y tareas; ISR del timer; `TareaMedicion`, `TareaFusionAlerta`; acciones de silenciar y rearmar |
| `config.h` | Configuración | Pines, periodo, altura del tanque, pesos, umbrales, histéresis, filtro de IPs |
| `tipos.h` | Tipos | `Lectura`, `ResultadoFusion`, `Registro`, `EstadoSistema`, `EstadoAlarma` |
| `recursos.h` | Sincronización | Mutex `mtxI2C` (bus I2C) y `mtxEstado` (estado, histórico y eventos) |
| `sistema.h` | Estado global | Declara `estado`, `diag` y las acciones sobre la alarma |
| `sensores.h/.cpp` | Adquisición | Lee HC-SR04, DHT22, BMP280 y LDR; reusa el último valor válido si un sensor falla |
| `fusion.h/.cpp` | Fusión y alarma | Ventana móvil, índice de evaporación, riesgo, causa probable, máquina de estados. No accede al hardware |
| `actuacion.h/.cpp` | Actuación local | LCD rotativo, semáforo de LEDs, buzzer intermitente |
| `historico.h/.cpp` | Histórico | Buffer circular de 360 mediciones y 20 eventos |
| `servidor.h/.cpp` | Servidor web | Wi-Fi, mDNS, rutas HTTP, SSE, filtro de IP y autenticación Digest |
| `tablero_html.h` | Tablero | Página HTML/CSS/JS en flash, sin dependencias externas |

## Flujo de una medición

```
Timer HW (cada 5 s) → ISR onTimerMedicion() → notifica a TareaMedicion (núcleo 0)
TareaMedicion → lee sensores → cola qLecturas
TareaFusionAlerta (núcleo 1) → fusión → máquina de alarma → histórico → evento SSE
TareaFusionAlerta (cada 250 ms) → LCD, LEDs y buzzer
Servidor web (tarea de AsyncTCP) → responde al tablero; silenciar/rearmar vía mutex
```

## API

| Método | Ruta | Respuesta |
|---|---|---|
| GET | `/` | Tablero |
| GET | `/api/estado` | Última lectura, riesgo, estado de la alarma, causa y estado de los sensores |
| GET | `/api/historico?n=120` | `{"campos":[...],"datos":[[...]]}` con las últimas *n* mediciones |
| GET | `/api/eventos` | Últimos 20 eventos, del más reciente al más antiguo |
| GET | `/api/diag` | Periodo real de medición (mín., máx., último), memoria libre, RSSI, clientes SSE |
| GET | `/events` | SSE: eventos `lectura` (cada medición) y `alerta` (al entrar en ALERTA) |
| POST | `/api/alarma/silenciar` | Apaga el buzzer; 409 si la alarma no está en ALERTA |
| POST | `/api/alarma/rearmar` | Vuelve a ALERTA; 409 si no está SILENCIADA |

Todas las rutas exigen que el cliente esté en la misma subred del ESP32 (403 si no) y credenciales Digest (401 si no).

## Registro de datos para las pruebas

Cada medición imprime por serie:

```
t_ms,nivel_cm,temp_c,hum_pct,presion_hpa,ldr,evaporacion,tasa_cm,riesgo,estado,causa
```

Copie la salida del Monitor Serie a un archivo `.csv` y procésela con `../pruebas/analizar_csv.py`. El script genera la tabla de estadísticas en Markdown, lista las transiciones de estado y, si matplotlib está instalado, produce las gráficas.

## Supuestos sobre el hardware

- **Buzzer activo** (suena con nivel alto). Si es pasivo, hay que reemplazar `digitalWrite` por `tone()` en `actuacion.cpp`.
- LEDs individuales verde, ámbar y rojo en GPIO 26, 33 y 25.
- LCD en 0x27 y BMP280 en 0x76. Si no aparecen, correr un escáner I2C y ajustar `config.h`.
