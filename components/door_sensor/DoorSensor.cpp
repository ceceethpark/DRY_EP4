#include "DoorSensor.hpp"

#include "esp_log.h"

namespace { constexpr const char *TAG = "DoorSensor"; }

esp_err_t DoorSensor::initialize(gpio_num_t pin)
{
    pin_ = pin;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pin_;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        initialized_ = true;
        ESP_LOGI(TAG, "door sensor initialized on GPIO%d (LOW=CLOSE, HIGH=OPEN)", static_cast<int>(pin_));
    }
    return err;
}

int DoorSensor::rawLevel() const
{
    return initialized_ ? gpio_get_level(pin_) : 0;
}

bool DoorSensor::isOpen() const
{
    return rawLevel() != 0;
}
