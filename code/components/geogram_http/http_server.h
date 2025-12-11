/**
 * @file http_server.h
 * @brief HTTP server for WiFi configuration
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi credentials received callback
 *
 * @param ssid SSID to connect to
 * @param password WiFi password
 */
typedef void (*wifi_config_callback_t)(const char *ssid, const char *password);

/**
 * @brief Start the HTTP configuration server
 *
 * Starts an HTTP server that serves a WiFi configuration page.
 *
 * @param callback Callback to invoke when WiFi credentials are submitted
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_start(wifi_config_callback_t callback);

/**
 * @brief Stop the HTTP configuration server
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_stop(void);

/**
 * @brief Check if HTTP server is running
 *
 * @return true if server is running
 */
bool http_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
