idf_component_register(
    SRCS "RS485Sensor.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart load_cell inc
)
