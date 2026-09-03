#pragma once
#include "lvgl.h"
#include "MqttClient.hpp"
#include "LoadCell.hpp"
#include "Vision.hpp"
#include "DoorSensor.hpp"
#include "type_def.h"

class RS485Sensor;
extern EVENT_INFO g_alarm_info;

/* ── Dryer state machine ─────────────────────────────── */
enum DryScreen  { SCR_MAIN = 0, SCR_PROC_SELECT, SCR_CALIBRATION, SCR_WEIGHT_CALIBRATION, SCR_COOLING_SETTINGS };
enum DamperMode { DAMPER_AUTO = 0, DAMPER_OPEN, DAMPER_CLOSE };

/* ── Sensor pair (one location) ───────────────────────── */
struct SensorPair {
    float temp;  /* °C */
    float hum;   /* %  */
};

/* ── Per-process step definition ─────────────────────── */
struct DryProcess {
    const char *name;
    lv_color_t  color;
    int  n_steps;
    int  step_temp[8];
    int  step_dur[8];
    int  total_min;
    int  target_hum;
    int  hold_min;
    int  fan_min;
};

class DryerApp {
public:
    DryerApp();
    ~DryerApp();

    bool init(void);
    bool run(void);
    bool back(void);
    bool close(void);
    bool startCommunication(void);
    void setSensorSource(RS485Sensor *sensors) { _rs485_sensors = sensors; }
    void setLoadCell(LoadCell *loadCell) { _load_cell = loadCell; }
    float fanVelocity() const { return g_dryer_sensor_values.fan_velocity.velocity_ms; }
    int fanVelocityRaw() const { return g_dryer_sensor_values.fan_velocity.raw; }
    int fanVelocityReferenceAdc() const { return g_dryer_sensor_values.fan_velocity.reference_adc; }
    const DryerSensorValues &sensorValues() const { return g_dryer_sensor_values; }

    /* Hook for real hardware sensor task */
    void updateSensors(float temp_c, float hum_pct, float set_temp_c);
    void updateMachineInputs(bool door_open, float blower_speed_ms,
                             int32_t weight_g, float damper_percent);

private:
    /* ── Screen roots ────────────────────────────────── */
    lv_obj_t *_scr_main;
    lv_obj_t *_scr_proc;
    lv_obj_t *_scr_cal;
    lv_obj_t *_scr_weight_cal;
    lv_obj_t *_scr_cooling;
    lv_obj_t *_error_popup_overlay;
    lv_obj_t *_notice_popup_overlay;
    lv_timer_t *_notice_popup_timer;

    /* ── Main · header live-value labels ────────────── */
    lv_obj_t *_lbl_hdr_dry_temp;   /* CUR TEMP value        */
    lv_obj_t *_lbl_hdr_humidity;   /* HUMIDITY value        */
    lv_obj_t *_lbl_hdr_set_temp;   /* SET TEMP big display  */
    lv_obj_t *_lbl_pre_disp;       /* preTime HH:MM display */
    lv_obj_t *_lbl_rem_disp;       /* remainTime HH:MM disp */
    lv_obj_t *_canvas_pre_remain;
    lv_obj_t *_canvas_rem_time;
    lv_obj_t *_state_card;         /* transparent Korean state title */
    lv_obj_t *_lbl_state;          /* PREPARE/PREHEAT/RUN…  */
    lv_obj_t *_lbl_messenger;      /* latest server control history */

    /* ── Main · body row 1 — preheat settings ─────── */
    lv_obj_t *_lbl_pre_temp_val;   /* 예열설정온도 value */
    lv_obj_t *_btn_pre_temp_up;
    lv_obj_t *_btn_pre_temp_dn;
    lv_obj_t *_lbl_pre_time_val;   /* 예열설정시간 value */
    lv_obj_t *_btn_pre_time_up;
    lv_obj_t *_btn_pre_time_dn;
    lv_obj_t *_btn_preheat_start;  /* START/STOP toggle          */
    lv_obj_t *_lbl_btn_preheat;    /* label inside START/STOP btn */
    lv_obj_t *_btn_preheat_stop;   /* ABORT button               */
    lv_obj_t *_btn_dry_start;
    lv_obj_t *_lbl_btn_dry_start;
    lv_obj_t *_canvas_preheat_title;
    lv_obj_t *_canvas_dry_title;

    /* ── Main · body row 2 — dry settings ──────────── */
    lv_obj_t *_lbl_dry_temp_val;   /* 설정온도 value     */
    lv_obj_t *_btn_dry_temp_up;
    lv_obj_t *_btn_dry_temp_dn;
    lv_obj_t *_lbl_dry_time_val;   /* 설정시간 value     */
    lv_obj_t *_btn_dry_time_up;
    lv_obj_t *_btn_dry_time_dn;

    /* ── Main · body row 3 — sensor display + damper ─────── */
    lv_obj_t *_lbl_sen_out;    /* 외기 (외부공기) TEMP/HUM  */
    lv_obj_t *_lbl_sen_in;     /* 내부 (chamber)   TEMP/HUM  */
    lv_obj_t *_lbl_sen_ex;     /* 출구 (outlet)    TEMP/HUM  */
    lv_obj_t *_lbl_sen_4;
    lv_obj_t *_lbl_sen_5;
    lv_obj_t *_lbl_sen_6;
    lv_obj_t *_lbl_weight;       /* net weight value          */
    lv_obj_t *_lbl_weight_gross; /* gross weight value        */
    lv_obj_t *_lbl_weight_tare;  /* tare/container value      */
    lv_obj_t *_lbl_door;
    lv_obj_t *_lbl_fan_rate;
    lv_obj_t *_lbl_dryness;
    lv_obj_t *_lbl_damper;     /* DAMPER status            */
    lv_obj_t *_lbl_equipment_name;
    lv_obj_t *_btn_damper;     /* DAMPER toggle button     */

    /* ── Main · footer ───────────────────────────────── */
    lv_obj_t *_dot_heater;     /* heater LED (red=ON gray=OFF) */
    lv_obj_t *_fan_ring;       /* fixed fan outline */
    lv_obj_t *_dot_fan;        /* fan LED    (red=ON gray=OFF) */
    lv_obj_t *_dot_damper;
    lv_obj_t *_door_icon;
    lv_obj_t *_lbl_heater_status;
    lv_obj_t *_lbl_door_status;
    lv_obj_t *_lbl_server_time;
    lv_obj_t *_lbl_device_id;
    lv_obj_t *_lbl_device_ip;
    lv_obj_t *_btn_set;
    lv_obj_t *_lbl_cal_sensor;
    lv_obj_t *_lbl_cal_temp;
    lv_obj_t *_lbl_cal_hum;
    lv_obj_t *_cal_cells[7][3];
    lv_obj_t *_lbl_weight_raw;
    lv_obj_t *_lbl_weight_live;
    lv_obj_t *_lbl_weight_reference;
    lv_obj_t *_btn_weight_ref_dn;
    lv_obj_t *_btn_weight_ref_up;
    lv_obj_t *_dryer_setting_rows[DRYER_SETTING_COUNT];
    lv_obj_t *_dryer_setting_values[DRYER_SETTING_COUNT];
    MqttClient _mqtt;
    Vision _vision;
    DoorSensor _door_sensor;
    RS485Sensor *_rs485_sensors = nullptr;
    LoadCell *_load_cell = nullptr;

    /* ── Process-select screen ───────────────────────── */
    lv_obj_t *_proc_boxes[4];
    lv_obj_t *_canvas_pnames[4]; /* proc name canvas (ycb_hangul) */
    lv_obj_t *_lbl_run_mark[4];

    /* ── Static process table ────────────────────────── */
    static const DryProcess _procs[4];

    /* ── Runtime state ───────────────────────────────── */
    DryState   _dry_state;
    DryScreen  _cur_scr;
    DamperMode _damper_mode;   /* AUTO / OPEN / CLOSE          */
    int   _proc_no;
    int   _sel_proc;
    int   _remaining_min;
    int   _cool_remain;
    DryerNvsCoolingSettings _dryer_settings;
    DryerNvsCoolingSettings _dryer_settings_edit;
    int   _dryer_setting_sel;
    int   _dryer_ip_octet_sel;
    float _cur_temp;
    float _humidity;
    float _set_temp;
    int   _pre_temp;
    int   _pre_time_min;     /* preheat setting (minutes)        */
    int   _pre_time_remain;  /* preheat countdown (minutes)      */
    int   _dry_time_min;     /* dry duration setting (minutes)   */
    int   _btn_repeat_cnt;   /* long-press repeat counter        */

    /* Long-press acceleration context for time Up/Dn buttons */
    struct TimeCtx {
        DryerApp *app;
        int      *setting;   /* ptr to _pre_time_min or _dry_time_min */
        int       dir;       /* +1 or -1 */
        int       max_val;
    };
    TimeCtx _ctx_pt_up, _ctx_pt_dn, _ctx_dt_up, _ctx_dt_dn;
    bool  _fan_on;
    uint32_t _fan_on_elapsed_seconds;
    bool  _heater_on;
    bool  _standby_mode_active;
    uint32_t _heater_enable_after_ms;
    uint32_t _fan_disable_after_ms;
    bool  _fan_icon_state_valid;
    bool  _fan_icon_last_on;
    bool  _footer_icon_state_valid;
    bool  _heater_icon_last_on;
    bool  _door_icon_last_open;
    float _damper_icon_last_percent;
    bool  _colon_blink;
    bool  _time_canvas_state_valid;
    int   _time_canvas_last_pre_remain;
    int   _time_canvas_last_remaining;
    DryState _time_canvas_last_state;
    bool  _door_open;
    float _blower_speed_ms;
    float _damper_percent;
    char _equipment_name[33];
    int32_t _equipment_id;
    int   _cal_sensor;
    int   _cal_item;
    float _temp_cal[7];
    float _hum_cal[7];
    float _temp_cal_backup[7];
    float _hum_cal_backup[7];

    /* ── 3 sensor pairs: [0]=외기, [1]=내부, [2]=출구 ──────────── */
    SensorPair _sensors[6];
    int32_t    _weight_g;      /* signed weight in whole grams     */
    int32_t    _tare_weight_g; /* saved tare/container grams       */
    int        _weight_reference_g;
    LoadCellCalibration _weight_cal_backup;

    /* ── Tick counters (1-s timer) ───────────────────── */
    int   _tick_s;           /* seconds within current minute */
    uint32_t _mqtt_publish_elapsed_s;
    uint8_t _over_heat_seconds;
    uint32_t _target_reach_elapsed_seconds;
    bool _target_temperature_reached;
    uint32_t _heater_continuous_on_seconds;
    float _heater_on_start_temperature;
    bool _heater_on_start_temperature_valid;
    int   _demo_tick;        /* demo speed multiplier counter */

    /* ── 5-min history for chart ─────────────────────── */
    static const int HIST_MAX = 290;
    int _hist_cnt;
    int _tick5min;           /* counts ticks to next sample  */

    lv_timer_t *_timer;
    lv_timer_t *_fan_spin_timer;
    int32_t     _fan_spin_angle;
    uint32_t    _fan_spin_epoch_ms;
    uint8_t     _periodic_ui_refresh_phase;
    EVENT_INFO  _previous_event_info{};

    /* ── Layout build helpers ────────────────────────── */
    void buildMainScreen(void);
    void buildProcSelectScreen(void);
    void buildCalibrationScreen(void);
    void buildWeightCalibrationScreen(void);
    void buildCoolingSettingsScreen(void);
    void refreshCalibration(void);
    void refreshWeightCalibration(void);
    void refreshCoolingSettings(void);
    void adjustDryerSetting(int direction);

    static lv_obj_t *makeBtn(lv_obj_t *parent,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             const char *text, lv_color_t bg,
                             lv_event_cb_t cb = nullptr,
                             void *ud = nullptr);

    static lv_obj_t *makeCard(lv_obj_t *parent,
                              lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h,
                              lv_color_t border, const char *title,
                              lv_obj_t **val_out);

    static lv_obj_t *makeDot(lv_obj_t *parent,
                             lv_coord_t cx, lv_coord_t cy,
                             lv_coord_t d,  lv_color_t color);

    /* ── Refresh helpers ─────────────────────────────── */
    void refreshHeader(void);
    void refreshCards(void);
    void refreshFooter(void);
    void refreshProgressCard(void);
    void refreshProcSelect(void);

    /* Capture all physical sensors into one shared snapshot. */
    void updateSensorValues(void);
    void errorCheck(void);
    void processAlarmEvent(uint8_t bit, bool active);
    void showErrorPopup(const char *title, const char *line1, const char *line2);
    void showNoticePopup(const char *message);
    void applyActuatorSafetyChecks(void);
    void writeActuatorOutputs(void);
    bool processMqttCommands(void);

    /* ── Business-logic helpers ──────────────────────── */
    void doStartStop(void);
    void doProgress(void);
    void startDrying(void);
    void startDryingFromAdjustedTime(void);
    void enterStandby(void);
    void startCooling(void);
    void finishCycle(void);
    void doApplyProc(void);
    void applyProcActive(void);
    void doPreheatStart(void);
    void doPreheatStop(void);
    void doCycleDamper(void);   /* CLOSE(0%) -> AUTO -> OPEN(100%) -> CLOSE */
    void saveRuntimeState(void);
    void restoreRuntimeState(void);

    /* ── Callbacks ───────────────────────────────────── */
    static void cbTimer(lv_timer_t *t);
    static esp_err_t uploadVisionFrame(const void *rgb565, size_t size,
                                       uint32_t width, uint32_t height,
                                       void *context);
    static void cbFanSpinTimer(lv_timer_t *t);
    static void cbErrorPopupOk(lv_event_t *e);
    static void cbBtnSet(lv_event_t *e);
    static void cbLogoVision(lv_event_t *e);
    static void cbBtnPreheatStart(lv_event_t *e);
    static void cbBtnPreheatStop(lv_event_t *e);
    static void cbBtnPreTempUp(lv_event_t *e);
    static void cbBtnPreTempDn(lv_event_t *e);
    static void cbBtnPreTimeUp(lv_event_t *e);
    static void cbBtnPreTimeDn(lv_event_t *e);
    static void cbBtnDryTempUp(lv_event_t *e);
    static void cbBtnDryTempDn(lv_event_t *e);
    static void cbBtnDryTimeUp(lv_event_t *e);
    static void cbBtnDryTimeDn(lv_event_t *e);
    static void cbTimeRepeat(lv_event_t *e);  /* long-press repeat for time btns */
    static void cbAdjustRepeat(lv_event_t *e); /* accelerated repeat for all +/- buttons */
    static void cbOpenCalibration(lv_event_t *e);
    static void cbCalSensorPrev(lv_event_t *e);
    static void cbCalSensorNext(lv_event_t *e);
    static void cbCalTempDn(lv_event_t *e);
    static void cbCalTempUp(lv_event_t *e);
    static void cbCalHumDn(lv_event_t *e);
    static void cbCalHumUp(lv_event_t *e);
    static void cbCalSaveBack(lv_event_t *e);
    static void cbCalExit(lv_event_t *e);
    static void cbCalResetAll(lv_event_t *e);
    static void cbOpenWeightCalibration(lv_event_t *e);
    static void cbWeightTare(lv_event_t *e);
    static void cbWeightRefDn(lv_event_t *e);
    static void cbWeightRefUp(lv_event_t *e);
    static void cbWeightRefRepeat(lv_event_t *e);
    static void cbWeightSet(lv_event_t *e);
    static void cbWeightSaveExit(lv_event_t *e);
    static void cbWeightExit(lv_event_t *e);
    static void cbOpenCoolingSettings(lv_event_t *e);
    static void cbCoolingRowSelect(lv_event_t *e);
    static void cbCoolingValueDn(lv_event_t *e);
    static void cbCoolingValueUp(lv_event_t *e);
    static void cbCoolingIpNextOctet(lv_event_t *e);
    static void cbCoolingReset(lv_event_t *e);
    static void cbCoolingSaveExit(lv_event_t *e);
    static void cbCoolingExit(lv_event_t *e);
    static void cbBtnDamper(lv_event_t *e);
    static void cbLoadCellZero(lv_event_t *e);
    static void cbLoadCellTareReset(lv_event_t *e);
    static void cbLoadCellTare(lv_event_t *e);
    static void cbNoticePopupTimer(lv_timer_t *timer);
    static void cbProcNext(lv_event_t *e);
    static void cbProcApply(lv_event_t *e);
};
