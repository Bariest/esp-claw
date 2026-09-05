# UART link to the Raspberry Pi on the J-PI header. See include/mpx_pi_link.h.
#
# Gated at the manifest level (main/idf_component.yml pulls this in under
# MP4_ROBOT_ENABLE) and at the call site (main.c under MP4_PI_LINK_ENABLE);
# the sources themselves always build, so a Kconfig change does not need a
# reconfigure to take effect on this component.
idf_component_register(
    SRCS
        "src/mpx_pi_link.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        console
        esp_driver_uart
        esp_timer
)
