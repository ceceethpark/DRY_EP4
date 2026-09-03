#include "DryerNvsStore.hpp"
#include "config.h"

#include "esp_err.h"
#include "nvs.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *RUNTIME_NS = "dryer";
constexpr const char *CALIBRATION_NS = "sensor_cal";
constexpr const char *LOAD_CELL_NS = "load_cell";
constexpr const char *COOLING_NS = "cooling_cfg";
constexpr int32_t COOLING_VERSION = 3;
constexpr int32_t RUNTIME_VERSION = 3;
constexpr int32_t CALIBRATION_VERSION = 1;
constexpr const char *EQUIPMENT_NS = "equipment";
}

bool DryerNvsStore::saveEquipmentName(const char *name) const
{
    if (!name || !name[0]) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(EQUIPMENT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    if (err == ESP_OK) err = nvs_set_str(h, "name", name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadEquipmentName(char *name, size_t size) const
{
    if (!name || size == 0) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(EQUIPMENT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    size_t required = size;
    err = nvs_get_str(h, "name", name, &required);
    nvs_close(h);
    return err == ESP_OK && name[0] != '\0';
}

bool DryerNvsStore::saveEquipmentInfo(const char *name, int32_t equipmentId) const
{
    if (!name || !name[0] || equipmentId < EQUIPMENT_ID_MIN || equipmentId > EQUIPMENT_ID_MAX)
        return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(EQUIPMENT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_set_str(h, "name", name);
    if (err == ESP_OK) err = nvs_set_i32(h, "id", equipmentId);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadEquipmentId(int32_t *equipmentId) const
{
    if (!equipmentId) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(EQUIPMENT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    int32_t loaded = 0;
    err = nvs_get_i32(h, "id", &loaded);
    nvs_close(h);
    if (err != ESP_OK || loaded < EQUIPMENT_ID_MIN || loaded > EQUIPMENT_ID_MAX) return false;
    *equipmentId = loaded;
    return true;
}

bool DryerNvsStore::saveCoolingSettings(const DryerNvsCoolingSettings &v) const
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(COOLING_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_set_blob(h, "settings", &v, sizeof(v));
    if (err == ESP_OK) err = nvs_set_i32(h, "version", COOLING_VERSION);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadCoolingSettings(DryerNvsCoolingSettings *v) const
{
    if (v == nullptr) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(COOLING_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    int32_t version=1;
    if(nvs_get_i32(h,"version",&version)!=ESP_OK) version=1;
    DryerNvsCoolingSettings loaded{};
    loaded.fan_min_speed_ms=DRYER_CFG_DEFAULT_FAN_MIN_SPEED_MS;
    size_t storedSize=0;
    err=nvs_get_blob(h,"settings",nullptr,&storedSize);
    const size_t previousSize=sizeof(loaded)-sizeof(loaded.fan_min_speed_ms);
    const size_t legacySize=previousSize-5U*sizeof(int32_t);
    if(err==ESP_OK && (storedSize==sizeof(loaded)||storedSize==previousSize||storedSize==legacySize)) {
        size_t readSize=storedSize;
        err=nvs_get_blob(h,"settings",&loaded,&readSize);
        if(err==ESP_OK && storedSize==legacySize) {
            loaded.preheat_temp_c=DRYER_CFG_DEFAULT_PREHEAT_TEMP_C;
            loaded.preheat_time_min=DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN;
            loaded.standby_enabled=DRYER_CFG_DEFAULT_STANDBY_ENABLED;
            loaded.standby_time_min=DRYER_CFG_DEFAULT_STANDBY_TIME_MIN;
            loaded.standby_temp_c=DRYER_CFG_DEFAULT_STANDBY_TEMP_C;
        }
    } else if(err==ESP_OK) {
        err=ESP_ERR_INVALID_SIZE;
    }
    /* Migrate former factory calibrations to the new 10 m/s ADC value. */
    if(err==ESP_OK && version<COOLING_VERSION &&
       (loaded.fan_adc_at_10ms==1135 || loaded.fan_adc_at_10ms==4096))
        loaded.fan_adc_at_10ms=DRYER_CFG_DEFAULT_FAN_ADC_AT_10MS;
    nvs_close(h);
    if (err != ESP_OK || loaded.dry_temp_c<DRYER_CFG_TEMP_MIN_C || loaded.dry_temp_c>DRYER_CFG_TEMP_MAX_C ||
        loaded.dry_time_min<DRYER_CFG_TIME_MIN_MIN || loaded.dry_time_min>DRYER_CFG_TIME_MIN_MAX || loaded.temp_hysteresis_c<DRYER_CFG_TEMP_HYSTERESIS_MIN_C || loaded.temp_hysteresis_c>DRYER_CFG_TEMP_HYSTERESIS_MAX_C ||
        loaded.cooling_temp_c<DRYER_CFG_TEMP_MIN_C || loaded.cooling_temp_c>DRYER_CFG_TEMP_MAX_C || loaded.cooling_time_min<DRYER_CFG_TIME_MIN_MIN || loaded.cooling_time_min>DRYER_CFG_TIME_MIN_MAX ||
        loaded.damper_mode<DRYER_CFG_DAMPER_MODE_MIN || loaded.damper_mode>DRYER_CFG_DAMPER_MODE_MAX ||
        loaded.damper_open_humidity_pct<DRYER_CFG_HUMIDITY_MIN_PCT || loaded.damper_open_humidity_pct>DRYER_CFG_HUMIDITY_MAX_PCT || loaded.damper_hysteresis_pct<DRYER_CFG_DAMPER_HYSTERESIS_MIN_PCT || loaded.damper_hysteresis_pct>DRYER_CFG_DAMPER_HYSTERESIS_MAX_PCT ||
        loaded.high_warning_temp_c<DRYER_CFG_TEMP_MIN_C || loaded.high_warning_temp_c>DRYER_CFG_WARNING_TEMP_MAX_C || loaded.low_warning_reach_time_min<DRYER_CFG_LOW_REACH_TIME_MIN || loaded.low_warning_reach_time_min>DRYER_CFG_TIME_MIN_MAX ||
        loaded.min_temp_rise_c_per_min<DRYER_CFG_RISE_RATE_MIN_C_PER_MIN || loaded.min_temp_rise_c_per_min>DRYER_CFG_RISE_RATE_MAX_C_PER_MIN ||
        loaded.fan_adc_at_10ms<DRYER_CFG_FAN_ADC_AT_10MS_MIN || loaded.fan_adc_at_10ms>DRYER_CFG_FAN_ADC_AT_10MS_MAX ||
        loaded.mqtt_publish_interval_min<DRYER_CFG_MQTT_PUBLISH_INTERVAL_MIN || loaded.mqtt_publish_interval_min>DRYER_CFG_MQTT_PUBLISH_INTERVAL_MAX ||
        loaded.http_server_ip1<0 || loaded.http_server_ip1>255 || loaded.http_server_ip2<0 || loaded.http_server_ip2>255 ||
        loaded.http_server_ip3<0 || loaded.http_server_ip3>255 || loaded.http_server_ip4<0 || loaded.http_server_ip4>255 ||
        loaded.http_server_port<1 || loaded.http_server_port>65535 ||
        loaded.preheat_temp_c<DRYER_CFG_PREHEAT_TEMP_MIN_C || loaded.preheat_temp_c>DRYER_CFG_TEMP_MAX_C ||
        loaded.preheat_time_min<DRYER_CFG_TIME_MIN_MIN || loaded.preheat_time_min>DRYER_CFG_PREHEAT_TIME_MIN_MAX ||
        loaded.standby_enabled<DRYER_CFG_STANDBY_MODE_MIN || loaded.standby_enabled>DRYER_CFG_STANDBY_MODE_MAX ||
        loaded.standby_time_min<DRYER_CFG_TIME_MIN_MIN || loaded.standby_time_min>DRYER_CFG_TIME_MIN_MAX ||
        loaded.standby_temp_c<DRYER_CFG_STANDBY_TEMP_MIN_C || loaded.standby_temp_c>DRYER_CFG_TEMP_MAX_C ||
        loaded.fan_min_speed_ms<DRYER_CFG_FAN_MIN_SPEED_MS_MIN ||
        loaded.fan_min_speed_ms>DRYER_CFG_FAN_MIN_SPEED_MS_MAX) return false;
    *v = loaded;
    return true;
}

bool DryerNvsStore::saveLoadCellCalibration(const LoadCellCalibration &v) const
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(LOAD_CELL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    if (err == ESP_OK) err = nvs_set_i32(h, "zero", v.zero_raw);
    if (err == ESP_OK) err = nvs_set_i32(h, "span", v.span_raw);
    if (err == ESP_OK) err = nvs_set_i32(h, "ref_dg", v.reference_weight_deci_g);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadLoadCellCalibration(LoadCellCalibration *v) const
{
    if (v == nullptr) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(LOAD_CELL_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    LoadCellCalibration loaded{};
    err = nvs_get_i32(h, "zero", &loaded.zero_raw);
    if (err == ESP_OK) err = nvs_get_i32(h, "span", &loaded.span_raw);
    if (err == ESP_OK) err = nvs_get_i32(h, "ref_dg", &loaded.reference_weight_deci_g);
    nvs_close(h);
    if (err != ESP_OK || loaded.reference_weight_deci_g <= 0 ||
        loaded.span_raw == loaded.zero_raw) return false;
    *v = loaded;
    return true;
}

bool DryerNvsStore::saveRuntime(const DryerNvsRuntime &v) const
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RUNTIME_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    if (err == ESP_OK) err = nvs_set_i32(h, "remain", v.remain_min);
    if (err == ESP_OK) err = nvs_set_i32(h, "set_temp10", v.set_temp_deci_c);
    if (err == ESP_OK) err = nvs_set_i32(h, "pre_temp", v.preheat_temp_c);
    if (err == ESP_OK) err = nvs_set_i32(h, "pre_time", v.preheat_time_min);
    if (err == ESP_OK) err = nvs_set_i32(h, "pre_rem", v.preheat_remain_min);
    if (err == ESP_OK) err = nvs_set_u8(h, "pre_active", v.preheat_active);
    if (err == ESP_OK) err = nvs_set_u8(h, "dry_state", v.dry_state);
    if (err == ESP_OK) err = nvs_set_u8(h, "standby", v.standby_active);
    if (err == ESP_OK) err = nvs_set_i32(h, "cool_rem", v.cooling_remain_min);
    if (err == ESP_OK) err = nvs_set_i32(h, "tare_g", v.tare_weight_g);
    if (err == ESP_OK) err = nvs_set_i32(h, "cfg_ver", RUNTIME_VERSION);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadRuntime(DryerNvsRuntime *v) const
{
    if (v == nullptr) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(RUNTIME_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    DryerNvsRuntime loaded{};
    int32_t version = 0;
    err = nvs_get_i32(h, "cfg_ver", &version);
    if (err == ESP_OK) err = nvs_get_i32(h, "remain", &loaded.remain_min);
    if (err == ESP_OK) err = nvs_get_i32(h, "set_temp10", &loaded.set_temp_deci_c);
    if (err == ESP_OK) err = nvs_get_i32(h, "pre_temp", &loaded.preheat_temp_c);
    if (err == ESP_OK) err = nvs_get_i32(h, "pre_time", &loaded.preheat_time_min);
    if (err == ESP_OK) err = nvs_get_i32(h, "pre_rem", &loaded.preheat_remain_min);
    if (err == ESP_OK) err = nvs_get_u8(h, "pre_active", &loaded.preheat_active);
    if (err == ESP_OK && version >= 2) err = nvs_get_u8(h, "dry_state", &loaded.dry_state);
    if (err == ESP_OK && version >= 2) err = nvs_get_u8(h, "standby", &loaded.standby_active);
    if (err == ESP_OK && version >= 2) err = nvs_get_i32(h, "cool_rem", &loaded.cooling_remain_min);
    if (err == ESP_OK && version >= 3) err = nvs_get_i32(h, "tare_g", &loaded.tare_weight_g);
    nvs_close(h);
    if (err != ESP_OK || (version < 1 || version > RUNTIME_VERSION) ||
        loaded.remain_min < 0 || loaded.remain_min > 1440 ||
        loaded.set_temp_deci_c < 0 || loaded.set_temp_deci_c > 900 ||
        loaded.preheat_temp_c < DRYER_CFG_PREHEAT_TEMP_MIN_C ||
        loaded.preheat_temp_c > DRYER_CFG_TEMP_MAX_C ||
        loaded.preheat_time_min < 0 || loaded.preheat_time_min > 120 ||
        loaded.preheat_remain_min < 0 || loaded.preheat_remain_min > 120) return false;
    if (version == 1) {
        loaded.dry_state = loaded.preheat_active && loaded.preheat_remain_min > 0
                         ? DRY_PREHEAT
                         : loaded.remain_min > 0 ? DRY_RUN : DRY_FINISH;
        loaded.standby_active = 0;
        loaded.cooling_remain_min = 0;
    }
    if (version < 3) loaded.tare_weight_g = 0;
    if (loaded.dry_state > DRY_FINISH || loaded.standby_active > 1 ||
        loaded.cooling_remain_min < 0 || loaded.cooling_remain_min > DRYER_CFG_TIME_MIN_MAX)
        return false;
    *v = loaded;
    return true;
}

bool DryerNvsStore::saveCalibration(const DryerNvsCalibration &v) const
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CALIBRATION_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    for (int i = 0; i < DRYER_SENSOR_COUNT && err == ESP_OK; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", i);
        err = nvs_set_i16(h, key, (int16_t)std::lround(v.temperature[i] * 10.0f));
        snprintf(key, sizeof(key), "h%d", i);
        if (err == ESP_OK) err = nvs_set_i16(h, key, (int16_t)std::lround(v.humidity[i] * 10.0f));
    }
    if (err == ESP_OK) err = nvs_set_i32(h, "version", CALIBRATION_VERSION);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool DryerNvsStore::loadCalibration(DryerNvsCalibration *v) const
{
    if (v == nullptr) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CALIBRATION_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    DryerNvsCalibration loaded{};
    int32_t version = 0;
    err = nvs_get_i32(h, "version", &version);
    for (int i = 0; i < DRYER_SENSOR_COUNT && err == ESP_OK; ++i) {
        char key[8]; int16_t raw = 0;
        snprintf(key, sizeof(key), "t%d", i);
        err = nvs_get_i16(h, key, &raw);
        if (err == ESP_OK) loaded.temperature[i] = raw / 10.0f;
        snprintf(key, sizeof(key), "h%d", i);
        if (err == ESP_OK) err = nvs_get_i16(h, key, &raw);
        if (err == ESP_OK) loaded.humidity[i] = raw / 10.0f;
    }
    nvs_close(h);
    if (err != ESP_OK || version != CALIBRATION_VERSION) return false;
    *v = loaded;
    return true;
}
