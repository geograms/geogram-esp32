/**
 * @file mesh_bsp.c
 * @brief Geogram ESP-MESH networking core implementation
 */

#include "mesh_bsp.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "mesh_bsp";

// ============================================================================
// Configuration defaults
// ============================================================================

#ifndef CONFIG_GEOGRAM_MESH_CHANNEL
#define CONFIG_GEOGRAM_MESH_CHANNEL 1
#endif

#ifndef CONFIG_GEOGRAM_MESH_MAX_LAYER
#define CONFIG_GEOGRAM_MESH_MAX_LAYER 6
#endif

#ifndef CONFIG_GEOGRAM_MESH_ROUTE_TABLE_SIZE
#define CONFIG_GEOGRAM_MESH_ROUTE_TABLE_SIZE 50
#endif

#ifndef CONFIG_GEOGRAM_MESH_EXTERNAL_AP_MAX_CONN
#define CONFIG_GEOGRAM_MESH_EXTERNAL_AP_MAX_CONN 4
#endif

// ============================================================================
// State variables
// ============================================================================

static bool s_initialized = false;
static bool s_started = false;
static geogram_mesh_status_t s_status = GEOGRAM_MESH_STATUS_STOPPED;
static geogram_mesh_event_cb_t s_event_callback = NULL;
static geogram_mesh_data_cb_t s_data_callback = NULL;

// Mesh configuration
static mesh_addr_t s_mesh_id;
static uint8_t s_channel = CONFIG_GEOGRAM_MESH_CHANNEL;
static uint8_t s_max_layer = CONFIG_GEOGRAM_MESH_MAX_LAYER;
static bool s_is_root = false;
static uint8_t s_layer = 0;
static uint8_t s_subnet_id = 0;
static mesh_addr_t s_parent_mac;
static bool s_has_parent = false;

// External AP state
static bool s_external_ap_running = false;
static char s_external_ap_ssid[33] = {0};
static uint8_t s_external_ap_clients = 0;
static esp_netif_t *s_external_netif = NULL;

// Route table
static mesh_addr_t s_route_table[CONFIG_GEOGRAM_MESH_ROUTE_TABLE_SIZE];
static int s_route_table_size = 0;

// Network interfaces
static esp_netif_t *s_mesh_sta_netif = NULL;
static esp_netif_t *s_mesh_ap_netif = NULL;

// Receive task
static TaskHandle_t s_rx_task = NULL;
static bool s_rx_task_running = false;

// NVS namespace
#define MESH_NVS_NAMESPACE "mesh_config"

// ============================================================================
// Forward declarations
// ============================================================================

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data);
static void mesh_rx_task(void *arg);
static void update_route_table(void);
static uint8_t calculate_subnet_id(const uint8_t *mac);

// ============================================================================
// Initialization
// ============================================================================

esp_err_t geogram_mesh_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Mesh already initialized");
        return ESP_OK;
    }

    // Suppress verbose internal ESP-MESH network scanning logs
    esp_log_level_set("mesh", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initializing ESP-MESH subsystem");
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret;

    // Initialize NVS (may already be done)
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "[INIT] NVS needs erase, erasing...");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[INIT] Failed to erase NVS: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] Failed to init NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize TCP/IP stack (may already be initialized by wifi_bsp)
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[INIT] Failed to init netif: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "[INIT] TCP/IP stack already initialized");
    }

    // Create default event loop (may already exist)
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[INIT] Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "[INIT] Event loop already exists");
    }

    // Reuse existing STA netif if already created, otherwise create new
    s_mesh_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_mesh_sta_netif) {
        s_mesh_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_LOGI(TAG, "[INIT] Created new STA netif");
    } else {
        ESP_LOGI(TAG, "[INIT] Reusing existing STA netif");
    }

    // Reuse existing AP netif if already created, otherwise create new
    s_mesh_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_mesh_ap_netif) {
        s_mesh_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_LOGI(TAG, "[INIT] Created new AP netif");
    } else {
        ESP_LOGI(TAG, "[INIT] Reusing existing AP netif");
    }

    // Initialize WiFi (may already be initialized)
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "[INIT] Failed to init WiFi: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGI(TAG, "[INIT] WiFi already initialized");
    }

    ret = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[INIT] Failed to set WiFi storage: %s", esp_err_to_name(ret));
        // Non-fatal, continue
    }

    // Start WiFi (may already be started)
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "[INIT] Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ret = esp_event_handler_instance_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[INIT] Failed to register mesh event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[INIT] Failed to register IP event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register WiFi AP event handlers for non-mesh client tracking
    ret = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_ap_event_handler, NULL, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "[INIT] Failed to register AP_STACONNECTED handler: %s", esp_err_to_name(ret));
    }
    ret = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &wifi_ap_event_handler, NULL, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "[INIT] Failed to register AP_STADISCONNECTED handler: %s", esp_err_to_name(ret));
    }

    // Initialize mesh
    ESP_LOGI(TAG, "[INIT] Initializing ESP-MESH stack...");
    ret = esp_mesh_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] Failed to init mesh: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "[INIT] Mesh subsystem initialized successfully");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

esp_err_t geogram_mesh_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_started) {
        geogram_mesh_stop();
    }

    esp_mesh_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_mesh_sta_netif) {
        esp_netif_destroy(s_mesh_sta_netif);
        s_mesh_sta_netif = NULL;
    }
    if (s_mesh_ap_netif) {
        esp_netif_destroy(s_mesh_ap_netif);
        s_mesh_ap_netif = NULL;
    }

    s_initialized = false;
    s_status = GEOGRAM_MESH_STATUS_STOPPED;

    ESP_LOGI(TAG, "Mesh subsystem deinitialized");
    return ESP_OK;
}

// ============================================================================
// Mesh Control
// ============================================================================

esp_err_t geogram_mesh_start(const geogram_mesh_config_t *config)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "[START] ERROR: Mesh not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started) {
        ESP_LOGW(TAG, "[START] Mesh already started");
        return ESP_OK;
    }

    if (!config) {
        ESP_LOGE(TAG, "[START] ERROR: Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[START] Starting ESP-MESH network");
    ESP_LOGI(TAG, "[START] Channel: %d", config->channel);
    ESP_LOGI(TAG, "[START] Max Layer: %d", config->max_layer);
    ESP_LOGI(TAG, "[START] Allow Root: %s", config->allow_root ? "YES" : "NO");
    ESP_LOGI(TAG, "[START] Mesh ID: %02X:%02X:%02X:%02X:%02X:%02X",
             config->mesh_id[0], config->mesh_id[1], config->mesh_id[2],
             config->mesh_id[3], config->mesh_id[4], config->mesh_id[5]);
    ESP_LOGI(TAG, "========================================");

    // Store callback
    s_event_callback = config->callback;

    // Configure mesh
    mesh_cfg_t mesh_cfg = MESH_INIT_CONFIG_DEFAULT();

    // Set mesh ID
    memcpy(&s_mesh_id, config->mesh_id, 6);
    memcpy(&mesh_cfg.mesh_id, config->mesh_id, 6);

    // Set channel
    s_channel = config->channel;
    mesh_cfg.channel = config->channel;

    // ESP-MESH requires a non-empty router SSID in the config. Use a dummy SSID
    // for off-grid self-organized mesh so config validation passes.
    const char *router_ssid = "geogram-mesh";
    size_t router_ssid_len = strlen(router_ssid);
    if (router_ssid_len > sizeof(mesh_cfg.router.ssid)) {
        router_ssid_len = sizeof(mesh_cfg.router.ssid);
    }
    mesh_cfg.router.ssid_len = (uint8_t)router_ssid_len;
    memset(&mesh_cfg.router.ssid, 0, sizeof(mesh_cfg.router.ssid));
    memcpy(mesh_cfg.router.ssid, router_ssid, router_ssid_len);
    memset(&mesh_cfg.router.bssid, 0, sizeof(mesh_cfg.router.bssid));
    ESP_LOGI(TAG, "[START] Router SSID placeholder: %s", router_ssid);

    // Set mesh AP config
    mesh_cfg.mesh_ap.max_connection = CONFIG_GEOGRAM_MESH_EXTERNAL_AP_MAX_CONN;
    mesh_cfg.mesh_ap.nonmesh_max_connection = CONFIG_GEOGRAM_MESH_EXTERNAL_AP_MAX_CONN;
    if (strlen(config->password) > 0) {
        memcpy(mesh_cfg.mesh_ap.password, config->password, strlen(config->password));
    }

    ESP_ERROR_CHECK(esp_mesh_set_config(&mesh_cfg));

    // Set max layer
    s_max_layer = config->max_layer;
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(config->max_layer));

    // Set topology as tree
    ESP_ERROR_CHECK(esp_mesh_set_topology(MESH_TOPO_TREE));

    // Set self-organized mode (no external router)
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));

    // Allow or disallow this node to become root
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(config->allow_root ? 1.0 : 0.0));

    // Calculate this node's subnet ID from MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    s_subnet_id = calculate_subnet_id(mac);
    ESP_LOGI(TAG, "This node's subnet ID: %d (192.168.%d.0/24)", s_subnet_id, 10 + s_subnet_id);

    // Start mesh
    ESP_ERROR_CHECK(esp_mesh_start());

    s_started = true;
    s_status = GEOGRAM_MESH_STATUS_STARTED;

    ESP_LOGI(TAG, "Mesh started, scanning for network...");

    return ESP_OK;
}

esp_err_t geogram_mesh_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping mesh network");

    // Stop external AP if running
    geogram_mesh_stop_external_ap();

    // Stop receive task - signal and wait for clean exit
    if (s_rx_task) {
        ESP_LOGI(TAG, "[STOP] Signaling RX task to stop...");
        s_rx_task_running = false;

        // Wait up to 6 seconds for task to exit (slightly longer than recv timeout)
        // The task will exit on next timeout cycle
        for (int i = 0; i < 60; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            // Check if task has been deleted (eDeleted state)
            eTaskState state = eTaskGetState(s_rx_task);
            if (state == eDeleted) {
                ESP_LOGI(TAG, "[STOP] RX task exited cleanly");
                break;
            }
        }
        s_rx_task = NULL;
    }

    // Stop mesh
    esp_mesh_stop();

    s_started = false;
    s_status = GEOGRAM_MESH_STATUS_STOPPED;
    s_is_root = false;
    s_layer = 0;
    s_has_parent = false;
    s_route_table_size = 0;

    if (s_event_callback) {
        s_event_callback(GEOGRAM_MESH_EVENT_STOPPED, NULL);
    }

    ESP_LOGI(TAG, "Mesh stopped");
    return ESP_OK;
}

// ============================================================================
// Status Queries
// ============================================================================

geogram_mesh_status_t geogram_mesh_get_status(void)
{
    return s_status;
}

bool geogram_mesh_is_connected(void)
{
    return s_status == GEOGRAM_MESH_STATUS_CONNECTED ||
           s_status == GEOGRAM_MESH_STATUS_ROOT;
}

bool geogram_mesh_is_root(void)
{
    return s_is_root;
}

uint8_t geogram_mesh_get_layer(void)
{
    return s_layer;
}

uint8_t geogram_mesh_get_subnet_id(void)
{
    return s_subnet_id;
}

esp_err_t geogram_mesh_get_parent_mac(uint8_t *mac)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!s_has_parent) return ESP_ERR_NOT_FOUND;

    memcpy(mac, s_parent_mac.addr, 6);
    return ESP_OK;
}

// ============================================================================
// External SoftAP
// ============================================================================

esp_err_t geogram_mesh_start_external_ap(const char *ssid, const char *password,
                                          uint8_t max_connections)
{
    if (!s_started) {
        ESP_LOGE(TAG, "Mesh not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_external_ap_running) {
        ESP_LOGW(TAG, "External AP already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting external AP: %s", ssid);

    // Use the existing mesh AP netif (WIFI_AP_DEF) for external clients
    // In ESP-MESH, external clients connect through the mesh AP interface
    if (!s_external_netif) {
        s_external_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!s_external_netif) {
            ESP_LOGE(TAG, "Failed to get mesh AP netif");
            return ESP_FAIL;
        }
    }

    // Use the mesh AP's default subnet (192.168.4.x)
    // Reconfiguring IP would break mesh AP functionality

    ESP_LOGI(TAG, "Configuring mesh AP for external clients (SSID: %s)", ssid);

    // Store SSID for status queries
    strncpy(s_external_ap_ssid, ssid, sizeof(s_external_ap_ssid) - 1);
    s_external_ap_ssid[sizeof(s_external_ap_ssid) - 1] = '\0';

    // Configure the external AP using mesh's mechanism
    mesh_cfg_t mesh_cfg;
    esp_mesh_get_config(&mesh_cfg);
    mesh_cfg.mesh_ap.nonmesh_max_connection = max_connections;
    esp_mesh_set_config(&mesh_cfg);

    // Set AP SSID via WiFi config - must be done AFTER mesh is configured
    // This allows us to have a custom SSID like "geogram" instead of mesh's auto-generated SSID
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = s_channel;
    wifi_config.ap.max_connection = max_connections;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.ssid_hidden = 0;  // Ensure SSID is broadcast
    wifi_config.ap.beacon_interval = 100;
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s (AP may use mesh SSID)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AP SSID set to: %s", ssid);
    }

    // Allow non-mesh stations on the mesh AP
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(false));

    s_external_ap_running = true;
    s_external_ap_clients = 0;

    ESP_LOGI(TAG, "External AP started: %s (192.168.4.1)", ssid);

    return ESP_OK;
}

esp_err_t geogram_mesh_stop_external_ap(void)
{
    if (!s_external_ap_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping external AP");

    // Disable non-mesh connections
    mesh_cfg_t mesh_cfg;
    esp_mesh_get_config(&mesh_cfg);
    mesh_cfg.mesh_ap.nonmesh_max_connection = 0;
    esp_mesh_set_config(&mesh_cfg);

    s_external_ap_running = false;
    s_external_ap_clients = 0;
    s_external_ap_ssid[0] = '\0';

    ESP_LOGI(TAG, "External AP stopped");
    return ESP_OK;
}

bool geogram_mesh_external_ap_is_running(void)
{
    return s_external_ap_running;
}

esp_err_t geogram_mesh_get_external_ap_ip(char *ip_str, size_t len)
{
    if (!ip_str || len < 16) return ESP_ERR_INVALID_ARG;
    if (!s_external_ap_running) return ESP_ERR_INVALID_STATE;

    // Use mesh AP's default subnet
    snprintf(ip_str, len, "192.168.4.1");
    return ESP_OK;
}

esp_err_t geogram_mesh_get_external_ap_ip_addr(uint32_t *ip)
{
    if (!ip) return ESP_ERR_INVALID_ARG;
    if (!s_external_ap_running) return ESP_ERR_INVALID_STATE;

    // Use mesh AP's default subnet
    ip4_addr_t addr;
    IP4_ADDR(&addr, 192, 168, 4, 1);
    *ip = addr.addr;
    return ESP_OK;
}

uint8_t geogram_mesh_get_external_ap_client_count(void)
{
    return s_external_ap_clients;
}

// ============================================================================
// Node Discovery
// ============================================================================

esp_err_t geogram_mesh_get_nodes(geogram_mesh_node_t *nodes, size_t max_nodes,
                                  size_t *node_count)
{
    if (!nodes || !node_count) return ESP_ERR_INVALID_ARG;

    update_route_table();

    size_t count = 0;
    for (int i = 0; i < s_route_table_size && count < max_nodes; i++) {
        memcpy(nodes[count].mac, s_route_table[i].addr, 6);
        nodes[count].subnet_id = calculate_subnet_id(s_route_table[i].addr);
        nodes[count].layer = 0;  // Would need to query each node for accurate layer
        nodes[count].rssi = 0;   // Would need additional API
        nodes[count].is_root = false;  // Would need to compare with root MAC

        count++;
    }

    *node_count = count;
    return ESP_OK;
}

size_t geogram_mesh_get_node_count(void)
{
    update_route_table();
    return (size_t)s_route_table_size;
}

esp_err_t geogram_mesh_find_node_by_subnet(uint8_t subnet_id, geogram_mesh_node_t *node)
{
    if (!node) return ESP_ERR_INVALID_ARG;

    update_route_table();

    for (int i = 0; i < s_route_table_size; i++) {
        if (calculate_subnet_id(s_route_table[i].addr) == subnet_id) {
            memcpy(node->mac, s_route_table[i].addr, 6);
            node->subnet_id = subnet_id;
            node->layer = 0;
            node->rssi = 0;
            node->is_root = false;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

// ============================================================================
// Data Transmission
// ============================================================================

esp_err_t geogram_mesh_send_to_node(const uint8_t *dest_mac, const void *data, size_t len)
{
    if (!dest_mac || !data || len == 0) {
        ESP_LOGE(TAG, "[TX] Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || !geogram_mesh_is_connected()) {
        ESP_LOGE(TAG, "[TX] Mesh not connected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "[TX] Sending %zu bytes to " MACSTR,
             len, MAC2STR(dest_mac));

    mesh_addr_t dest;
    memcpy(dest.addr, dest_mac, 6);

    mesh_data_t mesh_data = {
        .data = (uint8_t *)data,
        .size = (uint16_t)len,
        .proto = MESH_PROTO_BIN,
        .tos = MESH_TOS_P2P
    };

    esp_err_t ret = esp_mesh_send(&dest, &mesh_data, MESH_DATA_P2P, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[TX] FAILED: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[TX] SUCCESS");
    }

    return ret;
}

void geogram_mesh_register_data_callback(geogram_mesh_data_cb_t callback)
{
    s_data_callback = callback;
}

// ============================================================================
// Configuration Persistence
// ============================================================================

esp_err_t geogram_mesh_save_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MESH_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_set_blob(nvs, "mesh_id", &s_mesh_id, 6);
    nvs_set_u8(nvs, "channel", s_channel);
    nvs_set_u8(nvs, "max_layer", s_max_layer);

    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Mesh config saved to NVS");
    return ESP_OK;
}

esp_err_t geogram_mesh_load_config(geogram_mesh_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MESH_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    size_t len = 6;
    nvs_get_blob(nvs, "mesh_id", config->mesh_id, &len);
    nvs_get_u8(nvs, "channel", &config->channel);
    nvs_get_u8(nvs, "max_layer", &config->max_layer);

    nvs_close(nvs);

    ESP_LOGI(TAG, "Mesh config loaded from NVS");
    return ESP_OK;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case MESH_EVENT_STARTED:
            ESP_LOGI(TAG, "[EVENT] *** MESH STARTED ***");
            ESP_LOGI(TAG, "[EVENT] Scanning for mesh network...");
            s_status = GEOGRAM_MESH_STATUS_STARTED;
            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_STARTED, NULL);
            }
            break;

        case MESH_EVENT_STOPPED:
            ESP_LOGI(TAG, "[EVENT] *** MESH STOPPED ***");
            s_status = GEOGRAM_MESH_STATUS_STOPPED;
            s_is_root = false;
            s_layer = 0;
            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_STOPPED, NULL);
            }
            break;

        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
            s_layer = esp_mesh_get_layer();
            s_is_root = esp_mesh_is_root();
            memcpy(&s_parent_mac, &connected->connected.bssid, 6);
            s_has_parent = true;

            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "[EVENT] *** CONNECTED TO MESH ***");
            ESP_LOGI(TAG, "[EVENT] Layer: %d", s_layer);
            ESP_LOGI(TAG, "[EVENT] Is Root: %s", s_is_root ? "YES" : "NO");
            ESP_LOGI(TAG, "[EVENT] Parent MAC: " MACSTR, MAC2STR(s_parent_mac.addr));
            ESP_LOGI(TAG, "[EVENT] Subnet: 192.168.%d.0/24", 10 + s_subnet_id);
            ESP_LOGI(TAG, "========================================");

            s_status = s_is_root ? GEOGRAM_MESH_STATUS_ROOT : GEOGRAM_MESH_STATUS_CONNECTED;

            // Start receive task if not running
            if (!s_rx_task) {
                ESP_LOGI(TAG, "[EVENT] Starting mesh RX task...");
                s_rx_task_running = true;
                // 6144 bytes stack for ESP32-C3 safety (handles 1500-byte packets)
                xTaskCreate(mesh_rx_task, "mesh_rx", 6144, NULL, 5, &s_rx_task);
            }

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_CONNECTED, NULL);
            }
            break;
        }

        case MESH_EVENT_PARENT_DISCONNECTED: {
            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;

            // For standalone root nodes (no router), ignore parent disconnect events
            // Root nodes don't have a parent, so these events are spurious
            if (s_is_root && s_status == GEOGRAM_MESH_STATUS_ROOT) {
                ESP_LOGD(TAG, "[EVENT] Ignoring parent disconnect for standalone root (reason: %d)",
                         disconnected->reason);
                break;
            }

            ESP_LOGW(TAG, "========================================");
            ESP_LOGW(TAG, "[EVENT] *** PARENT DISCONNECTED ***");
            ESP_LOGW(TAG, "[EVENT] Reason: %d", disconnected->reason);
            ESP_LOGW(TAG, "========================================");

            s_status = GEOGRAM_MESH_STATUS_DISCONNECTED;
            s_has_parent = false;
            s_layer = 0;

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_DISCONNECTED, NULL);
            }
            break;
        }

        case MESH_EVENT_CHILD_CONNECTED: {
            mesh_event_child_connected_t *child = (mesh_event_child_connected_t *)event_data;
            ESP_LOGI(TAG, "[EVENT] Child node connected: " MACSTR, MAC2STR(child->mac));

            update_route_table();
            ESP_LOGI(TAG, "[EVENT] Route table now has %d nodes", s_route_table_size);

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_CHILD_CONNECTED, child);
            }
            break;
        }

        case MESH_EVENT_CHILD_DISCONNECTED: {
            mesh_event_child_disconnected_t *child = (mesh_event_child_disconnected_t *)event_data;
            ESP_LOGW(TAG, "[EVENT] Child node disconnected: " MACSTR, MAC2STR(child->mac));

            update_route_table();
            ESP_LOGI(TAG, "[EVENT] Route table now has %d nodes", s_route_table_size);

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_CHILD_DISCONNECTED, child);
            }
            break;
        }

        case MESH_EVENT_ROOT_SWITCH_REQ:
            ESP_LOGI(TAG, "[EVENT] Root switch requested");
            break;

        case MESH_EVENT_ROOT_SWITCH_ACK:
            s_is_root = esp_mesh_is_root();
            s_layer = esp_mesh_get_layer();
            s_status = s_is_root ? GEOGRAM_MESH_STATUS_ROOT : GEOGRAM_MESH_STATUS_CONNECTED;

            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "[EVENT] *** ROOT STATUS CHANGED ***");
            ESP_LOGI(TAG, "[EVENT] Is Root: %s", s_is_root ? "YES" : "NO");
            ESP_LOGI(TAG, "[EVENT] Layer: %d", s_layer);
            ESP_LOGI(TAG, "========================================");

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_ROOT_CHANGED, NULL);
            }
            break;

        // MESH_EVENT_ROOT_GOT_IP and MESH_EVENT_ROOT_LOST_IP were removed in ESP-IDF v5
        // IP events for the mesh root are handled via IP_EVENT_STA_GOT_IP

        case MESH_EVENT_ROUTING_TABLE_ADD:
            ESP_LOGD(TAG, "[EVENT] Routing table: node added");
            update_route_table();
            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_ROUTE_TABLE_CHANGE, NULL);
            }
            break;

        case MESH_EVENT_ROUTING_TABLE_REMOVE:
            ESP_LOGD(TAG, "[EVENT] Routing table: node removed");
            update_route_table();
            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_ROUTE_TABLE_CHANGE, NULL);
            }
            break;

        case MESH_EVENT_NO_PARENT_FOUND: {
            ESP_LOGI(TAG, "[EVENT] No parent found - becoming root");
            // Force this node to become root since no network exists
            esp_err_t err = esp_mesh_set_type(MESH_ROOT);
            if (err == ESP_OK) {
                // Root has no parent, so PARENT_CONNECTED won't fire
                // Manually set state and trigger callback
                s_is_root = true;
                s_layer = 1;
                s_status = GEOGRAM_MESH_STATUS_ROOT;
                ESP_LOGI(TAG, "[EVENT] Now operating as ROOT node");

                // Disable self-organization to stop mesh from reconfiguring the AP
                // This allows our custom SSID and settings to persist
                esp_mesh_set_self_organized(false, false);
                ESP_LOGI(TAG, "[EVENT] Disabled mesh self-organization for stable AP");

                if (s_event_callback) {
                    s_event_callback(GEOGRAM_MESH_EVENT_CONNECTED, NULL);
                }
            } else {
                ESP_LOGE(TAG, "Failed to set as root: %s", esp_err_to_name(err));
            }
            break;
        }

        case MESH_EVENT_LAYER_CHANGE:
            s_layer = esp_mesh_get_layer();
            ESP_LOGI(TAG, "[EVENT] Layer changed to %d", s_layer);
            break;

        case MESH_EVENT_SCAN_DONE:
            ESP_LOGD(TAG, "[EVENT] Network scan completed");
            break;

        default:
            ESP_LOGD(TAG, "[EVENT] Unhandled mesh event: %ld", event_id);
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    switch (event_id) {
        case IP_EVENT_AP_STAIPASSIGNED: {
            ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
            ESP_LOGI(TAG, "Phone connected to external AP, IP: " IPSTR,
                     IP2STR(&event->ip));
            s_external_ap_clients++;

            if (s_event_callback) {
                geogram_mesh_external_sta_t sta = {0};
                memcpy(sta.mac, event->mac, 6);
                sta.ip = event->ip.addr;
                s_event_callback(GEOGRAM_MESH_EVENT_EXTERNAL_STA_CONNECTED, &sta);
            }
            break;
        }

        case IP_EVENT_STA_LOST_IP:
            ESP_LOGW(TAG, "Lost STA IP");
            break;

        default:
            break;
    }
}

static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "[AP] Station connected: " MACSTR " (AID=%d)",
                     MAC2STR(event->mac), event->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "[AP] Station disconnected: " MACSTR " (AID=%d, reason=%d)",
                     MAC2STR(event->mac), event->aid, event->reason);
            if (s_external_ap_clients > 0) {
                s_external_ap_clients--;
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Receive Task
// ============================================================================

static void mesh_rx_task(void *arg)
{
    ESP_LOGI(TAG, "[RX] Mesh receive task started");
    ESP_LOGI(TAG, "[RX] Waiting for incoming mesh data...");

    mesh_addr_t from;
    mesh_data_t data;
    int flag = 0;

    // Allocate receive buffer
    uint8_t *rx_buf = malloc(1500);
    if (!rx_buf) {
        ESP_LOGE(TAG, "[RX] Failed to allocate RX buffer");
        s_rx_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t rx_count = 0;

    while (s_rx_task_running) {
        data.data = rx_buf;
        data.size = 1500;

        // Use 5 second timeout instead of portMAX_DELAY to prevent watchdog issues
        esp_err_t ret = esp_mesh_recv(&from, &data, pdMS_TO_TICKS(5000), &flag, NULL, 0);
        if (ret == ESP_ERR_MESH_TIMEOUT) {
            // Normal timeout, just continue to check s_rx_task_running flag
            continue;
        }
        if (ret == ESP_OK && data.size > 0) {
            rx_count++;
            ESP_LOGI(TAG, "[RX] ========================================");
            ESP_LOGI(TAG, "[RX] Packet #%lu received", (unsigned long)rx_count);
            ESP_LOGI(TAG, "[RX] From: " MACSTR, MAC2STR(from.addr));
            ESP_LOGI(TAG, "[RX] Size: %d bytes", data.size);
            ESP_LOGI(TAG, "[RX] Flag: 0x%02x", flag);

            if (s_data_callback) {
                ESP_LOGD(TAG, "[RX] Invoking data callback...");
                s_data_callback(from.addr, data.data, data.size);
            } else {
                ESP_LOGW(TAG, "[RX] No data callback registered!");
            }
            ESP_LOGI(TAG, "[RX] ========================================");
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[RX] Receive error: %s", esp_err_to_name(ret));
        }
    }

    free(rx_buf);
    ESP_LOGI(TAG, "[RX] Mesh receive task stopped (total packets: %lu)", (unsigned long)rx_count);
    vTaskDelete(NULL);
}

// ============================================================================
// Helper Functions
// ============================================================================

static void update_route_table(void)
{
    s_route_table_size = esp_mesh_get_routing_table(
        (mesh_addr_t *)s_route_table,
        CONFIG_GEOGRAM_MESH_ROUTE_TABLE_SIZE * 6,
        &s_route_table_size);

    ESP_LOGD(TAG, "Route table updated: %d nodes", s_route_table_size);
}

static uint8_t calculate_subnet_id(const uint8_t *mac)
{
    // Use last byte of MAC, mapped to range 0-239
    // This gives subnet 192.168.{10+id}.0/24
    return mac[5] % 240;
}
