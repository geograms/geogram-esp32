#include <stdio.h>
#include <string.h>
#include "wifi_bsp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "wifi_bsp";

static geogram_wifi_status_t s_wifi_status = GEOGRAM_WIFI_STATUS_DISCONNECTED;
static geogram_wifi_event_cb_t s_sta_callback = NULL;
static geogram_wifi_event_cb_t s_ap_callback = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static uint32_t s_current_ip = 0;
static bool s_initialized = false;
static bool s_ap_active = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                s_wifi_status = GEOGRAM_WIFI_STATUS_CONNECTING;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                s_wifi_status = GEOGRAM_WIFI_STATUS_CONNECTED;
                if (s_sta_callback) {
                    s_sta_callback(GEOGRAM_WIFI_STATUS_CONNECTED, event_data);
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected");
                s_wifi_status = GEOGRAM_WIFI_STATUS_DISCONNECTED;
                s_current_ip = 0;
                if (s_sta_callback) {
                    s_sta_callback(GEOGRAM_WIFI_STATUS_DISCONNECTED, event_data);
                }
                break;

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "WiFi AP started");
                s_ap_active = true;
                s_wifi_status = GEOGRAM_WIFI_STATUS_AP_STARTED;
                if (s_ap_callback) {
                    s_ap_callback(GEOGRAM_WIFI_STATUS_AP_STARTED, event_data);
                }
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "WiFi AP stopped");
                s_ap_active = false;
                break;

            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Station connected to AP");
                if (s_ap_callback) {
                    s_ap_callback(GEOGRAM_WIFI_STATUS_AP_STACONNECTED, event_data);
                }
                break;

            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Station disconnected from AP");
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            s_current_ip = event->ip_info.ip.addr;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_wifi_status = GEOGRAM_WIFI_STATUS_GOT_IP;
            if (s_sta_callback) {
                s_sta_callback(GEOGRAM_WIFI_STATUS_GOT_IP, event_data);
            }
        }
    }
}

esp_err_t geogram_wifi_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default WiFi station
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t geogram_wifi_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_initialized = false;
    s_wifi_status = GEOGRAM_WIFI_STATUS_DISCONNECTED;
    s_current_ip = 0;

    return ESP_OK;
}

esp_err_t geogram_wifi_connect(const geogram_wifi_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_sta_callback = config->callback;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", config->ssid);
    return ESP_OK;
}

esp_err_t geogram_wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_wifi_disconnect();
    s_wifi_status = GEOGRAM_WIFI_STATUS_DISCONNECTED;
    s_current_ip = 0;
    return ret;
}

geogram_wifi_status_t geogram_wifi_get_status(void)
{
    return s_wifi_status;
}

esp_err_t geogram_wifi_get_ip(char *ip_str)
{
    if (ip_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_current_ip == 0) {
        ip_str[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    sprintf(ip_str, "%d.%d.%d.%d",
            (uint8_t)(s_current_ip),
            (uint8_t)(s_current_ip >> 8),
            (uint8_t)(s_current_ip >> 16),
            (uint8_t)(s_current_ip >> 24));

    return ESP_OK;
}

esp_err_t geogram_wifi_start_ap(const geogram_wifi_ap_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Create AP netif if not already created
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    s_ap_callback = config->callback;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, config->ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(config->ssid);
    strncpy((char *)wifi_config.ap.password, config->password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.channel = config->channel > 0 ? config->channel : 1;
    wifi_config.ap.max_connection = config->max_connections > 0 ? config->max_connections : 4;
    wifi_config.ap.authmode = strlen(config->password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started - SSID: %s, Channel: %d",
             config->ssid, wifi_config.ap.channel);

    return ESP_OK;
}

esp_err_t geogram_wifi_stop_ap(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_wifi_stop();
    s_ap_active = false;
    s_ap_callback = NULL;

    ESP_LOGI(TAG, "WiFi AP stopped");
    return ESP_OK;
}

bool geogram_wifi_is_ap_active(void)
{
    return s_ap_active;
}

esp_err_t geogram_wifi_get_ap_ip(char *ip_str)
{
    if (ip_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ap_netif == NULL) {
        ip_str[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(s_ap_netif, &ip_info);
    if (ret != ESP_OK) {
        ip_str[0] = '\0';
        return ret;
    }

    sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t geogram_wifi_load_credentials(char *ssid, char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = 33;
    size_t pass_len = 65;

    err = nvs_get_str(nvs, "ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    err = nvs_get_str(nvs, "password", password, &pass_len);
    if (err != ESP_OK) {
        // Password is optional for open networks
        password[0] = '\0';
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded WiFi credentials for SSID: %s", ssid);
    return ESP_OK;
}
