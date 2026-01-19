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

static const char *TAG = "mesh";

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

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initializing ESP-MESH subsystem");
    ESP_LOGI(TAG, "========================================");

    // Initialize NVS (may already be done)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create network interfaces for mesh
    s_mesh_sta_netif = esp_netif_create_default_wifi_sta();
    s_mesh_ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL, NULL));

    // Initialize mesh
    ESP_LOGI(TAG, "[INIT] Initializing ESP-MESH stack...");
    ESP_ERROR_CHECK(esp_mesh_init());

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

    // Set router (none for self-organized mesh without external router)
    mesh_cfg.router.ssid_len = 0;
    memset(&mesh_cfg.router.ssid, 0, sizeof(mesh_cfg.router.ssid));
    memset(&mesh_cfg.router.bssid, 0, sizeof(mesh_cfg.router.bssid));

    // Set mesh AP config
    mesh_cfg.mesh_ap.max_connection = CONFIG_GEOGRAM_MESH_EXTERNAL_AP_MAX_CONN;
    mesh_cfg.mesh_ap.nonmesh_max_connection = 0;  // Will configure later
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

    // Stop receive task
    if (s_rx_task) {
        s_rx_task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));
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

    // Create a separate netif for the external AP if not already created
    if (!s_external_netif) {
        esp_netif_inherent_config_t netif_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
        netif_cfg.route_prio = 10;
        esp_netif_config_t cfg = {
            .base = &netif_cfg,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP,
        };
        s_external_netif = esp_netif_new(&cfg);
    }

    // Configure subnet based on this node's subnet ID
    // 192.168.{10+subnet_id}.1 for AP gateway
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 10 + s_subnet_id, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 10 + s_subnet_id, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(s_external_netif);
    esp_netif_set_ip_info(s_external_netif, &ip_info);
    esp_netif_dhcps_start(s_external_netif);

    // Store SSID for status queries
    strncpy(s_external_ap_ssid, ssid, sizeof(s_external_ap_ssid) - 1);
    s_external_ap_ssid[sizeof(s_external_ap_ssid) - 1] = '\0';

    // Configure the external AP using mesh's mechanism
    mesh_cfg_t mesh_cfg;
    esp_mesh_get_config(&mesh_cfg);
    mesh_cfg.mesh_ap.nonmesh_max_connection = max_connections;
    esp_mesh_set_config(&mesh_cfg);

    // Allow non-mesh stations on the mesh AP
    ESP_ERROR_CHECK(esp_mesh_allow_root_conflicts(false));

    s_external_ap_running = true;
    s_external_ap_clients = 0;

    ESP_LOGI(TAG, "External AP started: %s (192.168.%d.1)", ssid, 10 + s_subnet_id);

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

    snprintf(ip_str, len, "192.168.%d.1", 10 + s_subnet_id);
    return ESP_OK;
}

esp_err_t geogram_mesh_get_external_ap_ip_addr(uint32_t *ip)
{
    if (!ip) return ESP_ERR_INVALID_ARG;
    if (!s_external_ap_running) return ESP_ERR_INVALID_STATE;

    ip4_addr_t addr;
    IP4_ADDR(&addr, 192, 168, 10 + s_subnet_id, 1);
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
                xTaskCreate(mesh_rx_task, "mesh_rx", 4096, NULL, 5, &s_rx_task);
            }

            if (s_event_callback) {
                s_event_callback(GEOGRAM_MESH_EVENT_CONNECTED, NULL);
            }
            break;
        }

        case MESH_EVENT_PARENT_DISCONNECTED: {
            mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
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

        case MESH_EVENT_NO_PARENT_FOUND:
            ESP_LOGI(TAG, "[EVENT] No parent found - will become root if allowed");
            break;

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

        esp_err_t ret = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
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
