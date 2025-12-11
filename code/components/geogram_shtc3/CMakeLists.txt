# Only build for ESP32-S3 targets (for now - this sensor is on the ePaper board)
if(CONFIG_IDF_TARGET_ESP32S3)
    idf_component_register(
        SRCS "shtc3.cpp"
        INCLUDE_DIRS "."
        REQUIRES geogram_i2c log
        PRIV_REQUIRES freertos
    )
else()
    # Register empty component for non-ESP32S3 targets
    idf_component_register()
endif()
