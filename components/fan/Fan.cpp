#include "Fan.hpp"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

namespace {
constexpr const char *kTag = "Fan";
constexpr adc_atten_t kAttenuation = ADC_ATTEN_DB_12;
}

Fan::~Fan() { deinitialize(); }

esp_err_t Fan::initialize(int gpio)
{
    if (initialized()) return ESP_ERR_INVALID_STATE;

    adc_unit_t unit_id;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit_id, &channel_);
    if (err != ESP_OK) return err;

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit_id,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_config, &unit_);
    if (err != ESP_OK) return err;

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = kAttenuation,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(unit_, channel_, &channel_config);
    if (err != ESP_OK) {
        deinitialize();
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = unit_id,
        .chan = channel_,
        .atten = kAttenuation,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&calibration_config, &calibration_);
    if (err != ESP_OK) {
        calibration_ = nullptr;
        ESP_LOGW(kTag, "ADC calibration unavailable: %s", esp_err_to_name(err));
    }
#endif

    ESP_LOGI(kTag, "fan current ADC initialized: GPIO%d unit=%d channel=%d",
             gpio, static_cast<int>(unit_id), static_cast<int>(channel_));
    return ESP_OK;
}

void Fan::deinitialize()
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (calibration_ != nullptr) {
        adc_cali_delete_scheme_curve_fitting(calibration_);
        calibration_ = nullptr;
    }
#endif
    if (unit_ != nullptr) {
        adc_oneshot_del_unit(unit_);
        unit_ = nullptr;
    }
}

esp_err_t Fan::readRaw(int &raw)
{
    if (!initialized()) return ESP_ERR_INVALID_STATE;
    int64_t sum = 0;
    for (int i = 0; i < FAN_CURRENT_SAMPLE_COUNT; ++i) {
        int sample = 0;
        const esp_err_t err = adc_oneshot_read(unit_, channel_, &sample);
        if (err != ESP_OK) return err;
        sum += sample;
    }
    raw = static_cast<int>(sum / FAN_CURRENT_SAMPLE_COUNT);
    return ESP_OK;
}

esp_err_t Fan::readMillivolts(int &millivolts)
{
    int raw = 0;
    const esp_err_t err = readRaw(raw);
    if (err != ESP_OK) return err;
    if (calibration_ != nullptr) return adc_cali_raw_to_voltage(calibration_, raw, &millivolts);
    millivolts = (raw * 3300) / 4095;
    return ESP_OK;
}

esp_err_t Fan::read(FanCurrentSensorValue &reading)
{
    reading = {};
    esp_err_t err = readRaw(reading.raw);
    if (err != ESP_OK) return err;
    if (calibration_ != nullptr) {
        err = adc_cali_raw_to_voltage(calibration_, reading.raw, &reading.millivolts);
        if (err != ESP_OK) return err;
    } else {
        reading.millivolts = (reading.raw * 3300) / 4095;
    }
    reading.amperes = millivoltsToAmperes(reading.millivolts);
    reading.valid = true;
    return ESP_OK;
}

void Fan::setCurrentCalibration(float zero_mv, float millivolts_per_amp)
{
    zero_mv_ = zero_mv;
    if (millivolts_per_amp > 0.0F) millivolts_per_amp_ = millivolts_per_amp;
}

float Fan::millivoltsToAmperes(int millivolts) const
{
    return (static_cast<float>(millivolts) - zero_mv_) / millivolts_per_amp_;
}
