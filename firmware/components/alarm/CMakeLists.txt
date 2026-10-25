# Alarm output.
#   src/  pure C: state machine, temporal-three pattern, debouncing (host-tested)
#   hw/   buzzer (LEDC or GPIO) and the WS2812 status ring (led_strip over RMT)
idf_component_register(
    SRCS
        "src/alarm.c"
        "src/debounce.c"
        "hw/buzzer.c"
        "hw/status_ring.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fusion
        esp_driver_gpio
        esp_driver_ledc
)
