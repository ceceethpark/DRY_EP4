#include "EmacDefaults.h"

eth_esp32_emac_config_t wired_network_default_emac_config(void)
{
    eth_esp32_emac_config_t config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    return config;
}
