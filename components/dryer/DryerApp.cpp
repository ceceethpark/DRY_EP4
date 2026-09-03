#include "DryerApp.hpp"
#include "DryerNvsStore.hpp"
#include "RS485Sensor.hpp"
#include "Buzzer.hpp"
#include "Relay.hpp"
#include "Fan.hpp"
#include "ycb_7seg.h"
#include "ycb_hangul.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/jpeg_encode.h"
#include "config.h"
#include "hanwool_logo.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cinttypes>
#include <limits>

static DryerNvsStore s_nvs;
static Relay s_relay;
static Fan s_fan;

namespace {
void urlEncodeHeader(const char *input, char *output, size_t outputSize)
{
    static constexpr char hex[]="0123456789ABCDEF";
    size_t out=0;
    for(const unsigned char *p=reinterpret_cast<const unsigned char*>(input);*p&&out+1<outputSize;++p){
        const bool safe=(*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'||*p=='.';
        if(safe)output[out++]=static_cast<char>(*p);
        else if(out+3<outputSize){output[out++]='%';output[out++]=hex[*p>>4];output[out++]=hex[*p&0x0F];}
        else break;
    }
    output[out]='\0';
}
}
DryerSensorValues g_dryer_sensor_values{};
EVENT_INFO g_alarm_info{};

static void setLabelTextIfChanged(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;
    const char *current = lv_label_get_text(label);
    if (!current || std::strcmp(current, text) != 0)
        lv_label_set_text(label, text);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Canvas pixel buffers (file-scope static → persist for app lifetime)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* (timer canvas replaced by plain background panel + label cards) */

/* ── Row label canvases (한글 2글자, scale=2: 64×32px + margin) ── */
#define KOR_ROW_LBL_W  72
#define KOR_ROW_LBL_H  40
static lv_color_t s_row_lbl_buf[3][KOR_ROW_LBL_W * KOR_ROW_LBL_H];

/* ── 예열 버튼 한글 캐린버스 ("예열대기"/"예열중" 4/3글자 scale=2) ── */
#define KOR_BTN_PRE_W   (4 * 16 * 2 + 10)   /* 138 px, 4글자 기준 */
#define KOR_BTN_PRE_H   (16 * 2 + 10)        /*  42 px */
static lv_color_t s_btn_preheat_buf[KOR_BTN_PRE_W * KOR_BTN_PRE_H];
#define C_PRE_BTN_IDLE lv_color_hex(0x1A3A1A)
#define C_PRE_BTN_ACT  lv_color_hex(0x3A2000)

/* ── Heater / Fan icon canvases (40×40 px) ──────────────────── */
#define ICON_SZ  64
static lv_color_t s_icon_heater_buf[ICON_SZ * ICON_SZ];
#define FAN_ICON_SZ 52
static lv_color_t s_icon_fan_buf   [FAN_ICON_SZ * FAN_ICON_SZ];
static lv_color_t s_icon_damper_buf[ICON_SZ * ICON_SZ];
static lv_color_t s_damper_status_buf[118 * 24];
#define DOOR_ICON_SZ 64
static lv_color_t s_door_icon_buf[DOOR_ICON_SZ * DOOR_ICON_SZ];
#define PROGRESS_ICON_SZ 120
static lv_color_t s_progress_icon_buf[PROGRESS_ICON_SZ * PROGRESS_ICON_SZ];
#define TIME_7SEG_W 220
#define TIME_7SEG_H 60
static lv_color_t s_set_temp_7seg_buf[TIME_7SEG_W * TIME_7SEG_H];
static lv_color_t s_pre_remain_7seg_buf[TIME_7SEG_W * TIME_7SEG_H];
static lv_color_t s_remain_7seg_buf[TIME_7SEG_W * TIME_7SEG_H];

static void drawTime7Seg(lv_obj_t *canvas, int totalMin, lv_color_t fg,
                         lv_color_t bg, bool colonOn)
{
    if (!canvas) return;
    if (totalMin < 0) totalMin = 0;
    const int hours = (totalMin / 60) % 100;
    const int mins = totalMin % 60;
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    /* ycb digits include trailing cell spacing; +5 centers the visible segments. */
    ycb_7seg_draw(canvas, hours, 30, 5, 3, fg, bg, 2, 0);
    ycb_7seg_draw(canvas, mins, 124, 5, 3, fg, bg, 2, 0);
    lv_draw_rect_dsc_t dot; lv_draw_rect_dsc_init(&dot);
    dot.bg_color = colonOn ? fg : bg; dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(canvas, 107, 18, 6, 6, &dot);
    lv_canvas_draw_rect(canvas, 107, 37, 6, 6, &dot);
}

static void drawTemperature7Seg(lv_obj_t *canvas, int temperature,
                                lv_color_t fg, lv_color_t bg)
{
    if (!canvas) return;
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);

    const int digits = (temperature <= -10 || temperature >= 100) ? 3 : 2;
    const int digit_width = ycb_7seg_width(3, digits);
    const int unit_width = 48;
    const int gap = 8;
    int x = (TIME_7SEG_W - digit_width - gap - unit_width) / 2;
    if (x < 0) x = 0;
    ycb_7seg_draw(canvas, temperature, x, 5, 3, fg, bg, digits, 0);

    const int unit_x = x + digit_width + gap;
    lv_draw_arc_dsc_t degree;
    lv_draw_arc_dsc_init(&degree);
    degree.color = fg;
    degree.opa = LV_OPA_COVER;
    degree.width = 3;
    degree.rounded = 1;
    lv_canvas_draw_arc(canvas, unit_x + 7, 13, 6, 0, 360, &degree);

    lv_draw_line_dsc_t c_line;
    lv_draw_line_dsc_init(&c_line);
    c_line.color = fg;
    c_line.opa = LV_OPA_COVER;
    c_line.width = 6;
    c_line.round_start = 1;
    c_line.round_end = 1;
    const lv_point_t c_shape[] = {
        {static_cast<lv_coord_t>(unit_x + 44), 12},
        {static_cast<lv_coord_t>(unit_x + 22), 12},
        {static_cast<lv_coord_t>(unit_x + 22), 48},
        {static_cast<lv_coord_t>(unit_x + 44), 48}
    };
    lv_canvas_draw_line(canvas, c_shape, sizeof(c_shape) / sizeof(c_shape[0]), &c_line);
}

/* Heater icon: two vertical spring coils, shaped like "11". */
static void drawHeaterIcon(lv_obj_t *cv, lv_color_t col, lv_color_t bg)
{
    lv_canvas_fill_bg(cv, bg, LV_OPA_COVER);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = col;
    line.opa = LV_OPA_COVER;
    line.width = 6;
    line.round_start = 1;
    line.round_end = 1;
    const lv_point_t left_coil[] = {
        {19, 4}, {12, 11}, {26, 18}, {12, 25},
        {26, 32}, {12, 39}, {26, 46}, {19, 59}
    };
    const lv_point_t right_coil[] = {
        {45, 4}, {38, 11}, {52, 18}, {38, 25},
        {52, 32}, {38, 39}, {52, 46}, {45, 59}
    };
    lv_canvas_draw_line(cv, left_coil,
                        sizeof(left_coil) / sizeof(left_coil[0]), &line);
    lv_canvas_draw_line(cv, right_coil,
                        sizeof(right_coil) / sizeof(right_coil[0]), &line);
}

/* Fan icon: 3-blade propeller inside circle */
static void drawFanIcon(lv_obj_t *cv, lv_color_t col, lv_color_t bg,
                        float rotation_deg = 0.0F)
{
    (void)bg;
    /* The canvas contains only transparent rotating blades. The outline is a
       separate fixed object, so neither the square background nor ring rotates. */
    lv_canvas_fill_bg(cv, lv_color_chroma_key(), LV_OPA_COVER);
    /* Three identical swept blades, exactly 120 degrees apart. */
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = col;  r.bg_opa = LV_OPA_COVER;  r.radius = 2;
    constexpr float pi = 3.14159265358979323846f;
    for (int blade = 0; blade < 3; ++blade) {
        const float base = (-90.0f + rotation_deg + 120.0f * blade) * pi / 180.0f;
        const float angle_offset[4] = {-28.0f, -38.0f, 12.0f, 32.0f};
        const float radius[4] = {6.0f, 20.0f, 21.0f, 8.0f};
        lv_point_t p[4];
        for (int i = 0; i < 4; ++i) {
            const float a_blade = base + angle_offset[i] * pi / 180.0f;
            p[i].x = static_cast<lv_coord_t>(std::lround(26.0f + radius[i] * std::cos(a_blade)));
            p[i].y = static_cast<lv_coord_t>(std::lround(26.0f + radius[i] * std::sin(a_blade)));
        }
        lv_canvas_draw_polygon(cv, p, 4, &r);
    }
    /* Center hub */
    r.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(cv, 20, 20, 12, 12, &r);
}

static void drawDamperIcon(lv_obj_t *cv, float percent, lv_color_t col, lv_color_t bg)
{
    lv_canvas_fill_bg(cv, bg, LV_OPA_COVER);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER; r.radius = 0; r.bg_color = lv_color_hex(0x607080);
    lv_canvas_draw_rect(cv, 5, 7, 54, 50, &r);
    r.bg_color = bg; lv_canvas_draw_rect(cv, 10, 12, 44, 40, &r);
    /* Fixed vertical blade: closed is thick, open is thin, center stays fixed. */
    lv_point_t blade[2] = {
        {32, 16}, {32, 48}
    };
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
    line.color = col; line.opa = LV_OPA_COVER;
    line.width = static_cast<lv_coord_t>(44.0f - 40.0f * percent / 100.0f);
    line.round_start = 0; line.round_end = 0;
    lv_canvas_draw_line(cv, blade, 2, &line);
}

static void drawDoorIcon(lv_obj_t *cv, bool open, lv_color_t col, lv_color_t bg)
{
    lv_canvas_fill_bg(cv, bg, LV_OPA_COVER);
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER; r.radius = 0; r.bg_color = col;
    lv_canvas_draw_rect(cv, 10, 5, 44, 54, &r);
    r.bg_color = bg; lv_canvas_draw_rect(cv, 16, 11, 32, 42, &r);
    r.bg_color = col;
    if (open) {
        lv_canvas_draw_rect(cv, 20, 12, 18, 40, &r);
        lv_canvas_draw_rect(cv, 40, 30, 7, 5, &r);
    } else {
        lv_canvas_draw_rect(cv, 42, 30, 6, 5, &r);
    }
}

static void drawDamperStatus(lv_obj_t *canvas, DamperMode mode, float percent)
{
    if (!canvas) return;
    const char *text;
    lv_color_t color;
    if (mode == DAMPER_AUTO) {
        text = "자동";
        color = lv_color_hex(0x00FFFF);
    } else if (percent >= 99.5F) {
        text = "100%";
        color = lv_color_hex(0x07E000);
    } else {
        text = "0%";
        color = lv_color_hex(0xF44336);
    }
    const lv_color_t background = lv_color_hex(0x152238);
    lv_canvas_fill_bg(canvas, background, LV_OPA_COVER);
    int x = (118 - ycb_hangul_measure(text, 1)) / 2;
    if (x < 0) x = 0;
    ycb_hangul_draw(canvas, x, 4, color, background, text, 1, false);
}

/* Large, high-contrast icons used by the main operating-state card. */
static void drawProgressStateIcon(lv_obj_t *cv, DryState state)
{
    if (!cv) return;
    lv_canvas_fill_bg(cv, LV_COLOR_CHROMA_KEY, LV_OPA_COVER);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_white();
    line.opa = LV_OPA_COVER;
    line.width = 8;
    line.round_start = 1;
    line.round_end = 1;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_white();
    arc.opa = LV_OPA_COVER;
    arc.width = 7;
    arc.rounded = 1;

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_color_white();
    fill.bg_opa = LV_OPA_COVER;
    fill.radius = 3;

    switch (state) {
    case DRY_PREHEAT: {
        const lv_point_t heat1[] = {{28, 92}, {20, 78}, {36, 64}, {20, 49}, {34, 32}, {28, 18}};
        const lv_point_t heat2[] = {{60, 98}, {50, 82}, {69, 65}, {50, 47}, {67, 29}, {60, 12}};
        const lv_point_t heat3[] = {{92, 92}, {84, 78}, {100, 64}, {84, 49}, {98, 32}, {92, 18}};
        lv_canvas_draw_line(cv, heat1, sizeof(heat1) / sizeof(heat1[0]), &line);
        lv_canvas_draw_line(cv, heat2, sizeof(heat2) / sizeof(heat2[0]), &line);
        lv_canvas_draw_line(cv, heat3, sizeof(heat3) / sizeof(heat3[0]), &line);
        break;
    }
    case DRY_PREPARE: {
        const lv_point_t upper[] = {{24, 18}, {96, 18}, {60, 57}, {24, 18}};
        const lv_point_t lower[] = {{24, 102}, {60, 63}, {96, 102}, {24, 102}};
        lv_canvas_draw_line(cv, upper, sizeof(upper) / sizeof(upper[0]), &line);
        lv_canvas_draw_line(cv, lower, sizeof(lower) / sizeof(lower[0]), &line);
        lv_canvas_draw_rect(cv, 18, 10, 84, 8, &fill);
        lv_canvas_draw_rect(cv, 18, 102, 84, 8, &fill);
        break;
    }
    case DRY_RUN: {
        lv_canvas_draw_arc(cv, 60, 60, 48, 0, 360, &arc);
        const lv_point_t play[] = {{48, 34}, {48, 86}, {86, 60}};
        lv_canvas_draw_polygon(cv, play, sizeof(play) / sizeof(play[0]), &fill);
        break;
    }
    case DRY_COOL: {
        const lv_point_t vertical[] = {{60, 10}, {60, 110}};
        const lv_point_t diag1[] = {{17, 35}, {103, 85}};
        const lv_point_t diag2[] = {{17, 85}, {103, 35}};
        lv_canvas_draw_line(cv, vertical, 2, &line);
        lv_canvas_draw_line(cv, diag1, 2, &line);
        lv_canvas_draw_line(cv, diag2, 2, &line);
        const lv_point_t branches[][2] = {
            {{45, 20}, {60, 34}}, {{75, 20}, {60, 34}},
            {{45, 100}, {60, 86}}, {{75, 100}, {60, 86}},
            {{19, 53}, {39, 48}}, {{29, 35}, {39, 48}},
            {{101, 67}, {81, 72}}, {{91, 85}, {81, 72}},
            {{19, 67}, {39, 72}}, {{29, 85}, {39, 72}},
            {{101, 53}, {81, 48}}, {{91, 35}, {81, 48}}
        };
        for (const auto &branch : branches) lv_canvas_draw_line(cv, branch, 2, &line);
        break;
    }
    case DRY_FINISH: {
        lv_canvas_draw_arc(cv, 60, 60, 48, 0, 360, &arc);
        const lv_point_t check[] = {{32, 61}, {52, 82}, {91, 38}};
        line.width = 10;
        lv_canvas_draw_line(cv, check, sizeof(check) / sizeof(check[0]), &line);
        break;
    }
    }
}

/* ── Process title canvas (한글) ── */
#define KOR_PROC_SCALE  3
#define KOR_PROC_W      (16 * 4 * KOR_PROC_SCALE + 4)  /* max 4 chars */
#define KOR_PROC_H      (16 * KOR_PROC_SCALE + 4)
static lv_color_t s_proc_title_buf[KOR_PROC_W * KOR_PROC_H];

/* ── Header proc-card name canvas (한글 2×2 = 4글자 scale=2) ── */
#define KOR_HDR_PROC_W  70   /* 2chars×32px + margin */
#define KOR_HDR_PROC_H  72   /* 2lines×32px + 8margin */
static lv_color_t s_hdr_proc_buf[KOR_HDR_PROC_W * KOR_HDR_PROC_H];

/* ── Proc-select box name canvases (한글 4글자 scale=1 = 64px×16px) ── */
#define KOR_SEL_PROC_W  70   /* 4chars×16px + margin */
#define KOR_SEL_PROC_H  22
static lv_color_t s_sel_proc_buf[4][KOR_SEL_PROC_W * KOR_SEL_PROC_H];

#define DRYER_SET_NAME_W 440
#define DRYER_SET_NAME_H 40
#define DRYER_SET_TITLE_W 220
#define DRYER_SET_TITLE_H 38
#define DRYER_SET_BTN_W 260
#define DRYER_SET_BTN_H 36
static lv_color_t *s_dryer_set_name_buf[DRYER_SETTING_COUNT];
static lv_color_t *s_dryer_set_title_buf;
static lv_color_t *s_dryer_set_btn_buf[7];
static lv_color_t *s_dryer_set_unit_buf[4];
static lv_color_t *s_main_set_temp_title_buf[3];
static lv_color_t *s_error_title_buf;
static lv_color_t *s_error_line1_buf;
static lv_color_t *s_error_line2_buf;
static lv_color_t *s_error_ok_buf;
static lv_color_t *s_notice_message_buf;
static lv_color_t *s_equipment_name_buf;
static lv_color_t *allocCanvasBuffer(size_t pixels)
{
    return static_cast<lv_color_t*>(heap_caps_calloc(pixels,sizeof(lv_color_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
}

static int utf8GlyphCount(const char *text)
{
    int count=0;
    for(const unsigned char*p=reinterpret_cast<const unsigned char*>(text);*p;p++)
        if((*p&0xC0)!=0x80)count++;
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Icon declaration
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Colour palette
 * ═══════════════════════════════════════════════════════════════════════════ */
#define C_BG        lv_color_hex(0x0D1B2A)
#define C_PANEL     lv_color_hex(0x152238)
#define C_CARD      lv_color_hex(0x0A0F16)   /* card body = near-black      */
#define C_BORDER    lv_color_hex(0x2A3F58)
#define C_WHITE     lv_color_hex(0xF0F4F8)
#define C_GRAY      lv_color_hex(0x607080)
#define C_DARKGRAY  lv_color_hex(0x2A3040)
#define C_RED       lv_color_hex(0xF44336)
#define C_GREEN     lv_color_hex(0x07E000)
#define C_BLUE      lv_color_hex(0x0000F8)
#define C_ORANGE    lv_color_hex(0xFF9800)
#define C_CYAN      lv_color_hex(0x00FFFF)
#define C_YELLOW    lv_color_hex(0xFFFF00)
#define C_DARK_BTN  lv_color_hex(0x1C2E44)
#define C_LIGHTGRAY lv_color_hex(0xC6C3C6)

/* ═══════════════════════════════════════════════════════════════════════════
 *  Layout constants  (screen 1024 × 600, from dryer_ui_layout.json)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define SB_H               0   /* status bar removed                      */

/* Header (높이 150px 로 확장) */
#define HDR_Y              0
#define HDR_H            150   /* was 110 */

/* Header: CUR TEMP / HUMIDITY 카드 제거됨 */
/* HDR_DRY_TEMP, HDR_HUM 콜렉션 상수는 유지(콜에서 탄조제없음) */
#define HDR_DRY_TEMP_X    10
#define HDR_DRY_TEMP_Y    70
#define HDR_DRY_TEMP_W   120
#define HDR_DRY_TEMP_H    70
#define HDR_HUM_X        140
#define HDR_HUM_Y         70
#define HDR_HUM_W        130
#define HDR_HUM_H         70

/* SET TEMP 카드 + Up/Dn (왜쪽 첫번째) */
#define HDR_STEMP_X        10
#define HDR_STEMP_Y        62
#define HDR_STEMP_W       130
#define HDR_STEMP_H       120
/* Up: x:148 y:62 w:80 h:55  /  Dn: x:148 y:125 w:80 h:55 */

/* Timer background (no longer used, removed) */
#define TIMER_X          330
#define TIMER_Y           60
#define TIMER_W          520
#define TIMER_H           90

#define PRE_DISP_X       245   /* preTime card  */
#define PRE_DISP_Y        62
#define PRE_DISP_W       155
#define PRE_DISP_H       120

#define REM_DISP_X       415   /* remainTime 큰 카드 (x:415~930) */
#define REM_DISP_Y        52
#define REM_DISP_W       510
#define REM_DISP_H       145   /* 높이는 헤더 h:150 안에 맞춰 */

#define STATE_CARD_X     880
#define STATE_CARD_Y      70
#define STATE_CARD_W     116
#define STATE_CARD_H      70

/* Preheat accent colour (#C8C800) */
#define C_PREHEAT_COL    lv_color_hex(0xC8C800)

/* Body */
#define BODY_Y           205   /* header ends at y:200 + 5px margin */

/* Body row 1 — preheat (large layout, y:205 after taller header) */
#define PRE_TEMP_X         10
#define PRE_TEMP_Y        210
#define PRE_TEMP_W        200
#define PRE_TEMP_H        130
#define PRE_TEMP_BTN_X    218
#define PRE_TEMP_UP_Y     210
#define PRE_TEMP_DN_Y     276   /* 210+55+11 */

#define PRE_TIME_X        356
#define PRE_TIME_Y        210
#define PRE_TIME_W        200
#define PRE_TIME_H        130
#define PRE_TIME_BTN_X    564
#define PRE_TIME_UP_Y     210
#define PRE_TIME_DN_Y     276

#define BTN_PH_START_X    700
#define BTN_PH_START_Y    210
#define BTN_PH_START_W    310
#define BTN_PH_START_H    130

/* Body row 2 — dry: moved to header; constants kept for ctx only */
#define DRY_TEMP_X        170
#define DRY_TEMP_Y        300
#define DRY_TEMP_W        160
#define DRY_TEMP_H         90
#define DRY_TEMP_BTN_X    340
#define DRY_TEMP_UP_Y     300
#define DRY_TEMP_DN_Y     350
#define DRY_TIME_X        480
#define DRY_TIME_Y        300
#define DRY_TIME_W        170
#define DRY_TIME_H         90
#define DRY_TIME_BTN_X    660
#define DRY_TIME_UP_Y     300
#define DRY_TIME_DN_Y     350

/* Body row 2 (sensor): h 동일하게 예열행과 동일 h:130 */
#define STAT_CARD_Y       360
#define STAT_CARD_H       130   /* == PRE_TEMP_H */
#define STAT_W            160
#define STAT1_X             5
#define STAT2_X           219
#define STAT3_X           433
#define STAT4_X           647
#define STAT5_X           861

/* Row label column — removed (no row labels in new layout) */

/* Footer */
#define FTR_Y             530
#define FTR_H              70
#define DOT_HTR_X          70
#define DOT_FAN_X         150
#define DOT_LED_Y         555
#define DOT_LED_D          30
#define BTN_SET_X         820
#define BTN_SET_Y         540
#define BTN_SET_W         190
#define BTN_SET_H          50

/* Shared Up/Down button size (larger for new layout) */
#define UPDN_W            120
#define UPDN_H             55

/* ═══════════════════════════════════════════════════════════════════════════
 *  Static process table
 * ═══════════════════════════════════════════════════════════════════════════ */
const DryProcess DryerApp::_procs[4] = {
    /* 0: 표준건조 GREEN */
    { "표준건조", {}, 1,
      {64,  0,  0,  0,  0,  0,  0,  0},
      {840, 0,  0,  0,  0,  0,  0,  0}, 840, 12, 5, 5 },
    /* 1: 저온건조 BLUE */
    { "저온건조", {}, 2,
      {60, 55,  0,  0,  0,  0,  0,  0},
      {120,840, 0,  0,  0,  0,  0,  0}, 960, 12, 5, 5 },
    /* 2: 고온건조 RED */
    { "고온건조", {}, 1,
      {70,  0,  0,  0,  0,  0,  0,  0},
      {840, 0,  0,  0,  0,  0,  0,  0}, 840, 12, 5, 5 },
    /* 3: 맞춤건조 ORANGE */
    { "맞춤건조", {}, 6,
      {50, 64, 68, 68, 50, 45,  0,  0},
      {120,120,120,120,120,120,  0,  0}, 720, 12, 5, 5 },
};

/* ── Process colour helper (can't initialise lv_color_t in struct literal) ── */
static lv_color_t procColor(int no) {
    switch (no) {
        case 0: return C_GREEN;
        case 1: return C_BLUE;
        case 2: return C_RED;
        case 3: return C_ORANGE;
        default: return C_GRAY;
    }
}

static const char *stateStr(DryState s) {
    switch (s) {
        case DRY_PREPARE: return "PREPARE";
        case DRY_PREHEAT: return "PREHEAT";
        case DRY_RUN:     return "RUN";
        case DRY_COOL:    return "COOL";
        case DRY_FINISH:  return "FINISH";
    }
    return "---";
}

static const char *stateKor(DryState s) {
    switch (s) {
        case DRY_PREHEAT: return "\xec\x98\x88\xec\x97\xb4"; /* 예열 */
        case DRY_PREPARE: return "\xeb\x8c\x80\xea\xb8\xb0"; /* 대기 */
        case DRY_RUN:     return "\xea\xb1\xb4\xec\xa1\xb0"; /* 건조 */
        case DRY_COOL:    return "\xec\xbf\xa8\xeb\xa7\x81"; /* 쿨링 */
        case DRY_FINISH:  return "\xec\x99\x84\xeb\xa3\x8c"; /* 완료 */
    }
    return "--";
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constructor / Destructor
 * ═══════════════════════════════════════════════════════════════════════════ */
DryerApp::DryerApp()
    : _scr_main(nullptr), _scr_proc(nullptr), _scr_cal(nullptr), _scr_weight_cal(nullptr),
      _scr_cooling(nullptr), _error_popup_overlay(nullptr),
      _notice_popup_overlay(nullptr), _notice_popup_timer(nullptr),
      _lbl_hdr_dry_temp(nullptr), _lbl_hdr_humidity(nullptr), _lbl_hdr_set_temp(nullptr),
      _lbl_pre_disp(nullptr), _lbl_rem_disp(nullptr),
      _canvas_pre_remain(nullptr), _canvas_rem_time(nullptr),
      _state_card(nullptr), _lbl_state(nullptr), _lbl_messenger(nullptr),
      _lbl_pre_temp_val(nullptr), _btn_pre_temp_up(nullptr), _btn_pre_temp_dn(nullptr),
      _lbl_pre_time_val(nullptr), _btn_pre_time_up(nullptr), _btn_pre_time_dn(nullptr),
      _btn_preheat_start(nullptr), _lbl_btn_preheat(nullptr), _btn_preheat_stop(nullptr),
      _btn_dry_start(nullptr), _lbl_btn_dry_start(nullptr),
      _canvas_preheat_title(nullptr), _canvas_dry_title(nullptr),
      _lbl_dry_temp_val(nullptr), _btn_dry_temp_up(nullptr), _btn_dry_temp_dn(nullptr),
      _lbl_dry_time_val(nullptr), _btn_dry_time_up(nullptr), _btn_dry_time_dn(nullptr),
      _lbl_sen_out(nullptr), _lbl_sen_in(nullptr), _lbl_sen_ex(nullptr),
      _lbl_sen_4(nullptr), _lbl_sen_5(nullptr), _lbl_sen_6(nullptr),
      _lbl_weight(nullptr), _lbl_weight_gross(nullptr), _lbl_weight_tare(nullptr),
      _lbl_door(nullptr), _lbl_fan_rate(nullptr), _lbl_dryness(nullptr),
      _lbl_damper(nullptr), _lbl_equipment_name(nullptr), _btn_damper(nullptr),
      _dot_heater(nullptr), _fan_ring(nullptr), _dot_fan(nullptr), _dot_damper(nullptr), _door_icon(nullptr),
      _lbl_heater_status(nullptr), _lbl_door_status(nullptr),
      _lbl_server_time(nullptr), _lbl_device_id(nullptr),
      _lbl_device_ip(nullptr),
      _btn_set(nullptr), _lbl_cal_sensor(nullptr), _lbl_cal_temp(nullptr), _lbl_cal_hum(nullptr),
      _lbl_weight_raw(nullptr), _lbl_weight_live(nullptr), _lbl_weight_reference(nullptr),
      _btn_weight_ref_dn(nullptr), _btn_weight_ref_up(nullptr),
      _dry_state(DRY_FINISH), _cur_scr(SCR_MAIN),
      _damper_mode(DAMPER_AUTO),
      _proc_no(0), _sel_proc(0),
      _remaining_min(0), _cool_remain(0),
      _dryer_settings{DRYER_CFG_DEFAULT_DRY_TEMP_C,DRYER_CFG_DEFAULT_DRY_TIME_MIN,DRYER_CFG_DEFAULT_TEMP_HYSTERESIS_C,DRYER_CFG_DEFAULT_COOLING_TEMP_C,DRYER_CFG_DEFAULT_COOLING_TIME_MIN,DRYER_CFG_DEFAULT_DAMPER_MODE,DRYER_CFG_DEFAULT_DAMPER_OPEN_HUMIDITY_PCT,DRYER_CFG_DEFAULT_DAMPER_HYSTERESIS_PCT,DRYER_CFG_DEFAULT_HIGH_WARNING_TEMP_C,DRYER_CFG_DEFAULT_LOW_REACH_TIME_MIN,DRYER_CFG_DEFAULT_MIN_TEMP_RISE_C_PER_MIN,DRYER_CFG_DEFAULT_FAN_ADC_AT_10MS,DRYER_CFG_DEFAULT_MQTT_PUBLISH_INTERVAL_MIN,IMAGE_UPLOAD_DEFAULT_IP1,IMAGE_UPLOAD_DEFAULT_IP2,IMAGE_UPLOAD_DEFAULT_IP3,IMAGE_UPLOAD_DEFAULT_IP4,IMAGE_UPLOAD_DEFAULT_PORT,DRYER_CFG_DEFAULT_PREHEAT_TEMP_C,DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_ENABLED,DRYER_CFG_DEFAULT_STANDBY_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_TEMP_C,DRYER_CFG_DEFAULT_FAN_MIN_SPEED_MS},
      _dryer_settings_edit{DRYER_CFG_DEFAULT_DRY_TEMP_C,DRYER_CFG_DEFAULT_DRY_TIME_MIN,DRYER_CFG_DEFAULT_TEMP_HYSTERESIS_C,DRYER_CFG_DEFAULT_COOLING_TEMP_C,DRYER_CFG_DEFAULT_COOLING_TIME_MIN,DRYER_CFG_DEFAULT_DAMPER_MODE,DRYER_CFG_DEFAULT_DAMPER_OPEN_HUMIDITY_PCT,DRYER_CFG_DEFAULT_DAMPER_HYSTERESIS_PCT,DRYER_CFG_DEFAULT_HIGH_WARNING_TEMP_C,DRYER_CFG_DEFAULT_LOW_REACH_TIME_MIN,DRYER_CFG_DEFAULT_MIN_TEMP_RISE_C_PER_MIN,DRYER_CFG_DEFAULT_FAN_ADC_AT_10MS,DRYER_CFG_DEFAULT_MQTT_PUBLISH_INTERVAL_MIN,IMAGE_UPLOAD_DEFAULT_IP1,IMAGE_UPLOAD_DEFAULT_IP2,IMAGE_UPLOAD_DEFAULT_IP3,IMAGE_UPLOAD_DEFAULT_IP4,IMAGE_UPLOAD_DEFAULT_PORT,DRYER_CFG_DEFAULT_PREHEAT_TEMP_C,DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_ENABLED,DRYER_CFG_DEFAULT_STANDBY_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_TEMP_C,DRYER_CFG_DEFAULT_FAN_MIN_SPEED_MS}, _dryer_setting_sel(0), _dryer_ip_octet_sel(0),
      _cur_temp(25.0f), _humidity(65.0f), _set_temp(64.0f),
      _pre_temp(DRYER_CFG_DEFAULT_PREHEAT_TEMP_C), _pre_time_min(DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN), _pre_time_remain(0), _dry_time_min(DRYER_CFG_DEFAULT_DRY_TIME_MIN),
      _btn_repeat_cnt(0),
      _fan_on(false), _fan_on_elapsed_seconds(0), _heater_on(false), _standby_mode_active(false),
      _heater_enable_after_ms(0), _fan_disable_after_ms(0),
      _fan_icon_state_valid(false), _fan_icon_last_on(false),
      _footer_icon_state_valid(false), _heater_icon_last_on(false),
      _door_icon_last_open(false), _damper_icon_last_percent(-1.0F),
      _colon_blink(false), _time_canvas_state_valid(false),
      _time_canvas_last_pre_remain(-1), _time_canvas_last_remaining(-1),
      _time_canvas_last_state(DRY_FINISH),
      _door_open(false), _blower_speed_ms(0.0f), _damper_percent(0.0f), _equipment_name{"DY-EP4"}, _equipment_id(0), _cal_sensor(0), _cal_item(0),
      _weight_g(0), _tare_weight_g(0), _weight_reference_g(1000), _weight_cal_backup{},
      _tick_s(0), _mqtt_publish_elapsed_s(0), _over_heat_seconds(0),
      _target_reach_elapsed_seconds(0), _target_temperature_reached(false),
      _heater_continuous_on_seconds(0), _heater_on_start_temperature(0.0F),
      _heater_on_start_temperature_valid(false), _demo_tick(0),
      _hist_cnt(0), _tick5min(0),
      _timer(nullptr), _fan_spin_timer(nullptr), _fan_spin_angle(30),
      _fan_spin_epoch_ms(0), _periodic_ui_refresh_phase(0)
{
    memset(_proc_boxes,    0, sizeof(_proc_boxes));
    memset(_canvas_pnames, 0, sizeof(_canvas_pnames));
    memset(_lbl_run_mark,  0, sizeof(_lbl_run_mark));
    memset(_dryer_setting_rows,0,sizeof(_dryer_setting_rows));
    memset(_dryer_setting_values,0,sizeof(_dryer_setting_values));
}

DryerApp::~DryerApp() {}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Static UI helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
lv_obj_t *DryerApp::makeBtn(lv_obj_t *parent,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             const char *text, lv_color_t bg,
                             lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_color(btn, lv_color_mix(bg, lv_color_black(), 180), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, C_BORDER, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, C_WHITE, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}

lv_obj_t *DryerApp::makeDot(lv_obj_t *parent,
                             lv_coord_t cx, lv_coord_t cy,
                             lv_coord_t d,  lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, d, d);
    lv_obj_set_pos(dot, cx - d/2, cy - d/2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  makeCard — card with title + value label
 * ═══════════════════════════════════════════════════════════════════════════ */
lv_obj_t *DryerApp::makeCard(lv_obj_t *parent,
                              lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h,
                              lv_color_t border, const char *title,
                              lv_obj_t **val_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, border, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title && title[0]) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, border, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);
    }

    if (val_out) {
        lv_obj_t *v = lv_label_create(card);
        lv_label_set_text(v, "\xe2\x80\x94");
        lv_obj_set_style_text_color(v, C_WHITE, 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
        lv_obj_align(v, LV_ALIGN_CENTER, 0, 8);
        *val_out = v;
    }
    return card;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Build — Main screen  (layout: dryer_ui_layout.json)
 * ═══════════════════════════════════════════════════════════════════════════ */
#if 0
void DryerApp::buildMainScreen(void)
{
    _scr_main = lv_obj_create(nullptr);
    lv_obj_set_size(_scr_main, 1024, 600);
    lv_obj_set_style_bg_color(_scr_main, C_BG, 0);
    lv_obj_set_style_bg_opa(_scr_main, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scr_main, 0, 0);
    lv_obj_set_style_pad_all(_scr_main, 0, 0);
    lv_obj_clear_flag(_scr_main, LV_OBJ_FLAG_SCROLLABLE);

    /* ── HEADER background panel (y:50, h:150) ───────────────────── */
    lv_obj_t *hdr = lv_obj_create(_scr_main);
    lv_obj_set_pos(hdr, 0, HDR_Y);
    lv_obj_set_size(hdr, 1024, HDR_H);
    lv_obj_set_style_bg_color(hdr, C_PANEL, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hdr, C_BORDER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* CUR TEMP / HUMIDITY 카드 제거됨 */

    /* SET TEMP 카드 (x:10 y:62 w:130 h:120) + Up/Dn */
    makeCard(_scr_main, HDR_STEMP_X, HDR_STEMP_Y,
             HDR_STEMP_W, HDR_STEMP_H,
             C_ORANGE, "SET" "\xC2\xB0""C", &_lbl_hdr_set_temp);
    _btn_dry_temp_up = makeBtn(_scr_main, 148, 62, 80, 55,
                               LV_SYMBOL_UP " Up",
                               lv_color_hex(0x1A3A1A), cbBtnDryTempUp, this);
    lv_obj_set_style_border_color(_btn_dry_temp_up, C_ORANGE, 0);
    _btn_dry_temp_dn = makeBtn(_scr_main, 148, 125, 80, 55,
                               LV_SYMBOL_DOWN " Dn",
                               lv_color_hex(0x1A3A1A), cbBtnDryTempDn, this);
    lv_obj_set_style_border_color(_btn_dry_temp_dn, C_ORANGE, 0);

    /* preTtime card (x:245 y:62 w:155 h:120, border:C8C800, fs:32) */
    {
        lv_obj_t *c = lv_obj_create(_scr_main);
        lv_obj_set_pos(c, PRE_DISP_X, PRE_DISP_Y);
        lv_obj_set_size(c, PRE_DISP_W, PRE_DISP_H);
        lv_obj_set_style_bg_color(c, C_CARD, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 8, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, C_PREHEAT_COL, 0);
        lv_obj_set_style_pad_all(c, 2, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        _lbl_pre_disp = lv_label_create(c);
        lv_label_set_text(_lbl_pre_disp, "--:--");
        lv_obj_set_style_text_color(_lbl_pre_disp, C_PREHEAT_COL, 0);
        lv_obj_set_style_text_font(_lbl_pre_disp, &lv_font_montserrat_32, 0);
        lv_obj_align(_lbl_pre_disp, LV_ALIGN_CENTER, 0, 0);
    }

    /* remainTimeHH:MM 카드 — 타이머 + Up/Dn 내장 (카드 오른쪽에 뚜렷한 아웃라인) */
    {
        lv_obj_t *c = lv_obj_create(_scr_main);
        lv_obj_set_pos(c, REM_DISP_X, REM_DISP_Y);
        lv_obj_set_size(c, REM_DISP_W, REM_DISP_H);
        lv_obj_set_style_bg_color(c, C_CARD, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 8, 0);
        lv_obj_set_style_border_width(c, 3, 0);
        lv_obj_set_style_border_color(c, C_RED, 0);
        lv_obj_set_style_pad_all(c, 2, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        /* 세로 구분선 (타이머 ↔ 버튼 영역) */
        lv_obj_t *sep = lv_obj_create(c);
        lv_obj_set_pos(sep, REM_DISP_W - 105, 8);
        lv_obj_set_size(sep, 2, REM_DISP_H - 18);
        lv_obj_set_style_bg_color(sep, C_BORDER, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_pad_all(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 타이머 레이블 (왼쪽 영역 중앙) */
        _lbl_rem_disp = lv_label_create(c);
        lv_label_set_text(_lbl_rem_disp, "00:00");
        lv_obj_set_style_text_color(_lbl_rem_disp, C_RED, 0);
        lv_obj_set_style_text_font(_lbl_rem_disp, &lv_font_montserrat_48, 0);
        lv_obj_set_width(_lbl_rem_disp, REM_DISP_W - 112);
        lv_obj_set_style_text_align(_lbl_rem_disp, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(_lbl_rem_disp, LV_ALIGN_LEFT_MID, 6, 0);

        /* Up / Dn 버튼 (카드 내부 오른쪽, 흰 아웃라인으로 구분) */
        const lv_coord_t BTN_X = REM_DISP_W - 101;
        const lv_coord_t BTN_W = 92;
        const lv_coord_t BTN_H = 60;

        _btn_dry_time_up = lv_btn_create(c);
        lv_obj_set_pos(_btn_dry_time_up, BTN_X, 8);
        lv_obj_set_size(_btn_dry_time_up, BTN_W, BTN_H);
        lv_obj_set_style_bg_color(_btn_dry_time_up, lv_color_hex(0x1A2E50), 0);
        lv_obj_set_style_bg_color(_btn_dry_time_up,
            lv_color_mix(lv_color_hex(0x1A2E50), lv_color_black(), 180), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_btn_dry_time_up, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_btn_dry_time_up, 6, 0);
        lv_obj_set_style_border_width(_btn_dry_time_up, 2, 0);
        lv_obj_set_style_border_color(_btn_dry_time_up, C_WHITE, 0);
        lv_obj_set_style_shadow_width(_btn_dry_time_up, 0, 0);
        lv_obj_set_style_pad_all(_btn_dry_time_up, 0, 0);
        lv_obj_add_event_cb(_btn_dry_time_up, cbBtnDryTimeUp, LV_EVENT_CLICKED, this);
        {   lv_obj_t *l = lv_label_create(_btn_dry_time_up);
            lv_label_set_text(l, LV_SYMBOL_UP " Up");
            lv_obj_set_style_text_color(l, C_WHITE, 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l); }

        _btn_dry_time_dn = lv_btn_create(c);
        lv_obj_set_pos(_btn_dry_time_dn, BTN_X, 8 + BTN_H + 9);
        lv_obj_set_size(_btn_dry_time_dn, BTN_W, BTN_H);
        lv_obj_set_style_bg_color(_btn_dry_time_dn, lv_color_hex(0x2E1A1A), 0);
        lv_obj_set_style_bg_color(_btn_dry_time_dn,
            lv_color_mix(lv_color_hex(0x2E1A1A), lv_color_black(), 180), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(_btn_dry_time_dn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_btn_dry_time_dn, 6, 0);
        lv_obj_set_style_border_width(_btn_dry_time_dn, 2, 0);
        lv_obj_set_style_border_color(_btn_dry_time_dn, C_WHITE, 0);
        lv_obj_set_style_shadow_width(_btn_dry_time_dn, 0, 0);
        lv_obj_set_style_pad_all(_btn_dry_time_dn, 0, 0);
        lv_obj_add_event_cb(_btn_dry_time_dn, cbBtnDryTimeDn, LV_EVENT_CLICKED, this);
        {   lv_obj_t *l = lv_label_create(_btn_dry_time_dn);
            lv_label_set_text(l, LV_SYMBOL_DOWN " Dn");
            lv_obj_set_style_text_color(l, C_WHITE, 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l); }
    }

    /* Long-press 가속 등록 */
    _ctx_dt_up = {this, &_dry_time_min, +1, 1440};
    _ctx_dt_dn = {this, &_dry_time_min, -1, 1440};
    lv_obj_add_event_cb(_btn_dry_time_up, cbTimeRepeat, LV_EVENT_LONG_PRESSED_REPEAT, &_ctx_dt_up);
    lv_obj_add_event_cb(_btn_dry_time_up, cbTimeRepeat, LV_EVENT_RELEASED, &_ctx_dt_up);
    lv_obj_add_event_cb(_btn_dry_time_dn, cbTimeRepeat, LV_EVENT_LONG_PRESSED_REPEAT, &_ctx_dt_dn);
    lv_obj_add_event_cb(_btn_dry_time_dn, cbTimeRepeat, LV_EVENT_RELEASED, &_ctx_dt_dn);

    /* ── BODY background panels + row labels ──────────────────────── */
    auto makeBgRow = [&](lv_coord_t y, lv_coord_t h) {
        lv_obj_t *bg = lv_obj_create(_scr_main);
        lv_obj_set_pos(bg, 0, y); lv_obj_set_size(bg, 1020, h);
        lv_obj_set_style_bg_color(bg, C_PANEL, 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bg, 0, 0);
        lv_obj_set_style_pad_all(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    };
    makeBgRow(205, 145);  /* preheat row (y:205-350) */
    makeBgRow(355, 145);  /* sensor row  (y:355-500, same h as preheat) */

    /* 행 레이블 없음 — 모두 제거됨 */

    /* Large-font setting card helper */
    auto makeBigSet = [&](lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                          lv_color_t border, lv_obj_t **val_out) {
        lv_obj_t *c = lv_obj_create(_scr_main);
        lv_obj_set_pos(c, x, y); lv_obj_set_size(c, w, h);
        lv_obj_set_style_bg_color(c, C_CARD, 0); lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 8, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, border, 0);
        lv_obj_set_style_pad_all(c, 2, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *v = lv_label_create(c);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_color(v, C_WHITE, 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_32, 0);
        lv_obj_align(v, LV_ALIGN_CENTER, 0, 0);
        if (val_out) *val_out = v;
    };

    /* ── Body row 1 — preheat (y:180) ────────────────────────────── */
    makeBigSet(PRE_TEMP_X, PRE_TEMP_Y, PRE_TEMP_W, PRE_TEMP_H,
               C_PREHEAT_COL, &_lbl_pre_temp_val);
    _btn_pre_temp_up = makeBtn(_scr_main, PRE_TEMP_BTN_X, PRE_TEMP_UP_Y,
                               UPDN_W, UPDN_H, LV_SYMBOL_UP " Up",
                               lv_color_hex(0x1A3A1A), cbBtnPreTempUp, this);
    lv_obj_set_style_border_color(_btn_pre_temp_up, C_PREHEAT_COL, 0);
    _btn_pre_temp_dn = makeBtn(_scr_main, PRE_TEMP_BTN_X, PRE_TEMP_DN_Y,
                               UPDN_W, UPDN_H, LV_SYMBOL_DOWN " Dn",
                               lv_color_hex(0x1A3A1A), cbBtnPreTempDn, this);
    lv_obj_set_style_border_color(_btn_pre_temp_dn, C_PREHEAT_COL, 0);
    makeBigSet(PRE_TIME_X, PRE_TIME_Y, PRE_TIME_W, PRE_TIME_H,
               C_PREHEAT_COL, &_lbl_pre_time_val);
    _btn_pre_time_up = makeBtn(_scr_main, PRE_TIME_BTN_X, PRE_TIME_UP_Y,
                               UPDN_W, UPDN_H, LV_SYMBOL_UP " Up",
                               lv_color_hex(0x1A3A1A), cbBtnPreTimeUp, this);
    lv_obj_set_style_border_color(_btn_pre_time_up, C_PREHEAT_COL, 0);
    /* Long-press acceleration for PRE TIME */
    _ctx_pt_up = {this, &_pre_time_min, +1, 120};
    _ctx_pt_dn = {this, &_pre_time_min, -1, 120};
    lv_obj_add_event_cb(_btn_pre_time_up, cbTimeRepeat, LV_EVENT_LONG_PRESSED_REPEAT, &_ctx_pt_up);
    lv_obj_add_event_cb(_btn_pre_time_up, cbTimeRepeat, LV_EVENT_RELEASED, &_ctx_pt_up);
    _btn_pre_time_dn = makeBtn(_scr_main, PRE_TIME_BTN_X, PRE_TIME_DN_Y,
                               UPDN_W, UPDN_H, LV_SYMBOL_DOWN " Dn",
                               lv_color_hex(0x1A3A1A), cbBtnPreTimeDn, this);
    lv_obj_set_style_border_color(_btn_pre_time_dn, C_PREHEAT_COL, 0);
    lv_obj_add_event_cb(_btn_pre_time_dn, cbTimeRepeat, LV_EVENT_LONG_PRESSED_REPEAT, &_ctx_pt_dn);
    lv_obj_add_event_cb(_btn_pre_time_dn, cbTimeRepeat, LV_EVENT_RELEASED, &_ctx_pt_dn);
    /* 예열 버튼 — 한글 "예열" (ycb_hangul canvas, scale=3) */
    _btn_preheat_start = lv_btn_create(_scr_main);
    lv_obj_set_pos(_btn_preheat_start, BTN_PH_START_X, BTN_PH_START_Y);
    lv_obj_set_size(_btn_preheat_start, BTN_PH_START_W, BTN_PH_START_H);
    lv_obj_set_style_bg_color(_btn_preheat_start, C_PRE_BTN_IDLE, 0);
    lv_obj_set_style_bg_color(_btn_preheat_start,
        lv_color_mix(C_PRE_BTN_IDLE, lv_color_black(), 180), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_btn_preheat_start, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_btn_preheat_start, 8, 0);
    lv_obj_set_style_border_width(_btn_preheat_start, 2, 0);
    lv_obj_set_style_border_color(_btn_preheat_start, C_PREHEAT_COL, 0);
    lv_obj_set_style_shadow_width(_btn_preheat_start, 0, 0);
    lv_obj_set_style_pad_all(_btn_preheat_start, 0, 0);
    lv_obj_add_event_cb(_btn_preheat_start, cbBtnPreheatStart, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *cv = lv_canvas_create(_btn_preheat_start);
        lv_canvas_set_buffer(cv, s_btn_preheat_buf,
                             KOR_BTN_PRE_W, KOR_BTN_PRE_H, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(cv, C_PRE_BTN_IDLE, LV_OPA_COVER);
        /* 예열대기 (4글자, scale=2) — 초기담 */
        ycb_hangul_draw(cv, 5, 5, C_WHITE, C_PRE_BTN_IDLE,
                        "\xec\x98\x88\xec\x97\xb4\xeb\x8c\x80\xea\xb8\xb0", 2, false);  /* 예열대기 */
        lv_obj_center(cv);
        lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _lbl_btn_preheat = cv;   /* 재사용: canvas 포인터 저장 */
    }

    /* ── Body row 2 — dry (y:300): 설정온도·Up/Dn은 헤더로 이동됨 ── */
    /* (건조행은 빈 행으로 유지) */

    /* ── Body row 3 — sensor display + weight + damper (y:430) ─── */
    makeCard(_scr_main, STAT1_X, STAT_CARD_Y, STAT_W, STAT_CARD_H,
             C_ORANGE, "OUTSIDE", &_lbl_sen_out);
    makeCard(_scr_main, STAT2_X, STAT_CARD_Y, STAT_W, STAT_CARD_H,
             C_ORANGE, "INSIDE", &_lbl_sen_in);
    makeCard(_scr_main, STAT3_X, STAT_CARD_Y, STAT_W, STAT_CARD_H,
             C_ORANGE, "OUTLET", &_lbl_sen_ex);
    makeCard(_scr_main, STAT4_X, STAT_CARD_Y, STAT_W, STAT_CARD_H,
             lv_color_hex(0x00E5FF), "WEIGHT", &_lbl_weight);

    /* DAMPER card (x:770) — click to cycle AUTO → OPEN → CLOSE → AUTO */
    _btn_damper = lv_btn_create(_scr_main);
    lv_obj_set_pos(_btn_damper, STAT5_X, STAT_CARD_Y);
    lv_obj_set_size(_btn_damper, STAT_W, STAT_CARD_H);
    lv_obj_set_style_bg_color(_btn_damper, C_CARD, 0);
    lv_obj_set_style_bg_color(_btn_damper, lv_color_mix(C_CARD, lv_color_black(), 200), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(_btn_damper, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_btn_damper, 8, 0);
    lv_obj_set_style_border_width(_btn_damper, 2, 0);
    lv_obj_set_style_border_color(_btn_damper, C_CYAN, 0);
    lv_obj_set_style_shadow_width(_btn_damper, 0, 0);
    lv_obj_set_style_pad_all(_btn_damper, 4, 0);
    lv_obj_add_event_cb(_btn_damper, cbBtnDamper, LV_EVENT_CLICKED, this);
    {
        lv_obj_t *t = lv_label_create(_btn_damper);
        lv_label_set_text(t, "DAMPER");
        lv_obj_set_style_text_color(t, C_CYAN, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);
    }
    _lbl_damper = lv_label_create(_btn_damper);
    lv_label_set_text(_lbl_damper, "AUTO");
    lv_obj_set_style_text_color(_lbl_damper, C_WHITE, 0);
    lv_obj_set_style_text_font(_lbl_damper, &lv_font_montserrat_20, 0);
    lv_obj_align(_lbl_damper, LV_ALIGN_CENTER, 0, 8);

    /* ── FOOTER (y:530, h:70) ────────────────────────────────────── */
    lv_obj_t *ftr = lv_obj_create(_scr_main);
    lv_obj_set_pos(ftr, 0, FTR_Y);
    lv_obj_set_size(ftr, 1020, FTR_H);
    lv_obj_set_style_bg_color(ftr, C_PANEL, 0);
    lv_obj_set_style_bg_opa(ftr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(ftr, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(ftr, C_BORDER, 0);
    lv_obj_set_style_border_width(ftr, 1, 0);
    lv_obj_set_style_radius(ftr, 0, 0);
    lv_obj_set_style_pad_all(ftr, 0, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);

    /* Heater icon canvas (x:50 y:540 40×40) */
    {
        lv_obj_t *htr_lbl = lv_label_create(_scr_main);
        lv_label_set_text(htr_lbl, "HTR");
        lv_obj_set_style_text_color(htr_lbl, C_GRAY, 0);
        lv_obj_set_style_text_font(htr_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(htr_lbl, 50, FTR_Y + 2);
    }
    _dot_heater = lv_canvas_create(_scr_main);
    lv_canvas_set_buffer(_dot_heater, s_icon_heater_buf, ICON_SZ, ICON_SZ, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(_dot_heater, 50, 540);
    drawHeaterIcon(_dot_heater, C_GRAY, C_PANEL);
    lv_obj_clear_flag(_dot_heater, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Fan icon canvas (x:130 y:540 40×40) */
    {
        lv_obj_t *fan_lbl = lv_label_create(_scr_main);
        lv_label_set_text(fan_lbl, "FAN");
        lv_obj_set_style_text_color(fan_lbl, C_GRAY, 0);
        lv_obj_set_style_text_font(fan_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(fan_lbl, 130, FTR_Y + 2);
    }
    _fan_ring = lv_obj_create(_scr_main);
    lv_obj_set_pos(_fan_ring, 130, 540);
    lv_obj_set_size(_fan_ring, FAN_ICON_SZ, FAN_ICON_SZ);
    lv_obj_set_style_radius(_fan_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(_fan_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_fan_ring, 3, 0);
    lv_obj_set_style_border_color(_fan_ring, C_GRAY, 0);
    lv_obj_set_style_pad_all(_fan_ring, 0, 0);
    lv_obj_clear_flag(_fan_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    _dot_fan = lv_canvas_create(_scr_main);
    lv_canvas_set_buffer(_dot_fan, s_icon_fan_buf, FAN_ICON_SZ, FAN_ICON_SZ, LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);
    lv_obj_set_pos(_dot_fan, 130, 540);
    lv_img_set_pivot(_dot_fan, FAN_ICON_SZ / 2, FAN_ICON_SZ / 2);
    lv_img_set_antialias(_dot_fan, true);
    drawFanIcon(_dot_fan, C_GRAY, C_PANEL);
    lv_obj_clear_flag(_dot_fan, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* SET button */
    _btn_set = makeBtn(_scr_main, BTN_SET_X, BTN_SET_Y, BTN_SET_W, BTN_SET_H,
                       LV_SYMBOL_LIST "  SET " LV_SYMBOL_SETTINGS,
                       C_DARK_BTN, cbBtnSet, this);

    _lbl_server_time = lv_label_create(_scr_main);
    lv_label_set_text(_lbl_server_time, "SERVER TIME  --");
    lv_obj_set_style_text_color(_lbl_server_time, C_WHITE, 0);
    lv_obj_set_style_text_font(_lbl_server_time, &lv_font_montserrat_18, 0);
    lv_obj_align(_lbl_server_time, LV_ALIGN_BOTTOM_MID, 40, -22);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Build — Process select screen
 * ═══════════════════════════════════════════════════════════════════════════ */
#endif

void DryerApp::buildMainScreen(void)
{
    _scr_main=lv_obj_create(nullptr); lv_obj_set_size(_scr_main,1024,600);
    lv_obj_set_style_bg_color(_scr_main,lv_color_hex(0x111111),0); lv_obj_set_style_bg_opa(_scr_main,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(_scr_main,0,0); lv_obj_set_style_pad_all(_scr_main,0,0); lv_obj_clear_flag(_scr_main,LV_OBJ_FLAG_SCROLLABLE);
    const lv_color_t navy=lv_color_hex(0x12324A),blue=lv_color_hex(0x0067A3),orange=lv_color_hex(0xD35400),green=lv_color_hex(0x287D3C),purple=lv_color_hex(0x663399),dark=lv_color_hex(0x202A33);
    auto tile=[&](int x,int y,int w,int h,lv_color_t bg){lv_obj_t*o=lv_obj_create(_scr_main);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,bg,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_radius(o,0,0);lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_shadow_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;};
    auto label=[&](lv_obj_t*p,const char*t,int x,int y,const lv_font_t*f,lv_color_t c){lv_obj_t*l=lv_label_create(p);lv_label_set_text(l,t);lv_obj_set_pos(l,x,y);lv_obj_set_style_text_font(l,f,0);lv_obj_set_style_text_color(l,c,0);return l;};
    auto kor=[&](lv_obj_t*p,const char*t,int x,int y,int w,int h,lv_color_t fg,lv_color_t bg,int scale,bool center){lv_color_t*buf=allocCanvasBuffer((size_t)w*h);lv_obj_t*c=lv_canvas_create(p);lv_canvas_set_buffer(c,buf,w,h,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(c,bg,LV_OPA_COVER);int tx=center?(w-ycb_hangul_measure(t,scale))/2:0;if(tx<0)tx=0;ycb_hangul_draw(c,tx,(h-16*scale)/2,fg,bg,t,scale,false);lv_obj_set_pos(c,x,y);lv_obj_clear_flag(c,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);return c;};
    auto korTransparent=[&](lv_obj_t*p,const char*t,int x,int y,int w,int h,int scale){lv_color_t*buf=allocCanvasBuffer((size_t)w*h);lv_obj_t*c=lv_canvas_create(p);lv_canvas_set_buffer(c,buf,w,h,LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);lv_canvas_fill_bg(c,LV_COLOR_CHROMA_KEY,LV_OPA_COVER);int tx=(w-ycb_hangul_measure(t,scale))/2;if(tx<0)tx=0;ycb_hangul_draw(c,tx,(h-16*scale)/2,C_WHITE,LV_COLOR_CHROMA_KEY,t,scale,false);lv_obj_set_pos(c,x,y);lv_obj_clear_flag(c,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);return c;};
    auto row=[&](lv_obj_t*p,int y,const char*n,lv_obj_t**out){lv_obj_t*o=lv_obj_create(p);lv_obj_set_pos(o,0,y);lv_obj_set_size(o,296,53);lv_obj_set_style_radius(o,0,0);lv_obj_set_style_bg_color(o,dark,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_border_side(o,LV_BORDER_SIDE_BOTTOM,0);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x536675),0);lv_obj_set_style_pad_all(o,0,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);label(o,n,10,18,&lv_font_montserrat_12,lv_color_hex(0xAFC5D5));*out=label(o,"--",194,15,&lv_font_montserrat_18,lv_color_white());};
    auto btn=[&](int x,int y,int w,int h,const char*t,lv_color_t bg,lv_event_cb_t cb){lv_obj_t*b=makeBtn(_scr_main,x,y,w,h,t,bg,cb,this);lv_obj_set_style_radius(b,0,0);lv_obj_set_style_border_width(b,0,0);return b;};
    lv_obj_t*brand=tile(0,0,296,116,lv_color_hex(0xF5F3EA));
    lv_obj_t*logo=lv_img_create(brand);lv_img_set_src(logo,&hanwool_logo);lv_obj_center(logo);lv_obj_clear_flag(logo,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(logo,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(logo,cbLogoVision,LV_EVENT_CLICKED,this);
    lv_obj_t*s=tile(0,280,296,320,navy);row(s,1,"TEMP 1 (101)",&_lbl_sen_out);row(s,54,"TEMP 2 (102)",&_lbl_sen_in);row(s,107,"TEMP 3 (103)",&_lbl_sen_ex);row(s,160,"TEMP 4 (104)",&_lbl_sen_4);row(s,213,"TEMP 5 (105)",&_lbl_sen_5);row(s,266,"TEMP 6 (106)",&_lbl_sen_6);
    for(int i=0;i<6;i++){lv_obj_t*r=lv_obj_get_child(s,i);lv_obj_add_flag(lv_obj_get_child(r,0),LV_OBJ_FLAG_HIDDEN);char kt[16],addr[12];snprintf(kt,sizeof(kt),"온습도%d",i+1);snprintf(addr,sizeof(addr),"(%d)",101+i);kor(r,kt,0,10,112,32,lv_color_hex(0xAFC5D5),dark,2,false);kor(r,addr,116,18,40,16,lv_color_hex(0xAFC5D5),dark,1,false);}
    lv_obj_t*sensorHold=lv_obj_create(_scr_main);lv_obj_set_pos(sensorHold,0,280);lv_obj_set_size(sensorHold,296,320);lv_obj_set_style_bg_opa(sensorHold,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(sensorHold,0,0);lv_obj_set_style_shadow_width(sensorHold,0,0);lv_obj_set_style_pad_all(sensorHold,0,0);lv_obj_add_event_cb(sensorHold,cbOpenCalibration,LV_EVENT_LONG_PRESSED,this);
    _lbl_door=nullptr;_lbl_dryness=nullptr;
    lv_obj_t*current=tile(300,0,234,116,orange);label(current,"CURRENT TEMP / HUMIDITY",12,8,&lv_font_montserrat_14,lv_color_white());_lbl_hdr_dry_temp=label(current,"--",0,44,&lv_font_montserrat_32,lv_color_white());lv_obj_set_width(_lbl_hdr_dry_temp,117);lv_obj_set_style_text_align(_lbl_hdr_dry_temp,LV_TEXT_ALIGN_CENTER,0);_lbl_hdr_humidity=label(current,"--",117,44,&lv_font_montserrat_32,lv_color_white());lv_obj_set_width(_lbl_hdr_humidity,117);lv_obj_set_style_text_align(_lbl_hdr_humidity,LV_TEXT_ALIGN_CENTER,0);
    kor(current,"현재온도 / 습도",10,5,214,28,C_WHITE,orange,1,true);
    _lbl_server_time=label(current,"---- -- -- --:--",0,91,&lv_font_montserrat_16,C_WHITE);lv_obj_set_width(_lbl_server_time,234);lv_obj_set_style_text_align(_lbl_server_time,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_t*setTempCard=tile(538,0,234,116,blue);_lbl_hdr_set_temp=lv_canvas_create(setTempCard);lv_canvas_set_buffer(_lbl_hdr_set_temp,s_set_temp_7seg_buf,TIME_7SEG_W,TIME_7SEG_H,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_lbl_hdr_set_temp,7,39);lv_obj_clear_flag(_lbl_hdr_set_temp,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    if(!s_main_set_temp_title_buf[0]) {
        s_main_set_temp_title_buf[0]=allocCanvasBuffer(110*24);
    }
    lv_obj_t*setTempTitle=lv_canvas_create(setTempCard);lv_canvas_set_buffer(setTempTitle,s_main_set_temp_title_buf[0],110,24,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(setTempTitle,blue,LV_OPA_COVER);ycb_hangul_draw(setTempTitle,0,4,C_WHITE,blue,"설정온도",1,false);lv_obj_set_pos(setTempTitle,12,5);
    _state_card=nullptr;
    lv_obj_t*remaining=tile(300,120,472,116,purple);label(remaining,"REMAINING TIME",12,9,&lv_font_montserrat_14,C_WHITE);_canvas_rem_time=lv_canvas_create(remaining);lv_canvas_set_buffer(_canvas_rem_time,s_remain_7seg_buf,TIME_7SEG_W,TIME_7SEG_H,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_canvas_rem_time,(472-TIME_7SEG_W)/2,39);_lbl_rem_disp=nullptr;
    kor(remaining,"남은시간",12,5,130,26,C_WHITE,purple,1,false);
    lv_obj_t*weight=tile(300,240,348,156,navy);
    kor(weight,"중량",0,14,348,56,C_WHITE,navy,3,true);
    _lbl_weight_gross=nullptr;_lbl_weight_tare=nullptr;
    _lbl_weight=label(weight,"--",10,84,&lv_font_montserrat_36,C_YELLOW);lv_obj_set_width(_lbl_weight,328);lv_obj_set_style_text_align(_lbl_weight,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_t*zeroBtn=btn(652,240,120,156,"",orange,cbLoadCellZero);
    kor(zeroBtn,"영점",0,46,120,64,C_WHITE,orange,3,true);
    _lbl_pre_temp_val=nullptr;_lbl_pre_time_val=nullptr;_canvas_pre_remain=nullptr;_lbl_pre_disp=nullptr;
    lv_obj_t*messenger=tile(300,400,472,76,dark);_lbl_messenger=label(messenger,"Waiting for server control...",12,5,&lv_font_montserrat_12,C_WHITE);lv_obj_set_size(_lbl_messenger,448,66);lv_label_set_long_mode(_lbl_messenger,LV_LABEL_LONG_WRAP);lv_label_set_recolor(_lbl_messenger,true);lv_obj_set_style_text_line_space(_lbl_messenger,1,0);
    _lbl_dry_time_val=nullptr;_lbl_dry_temp_val=nullptr;
    lv_obj_t*device=tile(0,120,296,160,navy);if(!s_equipment_name_buf)s_equipment_name_buf=allocCanvasBuffer(296*40);_lbl_equipment_name=lv_canvas_create(device);lv_canvas_set_buffer(_lbl_equipment_name,s_equipment_name_buf,296,40,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(_lbl_equipment_name,navy,LV_OPA_COVER);int equipment_name_scale=ycb_hangul_measure(_equipment_name,2)<=296?2:1;int equipment_name_width=ycb_hangul_measure(_equipment_name,equipment_name_scale);int equipment_name_x=(296-equipment_name_width)/2;if(equipment_name_x<0)equipment_name_x=0;int equipment_name_y=(40-16*equipment_name_scale)/2;ycb_hangul_draw(_lbl_equipment_name,equipment_name_x,equipment_name_y,C_WHITE,navy,_equipment_name,equipment_name_scale,false);lv_obj_set_pos(_lbl_equipment_name,0,6);lv_obj_clear_flag(_lbl_equipment_name,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);char id_text[20];if(_equipment_id>=EQUIPMENT_ID_MIN)snprintf(id_text,sizeof(id_text),"%06ld",static_cast<long>(_equipment_id));else snprintf(id_text,sizeof(id_text),"------");const lv_font_t*id_font=equipment_name_scale==2?&lv_font_montserrat_32:&lv_font_montserrat_16;_lbl_device_id=label(device,id_text,0,48,id_font,C_WHITE);lv_obj_set_width(_lbl_device_id,296);lv_obj_set_style_text_align(_lbl_device_id,LV_TEXT_ALIGN_CENTER,0);uint8_t device_mac[6]{};char mac_text[24]="--:--:--:--:--:--";if(esp_efuse_mac_get_default(device_mac)==ESP_OK)snprintf(mac_text,sizeof(mac_text),"%02X:%02X:%02X:%02X:%02X:%02X",device_mac[0],device_mac[1],device_mac[2],device_mac[3],device_mac[4],device_mac[5]);_lbl_device_ip=label(device,"IP: ---.---.---.---",10,115,&lv_font_montserrat_14,C_WHITE);lv_obj_t*mac_label=label(device,mac_text,140,116,&lv_font_montserrat_12,C_YELLOW);lv_obj_set_width(mac_label,136);lv_obj_set_style_text_align(mac_label,LV_TEXT_ALIGN_RIGHT,0);lv_obj_add_flag(device,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(device,cbOpenCoolingSettings,LV_EVENT_LONG_PRESSED,this);
    lv_obj_t*icons=tile(300,480,472,120,C_PANEL);
    auto icon_text=[&](const char*t,int x,int y,lv_color_t c){lv_obj_t*l=label(icons,t,x,y,&lv_font_montserrat_14,c);lv_obj_set_width(l,118);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);return l;};
    icon_text("HEATER",0,79,C_WHITE);_lbl_heater_status=icon_text("OFF",0,99,C_GRAY);
    icon_text("FAN",118,79,C_WHITE);icon_text("DAMPER",236,79,C_WHITE);
    icon_text("DOOR",354,79,C_WHITE);_lbl_door_status=icon_text("CLOSE",354,99,C_GREEN);
    kor(icons,"히터",0,75,118,24,C_WHITE,C_PANEL,1,true);kor(icons,"송풍기",118,75,118,24,C_WHITE,C_PANEL,1,true);kor(icons,"댐퍼",236,75,118,24,C_WHITE,C_PANEL,1,true);kor(icons,"도어",354,75,118,24,C_WHITE,C_PANEL,1,true);
    _dot_heater=lv_canvas_create(icons);lv_canvas_set_buffer(_dot_heater,s_icon_heater_buf,ICON_SZ,ICON_SZ,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_dot_heater,27,14);
    _fan_ring=lv_obj_create(icons);lv_obj_set_pos(_fan_ring,151,14);lv_obj_set_size(_fan_ring,FAN_ICON_SZ,FAN_ICON_SZ);lv_obj_set_style_radius(_fan_ring,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(_fan_ring,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(_fan_ring,3,0);lv_obj_set_style_border_color(_fan_ring,C_GRAY,0);lv_obj_set_style_pad_all(_fan_ring,0,0);lv_obj_clear_flag(_fan_ring,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    _dot_fan=lv_canvas_create(icons);lv_canvas_set_buffer(_dot_fan,s_icon_fan_buf,FAN_ICON_SZ,FAN_ICON_SZ,LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);lv_obj_set_pos(_dot_fan,151,14);lv_img_set_pivot(_dot_fan,FAN_ICON_SZ/2,FAN_ICON_SZ/2);lv_img_set_antialias(_dot_fan,true);_lbl_fan_rate=icon_text("0.0m/s(0)",118,99,C_CYAN);
    _dot_damper=lv_canvas_create(icons);lv_canvas_set_buffer(_dot_damper,s_icon_damper_buf,ICON_SZ,ICON_SZ,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_dot_damper,263,14);_lbl_damper=lv_canvas_create(icons);lv_canvas_set_buffer(_lbl_damper,s_damper_status_buf,118,24,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_lbl_damper,236,96);
    _door_icon=lv_canvas_create(icons);lv_canvas_set_buffer(_door_icon,s_door_icon_buf,DOOR_ICON_SZ,DOOR_ICON_SZ,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(_door_icon,381,14);
    _btn_damper=lv_btn_create(icons);lv_obj_set_pos(_btn_damper,236,0);lv_obj_set_size(_btn_damper,118,120);lv_obj_set_style_radius(_btn_damper,0,0);lv_obj_set_style_bg_opa(_btn_damper,LV_OPA_TRANSP,LV_STATE_DEFAULT);lv_obj_set_style_bg_color(_btn_damper,C_WHITE,LV_STATE_PRESSED);lv_obj_set_style_bg_opa(_btn_damper,LV_OPA_10,LV_STATE_PRESSED);lv_obj_set_style_border_width(_btn_damper,1,0);lv_obj_set_style_border_color(_btn_damper,C_PANEL,0);lv_obj_set_style_shadow_width(_btn_damper,0,0);lv_obj_add_event_cb(_btn_damper,cbBtnDamper,LV_EVENT_CLICKED,this);
    auto adjust_btn=[&](int x,int y,int w,int h,const char*title,const char*symbol,lv_color_t bg,lv_event_cb_t cb){lv_obj_t*b=btn(x,y,w,h,"",bg,cb);lv_obj_t*t=label(b,title,8,6,&lv_font_montserrat_12,C_WHITE);lv_obj_set_width(t,w-16);lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_LEFT,0);lv_obj_t*tri=label(b,symbol,0,0,&lv_font_montserrat_48,C_WHITE);lv_obj_set_width(tri,w);lv_obj_set_style_text_align(tri,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(tri,LV_ALIGN_CENTER,0,6);return b;};
    _btn_dry_temp_up=adjust_btn(776,0,120,116,"",LV_SYMBOL_UP,blue,cbBtnDryTempUp);_btn_dry_temp_dn=adjust_btn(900,0,124,116,"",LV_SYMBOL_DOWN,blue,cbBtnDryTempDn);
    lv_obj_t*setTempBtns[2]={_btn_dry_temp_up,_btn_dry_temp_dn};for(int i=0;i<2;i++){if(!s_main_set_temp_title_buf[i+1])s_main_set_temp_title_buf[i+1]=allocCanvasBuffer(104*24);lv_obj_t*c=lv_canvas_create(setTempBtns[i]);lv_canvas_set_buffer(c,s_main_set_temp_title_buf[i+1],104,24,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(c,blue,LV_OPA_COVER);ycb_hangul_draw(c,0,4,C_WHITE,blue,"설정온도",1,false);lv_obj_set_pos(c,8,4);lv_obj_clear_flag(c,LV_OBJ_FLAG_CLICKABLE);}
    _btn_dry_time_up=adjust_btn(776,120,120,116,"DRY TIME",LV_SYMBOL_UP,purple,cbBtnDryTimeUp);_btn_dry_time_dn=adjust_btn(900,120,124,116,"DRY TIME",LV_SYMBOL_DOWN,purple,cbBtnDryTimeDn);
    _btn_pre_temp_up=nullptr;_btn_pre_temp_dn=nullptr;_btn_pre_time_up=nullptr;_btn_pre_time_dn=nullptr;_btn_preheat_start=nullptr;_lbl_btn_preheat=nullptr;_canvas_preheat_title=nullptr;
    _btn_dry_start=btn(776,240,248,360,"",C_GRAY,[](lv_event_t*e){static_cast<DryerApp*>(lv_event_get_user_data(e))->doProgress();});
    _lbl_state=nullptr;
    _lbl_btn_dry_start=lv_canvas_create(_btn_dry_start);lv_canvas_set_buffer(_lbl_btn_dry_start,s_progress_icon_buf,PROGRESS_ICON_SZ,PROGRESS_ICON_SZ,LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED);lv_obj_set_pos(_lbl_btn_dry_start,(248-PROGRESS_ICON_SZ)/2,60);lv_obj_clear_flag(_lbl_btn_dry_start,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    _state_card=korTransparent(_btn_dry_start,"완료",0,220,248,80,4);
    _canvas_dry_title=nullptr;
    lv_obj_t*kBtns[2]={_btn_dry_time_up,_btn_dry_time_dn};const char*kTxt[2]={"건조시간","건조시간"};for(int i=0;i<2;i++)kor(kBtns[i],kTxt[i],8,4,104,24,C_WHITE,purple,1,false);
    lv_obj_t*adjustBtns[]={_btn_dry_temp_up,_btn_dry_temp_dn,_btn_dry_time_up,_btn_dry_time_dn};for(lv_obj_t*b:adjustBtns){lv_obj_add_event_cb(b,cbAdjustRepeat,LV_EVENT_LONG_PRESSED_REPEAT,this);lv_obj_add_event_cb(b,cbAdjustRepeat,LV_EVENT_RELEASED,this);}
    _ctx_pt_up={this,&_pre_time_min,+1,120};_ctx_pt_dn={this,&_pre_time_min,-1,120};_ctx_dt_up={this,&_dry_time_min,+1,1440};_ctx_dt_dn={this,&_dry_time_min,-1,1440};
}

void DryerApp::buildProcSelectScreen(void)
{
    _scr_proc = lv_obj_create(nullptr);
    lv_obj_set_size(_scr_proc, 1024, 600);
    lv_obj_set_style_bg_color(_scr_proc, C_BG, 0);
    lv_obj_set_style_bg_opa(_scr_proc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scr_proc, 0, 0);
    lv_obj_set_style_pad_all(_scr_proc, 0, 0);
    lv_obj_clear_flag(_scr_proc, LV_OBJ_FLAG_SCROLLABLE);

    /* Title "공정선택" — shifted below phone status bar */
    lv_obj_t *title_canvas = lv_canvas_create(_scr_proc);
    lv_canvas_set_buffer(title_canvas, s_proc_title_buf, KOR_PROC_W, KOR_PROC_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(title_canvas, C_BG, LV_OPA_COVER);
    ycb_hangul_draw(title_canvas, 2, 2, C_YELLOW, C_BG, "\xea\xb3\xb5\xec\xa0\x95\xec\x84\xa0\xed\x83\x9d", KOR_PROC_SCALE, false);
    lv_obj_set_pos(title_canvas, (1024 - KOR_PROC_W) / 2, SB_H + 10);
    lv_obj_clear_flag(title_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 4 process boxes — shifted down by SB_H */
    const lv_coord_t BOX_X = 200, BOX_W = 624, BOX_H = 66, BOX_GAP = 78;
    const lv_coord_t BOX_START_Y = SB_H + 60;
    for (int i = 0; i < 4; i++) {
        lv_coord_t by = BOX_START_Y + i * BOX_GAP;
        lv_color_t col = procColor(i);

        lv_obj_t *box = lv_obj_create(_scr_proc);
        lv_obj_set_pos(box, BOX_X, by);
        lv_obj_set_size(box, BOX_W, BOX_H);
        lv_obj_set_style_bg_color(box, C_CARD, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, col, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        _proc_boxes[i] = box;

        /* Process name — ycb_hangul canvas (4 chars, scale=1) */
        lv_obj_t *nc = lv_canvas_create(box);
        lv_canvas_set_buffer(nc, s_sel_proc_buf[i], KOR_SEL_PROC_W, KOR_SEL_PROC_H, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(nc, C_CARD, LV_OPA_COVER);
        ycb_hangul_draw(nc, 2, 2, col, C_CARD, _procs[i].name, 1, false);
        lv_obj_set_pos(nc, 30, (BOX_H - KOR_SEL_PROC_H) / 2);
        lv_obj_clear_flag(nc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _canvas_pnames[i] = nc;

        /* Duration / steps info */
        char info[48];
        snprintf(info, sizeof(info), "%d step(s)  %dh%02dm",
                 _procs[i].n_steps,
                 _procs[i].total_min / 60, _procs[i].total_min % 60);
        lv_obj_t *info_lbl = lv_label_create(box);
        lv_obj_set_style_text_font(info_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(info_lbl, C_GRAY, 0);
        lv_label_set_text(info_lbl, info);
        lv_obj_align(info_lbl, LV_ALIGN_RIGHT_MID, -16, 0);

        /* Running indicator dot */
        lv_obj_t *run_dot = makeDot(box, 14, BOX_H/2, 14, C_GRAY);
        _lbl_run_mark[i] = run_dot;
    }

    /* Hint bar */
    lv_obj_t *hint_bg = lv_obj_create(_scr_proc);
    lv_obj_set_pos(hint_bg, 0, SB_H + 400);
    lv_obj_set_size(hint_bg, 1024, 60);
    lv_obj_set_style_bg_color(hint_bg, C_PANEL, 0);
    lv_obj_set_style_bg_opa(hint_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(hint_bg, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(hint_bg, C_BORDER, 0);
    lv_obj_set_style_border_width(hint_bg, 1, 0);
    lv_obj_set_style_pad_all(hint_bg, 0, 0);
    lv_obj_set_style_radius(hint_bg, 0, 0);
    lv_obj_clear_flag(hint_bg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(hint_bg);
    lv_label_set_text(hint, LV_SYMBOL_NEXT "  NEXT — cycle process       " LV_SYMBOL_OK "  APPLY — start with selected");
    lv_obj_set_style_text_color(hint, C_WHITE, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    /* Next button */
    makeBtn(_scr_proc, 200, SB_H + 468, 280, 60,
            LV_SYMBOL_NEXT "  NEXT PROC",
            C_DARK_BTN, cbProcNext, this);

    /* Apply button */
    makeBtn(_scr_proc, 544, SB_H + 468, 280, 60,
            LV_SYMBOL_OK "  APPLY",
            lv_color_hex(0x1A3A1A), cbProcApply, this);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  init()
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::buildCalibrationScreen(void)
{
    _scr_cal=lv_obj_create(nullptr);lv_obj_set_size(_scr_cal,1024,600);lv_obj_set_style_bg_color(_scr_cal,C_BG,0);lv_obj_set_style_bg_opa(_scr_cal,LV_OPA_COVER,0);lv_obj_clear_flag(_scr_cal,LV_OBJ_FLAG_SCROLLABLE);
    auto cell=[&](const char*t,int x,int y,int w,lv_color_t bg){lv_obj_t*o=lv_obj_create(_scr_cal);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,58);lv_obj_set_style_radius(o,0,0);lv_obj_set_style_bg_color(o,bg,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,C_BORDER,0);lv_obj_set_style_pad_all(o,0,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);lv_obj_t*l=lv_label_create(o);lv_label_set_text(l,t);lv_obj_set_width(l,w);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_font(l,&lv_font_montserrat_18,0);lv_obj_set_style_text_color(l,C_WHITE,0);lv_obj_align(l,LV_ALIGN_CENTER,0,0);return o;};
    lv_obj_t*title=lv_label_create(_scr_cal);lv_label_set_text(title,"SENSOR CALIBRATION   (0.1 STEP)");lv_obj_set_pos(title,20,14);lv_obj_set_style_text_font(title,&lv_font_montserrat_24,0);lv_obj_set_style_text_color(title,C_WHITE,0);
    cell("SENSOR",20,55,180,C_DARK_BTN);cell("TEMP OFFSET",200,55,270,C_DARK_BTN);cell("HUM OFFSET",470,55,270,C_DARK_BTN);
    for(int i=0;i<7;i++){char n[24];snprintf(n,sizeof(n),"SENSOR %d%s",i,i==0?" CTRL":"");_cal_cells[i][0]=cell(n,20,113+i*58,180,C_PANEL);_cal_cells[i][1]=cell("+0.0 C",200,113+i*58,270,C_PANEL);_cal_cells[i][2]=cell("+0.0 %",470,113+i*58,270,C_PANEL);}
    makeBtn(_scr_cal,770,55,224,80,"UP",C_DARK_BTN,cbCalSensorPrev,this);makeBtn(_scr_cal,770,143,224,80,"DOWN",C_DARK_BTN,cbCalSensorNext,this);makeBtn(_scr_cal,770,231,108,80,"-",lv_color_hex(0x0067A3),cbCalTempDn,this);makeBtn(_scr_cal,886,231,108,80,"+",lv_color_hex(0x0067A3),cbCalTempUp,this);makeBtn(_scr_cal,770,319,224,80,"RESET ALL 0.0",lv_color_hex(0x8A5A00),cbCalResetAll,this);makeBtn(_scr_cal,770,407,224,80,"SAVE & EXIT",lv_color_hex(0x287D3C),cbCalSaveBack,this);makeBtn(_scr_cal,770,495,224,80,"EXIT",lv_color_hex(0xB3261E),cbCalExit,this);
}

void DryerApp::refreshCalibration(void)
{
    char b[24];
    for(int i=0;i<7;i++)for(int c=0;c<3;c++){lv_obj_set_style_bg_color(_cal_cells[i][c],C_PANEL,0);lv_obj_set_style_border_color(_cal_cells[i][c],C_BORDER,0);}
    for(int i=0;i<7;i++){snprintf(b,sizeof(b),"%+.1f C",_temp_cal[i]);lv_label_set_text(lv_obj_get_child(_cal_cells[i][1],0),b);snprintf(b,sizeof(b),"%+.1f %%",_hum_cal[i]);lv_label_set_text(lv_obj_get_child(_cal_cells[i][2],0),b);}
    _cal_sensor=_cal_item/2;int col=1+(_cal_item%2);lv_obj_set_style_bg_color(_cal_cells[_cal_sensor][col],lv_color_hex(0x0067A3),0);lv_obj_set_style_border_color(_cal_cells[_cal_sensor][col],C_CYAN,0);
}

void DryerApp::buildWeightCalibrationScreen(void)
{
    _scr_weight_cal=lv_obj_create(nullptr);lv_obj_set_size(_scr_weight_cal,1024,600);lv_obj_set_style_bg_color(_scr_weight_cal,C_BG,0);lv_obj_set_style_bg_opa(_scr_weight_cal,LV_OPA_COVER,0);lv_obj_clear_flag(_scr_weight_cal,LV_OBJ_FLAG_SCROLLABLE);
    auto label=[&](const char*t,int x,int y,const lv_font_t*f,lv_color_t c){lv_obj_t*l=lv_label_create(_scr_weight_cal);lv_label_set_text(l,t);lv_obj_set_pos(l,x,y);lv_obj_set_style_text_font(l,f,0);lv_obj_set_style_text_color(l,c,0);return l;};
    label("LOAD CELL CALIBRATION  MODBUS ID 200",30,24,&lv_font_montserrat_24,C_WHITE);
    label("1. Remove all weight and press ZERO",30,90,&lv_font_montserrat_18,C_CYAN);
    label("2. Place reference weight, set its value, then press SET WEIGHT",30,125,&lv_font_montserrat_18,C_CYAN);
    label("RAW",30,195,&lv_font_montserrat_18,C_GRAY);_lbl_weight_raw=label("--",220,185,&lv_font_montserrat_24,C_WHITE);
    label("WEIGHT",30,245,&lv_font_montserrat_18,C_GRAY);_lbl_weight_live=label("--",220,235,&lv_font_montserrat_24,C_WHITE);
    label("REFERENCE (kg)",30,315,&lv_font_montserrat_18,C_GRAY);_lbl_weight_reference=label("1.0 kg",220,305,&lv_font_montserrat_24,C_WHITE);
    makeBtn(_scr_weight_cal,610,80,180,75,"ZERO",lv_color_hex(0x0067A3),cbWeightTare,this);
    _btn_weight_ref_dn=makeBtn(_scr_weight_cal,610,170,85,75,"-",C_DARK_BTN,nullptr,nullptr);
    _btn_weight_ref_up=makeBtn(_scr_weight_cal,705,170,85,75,"+",C_DARK_BTN,nullptr,nullptr);
    lv_obj_add_event_cb(_btn_weight_ref_dn,cbWeightRefDn,LV_EVENT_SHORT_CLICKED,this);lv_obj_add_event_cb(_btn_weight_ref_up,cbWeightRefUp,LV_EVENT_SHORT_CLICKED,this);
    lv_obj_add_event_cb(_btn_weight_ref_dn,cbWeightRefRepeat,LV_EVENT_LONG_PRESSED_REPEAT,this);lv_obj_add_event_cb(_btn_weight_ref_dn,cbWeightRefRepeat,LV_EVENT_RELEASED,this);
    lv_obj_add_event_cb(_btn_weight_ref_up,cbWeightRefRepeat,LV_EVENT_LONG_PRESSED_REPEAT,this);lv_obj_add_event_cb(_btn_weight_ref_up,cbWeightRefRepeat,LV_EVENT_RELEASED,this);
    makeBtn(_scr_weight_cal,610,260,180,75,"SET WEIGHT",lv_color_hex(0x8A5A00),cbWeightSet,this);
    makeBtn(_scr_weight_cal,550,470,210,80,"SAVE & EXIT",lv_color_hex(0x287D3C),cbWeightSaveExit,this);
    makeBtn(_scr_weight_cal,780,470,180,80,"EXIT",lv_color_hex(0xB3261E),cbWeightExit,this);
}

void DryerApp::buildCoolingSettingsScreen(void)
{
    _scr_cooling=lv_obj_create(nullptr);lv_obj_set_size(_scr_cooling,1024,600);lv_obj_set_style_bg_color(_scr_cooling,C_BG,0);lv_obj_set_style_bg_opa(_scr_cooling,LV_OPA_COVER,0);lv_obj_clear_flag(_scr_cooling,LV_OBJ_FLAG_SCROLLABLE);
    if(!s_dryer_set_title_buf)s_dryer_set_title_buf=allocCanvasBuffer(DRYER_SET_TITLE_W*DRYER_SET_TITLE_H);
    for(int i=0;i<DRYER_SETTING_COUNT;i++)if(!s_dryer_set_name_buf[i])s_dryer_set_name_buf[i]=allocCanvasBuffer(DRYER_SET_NAME_W*DRYER_SET_NAME_H);
    for(int i=0;i<7;i++)if(!s_dryer_set_btn_buf[i])s_dryer_set_btn_buf[i]=allocCanvasBuffer(DRYER_SET_BTN_W*DRYER_SET_BTN_H);
    for(int i=0;i<4;i++)if(!s_dryer_set_unit_buf[i])s_dryer_set_unit_buf[i]=allocCanvasBuffer(110*40);
    lv_obj_t*title=lv_canvas_create(_scr_cooling);lv_canvas_set_buffer(title,s_dryer_set_title_buf,DRYER_SET_TITLE_W,DRYER_SET_TITLE_H,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(title,C_BG,LV_OPA_COVER);const char*titleText="건조기 설정";int titleX=(DRYER_SET_TITLE_W-utf8GlyphCount(titleText)*32)/2;if(titleX<0)titleX=0;ycb_hangul_draw(title,titleX,2,C_YELLOW,C_BG,titleText,2,false);lv_obj_set_pos(title,(1024-DRYER_SET_TITLE_W)/2,8);
    lv_obj_t*list=lv_obj_create(_scr_cooling);lv_obj_set_pos(list,20,50);lv_obj_set_size(list,680,530);lv_obj_set_style_bg_color(list,C_BG,0);lv_obj_set_style_bg_opa(list,LV_OPA_COVER,0);lv_obj_set_style_border_width(list,0,0);lv_obj_set_style_pad_all(list,0,0);lv_obj_set_scroll_dir(list,LV_DIR_VER);lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_AUTO);
    static const char*names[DRYER_SETTING_COUNT]={"시작 건조 온도","시작 건조 시간","예열 온도","예열 시간","온도 제어 히스테리시스","쿨링 온도","쿨링 시간","댐퍼 동작","댐퍼 열림 습도","댐퍼 제어 히스테리시스","건조기 고온 경고","저온 경고 도달 시간","최저 온도 상승 속도","팬 10m/s 기준 ADC","MQTT 전송 주기","최저팬속도","대기 동작","대기 시간","대기 온도","HTTP 서버 IP"};
    for(int i=0;i<DRYER_SETTING_COUNT;i++){lv_obj_t*r=lv_obj_create(list);lv_obj_set_pos(r,0,i*67);lv_obj_set_size(r,660,65);lv_obj_set_style_radius(r,0,0);lv_obj_set_style_bg_color(r,C_PANEL,0);lv_obj_set_style_bg_opa(r,LV_OPA_COVER,0);lv_obj_set_style_border_width(r,1,0);lv_obj_set_style_border_color(r,C_BORDER,0);lv_obj_set_style_pad_all(r,0,0);lv_obj_clear_flag(r,LV_OBJ_FLAG_SCROLLABLE);lv_obj_add_flag(r,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(r,cbCoolingRowSelect,LV_EVENT_CLICKED,this);_dryer_setting_rows[i]=r;lv_obj_t*n=lv_canvas_create(r);lv_canvas_set_buffer(n,s_dryer_set_name_buf[i],DRYER_SET_NAME_W,DRYER_SET_NAME_H,LV_IMG_CF_TRUE_COLOR);lv_obj_set_pos(n,5,12);lv_obj_t*v=lv_label_create(r);lv_label_set_text(v,"--");lv_obj_set_width(v,200);lv_obj_set_pos(v,450,17);lv_obj_set_style_text_align(v,LV_TEXT_ALIGN_LEFT,0);lv_obj_set_style_text_font(v,&lv_font_montserrat_24,0);lv_obj_set_style_text_color(v,C_CYAN,0);_dryer_setting_values[i]=v;ycb_hangul_draw(n,8,4,C_WHITE,C_PANEL,names[i],2,false);}
    auto korBtn=[&](int index,int x,int y,int w,int h,const char*korean,lv_color_t color,lv_event_cb_t cb,int scale){lv_obj_t*b=makeBtn(_scr_cooling,x,y,w,h,"",color,cb,this);lv_obj_t*c=lv_canvas_create(b);lv_canvas_set_buffer(c,s_dryer_set_btn_buf[index],DRYER_SET_BTN_W,DRYER_SET_BTN_H,LV_IMG_CF_TRUE_COLOR);lv_canvas_fill_bg(c,color,LV_OPA_COVER);int px=utf8GlyphCount(korean)*16*scale;int xo=(w-px)/2;if(xo<0)xo=0;ycb_hangul_draw(c,xo,2,C_WHITE,color,korean,scale,false);lv_obj_set_size(c,w,DRYER_SET_BTN_H);lv_obj_center(c);lv_obj_clear_flag(c,LV_OBJ_FLAG_CLICKABLE);return b;};
    lv_obj_t*minus=makeBtn(_scr_cooling,730,50,120,85,"-",lv_color_hex(0x0067A3),cbCoolingValueDn,this);lv_obj_set_style_text_font(lv_obj_get_child(minus,0),&lv_font_montserrat_32,0);
    lv_obj_t*plus=makeBtn(_scr_cooling,870,50,120,85,"+",lv_color_hex(0x0067A3),cbCoolingValueUp,this);lv_obj_set_style_text_font(lv_obj_get_child(plus,0),&lv_font_montserrat_32,0);
    lv_obj_add_event_cb(minus,cbCoolingValueDn,LV_EVENT_LONG_PRESSED_REPEAT,this);lv_obj_add_event_cb(plus,cbCoolingValueUp,LV_EVENT_LONG_PRESSED_REPEAT,this);
    lv_obj_add_event_cb(minus,cbCoolingValueDn,LV_EVENT_RELEASED,this);lv_obj_add_event_cb(plus,cbCoolingValueUp,LV_EVENT_RELEASED,this);
    korBtn(4,730,265,260,75,"기본값 초기화",lv_color_hex(0x8A5A00),cbCoolingReset,2);korBtn(5,730,360,260,75,"저장 후 종료",lv_color_hex(0x287D3C),cbCoolingSaveExit,2);korBtn(6,730,455,260,75,"나가기",lv_color_hex(0xB3261E),cbCoolingExit,2);
    lv_obj_set_x(_dryer_setting_values[19],360);lv_obj_set_width(_dryer_setting_values[19],290);lv_label_set_recolor(_dryer_setting_values[19],true);lv_obj_add_event_cb(_dryer_setting_rows[19],cbCoolingIpNextOctet,LV_EVENT_CLICKED,this);
}

void DryerApp::refreshCoolingSettings(void)
{
    const int v[19]={_dryer_settings_edit.dry_temp_c,_dryer_settings_edit.dry_time_min,_dryer_settings_edit.preheat_temp_c,_dryer_settings_edit.preheat_time_min,_dryer_settings_edit.temp_hysteresis_c,_dryer_settings_edit.cooling_temp_c,_dryer_settings_edit.cooling_time_min,_dryer_settings_edit.damper_mode,_dryer_settings_edit.damper_open_humidity_pct,_dryer_settings_edit.damper_hysteresis_pct,_dryer_settings_edit.high_warning_temp_c,_dryer_settings_edit.low_warning_reach_time_min,_dryer_settings_edit.min_temp_rise_c_per_min,_dryer_settings_edit.fan_adc_at_10ms,_dryer_settings_edit.mqtt_publish_interval_min,_dryer_settings_edit.fan_min_speed_ms,_dryer_settings_edit.standby_enabled,_dryer_settings_edit.standby_time_min,_dryer_settings_edit.standby_temp_c};char b[40];
    for(int i=0;i<19;i++){if(i==1||i==3||i==6||i==17)snprintf(b,sizeof(b),"%02d:%02d",v[i]/60,v[i]%60);else if(i==7)snprintf(b,sizeof(b),"%s",v[i]==0?"AUTO":v[i]==1?"OPEN":"CLOSE");else if(i==16)snprintf(b,sizeof(b),"%s",v[i]==0?"OFF":v[i]==1?"TIMER":"INFINITE");else if(i==8||i==9)snprintf(b,sizeof(b),"%d %%",v[i]);else if(i==11||i==14)snprintf(b,sizeof(b),"%d min",v[i]);else if(i==12)snprintf(b,sizeof(b),"%d C/min",v[i]);else if(i==13)snprintf(b,sizeof(b),"%d",v[i]);else if(i==15)snprintf(b,sizeof(b),"%d m/s",v[i]);else snprintf(b,sizeof(b),"%d \xC2\xB0" "C",v[i]);lv_label_set_text(_dryer_setting_values[i],b);}
    for(int i=0;i<DRYER_SETTING_COUNT;i++){bool selected=i==_dryer_setting_sel;lv_obj_set_style_bg_color(_dryer_setting_rows[i],C_PANEL,0);lv_obj_set_style_border_width(_dryer_setting_rows[i],selected?4:1,0);lv_obj_set_style_border_color(_dryer_setting_rows[i],selected?C_YELLOW:C_BORDER,0);}
    const int ip[4]={_dryer_settings_edit.http_server_ip1,_dryer_settings_edit.http_server_ip2,_dryer_settings_edit.http_server_ip3,_dryer_settings_edit.http_server_ip4};
    char ipText[96];
    snprintf(ipText,sizeof(ipText),"%s%d#.%s%d#.%s%d#.%s%d#:%s%ld#",
             _dryer_ip_octet_sel==0?"#FFFF00 ":"#00FFFF ",ip[0],
             _dryer_ip_octet_sel==1?"#FFFF00 ":"#00FFFF ",ip[1],
             _dryer_ip_octet_sel==2?"#FFFF00 ":"#00FFFF ",ip[2],
             _dryer_ip_octet_sel==3?"#FFFF00 ":"#00FFFF ",ip[3],
             _dryer_ip_octet_sel==4?"#FFFF00 ":"#FF8C00 ",
             static_cast<long>(_dryer_settings_edit.http_server_port));
    lv_label_set_text(_dryer_setting_values[19],ipText);
    lv_obj_scroll_to_view(_dryer_setting_rows[_dryer_setting_sel],LV_ANIM_OFF);
}

void DryerApp::adjustDryerSetting(int direction)
{
    if(_dryer_setting_sel==19){if(_dryer_ip_octet_sel<4){int32_t*ip=&_dryer_settings_edit.http_server_ip1;int32_t&value=ip[_dryer_ip_octet_sel];value+=direction;if(value<0)value=0;if(value>255)value=255;}else{int32_t&value=_dryer_settings_edit.http_server_port;value+=direction;if(value<1)value=1;if(value>65535)value=65535;}refreshCoolingSettings();return;}
    int32_t*fields[19]={&_dryer_settings_edit.dry_temp_c,&_dryer_settings_edit.dry_time_min,&_dryer_settings_edit.preheat_temp_c,&_dryer_settings_edit.preheat_time_min,&_dryer_settings_edit.temp_hysteresis_c,&_dryer_settings_edit.cooling_temp_c,&_dryer_settings_edit.cooling_time_min,&_dryer_settings_edit.damper_mode,&_dryer_settings_edit.damper_open_humidity_pct,&_dryer_settings_edit.damper_hysteresis_pct,&_dryer_settings_edit.high_warning_temp_c,&_dryer_settings_edit.low_warning_reach_time_min,&_dryer_settings_edit.min_temp_rise_c_per_min,&_dryer_settings_edit.fan_adc_at_10ms,&_dryer_settings_edit.mqtt_publish_interval_min,&_dryer_settings_edit.fan_min_speed_ms,&_dryer_settings_edit.standby_enabled,&_dryer_settings_edit.standby_time_min,&_dryer_settings_edit.standby_temp_c};
    static const int minimum[19]={10,0,0,0,1,10,0,0,0,1,10,1,1,1,1,0,0,0,0};
    static const int maximum[19]={90,1440,90,120,20,90,1440,2,100,50,120,1440,60,4096,1440,15,2,1440,90};
    int32_t &value=*fields[_dryer_setting_sel];value+=direction;
    if(value<minimum[_dryer_setting_sel])value=minimum[_dryer_setting_sel];
    if(value>maximum[_dryer_setting_sel])value=maximum[_dryer_setting_sel];
    refreshCoolingSettings();
}

void DryerApp::refreshWeightCalibration(void)
{
    if (_load_cell == nullptr) return;
    const LoadCellReading reading = _load_cell->reading();
    char text[32];
    snprintf(text,sizeof(text),reading.valid?"%ld":"--",(long)reading.raw);lv_label_set_text(_lbl_weight_raw,text);
    snprintf(text,sizeof(text),reading.valid?"%" PRId32 " g":"--",reading.weight_g);lv_label_set_text(_lbl_weight_live,text);
    snprintf(text,sizeof(text),"%.1f kg",_weight_reference_g/1000.0f);lv_label_set_text(_lbl_weight_reference,text);
}

void DryerApp::saveRuntimeState(void)
{
    DryerNvsRuntime value{};
    value.remain_min = _remaining_min;
    value.set_temp_deci_c = (int32_t)std::lround(_set_temp * 10.0f);
    value.preheat_temp_c = _pre_temp;
    value.preheat_time_min = _pre_time_min;
    value.preheat_remain_min = _pre_time_remain;
    value.preheat_active = _dry_state == DRY_PREHEAT ? 1U : 0U;
    value.dry_state = static_cast<uint8_t>(_dry_state);
    value.standby_active = _standby_mode_active ? 1U : 0U;
    value.cooling_remain_min = _cool_remain;
    value.tare_weight_g = _tare_weight_g;
    if (!s_nvs.saveRuntime(value)) printf("Dryer runtime NVS save failed\n");
}

void DryerApp::restoreRuntimeState(void)
{
    DryerNvsRuntime value{};
    if (!s_nvs.loadRuntime(&value)) return;
    _remaining_min = value.remain_min;
    _set_temp = value.set_temp_deci_c / 10.0f;
    _pre_temp = value.preheat_temp_c;
    _pre_time_min = value.preheat_time_min;
    _pre_time_remain = value.preheat_remain_min;
    _dry_state = static_cast<DryState>(value.dry_state);
    _standby_mode_active = value.standby_active != 0;
    _cool_remain = value.cooling_remain_min;
    _tare_weight_g = value.tare_weight_g;
    _fan_on = _dry_state != DRY_FINISH;
    _heater_on = false;
    _fan_disable_after_ms = 0;
    _heater_enable_after_ms = (_dry_state == DRY_PREHEAT || _dry_state == DRY_RUN)
                            ? lv_tick_get() + 1000U : 0U;
    if (_dry_state == DRY_PREHEAT) {
        _set_temp = static_cast<float>(_pre_temp);
        _remaining_min = _pre_time_remain;
    } else if (_dry_state == DRY_PREPARE && _standby_mode_active) {
        _set_temp = static_cast<float>(_dryer_settings.standby_temp_c);
    } else if (_dry_state == DRY_COOL) {
        _cool_remain = _remaining_min;
        _set_temp = static_cast<float>(_dryer_settings.cooling_temp_c);
    } else if (_dry_state == DRY_FINISH) {
        _remaining_min = 0;
        _set_temp = 0.0F;
        _fan_on = false;
    }
    _tick_s = 0;
}

bool DryerApp::init(void)
{
    s_nvs.loadEquipmentName(_equipment_name, sizeof(_equipment_name));
    s_nvs.loadEquipmentId(&_equipment_id);
    _vision.setUploadHandler(uploadVisionFrame, this);
    memset(_temp_cal,0,sizeof(_temp_cal));memset(_hum_cal,0,sizeof(_hum_cal));memset(_temp_cal_backup,0,sizeof(_temp_cal_backup));memset(_hum_cal_backup,0,sizeof(_hum_cal_backup));memset(_cal_cells,0,sizeof(_cal_cells));
    DryerNvsCalibration calibration{};
    if (s_nvs.loadCalibration(&calibration)) {
        memcpy(_temp_cal, calibration.temperature, sizeof(_temp_cal));
        memcpy(_hum_cal, calibration.humidity, sizeof(_hum_cal));
    }
    if (_load_cell != nullptr) {
        LoadCellCalibration loadCellCalibration{};
        if (s_nvs.loadLoadCellCalibration(&loadCellCalibration)) {
            _load_cell->setCalibration(loadCellCalibration);
        }
    }
    DryerNvsCoolingSettings cooling{};
    if (s_nvs.loadCoolingSettings(&cooling)) {
        _dryer_settings = cooling;
        _damper_mode = static_cast<DamperMode>(cooling.damper_mode);
    }
    _pre_temp = _dryer_settings.preheat_temp_c;
    _pre_time_min = _dryer_settings.preheat_time_min;
    restoreRuntimeState();
    if (s_relay.initialize() != ESP_OK) return false;
    if (s_fan.initialize() != ESP_OK) return false;
    /* ── NVS 에서 남은 건조 시간 복원 ── */
    buildMainScreen();
    buildCalibrationScreen();
    buildWeightCalibrationScreen();
    buildCoolingSettingsScreen();
    Buzzer::instance().initialize();
    if (_door_sensor.initialize() == ESP_OK) _door_open = _door_sensor.isOpen();
    Buzzer::instance().attachToTree(_scr_main);
    Buzzer::instance().attachToTree(_scr_cal);
    Buzzer::instance().attachToTree(_scr_weight_cal);
    Buzzer::instance().attachToTree(_scr_cooling);
    refreshHeader();
    refreshCards();
    refreshFooter();
    return true;
}

esp_err_t DryerApp::uploadVisionFrame(const void *rgb565, size_t size,
                                      uint32_t width, uint32_t height,
                                      void *context)
{
    auto *self=static_cast<DryerApp*>(context);
    if(!self||!rgb565||size==0||width==0||height==0||size>UINT32_MAX)return ESP_ERR_INVALID_ARG;

    /* The LVGL guide box is centered and covers 60% of the preview. Crop the
       same normalized area from the native RGB565 frame before encoding. */
    const uint32_t cropWidth=((width*60U)/100U)&~1U;
    const uint32_t cropHeight=((height*60U)/100U)&~1U;
    if(cropWidth==0||cropHeight==0)return ESP_ERR_INVALID_SIZE;
    const uint32_t cropX=(width-cropWidth)/2U;
    const uint32_t cropY=(height-cropHeight)/2U;
    const size_t cropSize=static_cast<size_t>(cropWidth)*cropHeight*sizeof(uint16_t);
    size_t allocatedCropSize=0;
    const jpeg_encode_memory_alloc_cfg_t inputMemoryCfg{.buffer_direction=JPEG_ENC_ALLOC_INPUT_BUFFER};
    auto *cropped=static_cast<uint8_t*>(jpeg_alloc_encoder_mem(cropSize,&inputMemoryCfg,&allocatedCropSize));
    if(!cropped)return ESP_ERR_NO_MEM;
    const auto *source=static_cast<const uint16_t*>(rgb565);
    auto *destination=reinterpret_cast<uint16_t*>(cropped);
    for(uint32_t row=0;row<cropHeight;++row){
        memcpy(destination+static_cast<size_t>(row)*cropWidth,
               source+static_cast<size_t>(cropY+row)*width+cropX,
               static_cast<size_t>(cropWidth)*sizeof(uint16_t));
    }

    jpeg_encoder_handle_t encoder=nullptr;
    const jpeg_encode_engine_cfg_t engineCfg{.intr_priority=0,.timeout_ms=1000};
    esp_err_t err=jpeg_new_encoder_engine(&engineCfg,&encoder);
    if(err!=ESP_OK){heap_caps_free(cropped);return err;}

    const size_t requestedOutputSize=(cropSize*3U)/4U;
    size_t outputCapacity=0;
    const jpeg_encode_memory_alloc_cfg_t memoryCfg{.buffer_direction=JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    auto *jpeg=static_cast<uint8_t*>(jpeg_alloc_encoder_mem(requestedOutputSize,&memoryCfg,&outputCapacity));
    if(!jpeg){jpeg_del_encoder_engine(encoder);heap_caps_free(cropped);return ESP_ERR_NO_MEM;}

    const jpeg_encode_cfg_t encodeCfg{
        .height=cropHeight,
        .width=cropWidth,
        .src_type=JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample=JPEG_DOWN_SAMPLING_YUV422,
        .image_quality=IMAGE_UPLOAD_JPEG_QUALITY,
    };
    uint32_t jpegSize=0;
    err=jpeg_encoder_process(encoder,&encodeCfg,cropped,
                             static_cast<uint32_t>(cropSize),jpeg,
                             static_cast<uint32_t>(outputCapacity),&jpegSize);
    jpeg_del_encoder_engine(encoder);
    heap_caps_free(cropped);
    if(err!=ESP_OK){heap_caps_free(jpeg);return err;}

    uint8_t mac[6]{};
    err=esp_efuse_mac_get_default(mac);
    if(err!=ESP_OK){heap_caps_free(jpeg);return err;}
    char equipmentCode[13];
    snprintf(equipmentCode,sizeof(equipmentCode),"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    char url[160];
    snprintf(url,sizeof(url),"http://%ld.%ld.%ld.%ld:%ld/api/dryers/%s/captures",
             static_cast<long>(self->_dryer_settings.http_server_ip1),
             static_cast<long>(self->_dryer_settings.http_server_ip2),
             static_cast<long>(self->_dryer_settings.http_server_ip3),
             static_cast<long>(self->_dryer_settings.http_server_ip4),
             static_cast<long>(self->_dryer_settings.http_server_port),equipmentCode);

    const esp_http_client_config_t httpCfg{
        .url=url,
        .timeout_ms=IMAGE_UPLOAD_HTTP_TIMEOUT_MS,
        .buffer_size=1024,
        .buffer_size_tx=4096,
    };
    esp_http_client_handle_t client=esp_http_client_init(&httpCfg);
    if(!client){heap_caps_free(jpeg);return ESP_ERR_NO_MEM;}
    esp_http_client_set_method(client,HTTP_METHOD_POST);
    esp_http_client_set_header(client,"Content-Type","image/jpeg");
    char encodedName[100];urlEncodeHeader(self->_equipment_name,encodedName,sizeof(encodedName));
    esp_http_client_set_header(client,"X-Equipment-Name",encodedName);
    char number[16];
    snprintf(number,sizeof(number),"%" PRIu32,cropWidth);esp_http_client_set_header(client,"X-Image-Width",number);
    snprintf(number,sizeof(number),"%" PRIu32,cropHeight);esp_http_client_set_header(client,"X-Image-Height",number);
    const time_t now=time(nullptr);struct tm localTime{};localtime_r(&now,&localTime);
    char capturedAt[32];strftime(capturedAt,sizeof(capturedAt),"%Y-%m-%dT%H:%M:%S+09:00",&localTime);
    esp_http_client_set_header(client,"X-Captured-At",capturedAt);

    err=esp_http_client_open(client,static_cast<int>(jpegSize));
    size_t sent=0;
    while(err==ESP_OK&&sent<jpegSize){
        const int written=esp_http_client_write(client,reinterpret_cast<const char*>(jpeg+sent),jpegSize-sent);
        if(written<=0){err=ESP_FAIL;break;}sent+=static_cast<size_t>(written);
    }
    if(err==ESP_OK){
        const int64_t responseLength=esp_http_client_fetch_headers(client);
        const int status=esp_http_client_get_status_code(client);
        char response[256]{};
        const int read=esp_http_client_read_response(client,response,sizeof(response)-1);
        if(read>=0)response[read]='\0';
        ESP_LOGI("ImageUpload","POST %s JPEG=%" PRIu32 " status=%d responseLength=%" PRId64 " response=%s",url,jpegSize,status,responseLength,response);
        if(status<200||status>=300)err=ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    heap_caps_free(jpeg);
    return err;
}

bool DryerApp::startCommunication(void)
{
    return _mqtt.start();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  run() / back() / close()
 * ═══════════════════════════════════════════════════════════════════════════ */
bool DryerApp::run(void)
{
    if (!_scr_main) return false;
    lv_scr_load(_scr_main);
    _cur_scr = SCR_MAIN;

    if (!_timer)
        _timer = lv_timer_create(cbTimer, 1000, this);
    if (!_fan_spin_timer)
        /* Match the 16 ms display refresh period (about 60 fps). */
        _fan_spin_timer = lv_timer_create(cbFanSpinTimer, 16, this);
    return true;
}

bool DryerApp::back(void)
{
    if (_cur_scr == SCR_PROC_SELECT || _cur_scr == SCR_CALIBRATION ||
        _cur_scr == SCR_WEIGHT_CALIBRATION || _cur_scr == SCR_COOLING_SETTINGS) {
        lv_scr_load(_scr_main);
        _cur_scr = SCR_MAIN;
        return false;   /* don't close */
    }
    return false;
}

bool DryerApp::close(void)
{
    if (_timer) {
        lv_timer_del(_timer);
        _timer = nullptr;
    }
    if (_fan_spin_timer) {
        lv_timer_del(_fan_spin_timer);
        _fan_spin_timer = nullptr;
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  External sensor update hook
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::updateSensors(float temp_c, float hum_pct, float set_temp_c)
{
    _cur_temp  = temp_c + _temp_cal[0];
    _humidity  = hum_pct + _hum_cal[0];
    _set_temp  = set_temp_c;
    TemperatureHumidityValue &sensor = g_dryer_sensor_values.temperature_humidity[0];
    sensor.temperature_c = _cur_temp;
    sensor.humidity_pct = _humidity;
    sensor.updated_at = xTaskGetTickCount();
    sensor.valid = true;
    refreshCards();
}

void DryerApp::updateMachineInputs(bool door_open, float blower_speed_ms,
                                   int32_t weight_g, float damper_percent)
{
    _door_open = door_open;
    _blower_speed_ms = blower_speed_ms < 0.0f ? 0.0f : blower_speed_ms;
    _weight_g = weight_g;
    _damper_percent = damper_percent < 0.0f ? 0.0f :
                       (damper_percent > 100.0f ? 100.0f : damper_percent);
    g_dryer_sensor_values.door.open = _door_open;
    g_dryer_sensor_values.door.raw_level = _door_open ? 1 : 0;
    g_dryer_sensor_values.door.valid = true;
    g_dryer_sensor_values.fan_velocity.velocity_ms = _blower_speed_ms;
    g_dryer_sensor_values.fan_velocity.reference_adc = _dryer_settings.fan_adc_at_10ms;
    g_dryer_sensor_values.fan_velocity.valid = true;
    g_dryer_sensor_values.load_cell.weight_g = _weight_g;
    g_dryer_sensor_values.load_cell.updated_at = xTaskGetTickCount();
    g_dryer_sensor_values.load_cell.valid = true;
    g_dryer_sensor_values.damper_percent = _damper_percent;
    refreshCards();
    refreshFooter();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  applyProcActive() — load _procs[_proc_no] into runtime state
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::applyProcActive(void)
{
    const DryProcess &p = _procs[_proc_no];
    _remaining_min = p.total_min;
    _set_temp      = (float)p.step_temp[0];
    _tick_s        = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Refresh helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::refreshProgressCard(void)
{
    if (!_btn_dry_start) return;

    /* Dark, saturated stage colors keep all white text clearly readable. */
    const lv_color_t bg = _dry_state == DRY_PREHEAT ? lv_color_hex(0x8A3D00) :
                          _dry_state == DRY_PREPARE ? lv_color_hex(0x665000) :
                          _dry_state == DRY_RUN     ? lv_color_hex(0x17633A) :
                          _dry_state == DRY_COOL    ? lv_color_hex(0x006070) :
                                                     lv_color_hex(0x3A4652);
    lv_obj_set_style_bg_color(_btn_dry_start, bg, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(_btn_dry_start, lv_color_darken(bg, LV_OPA_20),
                              LV_STATE_PRESSED);
    if (_state_card) {
        const char *state_text = stateKor(_dry_state);
        lv_canvas_fill_bg(_state_card, LV_COLOR_CHROMA_KEY, LV_OPA_COVER);
        int text_x = (248 - ycb_hangul_measure(state_text, 4)) / 2;
        if (text_x < 0) text_x = 0;
        ycb_hangul_draw(_state_card, text_x, 8, C_WHITE, LV_COLOR_CHROMA_KEY,
                        state_text, 4, false);
    }
    drawProgressStateIcon(_lbl_btn_dry_start, _dry_state);
    lv_obj_invalidate(_btn_dry_start);
}

void DryerApp::refreshHeader(void)
{
    char buf[16];
    #define T_FMT "%02d:%02d"

    /* preTtime card
     *   PREPARE/FINISH : pre_time_min 설정값
     *   PREHEAT        : pre_time_remain 카운트다운
     *   DRY_RUN/COOL   : 00:00 (예열 완료, 동결) */
    if (_lbl_pre_disp) {
        if (_dry_state == DRY_PREHEAT) {
            snprintf(buf, sizeof(buf), T_FMT,
                     _pre_time_remain / 60, _pre_time_remain % 60);
            lv_color_t pc = _colon_blink ? C_PREHEAT_COL : C_GRAY;
            lv_obj_set_style_text_color(_lbl_pre_disp, pc, 0);
        } else if (_dry_state == DRY_RUN || _dry_state == DRY_COOL || _dry_state == DRY_FINISH) {
            snprintf(buf, sizeof(buf), "00:00");  /* 예열 완료 */
            lv_obj_set_style_text_color(_lbl_pre_disp, C_GRAY, 0);
        } else {
            snprintf(buf, sizeof(buf), "00:00");
            lv_obj_set_style_text_color(_lbl_pre_disp, C_GRAY, 0);
        }
        setLabelTextIfChanged(_lbl_pre_disp, buf);
    }
    const bool time_canvas_changed = !_time_canvas_state_valid ||
        _time_canvas_last_pre_remain != _pre_time_remain ||
        _time_canvas_last_remaining != _remaining_min ||
        _time_canvas_last_state != _dry_state;
    if (_canvas_pre_remain && time_canvas_changed) {
        const int preRemain = (_dry_state == DRY_PREHEAT) ? _pre_time_remain : 0;
        const lv_color_t preColor = (_dry_state == DRY_PREHEAT) ? C_PREHEAT_COL : C_WHITE;
        drawTime7Seg(_canvas_pre_remain, preRemain, preColor,
                     lv_color_hex(0x287D3C),
                     true);
    }

    /* remainTimeHH:MM card — 항상 _remaining_min 표시 (예열 중도 동결 표시) */
    snprintf(buf, sizeof(buf), T_FMT,
             _remaining_min / 60, _remaining_min % 60);
    lv_color_t rc = _dry_state == DRY_PREHEAT ? C_PREHEAT_COL :
                    _dry_state == DRY_PREPARE ? C_YELLOW :
                    _dry_state == DRY_RUN ? C_ORANGE :
                    _dry_state == DRY_COOL ? C_CYAN : C_GRAY;
    if (_lbl_rem_disp) {
        lv_obj_set_style_text_color(_lbl_rem_disp, rc, 0);
        setLabelTextIfChanged(_lbl_rem_disp, buf);
    }
    if (_canvas_rem_time && time_canvas_changed)
        drawTime7Seg(_canvas_rem_time, _remaining_min, rc,
                     lv_color_hex(0x663399),
                     true);

    if (time_canvas_changed) {
        _time_canvas_last_pre_remain = _pre_time_remain;
        _time_canvas_last_remaining = _remaining_min;
        _time_canvas_last_state = _dry_state;
        _time_canvas_state_valid = true;
    }

    refreshProgressCard();
    #undef T_FMT
}

void DryerApp::refreshCards(void)
{
    char buf[32];
    if (_lbl_messenger) {
        char history[6 * 96 + 32] = {};
        if (_mqtt.getControlHistory(history, sizeof(history)))
            setLabelTextIfChanged(_lbl_messenger, history);
    }
    #define DEG_C "\xC2\xB0""C"

    /* Header live values */
    if (_lbl_hdr_dry_temp) {
        snprintf(buf, sizeof(buf), "%d" DEG_C, (int)_cur_temp);
        setLabelTextIfChanged(_lbl_hdr_dry_temp, buf);
    }
    if (_lbl_hdr_humidity) {
        snprintf(buf, sizeof(buf), "%d%%", (int)_humidity);
        setLabelTextIfChanged(_lbl_hdr_humidity, buf);
    }
    if (_lbl_hdr_set_temp) {
        drawTemperature7Seg(_lbl_hdr_set_temp, static_cast<int>(_set_temp),
                            C_WHITE, lv_color_hex(0x0067A3));
    }

    /* Body row 1 — PRE TIME: 항상 설정값 고정 (카운트다운 안함) */
    if (_lbl_pre_temp_val) {
        snprintf(buf, sizeof(buf), "%d" DEG_C, _pre_temp);
        setLabelTextIfChanged(_lbl_pre_temp_val, buf);
    }
    if (_lbl_pre_time_val) {
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 _pre_time_min / 60, _pre_time_min % 60);
        setLabelTextIfChanged(_lbl_pre_time_val, buf);
    }

    /* Body row 2 — DRY TIME: 항상 _remaining_min 표시 (상단과 동기) */
    if (_lbl_dry_temp_val) {
        snprintf(buf, sizeof(buf), "%d" DEG_C, (int)_set_temp);
        setLabelTextIfChanged(_lbl_dry_temp_val, buf);
    }
    if (_lbl_dry_time_val) {
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 _remaining_min / 60, _remaining_min % 60);
        setLabelTextIfChanged(_lbl_dry_time_val, buf);
    }

    /* Body row 3 — sensor pairs */
    if (_lbl_sen_out) {
        snprintf(buf, sizeof(buf), "%d" DEG_C " %d%%",
                 (int)_sensors[0].temp, (int)_sensors[0].hum);
        setLabelTextIfChanged(_lbl_sen_out, buf);
    }
    if (_lbl_sen_in) {
        snprintf(buf, sizeof(buf), "%d" DEG_C " %d%%",
                 (int)_sensors[1].temp, (int)_sensors[1].hum);
        setLabelTextIfChanged(_lbl_sen_in, buf);
    }
    if (_lbl_sen_ex) {
        snprintf(buf, sizeof(buf), "%d" DEG_C " %d%%",
                 (int)_sensors[2].temp, (int)_sensors[2].hum);
        setLabelTextIfChanged(_lbl_sen_ex, buf);
    }
    lv_obj_t *extra_sensor_labels[3] = {_lbl_sen_4, _lbl_sen_5, _lbl_sen_6};
    for (size_t i = 0; i < 3; ++i) {
        if (extra_sensor_labels[i]) {
            snprintf(buf, sizeof(buf), "%d" DEG_C " %d%%",
                     (int)_sensors[i + 3].temp, (int)_sensors[i + 3].hum);
            setLabelTextIfChanged(extra_sensor_labels[i], buf);
        }
    }
    if (_lbl_weight || _lbl_weight_gross || _lbl_weight_tare) {
        const int64_t gross64 = static_cast<int64_t>(_tare_weight_g) + _weight_g;
        const int32_t grossG = gross64 < std::numeric_limits<int32_t>::min()
            ? std::numeric_limits<int32_t>::min()
            : gross64 > std::numeric_limits<int32_t>::max()
                ? std::numeric_limits<int32_t>::max()
                : static_cast<int32_t>(gross64);
        const lv_color_t weightColor = g_alarm_info.weight_sensor_error
            ? lv_color_hex(0x46505A) : C_WHITE;
        if (_lbl_weight_gross) {
            snprintf(buf, sizeof(buf), "%.2f kg", static_cast<double>(grossG) / 1000.0);
            setLabelTextIfChanged(_lbl_weight_gross, buf);
            lv_obj_set_style_text_color(_lbl_weight_gross, weightColor, 0);
        }
        if (_lbl_weight_tare) {
            snprintf(buf, sizeof(buf), "%.2f kg", static_cast<double>(_tare_weight_g) / 1000.0);
            setLabelTextIfChanged(_lbl_weight_tare, buf);
            lv_obj_set_style_text_color(_lbl_weight_tare,
                g_alarm_info.weight_sensor_error ? lv_color_hex(0x46505A) : C_CYAN, 0);
        }
        if (_lbl_weight) {
            snprintf(buf, sizeof(buf), "%.2f kg", static_cast<double>(_weight_g) / 1000.0);
            setLabelTextIfChanged(_lbl_weight, buf);
            lv_obj_set_style_text_color(_lbl_weight,
                g_alarm_info.weight_sensor_error ? lv_color_hex(0x46505A) : C_YELLOW, 0);
        }
    }
    if (_lbl_door) lv_label_set_text(_lbl_door, _door_open ? "OPEN" : "CLOSED");
    if (_lbl_fan_rate) {
        const FanVelocitySensorValue &fan = g_dryer_sensor_values.fan_velocity;
        if (fan.valid)
            snprintf(buf, sizeof(buf), "%.1fm/s(%d)", _blower_speed_ms, fan.raw);
        else
            snprintf(buf, sizeof(buf), "--.-m/s(----)");
        setLabelTextIfChanged(_lbl_fan_rate, buf);
    }
    if (_lbl_dryness) {
        int dryness = (int)std::round(100.0f - _humidity);
        if (dryness < 0) dryness = 0;
        if (dryness > 100) dryness = 100;
        snprintf(buf, sizeof(buf), "%d %%", dryness);
        setLabelTextIfChanged(_lbl_dryness, buf);
    }

    /* DAMPER */
    if (_lbl_damper) {
        drawDamperStatus(_lbl_damper, _damper_mode, _damper_percent);
        if (_btn_damper)
            lv_obj_set_style_border_color(_btn_damper, C_PANEL, 0);
    }
    #undef DEG_C
}

void DryerApp::refreshFooter(void)
{
    if (!_dot_heater) return;
    const bool heater_changed = !_footer_icon_state_valid ||
                                _heater_icon_last_on != _heater_on;
    const bool door_changed = !_footer_icon_state_valid ||
                              _door_icon_last_open != _door_open;
    const bool damper_changed = !_footer_icon_state_valid ||
        std::fabs(_damper_icon_last_percent - _damper_percent) >= 0.5F;

    /* Redraw footer canvases only when their displayed value changes. */
    if (heater_changed)
        drawHeaterIcon(_dot_heater, g_alarm_info.under_heat ? C_BLUE :
                       (_heater_on ? C_RED : C_GRAY), C_PANEL);
    /* Redraw only on state changes; redrawing every second causes a rotation hitch. */
    if (!_fan_icon_state_valid || _fan_icon_last_on != _fan_on) {
        if (_fan_on && (!_fan_icon_state_valid || !_fan_icon_last_on))
            _fan_spin_epoch_ms = lv_tick_get();
        drawFanIcon(_dot_fan, _fan_on ? C_CYAN : C_GRAY, C_PANEL);
        if (_fan_ring)
            lv_obj_set_style_border_color(_fan_ring,
                                           _fan_on ? C_CYAN : C_GRAY, 0);
        _fan_icon_last_on = _fan_on;
        _fan_icon_state_valid = true;
    }
    if (!_fan_on) {
        _fan_spin_angle = 30;
        drawFanIcon(_dot_fan, C_GRAY, C_PANEL);
    }
    if (_dot_damper && damper_changed)
        drawDamperIcon(_dot_damper, _damper_percent, C_CYAN, C_PANEL);
    if (_door_icon && door_changed)
        drawDoorIcon(_door_icon, _door_open,
                     _door_open ? C_RED : C_GREEN, C_PANEL);
    if (_lbl_heater_status && heater_changed) {
        lv_label_set_text(_lbl_heater_status, _heater_on ? "ON" : "OFF");
        lv_obj_set_style_text_color(_lbl_heater_status,
                                    g_alarm_info.under_heat ? C_BLUE :
                                    (_heater_on ? C_RED : C_GRAY), 0);
    }
    if (_lbl_door_status && door_changed) {
        lv_label_set_text(_lbl_door_status,
                          _door_open ? "OPEN" : "CLOSE");
        lv_obj_set_style_text_color(_lbl_door_status,
                                    _door_open ? C_RED : C_GREEN, 0);
    }
    _heater_icon_last_on = _heater_on;
    _door_icon_last_open = _door_open;
    _damper_icon_last_percent = _damper_percent;
    _footer_icon_state_valid = true;
    if (_lbl_device_ip) {
        char ipText[24] = "IP: ---.---.---.---";
        esp_netif_t *ethNetif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        esp_netif_ip_info_t ipInfo{};
        if (ethNetif && esp_netif_get_ip_info(ethNetif, &ipInfo) == ESP_OK &&
            ipInfo.ip.addr != 0) {
            snprintf(ipText, sizeof(ipText), "IP: " IPSTR, IP2STR(&ipInfo.ip));
        }
        setLabelTextIfChanged(_lbl_device_ip, ipText);
    }
    if (_lbl_server_time) {
        const time_t now = time(nullptr);
        struct tm localTime {};
        localtime_r(&now, &localTime);
        if (localTime.tm_year + 1900 >= 2024) {
            char timeText[20];
            strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M", &localTime);
            lv_label_set_text(_lbl_server_time, timeText);
        } else {
            lv_label_set_text(_lbl_server_time,"---- -- -- --:--");
        }
    }
    /* 예열 버튼: PREHEAT 중=주황 테두리+배경, 기타=녹색 */
    if (false && _btn_preheat_start && _lbl_btn_preheat) {
        bool active = (_dry_state == DRY_PREHEAT);
        lv_color_t bg = active ? C_PRE_BTN_ACT : C_PRE_BTN_IDLE;
        lv_obj_set_style_bg_color(_btn_preheat_start, bg, 0);
        lv_obj_set_style_border_color(_btn_preheat_start,
                                      active ? C_ORANGE : C_PREHEAT_COL, 0);
        /* 한글 캐린버스 배경도 맞춰 재드로우 */
        /* 예열중(3글자) or 예열대기(4글자): 중앙 정렬 */
        const char *pre_txt = active
            ? "\xec\x98\x88\xec\x97\xb4\xec\xa4\x91"                          /* 예열중 */
            : "\xec\x98\x88\xec\x97\xb4\xeb\x8c\x80\xea\xb8\xb0";             /* 예열대기 */
        int n_chars  = active ? 3 : 4;
        int x_off    = (KOR_BTN_PRE_W - n_chars * 16 * 2) / 2;
        if (x_off < 2) x_off = 2;
        lv_canvas_fill_bg((lv_obj_t*)_lbl_btn_preheat, bg, LV_OPA_COVER);
        ycb_hangul_draw((lv_obj_t*)_lbl_btn_preheat, x_off, 5, C_WHITE, bg,
                        pre_txt, 2, false);
    }
    if (_btn_preheat_start && _lbl_btn_preheat) {
        const bool active=(_dry_state==DRY_PREHEAT);
        const lv_color_t bg=active?lv_color_hex(0x287D3C):C_GRAY;
        lv_obj_set_style_bg_color(_btn_preheat_start,bg,0);
        lv_label_set_text(_lbl_btn_preheat,active?LV_SYMBOL_STOP:LV_SYMBOL_PLAY);
        if(_canvas_preheat_title){lv_canvas_fill_bg(_canvas_preheat_title,bg,LV_OPA_COVER);ycb_hangul_draw(_canvas_preheat_title,0,0,C_WHITE,bg,"예열",2,false);}
    }
}

void DryerApp::refreshProcSelect(void)
{
    for (int i = 0; i < 4; i++) {
        if (!_proc_boxes[i]) continue;
        lv_color_t col = procColor(i);
        bool isSel = (i == _sel_proc);
        bool isRun = (i == _proc_no);

        /* Box bg color changes on selection */
        lv_obj_set_style_bg_color(_proc_boxes[i], isSel ? col : C_CARD, 0);

        /* Redraw name canvas with correct fg/bg colors */
        if (_canvas_pnames[i]) {
            lv_color_t fg = isSel ? C_CARD : col;
            lv_color_t bg = isSel ? col : C_CARD;
            lv_canvas_fill_bg(_canvas_pnames[i], bg, LV_OPA_COVER);
            ycb_hangul_draw(_canvas_pnames[i], 2, 2, fg, bg, _procs[i].name, 1, false);
        }

        /* Running dot */
        lv_obj_set_style_bg_color(_lbl_run_mark[i],
                                   isRun ? C_CYAN : C_DARKGRAY, 0);
        lv_obj_set_style_shadow_width(_lbl_run_mark[i],
                                       isRun ? 8 : 0, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Business logic
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::doStartStop(void)
{
    if (_dry_state == DRY_FINISH) startDrying();
    else finishCycle();
    saveRuntimeState();
    refreshHeader();
    refreshCards();
    refreshFooter();
}

void DryerApp::startDrying(void)
{
    _standby_mode_active = false;
    _set_temp = static_cast<float>(_dryer_settings.dry_temp_c);
    _remaining_min = _dryer_settings.dry_time_min;
    _dry_time_min = _dryer_settings.dry_time_min;
    _pre_time_remain = 0;
    _cool_remain = 0;
    _dry_state = DRY_RUN;
    _fan_on = true;
    _heater_on = false;
    _heater_enable_after_ms = lv_tick_get() + 1000U;
    _fan_disable_after_ms = 0;
    _tick_s = 0;
    _hist_cnt = 0;
    _tick5min = 0;
}

void DryerApp::startDryingFromAdjustedTime(void)
{
    if (_dry_state != DRY_FINISH || _remaining_min <= 0) return;
    const int adjusted_remaining_min = _remaining_min;
    startDrying();
    _remaining_min = adjusted_remaining_min;
    _dry_time_min = adjusted_remaining_min;
}

void DryerApp::enterStandby(void)
{
    _pre_time_remain = 0;
    if (_dryer_settings.standby_enabled == 0 ||
        (_dryer_settings.standby_enabled == 1 &&
         _dryer_settings.standby_time_min == 0)) {
        startDrying();
        return;
    }
    _standby_mode_active = true;
    _dry_state = DRY_PREPARE;
    _set_temp = static_cast<float>(_dryer_settings.standby_temp_c);
    _remaining_min = _dryer_settings.standby_enabled == 2
                   ? 0 : _dryer_settings.standby_time_min;
    _fan_on = true;
    _heater_on = false;
    _heater_enable_after_ms = 0;
    _fan_disable_after_ms = 0;
    _tick_s = 0;
}

void DryerApp::startCooling(void)
{
    _standby_mode_active = false;
    _pre_time_remain = 0;
    _cool_remain = _dryer_settings.cooling_time_min;
    _remaining_min = _cool_remain;
    if (_cool_remain <= 0) {
        finishCycle();
        return;
    }
    _dry_state = DRY_COOL;
    _set_temp = static_cast<float>(_dryer_settings.cooling_temp_c);
    _fan_on = true;
    _heater_on = false;
    _heater_enable_after_ms = 0;
    _fan_disable_after_ms = 0;
    _tick_s = 0;
}

void DryerApp::finishCycle(void)
{
    _standby_mode_active = false;
    _remaining_min = 0;
    _pre_time_remain = 0;
    _cool_remain = 0;
    _set_temp = 0.0F;
    _dry_state = DRY_FINISH;
    _heater_on = false;
    _heater_enable_after_ms = 0;
    _fan_disable_after_ms = _fan_on ? lv_tick_get() + 1000U : 0U;
    _tick_s = 0;
}

void DryerApp::doProgress(void)
{
    switch (_dry_state) {
    case DRY_PREHEAT: enterStandby(); break;
    case DRY_PREPARE:
        if (_standby_mode_active) startDrying();
        else doPreheatStart();
        break;
    case DRY_RUN: startCooling(); break;
    case DRY_COOL: finishCycle(); break;
    case DRY_FINISH: doPreheatStart(); break;
    }
    saveRuntimeState();
    refreshHeader();
    refreshCards();
    refreshFooter();
}

void DryerApp::doApplyProc(void)
{
    _proc_no = _sel_proc;
    applyProcActive();
    saveRuntimeState();
    lv_scr_load(_scr_main);
    _cur_scr = SCR_MAIN;
    refreshHeader();
    refreshCards();
    refreshFooter();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  1-second timer callback
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::cbFanSpinTimer(lv_timer_t *t)
{
    DryerApp *self = static_cast<DryerApp *>(t->user_data);
    if (!self) return;

    if (self->_dot_fan && self->_fan_on) {
        const uint32_t elapsed_ms = lv_tick_elaps(self->_fan_spin_epoch_ms);
        self->_fan_spin_angle = 30 + static_cast<int32_t>((elapsed_ms * 3U) % 3600U);
        drawFanIcon(self->_dot_fan, C_CYAN, C_PANEL,
                    static_cast<float>(self->_fan_spin_angle) * 0.1F);
    }

    if (self->_cur_scr == SCR_MAIN) {
        switch (self->_periodic_ui_refresh_phase) {
            case 1: self->refreshHeader(); self->_periodic_ui_refresh_phase = 2; break;
            case 2: self->refreshCards();  self->_periodic_ui_refresh_phase = 3; break;
            case 3: self->refreshFooter(); self->_periodic_ui_refresh_phase = 0; break;
            default: break;
        }
    } else {
        self->_periodic_ui_refresh_phase = 0;
    }
}

void DryerApp::cbTimer(lv_timer_t *t)
{
    DryerApp *self = (DryerApp *)t->user_data;
    if (s_relay.fanOn()) {
        if (self->_fan_on_elapsed_seconds < FAN_MONITOR_START_DELAY_SECONDS)
            ++self->_fan_on_elapsed_seconds;
    } else {
        self->_fan_on_elapsed_seconds = 0;
    }
    static unsigned heartbeat = 0;
    if ((++heartbeat % 5U) == 0U) {
        printf("DY-EP4 heartbeat: UI running, MQTT=%s, FAN ADC=%d (constant=%d, %.2f m/s)\n",
               self->_mqtt.isConnected() ? "connected" : "waiting-network",
               static_cast<int>(g_dryer_sensor_values.fan_velocity.raw),
               static_cast<int>(g_dryer_sensor_values.fan_velocity.reference_adc),
               static_cast<double>(g_dryer_sensor_values.fan_velocity.velocity_ms));
    }

    /* ── Colon blink ── */
    self->_colon_blink = !self->_colon_blink;

    /* 1. Read every sensor first, regardless of communication state. */
    self->updateSensorValues();
    self->errorCheck();
    const bool mqtt_command_processed = self->processMqttCommands();

    /* 2. Evaluate the operating state using the latest sensor snapshot. */
    const bool standbyHeating = self->_dry_state == DRY_PREPARE &&
        self->_standby_mode_active && self->_set_temp > 0.0F;
    const bool temperatureControl = self->_dry_state == DRY_PREHEAT ||
        self->_dry_state == DRY_RUN || self->_dry_state == DRY_COOL || standbyHeating;
    if (temperatureControl) {
        if (self->_rs485_sensors == nullptr) {
            const float diff = self->_set_temp - self->_cur_temp;
            self->_cur_temp += diff *
                (self->_dry_state == DRY_PREHEAT ? 0.06F : 0.04F);
            if (self->_dry_state == DRY_RUN && self->_humidity > 12.0F)
                self->_humidity -= 0.15F;
        }
        const float onThreshold = self->_set_temp -
            static_cast<float>(self->_dryer_settings.temp_hysteresis_c);
        const bool heaterDelayDone = self->_heater_enable_after_ms == 0U ||
            static_cast<int32_t>(lv_tick_get() - self->_heater_enable_after_ms) >= 0;
        if (!heaterDelayDone) self->_heater_on = false;
        else if (self->_cur_temp <= onThreshold) self->_heater_on = true;
        else if (self->_cur_temp >= self->_set_temp) self->_heater_on = false;
    } else {
        self->_heater_on = false;
    }
    if (self->_dry_state == DRY_FINISH && self->_fan_disable_after_ms != 0U &&
        static_cast<int32_t>(lv_tick_get() - self->_fan_disable_after_ms) >= 0) {
        self->_fan_on = false;
        self->_fan_disable_after_ms = 0;
    }

    /* Automatic damper control with hysteresis:
       open at the configured humidity, close only after humidity falls by
       the configured hysteresis. Between both thresholds retain its state. */
    if (self->_damper_mode == DAMPER_AUTO &&
        g_dryer_sensor_values.temperature_humidity[0].valid) {
        const float open_threshold = static_cast<float>(
            self->_dryer_settings.damper_open_humidity_pct);
        float close_threshold = open_threshold - static_cast<float>(
            self->_dryer_settings.damper_hysteresis_pct);
        if (close_threshold < 0.0F) close_threshold = 0.0F;
        const float humidity =
            g_dryer_sensor_values.temperature_humidity[0].humidity_pct;
        if (humidity >= open_threshold) self->_damper_percent = 100.0F;
        else if (humidity <= close_threshold) self->_damper_percent = 0.0F;
    }

    /* ── Countdown ──────────────────────────────────────────────── */
    const bool timedState = self->_dry_state == DRY_PREHEAT ||
        self->_dry_state == DRY_RUN || self->_dry_state == DRY_COOL ||
        (self->_dry_state == DRY_PREPARE && self->_standby_mode_active &&
         self->_dryer_settings.standby_enabled == 1);
    if (timedState && self->_remaining_min == 0) {
        if (self->_dry_state == DRY_PREHEAT) self->enterStandby();
        else if (self->_dry_state == DRY_PREPARE) self->startDrying();
        else if (self->_dry_state == DRY_RUN) self->startCooling();
        else if (self->_dry_state == DRY_COOL) self->finishCycle();
        self->saveRuntimeState();
    } else if (timedState && self->_remaining_min > 0 &&
        ++self->_tick_s >= DRYER_COUNTDOWN_TICKS_PER_MINUTE) {
        self->_tick_s = 0;
        --self->_remaining_min;
        if (self->_dry_state == DRY_PREHEAT)
            self->_pre_time_remain = self->_remaining_min;
        else if (self->_dry_state == DRY_COOL)
            self->_cool_remain = self->_remaining_min;
        if (self->_remaining_min == 0) {
            if (self->_dry_state == DRY_PREHEAT) self->enterStandby();
            else if (self->_dry_state == DRY_PREPARE) self->startDrying();
            else if (self->_dry_state == DRY_RUN) self->startCooling();
            else if (self->_dry_state == DRY_COOL) self->finishCycle();
        }
        self->saveRuntimeState();
    }

    /* ── Graph history (removed — no chart in new layout) ── */

    /* 3. Apply safety checks, then write all three outputs every second. */
    self->applyActuatorSafetyChecks();
    self->writeActuatorOutputs();

    /* Commands and countdown can change these after the sensor read above. */
    g_dryer_sensor_values.remaining_time_min = self->_remaining_min;
    g_dryer_sensor_values.target_temperature_c = self->_set_temp;
    g_dryer_sensor_values.operating_state = self->_dry_state;
    g_dryer_sensor_values.damper_percent = self->_damper_percent;

    /* 4. A valid server command always gets a post-control telemetry response.
       This immediate response does not disturb the configured periodic schedule. */
    const uint32_t telemetry_period_s = static_cast<uint32_t>(
        self->_dryer_settings.mqtt_publish_interval_min) * 60U;
    const bool periodic_telemetry_due =
        ++self->_mqtt_publish_elapsed_s >= telemetry_period_s;
    bool command_telemetry_published = false;
    if (mqtt_command_processed) {
        command_telemetry_published =
            self->_mqtt.publishTelemetry(g_dryer_sensor_values);
        /* Avoid a duplicate only when the normal interval is due in this same loop. */
        if (command_telemetry_published && periodic_telemetry_due)
            self->_mqtt_publish_elapsed_s = 0;
    }
    if (periodic_telemetry_due && !command_telemetry_published) {
        if (self->_mqtt.publishTelemetry(g_dryer_sensor_values))
            self->_mqtt_publish_elapsed_s = 0;
        else if (self->_mqtt_publish_elapsed_s > telemetry_period_s)
            self->_mqtt_publish_elapsed_s = telemetry_period_s;
    }

    /* ── Refresh UI ── */
    if (self->_cur_scr == SCR_MAIN) {
        self->_periodic_ui_refresh_phase = 1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Business logic
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::doPreheatStart(void)
{
    _standby_mode_active = false;
    _pre_temp = _dryer_settings.preheat_temp_c;
    _pre_time_min = _dryer_settings.preheat_time_min;
    _set_temp = static_cast<float>(_pre_temp);
    _pre_time_remain = _pre_time_min;
    _remaining_min = _pre_time_remain;
    _cool_remain = 0;
    _dry_state = DRY_PREHEAT;
    _fan_on = true;
    _heater_on = false;
    _heater_enable_after_ms = lv_tick_get() + 1000U;
    _fan_disable_after_ms = 0;
    _tick_s = 0;
    saveRuntimeState();
    refreshHeader();
    refreshCards();
    refreshFooter();
}

void DryerApp::doPreheatStop(void)
{
    finishCycle();
    saveRuntimeState();
    refreshHeader();
    refreshCards();
    refreshFooter();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Button callbacks
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::cbBtnSet(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->_sel_proc = self->_proc_no;
    self->refreshProcSelect();
    lv_scr_load(self->_scr_proc);
    self->_cur_scr = SCR_PROC_SELECT;
}

void DryerApp::cbBtnPreheatStart(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->doPreheatStart();
}

void DryerApp::cbBtnPreheatStop(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->doPreheatStop();
}

void DryerApp::cbBtnPreTempUp(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_pre_temp < 90) self->_pre_temp++;
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnPreTempDn(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_pre_temp > 30) self->_pre_temp--;
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnPreTimeUp(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_pre_time_min < 120) self->_pre_time_min += 1;  /* +1 min (single click) */
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnPreTimeDn(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_pre_time_min > 0) self->_pre_time_min -= 1;
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnDryTempUp(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_set_temp < 90.0f) self->_set_temp += 1.0f;
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnDryTempDn(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_set_temp > 10.0f) self->_set_temp -= 1.0f;
    self->saveRuntimeState();
    self->refreshCards();
}

void DryerApp::cbBtnDryTimeUp(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    /* 항상 _remaining_min 조작 (상단 남은시간과 동기) */
    if (self->_remaining_min < 1440) self->_remaining_min += 1;
    self->startDryingFromAdjustedTime();
    if (self->_dry_state == DRY_PREHEAT) self->_pre_time_remain = self->_remaining_min;
    if (self->_dry_state == DRY_COOL) self->_cool_remain = self->_remaining_min;
    self->saveRuntimeState();
    self->refreshHeader();
    self->refreshCards();
    self->refreshFooter();
}

void DryerApp::cbBtnDryTimeDn(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    if (self->_remaining_min > 0) self->_remaining_min -= 1;
    if (self->_dry_state == DRY_PREHEAT) self->_pre_time_remain = self->_remaining_min;
    if (self->_dry_state == DRY_COOL) self->_cool_remain = self->_remaining_min;
    self->saveRuntimeState();
    self->refreshHeader();
    self->refreshCards();
}

/* Long-press repeat callback shared by all time Up/Dn buttons.
 * Step schedule (LV_EVENT_LONG_PRESSED_REPEAT fires ~every 100 ms):
 *   repeat  0-10  (~1 s)  : +/-  1 min
 *   repeat 10-25  (~1.5 s): +/- 10 min
 *   repeat 25-50  (~2.5 s): +/- 30 min
 *   repeat 50+    (>5 s)  : +/- 60 min (1 hour)
 */
void DryerApp::cbTimeRepeat(lv_event_t *e)
{
    TimeCtx *ctx = (TimeCtx *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED) {
        ctx->app->_btn_repeat_cnt = 0;
        return;
    }
    /* LV_EVENT_LONG_PRESSED_REPEAT */
    int cnt = ++ctx->app->_btn_repeat_cnt;
    int step = (cnt < 10) ? 1 : (cnt < 25) ? 10 : (cnt < 50) ? 30 : 60;

    /* DRY TIME 콘텍스트는 항상 _remaining_min 조작 */
    bool is_dry_ctx = (ctx->setting == &ctx->app->_dry_time_min);
    int *target = is_dry_ctx ? &ctx->app->_remaining_min : ctx->setting;

    int newval = *target + ctx->dir * step;
    if (newval < 0)             newval = 0;
    if (newval > ctx->max_val)  newval = ctx->max_val;
    *target = newval;
    if (is_dry_ctx) {
        ctx->app->startDryingFromAdjustedTime();
        ctx->app->saveRuntimeState();
        ctx->app->refreshHeader();
        ctx->app->refreshFooter();
    } else {
        ctx->app->saveRuntimeState();
    }
    ctx->app->refreshCards();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sensor read stub
 *  _sensors[0] = outside air (외기)
 *  _sensors[1] = internal chamber (내부)
 *  _sensors[2] = outlet / exhaust (출구)
 *  Replace TODO lines with real hardware I2C/SPI/ADC calls.
 * ═══════════════════════════════════════════════════════════════════════════ */
void DryerApp::updateSensorValues(void)
{
    DryerSensorValues &values = g_dryer_sensor_values;
    const TickType_t now = xTaskGetTickCount();

    if (_rs485_sensors != nullptr) {
        const RS485Sensor::Reading control = _rs485_sensors->controlReading();
        TemperatureHumidityValue &control_value = values.temperature_humidity[0];
        control_value.valid = control.valid;
        if (control.valid) {
            control_value.temperature_c = control.temperatureC + _temp_cal[0];
            control_value.humidity_pct = control.humidityPercent + _hum_cal[0];
            control_value.updated_at = control.updatedAt;
        }

        for (size_t i = 0; i < RS485Sensor::kMonitorCount; ++i) {
            const RS485Sensor::Reading monitor = _rs485_sensors->monitoringReading(i);
            TemperatureHumidityValue &sensor = values.temperature_humidity[i + 1];
            sensor.valid = monitor.valid;
            if (monitor.valid) {
                sensor.temperature_c = monitor.temperatureC + _temp_cal[i + 1];
                sensor.humidity_pct = monitor.humidityPercent + _hum_cal[i + 1];
                sensor.updated_at = monitor.updatedAt;
            }
        }

        if (_load_cell != nullptr) {
            values.load_cell = _load_cell->reading();
            if (_rs485_sensors->loadCellFailureCount() >= SENSOR_COMM_FAILURE_LIMIT)
                values.load_cell.valid = false;
            if (_cur_scr == SCR_WEIGHT_CALIBRATION) refreshWeightCalibration();
        } else {
            values.load_cell.valid = false;
        }
    } else {
        /* Demo snapshot when the RS485 source has not been attached. */
        for (size_t i = 0; i < DRYER_SENSOR_COUNT; ++i) {
            TemperatureHumidityValue &sensor = values.temperature_humidity[i];
            sensor.temperature_c = (i == 1) ? 22.0F : _cur_temp;
            sensor.humidity_pct = (i == 1) ? 55.0F : _humidity;
            sensor.updated_at = now;
            sensor.valid = true;
        }
        values.load_cell.raw = 0;
        values.load_cell.weight_g = _weight_g;
        values.load_cell.updated_at = now;
        values.load_cell.valid = true;
    }

    const bool fan_monitor_ready = _dry_state != DRY_FINISH &&
        s_relay.fanOn() && _fan_on &&
        _fan_on_elapsed_seconds >= FAN_MONITOR_START_DELAY_SECONDS;
    FanCurrentSensorValue fan_current{};
    if (fan_monitor_ready && s_fan.read(fan_current) == ESP_OK) {
        values.fan_velocity.raw = fan_current.raw;
        values.fan_velocity.reference_adc = _dryer_settings.fan_adc_at_10ms;
        values.fan_velocity.velocity_ms =
            static_cast<float>(fan_current.raw) * 10.0F /
            static_cast<float>(_dryer_settings.fan_adc_at_10ms);
        values.fan_velocity.valid = fan_current.valid;
    } else {
        values.fan_velocity.raw = 0;
        values.fan_velocity.reference_adc = _dryer_settings.fan_adc_at_10ms;
        values.fan_velocity.valid = false;
        values.fan_velocity.velocity_ms = 0.0F;
    }

    values.door.raw_level = _door_sensor.initialized() ? _door_sensor.rawLevel() : 0;
    values.door.open = values.door.raw_level != 0;
    values.door.valid = _door_sensor.initialized();

    _blower_speed_ms = values.fan_velocity.valid
        ? values.fan_velocity.velocity_ms : 0.0F;
    values.remaining_time_min = _remaining_min;
    values.target_temperature_c = _set_temp;
    values.operating_state = _dry_state;
    values.damper_percent = _damper_percent;
    values.updated_at = now;
    ++values.sequence;

    /* Keep the existing control and UI fields synchronized. */
    if (values.temperature_humidity[0].valid) {
        _cur_temp = values.temperature_humidity[0].temperature_c;
        _humidity = values.temperature_humidity[0].humidity_pct;
    }
    for (size_t i = 0; i < 6; ++i) {
        const TemperatureHumidityValue &sensor = values.temperature_humidity[i + 1];
        if (sensor.valid) {
            _sensors[i].temp = sensor.temperature_c;
            _sensors[i].hum = sensor.humidity_pct;
        }
    }
    if (values.load_cell.valid) _weight_g = values.load_cell.weight_g;
    if (values.door.valid) _door_open = values.door.open;
}

void DryerApp::errorCheck(void)
{
    const EVENT_INFO previous = _previous_event_info;
    EVENT_INFO current = g_alarm_info;
    const DryerSensorValues &values = g_dryer_sensor_values;

    current.door_open = values.door.valid && values.door.open;

    current.control_sensor_error = _rs485_sensors != nullptr &&
        _rs485_sensors->controlFailureCount() >= SENSOR_COMM_FAILURE_LIMIT;
    current.weight_sensor_error = _rs485_sensors != nullptr &&
        _rs485_sensors->loadCellFailureCount() >= SENSOR_COMM_FAILURE_LIMIT;

    /* A stopped/finishing fan normally decays through 1 m/s to 0 m/s.
       Running-speed alarms apply only while the relay is actually on and
       the dryer is in an operating state. */
    const bool fan_monitor_ready = _dry_state != DRY_FINISH &&
        s_relay.fanOn() && _fan_on &&
        _fan_on_elapsed_seconds >= FAN_MONITOR_START_DELAY_SECONDS;
    current.fan_min_error = fan_monitor_ready && values.fan_velocity.valid &&
        values.fan_velocity.velocity_ms <
            static_cast<float>(_dryer_settings.fan_min_speed_ms);
    current.fan_max_error = fan_monitor_ready && values.fan_velocity.valid &&
        values.fan_velocity.velocity_ms > FAN_SPEED_MAX_MPS;

    const bool heating_state = _dry_state == DRY_PREHEAT || _dry_state == DRY_RUN;
    const bool control_temperature_valid = values.temperature_humidity[0].valid;
    const float control_temperature = values.temperature_humidity[0].temperature_c;
    const float target_temperature = _set_temp;

    if (!heating_state) {
        _target_reach_elapsed_seconds = 0;
        _target_temperature_reached = false;
        current.under_heat = 0;
    } else {
        if (control_temperature_valid && control_temperature >= target_temperature)
            _target_temperature_reached = true;
        if (!_target_temperature_reached && _target_reach_elapsed_seconds < UINT32_MAX)
            ++_target_reach_elapsed_seconds;
        const uint32_t reach_limit_seconds =
            static_cast<uint32_t>(_dryer_settings.low_warning_reach_time_min) * 60U;
        current.under_heat = !_target_temperature_reached &&
            _target_reach_elapsed_seconds >= reach_limit_seconds;
    }

    const bool heater_relay_on = g_alarm_info.heater_relay_on != 0;
    if (!heater_relay_on || !control_temperature_valid) {
        _heater_continuous_on_seconds = 0;
        _heater_on_start_temperature_valid = false;
        current.heater1_error = 0;
    } else {
        if (!_heater_on_start_temperature_valid) {
            _heater_on_start_temperature = control_temperature;
            _heater_on_start_temperature_valid = true;
            _heater_continuous_on_seconds = 0;
        }
        if (_heater_continuous_on_seconds < UINT32_MAX)
            ++_heater_continuous_on_seconds;
        current.heater1_error =
            _heater_continuous_on_seconds >= HEATER_NO_RISE_CONFIRM_SECONDS &&
            control_temperature <
                _heater_on_start_temperature + HEATER_MIN_DETECTABLE_RISE_C;
    }

    const bool above_high_limit = control_temperature_valid &&
        control_temperature >
            static_cast<float>(_dryer_settings.high_warning_temp_c);
    if (above_high_limit) {
        if (_over_heat_seconds < OVER_HEAT_CONFIRM_SECONDS) ++_over_heat_seconds;
    } else {
        _over_heat_seconds = 0;
    }
    current.over_heat = _over_heat_seconds >= OVER_HEAT_CONFIRM_SECONDS;
    current.thermo_state = current.control_sensor_error;
    current.mqtt_connect = _mqtt.isConnected() ? 1U : 0U;

    g_alarm_info = current;
    g_dryer_sensor_values.event = current;

    const uint16_t changed = previous.data ^ current.data;
    for (uint8_t bit = 0; bit < 16; ++bit) {
        const uint16_t mask = static_cast<uint16_t>(1U << bit);
        if ((changed & mask) != 0U)
            processAlarmEvent(bit, (current.data & mask) != 0U);
    }
    if (changed != 0U)
        _mqtt.publishEvent(current.data);
    _previous_event_info = current;
}

bool DryerApp::processMqttCommands(void)
{
    MqttCommand command{};
    bool processed = false;
    while (_mqtt.popCommand(command)) {
        bool commandApplied = false;
        switch (command.type) {
        case MqttCommandType::SetTemperature:
            if (command.value >= DRYER_CFG_TEMP_MIN_C && command.value <= DRYER_CFG_TEMP_MAX_C) {
                _set_temp = static_cast<float>(command.value);
                saveRuntimeState();
                commandApplied = true;
            }
            break;
        case MqttCommandType::SetTime:
            if (command.value >= DRYER_CFG_TIME_MIN_MIN && command.value <= DRYER_CFG_TIME_MIN_MAX) {
                _remaining_min = command.value;
                startDryingFromAdjustedTime();
                saveRuntimeState();
                commandApplied = true;
            }
            break;
        case MqttCommandType::PreheatStart:
            if ((!command.hasTemperature ||
                 (command.temperature >= DRYER_CFG_PREHEAT_TEMP_MIN_C &&
                  command.temperature <= DRYER_CFG_TEMP_MAX_C)) &&
                (!command.hasTime ||
                 (command.timeMinutes >= DRYER_CFG_TIME_MIN_MIN &&
                  command.timeMinutes <= DRYER_CFG_TIME_MIN_MAX))) {
                if (command.hasTemperature) {
                    _pre_temp = command.temperature;
                    _dryer_settings.preheat_temp_c = command.temperature;
                }
                if (command.hasTime) {
                    _pre_time_min = command.timeMinutes;
                    _dryer_settings.preheat_time_min = command.timeMinutes;
                }
                if (command.damperMode >= 0) {
                    _damper_mode = static_cast<DamperMode>(command.damperMode);
                    _damper_percent = command.damperMode == 1 ? 100.0F :
                                       command.damperMode == 2 ? 0.0F : _damper_percent;
                }
                s_nvs.saveCoolingSettings(_dryer_settings);
                doPreheatStart();
                commandApplied = true;
            }
            break;
        case MqttCommandType::DryStart:
            if ((!command.hasTemperature ||
                 (command.temperature >= DRYER_CFG_TEMP_MIN_C &&
                  command.temperature <= DRYER_CFG_TEMP_MAX_C)) &&
                (!command.hasTime ||
                 (command.timeMinutes >= DRYER_CFG_TIME_MIN_MIN &&
                  command.timeMinutes <= DRYER_CFG_TIME_MIN_MAX))) {
                if (command.hasTemperature)
                    _dryer_settings.dry_temp_c = command.temperature;
                if (command.hasTime)
                    _dryer_settings.dry_time_min = command.timeMinutes;
                if (command.damperMode >= 0) {
                    _dryer_settings.damper_mode = command.damperMode;
                    _damper_mode = static_cast<DamperMode>(command.damperMode);
                    _damper_percent = command.damperMode == 1 ? 100.0F :
                                       command.damperMode == 2 ? 0.0F : _damper_percent;
                }
                s_nvs.saveCoolingSettings(_dryer_settings);
                _dry_state = DRY_FINISH;
                doStartStop();
                commandApplied = true;
            }
            break;
        case MqttCommandType::DryStop:
            if (_dry_state != DRY_FINISH) doPreheatStop();
            _set_temp = 0.0F;
            _remaining_min = 0;
            if (command.damperMode >= 0) {
                _damper_mode = static_cast<DamperMode>(command.damperMode);
                _damper_percent = command.damperMode == 1 ? 100.0F :
                                   command.damperMode == 2 ? 0.0F : _damper_percent;
            }
            saveRuntimeState();
            commandApplied = true;
            break;
        case MqttCommandType::DamperAuto:  _damper_mode = DAMPER_AUTO; commandApplied = true; break;
        case MqttCommandType::DamperOpen:  _damper_mode = DAMPER_OPEN; _damper_percent = 100.0F; commandApplied = true; break;
        case MqttCommandType::DamperClose: _damper_mode = DAMPER_CLOSE; _damper_percent = 0.0F; commandApplied = true; break;
        case MqttCommandType::SetEquipmentName:
            snprintf(_equipment_name, sizeof(_equipment_name), "%s", command.text);
            _equipment_id = command.equipmentId;
            s_nvs.saveEquipmentInfo(_equipment_name, _equipment_id);
            if (_lbl_equipment_name) {
                lv_canvas_fill_bg(_lbl_equipment_name,lv_color_hex(0x12324A),LV_OPA_COVER);
                const int nameScale=ycb_hangul_measure(_equipment_name,2)<=296?2:1;
                const int nameWidth=ycb_hangul_measure(_equipment_name,nameScale);
                int nameX=(296-nameWidth)/2;
                if(nameX<0)nameX=0;
                const int nameY=(40-16*nameScale)/2;
                ycb_hangul_draw(_lbl_equipment_name,nameX,nameY,C_WHITE,
                                lv_color_hex(0x12324A),_equipment_name,nameScale,false);
            }
            if (_lbl_device_id) {
                char idText[20];
                snprintf(idText,sizeof(idText),"%06ld",static_cast<long>(_equipment_id));
                lv_label_set_text(_lbl_device_id,idText);
            }
            commandApplied = true;
            break;
        default: break;
        }
        if (command.type == MqttCommandType::DamperAuto ||
            command.type == MqttCommandType::DamperOpen ||
            command.type == MqttCommandType::DamperClose) {
            _dryer_settings.damper_mode = static_cast<int32_t>(_damper_mode);
            s_nvs.saveCoolingSettings(_dryer_settings);
        }
        if (commandApplied) {
            processed = true;
            _mqtt.recordCommandHistory(command);
        }
    }
    if (processed) {
        refreshHeader();
        refreshCards();
        refreshFooter();
    }
    return processed;
}

void DryerApp::showErrorPopup(const char *title, const char *line1, const char *line2)
{
    constexpr int canvas_width = 560;
    if (_notice_popup_timer) {
        lv_timer_del(_notice_popup_timer);
        _notice_popup_timer = nullptr;
    }
    if (_notice_popup_overlay) {
        lv_obj_del(_notice_popup_overlay);
        _notice_popup_overlay = nullptr;
    }
    if (_error_popup_overlay) lv_obj_del(_error_popup_overlay);
    _error_popup_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(_error_popup_overlay,0,0);
    lv_obj_set_size(_error_popup_overlay,1024,600);
    lv_obj_set_style_bg_color(_error_popup_overlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(_error_popup_overlay,LV_OPA_60,0);
    lv_obj_set_style_border_width(_error_popup_overlay,0,0);
    lv_obj_set_style_pad_all(_error_popup_overlay,0,0);
    lv_obj_clear_flag(_error_popup_overlay,LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(_error_popup_overlay);
    lv_obj_set_size(box,640,330); lv_obj_center(box);
    lv_obj_set_style_bg_color(box,C_PANEL,0);
    lv_obj_set_style_bg_opa(box,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(box,C_RED,0);
    lv_obj_set_style_border_width(box,5,0);
    lv_obj_set_style_radius(box,10,0);
    lv_obj_clear_flag(box,LV_OBJ_FLAG_SCROLLABLE);

    if(!s_error_title_buf)s_error_title_buf=allocCanvasBuffer(canvas_width*52);
    if(!s_error_line1_buf)s_error_line1_buf=allocCanvasBuffer(canvas_width*42);
    if(!s_error_line2_buf)s_error_line2_buf=allocCanvasBuffer(canvas_width*42);
    if(!s_error_ok_buf)s_error_ok_buf=allocCanvasBuffer(150*50);
    auto drawLine=[&](lv_color_t *buffer,int height,const char *text,int y,
                      lv_color_t color,int scale){
        lv_obj_t *canvas=lv_canvas_create(box);
        lv_canvas_set_buffer(canvas,buffer,canvas_width,height,LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(canvas,C_PANEL,LV_OPA_COVER);
        int x=(canvas_width-ycb_hangul_measure(text,scale))/2;if(x<0)x=0;
        ycb_hangul_draw(canvas,x,(height-16*scale)/2,color,C_PANEL,text,scale,false);
        lv_obj_set_pos(canvas,35,y);
        lv_obj_clear_flag(canvas,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    };
    drawLine(s_error_title_buf,52,title,22,C_YELLOW,2);
    drawLine(s_error_line1_buf,42,line1,104,C_WHITE,1);
    drawLine(s_error_line2_buf,42,line2,150,C_WHITE,1);

    lv_obj_t *ok=makeBtn(box,245,230,150,58,"",C_BLUE,cbErrorPopupOk,this);
    lv_obj_t *ok_canvas=lv_canvas_create(ok);
    lv_canvas_set_buffer(ok_canvas,s_error_ok_buf,150,50,LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(ok_canvas,C_BLUE,LV_OPA_COVER);
    const char *ok_text="확인";
    int ok_x=(150-ycb_hangul_measure(ok_text,2))/2;
    ycb_hangul_draw(ok_canvas,ok_x,9,C_WHITE,C_BLUE,ok_text,2,false);
    lv_obj_center(ok_canvas);
    lv_obj_clear_flag(ok_canvas,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
}

void DryerApp::showNoticePopup(const char *message)
{
    if (!message) return;
    if (_notice_popup_timer) {
        lv_timer_del(_notice_popup_timer);
        _notice_popup_timer = nullptr;
    }
    if (_notice_popup_overlay) lv_obj_del(_notice_popup_overlay);

    _notice_popup_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(_notice_popup_overlay, 0, 0);
    lv_obj_set_size(_notice_popup_overlay, 1024, 600);
    lv_obj_set_style_bg_color(_notice_popup_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_notice_popup_overlay, LV_OPA_40, 0);
    lv_obj_set_style_border_width(_notice_popup_overlay, 0, 0);
    lv_obj_set_style_pad_all(_notice_popup_overlay, 0, 0);
    lv_obj_clear_flag(_notice_popup_overlay, LV_OBJ_FLAG_SCROLLABLE);

    const lv_color_t noticeBg = lv_color_hex(0x164D38);
    lv_obj_t *box = lv_obj_create(_notice_popup_overlay);
    lv_obj_set_size(box, 480, 150);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, noticeBg, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, C_CYAN, 0);
    lv_obj_set_style_border_width(box, 4, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int noticeWidth = 420;
    constexpr int noticeHeight = 60;
    if (!s_notice_message_buf)
        s_notice_message_buf = allocCanvasBuffer(noticeWidth * noticeHeight);
    lv_obj_t *canvas = lv_canvas_create(box);
    lv_canvas_set_buffer(canvas, s_notice_message_buf, noticeWidth, noticeHeight,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(canvas, noticeBg, LV_OPA_COVER);
    int x = (noticeWidth - ycb_hangul_measure(message, 2)) / 2;
    if (x < 0) x = 0;
    ycb_hangul_draw(canvas, x, 14, C_WHITE, noticeBg, message, 2, false);
    lv_obj_center(canvas);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    _notice_popup_timer = lv_timer_create(cbNoticePopupTimer, 1500, this);
    lv_timer_set_repeat_count(_notice_popup_timer, 1);
}

void DryerApp::cbNoticePopupTimer(lv_timer_t *timer)
{
    DryerApp *self = static_cast<DryerApp *>(timer ? timer->user_data : nullptr);
    if (!self) return;
    if (self->_notice_popup_overlay) {
        lv_obj_del(self->_notice_popup_overlay);
        self->_notice_popup_overlay = nullptr;
    }
    self->_notice_popup_timer = nullptr;
}

void DryerApp::cbErrorPopupOk(lv_event_t *e)
{
    DryerApp *self=static_cast<DryerApp*>(lv_event_get_user_data(e));
    if(!self)return;
    if(self->_error_popup_overlay){lv_obj_del(self->_error_popup_overlay);self->_error_popup_overlay=nullptr;}
    lv_scr_load(self->_scr_main);
    self->_cur_scr=SCR_MAIN;
    self->refreshHeader();self->refreshCards();self->refreshFooter();
}

void DryerApp::processAlarmEvent(uint8_t bit, bool active)
{
    static const char *const names[16] = {
        "DOOR_OPEN", "CONTROL_SENSOR_ERROR", "WEIGHT_SENSOR_ERROR", "THERMO_STATE",
        "FAN_MIN_ERROR", "FAN_MAX_ERROR", "HEATER1_ERROR", "HEATER2_ERROR",
        "MEM_ERROR", "UNDER_HEAT", "OVER_HEAT", "FAN_RELAY_ON",
        "HEATER_RELAY_ON", "DAMPER_RELAY_ON", "X14", "PWR_ON"
    };
    if (bit >= 16) return;
    if (bit == 9) _footer_icon_state_valid = false;
    if (active && (bit == 1 || bit == 4 || bit == 5 || bit == 10)) {
        _dry_state = DRY_FINISH;
        _remaining_min = 0;
        _pre_time_remain = 0;
        _tick_s = 0;
        _fan_on = false;
        _heater_on = false;
        saveRuntimeState();
        if (bit == 1)
            showErrorPopup("제어 센서 통신 오류",
                           "RS485 주소 100 통신이 5회 연속 실패했습니다.",
                           "건조기를 정지했습니다. 센서와 배선을 확인하세요.");
        else if (bit == 4)
            showErrorPopup("팬 저속 오류",
                           "팬 동작 중 풍속이 5.0 m/s 미만입니다.",
                           "정지했습니다. 팬, 벨트, 흡입구와 ADC를 확인하세요.");
        else if (bit == 5)
            showErrorPopup("팬 과속 오류",
                           "팬 동작 중 풍속이 15.0 m/s를 초과했습니다.",
                           "정지했습니다. 팬 구동상태와 ADC를 확인하세요.");
        else
            showErrorPopup("고온 오류",
                           "설정된 고온 경고값을 5초 이상 초과했습니다.",
                           "건조기를 정지했습니다. 히터와 센서를 확인하세요.");
    }
    if (active && bit == 9)
        showErrorPopup("저온 오류",
                       "설정된 도달시간 안에 목표온도에 도달하지 못했습니다.",
                       "히터 전원과 건조실의 열 손실을 확인하세요.");
    if (active && bit == 6)
        showErrorPopup("히터 1 오류",
                       "히터가 20분 동안 켜졌지만 온도가 상승하지 않았습니다.",
                       "히터 전원, 접촉기 및 온도센서를 확인하세요.");
    printf("ALARM EVENT: bit=%u name=%s state=%s data=0x%04X\n",
           static_cast<unsigned>(bit), names[bit], active ? "ON" : "OFF",
           static_cast<unsigned>(g_alarm_info.data));
}

void DryerApp::applyActuatorSafetyChecks(void)
{
    const TemperatureHumidityValue &control =
        g_dryer_sensor_values.temperature_humidity[0];
    const bool heating_state = _dry_state == DRY_PREHEAT || _dry_state == DRY_RUN ||
        _dry_state == DRY_COOL ||
        (_dry_state == DRY_PREPARE && _standby_mode_active && _set_temp > 0.0F);
    const bool over_temperature = g_alarm_info.over_heat != 0;
    const bool control_sensor_error = g_alarm_info.control_sensor_error != 0;

    /* Door state is informational only and does not interlock the heater. */
    if (!heating_state || !control.valid || over_temperature || control_sensor_error) {
        _heater_on = false;
    }
    if (control_sensor_error || over_temperature) _fan_on = false;
}

void DryerApp::writeActuatorOutputs(void)
{
    /* This is the single runtime GPIO output point; cbTimer calls it every second. */
    /* GPIO47 is active-high: 100% (OPEN) = HIGH, otherwise (OFF) = LOW. */
    const bool damper_open = _damper_percent >= 100.0F;
    s_relay.setAll(_fan_on, _heater_on, damper_open);
    g_alarm_info.fan_relay_on = s_relay.fanOn();
    g_alarm_info.heater_relay_on = s_relay.heaterOn();
    g_alarm_info.damper_relay_on = s_relay.damperOn();
    g_dryer_sensor_values.event = g_alarm_info;
}

void DryerApp::doCycleDamper(void)
{
    /* Main-screen touch sequence: 0% -> AUTO -> 100% -> 0%. */
    if (_damper_mode == DAMPER_CLOSE) _damper_mode = DAMPER_AUTO;
    else if (_damper_mode == DAMPER_AUTO) _damper_mode = DAMPER_OPEN;
    else _damper_mode = DAMPER_CLOSE;
    if (_damper_mode == DAMPER_OPEN) _damper_percent = 100.0F;
    else if (_damper_mode == DAMPER_CLOSE) _damper_percent = 0.0F;
    _dryer_settings.damper_mode = static_cast<int32_t>(_damper_mode);
    s_nvs.saveCoolingSettings(_dryer_settings);

    /* Apply GPIO47 immediately instead of waiting for the 1-second timer. */
    writeActuatorOutputs();
    refreshCards();
    refreshFooter();
}

void DryerApp::cbLogoVision(lv_event_t *e)
{
    DryerApp *self=static_cast<DryerApp*>(lv_event_get_user_data(e));
    const esp_err_t err=self->_vision.toggle(self->_scr_main);
    if(err!=ESP_OK)printf("Camera preview failed: %s\n",esp_err_to_name(err));
}

void DryerApp::cbAdjustRepeat(lv_event_t *e)
{
    DryerApp *self = static_cast<DryerApp *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        self->_btn_repeat_cnt = 0;
        return;
    }

    /* Repeat arrives about every 100 ms after the long-press threshold. */
    const int count = ++self->_btn_repeat_cnt;
    const int step = (count < 15) ? 5 : 10;
    lv_obj_t *button = lv_event_get_target(e);
    const int direction = (button == self->_btn_dry_temp_dn ||
                           button == self->_btn_dry_time_dn ||
                           button == self->_btn_pre_temp_dn ||
                           button == self->_btn_pre_time_dn) ? -1 : 1;

    if (button == self->_btn_dry_temp_up || button == self->_btn_dry_temp_dn) {
        self->_set_temp += static_cast<float>(direction * step);
        if (self->_set_temp < 10.0f) self->_set_temp = 10.0f;
        if (self->_set_temp > 90.0f) self->_set_temp = 90.0f;
    } else if (button == self->_btn_pre_temp_up || button == self->_btn_pre_temp_dn) {
        self->_pre_temp += direction * step;
        if (self->_pre_temp < 30) self->_pre_temp = 30;
        if (self->_pre_temp > 90) self->_pre_temp = 90;
    } else if (button == self->_btn_pre_time_up || button == self->_btn_pre_time_dn) {
        self->_pre_time_min += direction * step;
        if (self->_pre_time_min < 0) self->_pre_time_min = 0;
        if (self->_pre_time_min > 120) self->_pre_time_min = 120;
    } else {
        self->_remaining_min += direction * step;
        if (self->_remaining_min < 0) self->_remaining_min = 0;
        if (self->_remaining_min > 1440) self->_remaining_min = 1440;
        self->startDryingFromAdjustedTime();
        self->saveRuntimeState();
    }
    if (button == self->_btn_pre_temp_up || button == self->_btn_pre_temp_dn ||
        button == self->_btn_pre_time_up || button == self->_btn_pre_time_dn) {
        self->saveRuntimeState();
    }
    if (button == self->_btn_dry_temp_up || button == self->_btn_dry_temp_dn) {
        self->saveRuntimeState();
    }
    self->refreshHeader();
    self->refreshCards();
    self->refreshFooter();
    Buzzer::instance().keyTone(25);
}

void DryerApp::cbOpenCalibration(lv_event_t *e)
{
    DryerApp*self=static_cast<DryerApp*>(lv_event_get_user_data(e));memcpy(self->_temp_cal_backup,self->_temp_cal,sizeof(self->_temp_cal));memcpy(self->_hum_cal_backup,self->_hum_cal,sizeof(self->_hum_cal));self->_cal_item=0;self->refreshCalibration();lv_scr_load(self->_scr_cal);self->_cur_scr=SCR_CALIBRATION;
}
void DryerApp::cbCalSensorPrev(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_cal_item=(s->_cal_item+13)%14;s->refreshCalibration();}
void DryerApp::cbCalSensorNext(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_cal_item=(s->_cal_item+1)%14;s->refreshCalibration();}
void DryerApp::cbCalTempDn(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));int i=s->_cal_item/2;if((s->_cal_item%2)==0){s->_temp_cal[i]-=0.1f;if(s->_temp_cal[i]<-20)s->_temp_cal[i]=-20;}else{s->_hum_cal[i]-=0.1f;if(s->_hum_cal[i]<-50)s->_hum_cal[i]=-50;}s->refreshCalibration();}
void DryerApp::cbCalTempUp(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));int i=s->_cal_item/2;if((s->_cal_item%2)==0){s->_temp_cal[i]+=0.1f;if(s->_temp_cal[i]>20)s->_temp_cal[i]=20;}else{s->_hum_cal[i]+=0.1f;if(s->_hum_cal[i]>50)s->_hum_cal[i]=50;}s->refreshCalibration();}
void DryerApp::cbCalHumDn(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_hum_cal[s->_cal_sensor]-=0.1f;if(s->_hum_cal[s->_cal_sensor]<-50)s->_hum_cal[s->_cal_sensor]=-50;s->refreshCalibration();}
void DryerApp::cbCalHumUp(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_hum_cal[s->_cal_sensor]+=0.1f;if(s->_hum_cal[s->_cal_sensor]>50)s->_hum_cal[s->_cal_sensor]=50;s->refreshCalibration();}
void DryerApp::cbCalSaveBack(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));DryerNvsCalibration value{},verify{};memcpy(value.temperature,s->_temp_cal,sizeof(s->_temp_cal));memcpy(value.humidity,s->_hum_cal,sizeof(s->_hum_cal));if(!s_nvs.saveCalibration(value)||!s_nvs.loadCalibration(&verify)){printf("Calibration SAVE & EXIT blocked: NVS verification failed\n");return;}for(int i=0;i<DRYER_SENSOR_COUNT;i++){if(std::fabs(verify.temperature[i]-s->_temp_cal[i])>0.05f||std::fabs(verify.humidity[i]-s->_hum_cal[i])>0.05f){printf("Calibration read-back mismatch at sensor %d\n",i);return;}}lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}
void DryerApp::cbCalExit(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));memcpy(s->_temp_cal,s->_temp_cal_backup,sizeof(s->_temp_cal));memcpy(s->_hum_cal,s->_hum_cal_backup,sizeof(s->_hum_cal));lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}
void DryerApp::cbCalResetAll(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));memset(s->_temp_cal,0,sizeof(s->_temp_cal));memset(s->_hum_cal,0,sizeof(s->_hum_cal));s->refreshCalibration();}

void DryerApp::cbOpenWeightCalibration(lv_event_t *e)
{
    DryerApp *s=static_cast<DryerApp*>(lv_event_get_user_data(e));
    if(s->_load_cell==nullptr)return;
    s->_weight_cal_backup=s->_load_cell->calibration();
    if(s->_weight_cal_backup.reference_weight_deci_g>0)
        s->_weight_reference_g=s->_weight_cal_backup.reference_weight_deci_g/10;
    s->refreshWeightCalibration();lv_scr_load(s->_scr_weight_cal);s->_cur_scr=SCR_WEIGHT_CALIBRATION;
}
void DryerApp::cbWeightTare(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(s->_load_cell){s->_load_cell->tare();s->refreshWeightCalibration();}}
void DryerApp::cbWeightRefDn(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(s->_weight_reference_g>100)s->_weight_reference_g-=100;s->refreshWeightCalibration();}
void DryerApp::cbWeightRefUp(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(s->_weight_reference_g<100000)s->_weight_reference_g+=100;s->refreshWeightCalibration();}
void DryerApp::cbWeightRefRepeat(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(lv_event_get_code(e)==LV_EVENT_RELEASED){s->_btn_repeat_cnt=0;return;}const int count=++s->_btn_repeat_cnt;const int step=count<15?100:count<40?1000:5000;const int direction=lv_event_get_target(e)==s->_btn_weight_ref_dn?-1:1;s->_weight_reference_g+=direction*step;if(s->_weight_reference_g<100)s->_weight_reference_g=100;if(s->_weight_reference_g>100000)s->_weight_reference_g=100000;s->refreshWeightCalibration();Buzzer::instance().keyTone(25);}
void DryerApp::cbWeightSet(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(s->_load_cell&&!s->_load_cell->calibrate((float)s->_weight_reference_g))printf("Load cell calibration failed: invalid raw span\n");s->refreshWeightCalibration();}
void DryerApp::cbWeightSaveExit(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(!s->_load_cell||!s_nvs.saveLoadCellCalibration(s->_load_cell->calibration())){printf("Load cell NVS save failed\n");return;}lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}
void DryerApp::cbWeightExit(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(s->_load_cell)s->_load_cell->setCalibration(s->_weight_cal_backup);lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}

void DryerApp::cbOpenCoolingSettings(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_dryer_settings_edit=s->_dryer_settings;s->_dryer_setting_sel=0;s->_dryer_ip_octet_sel=0;s->refreshCoolingSettings();lv_scr_load(s->_scr_cooling);s->_cur_scr=SCR_COOLING_SETTINGS;}
void DryerApp::cbCoolingRowSelect(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));lv_obj_t*row=lv_event_get_target(e);for(int i=0;i<DRYER_SETTING_COUNT;i++){if(s->_dryer_setting_rows[i]==row){if(i==19)return;s->_dryer_setting_sel=i;s->refreshCoolingSettings();Buzzer::instance().keyTone(25);break;}}}
void DryerApp::cbCoolingValueDn(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(lv_event_get_code(e)==LV_EVENT_RELEASED){s->_btn_repeat_cnt=0;return;}int step=1;if(lv_event_get_code(e)==LV_EVENT_LONG_PRESSED_REPEAT){const int count=++s->_btn_repeat_cnt;step=count<15?5:count<40?25:100;Buzzer::instance().keyTone(25);}else{s->_btn_repeat_cnt=0;}s->adjustDryerSetting(-step);}
void DryerApp::cbCoolingValueUp(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));if(lv_event_get_code(e)==LV_EVENT_RELEASED){s->_btn_repeat_cnt=0;return;}int step=1;if(lv_event_get_code(e)==LV_EVENT_LONG_PRESSED_REPEAT){const int count=++s->_btn_repeat_cnt;step=count<15?5:count<40?25:100;Buzzer::instance().keyTone(25);}else{s->_btn_repeat_cnt=0;}s->adjustDryerSetting(step);}
void DryerApp::cbCoolingIpNextOctet(lv_event_t*e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_dryer_setting_sel=19;s->_dryer_ip_octet_sel=(s->_dryer_ip_octet_sel+1)%5;s->refreshCoolingSettings();Buzzer::instance().keyTone(25);}
void DryerApp::cbCoolingReset(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));s->_dryer_settings_edit={DRYER_CFG_DEFAULT_DRY_TEMP_C,DRYER_CFG_DEFAULT_DRY_TIME_MIN,DRYER_CFG_DEFAULT_TEMP_HYSTERESIS_C,DRYER_CFG_DEFAULT_COOLING_TEMP_C,DRYER_CFG_DEFAULT_COOLING_TIME_MIN,DRYER_CFG_DEFAULT_DAMPER_MODE,DRYER_CFG_DEFAULT_DAMPER_OPEN_HUMIDITY_PCT,DRYER_CFG_DEFAULT_DAMPER_HYSTERESIS_PCT,DRYER_CFG_DEFAULT_HIGH_WARNING_TEMP_C,DRYER_CFG_DEFAULT_LOW_REACH_TIME_MIN,DRYER_CFG_DEFAULT_MIN_TEMP_RISE_C_PER_MIN,DRYER_CFG_DEFAULT_FAN_ADC_AT_10MS,DRYER_CFG_DEFAULT_MQTT_PUBLISH_INTERVAL_MIN,IMAGE_UPLOAD_DEFAULT_IP1,IMAGE_UPLOAD_DEFAULT_IP2,IMAGE_UPLOAD_DEFAULT_IP3,IMAGE_UPLOAD_DEFAULT_IP4,IMAGE_UPLOAD_DEFAULT_PORT,DRYER_CFG_DEFAULT_PREHEAT_TEMP_C,DRYER_CFG_DEFAULT_PREHEAT_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_ENABLED,DRYER_CFG_DEFAULT_STANDBY_TIME_MIN,DRYER_CFG_DEFAULT_STANDBY_TEMP_C,DRYER_CFG_DEFAULT_FAN_MIN_SPEED_MS};s->refreshCoolingSettings();}
void DryerApp::cbCoolingSaveExit(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));DryerNvsCoolingSettings verify{};if(!s_nvs.saveCoolingSettings(s->_dryer_settings_edit)||!s_nvs.loadCoolingSettings(&verify)||memcmp(&verify,&s->_dryer_settings_edit,sizeof(verify))!=0){printf("Dryer settings NVS verification failed\n");return;}s->_dryer_settings=verify;s->_pre_temp=verify.preheat_temp_c;s->_pre_time_min=verify.preheat_time_min;s->_mqtt_publish_elapsed_s=0;s->_damper_mode=static_cast<DamperMode>(verify.damper_mode);lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}
void DryerApp::cbCoolingExit(lv_event_t *e){DryerApp*s=static_cast<DryerApp*>(lv_event_get_user_data(e));lv_scr_load(s->_scr_main);s->_cur_scr=SCR_MAIN;}

void DryerApp::cbBtnDamper(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->doCycleDamper();
}

void DryerApp::cbLoadCellZero(lv_event_t *e)
{
    DryerApp *self = static_cast<DryerApp *>(lv_event_get_user_data(e));
    if (self->_rs485_sensors != nullptr && self->_rs485_sensors->requestLoadCellZero()) {
        Buzzer::instance().keyTone(25);
        self->showNoticePopup("0점 설정 완료");
    }
}

void DryerApp::cbLoadCellTareReset(lv_event_t *e)
{
    DryerApp *self = static_cast<DryerApp *>(lv_event_get_user_data(e));
    if (self->_rs485_sensors != nullptr &&
        self->_rs485_sensors->requestLoadCellClearTare()) {
        const int64_t gross64 = static_cast<int64_t>(self->_tare_weight_g) +
                                self->_weight_g;
        self->_weight_g = gross64 < std::numeric_limits<int32_t>::min()
            ? std::numeric_limits<int32_t>::min()
            : gross64 > std::numeric_limits<int32_t>::max()
                ? std::numeric_limits<int32_t>::max()
                : static_cast<int32_t>(gross64);
        self->_tare_weight_g = 0;
        self->saveRuntimeState();
        self->refreshCards();
        Buzzer::instance().keyTone(25);
    }
}

void DryerApp::cbLoadCellTare(lv_event_t *e)
{
    DryerApp *self = static_cast<DryerApp *>(lv_event_get_user_data(e));
    if (self->_rs485_sensors != nullptr && self->_rs485_sensors->requestLoadCellTare()) {
        const int64_t tare64 = static_cast<int64_t>(self->_tare_weight_g) + self->_weight_g;
        self->_tare_weight_g = tare64 < std::numeric_limits<int32_t>::min()
            ? std::numeric_limits<int32_t>::min()
            : tare64 > std::numeric_limits<int32_t>::max()
                ? std::numeric_limits<int32_t>::max()
                : static_cast<int32_t>(tare64);
        self->saveRuntimeState();
        self->refreshCards();
        Buzzer::instance().keyTone(25);
    }
}

void DryerApp::cbProcNext(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->_sel_proc = (self->_sel_proc + 1) % 4;
    self->refreshProcSelect();
}

void DryerApp::cbProcApply(lv_event_t *e)
{
    DryerApp *self = (DryerApp *)lv_event_get_user_data(e);
    self->doApplyProc();
}
