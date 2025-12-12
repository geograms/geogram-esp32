# Serial console component for Geogram
# Provides CLI over UART for device control and debugging

idf_component_register(
    SRCS "console.c" "cmd_system.c" "cmd_wifi.c" "cmd_display.c" "cmd_config.c" "cmd_ssh.c" "cmd_ftp.c"
    INCLUDE_DIRS "." "../../include"
    REQUIRES console esp_system driver nvs_flash log vfs
             geogram_station geogram_wifi geogram_json geogram_sdcard geogram_ssh geogram_ftp
    PRIV_REQUIRES geogram_model_epaper_1in54 geogram_shtc3 geogram_lvgl
)
