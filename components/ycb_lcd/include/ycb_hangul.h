/**
 * @file  ycb_hangul.h
 * @brief LVGL canvas 위에 조합형 한글(KSX1001) + ASCII 문자열을 그리는 API.
 *
 *  폰트 데이터:
 *    - 한글 : KSX1001 조합형 16×16 비트맵 (kssm_font.h)
 *              초성 8벌×20=160, 중성 4벌×22=88, 종성 4벌×28=112 → 합계 360 글리프
 *    - ASCII : 8×16 비트맵 (english.h), 128자
 *
 *  UTF-8 입력을 파싱하여 한글·ASCII·도(°) 기호를 자동 선택 렌더링합니다.
 *
 *  DryerLCD::draw16Korean / draw16English / draw16String (Adafruit GFX 버전) 을
 *  LVGL 8.x canvas 로 포팅.
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief LVGL canvas 에 UTF-8 문자열(한글+ASCII)을 그립니다.
     *
     * @param canvas       대상 lv_canvas 오브젝트
     * @param x            캔버스 내 시작 X
     * @param y            캔버스 내 시작 Y
     * @param fg           전경색 (글자 색)
     * @param bg           배경색 (transparent=true 이면 무시됨)
     * @param str          UTF-8 문자열 (한글·ASCII 혼용 가능)
     * @param scale        픽셀 확대 배율 (1=원본, 2=2배 크기)
     * @param transparent  true 이면 배경 픽셀을 그리지 않습니다
     * @return             렌더링된 총 가로 픽셀 수
     */
    int ycb_hangul_draw(lv_obj_t *canvas,
                        int x, int y,
                        lv_color_t fg, lv_color_t bg,
                        const char *str,
                        uint8_t scale,
                        bool transparent);

    /**
     * @brief 실제로 그리지 않고 문자열의 가로 폭(px)을 측정합니다.
     *
     * @param str    UTF-8 문자열
     * @param scale  픽셀 확대 배율
     * @return       총 가로 픽셀 수
     */
    int ycb_hangul_measure(const char *str, uint8_t scale);

    /**
     * @brief 단일 한글 유니코드(0xAC00~0xD7A3)를 canvas 에 그립니다.
     *
     * @param canvas      대상 lv_canvas
     * @param x, y        위치
     * @param fg, bg      색상
     * @param code        Unicode 코드포인트 (0xAC00 ~ 0xD7A3)
     * @param scale       배율
     * @param transparent 배경 투명 여부
     */
    void ycb_hangul_draw_char(lv_obj_t *canvas,
                              int x, int y,
                              lv_color_t fg, lv_color_t bg,
                              uint16_t code,
                              uint8_t scale,
                              bool transparent);

    /**
     * @brief 단일 ASCII 문자(0x00~0x7F)를 canvas 에 그립니다.
     *
     * @param canvas      대상 lv_canvas
     * @param x, y        위치
     * @param fg, bg      색상
     * @param ch          ASCII 코드 (0x00~0x7F)
     * @param scale       배율
     * @param transparent 배경 투명 여부
     */
    void ycb_ascii_draw_char(lv_obj_t *canvas,
                             int x, int y,
                             lv_color_t fg, lv_color_t bg,
                             uint8_t ch,
                             uint8_t scale,
                             bool transparent);

#ifdef __cplusplus
}
#endif
