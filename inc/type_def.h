#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/* Modbus slave addresses and registers */
#define MODBUS_CONTROL_ADDRESS         100
#define MODBUS_MONITOR_FIRST_ADDRESS  101
#define MODBUS_MONITOR_LAST_ADDRESS   106
#define MODBUS_LOAD_CELL_ADDRESS       200
#define MODBUS_LOAD_CELL_WEIGHT_REG      0

/* Application-wide limits */
#define DRYER_SENSOR_COUNT               7
#define DRYER_MONITOR_COUNT              6
#define DRYER_SETTING_COUNT              18

/* Dryer operating state shared by all application components. */
typedef enum {
    DRY_PREPARE = 0,
    DRY_PREHEAT,
    DRY_RUN,
    DRY_COOL,
    DRY_FINISH
} DryState;

typedef struct {
    int32_t zero_raw;
    int32_t span_raw;
    int32_t reference_weight_deci_g;
} LoadCellCalibration;

typedef struct {
    int32_t raw;
    float weight_g;
    TickType_t updated_at;
    bool valid;
} LoadCellReading;

typedef struct {
    float temperature_c;
    float humidity_pct;
    TickType_t updated_at;
    bool valid;
} TemperatureHumidityValue;

typedef struct {
    int raw;
    int millivolts;
    float amperes;
    bool valid;
} FanCurrentSensorValue;

typedef struct {
    int raw_level;
    bool open;
    bool valid;
} DoorSensorValue;

/* Dryer alarm bitmap. Keep the bit order fixed for communication/storage. */
typedef union _ALARM_INFO {
    struct {
        uint8_t door_open      : 1;
        uint8_t thermist_short : 1;
        uint8_t thermist_open  : 1;
        uint8_t thermo_state   : 1;
        uint8_t fan_min_error  : 1;
        uint8_t fan_max_error  : 1;
        uint8_t heater1_error  : 1;
        uint8_t heater2_error  : 1;
        uint8_t mem_error      : 1;
        uint8_t under_heat     : 1;
        uint8_t over_heat      : 1;
        uint8_t fan_relay_on     : 1;
        uint8_t heater_relay_on  : 1;
        uint8_t damper_relay_on  : 1;
        uint8_t x14            : 1;
        uint8_t mqtt_connect   : 1;
    };
    uint16_t data;
} EVENT_INFO;

#ifdef __cplusplus
static_assert(sizeof(EVENT_INFO) == sizeof(uint16_t),
              "ALARM_INFO must remain a 16-bit bitmap");
#else
_Static_assert(sizeof(EVENT_INFO) == sizeof(uint16_t),
               "EVENT_INFO must remain a 16-bit bitmap");
#endif

/* Complete one-second sensor snapshot shared by the dryer application. */
typedef struct {
    TemperatureHumidityValue temperature_humidity[DRYER_SENSOR_COUNT];
    LoadCellReading load_cell;
    FanCurrentSensorValue fan_current;
    DoorSensorValue door;
    EVENT_INFO event;
    float blower_speed_ms;
    float damper_percent;
    TickType_t updated_at;
    uint32_t sequence;
} DryerSensorValues;

extern DryerSensorValues g_dryer_sensor_values;

typedef struct {
    int32_t remain_min;
    int32_t set_temp_deci_c;
    int32_t preheat_temp_c;
    int32_t preheat_time_min;
    int32_t preheat_remain_min;
    uint8_t preheat_active;
} DryerNvsRuntime;

typedef struct {
    float temperature[DRYER_SENSOR_COUNT];
    float humidity[DRYER_SENSOR_COUNT];
} DryerNvsCalibration;

typedef struct {
    int32_t dry_temp_c;
    int32_t dry_time_min;
    int32_t temp_hysteresis_c;
    int32_t cooling_temp_c;
    int32_t cooling_time_min;
    int32_t damper_mode;
    int32_t damper_open_humidity_pct;
    int32_t damper_hysteresis_pct;
    int32_t high_warning_temp_c;
    int32_t low_warning_reach_time_min;
    int32_t min_temp_rise_c_per_min;
    int32_t fan_adc_at_10ms;
    int32_t mqtt_publish_interval_min;
    int32_t http_server_ip1;
    int32_t http_server_ip2;
    int32_t http_server_ip3;
    int32_t http_server_ip4;
    int32_t http_server_port;
} DryerNvsCoolingSettings;
