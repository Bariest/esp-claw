# mpx_robot -- the MangDang quadruped: servo driver boards, gait, IK, IMU.
#
# Ported from main/robot/ of the MPX-Dog firmware. The C++ is deliberate: this
# is proven gait and kinematics code and rewriting it in C would trade a lot of
# risk for a little house style. Everything outside this component talks to it
# through the C facade in include/mpx_robot.h.

idf_component_register(
    SRCS
        "src/driver_board.c"
        "src/robot.cc"
        "src/stanford_gait.cc"
        "src/stanford_kinematics.cc"
        "src/mpx_imu.cc"
        "src/mpx_robot_c_api.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        esp_board_manager
    PRIV_REQUIRES
        driver
        esp_driver_spi
        esp_driver_gpio
        esp_timer
        nvs_flash
        espressif__i2c_bus
)

# The BMI270 driver is only pulled in when the board selects it. Optional so a
# board without an IMU still links.
idf_component_optional_requires(PRIVATE espressif__bmi270_sensor)
