#pragma once

/* Hardware pin map (single source of truth) */
#define HW_BUZZER_GPIO                  1
#define HW_DOOR_SENSOR_GPIO             2
#define HW_RELAY_HEATER_GPIO            5
#define HW_RELAY_FAN_GPIO               3
#define HW_FAN_CURRENT_ADC_GPIO         20
#define HW_RELAY_DAMPER_GPIO            47
#define HW_RS485_TX_GPIO                26
#define HW_RS485_RX_GPIO                27

/* Hardware communication settings */
#define HW_RS485_UART_NUM               1
#define HW_RS485_BAUD_RATE           4800

/* Fan current sensor conversion defaults; adjust for the fitted sensor. */
#define FAN_CURRENT_ZERO_MV             0.0F
#define FAN_CURRENT_MV_PER_AMP       1000.0F
#define FAN_CURRENT_SAMPLE_COUNT        16
#define FAN_MONITOR_START_DELAY_SECONDS 10U
#define SENSOR_COMM_FAILURE_LIMIT        5
#define FAN_SPEED_MIN_MPS              5.0F
#define FAN_SPEED_MAX_MPS             15.0F
#define OVER_HEAT_CONFIRM_SECONDS        5
#define HEATER_NO_RISE_CONFIRM_SECONDS (20U * 60U)
#define HEATER_MIN_DETECTABLE_RISE_C    0.5F

/* The DryerApp timer runs once per second. */
#define DRYER_COUNTDOWN_TICKS_PER_MINUTE 60

/* Dryer setting defaults */
#define DRYER_CFG_DEFAULT_DRY_TEMP_C                 64
#define DRYER_CFG_DEFAULT_DRY_TIME_MIN              840
#define DRYER_CFG_DEFAULT_TEMP_HYSTERESIS_C           2
#define DRYER_CFG_DEFAULT_COOLING_TEMP_C              30
#define DRYER_CFG_DEFAULT_COOLING_TIME_MIN            5
#define DRYER_CFG_DEFAULT_DAMPER_MODE                   0
#define DRYER_CFG_DEFAULT_DAMPER_OPEN_HUMIDITY_PCT    50
#define DRYER_CFG_DEFAULT_DAMPER_HYSTERESIS_PCT        5
#define DRYER_CFG_DEFAULT_HIGH_WARNING_TEMP_C         90
#define DRYER_CFG_DEFAULT_LOW_REACH_TIME_MIN         320
#define DRYER_CFG_DEFAULT_MIN_TEMP_RISE_C_PER_MIN     15
#define DRYER_CFG_DEFAULT_FAN_ADC_AT_10MS            3600
#define DRYER_CFG_DEFAULT_MQTT_PUBLISH_INTERVAL_MIN    5
#define DRYER_CFG_DEFAULT_PREHEAT_TEMP_C               50
#define DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN             30
#define DRYER_CFG_DEFAULT_STANDBY_ENABLED                1
#define DRYER_CFG_DEFAULT_STANDBY_TIME_MIN              30
#define DRYER_CFG_DEFAULT_STANDBY_TEMP_C                40
#define DRYER_CFG_DEFAULT_FAN_MIN_SPEED_MS                5

/* Dryer setting validation limits */
#define DRYER_CFG_TEMP_MIN_C                          10
#define DRYER_CFG_TEMP_MAX_C                          90
#define DRYER_CFG_TIME_MIN_MIN                         0
#define DRYER_CFG_TIME_MIN_MAX                      1440
#define DRYER_CFG_TEMP_HYSTERESIS_MIN_C                1
#define DRYER_CFG_TEMP_HYSTERESIS_MAX_C               20
#define DRYER_CFG_HUMIDITY_MIN_PCT                     0
#define DRYER_CFG_HUMIDITY_MAX_PCT                   100
#define DRYER_CFG_DAMPER_HYSTERESIS_MIN_PCT            1
#define DRYER_CFG_DAMPER_HYSTERESIS_MAX_PCT           50
#define DRYER_CFG_DAMPER_MODE_MIN                       0
#define DRYER_CFG_DAMPER_MODE_MAX                       2
#define DRYER_CFG_WARNING_TEMP_MAX_C                 120
#define DRYER_CFG_LOW_REACH_TIME_MIN                   1
#define DRYER_CFG_RISE_RATE_MIN_C_PER_MIN              1
#define DRYER_CFG_RISE_RATE_MAX_C_PER_MIN             60
#define DRYER_CFG_FAN_ADC_AT_10MS_MIN                   1
#define DRYER_CFG_FAN_ADC_AT_10MS_MAX                4096
#define DRYER_CFG_MQTT_PUBLISH_INTERVAL_MIN             1
#define DRYER_CFG_MQTT_PUBLISH_INTERVAL_MAX          1440
#define DRYER_CFG_PREHEAT_TIME_MIN_MAX                120
#define DRYER_CFG_PREHEAT_TEMP_MIN_C                    0
#define DRYER_CFG_STANDBY_TEMP_MIN_C                    0
#define DRYER_CFG_STANDBY_MODE_MIN                       0
#define DRYER_CFG_STANDBY_MODE_MAX                       2
#define DRYER_CFG_FAN_MIN_SPEED_MS_MIN                    0
#define DRYER_CFG_FAN_MIN_SPEED_MS_MAX                   15
#define EQUIPMENT_ID_MIN                            100000
#define EQUIPMENT_ID_MAX                            999999

/* Frozen camera image upload server defaults (editable in settings). */
#define IMAGE_UPLOAD_DEFAULT_IP1 192
#define IMAGE_UPLOAD_DEFAULT_IP2 168
#define IMAGE_UPLOAD_DEFAULT_IP3 0
#define IMAGE_UPLOAD_DEFAULT_IP4 41
#define IMAGE_UPLOAD_DEFAULT_PORT 8080
#define IMAGE_UPLOAD_JPEG_QUALITY 80
#define IMAGE_UPLOAD_HTTP_TIMEOUT_MS 2000
