#ifndef GEOGRAM_WS_SERVER_H
#define GEOGRAM_WS_SERVER_H

#include <esp_http_server.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum WebSocket frame size
#define WS_MAX_FRAME_SIZE 1024

// WebSocket message types
typedef enum {
    WS_MSG_HELLO,
    WS_MSG_PING,
    WS_MSG_UNKNOWN
} ws_message_type_t;

// Register WebSocket handler with HTTP server
esp_err_t ws_server_register(httpd_handle_t server);

// Send text message to a specific client
esp_err_t ws_send_text(httpd_handle_t server, int fd, const char *message, size_t len);

// Broadcast text message to all authenticated clients
void ws_broadcast_text(httpd_handle_t server, const char *message, size_t len);

// Parse incoming message type
ws_message_type_t ws_parse_message_type(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // GEOGRAM_WS_SERVER_H
