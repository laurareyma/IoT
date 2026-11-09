// ---------------------------------------------------------------------------
// mqtt_cliente.h — Comunicación MQTT del nodo con el gateway (Challenge 3).
// La publicación corre en su propia tarea FreeRTOS (TareaMQTT), separada del
// hilo principal: un broker lento o caído no retrasa la medición ni la alarma.
// Temas, QoS y formato de los mensajes: wiki, página 02e (sección 4).
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"

// Quién provocó un cambio de estado de la alarma. El gateway lo guarda en la
// tabla `eventos` de SQLite (trazabilidad).
enum OrigenCambio : uint8_t {
  ORIGEN_AUTOMATICO,      // la máquina de alarma, por el riesgo calculado
  ORIGEN_TABLERO_LOCAL,   // botón Silenciar/Rearmar del tablero del ESP32
  ORIGEN_UBIDOTS          // interruptor del tablero global, vía gateway
};

// Crea la cola qMQTT y la tarea TareaMQTT. Se llama una vez en setup().
void mqttIniciar();

// Avisa a TareaMQTT que hay una lectura nueva para publicar.
// Se llama desde TareaFusionAlerta después de cada medición.
void mqttAvisarLectura();

// Deja un cambio de estado de la alarma en la cola qMQTT. No bloquea y se
// puede llamar con mtxEstado tomado. Los cambios esperan en la cola hasta que
// el broker esté disponible, así no se pierde ninguna alerta.
void mqttAvisarCambioAlarma(uint32_t t, EstadoAlarma estado, float riesgo,
                            CausaDescenso causa, OrigenCambio origen);

// true si el nodo está conectado al broker. Seguro desde cualquier tarea.
bool mqttConectado();
