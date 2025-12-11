# Geogram HTTP server component for WiFi configuration

idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server nvs_flash log
    PRIV_REQUIRES esp_wifi
)
