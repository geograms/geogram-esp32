# Geogram HTTP server component for WiFi configuration and Station API

idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server nvs_flash log geogram_station geogram_ws geogram_tiles geogram_updates
    PRIV_REQUIRES esp_wifi
)
