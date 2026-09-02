#include "Relay.hpp"

#include "esp_log.h"

namespace { constexpr const char *TAG = "Relay"; }

esp_err_t Relay::initialize(gpio_num_t fan_pin, gpio_num_t heater_pin,
                            gpio_num_t damper_pin)
{
    fan_pin_ = fan_pin;
    heater_pin_ = heater_pin;
    damper_pin_ = damper_pin;

    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << fan_pin_) | (1ULL << heater_pin_) |
                          (1ULL << damper_pin_);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;

    initialized_ = true;
    allOff();
    ESP_LOGI(TAG, "active-high relays initialized: FAN=GPIO%d HEATER=GPIO%d DAMPER=GPIO%d",
             static_cast<int>(fan_pin_), static_cast<int>(heater_pin_),
             static_cast<int>(damper_pin_));
    return ESP_OK;
}

void Relay::write(gpio_num_t pin, bool on)
{
    if (initialized_) gpio_set_level(pin, on ? 1 : 0);
}

void Relay::setFan(bool on) { fan_on_ = on; write(fan_pin_, on); }
void Relay::setHeater(bool on) { heater_on_ = on; write(heater_pin_, on); }
void Relay::setDamper(bool on) { damper_on_ = on; write(damper_pin_, on); }

void Relay::setAll(bool fan_on, bool heater_on, bool damper_on)
{
    setFan(fan_on);
    setHeater(heater_on);
    setDamper(damper_on);
}

void Relay::allOff() { setAll(false, false, false); }
