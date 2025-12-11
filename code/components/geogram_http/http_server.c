/**
 * @file http_server.c
 * @brief HTTP server for WiFi configuration and Geogram Station API
 */

#include <stdio.h>
#include <string.h>
#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "station.h"
#include "ws_server.h"

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;
static wifi_config_callback_t s_config_callback = NULL;
static bool s_station_api_enabled = false;

// HTML configuration page
static const char *CONFIG_PAGE_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Geogram WiFi Setup</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:400px;margin:40px auto;padding:20px;background:#f5f5f5;}"
    ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}"
    "h1{color:#333;margin-bottom:20px;font-size:24px;}"
    "label{display:block;margin:15px 0 5px;color:#555;}"
    "input[type=text],input[type=password]{width:100%;padding:12px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:16px;}"
    "input[type=submit]{width:100%;padding:14px;background:#2196F3;color:white;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin-top:20px;}"
    "input[type=submit]:hover{background:#1976D2;}"
    ".status{padding:10px;margin-top:15px;border-radius:4px;text-align:center;}"
    ".success{background:#e8f5e9;color:#2e7d32;}"
    ".error{background:#ffebee;color:#c62828;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>Geogram WiFi Setup</h1>"
    "<form action=\"/connect\" method=\"POST\">"
    "<label for=\"ssid\">WiFi Network Name (SSID)</label>"
    "<input type=\"text\" id=\"ssid\" name=\"ssid\" required maxlength=\"32\">"
    "<label for=\"password\">Password</label>"
    "<input type=\"password\" id=\"password\" name=\"password\" maxlength=\"64\">"
    "<input type=\"submit\" value=\"Connect\">"
    "</form>"
    "</div>"
    "</body>"
    "</html>";

// Success page
static const char *SUCCESS_PAGE_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Geogram - Connected</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:400px;margin:40px auto;padding:20px;background:#f5f5f5;}"
    ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);text-align:center;}"
    "h1{color:#2e7d32;margin-bottom:20px;}"
    "p{color:#555;line-height:1.6;}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>Configuration Saved</h1>"
    "<p>The device will now attempt to connect to the WiFi network.</p>"
    "<p>If successful, the AP will be disabled and you can close this page.</p>"
    "</div>"
    "</body>"
    "</html>";

/**
 * @brief URL decode a string in-place
 */
static void url_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Extract value from form data
 */
static bool extract_form_value(const char *data, const char *key, char *value, size_t value_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "%s=", key);

    const char *start = strstr(data, search_key);
    if (start == NULL) {
        return false;
    }

    start += strlen(search_key);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    if (len >= value_len) {
        len = value_len - 1;
    }

    strncpy(value, start, len);
    value[len] = '\0';
    url_decode(value);

    return true;
}

/**
 * @brief Handler for root page (WiFi config form)
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_PAGE_HTML, strlen(CONFIG_PAGE_HTML));
    return ESP_OK;
}

/**
 * @brief Handler for WiFi configuration POST
 */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char content[256];
    int ret;

    // Read POST data
    int total_len = req->content_len;
    if (total_len >= sizeof(content)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, content, total_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    content[total_len] = '\0';

    ESP_LOGI(TAG, "Received config: %s", content);

    // Extract SSID and password
    char ssid[33] = {0};
    char password[65] = {0};

    if (!extract_form_value(content, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    extract_form_value(content, "password", password, sizeof(password));

    ESP_LOGI(TAG, "WiFi config received - SSID: %s", ssid);

    // Save to NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "password", password);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }

    // Send success page
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SUCCESS_PAGE_HTML, strlen(SUCCESS_PAGE_HTML));

    // Invoke callback
    if (s_config_callback != NULL) {
        s_config_callback(ssid, password);
    }

    return ESP_OK;
}

/**
 * @brief Handler for status endpoint (JSON) - basic
 */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"ok\",\"device\":\"geogram\"}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/**
 * @brief Handler for /api/status endpoint - full station status
 */
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    char response[512];
    size_t len = station_build_status_json(response, sizeof(response));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

static const httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_connect = {
    .uri = "/connect",
    .method = HTTP_POST,
    .handler = connect_post_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_status = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_api_status = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = api_status_get_handler,
    .user_ctx = NULL
};

esp_err_t http_server_start(wifi_config_callback_t callback)
{
    return http_server_start_ex(callback, false);
}

esp_err_t http_server_start_ex(wifi_config_callback_t callback, bool enable_station_api)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    s_config_callback = callback;
    s_station_api_enabled = enable_station_api;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Increase max URI handlers if station API is enabled
    if (enable_station_api) {
        config.max_uri_handlers = 10;
    }

    ESP_LOGI(TAG, "Starting HTTP server on port %d (station_api=%d)", config.server_port, enable_station_api);

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register base URI handlers
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_connect);
    httpd_register_uri_handler(s_server, &uri_status);

    // Register Station API handlers if enabled
    if (enable_station_api) {
        httpd_register_uri_handler(s_server, &uri_api_status);

        // Register WebSocket handler
        ret = ws_server_register(s_server);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register WebSocket handler: %s", esp_err_to_name(ret));
        }

        ESP_LOGI(TAG, "Station API endpoints registered");
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    s_config_callback = NULL;

    ESP_LOGI(TAG, "HTTP server stopped");
    return ret;
}

bool http_server_is_running(void)
{
    return s_server != NULL;
}

httpd_handle_t http_server_get_handle(void)
{
    return s_server;
}
