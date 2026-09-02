#pragma once

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_err.h"
#include "config.h"
#include "type_def.h"

class Fan {
public:
    Fan() = default;
    ~Fan();

    Fan(const Fan &) = delete;
    Fan &operator=(const Fan &) = delete;

    esp_err_t initialize(int gpio = HW_FAN_CURRENT_ADC_GPIO);
    void deinitialize();
    esp_err_t read(FanCurrentSensorValue &reading);
    esp_err_t readRaw(int &raw);
    esp_err_t readMillivolts(int &millivolts);
    void setCurrentCalibration(float zero_mv, float millivolts_per_amp);
    float millivoltsToAmperes(int millivolts) const;
    bool initialized() const { return unit_ != nullptr; }

private:
    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    adc_channel_t channel_ = ADC_CHANNEL_0;
    float zero_mv_ = FAN_CURRENT_ZERO_MV;
    float millivolts_per_amp_ = FAN_CURRENT_MV_PER_AMP;
};
