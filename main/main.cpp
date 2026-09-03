#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"

#include <cstdlib>
#include <ctime>

#include "DryerApp.hpp"
#include "WiredNetwork.hpp"
#include "RS485Sensor.hpp"
#include "LoadCell.hpp"
#include "OtaService.hpp"

namespace {
constexpr const char *TAG = "DY-EP4";
DryerApp g_dryer;
WiredNetwork g_network;
RS485Sensor g_rs485Sensors;
LoadCell g_loadCell;
OtaService g_otaService;

void networkReady(void *context)
{
    auto *dryer = static_cast<DryerApp *>(context);
    setenv("TZ", "KST-9", 1);
    tzset();
    const esp_sntp_config_t ntpConfig =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    const esp_err_t ntpErr = esp_netif_sntp_init(&ntpConfig);
    if (ntpErr == ESP_OK) {
        ESP_LOGI(TAG, "NTP synchronization started: pool.ntp.org (KST)");
    } else {
        ESP_LOGW(TAG, "NTP initialization failed: %s", esp_err_to_name(ntpErr));
    }
    ESP_LOGI(TAG, "Network ready: starting MQTT");
    if (!g_otaService.start()) {
        ESP_LOGE(TAG, "EP6 OTA service start failed");
    }
    if (!dryer->startCommunication()) {
        ESP_LOGE(TAG, "MQTT client start failed");
    }
}
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Boot: app_main entered");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS requires erase before init: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "Boot: NVS initialized");

    g_dryer.setSensorSource(&g_rs485Sensors);
    g_dryer.setLoadCell(&g_loadCell);
    g_rs485Sensors.setLoadCell(&g_loadCell);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_max_sleep_ms = 16;
    lvgl_cfg.timer_period_ms = 1;

    const bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = lvgl_cfg,
        .buffer_size = BSP_LCD_H_RES * 1280,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        },
    };

    ESP_LOGI(TAG, "Boot: starting display");
    if (bsp_display_start_with_config(&display_cfg) == nullptr) {
        ESP_LOGE(TAG, "LCD/LVGL initialization failed");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(TAG, "Boot: display ready");

    bsp_display_lock(0);
    ESP_LOGI(TAG, "Boot: building UI");
    const bool uiInitialized = g_dryer.init();
    const bool uiRunning = uiInitialized && g_dryer.run();
    bsp_display_unlock();
    if (!uiRunning) {
        ESP_LOGE(TAG, "DY-EP4 UI initialization failed");
        return;
    }
    ESP_LOGI(TAG, "Boot: UI running");

    err = g_rs485Sensors.start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RS485 sensor start failed: %s", esp_err_to_name(err));
    }

    if (!g_network.start(networkReady, &g_dryer)) {
        ESP_LOGE(TAG, "Wired Ethernet initialization failed");
    }
    ESP_LOGI(TAG, "DY-EP4 dryer started");
}
