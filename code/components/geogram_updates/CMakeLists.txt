idf_component_register(
    SRCS "updates.c"
    INCLUDE_DIRS "."
    REQUIRES log json geogram_sdcard geogram_http_client esp_http_server
)
