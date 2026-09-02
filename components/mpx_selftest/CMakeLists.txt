set(selftest_requires
    console
    cap_display
    claw_core
    esp_board_manager
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_partition
    fatfs
)

if(CONFIG_MP4_ROBOT_ENABLE)
    list(APPEND selftest_requires mpx_robot)
endif()

idf_component_register(
    SRCS "mpx_selftest.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${selftest_requires}
)
