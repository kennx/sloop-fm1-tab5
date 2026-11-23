/* 旋钮纯函数 host 单测（scripts/knob-test.sh）：knob_math.c + 上游生成表。
 *
 * 覆盖：
 *  - 值 ↔ 弧线值往返（多组值域，含越界钳位）；
 *  - 环带判定（中心/盒角不响应，环带边界）；
 *  - 角度差归一化（跨底部死区取短路径、atan2 的负角）；
 *  - 相对拖拽换算（270° = 整个值域、四舍五入、两端钳位）；
 *  - 格式化表（fmt → 文本）：F_ENUM/F_PCT/F_BIPCT/F_TIME/F_LFOHZ/F_CUTOFF/F_DB/
 *    F_SEMI/F_BPM/F_NOTE/F_ONOFF/F_STEPS/F_SWING/F_FILT/F_INT（unit/names）/未知。
 *
 * 期望值来自上游生成表的实测索引（felucca_tables.h，注释标索引）与 params.c
 * param_format 的算法；不是从被测函数反推。 */
#include <stdio.h>
#include <string.h>

#include "input_map.h"
#include "knob_math.h"
#include "theme_derive.h"

static int g_failures;
static int g_passed;
static int g_case_failed;
static int g_case_index;

static void check_ll(const char *what, long long got, long long want)
{
    if (got != want) {
        printf("  FAIL k=%d %s: got %lld want %lld\n", g_case_index, what, got, want);
        g_case_failed = 1;
    }
}

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            printf("  FAIL k=%d %s:%d: %s\n", g_case_index, __func__, __LINE__, #expr); \
            g_case_failed = 1;                                                       \
        }                                                                            \
    } while (0)

static void run(const char *name, void (*fn)(void))
{
    g_case_failed = 0;
    g_case_index++;
    fn();
    if (g_case_failed) {
        printf("FAIL k=%d %s\n", g_case_index, name);
        g_failures++;
    } else {
        printf("PASS k=%d %s\n", g_case_index, name);
        g_passed++;
    }
}

static sloop_desc_t mk(uint8_t fmt, int16_t min, int16_t max, const char *const *enums,
                       uint8_t enum_count, const char *unit)
{
    sloop_desc_t d;
    memset(&d, 0, sizeof(d));
    d.label = "X";
    d.fmt = fmt;
    d.min = min;
    d.max = max;
    d.enums = enums;
    d.enum_count = enum_count;
    d.unit = unit;
    return d;
}

static void expect_str(const char *what, const sloop_desc_t *d, int16_t v, const char *want)
{
    char buf[24];
    knob_format_value(d, v, buf, sizeof(buf));
    if (strcmp(buf, want) != 0) {
        printf("  FAIL k=%d %s: v=%d got \"%s\" want \"%s\"\n", g_case_index, what, (int)v, buf,
               want);
        g_case_failed = 1;
    }
}

/* ① 值 ↔ 弧线值往返：range ≤ 1000 时必须是精确双射（UI 拖动按此映射） */
static void case_arc_roundtrip(void)
{
    static const struct {
        int16_t min, max;
    } ranges[] = {{0, 127}, {-64, 63}, {40, 240}, {0, 4}, {1, 16}};
    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        sloop_desc_t d = mk(SLOOP_DESC_FMT_INT, ranges[r].min, ranges[r].max, NULL, 0, NULL);
        for (int32_t v = ranges[r].min; v <= ranges[r].max; v++) {
            int32_t a = knob_value_to_arc(&d, (int16_t)v);
            CHECK(a >= 0 && a <= KNOB_ARC_RANGE);
            check_ll("arc→value 往返", knob_arc_to_value(&d, a), v);
        }
    }
    sloop_desc_t d = mk(SLOOP_DESC_FMT_INT, 0, 127, NULL, 0, NULL);
    check_ll("越界值钳到 max", knob_value_to_arc(&d, 500), KNOB_ARC_RANGE);
    check_ll("越界值钳到 min", knob_value_to_arc(&d, -5), 0);
    check_ll("弧线越界钳到 max", knob_arc_to_value(&d, 2000), 127);
    check_ll("弧线越界钳到 min", knob_arc_to_value(&d, -1), 0);
}

/* ② 环带：半径 55..95px 才响应（中心/盒角不跳值） */
static void case_ring(void)
{
    CHECK(knob_ring_hit(0, -73));   /* 环线正上 */
    CHECK(knob_ring_hit(73, 0));
    CHECK(knob_ring_hit(0, 73));
    CHECK(knob_ring_hit(-73, 0));
    CHECK(knob_ring_hit(0, -55));   /* 内边界 */
    CHECK(knob_ring_hit(0, -95));   /* 外边界 */
    CHECK(!knob_ring_hit(0, 0));    /* 中心：只设焦点 */
    CHECK(!knob_ring_hit(0, -40));  /* 内圈外沿以内 */
    CHECK(!knob_ring_hit(0, -100)); /* 盒外 */
    CHECK(!knob_ring_hit(96, 0));
}

/* ③ 角度差归一化：(-180, 180]，跨底部死区取短路径 */
static void case_angle_delta(void)
{
    check_ll("270→0", knob_angle_delta(270, 0), 90);
    check_ll("0→270", knob_angle_delta(0, 270), -90);
    check_ll("350→10", knob_angle_delta(350, 10), 20);
    check_ll("10→350", knob_angle_delta(10, 350), -20);
    check_ll("同角", knob_angle_delta(123, 123), 0);
    check_ll("负角（atan2 返回 -90）→0", knob_angle_delta(-90, 0), 90);
    check_ll("0→180", knob_angle_delta(0, 180), 180);
    check_ll("180→0", knob_angle_delta(180, 0), 180);
}

/* ④ 相对拖拽：270° = 整个值域；四舍五入、两端钳位 */
static void case_drag(void)
{
    sloop_desc_t bpm = mk(SLOOP_DESC_FMT_BPM, 40, 240, NULL, 0, NULL); /* range 200 */
    check_ll("无位移", knob_drag_value(&bpm, 90, 0), 90);
    check_ll("+90° → +67", knob_drag_value(&bpm, 90, 90), 157);
    check_ll("-90° → -67（撞下端 40）", knob_drag_value(&bpm, 90, -90), 40);
    check_ll("+3° → +2", knob_drag_value(&bpm, 90, 3), 92);
    check_ll("-3° → -2", knob_drag_value(&bpm, 90, -3), 88);
    check_ll("上端钳位", knob_drag_value(&bpm, 230, 90), 240);
    check_ll("下端钳位", knob_drag_value(&bpm, 50, -90), 40);

    sloop_desc_t bip = mk(SLOOP_DESC_FMT_BIPCT, -64, 63, NULL, 0, NULL); /* range 127 */
    check_ll("双极 +90° → +42", knob_drag_value(&bip, 0, 90), 42);
    check_ll("双极 -90° → -42", knob_drag_value(&bip, 0, -90), -42);
    check_ll("双极 +3° → +1", knob_drag_value(&bip, 0, 3), 1);

    sloop_desc_t wave = mk(SLOOP_DESC_FMT_ENUM, 0, 4, NULL, 0, NULL); /* range 4 */
    check_ll("WAVE +90° → +1", knob_drag_value(&wave, 0, 90), 1);
    check_ll("WAVE +135° → +2", knob_drag_value(&wave, 2, 135), 4);
    check_ll("WAVE -135° → -2", knob_drag_value(&wave, 2, -135), 0);
    check_ll("WAVE 180° 钳位", knob_drag_value(&wave, 4, 180), 4);
}

/* ⑤ 格式化表 */
static void case_format(void)
{
    static const char *const waves[] = {"SAW", "SQR", "TRI", "SIN", "PWM"};
    static const char *const esync[] = {"0", "1/4", "1/2", NULL};
    static const char *const onoff[] = {"OFF", "ON"};
    static const char *const notes[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                       "F#", "G",  "G#", "A",  "A#", "B"};

    /* F_ENUM：索引 v-min（PE 约定 min=0）+ desc.unit */
    sloop_desc_t d = mk(SLOOP_DESC_FMT_ENUM, 0, 4, waves, 5, NULL);
    expect_str("ENUM 0", &d, 0, "SAW");
    expect_str("ENUM 4", &d, 4, "PWM");
    expect_str("ENUM 越界钳位", &d, 9, "PWM");

    /* F_PCT：v*100/max，四舍五入 */
    d = mk(SLOOP_DESC_FMT_PCT, 0, 127, NULL, 0, NULL);
    expect_str("PCT 0", &d, 0, "0%");
    expect_str("PCT 90", &d, 90, "71%");
    expect_str("PCT 127", &d, 127, "100%");

    /* F_BIPCT：v*100/64；仅 v>0 加 '+' */
    d = mk(SLOOP_DESC_FMT_BIPCT, -64, 63, NULL, 0, NULL);
    expect_str("BIPCT 0（无 +）", &d, 0, "0%");
    expect_str("BIPCT +32", &d, 32, "+50%");
    expect_str("BIPCT -32", &d, -32, "-50%");
    expect_str("BIPCT +1", &d, 1, "+1%");

    /* F_TIME：TIME_MS_X10[0]=10 / [10]=21 / [60]=776 / [90]=6834 / [104]=18862 / [127]=100000
     * [104] 故意取非整两位的值：上游 fmt_fix 截断 → 1.88s（printf 四舍五入会给 1.89s） */
    d = mk(SLOOP_DESC_FMT_TIME, 0, 127, NULL, 0, NULL);
    expect_str("TIME 0", &d, 0, "1.0ms");
    expect_str("TIME 10", &d, 10, "2.1ms");
    expect_str("TIME 60", &d, 60, "78ms");
    expect_str("TIME 90", &d, 90, "683ms");
    expect_str("TIME 104（截断 1.8862→1.88）", &d, 104, "1.88s");
    expect_str("TIME 127", &d, 127, "10.0s");

    /* F_LFOHZ：LFO_HZ_X100[0]=5 / [60]=118 / [90]=571 / [104]=1192 / [105]=1257
     * <10Hz 档 h/100 恰两位（571→5.71）；≥10Hz 档先 h/10 再 1 位（截断）：
     * [105] 1257 → 12.5Hz（12.57 四舍五入会给 12.6Hz） */
    d = mk(SLOOP_DESC_FMT_LFOHZ, 0, 127, NULL, 0, NULL);
    expect_str("LFOHZ 0", &d, 0, "0.05Hz");
    expect_str("LFOHZ 60", &d, 60, "1.18Hz");
    expect_str("LFOHZ 90", &d, 90, "5.71Hz");
    expect_str("LFOHZ 104", &d, 104, "11.9Hz");
    expect_str("LFOHZ 105（截断 1257→12.5）", &d, 105, "12.5Hz");

    /* F_CUTOFF：CUTOFF_HZ[0]=30 / [60]=583 / [90]=2568 / [104]=5132 / [127]=16000
     * [90] 2568 → h/100=25 → 2.5kHz（截断；printf 会给 2.6kHz） */
    d = mk(SLOOP_DESC_FMT_CUTOFF, 0, 127, NULL, 0, NULL);
    expect_str("CUTOFF 0", &d, 0, "30Hz");
    expect_str("CUTOFF 60", &d, 60, "583Hz");
    expect_str("CUTOFF 90（截断 2568→2.5）", &d, 90, "2.5kHz");
    expect_str("CUTOFF 104", &d, 104, "5.1kHz");
    expect_str("CUTOFF 127", &d, 127, "16kHz");

    /* F_DB：LEVEL_DB_X10[1]=-555 / [104]=-40 / [127]=75；0 = OFF */
    d = mk(SLOOP_DESC_FMT_DB, 0, 127, NULL, 0, NULL);
    expect_str("DB 0", &d, 0, "OFF");
    expect_str("DB 1", &d, 1, "-56dB");
    expect_str("DB 104", &d, 104, "-4.0dB");
    expect_str("DB 127", &d, 127, "7.5dB");
    /* 负数且 |db10| < 一个显示单位（|v| < 10）：整数分频得 ip=0，符号必须单独补。
     * LEVEL_DB_X10[111] = -5 → "-0.5dB"（上游 fmt_fix 输出 "-.5"；这里同值、
     * 补前导 0 以便读数）；[110] = -10 是边界（ip=-1，走常规负号）。 */
    expect_str("DB 111（-0.5dB，次单位负）", &d, 111, "-0.5dB");
    expect_str("DB 110（-1.0dB 边界）", &d, 110, "-1.0dB");

    /* F_SEMI：仅 v>0 加 '+' */
    d = mk(SLOOP_DESC_FMT_SEMI, -24, 24, NULL, 0, NULL);
    expect_str("SEMI 0（无 +）", &d, 0, "0st");
    expect_str("SEMI +2", &d, 2, "+2st");
    expect_str("SEMI -2", &d, -2, "-2st");

    d = mk(SLOOP_DESC_FMT_BPM, 40, 240, NULL, 0, NULL);
    expect_str("BPM", &d, 90, "90BPM");
    d = mk(SLOOP_DESC_FMT_NOTE, 0, 11, notes, 12, NULL);
    expect_str("NOTE C", &d, 0, "C");
    expect_str("NOTE A", &d, 9, "A");
    d = mk(SLOOP_DESC_FMT_ONOFF, 0, 1, onoff, 2, NULL);
    expect_str("ONOFF 1", &d, 1, "ON");
    d = mk(SLOOP_DESC_FMT_STEPS, 1, 64, NULL, 0, NULL);
    expect_str("STEPS", &d, 16, "16STEP");
    d = mk(SLOOP_DESC_FMT_SWING, 0, 100, NULL, 0, NULL);
    expect_str("SWING", &d, 50, "50");
    d = mk(SLOOP_DESC_FMT_FILT, -64, 63, NULL, 0, NULL);
    expect_str("FILT 0", &d, 0, "OFF");
    expect_str("FILT -32", &d, -32, "LP50%");
    expect_str("FILT +32", &d, 32, "HP51%");

    /* F_INT：desc.unit（ANALOG DTN "ct"）；0 终止 names 均分映射 */
    d = mk(SLOOP_DESC_FMT_INT, 0, 127, NULL, 0, "ct");
    expect_str("INT unit", &d, 10, "10ct");
    d = mk(SLOOP_DESC_FMT_INT, 0, 127, esync, 3, NULL);
    expect_str("INT names 0", &d, 0, "0");
    expect_str("INT names 64", &d, 64, "1/4");
    expect_str("INT names 127", &d, 127, "1/2");
    /* 未知 fmt（F_OCT=12）回落原始整数 */
    d = mk(SLOOP_DESC_FMT_OCT, 1, 4, NULL, 0, NULL);
    expect_str("未知 fmt 原始值", &d, 3, "3");
}

/* 鼓演奏键位（ADR-006）：低行白键 z x c v b n m . → lane 0..7，高行白键
 * q w e r t y u i → lane 8..15；其它键（半音黑键/走带/导航）不映射，选中鼓轨时
 * 不回落合成轨。 */
static void case_drum_lanes(void)
{
    static const uint8_t low[8] = { 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49 };
    static const uint8_t high[8] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28 };
    for (int i = 0; i < 8; i++) {
        CHECK(input_map_drum_lane(low[i]) == i);
        CHECK(input_map_drum_lane(high[i]) == 8 + i);
    }
    CHECK(input_map_drum_lane(0x33) < 0); /* s */
    CHECK(input_map_drum_lane(0x34) < 0); /* d */
    CHECK(input_map_drum_lane(0x36) < 0); /* g */
    CHECK(input_map_drum_lane(0x37) < 0); /* h */
    CHECK(input_map_drum_lane(0x38) < 0); /* j */
    CHECK(input_map_drum_lane(0x02) < 0); /* 2 */
    CHECK(input_map_drum_lane(0x03) < 0); /* 3 */
    CHECK(input_map_drum_lane(0x05) < 0); /* 5 */
    CHECK(input_map_drum_lane(0x06) < 0); /* 6 */
    CHECK(input_map_drum_lane(0x07) < 0); /* 7 */
    CHECK(input_map_drum_lane(0x4D) < 0); /* space */
    CHECK(input_map_drum_lane(0x0A) < 0); /* 0 = REC */
    CHECK(input_map_drum_lane(0x0B) < 0); /* - = FILL 保持 */
    CHECK(input_map_drum_lane(0x0C) < 0); /* + = FILL NEXT */
    CHECK(input_map_drum_lane(0x7F) < 0);
}

/* 演奏键路由（Task 6 fix 2 回归）：选中鼓轨时按下鼓键走鼓、其它键忽略；
 * **释放永远回按下时的轨**（选中鼓轨后松开合成键不得挂音）。 */
static void case_key_route(void)
{
    CHECK(input_map_key_route(3, 0x42, false) == INPUT_ROUTE_NOTE);
    CHECK(input_map_key_route(0, 0x42, false) == INPUT_ROUTE_NOTE);
    CHECK(input_map_key_route(3, 0x42, true) == INPUT_ROUTE_DRUM);
    CHECK(input_map_key_route(3, 0x28, true) == INPUT_ROUTE_DRUM);
    CHECK(input_map_key_route(3, 0x33, true) == INPUT_ROUTE_IGNORE);
    CHECK(input_map_key_route(3, 0x4D, true) == INPUT_ROUTE_IGNORE);
    CHECK(input_map_key_route(0, 0x42, true) == INPUT_ROUTE_NOTE);
    CHECK(input_map_key_route(2, 0x02, true) == INPUT_ROUTE_NOTE);
}

static void case_theme_mono_equals_constants(void)
{
    ui_theme_t t;
    ui_theme_derive_mono(&t);
    check_ll("MONO bg", t.bg, 0x101418);
    check_ll("MONO panel", t.panel, 0x1B2026);
    check_ll("MONO panel_hi", t.panel_hi, 0x242B33);
    check_ll("MONO panel_sel", t.panel_sel, 0x2C3540);
    check_ll("MONO text", t.text, 0xF2F5F8);
    check_ll("MONO text_dim", t.text_dim, 0x8A949E);
    check_ll("MONO accent", t.accent, 0xE0B32A);
    check_ll("MONO error", t.error, 0xE0542A);
    static const uint32_t tracks[4] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    for (int i = 0; i < 4; i++) check_ll("MONO track", t.track[i], tracks[i]);
}

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static void case_theme_green_accent(void)
{
    static const uint16_t green[5] = {
        RGB565(0, 40, 12), RGB565(0, 84, 30), RGB565(16, 140, 54),
        RGB565(56, 200, 92), RGB565(120, 255, 146),
    };
    ui_theme_t t;
    ui_theme_derive_palette(green, &t);
    check_ll("GREEN accent", t.accent, 0x78FF92);
    check_ll("GREEN bg 不变", t.bg, 0x101418);
    static const uint32_t tracks[4] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    for (int i = 0; i < 4; i++) check_ll("GREEN track 不变", t.track[i], tracks[i]);
}

static void case_theme_custom_overrides(void)
{
    song_theme_view_t th;
    memset(&th, 0, sizeof th);
    for (int i = 0; i < 5; i++) {
        th.c[i][0] = (uint8_t)(20 + i * 20);
        th.c[i][1] = (uint8_t)(30 + i * 20);
        th.c[i][2] = (uint8_t)(40 + i * 20);
    }
    for (int i = 0; i < 4; i++) {
        th.trk[i][0] = (uint8_t)(10 + i);
        th.trk[i][1] = (uint8_t)(20 + i);
        th.trk[i][2] = (uint8_t)(30 + i);
    }
    th.bg[0] = 1; th.bg[1] = 2; th.bg[2] = 3;
    th.hi[0] = 240; th.hi[1] = 230; th.hi[2] = 220;
    ui_theme_t t;
    ui_theme_derive_theme(&th, &t);
    check_ll("THEME bg", t.bg, 0x010203);
    check_ll("THEME accent", t.accent, 0xF0E6DC);
    check_ll("THEME text 由 hi 派生", t.text, 0xF0E6DC);
    for (int i = 0; i < 4; i++) {
        check_ll("THEME track", t.track[i],
                 ((uint32_t)th.trk[i][0] << 16) | ((uint32_t)th.trk[i][1] << 8) | th.trk[i][2]);
    }
}

int main(void)
{
    run("值 ↔ 弧线值往返与钳位", case_arc_roundtrip);
    run("环带判定（55..95px）", case_ring);
    run("角度差归一化（跨死区/负角）", case_angle_delta);
    run("相对拖拽换算与钳位", case_drag);
    run("格式化表（上游生成表口径）", case_format);
    run("鼓演奏 16 lane 键位（不回落合成轨）", case_drum_lanes);
    run("演奏键路由（释放永远回原轨）", case_key_route);
    run("MONO 色表恰等于现行常量", case_theme_mono_equals_constants);
    run("GREEN accent 与固定字段", case_theme_green_accent);
    run("自定义 theme 覆盖颜色", case_theme_custom_overrides);

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
