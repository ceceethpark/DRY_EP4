idf_component_register(
    SRCS "WiredNetwork.cpp" "EmacDefaults.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif esp_event
)
