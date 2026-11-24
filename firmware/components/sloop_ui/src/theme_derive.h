/* 运行时色表的纯派生函数（P4-E Task 5，spec §5 D3）。无 LVGL 依赖，host 同源测试。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sloop_store.h"

typedef struct {
    uint32_t bg, panel, panel_hi, panel_sel, text, text_dim, accent, error, track[4];
} ui_theme_t;

#define UI_RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* 上游 third_party/sloop-fm1/firmware/src/gfx.c:46-52 的五条 RGB565 色阶，
 * 顺序 GREEN/AMBER/CYAN/RED/MONO，每档由暗到亮。 */
static const uint16_t ui_palette_ramps[5][5] = {
    {UI_RGB565(0, 40, 12), UI_RGB565(0, 84, 30), UI_RGB565(16, 140, 54),
     UI_RGB565(56, 200, 92), UI_RGB565(120, 255, 146)},
    {UI_RGB565(60, 26, 0), UI_RGB565(110, 50, 0), UI_RGB565(170, 82, 0),
     UI_RGB565(225, 120, 8), UI_RGB565(255, 166, 40)},
    {UI_RGB565(0, 30, 50), UI_RGB565(0, 62, 96), UI_RGB565(16, 112, 160),
     UI_RGB565(56, 172, 222), UI_RGB565(140, 222, 255)},
    {UI_RGB565(52, 8, 8), UI_RGB565(100, 18, 14), UI_RGB565(170, 36, 26),
     UI_RGB565(226, 64, 48), UI_RGB565(255, 112, 92)},
    {UI_RGB565(40, 40, 40), UI_RGB565(80, 80, 80), UI_RGB565(130, 130, 130),
     UI_RGB565(186, 186, 186), UI_RGB565(226, 226, 226)},
};

static const uint8_t ui_palette_rgb[5][5][3] = {
    {{0, 40, 12}, {0, 84, 30}, {16, 140, 54}, {56, 200, 92}, {120, 255, 146}},
    {{60, 26, 0}, {110, 50, 0}, {170, 82, 0}, {225, 120, 8}, {255, 166, 40}},
    {{0, 30, 50}, {0, 62, 96}, {16, 112, 160}, {56, 172, 222}, {140, 222, 255}},
    {{52, 8, 8}, {100, 18, 14}, {170, 36, 26}, {226, 64, 48}, {255, 112, 92}},
    {{40, 40, 40}, {80, 80, 80}, {130, 130, 130}, {186, 186, 186}, {226, 226, 226}},
};

static inline uint32_t ui_rgb888(const uint8_t c[3])
{
    return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
}

static inline void ui_rgb565_to_rgb888(uint16_t v, uint8_t out[3])
{
    out[0] = (uint8_t)(((v >> 11) & 31u) << 3);
    out[1] = (uint8_t)(((v >> 5) & 63u) * 255u / 63u);
    out[2] = (uint8_t)((v & 31u) << 3);
}

static inline uint8_t ui_tint_channel(uint8_t base, int colour, int mono)
{
    int v = (int)base + (colour - mono) / 2;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static inline uint32_t ui_tint_grey(uint32_t base, const uint8_t c[3], const uint8_t mono[3])
{
    uint32_t out = 0;
    for (uint8_t i = 0; i < 3; i++) {
        out |= (uint32_t)ui_tint_channel((base >> (16 - 8 * i)) & 0xFFu, c[i], mono[i])
               << (16 - 8 * i);
    }
    return out;
}

static inline void ui_theme_fixed(ui_theme_t *out)
{
    static const uint32_t tracks[4] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    out->bg = 0x101418;
    out->text = 0xF2F5F8;
    out->error = 0xE0542A;
    for (uint8_t i = 0; i < 4; i++) out->track[i] = tracks[i];
}

static inline void ui_theme_derive_mono(ui_theme_t *out)
{
    ui_theme_fixed(out);
    out->panel = 0x1B2026;
    out->panel_hi = 0x242B33;
    out->panel_sel = 0x2C3540;
    out->text_dim = 0x8A949E;
    out->accent = 0xE0B32A;
}

static inline void ui_theme_derive_palette(const uint16_t pal5[5], ui_theme_t *out)
{
    static const uint8_t mono[5][3] = {
        {40, 40, 40}, {80, 80, 80}, {130, 130, 130}, {186, 186, 186}, {226, 226, 226},
    };
    uint8_t c[5][3], sel[3];
    int known = -1;
    for (uint8_t p = 0; p < 5; p++) {
        bool same = true;
        for (uint8_t i = 0; i < 5; i++) same = same && pal5[i] == ui_palette_ramps[p][i];
        if (same) known = p;
    }
    for (uint8_t i = 0; i < 5; i++) {
        if (known >= 0) {
            for (uint8_t j = 0; j < 3; j++) c[i][j] = ui_palette_rgb[known][i][j];
        } else {
            ui_rgb565_to_rgb888(pal5[i], c[i]);
        }
    }
    for (uint8_t i = 0; i < 3; i++) sel[i] = (uint8_t)(((int)c[1][i] + c[2][i]) / 2);
    static const uint8_t mono_sel[3] = {105, 105, 105};
    ui_theme_fixed(out);
    out->panel = ui_tint_grey(0x1B2026, c[0], mono[0]);
    out->panel_hi = ui_tint_grey(0x242B33, c[1], mono[1]);
    out->panel_sel = ui_tint_grey(0x2C3540, sel, mono_sel);
    out->text_dim = ui_tint_grey(0x8A949E, c[2], mono[2]);
    out->accent = ui_rgb888(c[4]);
}

static inline void ui_theme_derive_theme(const song_theme_view_t *th, ui_theme_t *out)
{
    uint8_t sel[3];
    for (uint8_t i = 0; i < 3; i++) sel[i] = (uint8_t)(((int)th->c[1][i] + th->c[2][i]) / 2);
    static const uint8_t mono[5][3] = {
        {40, 40, 40}, {80, 80, 80}, {130, 130, 130}, {186, 186, 186}, {226, 226, 226},
    };
    static const uint8_t mono_sel[3] = {105, 105, 105};
    ui_theme_fixed(out);
    out->bg = ui_rgb888(th->bg);
    out->panel = ui_tint_grey(0x1B2026, th->c[0], mono[0]);
    out->panel_hi = ui_tint_grey(0x242B33, th->c[1], mono[1]);
    out->panel_sel = ui_tint_grey(0x2C3540, sel, mono_sel);
    out->text = ui_rgb888(th->hi);
    out->text_dim = ui_tint_grey(0x8A949E, th->c[2], mono[2]);
    out->accent = ui_rgb888(th->hi);
    for (uint8_t i = 0; i < 4; i++) out->track[i] = ui_rgb888(th->trk[i]);
}
