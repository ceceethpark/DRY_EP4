idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES dryer wired_network rs485_sensor load_cell vision nvs_flash esp32_p4_function_ev_board esp_netif
)
