#include "Buzzer.hpp"
#include "esp_log.h"

namespace { constexpr const char *TAG = "Buzzer"; }

Buzzer &Buzzer::instance(){static Buzzer buzzer;return buzzer;}

esp_err_t Buzzer::initialize(gpio_num_t pin)
{
    if(initialized_) return ESP_OK;
    pin_=pin;
    gpio_config_t config{};config.pin_bit_mask=1ULL<<pin_;config.mode=GPIO_MODE_OUTPUT;config.pull_down_en=GPIO_PULLDOWN_ENABLE;config.intr_type=GPIO_INTR_DISABLE;
    esp_err_t err=gpio_config(&config);if(err!=ESP_OK)return err;gpio_set_level(pin_,0);
    const esp_timer_create_args_t timer_config={.callback=toneTimerCallback,.arg=this,.dispatch_method=ESP_TIMER_TASK,.name="key_tone",.skip_unhandled_events=true};
    err=esp_timer_create(&timer_config,&tone_timer_);if(err==ESP_OK){initialized_=true;ESP_LOGI(TAG,"active-high buzzer initialized on GPIO%d",static_cast<int>(pin_));}return err;
}
void Buzzer::set(bool on){if(initialized_)gpio_set_level(pin_,on?1:0);}
void Buzzer::keyTone(uint32_t duration_ms){if(!initialized_&&initialize()!=ESP_OK)return;esp_timer_stop(tone_timer_);set(true);esp_timer_start_once(tone_timer_,static_cast<uint64_t>(duration_ms)*1000ULL);}
void Buzzer::attach(lv_obj_t *object){if(object&&lv_obj_has_flag(object,LV_OBJ_FLAG_CLICKABLE))lv_obj_add_event_cb(object,touchEvent,LV_EVENT_PRESSED,this);}
void Buzzer::attachToTree(lv_obj_t *root){if(!root)return;attach(root);const uint32_t count=lv_obj_get_child_cnt(root);for(uint32_t i=0;i<count;i++)attachToTree(lv_obj_get_child(root,i));}
void Buzzer::toneTimerCallback(void *context){static_cast<Buzzer*>(context)->set(false);}
void Buzzer::touchEvent(lv_event_t *event){static_cast<Buzzer*>(lv_event_get_user_data(event))->keyTone();}
