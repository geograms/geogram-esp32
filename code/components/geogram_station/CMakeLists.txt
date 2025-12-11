idf_component_register(
    SRCS "station.c"
    INCLUDE_DIRS "."
    REQUIRES log esp_wifi esp_timer geogram_json geogram_nostr
)
