/**
 * @file ws_server.c
 * @brief WebSocket server stub (requires CONFIG_HTTPD_WS_SUPPORT=y)
 *
 * This is a placeholder implementation. WebSocket support requires
 * CONFIG_HTTPD_WS_SUPPORT=y in sdkconfig, which needs manual configuration
 * via idf.py menuconfig or sdkconfig editing.
 *
 * For now, this provides stub functions that do nothing.
 * The Station API works via HTTP polling (/api/status).
 */

#include "ws_server.h"
#include "station.h"
#include <esp_log.h>

static const char *TAG = "WS";

esp_err_t ws_server_register(httpd_handle_t server) {
    // WebSocket support requires CONFIG_HTTPD_WS_SUPPORT=y
    // For now, log a message and skip registration
    ESP_LOGW(TAG, "WebSocket support not enabled in sdkconfig");
    ESP_LOGW(TAG, "Use HTTP polling via /api/status instead");
    ESP_LOGW(TAG, "To enable: set CONFIG_HTTPD_WS_SUPPORT=y in sdkconfig");
    return ESP_OK;
}

esp_err_t ws_send_text(httpd_handle_t server, int fd, const char *message, size_t len) {
    // Stub - no WebSocket support
    return ESP_ERR_NOT_SUPPORTED;
}

void ws_broadcast_text(httpd_handle_t server, const char *message, size_t len) {
    // Stub - no WebSocket support
}

ws_message_type_t ws_parse_message_type(const char *data, size_t len) {
    return WS_MSG_UNKNOWN;
}
