idf_component_register(
    SRCS "Vision.cpp"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl esp_video esp32_p4_function_ev_board esp_driver_i2c buzzer
)
