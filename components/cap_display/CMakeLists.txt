idf_component_register(
    SRCS "src/cap_display.c" "src/cap_display_face.c"
    INCLUDE_DIRS "include"
    REQUIRES
        claw_cap
        display_service
        lvgl
    PRIV_REQUIRES
        espressif__cjson
        esp_board_manager
        esp_driver_ledc
        esp_hw_support
        esp_timer
)
