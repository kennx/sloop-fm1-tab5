/*
 * SYSTEM 设置页（P4-E Task 6，独立 SYS 组）。
 *
 * 页面区相对坐标：节标题 SCREEN(16) / LIGHTS(150) / AUDIO(230) / SYSTEM(430)；
 * 三区行 (24,y) 420×44，左右 42px 是 -/+；ZOOM/LOWCUT 是 140px chip。
 * SYSTEM 节显示 APP/SLOOP 两行版本与 SELFTEST chip；状态行 (24,600) 20pt。
 *
 * 所有编辑都经 system_action() 发状态层命令，控件回调、TAP 与键盘共用；值一律
 * 从命令后的 AppState 投影回读。COLOR 在 side theme 有效时增加第 6 档：进入 theme
 * 先发 THEME_ON=1，回内置 palette 则先发 THEME_ON=0 再发 PALETTE=n。
 *
 * build/update/tap 在 LVGL 锁内；page_system_key 在输入任务上下文，不碰 LVGL。
 * 样式只初始化一次，组作用域重建时只更新颜色属性（ADR-008 R34）。
 */
#include "page_system.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "sloop_store.h"
#include "theme.h"

#define SYS_ROW_X       24
#define SYS_ROW_W       420
#define SYS_ROW_H       44
#define SYS_ZONE_W      42
#define SYS_CHIP_X      304
#define SYS_CHIP_W      140
#define SYS_CHIP_H      30
#define SYS_CAPTION_W   260
#define SYS_STATUS_X    24
#define SYS_STATUS_Y    600
#define SYS_STATUS_W    1232
#define SYS_STATUS_H    40
#define SYS_VERSION_Y   454
#define SYS_SELFTEST_Y  510
#define SYS_SELFTEST_MAGIC 0x534C5459u

#define SYS_COLOR_Y     32
#define SYS_ZOOM_Y      76
#define SYS_BRIGHT_Y    106
#define SYS_LIGHTS_Y    174
#define SYS_VOLUME_Y    254
#define SYS_OUTPUT_Y    298
#define SYS_LOWCUT_Y    342
#define SYS_RESO_Y      372

#define K_ENTER 0x3Du
#define K_UP    0x3Bu
#define K_LEFT  0x4Au
#define K_DOWN  0x4Bu
#define K_RIGHT 0x4Cu

typedef enum {
    SYS_COLOR = 0,
    SYS_ZOOM,
    SYS_BRIGHT,
    SYS_LIGHTS,
    SYS_VOLUME,
    SYS_OUTPUT,
    SYS_LOWCUT,
    SYS_RESO,
    SYS_SELFTEST,
    SYS_ROW_COUNT,
} sys_row_t;

static lv_obj_t *s_rows[SYS_SELFTEST];
static lv_obj_t *s_selftest;
static lv_obj_t *s_status;

static lv_style_t s_st_section;
static lv_style_t s_st_row;
static lv_style_t s_st_chip;
static lv_style_t s_st_focus;
static lv_style_t s_st_version;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_focus;
static uint8_t s_act = UI_SYS_ACT_NONE;
static bool s_reset_checked;
static uint8_t s_selftest_ran __attribute__((section(".noinit")));
static uint32_t s_selftest_magic __attribute__((section(".noinit")));
static uint8_t s_selftest_last_done __attribute__((section(".noinit")));
static uint32_t s_selftest_last_ran __attribute__((section(".noinit")));
static uint32_t s_selftest_last_failed __attribute__((section(".noinit")));
static uint32_t s_selftest_last_magic __attribute__((section(".noinit")));
static char s_status_text[40];
static char s_status_echo[40];
static char s_row_cache[SYS_SELFTEST][64];
static char s_status_cache[40];
static uint32_t s_status_color;
static uint8_t s_focus_shown = 0xFF;
static uint32_t s_status_color_shown = 0xFFFFFFFFu;

static const char *const k_palette_names[SLOOP_SETTINGS_PALETTES] = {
    "GREEN", "AMBER", "CYAN", "RED", "MONO",
};
static const char *const k_light_names[4] = {"OFF", "LOW", "MID", "HIGH"};

static void styles_init(void)
{
    static bool done;
    if (done) {
        lv_style_set_text_color(&s_st_section, lv_color_hex(UI_TEXT_DIM));
        lv_style_set_bg_color(&s_st_row, lv_color_hex(UI_PANEL));
        lv_style_set_text_color(&s_st_row, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
        lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
        lv_style_set_outline_color(&s_st_focus, lv_color_hex(UI_ACCENT));
        lv_style_set_text_color(&s_st_version, lv_color_hex(UI_TEXT_DIM));
        return;
    }
    done = true;

    lv_style_init(&s_st_section);
    lv_style_set_text_font(&s_st_section, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_section, lv_color_hex(UI_TEXT_DIM));

    lv_style_init(&s_st_row);
    lv_style_set_radius(&s_st_row, 8);
    lv_style_set_bg_opa(&s_st_row, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_row, lv_color_hex(UI_PANEL));
    lv_style_set_text_font(&s_st_row, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_row, lv_color_hex(UI_TEXT));
    lv_style_set_pad_left(&s_st_row, 12);
    lv_style_set_pad_top(&s_st_row, 10);

    lv_style_init(&s_st_chip);
    lv_style_set_radius(&s_st_chip, 8);
    lv_style_set_bg_opa(&s_st_chip, LV_OPA_COVER);
    lv_style_set_bg_color(&s_st_chip, lv_color_hex(UI_PANEL_HI));
    lv_style_set_text_font(&s_st_chip, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_chip, lv_color_hex(UI_TEXT));
    lv_style_set_text_align(&s_st_chip, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_st_chip, 4);

    lv_style_init(&s_st_focus);
    lv_style_set_outline_width(&s_st_focus, 3);
    lv_style_set_outline_opa(&s_st_focus, LV_OPA_COVER);
    lv_style_set_outline_pad(&s_st_focus, 0);
    lv_style_set_outline_color(&s_st_focus, lv_color_hex(UI_ACCENT));

    lv_style_init(&s_st_version);
    lv_style_set_text_font(&s_st_version, &lv_font_montserrat_16);
    lv_style_set_text_color(&s_st_version, lv_color_hex(UI_TEXT_DIM));
}

static int row_y(uint8_t row)
{
    static const int16_t y[SYS_ROW_COUNT] = {
        SYS_COLOR_Y, SYS_ZOOM_Y, SYS_BRIGHT_Y, SYS_LIGHTS_Y, SYS_VOLUME_Y,
        SYS_OUTPUT_Y, SYS_LOWCUT_Y, SYS_RESO_Y, SYS_SELFTEST_Y,
    };
    return row < SYS_ROW_COUNT ? y[row] : 0;
}

static int row_h(uint8_t row)
{
    return row == SYS_ZOOM || row == SYS_LOWCUT ? SYS_CHIP_H : SYS_ROW_H;
}

static bool chip_row(uint8_t row)
{
    return row == SYS_ZOOM || row == SYS_LOWCUT;
}

static void status_record(const app_state_t *st)
{
    char text[40];
    uint32_t color;
    if (st->selftest_done) {
        s_selftest_last_done = 1;
        s_selftest_last_ran = st->selftest_ran;
        s_selftest_last_failed = st->selftest_failed;
        s_selftest_last_magic = SYS_SELFTEST_MAGIC;
    }
    if (s_act == UI_SYS_ACT_SELFTEST && !st->selftest_done &&
        s_selftest_last_magic == SYS_SELFTEST_MAGIC && s_selftest_last_done) {
        ui_sys_selftest_text(text, sizeof(text), s_selftest_last_done,
                             s_selftest_last_ran, s_selftest_last_failed);
        color = s_selftest_last_ran == 0 || s_selftest_last_failed != 0 ? UI_ERROR : UI_ACCENT;
    } else {
        ui_sys_status_text(text, sizeof(text), s_act, st, sloop_store_ready());
        color = ui_sys_status_color(s_act, st);
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_status_text, text, sizeof s_status_text);
    s_status_color = color;
    portEXIT_CRITICAL(&s_mux);
}

static void status_render(void)
{
    char text[40];
    uint32_t color;
    portENTER_CRITICAL(&s_mux);
    memcpy(text, s_status_text, sizeof text);
    color = s_status_color;
    portEXIT_CRITICAL(&s_mux);
    if (strcmp(text, s_status_cache) != 0) {
        snprintf(s_status_cache, sizeof(s_status_cache), "%s", text);
        lv_label_set_text(s_status, s_status_cache);
    }
    if (color != s_status_color_shown) {
        s_status_color_shown = color;
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    }
}

static int16_t row_value(const app_state_t *st, uint8_t row)
{
    switch (row) {
    case SYS_COLOR: return st->settings.theme_on && st->settings.theme_valid ? 5 : st->settings.palette;
    case SYS_ZOOM: return st->settings.zoom;
    case SYS_BRIGHT: return st->sys.bright;
    case SYS_LIGHTS: return st->settings.lights;
    case SYS_VOLUME: return st->sys.vol;
    case SYS_OUTPUT: return st->sys.out;
    case SYS_LOWCUT: return st->settings.lowcut;
    case SYS_RESO: return st->settings.reso;
    default: return 0;
    }
}

static void apply_setting(edit_cmd_type_t type, uint32_t field, uint32_t value)
{
    const edit_cmd_t cmd = {.type = type, .a = field, .v = value};
    state_apply(&cmd);
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int16_t system_action(uint8_t row, int8_t delta)
{
    if (row >= SYS_ROW_COUNT || delta == 0) return 0;
    if (row < SYS_SELFTEST) s_focus = row;
    app_state_t st;
    state_get_copy(&st);

    if (row == SYS_SELFTEST) {
        if (st.core.playing || st.rec.state != SEQ_REC_OFF || st.song.srec != 0) {
            s_act = UI_SYS_ACT_STOP_FIRST;
        } else if (s_selftest_magic == SYS_SELFTEST_MAGIC && s_selftest_ran) {
            s_act = UI_SYS_ACT_RESET_FIRST;
        } else if (!sloop_store_ready()) {
            s_act = UI_SYS_ACT_NO_STORAGE;
        } else {
            const edit_cmd_t cmd = {.type = CMD_SELFTEST_RUN};
            state_apply(&cmd);
            s_selftest_ran = 1;
            s_selftest_magic = SYS_SELFTEST_MAGIC;
            s_act = UI_SYS_ACT_SELFTEST;
            state_get_copy(&st);
        }
        status_record(&st);
        return 0;
    }

    s_act = UI_SYS_ACT_EDIT;
    switch (row) {
    case SYS_COLOR: {
        const int count = st.settings.theme_valid ? 6 : 5;
        int cur = st.settings.theme_on && st.settings.theme_valid ? 5 : st.settings.palette;
        int next = (cur + delta + count) % count;
        if (next == 5) {
            apply_setting(CMD_SETTINGS_SET, SLOOP_SET_THEME_ON, 1);
        } else {
            if (cur == 5) {
                apply_setting(CMD_SETTINGS_SET, SLOOP_SET_THEME_ON, 0);
                state_get_copy(&st);
                if (st.settings_status != APP_SETTINGS_OK) break;
            }
            apply_setting(CMD_SETTINGS_SET, SLOOP_SET_PALETTE, (uint32_t)next);
        }
        break;
    }
    case SYS_ZOOM:
        apply_setting(CMD_SETTINGS_SET, SLOOP_SET_ZOOM, st.settings.zoom ? 0u : 1u);
        break;
    case SYS_BRIGHT: {
        edit_cmd_t cmd = {.type = CMD_SYS_BRIGHT,
                          .v = (uint16_t)clamp_int(st.sys.bright + 10 * delta, 10, 100)};
        state_apply(&cmd);
        break;
    }
    case SYS_LIGHTS:
        apply_setting(CMD_SETTINGS_SET, SLOOP_SET_LIGHTS,
                      (uint32_t)((st.settings.lights + delta + 4) % 4));
        break;
    case SYS_VOLUME: {
        edit_cmd_t cmd = {.type = CMD_SYS_VOLUME,
                          .v = (uint16_t)clamp_int(st.sys.vol + 5 * delta, 0, 100)};
        state_apply(&cmd);
        break;
    }
    case SYS_OUTPUT: {
        edit_cmd_t cmd = {.type = CMD_SYS_OUTPUT, .v = st.sys.out ? 0u : 1u};
        state_apply(&cmd);
        break;
    }
    case SYS_LOWCUT:
        apply_setting(CMD_SETTINGS_SET, SLOOP_SET_LOWCUT, st.settings.lowcut ? 0u : 1u);
        break;
    case SYS_RESO:
        apply_setting(CMD_SETTINGS_SET, SLOOP_SET_RESO,
                      (uint32_t)clamp_int(st.settings.reso + delta, 0, 10));
        break;
    default: break;
    }
    state_get_copy(&st);
    status_record(&st);
    return row_value(&st, row);
}

static void refresh(void)
{
    app_state_t st;
    state_get_copy(&st);
    page_system_update(&st);
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w, int h,
                            const lv_style_t *style, const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (obj == NULL) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_add_style(obj, style, 0);
    lv_label_set_text(obj, text);
    return obj;
}

/* 同一个区域计算供物理点击与 TAP 使用：rel = 指针相对该单对象左缘的 x。 */
static int8_t row_zone_delta(uint8_t row, int32_t rel)
{
    int32_t width = chip_row(row) || row == SYS_SELFTEST ? SYS_CHIP_W : SYS_ROW_W;
    if (rel < 0 || rel >= width) return 0;
    if (row == SYS_SELFTEST || chip_row(row)) return 1;
    if (rel < SYS_ZONE_W) return -1;
    if (rel >= SYS_ROW_W - SYS_ZONE_W) return 1;
    return 0; /* 中段是标签 */
}

static void row_clicked_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(e), &c);
    uint8_t row = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    int8_t delta = row_zone_delta(row, p.x - c.x1);
    if (delta == 0) return;
    (void)system_action(row, delta);
    refresh();
}

static lv_obj_t *make_row(lv_obj_t *parent, uint8_t row)
{
    int x = chip_row(row) ? SYS_CHIP_X : SYS_ROW_X;
    lv_obj_t *obj = make_label(parent, x, row_y(row), chip_row(row) ? SYS_CHIP_W : SYS_ROW_W,
                               row_h(row), chip_row(row) ? &s_st_chip : &s_st_row, "");
    if (obj == NULL) return NULL;
    lv_obj_set_clickable(obj, true);
    lv_obj_add_event_cb(obj, row_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)row);
    return obj;
}

lv_obj_t *page_system_build(lv_obj_t *parent)
{
    if (!s_reset_checked) {
        s_reset_checked = true;
        esp_reset_reason_t reset = esp_reset_reason();
        if (reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT) {
            s_selftest_ran = 0;
            s_selftest_magic = 0;
            s_selftest_last_magic = 0;
        } else if (s_selftest_magic != SYS_SELFTEST_MAGIC) {
            s_selftest_ran = 0; /* warm reset 后随机 noinit 字节不能冒充「已跑过」 */
        }
    }
    app_state_t st;
    state_get_copy(&st);
    if (!st.selftest_done &&
        (s_act == UI_SYS_ACT_NONE || s_act == UI_SYS_ACT_RESET_FIRST) &&
        s_selftest_magic == SYS_SELFTEST_MAGIC &&
        s_selftest_last_magic == SYS_SELFTEST_MAGIC && s_selftest_last_done)
        s_act = UI_SYS_ACT_SELFTEST;
    styles_init();
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_cache, 0, sizeof(s_row_cache));
    s_selftest = NULL;
    s_status = NULL;
    s_focus_shown = 0xFF;
    s_status_cache[0] = '\0';
    s_status_color_shown = 0xFFFFFFFFu;

    static const struct { const char *text; int y; } sections[] = {
        {"SCREEN", 16}, {"LIGHTS", 150}, {"AUDIO", 230}, {"SYSTEM", 430},
    };
    for (uint8_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        UI_BUILD_CHECK(make_label(parent, SYS_ROW_X, sections[i].y, 300, 20, &s_st_section,
                                  sections[i].text));
    }

    for (uint8_t row = 0; row < SYS_SELFTEST; row++) {
        if (chip_row(row)) {
            lv_obj_t *caption = make_label(parent, SYS_ROW_X, row_y(row), SYS_CAPTION_W,
                                           SYS_CHIP_H, &s_st_row,
                                           row == SYS_ZOOM ? "ZOOM" : "SPK LOWCUT");
            UI_BUILD_CHECK(caption);
        }
        s_rows[row] = make_row(parent, row);
        UI_BUILD_CHECK(s_rows[row]);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char version[96];
    snprintf(version, sizeof(version), "APP %s\nSLOOP 2.8.5 (bac9c05)", app->version);
    UI_BUILD_CHECK(make_label(parent, SYS_ROW_X, SYS_VERSION_Y, 700, 48, &s_st_version, version));

    s_selftest = make_label(parent, SYS_ROW_X, SYS_SELFTEST_Y, SYS_CHIP_W, SYS_ROW_H,
                            &s_st_row, "SELFTEST");
    UI_BUILD_CHECK(s_selftest);
    lv_obj_set_clickable(s_selftest, true);
    lv_obj_add_event_cb(s_selftest, row_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)SYS_SELFTEST);

    s_status = make_label(parent, SYS_STATUS_X, SYS_STATUS_Y, SYS_STATUS_W, SYS_STATUS_H,
                          &s_st_version, "");
    UI_BUILD_CHECK(s_status);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    return parent;
}

void page_system_update(const app_state_t *st)
{
    for (uint8_t row = 0; row < SYS_SELFTEST; row++) {
        char text[64];
        int16_t value = row_value(st, row);
        switch (row) {
        case SYS_COLOR: {
            const char *name = value == 5 ? st->settings.theme_name : k_palette_names[value];
            snprintf(text, sizeof(text), "COLOR      -  %s  +", name);
            break;
        }
        case SYS_ZOOM:
        case SYS_LOWCUT: snprintf(text, sizeof(text), "%s", value ? "ON" : "OFF"); break;
        case SYS_BRIGHT: snprintf(text, sizeof(text), "BRIGHT     -  %d%%  +", value); break;
        case SYS_LIGHTS:
            snprintf(text, sizeof(text), "LIGHTS     -  %s  +", k_light_names[value & 3]);
            break;
        case SYS_VOLUME: snprintf(text, sizeof(text), "VOLUME     -  %d  +", value); break;
        case SYS_OUTPUT:
            snprintf(text, sizeof(text), "OUTPUT     -  %s  +", value ? "HEADPHONE" : "SPEAKER");
            break;
        case SYS_RESO:
            if (value == 0) snprintf(text, sizeof(text), "FILTER RESO  -  OFF  +");
            else snprintf(text, sizeof(text), "FILTER RESO  -  %d  +", value);
            break;
        default: text[0] = '\0'; break;
        }
        if (strcmp(text, s_row_cache[row]) != 0) {
            snprintf(s_row_cache[row], sizeof(s_row_cache[row]), "%s", text);
            lv_label_set_text(s_rows[row], s_row_cache[row]);
        }
    }
    if (s_focus_shown != s_focus) {
        for (uint8_t row = 0; row < SYS_SELFTEST; row++) {
            if (row == s_focus) lv_obj_add_style(s_rows[row], &s_st_focus, 0);
            else lv_obj_remove_style(s_rows[row], &s_st_focus, 0);
        }
        s_focus_shown = s_focus;
    }
    status_record(st);
    status_render();
}

void page_system_forget(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_selftest = NULL;
    s_status = NULL;
}

bool page_system_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    for (uint8_t row = 0; row < SYS_ROW_COUNT; row++) {
        if (y < row_y(row) || y >= row_y(row) + row_h(row)) continue;
        int32_t left = row == SYS_SELFTEST ? SYS_ROW_X : chip_row(row) ? SYS_CHIP_X : SYS_ROW_X;
        int8_t delta = row_zone_delta(row, x - left);
        if (delta == 0) return false;
        int16_t value = system_action(row, delta);
        refresh();
        if (out != NULL) {
            app_state_t st;
            state_get_copy(&st);
            out->kind = SLOOP_TAP_SYSTEM;
            out->a = row;
            out->v = value;
            out->d = delta;
            out->err = (uint8_t)st.settings_status;
        }
        return true;
    }
    return false;
}

esp_err_t page_system_key(uint32_t code)
{
    if (code == K_UP || code == K_DOWN) {
        s_focus = (uint8_t)((s_focus + SYS_SELFTEST + (code == K_UP ? -1 : 1)) % SYS_SELFTEST);
        return ESP_OK;
    }
    if (code == K_LEFT || code == K_RIGHT || code == K_ENTER) {
        int8_t delta = code == K_LEFT ? -1 : 1;
        (void)system_action(s_focus, delta);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

const char *page_system_status_text(void)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_status_echo, sizeof(s_status_echo), "%s", s_status_text);
    portEXIT_CRITICAL(&s_mux);
    return s_status_echo;
}

void page_system_clear_action(void)
{
    s_act = UI_SYS_ACT_NONE;
    portENTER_CRITICAL(&s_mux);
    s_status_text[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

/* 诊断注入（console `SYS SELFTEST ARM`，R25）：清「本 boot 已跑过」标记与 `.noinit` 的
 * 结果缓存，让 esptool 硬复位之后（标记按 R8 设计跨复位存活）的下一次 chip 走完整 worker
 * 路径。自检进行中拒绝（ESP_ERR_INVALID_STATE）。同一 boot 内已跑过一次时状态层仍留着上次的
 * 结果（selftest_done=1），状态行会继续显示它直到下一次 RUNNING——本命令只管 chip 的门。 */
esp_err_t page_system_selftest_arm(void)
{
    app_state_t st;
    state_get_copy(&st);
    if (st.selftest_req || (s_act == UI_SYS_ACT_SELFTEST && !st.selftest_done)) return ESP_ERR_INVALID_STATE;
    s_selftest_ran = 0;
    s_selftest_magic = 0;
    s_selftest_last_magic = 0;
    page_system_clear_action();
    return ESP_OK;
}
