#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include "config.h"

class DoorSensor {
public:
    esp_err_t initialize(gpio_num_t pin = static_cast<gpio_num_t>(HW_DOOR_SENSOR_GPIO));
    bool isOpen() const;
    bool isClosed() const { return !isOpen(); }
    int rawLevel() const;
    bool initialized() const { return initialized_; }

private:
    gpio_num_t pin_ = GPIO_NUM_NC;
    bool initialized_ = false;
};
