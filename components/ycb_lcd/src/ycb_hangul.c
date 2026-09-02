/**
 * @file  ycb_hangul.c
 * @brief DryerLCD::draw16Korean / draw16English / draw16String 의 LVGL canvas 포팅.
 *
 *  폰트 데이터 (컴포넌트 내부):
 *    font/kssm_font.h  → K_font[360][32]   16×16 한글 조합형 비트맵
 *    font/english.h    → english[128][16]   8×16 ASCII 비트맵
 *
 *  한글 조합 알고리즘:
 *    Unicode U+AC00~U+D7A3 → 초성(cho) / 중성(jung) / 종성(jong) 분리
 *    중성 벌수(4벌)·초성 벌수(8벌)·종성 벌수(4벌) 선택
 *    세 글리프를 비트OR 합성 후 픽셀 단위 렌더
 */

#include "ycb_hangul.h"
#include "font/malgun_gothic_32.h"
#include "font/english.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────
 * 내부 픽셀 쓰기 헬퍼
 * ───────────────────────────────────────────────────────────────── */

/**
 * @brief scale 배율로 (x, y) 기준 블록 픽셀을 canvas 에 씁니다.
 *  on==true → 전경색, on==false && !transparent → 배경색
 */
static inline void _px(lv_obj_t *cv,
                       int x, int y,
                       lv_color_t fg, lv_color_t bg,
                       bool on, uint8_t scale, bool transparent)
{
    if (!on && transparent)
        return;
    lv_color_t c = on ? fg : bg;
    for (int dy = 0; dy < (int)scale; dy++)
        for (int dx = 0; dx < (int)scale; dx++)
            lv_canvas_set_px_color(cv, x + dx, y + dy, c);
}

/* ─────────────────────────────────────────────────────────────────
 * ASCII 문자 렌더링
 *  비트맵 레이아웃: english[ch][16]
 *    [0..7]  → 전반부 8바이트 (각 바이트의 bit0=row0, bit1=row1...)
 *    [8..15] → 후반부 8바이트
 *  화면 매핑: col = byte_index(0~7), row = bit_index(0~7) + 8*(block)
 *  즉 x = _xchar + col * scale,  y = _ychar + row_of_byte * scale
 * ───────────────────────────────────────────────────────────────── */
void ycb_ascii_draw_char(lv_obj_t *canvas,
                         int x, int y,
                         lv_color_t fg, lv_color_t bg,
                         uint8_t ch,
                         uint8_t scale,
                         bool transparent)
{
    if (!canvas)
        return;
    if (ch > 0x7F)
        ch = 0x20;
    const uint8_t *font = english[ch];

    /* 배경 블록 먼저 채우기 (투명 모드가 아닐 때) */
    if (!transparent)
    {
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                _px(canvas, x + col * scale, y + row * scale,
                    fg, bg, false, scale, false);
    }

    /* 두 블록(각 8바이트) 처리 */
    for (int block = 0; block < 2; block++)
    {
        for (int k = 0; k < 8; k++)
        {
            uint8_t b = font[block * 8 + k];
            for (int bit = 0; bit < 8; bit++)
            {
                if (b & (1u << bit))
                {
                    int px_x = x + k * scale;
                    int px_y = y + (block * 8 + bit) * scale;
                    _px(canvas, px_x, px_y, fg, bg, true, scale, transparent);
                }
            }
        }
    }
}

/* ─────────────────────────────────────────────────────────────────
 * 한글 문자 렌더링
 *  Unicode U+AC00~U+D7A3 분해:
 *    jong  = (code - 0xAC00) % 28
 *    first = (code - 0xAC00) / 28 / 21 + 1   (1-based: 1=ㄱ .. 19=ㅎ, 없음=1→'ㅇ' 처리)
 *    mid   = (code - 0xAC00) / 28 % 21 + 1
 *
 *  원본 DryerLCD::draw16Korean 의 벌수 테이블 그대로 사용
 * ───────────────────────────────────────────────────────────────── */
void ycb_hangul_draw_char(lv_obj_t *canvas,
                          int x, int y,
                          lv_color_t fg, lv_color_t bg,
                          uint16_t code,
                          uint8_t scale,
                          bool transparent)
{
    if (!canvas)
        return;
    if (code < 0xAC00 || code > 0xD7A3)
        return;

    /* Malgun Gothic source is 32x32. Keep the legacy API geometry:
     * scale 1 samples it to 16x16, scale 2 renders native 32x32. */
    const uint8_t *malgun = malgun32_font[code - MALGUN32_FIRST];
    const int target = 16 * scale;
    if (!transparent)
        for (int row = 0; row < target; row++)
            for (int col = 0; col < target; col++)
                lv_canvas_set_px_color(canvas, x + col, y + row, bg);
    for (int row = 0; row < target; row++) {
        const int src_y = row * 32 / target;
        for (int col = 0; col < target; col++) {
            const int src_x = col * 32 / target;
            if (malgun[src_y * 4 + src_x / 8] & (0x80u >> (src_x & 7)))
                lv_canvas_set_px_color(canvas, x + col, y + row, fg);
        }
    }
    return;

#if 0 /* Legacy 16x16 KSSM compositor retained here for reference only. */

    /* ── 벌수 선택 룩업테이블 (원본 DryerLCD 와 동일) ── */
    static const uint8_t cho1[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 3, 3, 1, 2, 4, 4, 4, 2, 1, 3, 0};
    static const uint8_t cho2[22] = {0, 5, 5, 5, 5, 5, 5, 5, 5, 6, 7, 7, 7, 6, 6, 7, 7, 7, 6, 6, 7, 5};
    static const uint8_t jong_t[22] = {0, 0, 2, 0, 2, 1, 2, 1, 2, 3, 0, 2, 1, 3, 3, 1, 2, 1, 3, 3, 1, 1};

    uint16_t val = code - 0xAC00u;
    uint8_t last = (uint8_t)(val % 28u);
    uint8_t first = (uint8_t)(val / 28u / 21u + 1u);
    uint8_t mid = (uint8_t)(val / 28u % 21u + 1u);

    uint8_t firstType, midType, lastType = 0;

    if (last == 0)
    {
        firstType = cho1[mid];
        midType = (first == 1 || first == 24) ? 0u : 1u;
    }
    else
    {
        firstType = cho2[mid];
        midType = (first == 1 || first == 24) ? 2u : 3u;
        lastType = jong_t[mid];
    }

    /* ── 세 글리프를 비트OR 합성 ── */
    uint8_t buf[32] = {0};
    uint16_t pF;

    /* 초성 */
    pF = (uint16_t)firstType * 20u + first;
    for (int i = 0; i < 32; i++)
        buf[i] = K_font[pF][i];

    /* 중성 */
    pF = 160u + (uint16_t)midType * 22u + mid;
    for (int i = 0; i < 32; i++)
        buf[i] |= K_font[pF][i];

    /* 종성 (있을 때만) */
    if (last)
    {
        pF = 248u + (uint16_t)lastType * 28u + last;
        for (int i = 0; i < 32; i++)
            buf[i] |= K_font[pF][i];
    }

    /* ── 합성된 16×16 비트맵 렌더링 ──
     *  buf[0..15]  = 상위 바이트 (col 0~7 의 각 row bit)
     *  buf[16..31] = 하위 바이트 (col 8~15 의 각 row bit)
     *  비트 레이아웃: bits = (buf[row] << 8) | buf[row+16]
     *    bit15 = 왼쪽 첫 픽셀, bit0 = 오른쪽 마지막 픽셀
     */
    if (!transparent)
    {
        /* 배경 먼저 */
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 16; col++)
                _px(canvas, x + col * scale, y + row * scale,
                    fg, bg, false, scale, false);
    }

    for (int row = 0; row < 16; row++)
    {
        uint16_t bits = ((uint16_t)buf[row] << 8u) | buf[row + 16];
        for (int col = 0; col < 16; col++)
        {
            if (bits & 0x8000u)
            {
                _px(canvas, x + col * scale, y + row * scale,
                    fg, bg, true, scale, transparent);
            }
            bits <<= 1u;
        }
    }
#endif
}

/* ─────────────────────────────────────────────────────────────────
 * UTF-8 문자열 파서 → ycb_ascii_draw_char / ycb_hangul_draw_char 호출
 * ───────────────────────────────────────────────────────────────── */
int ycb_hangul_draw(lv_obj_t *canvas,
                    int x, int y,
                    lv_color_t fg, lv_color_t bg,
                    const char *str,
                    uint8_t scale,
                    bool transparent)
{
    if (!canvas || !str)
        return 0;
    if (scale < 1)
        scale = 1;

    int total_w = 0;
    int cx = x;

    while (*str)
    {
        uint8_t c1 = (uint8_t)*str++;

        if (c1 < 0x80u)
        {
            /* ── 1바이트 ASCII ── */
            ycb_ascii_draw_char(canvas, cx, y, fg, bg, c1, scale, transparent);
            cx += 8 * scale;
            total_w += 8 * scale;
        }
        else if ((c1 & 0xE0u) == 0xC0u)
        {
            /* ── 2바이트 UTF-8 (Latin, °기호 등) ── */
            uint8_t c2 = (uint8_t)*str++;
            uint16_t code = (uint16_t)((c1 & 0x1Fu) << 6u) | (c2 & 0x3Fu);

            if (code == 0x00B0u)
            {
                /* U+00B0 ° (도 기호): 오른쪽 위에 작은 원 표시 */
                int oy = y + 1 * scale;
                int ox = cx + 5 * scale;
                /* 4픽셀 원형 점 */
                lv_canvas_set_px_color(canvas, ox + 1, oy, fg);
                lv_canvas_set_px_color(canvas, ox + 2, oy, fg);
                lv_canvas_set_px_color(canvas, ox, oy + 1, fg);
                lv_canvas_set_px_color(canvas, ox + 3, oy + 1, fg);
                lv_canvas_set_px_color(canvas, ox, oy + 2, fg);
                lv_canvas_set_px_color(canvas, ox + 3, oy + 2, fg);
                lv_canvas_set_px_color(canvas, ox + 1, oy + 3, fg);
                lv_canvas_set_px_color(canvas, ox + 2, oy + 3, fg);
            }
            /* 그 외 2바이트 문자는 공백 폭만 차지 */
            cx += 8 * scale;
            total_w += 8 * scale;
        }
        else
        {
            /* ── 3바이트 UTF-8 (한글 포함 대부분의 CJK) ── */
            uint8_t c2 = (uint8_t)*str++;
            uint8_t c3 = (uint8_t)*str++;
            uint16_t code = (uint16_t)(((c1 & 0x0Fu) << 12u) |
                                       ((c2 & 0x3Fu) << 6u) |
                                       (c3 & 0x3Fu));

            if (code >= 0xAC00u && code <= 0xD7A3u)
            {
                /* 한글 */
                ycb_hangul_draw_char(canvas, cx, y, fg, bg, code, scale, transparent);
                cx += 16 * scale;
                total_w += 16 * scale;
            }
            else
            {
                /* 기타 3바이트 문자: 공백 */
                cx += 8 * scale;
                total_w += 8 * scale;
            }
        }
    }
    return total_w;
}

/* ─────────────────────────────────────────────────────────────────
 * 문자열 폭 측정 (그리지 않음)
 * ───────────────────────────────────────────────────────────────── */
int ycb_hangul_measure(const char *str, uint8_t scale)
{
    if (!str)
        return 0;
    if (scale < 1)
        scale = 1;

    int total = 0;
    while (*str)
    {
        uint8_t c = (uint8_t)*str++;
        if (c < 0x80u)
        {
            total += 8 * scale;
        }
        else if ((c & 0xE0u) == 0xC0u)
        {
            str++; /* 2바이트 UTF-8: 나머지 1바이트 건너뜀 */
            total += 8 * scale;
        }
        else
        {
            str++;
            str++; /* 3바이트 UTF-8: 나머지 2바이트 건너뜀 */
            total += 16 * scale;
        }
    }
    return total;
}
