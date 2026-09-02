idf_component_register(
    SRCS "DryerApp.cpp" "hanwool_logo.c" "nvs/DryerNvsStore.cpp"
    INCLUDE_DIRS "." "nvs"
    REQUIRES lvgl__lvgl ycb_lcd nvs_flash app_mqtt rs485_sensor load_cell vision buzzer door_sensor relay fan inc esp_hw_support esp_http_client esp_driver_jpeg esp_netif
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -DLV_LVGL_H_INCLUDE_SIMPLE
    -Wno-format
)
