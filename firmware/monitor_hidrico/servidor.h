// ---------------------------------------------------------------------------
// servidor.h — Wi-Fi (modo estación), servidor web embebido y tablero.
// Rutas: GET / · GET /api/estado · GET /api/historico?n= · GET /api/eventos
//        GET /api/diag · GET /events (SSE) · POST /api/alarma/silenciar
//        POST /api/alarma/rearmar
// Acceso: misma subred (y lista de IPs opcional) + autenticación HTTP Digest.
// ---------------------------------------------------------------------------
#pragma once
#include "tipos.h"

void servidorIniciar();

// Vigila la conexión Wi-Fi y reconecta. Se llama desde loop().
void servidorMantener();

bool        wifiConectado();
const char* wifiIP();

// Escribe el JSON de /api/estado. Si se indica `nodo`, agrega su identificador
// (mensaje MQTT `lectura`). Requiere mtxEstado tomado.
void escribirEstadoJSON(Print& out, const char* nodo = nullptr);

// Publica eventos SSE. Se llaman desde TareaFusionAlerta.
void servidorPublicarLectura();
void servidorPublicarAlerta();
