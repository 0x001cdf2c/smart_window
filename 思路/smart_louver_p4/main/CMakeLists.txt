idf_component_register(
    SRCS
        "app_main.c"
        "sensor_manager.c"
        "servo_manager.c"

    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        esp_timer
        esp_driver_ledc
        esp_adc
)