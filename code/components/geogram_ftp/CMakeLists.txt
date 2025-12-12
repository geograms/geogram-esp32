idf_component_register(
    SRCS "ftp_server.c"
    INCLUDE_DIRS "."
    REQUIRES log nvs_flash geogram_sdcard
)
