/*
 * InputManager 的鼓演奏键位表（无 LVGL/FreeRTOS）：选中鼓轨时，低行白键
 * z x c v b n m . → lane 0..7，高行白键 q w e r t y u i → lane 8..15
 * （spec §4.4 / ADR-006）。其它键（半音黑键、走带、导航、页面动作）在鼓轨上
 * 不发声、**不回落合成轨 0**。input.c 与 host 单测共用本表，映射只此一份。
 */
#ifndef SLOOP_UI_INPUT_MAP_H
#define SLOOP_UI_INPUT_MAP_H

#include <stdint.h>

#define INPUT_MAP_NO_LANE (-1)

static inline int8_t input_map_drum_lane(uint8_t code)
{
    static const uint8_t k_low[8] = { 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49 };
    static const uint8_t k_high[8] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28 };
    for (int8_t i = 0; i < 8; i++) {
        if (code == k_low[i]) return i;
        if (code == k_high[i]) return (int8_t)(8 + i);
    }
    return INPUT_MAP_NO_LANE;
}

#endif
