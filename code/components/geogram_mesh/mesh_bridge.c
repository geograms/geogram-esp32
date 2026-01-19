/**
 * @file mesh_bridge.c
 * @brief IP packet bridging over ESP-MESH
 *
 * Captures IP packets from the external SoftAP interface and forwards them
 * to the appropriate mesh node based on destination subnet. Received packets
 * from other mesh nodes are injected into the local network.
 */

#include "mesh_bsp.h"
#include "mesh_chat.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"

static const char *TAG = "mesh_bridge";

// ============================================================================
// Configuration
// ============================================================================

#ifndef CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE
#define CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE 1500
#endif

#ifndef CONFIG_GEOGRAM_MESH_BRIDGE_QUEUE_SIZE
#define CONFIG_GEOGRAM_MESH_BRIDGE_QUEUE_SIZE 8
#endif

// Bridge packet header
#define BRIDGE_MAGIC 0x47454F  // "GEO" in hex
#define BRIDGE_VERSION 1

// ============================================================================
// Data Structures
// ============================================================================

/**
 * @brief Bridge packet header (prepended to IP packets)
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;           // Magic number (BRIDGE_MAGIC)
    uint8_t version;          // Protocol version
    uint8_t src_subnet;       // Source subnet ID
    uint8_t dest_subnet;      // Destination subnet ID
    uint8_t reserved;         // Reserved for future use
    uint16_t payload_len;     // IP packet length
    uint16_t checksum;        // Simple checksum
} bridge_header_t;

/**
 * @brief Queued packet for bridging
 */
typedef struct {
    uint8_t dest_mac[6];
    uint8_t *data;
    size_t len;
} bridge_packet_t;

// ============================================================================
// State Variables
// ============================================================================

static bool s_bridge_enabled = false;
static TaskHandle_t s_bridge_task = NULL;
static QueueHandle_t s_tx_queue = NULL;

// Statistics
static uint32_t s_packets_tx = 0;
static uint32_t s_packets_rx = 0;
static uint32_t s_bytes_tx = 0;
static uint32_t s_bytes_rx = 0;

// Buffer for outgoing packets
static uint8_t s_tx_buffer[CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE + sizeof(bridge_header_t)];

// ============================================================================
// Forward Declarations
// ============================================================================

static void bridge_task(void *arg);
static void mesh_data_handler(const uint8_t *src_mac, const void *data, size_t len);
static uint8_t get_subnet_from_ip(uint32_t ip);
static uint16_t calculate_checksum(const uint8_t *data, size_t len);

// ============================================================================
// Public API
// ============================================================================

esp_err_t geogram_mesh_enable_bridge(void)
{
    if (s_bridge_enabled) {
        ESP_LOGW(TAG, "[BRIDGE] Already enabled");
        return ESP_OK;
    }

    if (!geogram_mesh_is_connected()) {
        ESP_LOGE(TAG, "[BRIDGE] Cannot enable: mesh not connected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "[BRIDGE] Enabling IP packet bridging");
    ESP_LOGI(TAG, "[BRIDGE] Buffer size: %d bytes", CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE);
    ESP_LOGI(TAG, "[BRIDGE] Queue size: %d packets", CONFIG_GEOGRAM_MESH_BRIDGE_QUEUE_SIZE);
    ESP_LOGI(TAG, "========================================");

    // Create TX queue
    s_tx_queue = xQueueCreate(CONFIG_GEOGRAM_MESH_BRIDGE_QUEUE_SIZE, sizeof(bridge_packet_t));
    if (!s_tx_queue) {
        ESP_LOGE(TAG, "Failed to create TX queue");
        return ESP_ERR_NO_MEM;
    }

    // Register for incoming mesh data
    geogram_mesh_register_data_callback(mesh_data_handler);

    // Start bridge task
    BaseType_t ret = xTaskCreate(bridge_task, "mesh_bridge", 4096, NULL, 4, &s_bridge_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create bridge task");
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_bridge_enabled = true;
    s_packets_tx = 0;
    s_packets_rx = 0;
    s_bytes_tx = 0;
    s_bytes_rx = 0;

    ESP_LOGI(TAG, "[BRIDGE] IP bridging enabled successfully");
    ESP_LOGI(TAG, "[BRIDGE] Ready to forward packets between mesh nodes");
    return ESP_OK;
}

esp_err_t geogram_mesh_disable_bridge(void)
{
    if (!s_bridge_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disabling IP bridge");

    // Unregister data callback
    geogram_mesh_register_data_callback(NULL);

    // Stop bridge task
    if (s_bridge_task) {
        vTaskDelete(s_bridge_task);
        s_bridge_task = NULL;
    }

    // Clean up TX queue
    if (s_tx_queue) {
        bridge_packet_t pkt;
        while (xQueueReceive(s_tx_queue, &pkt, 0) == pdTRUE) {
            if (pkt.data) {
                free(pkt.data);
            }
        }
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
    }

    s_bridge_enabled = false;

    ESP_LOGI(TAG, "IP bridge disabled");
    return ESP_OK;
}

bool geogram_mesh_bridge_is_enabled(void)
{
    return s_bridge_enabled;
}

void geogram_mesh_bridge_get_stats(uint32_t *packets_tx, uint32_t *packets_rx,
                                    uint32_t *bytes_tx, uint32_t *bytes_rx)
{
    if (packets_tx) *packets_tx = s_packets_tx;
    if (packets_rx) *packets_rx = s_packets_rx;
    if (bytes_tx) *bytes_tx = s_bytes_tx;
    if (bytes_rx) *bytes_rx = s_bytes_rx;
}

// ============================================================================
// Bridge Task
// ============================================================================

/**
 * @brief Bridge task - handles outgoing packet transmission
 */
static void bridge_task(void *arg)
{
    ESP_LOGI(TAG, "Bridge task started");

    bridge_packet_t pkt;

    while (s_bridge_enabled) {
        if (xQueueReceive(s_tx_queue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (pkt.data && pkt.len > 0) {
                ESP_LOGD(TAG, "Forwarding %zu bytes to " MACSTR,
                         pkt.len, MAC2STR(pkt.dest_mac));

                esp_err_t ret = geogram_mesh_send_to_node(pkt.dest_mac, pkt.data, pkt.len);
                if (ret == ESP_OK) {
                    s_packets_tx++;
                    s_bytes_tx += pkt.len;
                } else {
                    ESP_LOGW(TAG, "Failed to forward packet: %s", esp_err_to_name(ret));
                }

                free(pkt.data);
            }
        }
    }

    ESP_LOGI(TAG, "Bridge task stopped");
    vTaskDelete(NULL);
}

// ============================================================================
// Packet Forwarding
// ============================================================================

/**
 * @brief Queue an IP packet for forwarding to another mesh node
 *
 * @param dest_ip Destination IP address
 * @param ip_packet IP packet data
 * @param ip_len IP packet length
 * @return ESP_OK if queued successfully
 */
esp_err_t mesh_bridge_forward_packet(uint32_t dest_ip, const uint8_t *ip_packet, size_t ip_len)
{
    if (!s_bridge_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ip_packet || ip_len == 0 || ip_len > CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    // Extract destination subnet from IP
    uint8_t dest_subnet = get_subnet_from_ip(dest_ip);
    uint8_t my_subnet = geogram_mesh_get_subnet_id();

    // If destination is on our subnet, no need to bridge
    if (dest_subnet == my_subnet) {
        ESP_LOGD(TAG, "Packet destination on local subnet, not bridging");
        return ESP_OK;
    }

    // Find the mesh node that owns the destination subnet
    geogram_mesh_node_t dest_node;
    esp_err_t ret = geogram_mesh_find_node_by_subnet(dest_subnet, &dest_node);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No mesh node found for subnet %d", dest_subnet);
        return ESP_ERR_NOT_FOUND;
    }

    // Build bridge packet
    size_t total_len = sizeof(bridge_header_t) + ip_len;
    uint8_t *packet = malloc(total_len);
    if (!packet) {
        return ESP_ERR_NO_MEM;
    }

    bridge_header_t *header = (bridge_header_t *)packet;
    header->magic = BRIDGE_MAGIC;
    header->version = BRIDGE_VERSION;
    header->src_subnet = my_subnet;
    header->dest_subnet = dest_subnet;
    header->reserved = 0;
    header->payload_len = (uint16_t)ip_len;
    header->checksum = calculate_checksum(ip_packet, ip_len);

    memcpy(packet + sizeof(bridge_header_t), ip_packet, ip_len);

    // Queue for transmission
    bridge_packet_t pkt = {
        .data = packet,
        .len = total_len
    };
    memcpy(pkt.dest_mac, dest_node.mac, 6);

    if (xQueueSend(s_tx_queue, &pkt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropping packet");
        free(packet);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "Queued packet for subnet %d (%zu bytes)", dest_subnet, ip_len);
    return ESP_OK;
}

// ============================================================================
// Incoming Data Handler
// ============================================================================

/**
 * @brief Handle incoming mesh data (bridged packets from other nodes)
 */
static void mesh_data_handler(const uint8_t *src_mac, const void *data, size_t len)
{
    ESP_LOGD(TAG, "[BRIDGE RX] Received %zu bytes from " MACSTR,
             len, MAC2STR(src_mac));

    // First, try to handle as chat message (will return silently if not chat)
    mesh_chat_handle_packet(src_mac, data, len);

    if (len < sizeof(bridge_header_t)) {
        ESP_LOGD(TAG, "[BRIDGE RX] Packet too small for bridge (%zu bytes)", len);
        return;
    }

    const bridge_header_t *header = (const bridge_header_t *)data;

    // Validate magic number
    if (header->magic != BRIDGE_MAGIC) {
        ESP_LOGD(TAG, "[BRIDGE RX] Not a bridge packet (magic: 0x%08lx)", (unsigned long)header->magic);
        return;  // Not a bridge packet, was likely chat message already handled
    }

    ESP_LOGI(TAG, "[BRIDGE RX] ========================================");
    ESP_LOGI(TAG, "[BRIDGE RX] Bridge packet received");
    ESP_LOGI(TAG, "[BRIDGE RX] From: " MACSTR, MAC2STR(src_mac));
    ESP_LOGI(TAG, "[BRIDGE RX] Source subnet: %d (192.168.%d.x)", header->src_subnet, 10 + header->src_subnet);
    ESP_LOGI(TAG, "[BRIDGE RX] Dest subnet: %d (192.168.%d.x)", header->dest_subnet, 10 + header->dest_subnet);
    ESP_LOGI(TAG, "[BRIDGE RX] Payload: %d bytes", header->payload_len);

    // Validate version
    if (header->version != BRIDGE_VERSION) {
        ESP_LOGW(TAG, "[BRIDGE RX] Unsupported bridge version: %d", header->version);
        return;
    }

    // Validate payload length
    if (len < sizeof(bridge_header_t) + header->payload_len) {
        ESP_LOGW(TAG, "[BRIDGE RX] Payload length mismatch");
        return;
    }

    // Check if this packet is for us
    uint8_t my_subnet = geogram_mesh_get_subnet_id();
    if (header->dest_subnet != my_subnet) {
        ESP_LOGW(TAG, "[BRIDGE RX] Not for us (dest=%d, ours=%d)",
                 header->dest_subnet, my_subnet);
        return;
    }

    // Verify checksum
    const uint8_t *ip_packet = (const uint8_t *)data + sizeof(bridge_header_t);
    uint16_t checksum = calculate_checksum(ip_packet, header->payload_len);
    if (checksum != header->checksum) {
        ESP_LOGW(TAG, "[BRIDGE RX] Checksum mismatch (expected: 0x%04x, got: 0x%04x)",
                 header->checksum, checksum);
        return;
    }

    s_packets_rx++;
    s_bytes_rx += header->payload_len;

    ESP_LOGI(TAG, "[BRIDGE RX] Packet validated successfully");
    ESP_LOGI(TAG, "[BRIDGE RX] Total RX: %lu packets, %lu bytes",
             (unsigned long)s_packets_rx, (unsigned long)s_bytes_rx);
    ESP_LOGI(TAG, "[BRIDGE RX] ========================================");

    // Inject the IP packet into the local network
    // This would typically use netif input hooks or raw sockets
    // For now, we'll log the receipt - actual injection requires
    // platform-specific hooks into the lwIP stack

    // TODO: Implement packet injection via:
    // - esp_netif_receive() if available
    // - Direct netif->input() call with constructed pbuf
    // - Raw socket forwarding

    ESP_LOGD(TAG, "[BRIDGE RX] Packet injection pending implementation");
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Extract subnet ID from IP address
 *
 * Assumes IP is in format 192.168.{10+subnet_id}.x
 */
static uint8_t get_subnet_from_ip(uint32_t ip)
{
    // IP is in network byte order (big-endian on ESP32)
    // Extract third octet
    uint8_t third_octet = (ip >> 16) & 0xFF;

    // Subnet = 192.168.{10+id}.x, so id = third_octet - 10
    if (third_octet >= 10 && third_octet < 250) {
        return third_octet - 10;
    }

    // Invalid subnet, return 0
    return 0;
}

/**
 * @brief Calculate simple checksum for packet validation
 */
static uint16_t calculate_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    // Sum all bytes
    for (i = 0; i < len; i++) {
        sum += data[i];
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

// ============================================================================
// Network Interface Hook (for packet capture)
// ============================================================================

/**
 * @brief Hook to capture outgoing IP packets from external AP
 *
 * This needs to be called from the network interface's output function
 * to intercept packets destined for other subnets.
 *
 * @param netif Network interface
 * @param p Packet buffer
 * @param dest_ip Destination IP
 * @return true if packet was bridged (should not be sent locally)
 */
bool mesh_bridge_intercept_packet(struct netif *netif, struct pbuf *p, const ip4_addr_t *dest_ip)
{
    if (!s_bridge_enabled || !p || !dest_ip) {
        return false;
    }

    uint8_t dest_subnet = get_subnet_from_ip(dest_ip->addr);
    uint8_t my_subnet = geogram_mesh_get_subnet_id();

    // Only intercept packets going to other mesh subnets (192.168.10-249.x)
    if (dest_subnet == my_subnet || dest_subnet == 0) {
        return false;  // Local or invalid, don't intercept
    }

    // Check if destination subnet has a known mesh node
    geogram_mesh_node_t node;
    if (geogram_mesh_find_node_by_subnet(dest_subnet, &node) != ESP_OK) {
        return false;  // No known node for this subnet
    }

    // Copy packet data and forward
    if (p->tot_len <= CONFIG_GEOGRAM_MESH_BRIDGE_BUFFER_SIZE) {
        uint8_t *packet_copy = malloc(p->tot_len);
        if (packet_copy) {
            pbuf_copy_partial(p, packet_copy, p->tot_len, 0);

            if (mesh_bridge_forward_packet(dest_ip->addr, packet_copy, p->tot_len) == ESP_OK) {
                free(packet_copy);
                return true;  // Packet bridged, don't send locally
            }

            free(packet_copy);
        }
    }

    return false;  // Continue with normal transmission
}
