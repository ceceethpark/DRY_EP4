idf_component_register(
    SRCS "OtaService.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server app_update esp_netif esp_eth
)
