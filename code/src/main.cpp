#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_config.h"

// Station API
#include "station.h"

// Include board-specific model initialization
#if BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54
    #include "model_config.h"
    #include "model_init.h"
    #include "board_power.h"
    #include "epaper_1in54.h"
    #include "shtc3.h"
    #include "pcf85063.h"
    #include "lvgl_port.h"
    #include "geogram_ui.h"
    #include "wifi_bsp.h"
    #include "http_server.h"
#elif BOARD_MODEL == MODEL_ESP32_GENERIC
    #include "model_config.h"
    #include "model_init.h"
    #include "wifi_bsp.h"
    #include "http_server.h"
#else
    #error "Invalid BOARD_MODEL defined!"
#endif

static const char *TAG = "geogram";

#if BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54

// Sensor update interval (ms)
#define SENSOR_UPDATE_INTERVAL  30000
#define DISPLAY_REFRESH_INTERVAL 60000

// WiFi configuration
#define WIFI_AP_SSID        "Geogram-Setup"
#define WIFI_AP_PASSWORD    ""  // Open network for easy setup
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    4

static bool s_wifi_connected = false;
static char s_current_ip[16] = {0};

/**
 * @brief WiFi event callback
 */
static void wifi_event_cb(geogram_wifi_status_t status, void *event_data)
{
    switch (status) {
        case GEOGRAM_WIFI_STATUS_GOT_IP:
            ESP_LOGI(TAG, "WiFi connected with IP");
            s_wifi_connected = true;
            geogram_wifi_get_ip(s_current_ip);
            geogram_ui_update_wifi(UI_WIFI_STATUS_CONNECTED, s_current_ip, NULL);
            geogram_ui_show_status("WiFi Connected");
            geogram_ui_refresh(false);

            // Stop AP mode HTTP server and start Station API server
            http_server_stop();

            // Initialize and start Station API
            station_init();
            http_server_start_ex(NULL, true);  // Station API enabled
            ESP_LOGI(TAG, "Station API started - callsign: %s", station_get_callsign());
            break;

        case GEOGRAM_WIFI_STATUS_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            s_wifi_connected = false;
            s_current_ip[0] = '\0';
            geogram_ui_update_wifi(UI_WIFI_STATUS_DISCONNECTED, NULL, NULL);
            geogram_ui_show_status("WiFi Disconnected");
            geogram_ui_refresh(false);
            break;

        case GEOGRAM_WIFI_STATUS_AP_STARTED:
            ESP_LOGI(TAG, "AP mode started");
            geogram_wifi_get_ap_ip(s_current_ip);
            geogram_ui_update_wifi(UI_WIFI_STATUS_AP_MODE, s_current_ip, NULL);
            geogram_ui_show_status("Setup Mode");
            geogram_ui_refresh(false);
            break;

        default:
            break;
    }
}

/**
 * @brief Callback when WiFi credentials are submitted via HTTP
 */
static void wifi_config_received(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "WiFi credentials received for SSID: %s", ssid);

    geogram_ui_show_status("Connecting...");
    geogram_ui_refresh(false);

    // Stop AP mode
    geogram_wifi_stop_ap();

    // Connect to the configured network
    geogram_wifi_config_t config = {};
    strncpy(config.ssid, ssid, sizeof(config.ssid) - 1);
    strncpy(config.password, password, sizeof(config.password) - 1);
    config.callback = wifi_event_cb;

    geogram_wifi_connect(&config);
}

/**
 * @brief Start WiFi in AP mode for configuration
 */
static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode for WiFi configuration");

    geogram_wifi_ap_config_t ap_config = {};
    strncpy(ap_config.ssid, WIFI_AP_SSID, sizeof(ap_config.ssid) - 1);
    strncpy(ap_config.password, WIFI_AP_PASSWORD, sizeof(ap_config.password) - 1);
    ap_config.channel = WIFI_AP_CHANNEL;
    ap_config.max_connections = WIFI_AP_MAX_CONN;
    ap_config.callback = wifi_event_cb;

    geogram_wifi_start_ap(&ap_config);

    // Start HTTP server for configuration
    http_server_start(wifi_config_received);
}

/**
 * @brief Try to connect with saved credentials
 */
static bool try_saved_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    if (geogram_wifi_load_credentials(ssid, password) == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "Found saved credentials for SSID: %s", ssid);

        geogram_ui_show_status("Connecting...");
        geogram_ui_update_wifi(UI_WIFI_STATUS_CONNECTING, NULL, ssid);
        geogram_ui_refresh(false);

        geogram_wifi_config_t config = {};
        strncpy(config.ssid, ssid, sizeof(config.ssid) - 1);
        strncpy(config.password, password, sizeof(config.password) - 1);
        config.callback = wifi_event_cb;

        geogram_wifi_connect(&config);
        return true;
    }

    return false;
}

/**
 * @brief Sensor reading task
 */
static void sensor_task(void *pvParameter)
{
    shtc3_handle_t sensor = (shtc3_handle_t)pvParameter;
    shtc3_data_t data;
    uint32_t refresh_counter = 0;

    while (1) {
        if (shtc3_read(sensor, &data) == ESP_OK) {
            ESP_LOGI(TAG, "Temp: %.1f C, Humidity: %.1f %%",
                     data.temperature, data.humidity);
            geogram_ui_update_sensor(data.temperature, data.humidity);
        } else {
            ESP_LOGW(TAG, "Failed to read sensor");
        }

        // Refresh display periodically
        refresh_counter += SENSOR_UPDATE_INTERVAL;
        if (refresh_counter >= DISPLAY_REFRESH_INTERVAL) {
            geogram_ui_refresh(false);
            refresh_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_UPDATE_INTERVAL));
    }
}

/**
 * @brief RTC update task
 */
static void rtc_task(void *pvParameter)
{
    pcf85063_handle_t rtc = (pcf85063_handle_t)pvParameter;
    pcf85063_datetime_t datetime;
    uint8_t last_minute = 255;

    while (1) {
        if (pcf85063_get_datetime(rtc, &datetime) == ESP_OK) {
            // Update time display only when minute changes
            if (datetime.minute != last_minute) {
                geogram_ui_update_time(datetime.hour, datetime.minute);
                geogram_ui_update_date(datetime.year, datetime.month, datetime.day);
                last_minute = datetime.minute;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // Check every second
    }
}

#endif  // BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "  Geogram Firmware v%s", GEOGRAM_VERSION);
    ESP_LOGI(TAG, "  Board: %s", BOARD_NAME);
    ESP_LOGI(TAG, "  Model: %s", MODEL_NAME);
    ESP_LOGI(TAG, "=====================================");

    // Initialize board-specific hardware
    esp_err_t ret = model_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board initialization failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Board initialized successfully");

#if BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54
    // Get hardware handles
    epaper_1in54_handle_t display = model_get_display();
    shtc3_handle_t env_sensor = model_get_env_sensor();
    pcf85063_handle_t rtc = model_get_rtc();

    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to get display handle");
        return;
    }

    ESP_LOGI(TAG, "E-paper display: %dx%d",
             epaper_1in54_get_width(display),
             epaper_1in54_get_height(display));

    // Initialize LVGL with e-paper display
    ret = lvgl_port_init(display);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize UI
    ret = geogram_ui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UI: %s", esp_err_to_name(ret));
        return;
    }

    // Initial display refresh
    geogram_ui_show_status("Starting...");
    geogram_ui_refresh(true);  // Full refresh on startup

    // Initialize WiFi
    ret = geogram_wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(ret));
        geogram_ui_show_status("WiFi Init Failed");
        geogram_ui_refresh(false);
    } else {
        // Try to connect with saved credentials, otherwise start AP mode
        if (!try_saved_credentials()) {
            start_ap_mode();
        }
    }

    // Start sensor reading task
    if (env_sensor != NULL) {
        xTaskCreate(sensor_task, "sensor_task", 4096, env_sensor, 5, NULL);
    }

    // Start RTC update task
    if (rtc != NULL) {
        xTaskCreate(rtc_task, "rtc_task", 2048, rtc, 4, NULL);
    }

#endif  // BOARD_MODEL == MODEL_ESP32S3_EPAPER_1IN54

    // Main loop
    ESP_LOGI(TAG, "Entering main loop...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
