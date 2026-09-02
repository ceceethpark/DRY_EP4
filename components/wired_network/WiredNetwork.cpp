#include "WiredNetwork.hpp"
#include "WiredNetworkConfig.hpp"
#include "EmacDefaults.h"

#include "esp_check.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"

namespace {
constexpr const char *TAG = "WiredNetwork";
}

WiredNetwork::WiredNetwork()
    : _ethHandle(nullptr),
      _netif(nullptr),
      _glue(nullptr),
      _readyCallback(nullptr),
      _callbackContext(nullptr)
{
}

bool WiredNetwork::start(ReadyCallback callback, void *context)
{
    if (_ethHandle) return true;
    _readyCallback = callback;
    _callbackContext = context;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return false;
    }

    eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
    phyConfig.phy_addr = WiredNetworkConfig::PHY_ADDRESS;
    phyConfig.reset_gpio_num = WiredNetworkConfig::PHY_RESET_GPIO;

    eth_esp32_emac_config_t emacConfig =
        wired_network_default_emac_config();
    emacConfig.smi_gpio.mdc_num = WiredNetworkConfig::MDC_GPIO;
    emacConfig.smi_gpio.mdio_num = WiredNetworkConfig::MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emacConfig, &macConfig);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phyConfig);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "Failed to create IP101 MAC/PHY");
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        return false;
    }

    esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&ethConfig, &_ethHandle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        mac->del(mac);
        phy->del(phy);
        return false;
    }

    esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
    _netif = esp_netif_new(&netifConfig);
    _glue = esp_eth_new_netif_glue(_ethHandle);
    if (!_netif || !_glue || esp_netif_attach(_netif, _glue) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet netif attach failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, ethEvent, this));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, ipEvent, this));

    err = esp_eth_start(_ethHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Ethernet initialization started (IP101/RMII)");
    return true;
}

void WiredNetwork::ethEvent(void *, esp_event_base_t, int32_t eventId,
                            void *eventData)
{
    switch (eventId) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = {};
        esp_eth_handle_t handle = *static_cast<esp_eth_handle_t *>(eventData);
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Ethernet Link Up, MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet Link Down - check LAN cable");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started, waiting for link/DHCP");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

void WiredNetwork::ipEvent(void *arg, esp_event_base_t, int32_t,
                           void *eventData)
{
    auto *self = static_cast<WiredNetwork *>(arg);
    auto *event = static_cast<ip_event_got_ip_t *>(eventData);
    ESP_LOGI(TAG, "Ethernet Got IP");
    ESP_LOGI(TAG, "IP:      " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    if (self->_readyCallback) {
        self->_readyCallback(self->_callbackContext);
    }
}
