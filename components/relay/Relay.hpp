#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "config.h"

class Relay {
public:
    esp_err_t initialize(
        gpio_num_t fan_pin = static_cast<gpio_num_t>(HW_RELAY_FAN_GPIO),
        gpio_num_t heater_pin = static_cast<gpio_num_t>(HW_RELAY_HEATER_GPIO),
        gpio_num_t damper_pin = static_cast<gpio_num_t>(HW_RELAY_DAMPER_GPIO));

    void setFan(bool on);
    void setHeater(bool on);
    void setDamper(bool on);
    void setAll(bool fan_on, bool heater_on, bool damper_on);
    void allOff();

    bool fanOn() const { return fan_on_; }
    bool heaterOn() const { return heater_on_; }
    bool damperOn() const { return damper_on_; }
    bool initialized() const { return initialized_; }

private:
    void write(gpio_num_t pin, bool on);

    gpio_num_t fan_pin_ = GPIO_NUM_NC;
    gpio_num_t heater_pin_ = GPIO_NUM_NC;
    gpio_num_t damper_pin_ = GPIO_NUM_NC;
    bool initialized_ = false;
    bool fan_on_ = false;
    bool heater_on_ = false;
    bool damper_on_ = false;
};
