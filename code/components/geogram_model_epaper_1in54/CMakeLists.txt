# Only build for ESP32-S3 targets
if(CONFIG_IDF_TARGET_ESP32S3)
    idf_component_register(
        SRCS "model_init.c" "board_power.cpp"
        INCLUDE_DIRS "."
        REQUIRES driver log nvs_flash geogram_i2c geogram_epaper_1in54 geogram_pcf85063 geogram_shtc3 geogram_button geogram_sdcard
        PRIV_REQUIRES freertos
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
