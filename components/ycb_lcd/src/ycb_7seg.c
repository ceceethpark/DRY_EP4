/**
 * @file  ycb_7seg.c
 * @brief DryerLCD::draw7Number 의 LVGL canvas 포팅 구현.
 *
 * 원본 로직을 그대로 유지하면서
 *   tft->drawFastHLine(x, y, w, col)  →  내부 _hline()
 *   tft->drawFastVLine(x, y, h, col)  →  내부 _vline()
 * 으로 대체했습니다.
 */

#include "ycb_7seg.h"
#include <stdlib.h> /* abs() */

/* ─────────────────────────────────────────────────────────────────
 * 내부 드로잉 헬퍼
 * ───────────────────────────────────────────────────────────────── */

/** 수평선 (x, y) ~ (x+w-1, y) */
static void _hline(lv_obj_t *cv, int16_t x, int16_t y, int16_t w, lv_color_t c)
{
    for (int16_t i = 0; i < w; i++)
        lv_canvas_set_px_color(cv, x + i, y, c);
}

/** 수직선 (x, y) ~ (x, y+h-1) */
static void _vline(lv_obj_t *cv, int16_t x, int16_t y, int16_t h, lv_color_t c)
{
    for (int16_t i = 0; i < h; i++)
        lv_canvas_set_px_color(cv, x, y + i, c);
}

/* ─────────────────────────────────────────────────────────────────
 * 공개 API
 * ───────────────────────────────────────────────────────────────── */

uint16_t ycb_7seg_width(int8_t cS, int8_t nD)
{
    uint16_t S2 = 5u * (uint16_t)abs(cS);
    uint16_t S3 = 2u * (uint16_t)abs(cS);
    uint16_t d = S2 + (3u * S3) + 2u + (uint16_t)abs(cS);
    return d * (uint16_t)abs(nD);
}

uint16_t ycb_7seg_height(int8_t cS)
{
    uint16_t S3 = 2u * (uint16_t)abs(cS);
    uint16_t S4 = 5u * (uint16_t)abs(cS);
    /* 총 높이 = 2*(S3 + S4) + S3  ≈ 2*y3 + S3  (원본 로직에서 최하단 y 계산) */
    return 2u * (S3 + S4) + S3 + 2u;
}

uint16_t ycb_7seg_draw(lv_obj_t *canvas, long n,
                       uint16_t xLoc, uint16_t yLoc,
                       int8_t cS,
                       lv_color_t fC, lv_color_t bC,
                       int8_t nD, uint8_t dash)
{
    if (!canvas)
        return 0;

    /* ── 세그먼트 치수 계산 ── */
    uint16_t num = (uint16_t)(n < 0 ? -n : n);
    uint16_t S2 = 5u * (uint16_t)abs(cS); /* 수평 세그먼트 내부 폭  */
    uint16_t S3 = 2u * (uint16_t)abs(cS); /* 세그먼트 획 두께       */
    uint16_t S4 = 5u * (uint16_t)abs(cS); /* 수직 세그먼트 내부 높이 */
    uint16_t x1 = (uint16_t)abs(cS) + 1u;
    uint16_t x2 = S3 + S2 + 1u;
    uint16_t y1 = yLoc + x1;
    uint16_t y3 = yLoc + S3 + S4 + 1u;

    /*
     * 7세그먼트 앵커 좌표 [seg][0]=x_offset, [seg][1]=y_base
     *   seg 0 : a  상단 수평
     *   seg 1 : b  우상 수직
     *   seg 2 : c  우하 수직
     *   seg 3 : d  하단 수평
     *   seg 4 : e  좌하 수직
     *   seg 5 : f  좌상 수직
     *   seg 6 : g  중간 수평
     */
    uint16_t seg[7][2] = {
        {x1, yLoc},           /* a */
        {x2, y1},             /* b */
        {x2, y3 + x1},        /* c */
        {x1, 2u * y3 - yLoc}, /* d */
        {0u, y3 + x1},        /* e */
        {0u, y1},             /* f */
        {x1, y3}              /* g */
    };

    /* 0~9 세그먼트 맵, 10=공백, 11=마이너스(-) */
    static const uint8_t nums[12] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66,
        0x6D, 0x7D, 0x07, 0x7F, 0x67,
        0x00, 0x40};

    int8_t cnt = (int8_t)abs(nD);
    if (cnt > 10)
        cnt = 10;
    if (cnt < 1)
        cnt = 1;

    uint16_t d = S2 + (3u * S3) + 2u + (uint16_t)abs(cS); /* 한 자리 폭 */
    xLoc += (uint16_t)(cnt - 1) * d;                      /* 최우측 자리부터 */

    while (cnt > 0)
    {
        --cnt;
        uint8_t i;

        if (num > 9u)
            i = (uint8_t)(num % 10u);
        else if (!cnt && n < 0)
            i = 11; /* 마이너스 부호 */
        else if (nD < 0 && !num && cnt != 0)
            i = 10; /* 상위 자리 공백 */
        else
            i = (uint8_t)num;

        num /= 10u;
        if (dash)
            i = 11;

        /* ── 7개 세그먼트 그리기 ── */
        for (int j = 0; j < 7; ++j)
        {
            lv_color_t col = (nums[i] & (1u << j)) ? fC : bC;

            if (j == 0 || j == 3 || j == 6)
            {
                /* ── 수평 세그먼트 (다이아몬드 모양) ── */
                uint16_t w = S2;
                uint16_t sx = xLoc + seg[j][0] + (uint16_t)abs(cS);
                uint16_t sy = seg[j][1];
                uint16_t te = sy + S3;      /* 끝 y  */
                uint16_t mi = sy + S3 / 2u; /* 중간 y */
                /* 위→중간: 폭 늘리며 확장 */
                while (sy < mi)
                {
                    _hline(canvas, (int16_t)sx, (int16_t)sy, (int16_t)w, col);
                    ++sy;
                    --sx;
                    w += 2u;
                }
                /* 중간→아래: 폭 줄이며 수렴 */
                while (sy < te)
                {
                    _hline(canvas, (int16_t)sx, (int16_t)sy, (int16_t)w, col);
                    ++sy;
                    ++sx;
                    w -= 2u;
                }
            }
            else
            {
                /* ── 수직 세그먼트 (다이아몬드 모양) ── */
                uint16_t h = S4;
                uint16_t sx = xLoc + seg[j][0];
                uint16_t sy = seg[j][1] + (uint16_t)abs(cS);
                uint16_t te = sx + S3;      /* 끝 x  */
                uint16_t mi = sx + S3 / 2u; /* 중간 x */
                /* 좌→중간: 높이 늘리며 확장 */
                while (sx < mi)
                {
                    _vline(canvas, (int16_t)sx, (int16_t)sy, (int16_t)h, col);
                    ++sx;
                    --sy;
                    h += 2u;
                }
                /* 중간→우: 높이 줄이며 수렴 */
                while (sx < te)
                {
                    _vline(canvas, (int16_t)sx, (int16_t)sy, (int16_t)h, col);
                    ++sx;
                    ++sy;
                    h -= 2u;
                }
            }
        } /* for j */

        xLoc -= d;
    } /* while cnt */

    return d * (uint16_t)abs(nD);
}
