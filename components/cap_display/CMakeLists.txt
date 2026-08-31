idf_component_register(
    SRCS "src/cap_display.c"
    INCLUDE_DIRS "include"
    REQUIRES
        claw_cap
        display_service
        lvgl
    PRIV_REQUIRES
        espressif__cjson
        esp_board_manager
        esp_driver_ledc
)
