# Sensor drivers behind the common score API.
#   score/  pure C, host-tested by firmware/Makefile (test/test_sensors.c)
#   hw/     ESP-IDF I/O that feeds score/
idf_component_register(
    SRCS
        "score/ads1115_reg.c"
        "score/dht22_frame.c"
        "score/fan_health.c"
        "score/flame_score.c"
        "score/fx_sensor.c"
        "score/heat_score.c"
        "score/mq_score.c"
        "score/ntc_score.c"
        "hw/ads1115.c"
        "hw/dht22_driver.c"
        "hw/fan.c"
        "hw/flame_driver.c"
        "hw/mq_driver.c"
        "hw/ntc_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES
        fusion
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_pcnt
        esp_driver_rmt
        esp_timer
)
