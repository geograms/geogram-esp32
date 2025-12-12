idf_component_register(
    SRCS "tiles.c"
    INCLUDE_DIRS "."
    REQUIRES log geogram_sdcard geogram_http_client esp_http_server
)
