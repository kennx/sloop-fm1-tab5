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
 * param_format 的算法；不是从被测函数反推。
 *
 * P4-F Task 4 追加（参数页 chip 框架 + 九组导航的**纯数据**门禁）：page_layout.c 的
 * 布局表（页/chip/槽/drum 规则/条件 chip/est）与 ui_nav.h 的组表。槽位期望硬编码自
 * docs/design/012-feature-parity-checklist.md §3（权威槽表），参数 id 用上游 core.h:48-75
 * 的枚举顺序在测试里独立复刻（tP_*）——布局表自己写的是 L_P_* 数字，抄错即 FAIL。 */
#include <stdio.h>
#include <string.h>

#include "input_map.h"
#include "knob_math.h"
#include "page_layout.h"
#include "theme_derive.h"
#include "ui_nav.h"

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
    /* D3 中性灰染色：base + (c-mono)/2（C 整数除法向零截断）；panel_sel 的
     * 输入是 c[1]/c[2] 的半点 = (50,60,70)，对应 MONO 的 (105,105,105)。 */
    check_ll("THEME panel 由 c0 派生", t.panel, 0x111B26);
    check_ll("THEME panel_hi 由 c1 派生", t.panel_hi, 0x101C29);
    check_ll("THEME panel_sel 由 c1/c2 派生", t.panel_sel, 0x111F2F);
    check_ll("THEME text_dim 由 c2 派生", t.text_dim, 0x677685);
    check_ll("THEME error 固定", t.error, 0xE0542A);
    for (int i = 0; i < 4; i++) {
        check_ll("THEME track", t.track[i],
                 ((uint32_t)th.trk[i][0] << 16) | ((uint32_t)th.trk[i][1] << 8) | th.trk[i][2]);
    }
}


/* ---------------------------------------------------------------- 参数页布局表
 * 上游 core.h:48-75 的每轨参数枚举顺序（独立复刻；只列本测试用到的锚点，靠顺序
 * 隐式给值）。布局表 page_layout.c 用显式数字（L_P_*），两处不一致即 FAIL。 */
enum {
    tP_LEVEL, tP_ATK, tP_DEC, tP_SUS, tP_REL, tP_ED_FLT, tP_ED_PIT, tP_ED_SHP, tP_ED_FX,
    tP_LRATE, tP_LWAVE, tP_LPHASE, tP_LFADE, tP_LD_PIT, tP_LD_FLT, tP_LD_SHP, tP_LD_AMP,
    tP_AMODE, tP_ARATE, tP_AOCT, tP_AGATE, tP_ASWING, tP_APROB, tP_AHOLD, tP_AORDER,
    tP_ROOT, tP_SCALE, tP_QUANT, tP_TRANS, tP_SLEN, tP_SDIV, tP_SSWING, tP_SGATE,
    tP_DIST, tP_CHOR, tP_DLY, tP_REV, tP_VOICE, tP_GLIDE, tP_PAN, tP_MUTE, tP_GLMODE,
    tP_PRIO, tP_ALLOC, tP_DETUNE, tP_SLCR, tP_SLPAT, tP_SLRATE, tP_SLDEPTH, tP_CHORD,
    tP_TFLT, tP_STRUM, tP_VLEAD, tP_ITYPE, tP_IA, tP_IB, tP_IC, tP_IMIX, tP_FTYPE,
    tP_ESYNC, tP_LSYNC, tP_LTRIG, tP_LPOL, tP_E0, tP_E1, tP_E2, tP_E3, tP_E4, tP_E5,
    tP_E6, tP_E7, tP_COUNT,
};
/* 上游 core.h:76-96 的全局参数枚举顺序（独立复刻；只列锚点） */
enum {
    tG_BPM, tG_SWING, tG_CLOCK, tG_TUNE, tG_DTIME, tG_DFDBK, tG_DCOLOR, tG_DMIX,
    tG_RSIZE, tG_RDAMP, tG_CRATE, tG_CDEPTH,
    tG_MIDI, tG_SYNC, tG_ROUTE, tG_INFO, tG_SLOT, tG_NAME, tG_LOAD, tG_SAVE,
    tG_ENGSEL, tG_ENGGO, tG_CLRSEQ, tG_INITSND, tG_DRCH, tG_DRLVL, tG_DRREV,
    tG_DUST, tG_DUCK, tG_FILT, tG_ROLL, tG_NEWPRJ, tG_DRDLY,
    tG_CH1, tG_CH2, tG_CH3, tG_CHOTH, tG_TXSRC, tG_TXLVL, tG_TXTONE, tG_TXMOVE,
    tG_TXRATE, tG_TXWHEN, tG_TXDUCK,
};

/* 期望 chip：{名, when, 槽数, 槽 id…（TRACK scope 的绝对 id；ENGINE scope = E0+id）} */
typedef struct {
    const char *name;
    uint8_t when;
    uint8_t n;
    uint8_t ids[PARAM_PAGE_SLOTS];
    uint8_t drum_ok; /* 位 k = 槽 k 在鼓轨上可编辑 */
    uint8_t engine_scope; /* 位 k = 槽 k 是 ENGINE scope（页声明引擎 0 = ANALOG） */
    uint8_t global;       /* 1 = 全部槽是 GLOBAL scope（id 为绝对全局 id） */
} exp_chip_t;

#define EXP(nm, wh, drum, eng, ...)                                              \
    { nm, wh, (uint8_t)(sizeof((uint8_t[]){__VA_ARGS__}) / sizeof(uint8_t)),       \
      {__VA_ARGS__}, drum, eng, 0 }
#define EXPG(nm, ...)                                                            \
    { nm, PARAM_CHIP_ALWAYS, (uint8_t)(sizeof((uint8_t[]){__VA_ARGS__}) / sizeof(uint8_t)), \
      {__VA_ARGS__}, 0xFF, 0x00, 1 }

static const exp_chip_t k_exp_mod[] = {
    EXP("ENV", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_ATK, tP_DEC, tP_SUS, tP_REL),
    EXP("ENV DEST", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_ED_FLT, tP_ED_PIT, tP_ED_SHP, tP_ESYNC),
    EXP("LFO", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_LRATE, tP_LWAVE, tP_LPHASE, tP_LFADE),
    EXP("LFO 2", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_LSYNC, tP_LTRIG, tP_LPOL),
    EXP("LFO DEST", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_LD_PIT, tP_LD_FLT, tP_LD_SHP, tP_LD_AMP),
};
static const exp_chip_t k_exp_scale[] = {
    EXP("SEL", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_ROOT, tP_SCALE, tP_QUANT, tP_CHORD),
    EXP("SEL 2", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_TRANS, tP_STRUM, tP_VLEAD),
    EXP("ARP", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_AMODE, tP_ARATE, tP_AOCT, tP_AGATE),
    EXP("ARP 2", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_ASWING, tP_APROB, tP_AHOLD, tP_AORDER),
};
static const exp_chip_t k_exp_voice[] = {
    EXP("VOICE", PARAM_CHIP_ALWAYS, 0x00, 0x00, tP_VOICE, tP_GLIDE, tP_GLMODE, tP_PRIO),
    EXP("VOICE 2", PARAM_CHIP_ALWAYS, 0x04, 0x00, tP_ALLOC, tP_DETUNE, tP_PAN, tP_MUTE),
    EXP("PATTERN", PARAM_CHIP_ALWAYS, 0x0F, 0x00,
        tP_SLEN, tP_SDIV, tP_SSWING, tP_SGATE),
    EXP("FILTER", PARAM_CHIP_WHEN_ANALOG, 0x00, 0x0E, tP_FTYPE, tP_E4, tP_E5, tP_E7),
};

static const exp_chip_t k_exp_fx[] = {
    EXP("FX", PARAM_CHIP_ALWAYS, 0x70, 0x00, tP_DIST, tP_CHOR, tP_DLY, tP_REV, tP_TFLT,
        tP_LEVEL, tP_PAN, tP_ATK),
    EXP("SLICER", PARAM_CHIP_ALWAYS, 0x0F, 0x00, tP_SLCR, tP_SLPAT, tP_SLRATE, tP_SLDEPTH),
    EXP("INSERT", PARAM_CHIP_ALWAYS, 0x0F, 0x00, tP_ITYPE, tP_IA, tP_IB, tP_IC),
    EXP("INSERT 2", PARAM_CHIP_ALWAYS, 0x01, 0x00, tP_IMIX),
};
static const exp_chip_t k_exp_global[] = {
    EXPG("GLO", tG_BPM, tG_SWING, tG_CLOCK, tG_TUNE, tG_DTIME, tG_DFDBK, tG_DCOLOR, tG_DMIX),
    EXPG("REV", tG_RSIZE, tG_RDAMP, tG_CRATE, tG_CDEPTH, tG_DUST, tG_DUCK, tG_FILT, tG_ROLL),
    EXPG("DRUMS", tG_DRCH, tG_DRLVL, tG_DRREV, tG_DRDLY),
    EXPG("TEXTURE", tG_TXSRC, tG_TXLVL, tG_TXTONE, tG_TXMOVE, tG_TXRATE, tG_TXWHEN, tG_TXDUCK),
};

static void layout_check_chips(const param_page_t *pg, const exp_chip_t *exp, uint8_t n)
{
    char what[64];
    snprintf(what, sizeof(what), "%s chip_count", pg->title);
    check_ll(what, pg->chip_count, n);
    for (uint8_t c = 0; c < PARAM_PAGE_CHIPS; c++) {
        const param_chip_t *ch = &pg->chips[c];
        if (c >= n) {
            /* 未用 chip 槽必须全零（零填充 = 无 chip；不留悬空指针） */
            const uint8_t *raw = (const uint8_t *)ch;
            uint32_t acc = 0;
            for (size_t i = 0; i < sizeof(*ch); i++) acc |= raw[i];
            snprintf(what, sizeof(what), "%s chips[%u] 未用槽全零", pg->title, c);
            check_ll(what, acc, 0);
            continue;
        }
        snprintf(what, sizeof(what), "%s chips[%u].name", pg->title, c);
        check_ll(what, ch->name != NULL && strcmp(ch->name, exp[c].name) == 0, 1);
        snprintf(what, sizeof(what), "%s %s when", pg->title, exp[c].name);
        check_ll(what, ch->when, exp[c].when);
        snprintf(what, sizeof(what), "%s %s slot_count", pg->title, exp[c].name);
        check_ll(what, ch->slot_count, exp[c].n);
        for (uint8_t k = 0; k < PARAM_PAGE_SLOTS; k++) {
            snprintf(what, sizeof(what), "%s %s slot[%u]", pg->title, exp[c].name, k);
            if (k >= ch->slot_count) {
                /* slot_count 之外的槽必须零填充（代码只读 < slot_count） */
                const uint8_t *raw = (const uint8_t *)&ch->slots[k];
                uint32_t acc = 0;
                for (size_t i = 0; i < sizeof(ch->slots[k]); i++) acc |= raw[i];
                check_ll(what, acc, 0);
                continue;
            }
            snprintf(what, sizeof(what), "%s %s slot[%u].id", pg->title, exp[c].name, k);
            /* ENGINE scope 存相对号（P_E0 + id）；TRACK/GLOBAL 存绝对 id */
            check_ll(what, ch->slots[k].id,
                     ((exp[c].engine_scope >> k) & 1u) ? exp[c].ids[k] - tP_E0 : exp[c].ids[k]);
            snprintf(what, sizeof(what), "%s %s slot[%u].drum_ok", pg->title, exp[c].name, k);
            check_ll(what, ch->slots[k].drum_ok, (exp[c].drum_ok >> k) & 1u);
            snprintf(what, sizeof(what), "%s %s slot[%u].scope", pg->title, exp[c].name, k);
            check_ll(what, ch->slots[k].scope,
                     exp[c].global ? SLOOP_DESC_GLOBAL
                     : ((exp[c].engine_scope >> k) & 1u) ? SLOOP_DESC_ENGINE : SLOOP_DESC_TRACK);
            if (exp[c].global) {
                snprintf(what, sizeof(what), "%s %s slot[%u].drum_ok (global)", pg->title,
                         exp[c].name, k);
                check_ll(what, ch->slots[k].drum_ok, 1); /* 全局参数鼓轨恒可编辑 */
            }
            if ((exp[c].engine_scope >> k) & 1u) {
                snprintf(what, sizeof(what), "%s %s slot[%u].engine", pg->title, exp[c].name, k);
                check_ll(what, ch->slots[k].engine, 0); /* ANALOG */
            }
        }
    }
}

/* 布局表：页数/页 id/标题/est。既有五页的 id 与 est 不变；MOD/SCALE/VOICE 追加在
 * app_page_t 末尾（15..17）。chip 页 est ≤ 5,120（控制器上限），且 EDIT 组 Σ est 满足
 * ADR-005 §5 的 EDIT→STEP 有序对：base 11,624 + Σ + STEP 23,040 + 4,096 ≤ 63,120。 */
#define UI_BASE_BYTES 11624      /* ADR-005 §5：非页面基础占用（实测） */
#define UI_STEP_EST 23040        /* ui.c k_builtin_pages 的 STEP est */
#define UI_POOL_AVAIL 63120      /* TLSF 可用（64KB 池） */
#define UI_CHIP_PAGE_EST_MAX 5120u

static void case_layout_pages(void)
{
    check_ll("PARAM_PAGE_COUNT", PARAM_PAGE_COUNT, 7);
    static const uint8_t ids[PARAM_PAGE_COUNT] = {
        APP_PAGE_GLOBAL, APP_PAGE_FX, APP_PAGE_ANALOG, APP_PAGE_SAMPLE,
        APP_PAGE_MOD, APP_PAGE_SCALE, APP_PAGE_VOICE,
    };
    static const char *const titles[PARAM_PAGE_COUNT] = {"GLOBAL", "FX",  "SOUND", "SAMPLE",
                                                          "MOD",    "SCALE", "VOICE"};
    check_ll("SOUND 页沿用 ANALOG 的 id", APP_PAGE_ANALOG, 6);
    check_ll("FM6 页 id 保留为空位", APP_PAGE_FM6, 7);
    check_ll("SAMPLE id 不移位", APP_PAGE_SAMPLE, 8);
    check_ll("APP_STATE_PAGES", APP_STATE_PAGES, 19);
    uint32_t edit_est = 0, mix_est = 0, mix_max = 0;
    const param_page_t *mod = page_layout_get(page_layout_index(APP_PAGE_MOD));
    CHECK(mod != NULL);
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        const param_page_t *pg = page_layout_get(i);
        CHECK(pg != NULL);
        if (pg == NULL) continue;
        check_ll("page id", pg->page_id, ids[i]);
        check_ll("title", pg->title != NULL && strcmp(pg->title, titles[i]) == 0, 1);
        check_ll("est 取值", page_layout_est(i), pg->est_bytes);
        check_ll("page_id → 下标", page_layout_index(ids[i]), i);
        if (i == 2) {
            /* SOUND：8 旋钮 + 页眉两行 + 条件 bank 行；est 6,144 起、上限 7,168 */
            CHECK(pg->est_bytes >= 6144u && pg->est_bytes <= 7168u);
        } else if (i == 3) {
            check_ll("SAMPLE（旧式整页）est 不变", pg->est_bytes, 6144u);
        } else {
            /* 六张 chip 页对象设计相同 ⇒ est 相同（同一实测 delta 定稿，不逐页漂移） */
            CHECK(pg->est_bytes > 0 && pg->est_bytes <= UI_CHIP_PAGE_EST_MAX);
            if (mod) check_ll("chip 页 est 相同", pg->est_bytes, mod->est_bytes);
        }
        if (ids[i] == APP_PAGE_MOD || ids[i] == APP_PAGE_SCALE || ids[i] == APP_PAGE_VOICE ||
            ids[i] == APP_PAGE_ANALOG) {
            edit_est += pg->est_bytes;
        }
        if (ids[i] == APP_PAGE_GLOBAL || ids[i] == APP_PAGE_FX) {
            CHECK(pg->est_bytes <= 7168u); /* MIX 两页的上限 */
            mix_est += pg->est_bytes;
            if (pg->est_bytes > mix_max) mix_max = pg->est_bytes;
        }
    }
    check_ll("越界下标", page_layout_get(PARAM_PAGE_COUNT) == NULL, 1);
    check_ll("非参数页 → -1", page_layout_index(APP_PAGE_PLAY), -1);
    check_ll("FM6 页不再注册（空位 id）", page_layout_index(APP_PAGE_FM6), -1);
    check_ll("未注册 id → -1", page_layout_index(APP_STATE_PAGES), -1);
    /* 有序对账面 = base + Σ est + 4KB ≤ 池可用（ADR-005 §5） */
    check_ll("EDIT Σ est", edit_est, 6144u + 3u * (mod ? mod->est_bytes : 0));
    check_ll("MIX Σ est", mix_est, 2u * mix_max);
    CHECK((long long)UI_BASE_BYTES + edit_est + UI_STEP_EST + 4096 <= UI_POOL_AVAIL); /* EDIT→STEP */
    CHECK((long long)UI_BASE_BYTES + mix_est + UI_STEP_EST + 4096 <= UI_POOL_AVAIL);  /* MIX→STEP */
    /* SEQ→MIX：跨组峰值 = 源组已建 Σ（SEQ 两页）+ 目标页 est */
    CHECK((long long)UI_BASE_BYTES + 23040u + 9216u + mix_max + 4096 <= UI_POOL_AVAIL);
}

/* 布局表：逐页 chip 完整性（≤8 槽、id 按作用域合法、chip 名页内唯一、when 枚举合法、
 * 旧式整页 = 无名单 chip 且不画 chip 行）。 */
static void case_layout_integrity(void)
{
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        const param_page_t *pg = page_layout_get(i);
        CHECK(pg != NULL);
        if (pg == NULL) continue;
        check_ll("chip_count ∈ {0} ∪ [2..CHIPS]",
                 pg->chip_count == 0 || (pg->chip_count >= 2 && pg->chip_count <= PARAM_PAGE_CHIPS),
                 1);
        uint8_t named = page_layout_chip_count(pg);
        check_ll("features 只用已定义的位",
                 (pg->features & ~(uint8_t)(PARAM_FEAT_SOUND | PARAM_FEAT_BANK_FM6 | PARAM_FEAT_SAMPLE)) == 0, 1);
        if (pg->chip_count == 0) {
            check_ll("旧式页 chips[0] 无名", pg->chips[0].name == NULL, 1);
            check_ll("旧式页生效 chip 数", named, 1);
            check_ll("旧式页 when", pg->chips[0].when, PARAM_CHIP_ALWAYS);
        } else {
            check_ll("生效 chip 数 = chip_count", named, pg->chip_count);
        }
        for (uint8_t c = 0; c < named; c++) {
            const param_chip_t *ch = &pg->chips[c];
            check_ll("slot_count 1..8", ch->slot_count >= 1 && ch->slot_count <= PARAM_PAGE_SLOTS, 1);
            check_ll("when 合法", ch->when <= PARAM_CHIP_WHEN_ANALOG, 1);
            if (pg->chip_count > 0) {
                check_ll("chip 名非空", ch->name != NULL && ch->name[0] != '\0', 1);
                for (uint8_t d = 0; d < c; d++) {
                    check_ll("chip 名页内唯一",
                             ch->name != NULL && pg->chips[d].name != NULL &&
                                 strcmp(ch->name, pg->chips[d].name) != 0,
                             1);
                }
                check_ll("chip 宽度公式（16pt 不溢出）",
                         page_layout_chip_width(ch->name) <= 128, 1);
            }
            for (uint8_t k = 0; k < ch->slot_count; k++) {
                const param_slot_t *sl = &ch->slots[k];
                if (sl->id == 0xFF) continue; /* 旧式空槽（SAMPLE 页占位） */
                check_ll("id 合法", sl->id < SLOOP_CORE_P_COUNT, 1);
                switch (sl->scope) {
                case SLOOP_DESC_TRACK:
                    /* 轨道参数 id < P_E0；例外只有 SOUND 页的 8 个引擎槽 P_E0..P_E7
                     * （经 sloop_core_desc_track_live 取选中轨引擎的描述符） */
                    check_ll("TRACK id < P_E0（或 SOUND 页的引擎槽）",
                             sl->id < tP_E0 ||
                                 ((pg->features & PARAM_FEAT_SOUND) != 0 && sl->id <= tP_E7),
                             1);
                    if (sl->id >= tP_E0) {
                        check_ll("引擎槽只在 SOUND 页", pg->page_id, APP_PAGE_ANALOG);
                        check_ll("引擎槽鼓轨锁定（旋钮区锁，kit 走页眉 KIT 行）", sl->drum_ok, 0);
                    }
                    break;
                case SLOOP_DESC_GLOBAL:
                    check_ll("GLOBAL id < G_COUNT", sl->id < SLOOP_CORE_G_COUNT, 1);
                    check_ll("GLOBAL 鼓轨可编辑", sl->drum_ok, 1);
                    break;
                case SLOOP_DESC_ENGINE:
                    check_ll("ENGINE id < 8", sl->id < 8, 1);
                    check_ll("ENGINE 引擎号 < NENGINES", sl->engine < SLOOP_CORE_NENGINES, 1);
                    break;
                default:
                    check_ll("scope 合法", 0, 1);
                    break;
                }
                check_ll("drum_ok ∈ {0,1}", sl->drum_ok <= 1, 1);
                /* 槽位访问器：slot_count 之内非空槽可取、之外/空槽 NULL */
                check_ll("slot 访问器", page_layout_slot(ch, k) == sl ? 1 : 0,
                         sl->id == 0xFF ? 0 : 1);
            }
            check_ll("slot_count 之外 NULL", page_layout_slot(ch, ch->slot_count) == NULL, 1);
            check_ll("越界槽 NULL", page_layout_slot(ch, PARAM_PAGE_SLOTS) == NULL, 1);
        }
    }
}

/* 布局表内容 = 012 §3 权威槽表（MOD/SCALE/VOICE 逐 chip；FX 页的鼓轨逐槽位）。 */
static void case_layout_content(void)
{
    const param_page_t *mod = page_layout_get(page_layout_index(APP_PAGE_MOD));
    const param_page_t *scale = page_layout_get(page_layout_index(APP_PAGE_SCALE));
    const param_page_t *voice = page_layout_get(page_layout_index(APP_PAGE_VOICE));
    const param_page_t *fx = page_layout_get(page_layout_index(APP_PAGE_FX));
    CHECK(mod != NULL && scale != NULL && voice != NULL && fx != NULL);
    if (!mod || !scale || !voice || !fx) return;
    layout_check_chips(mod, k_exp_mod, 5);
    layout_check_chips(scale, k_exp_scale, 4);
    layout_check_chips(voice, k_exp_voice, 4);
    /* FX 页 = 4 个 chip（FX 沿用原 8 槽；鼓轨上 FX chip 内 TFLT/LEVEL/PAN 可编辑、其余
     * 锁定；SLICER/INSERT/INSERT 2 全部可编辑）；GLOBAL 页 = GLO/REV/DRUMS/TEXTURE 四个
     * chip，全局参数鼓轨恒可编辑（012 §3） */
    layout_check_chips(fx, k_exp_fx, 4);
    const param_page_t *g = page_layout_get(page_layout_index(APP_PAGE_GLOBAL));
    CHECK(g != NULL);
    if (g != NULL) layout_check_chips(g, k_exp_global, 4);
    check_ll("GLO id[0..7] = 原 GLOBAL 页 8 槽", g && g->chips[0].slots[0].id == tG_BPM &&
               g->chips[0].slots[7].id == tG_DMIX && g->chips[0].slot_count == 8, 1);
}

/* SOUND 页（P4-F Task 5）：主体 = 选中轨引擎的 8 槽（TRACK scope 的 P_E0..P_E7，经
 * sloop_core_desc_track_live 取引擎钩子）、页眉 ENGINE/PRESET（鼓轨 KIT）、底部 bank 行
 * 只在 FM6 引擎时出现（A10）。 */
static void case_layout_sound(void)
{
    const param_page_t *snd = page_layout_get(page_layout_index(APP_PAGE_ANALOG));
    const param_page_t *fx = page_layout_get(page_layout_index(APP_PAGE_FX));
    const param_page_t *smp = page_layout_get(page_layout_index(APP_PAGE_SAMPLE));
    CHECK(snd != NULL && fx != NULL && smp != NULL);
    if (!snd || !fx || !smp) return;
    check_ll("SOUND 标题", strcmp(snd->title, "SOUND") == 0, 1);
    check_ll("SOUND 是整页（无 chip 行）", snd->chip_count, 0);
    check_ll("SOUND features", snd->features, PARAM_FEAT_SOUND | PARAM_FEAT_BANK_FM6);
    check_ll("page_layout_is_sound", page_layout_is_sound(snd), 1);
    check_ll("is_sound(FX)", page_layout_is_sound(fx), 0);
    check_ll("is_sound(NULL)", page_layout_is_sound(NULL), 0);
    /* 8 槽 = P_E0..P_E7、TRACK scope、鼓轨锁定（旋钮区锁） */
    for (uint8_t k = 0; k < PARAM_PAGE_SLOTS; k++) {
        const param_slot_t *sl = page_layout_slot(&snd->chips[0], k);
        CHECK(sl != NULL);
        if (!sl) continue;
        check_ll("SOUND slot id", sl->id, tP_E0 + k);
        check_ll("SOUND slot scope", sl->scope, SLOOP_DESC_TRACK);
        check_ll("SOUND slot drum_ok", sl->drum_ok, 0);
        check_ll("SOUND slot 鼓轨锁", page_layout_slot_locked(sl, true), 1);
        check_ll("SOUND slot 合成轨不锁", page_layout_slot_locked(sl, false), 0);
    }
    check_ll("SOUND 槽数", snd->chips[0].slot_count, PARAM_PAGE_SLOTS);
    /* 只有 SOUND 页带 bank 行；SAMPLE 页保留槽管理行；其余页无附加行 */
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        const param_page_t *pg = page_layout_get(i);
        check_ll("bank 行只在 SOUND 页", (pg->features & PARAM_FEAT_BANK_FM6) != 0, pg == snd);
        check_ll("SOUND 页眉只在 SOUND 页", (pg->features & PARAM_FEAT_SOUND) != 0, pg == snd);
        check_ll("槽管理行只在 SAMPLE 页", (pg->features & PARAM_FEAT_SAMPLE) != 0, pg == smp);
    }
    /* bank 行可见性（A10）：仅合成轨 + 引擎 FM6（上游 ENGINES[9]）；鼓轨与其余 11 个引擎隐藏 */
    for (uint8_t e = 0; e < SLOOP_CORE_NENGINES; e++) {
        check_ll("bank 行 = 仅 FM6 引擎", page_layout_bank_row_visible(snd, false, e), e == 9);
    }
    check_ll("bank 行 FM6 引擎常量", PARAM_ENGINE_FM6, 9);
    check_ll("bank 行：鼓轨隐藏", page_layout_bank_row_visible(snd, true, 9), 0);
    check_ll("bank 行：非 SOUND 页恒隐藏（即使引擎 FM6）", page_layout_bank_row_visible(fx, false, 9), 0);
    check_ll("bank 行：NULL 页隐藏", page_layout_bank_row_visible(NULL, false, 9), 0);
    /* 页眉行数：合成轨 2（ENGINE + PRESET），鼓轨 1（KIT 取代两行） */
    check_ll("合成轨页眉行数", page_layout_sound_rows(false), 2);
    check_ll("鼓轨页眉行数", page_layout_sound_rows(true), 1);
    /* 页眉几何：两行同在页眉带（y=4..36，与 ± 按钮不重叠）、互不相交、右端 < 1096 */
    int16_t prev_x1 = -1;
    for (uint8_t r = 0; r < page_layout_sound_rows(false); r++) {
        int16_t x = page_layout_sound_row_x(r), w = page_layout_sound_row_w(r);
        check_ll("行 x 单调", x > prev_x1, 1);
        check_ll("行宽 > 2×区宽", w > 2 * SOUND_ROW_ZONE, 1);
        check_ll("行右端 < ± 按钮", x + w <= 1096, 1);
        prev_x1 = (int16_t)(x + w);
    }
    check_ll("行 y", SOUND_ROW_Y, 4);
    check_ll("行高", SOUND_ROW_H, 32);
    check_ll("区宽 = DSYN LANE / FM6P BANK 同款", SOUND_ROW_ZONE, 42);
    /* KIT 行复用第 0 行的矩形；上下文行（`SOUND · TRACK n`）在最后一行右侧 */
    check_ll("KIT 行 = 第 0 行", page_layout_sound_row_x(0), 16);
    check_ll("合成轨上下文 x", page_layout_sound_header_x(false), 720);
    check_ll("鼓轨上下文 x", page_layout_sound_header_x(true), 320);
    /* 名字列宽 = 行宽 − 名字列左缘 − 右留白 ≥ 11px × 上游最长名字（引擎 7 / 预置 10 /
     * kit 8 字符；scripts/synth-test.sh 钉住上游名字长度）——按保守上界不溢出 */
    check_ll("ENGINE 名字列宽", page_layout_sound_row_w(0) - page_layout_sound_name_x(0, false) -
                                    SOUND_ROW_NAME_R >= 11 * 7, 1);
    check_ll("PRESET 名字列宽", page_layout_sound_row_w(1) - page_layout_sound_name_x(1, false) -
                                    SOUND_ROW_NAME_R >= 11 * 10, 1);
    check_ll("KIT 名字列宽", page_layout_sound_row_w(0) - page_layout_sound_name_x(0, true) -
                                 SOUND_ROW_NAME_R >= 11 * 8, 1);
    /* 名字列左缘 = 区宽 42 + 留白 12 + 行名宽（11px/字符）+ 间隙 8，独立算出期望值：
     * ENGINE/PRESET（6 字符）= 128，KIT（3 字符）= 95 */
    check_ll("名字列左缘（ENGINE）", page_layout_sound_name_x(0, false), 42 + 12 + 11 * 6 + 8);
    check_ll("名字列左缘（PRESET）", page_layout_sound_name_x(1, false), 42 + 12 + 11 * 6 + 8);
    check_ll("名字列左缘（KIT）", page_layout_sound_name_x(0, true), 42 + 12 + 11 * 3 + 8);
    check_ll("上下文行宽 > 0", UI_W - 16 - page_layout_sound_header_x(false) > 0, 1);
    check_ll("上下文行不压 ± 按钮", page_layout_sound_header_x(false) < 1096, 1);
}

/* 纯规则：条件 chip 可见性与鼓轨逐槽锁定。 */
static void case_layout_rules(void)
{
    const param_page_t *voice = page_layout_get(page_layout_index(APP_PAGE_VOICE));
    CHECK(voice != NULL);
    if (!voice) return;
    const param_chip_t *filter = &voice->chips[3];
    check_ll("ALWAYS/合成轨", page_layout_chip_visible(&voice->chips[0], false, 1), 1);
    check_ll("ALWAYS/鼓轨", page_layout_chip_visible(&voice->chips[0], true, 0), 1);
    check_ll("FILTER/ANALOG", page_layout_chip_visible(filter, false, 0), 1);
    check_ll("FILTER/DIGITAL", page_layout_chip_visible(filter, false, 1), 0);
    check_ll("FILTER/FM6", page_layout_chip_visible(filter, false, 9), 0);
    check_ll("FILTER/鼓轨", page_layout_chip_visible(filter, true, 0), 0);
    /* VOICE 页：ANALOG 合成轨 4 chip、其余 3 chip（FILTER 隐藏后下标不塌陷） */
    check_ll("ANALOG 可见 chip 数", page_layout_visible_chips(voice, false, 0), 4);
    check_ll("非 ANALOG 可见 chip 数", page_layout_visible_chips(voice, false, 1), 3);
    check_ll("鼓轨可见 chip 数", page_layout_visible_chips(voice, true, 0), 3);
    check_ll("可见 chip[3] = FILTER", page_layout_visible_chip_at(voice, 3, false, 0), 3);
    check_ll("非 ANALOG 的可见 chip[3] 不存在", page_layout_visible_chip_at(voice, 3, false, 1), -1);
    check_ll("可见 chip[0..2] 下标不变", page_layout_visible_chip_at(voice, 2, false, 1), 2);
    check_ll("越界可见序号", page_layout_visible_chip_at(voice, 4, false, 0), -1);
    /* 无条件页（MOD）：可见数 = chip_count，可见序号 = 下标 */
    const param_page_t *mod = page_layout_get(page_layout_index(APP_PAGE_MOD));
    CHECK(mod != NULL);
    if (mod) {
        check_ll("MOD 可见 chip 数（鼓轨）", page_layout_visible_chips(mod, true, 0), 5);
        check_ll("MOD 可见 chip[4]", page_layout_visible_chip_at(mod, 4, true, 0), 4);
    }
    /* 旧式整页（无 chip 行）：可见数 1、序号 0 = chips[0] */
    const param_page_t *smp = page_layout_get(page_layout_index(APP_PAGE_SAMPLE));
    CHECK(smp != NULL);
    if (smp) {
        check_ll("SAMPLE 可见 chip 数", page_layout_visible_chips(smp, true, 0), 1);
        check_ll("SAMPLE 可见 chip[0]", page_layout_visible_chip_at(smp, 0, true, 0), 0);
    }
    /* GLOBAL / FX chip 页：chip 全部无条件出现（鼓轨上也一样），可见数 = chip 数 */
    const param_page_t *glo = page_layout_get(page_layout_index(APP_PAGE_GLOBAL));
    const param_page_t *fx = page_layout_get(page_layout_index(APP_PAGE_FX));
    CHECK(glo != NULL && fx != NULL);
    if (glo && fx) {
        check_ll("GLOBAL chip 数", glo->chip_count, 4);
        check_ll("FX chip 数", fx->chip_count, 4);
        check_ll("GLOBAL 鼓轨可见 chip 数", page_layout_visible_chips(glo, true, 0), 4);
        check_ll("FX 鼓轨可见 chip 数", page_layout_visible_chips(fx, true, 9), 4);
        uint8_t unlocked = 0, total = 0;
        for (uint8_t c = 0; c < glo->chip_count; c++) {
            for (uint8_t k = 0; k < PARAM_PAGE_SLOTS; k++) {
                const param_slot_t *sl = page_layout_slot(&glo->chips[c], k);
                if (!sl) continue;
                total++;
                unlocked += !page_layout_slot_locked(sl, true);
            }
        }
        check_ll("GLOBAL 槽总数（8+8+4+7）", total, 27);
        check_ll("GLOBAL 鼓轨逐槽全不锁（含 DRUMS chip）", unlocked, total);
    }
    /* 逐槽锁定：全局恒不锁；drum_ok=1 恒不锁；其余仅鼓轨锁 */
    param_slot_t sl = {SLOOP_DESC_TRACK, 0, tP_ATK, 0};
    check_ll("合成轨不锁", page_layout_slot_locked(&sl, false), 0);
    check_ll("鼓轨锁", page_layout_slot_locked(&sl, true), 1);
    sl.drum_ok = 1;
    check_ll("鼓轨可编辑槽不锁", page_layout_slot_locked(&sl, true), 0);
    sl.scope = SLOOP_DESC_GLOBAL;
    sl.drum_ok = 0;
    check_ll("全局不锁（即使 drum_ok=0）", page_layout_slot_locked(&sl, true), 0);
}

/* 九组导航：组表顺序/成员/上限、TAB 扁平轮转 18 页（P4-F Task 5：SOUND 取代 ANALOG+FM6；
 * Task 6：TOOLS 进 MIX 成为第 3 张子签）、组签 104px 几何。 */
#define X_TEST_GROUP(NAME, ...) \
    {.n = (uint8_t)(sizeof((uint8_t[]){__VA_ARGS__}) / sizeof(uint8_t)), \
     .pages = {__VA_ARGS__}, .name = #NAME},
static const struct {
    const char *name;
    uint8_t n;
    uint8_t pages[UI_GROUP_MAX];
} k_exp_groups[] = {UI_GROUP_LIST(X_TEST_GROUP)};

static void case_nav_groups(void)
{
    check_ll("组数", sizeof(k_exp_groups) / sizeof(k_exp_groups[0]), 9);
    check_ll("UI_GROUP_COUNT（由组表数出）", UI_GROUP_COUNT, 9);
    static const char *const names[9] = {"PLAY", "TRACKS", "EDIT", "MIX",
                                         "SYNTH", "SEQ",   "SONG", "FILE", "SYS"};
    static const uint8_t counts[9] = {1, 1, 4, 3, 3, 2, 1, 2, 1};
    static const uint8_t flat[] = {
        APP_PAGE_PLAY, APP_PAGE_TRACKS,
        APP_PAGE_ANALOG, APP_PAGE_MOD, APP_PAGE_SCALE, APP_PAGE_VOICE,
        APP_PAGE_GLOBAL, APP_PAGE_FX, APP_PAGE_TOOLS,
        APP_PAGE_SAMPLE, APP_PAGE_DSYN, APP_PAGE_FM6P,
        APP_PAGE_STEP, APP_PAGE_TRANSPORT,
        APP_PAGE_SONG, APP_PAGE_PROJECTS, APP_PAGE_PRESETS, APP_PAGE_SYSTEM,
    };
    uint8_t seen[APP_STATE_PAGES] = {0};
    uint8_t fi = 0;
    for (uint8_t g = 0; g < 9; g++) {
        check_ll("组名", strcmp(k_exp_groups[g].name, names[g]) == 0, 1);
        check_ll("组成员数", k_exp_groups[g].n, counts[g]);
        check_ll("组成员 ≤ UI_GROUP_MAX", k_exp_groups[g].n <= UI_GROUP_MAX, 1);
        for (uint8_t i = 0; i < k_exp_groups[g].n; i++) {
            uint8_t pid = k_exp_groups[g].pages[i];
            check_ll("扁平顺序", pid, flat[fi]);
            check_ll("页 id 合法", pid < APP_STATE_PAGES, 1);
            if (fi < sizeof(flat) / sizeof(flat[0])) fi++;
            if (pid < APP_STATE_PAGES) seen[pid]++;
        }
    }
    check_ll("扁平页数 = 18", fi, 18);
    for (uint8_t p = 0; p < APP_STATE_PAGES; p++) {
        /* APP_STATE_PAGES 是 19：APP_PAGE_FM6（7）是空位 id，不在任何组里 */
        check_ll("每页恰在一个组里（页签/TAB 可达），FM6 空位除外", seen[p],
                 p == APP_PAGE_FM6 ? 0 : 1);
    }
    check_ll("EDIT 首子页 = SOUND", k_exp_groups[2].pages[0], APP_PAGE_ANALOG);
    check_ll("MIX 末子页 = TOOLS（排在 GLOBAL/FX 之后，既有子签下标不变）",
             k_exp_groups[3].pages[2], APP_PAGE_TOOLS);
    check_ll("TOOLS 页 id", APP_PAGE_TOOLS, 18);
    check_ll("组表里没有 FM6 页 id", seen[APP_PAGE_FM6], 0);
    check_ll("UI_GROUP_W", UI_GROUP_W, 104);
    check_ll("UI_SUB_W", UI_SUB_W, 176);
    check_ll("UI_GROUP_MAX", UI_GROUP_MAX, 5);
    /* 组行右端 ≤ 1,280 − 216（SLOOP 标题/层提示的 reserve 线；theme.h 有静态断言） */
    check_ll("组签行右端", UI_TAB_X0 + 9 * (UI_GROUP_W + UI_TAB_GAP) - UI_TAB_GAP, 1016);
    check_ll("组签行不越 reserve", UI_TAB_X0 + 9 * (UI_GROUP_W + UI_TAB_GAP) - UI_TAB_GAP <=
                                       UI_W - UI_TAB_RIGHT_RESERVE, 1);
    check_ll("子签行不越 reserve", UI_TAB_X0 + UI_GROUP_MAX * (UI_SUB_W + UI_TAB_GAP) - UI_TAB_GAP <=
                                       UI_W - UI_TAB_RIGHT_RESERVE, 1);
    /* 最长组签 TRACKS：16pt Montserrat 大写 ≈ 11px/字符（保守上界）须放进签宽 */
    check_ll("TRACKS 标签宽 ≤ 组签宽", 11 * 6 <= UI_GROUP_W, 1);
}

/* EDIT 层步工具键（P4-F Task 6，ADR-006）：k = 参数锁、l = 解锁、9 / ; = fill 条件循环。
 * 键表是 input_map.h 的纯数据（input.c 与本测试同源），这里证明：
 *  - 同一 (键, 层) 在表里至多一条（find_binding 取首条，重复条目会被静默遮蔽）；
 *  - EDIT 层 = a–g 的 7 条音名擦除 + 4 条步工具，不多不少；
 *  - 四个步工具键码在**其它任何层**（基础演奏/GLO/SEL/FX）都没有绑定——它们在该层是
 *    空闲键，不会抢演奏键，也不会被层内别的功能抢先。 */
static void case_step_tool_keys(void)
{
    const struct {
        uint8_t code;
        uint8_t action;
        const char *name;
    } exp[] = {
        {0x39, ACT_STEP_LOCK, "k"},   {0x3A, ACT_STEP_UNLOCK, "l"},
        {0x09, ACT_STEP_FILL, "9"},   {0x2B, ACT_STEP_FILL, ";"},
    };
    for (size_t i = 0; i < INPUT_MAP_BINDING_COUNT; i++) {
        for (size_t j = i + 1u; j < INPUT_MAP_BINDING_COUNT; j++) {
            CHECK(!(k_bindings[i].code == k_bindings[j].code &&
                    k_bindings[i].layer == k_bindings[j].layer));
        }
    }
    uint32_t edit_n = 0;
    static const uint8_t erase_codes[7] = {0x32, 0x46, 0x44, 0x34, 0x23, 0x35, 0x36};
    for (size_t i = 0; i < INPUT_MAP_BINDING_COUNT; i++) {
        if (k_bindings[i].layer != APP_LAYER_EDIT) continue;
        edit_n++;
        const key_binding_t *b = &k_bindings[i];
        bool tool = false, erase = false;
        for (size_t k = 0; k < sizeof(exp) / sizeof(exp[0]); k++) tool |= b->code == exp[k].code;
        for (size_t k = 0; k < 7u; k++) erase |= b->code == erase_codes[k];
        CHECK(tool || erase);
        if (erase) CHECK(b->action == ACT_ERASE);
    }
    check_ll("EDIT 层条目数 = 7 音名擦除 + 4 步工具", edit_n, 11);
    for (size_t k = 0; k < sizeof(exp) / sizeof(exp[0]); k++) {
        uint32_t n = 0;
        for (size_t i = 0; i < INPUT_MAP_BINDING_COUNT; i++) n += k_bindings[i].code == exp[k].code;
        CHECK(n == 1);
        const key_binding_t *b = input_map_find_binding(exp[k].code, APP_LAYER_EDIT);
        CHECK(b != NULL && b->action == exp[k].action && b->layer == APP_LAYER_EDIT);
        CHECK(input_map_find_binding(exp[k].code, 0) == NULL);                 /* 基础层空闲 */
        CHECK(input_map_find_binding(exp[k].code, APP_LAYER_GLO | APP_LAYER_SEL | APP_LAYER_FX) == NULL);
    }
    /* 键名与码的对照（tab5_keyboard.c 的 k_key_names：row<<4|col） */
    CHECK(K_K == 0x39 && K_L == 0x3A && K_9 == 0x09 &&
          K_SEMI == 0x2B);
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
    run("参数页布局表：页数/页 id/est 与 EDIT/MIX 有序对账面", case_layout_pages);
    run("参数页布局表：chip 完整性（槽数/id/唯一名/枚举）", case_layout_integrity);
    run("参数页布局表：MOD/SCALE/VOICE/FX/GLOBAL 槽位 = 012 §3", case_layout_content);
    run("SOUND 页：8 引擎槽/页眉行/bank 行仅 FM6 可见（A10）", case_layout_sound);
    run("条件 chip 可见性与鼓轨逐槽锁定规则", case_layout_rules);
    run("九组导航：组表顺序/成员/18 页扁平轮转/104px 组签", case_nav_groups);
    run("EDIT 层步工具键：k/l/9/; 唯一且不占其它层", case_step_tool_keys);

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
