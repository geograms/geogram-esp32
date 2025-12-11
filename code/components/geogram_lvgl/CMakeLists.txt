# LVGL port for Geogram project
# Only build for ESP32-S3 targets with e-paper display

if(CONFIG_IDF_TARGET_ESP32S3)
    idf_component_register(
        SRCS "lvgl_port.c"
        INCLUDE_DIRS "."
        REQUIRES lvgl geogram_epaper_1in54 log
        PRIV_REQUIRES freertos esp_timer heap
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
