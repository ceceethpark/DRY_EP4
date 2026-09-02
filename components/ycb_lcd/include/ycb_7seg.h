/**
 * @file  ycb_7seg.h
 * @brief LVGL canvas 위에 7-세그먼트 스타일 숫자를 그리는 API.
 *
 *  DryerLCD::draw7Number (Adafruit GFX 버전) 을 LVGL 8.x canvas 로 포팅.
 *  각 세그먼트는 다이아몬드(마름모) 형태의 수평/수직 획으로 구성됩니다.
 *
 *  cS=3 기준:
 *    - 한 자리 너비 : 38 px
 *    - 한 자리 높이 : 50 px
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief LVGL canvas 에 7-세그먼트 숫자를 그립니다.
     *
     * @param canvas  대상 lv_canvas 오브젝트
     * @param n       표시할 정수값 (부호 있음)
     * @param xLoc    캔버스 내 시작 X 좌표
     * @param yLoc    캔버스 내 시작 Y 좌표
     * @param cS      세그먼트 스케일 (보통 3 → 1자리 = 38×50 px)
     * @param fC      전경색 (켜진 세그먼트 색)
     * @param bC      배경색 (꺼진 세그먼트 색)
     * @param nD      자릿수; 음수이면 최상위 0 을 공백으로 표시
     * @param dash    0 이 아니면 숫자 대신 '---' 를 표시
     * @return        렌더링에 사용된 총 가로 픽셀 수
     */
    uint16_t ycb_7seg_draw(lv_obj_t *canvas, long n,
                           uint16_t xLoc, uint16_t yLoc,
                           int8_t cS,
                           lv_color_t fC, lv_color_t bC,
                           int8_t nD, uint8_t dash);

    /**
     * @brief 실제로 그리지 않고 ycb_7seg_draw 가 차지하는 가로 폭을 반환합니다.
     *
     * @param cS  세그먼트 스케일
     * @param nD  자릿수
     * @return    총 가로 픽셀 수
     */
    uint16_t ycb_7seg_width(int8_t cS, int8_t nD);

    /**
     * @brief cS 기준 한 자리의 높이(px)를 반환합니다.
     */
    uint16_t ycb_7seg_height(int8_t cS);

#ifdef __cplusplus
}
#endif
