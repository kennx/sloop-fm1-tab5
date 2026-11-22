/*
 * InputManager 的鼓演奏键位表（无 LVGL/FreeRTOS）：选中鼓轨时，低行白键
 * z x c v b n m . → lane 0..7，高行白键 q w e r t y u i → lane 8..15
 * （spec §4.4 / ADR-006）。其它键（半音黑键、走带、导航、页面动作）在鼓轨上
 * 不发声、**不回落合成轨 0**。input.c 与 host 单测共用本表，映射只此一份。
 */
#ifndef SLOOP_UI_INPUT_MAP_H
#define SLOOP_UI_INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define INPUT_MAP_NO_LANE (-1)
#define INPUT_MAP_DRUM_TRACK 3u /* APP_STATE_TRACKS - 1：最后一条轨是鼓轨 */

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

/* 演奏键的路由（input.c 的 act_note 与 host 单测共用）：
 *  - 按下：选中鼓轨时鼓键 → DRUM、其它键 → IGNORE（不回落合成轨 0）；合成轨 → NOTE。
 *  - **释放永远回 NOTE**：note-off 用按下时记录的轨号，即使期间切到鼓轨也不能被
 *    鼓分支吞掉（否则音符挂住）。 */
enum { INPUT_ROUTE_NOTE = 0, INPUT_ROUTE_DRUM, INPUT_ROUTE_IGNORE };

static inline int input_map_key_route(uint8_t selected_track, uint8_t code, bool down)
{
    if (!down) return INPUT_ROUTE_NOTE;
    if (selected_track >= INPUT_MAP_DRUM_TRACK)
        return input_map_drum_lane(code) >= 0 ? INPUT_ROUTE_DRUM : INPUT_ROUTE_IGNORE;
    return INPUT_ROUTE_NOTE;
}

#endif
