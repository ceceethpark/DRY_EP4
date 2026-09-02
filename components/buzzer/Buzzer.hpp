#pragma once
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "config.h"

class Buzzer {
public:
    static Buzzer &instance();
    esp_err_t initialize(gpio_num_t pin = static_cast<gpio_num_t>(HW_BUZZER_GPIO));
    void set(bool on);
    void keyTone(uint32_t duration_ms = 45);
    void attach(lv_obj_t *object);
    void attachToTree(lv_obj_t *root);
private:
    Buzzer() = default;
    static void toneTimerCallback(void *context);
    static void touchEvent(lv_event_t *event);
    gpio_num_t pin_ = GPIO_NUM_NC;
    esp_timer_handle_t tone_timer_ = nullptr;
    bool initialized_ = false;
};
