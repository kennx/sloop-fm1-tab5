/* Sound 仪表盘纯数据/纯函数 host 单测（scripts/sound-test.sh）：sound_layout.c + sloop_core.h。
 *
 * 覆盖：
 *  - 卡片成员表：13 张卡、各卡槽数、成员总数 109、标题、id 按作用域合法、逐槽 = 期望表；
 *  - 扁平下标：卡片 × 槽 → 扁平下标及其反查、越界；
 *  - staggered 布局：行数/卡高/落列/折叠/同列卡片不重叠；
 *  - 槽矩形、存在位图上的焦点移动（跳过不存在的槽与折叠卡片、环绕）；
 *  - 环带判定（30..52px）。
 *
 * 期望值来自 5B 规格 §5.2 的卡片成员表（顺序与分组照 Sloop Go ParamsScreen.kt 的
 * layout(pe0)）与 012 §3 的鼓轨逐槽规则；像素期望按卡高公式手算，不从被测函数反推。
 * 参数 id 用上游 core.h:48-96 的枚举顺序在测试里独立复刻（tP_* / tG_*）——成员表自己写的
 * 是 param_ids.h 的本地复刻（L_P_* / L_G_*），抄错即 FAIL；另有几处字面量数字钉，不经任何
 * 枚举。 */
#include <stdio.h>
#include <string.h>

#include "sound_layout.h"

static int g_failures;
static int g_passed;
static int g_case_failed;

#define ASSERT(expr)                                                       \
    do {                                                                   \
        if (!(expr)) {                                                     \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #expr);       \
            g_case_failed = 1;                                             \
        }                                                                  \
    } while (0)

static void run(const char *name, void (*fn)(void))
{
    g_case_failed = 0;
    fn();
    if (g_case_failed) {
        printf("FAIL %s\n", name);
        g_failures++;
    } else {
        printf("PASS %s\n", name);
        g_passed++;
    }
}

/* ---------------------------------------------------------------- 期望成员表
 * 上游 core.h:48-75 的每轨参数枚举顺序（独立复刻，靠顺序隐式给值；= page_layout.c 的
 * PARAM_PAGES 同一来源）。 */
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
/* 上游 core.h:76-108 的全局参数枚举顺序（独立复刻）；最高位 = GLOBAL 作用域
 * （轨道 id < 71 < 0x80，两套 id 装进同一个字节）。 */
enum {
    tG_BPM = 0x80, tG_SWING, tG_CLOCK, tG_TUNE, tG_DTIME, tG_DFDBK, tG_DCOLOR, tG_DMIX,
    tG_RSIZE, tG_RDAMP, tG_CRATE, tG_CDEPTH,
    tG_MIDI, tG_SYNC, tG_ROUTE, tG_INFO, tG_SLOT, tG_NAME, tG_LOAD, tG_SAVE,
    tG_ENGSEL, tG_ENGGO, tG_CLRSEQ, tG_INITSND, tG_DRCH, tG_DRLVL, tG_DRREV,
    tG_DUST, tG_DUCK, tG_FILT, tG_ROLL, tG_NEWPRJ, tG_DRDLY,
    tG_CH1, tG_CH2, tG_CH3, tG_CHOTH, tG_TXSRC, tG_TXLVL, tG_TXTONE, tG_TXMOVE,
    tG_TXRATE, tG_TXWHEN, tG_TXDUCK, tG_BEND, tG_FXCH, tG_DRMAP, tG_RMCH, tG_CLKOUT,
    tG_FXFADE,
};

/* 期望卡片：{标题, 槽数, 槽 id…, 位 k = 槽 k 鼓轨可编辑, 位 k = 槽 k 仅 ANALOG} */
typedef struct {
    const char *title;
    uint8_t n;
    uint8_t ids[22]; /* 最大卡 = FX */
    uint32_t drum_ok;
    uint32_t gate;
} exp_card_t;

#define CARD(nm, drum, gate_, ...)                                                \
    { nm, (uint8_t)(sizeof((uint8_t[]){__VA_ARGS__}) / sizeof(uint8_t)),          \
      {__VA_ARGS__}, (drum), (gate_) }

static const exp_card_t k_exp[SOUND_CARDS_MAX] = {
    CARD("Global", 0xF, 0, tG_BPM, tG_SWING, tG_CLOCK, tG_TUNE),
    CARD("Master", 0x1F, 0, tG_DUST, tG_DUCK, tG_FILT, tG_ROLL, tP_TFLT),
    CARD("Envelope", 0, 0, tP_ATK, tP_DEC, tP_SUS, tP_REL, tP_ED_FLT, tP_ED_PIT, tP_ED_SHP,
         tP_ED_FX, tP_ESYNC),
    CARD("LFO", 0, 0, tP_LRATE, tP_LWAVE, tP_LPHASE, tP_LFADE, tP_LSYNC, tP_LTRIG, tP_LPOL,
         tP_LD_PIT, tP_LD_FLT, tP_LD_SHP, tP_LD_AMP),
    CARD("Engine edit", 0, 1u << 8, tP_E0, tP_E1, tP_E2, tP_E3, tP_E4, tP_E5, tP_E6, tP_E7,
         tP_FTYPE),
    CARD("Voice", 0x81, 0, tP_LEVEL, tP_VOICE, tP_GLIDE, tP_GLMODE, tP_PRIO, tP_ALLOC,
         tP_DETUNE, tP_PAN, tP_MUTE),
    CARD("FX", 0x3FFFF0, 0, tP_DIST, tP_CHOR, tP_DLY, tP_REV, tP_SLCR, tP_SLPAT, tP_SLRATE,
         tP_SLDEPTH, tG_FXFADE, tG_DTIME, tG_DFDBK, tG_DCOLOR, tG_DMIX, tG_RSIZE, tG_RDAMP,
         tG_CRATE, tG_CDEPTH, tP_ITYPE, tP_IA, tP_IB, tP_IC, tP_IMIX),
    CARD("Scale", 0, 0, tP_ROOT, tP_SCALE, tP_QUANT, tP_CHORD, tP_TRANS, tP_STRUM, tP_VLEAD),
    CARD("Arp", 0, 0, tP_AMODE, tP_ARATE, tP_AOCT, tP_AGATE, tP_ASWING, tP_APROB, tP_AHOLD,
         tP_AORDER),
    CARD("Pattern", 0xF, 0, tP_SLEN, tP_SDIV, tP_SSWING, tP_SGATE),
    CARD("Drums", 0xF, 0, tG_DRCH, tG_DRLVL, tG_DRREV, tG_DRDLY),
    CARD("MIDI", 0x3FF, 0, tG_CH1, tG_CH2, tG_CH3, tG_DRCH, tG_CHOTH, tG_BEND, tG_FXCH,
         tG_DRMAP, tG_RMCH, tG_CLKOUT),
    CARD("Texture", 0x7F, 0, tG_TXSRC, tG_TXLVL, tG_TXTONE, tG_TXMOVE, tG_TXRATE, tG_TXWHEN,
         tG_TXDUCK),
};

static void test_cards(void) {            /* 成员 = 5B 规格 §5.2 表（012 §3 的槽位映射） */
    ASSERT(SOUND_CARDS_MAX == 13);
    static const uint8_t want[SOUND_CARDS_MAX] = {4, 5, 9, 11, 9, 9, 22, 7, 8, 4, 4, 10, 7};
    uint16_t total = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        ASSERT(k_sound_cards[c].slot_count == want[c]);
        total = (uint16_t)(total + k_sound_cards[c].slot_count);
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            const sound_slot_t *x = &k_sound_cards[c].slots[s];
            if (x->scope == SLOOP_DESC_GLOBAL) ASSERT(x->id < SLOOP_CORE_G_COUNT);
            else ASSERT(x->id < SLOOP_CORE_P_COUNT);
        }
    }
    ASSERT(total == SOUND_SLOT_TABLE_MAX);          /* 109 */
    ASSERT(strcmp(k_sound_cards[0].title, "Global") == 0);
    ASSERT(strcmp(k_sound_cards[12].title, "Texture") == 0);
    /* 抽钉：Global 卡 = G_BPM G_SWING G_CLOCK G_TUNE；Master 卡第 4 槽 = G_ROLL、第 5 槽 = TFLT(TRACK)；
       Engine edit 卡第 9 槽 = P_FTYPE 且 gate=1；Drums 卡首槽与 MIDI 卡第 4 槽同为 G_DRCH */
    ASSERT(k_sound_cards[0].slots[0].scope == SLOOP_DESC_GLOBAL);
    ASSERT(k_sound_cards[1].slots[3].scope == SLOOP_DESC_GLOBAL && k_sound_cards[1].slots[4].scope == SLOOP_DESC_TRACK);
    ASSERT(k_sound_cards[4].slots[8].gate == 1);
    ASSERT(k_sound_cards[10].slots[0].id == k_sound_cards[11].slots[3].id);
    /* 鼓轨规则：仅 PATTERN(3..6)/SLICER/INSERT/TFLT/LEVEL/PAN 与 GLOBAL 槽 drum_ok */
    uint8_t ok = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++)
        for (uint8_t s = 0; s < k_sound_cards[c].slot_count; s++) {
            const sound_slot_t *x = &k_sound_cards[c].slots[s];
            if (x->scope != SLOOP_DESC_GLOBAL && x->drum_ok) ok++;
        }
    ASSERT(ok == 16);     /* 012 §3「鼓」列：PATTERN 4 + SLICER 4 + INSERT 4 + INSERT 2 1 + TFLT/LEVEL/PAN 3 */

    /* 字面量钉（不经任何枚举；上游 core.h 的序号）：本地复刻枚举 param_ids.h 抄错即 FAIL */
    ASSERT(k_sound_cards[1].slots[3].id == 30);    /* G_ROLL */
    ASSERT(k_sound_cards[1].slots[4].id == 50);    /* P_TFLT */
    ASSERT(k_sound_cards[4].slots[8].id == 58);    /* P_FTYPE */
    ASSERT(k_sound_cards[6].slots[8].id == 49);    /* G_FXFADE = FX 卡的 FX FADE 槽 */
    ASSERT(k_sound_cards[10].slots[0].id == 24);   /* G_DRCH */
    ASSERT(k_sound_cards[11].slots[3].id == 24);
    /* Envelope 卡 9 槽（Sloop Go 的 ENV DEST = 5,6,7,8 + ENV SYNC）：第 8 槽是 TRIM（P_ED_FX），
       不是旧 MOD·ENV DEST chip 的 ESYNC */
    ASSERT(k_sound_cards[2].slots[7].id == 8);
    ASSERT(k_sound_cards[2].slots[8].id == 59);    /* P_ESYNC */

    /* 逐槽对照期望表：作用域、id、drum_ok、gate；引擎门槛只有 P_FTYPE 一槽 */
    uint8_t gated = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        const sound_card_t *card = &k_sound_cards[c];
        ASSERT(strcmp(card->title, k_exp[c].title) == 0);
        ASSERT(card->slot_count == k_exp[c].n);
        ASSERT(card->slots != NULL);
        for (uint8_t s = 0; s < card->slot_count && s < k_exp[c].n; s++) {
            const sound_slot_t *x = &card->slots[s];
            uint8_t scope = (uint8_t)((k_exp[c].ids[s] & 0x80u) ? SLOOP_DESC_GLOBAL : SLOOP_DESC_TRACK);
            uint8_t id = (uint8_t)(k_exp[c].ids[s] & 0x7Fu);
            if (x->scope != scope || x->id != id) {
                printf("  FAIL %s 槽 %u: scope %u id %u, 期望 scope %u id %u\n", card->title, s,
                       x->scope, x->id, scope, id);
                g_case_failed = 1;
            }
            ASSERT(x->drum_ok == ((k_exp[c].drum_ok >> s) & 1u));
            ASSERT(x->gate == ((k_exp[c].gate >> s) & 1u));
            gated += x->gate;
        }
    }
    ASSERT(gated == 1);
}

static void test_index(void) {
    uint8_t c, s;
    ASSERT(sound_layout_slot_index(0, 0) == 0);
    ASSERT(sound_layout_slot_index(0, 4) == SOUND_SLOT_NONE);
    ASSERT(sound_layout_slot_index(1, 0) == 4 && sound_layout_slot_index(4, 8) == 4 + 5 + 9 + 11 + 8);
    ASSERT(sound_layout_slot_locate(108, &c, &s) && c == 12 && s == 6);
    ASSERT(!sound_layout_slot_locate(109, &c, &s));
    /* 逐槽往返 + 表外一律拒绝 */
    for (uint8_t ci = 0; ci < SOUND_CARDS_MAX; ci++)
        for (uint8_t si = 0; si < k_sound_cards[ci].slot_count; si++) {
            uint16_t flat = sound_layout_slot_index(ci, si);
            ASSERT(flat != SOUND_SLOT_NONE);
            ASSERT(sound_layout_slot_locate(flat, &c, &s) && c == ci && s == si);
        }
    ASSERT(sound_layout_slot_index(SOUND_CARDS_MAX, 0) == SOUND_SLOT_NONE);
    ASSERT(sound_layout_slot_index(9, 4) == SOUND_SLOT_NONE);       /* Pattern 卡只有 4 槽 */
    ASSERT(!sound_layout_slot_locate(SOUND_SLOT_NONE, &c, &s));
}
static void test_place(void) {
    sound_card_pos_t p[SOUND_CARDS_MAX];
    int16_t h = sound_layout_place(0, p);
    /* 行数 = ceil(n/4)：Global 1 行 ⇒ h = 36 + 132 + 12 = 180；FX 6 行 ⇒ 36 + 792 + 12 = 840 */
    ASSERT(p[0].rows == 1 && p[0].h == 180 && p[0].y == 0 && p[0].col == 0);
    ASSERT(p[6].rows == 6 && p[6].h == 840);
    /* staggered：第 2 张（Master 5 槽 = 2 行，h = 36 + 264 + 12 = 312）落在右列；每张卡 x = 16 + col*(616+16) */
    ASSERT(p[1].rows == 2 && p[1].h == 312 && p[1].col == 1 && p[1].y == 0);
    ASSERT(p[1].col != p[0].col);
    /* 折叠第 6 张（FX 840）：高 = SOUND_TITLE_H（规格 §5.2「折叠时 36」），总高变短；
       折叠后它仍是左列第 4 张，y 不变 */
    int16_t h2 = sound_layout_place(1u << 6, p);
    ASSERT(p[6].h == SOUND_TITLE_H && p[6].y == 1092 && h2 < h);
    /* 每张卡不得越过另一列已放卡片：同列卡片 y 严格递增且不重叠 */
    for (uint8_t i = 0; i < SOUND_CARDS_MAX; i++)
        for (uint8_t j = 0; j < SOUND_CARDS_MAX; j++)
            if (i != j && p[i].col == p[j].col && p[j].y > p[i].y) ASSERT(p[j].y >= p[i].y + p[i].h + 8);
    /* 折叠不改变成员与行数（展开时原样恢复）；内容总高 = 较高一列（末卡的 8px 间距不计） */
    ASSERT(p[6].rows == 6);
    for (uint8_t i = 0; i < SOUND_CARDS_MAX; i++) ASSERT(p[i].card == i);
    int16_t bottom[2] = {0, 0};
    for (uint8_t i = 0; i < SOUND_CARDS_MAX; i++) {
        int16_t b = (int16_t)(p[i].y + p[i].h);
        if (b > bottom[p[i].col]) bottom[p[i].col] = b;
    }
    ASSERT(h2 == (bottom[0] > bottom[1] ? bottom[0] : bottom[1]));
    /* 手算整表（未折叠）：Global 左列 y0 → Master 右列 y0 → Envelope 左 y188 → LFO 右 y320
       → Engine edit 左 y640 → Voice 右 y772 → FX 左 y1092；两列底 2448/2504 ⇒ 总高 2496 */
    (void)sound_layout_place(0, p);
    ASSERT(p[2].col == 0 && p[2].y == 188 && p[3].col == 1 && p[3].y == 320);
    ASSERT(p[4].col == 0 && p[4].y == 640 && p[5].col == 1 && p[5].y == 772);
    ASSERT(p[6].col == 0 && p[6].y == 1092 && h == 2496);
    /* 全部折叠：每张卡只剩标题行，左列 7 张、右列 6 张 ⇒ 7×36 + 6×8 = 300；
       第 6 张是左列第 4 张（0,2,4,6）⇒ y = 3×(36+8) */
    ASSERT(sound_layout_place(0x1FFF, p) == 300 && p[6].y == 3 * (SOUND_TITLE_H + 8));
}
static void test_rect_and_move(void) {
    sound_card_pos_t p[SOUND_CARDS_MAX]; int16_t x, y, w, h;
    (void)sound_layout_place(0, p);
    ASSERT(sound_layout_slot_rect(&p[0], 0, &x, &y, &w, &h) && w == SOUND_CELL_W && h == SOUND_CELL_H);
    ASSERT(sound_layout_slot_rect(&p[0], 1, &x, &y, &w, &h) && x == p[0].col * (SOUND_COL_W + SOUND_COL_GAP) + SOUND_CARD_PAD + SOUND_CELL_W);
    ASSERT(!sound_layout_slot_rect(&p[0], 4, &x, &y, &w, &h));
    /* 第二行槽落在第二行；Master 第 5 槽（下标 4）存在、下标 5 不存在（行有 8 格，卡只有 5 槽） */
    ASSERT(sound_layout_slot_rect(&p[1], 4, &x, &y, &w, &h) &&
           x == p[1].col * (SOUND_COL_W + SOUND_COL_GAP) + SOUND_CARD_PAD &&
           y == p[1].y + SOUND_TITLE_H + SOUND_CELL_H);
    ASSERT(!sound_layout_slot_rect(&p[1], 5, &x, &y, &w, &h));
    /* 折叠卡片里的槽没有矩形 */
    (void)sound_layout_place(1u << 1, p);
    ASSERT(!sound_layout_slot_rect(&p[1], 0, &x, &y, &w, &h));
    (void)sound_layout_place(0, p);
    uint32_t present[4] = {0};    /* 全存在 */
    for (uint16_t i = 0; i < SOUND_SLOT_TABLE_MAX; i++) present[i >> 5] |= 1ul << (i & 31);
    uint16_t f;
    ASSERT(sound_layout_flat_move(present, 0, 0, 1, &f) && f == 1);
    ASSERT(sound_layout_flat_move(present, 0, 0, -1, &f) && f == 108);     /* 环绕 */
    present[0] &= ~(1ul << 1);                                              /* 槽 1 不存在 */
    ASSERT(sound_layout_flat_move(present, 0, 0, 1, &f) && f == 2);
    present[0] &= ~1ul; present[0] |= 1ul << 1;                             /* 槽 0 不存在：从 1 起向后环绕到 108 */
    ASSERT(sound_layout_flat_move(present, 0, 1, -1, &f) && f == 108);
    /* 折叠整张 Global 卡 ⇒ 焦点越过它 */
    ASSERT(sound_layout_flat_move(present, 1u << 0, 3, 1, &f) && f == 4);
    ASSERT(sound_layout_present_count(present) == 108);
    uint32_t none[4] = {0}; uint16_t g;
    ASSERT(!sound_layout_flat_move(none, 0, 0, 1, &g) && !sound_layout_first_present(none, &g));
    ASSERT(sound_layout_first_present(present, &g) && g == 1);
    /* 无焦点（from = SOUND_SLOT_NONE）：向后 = 第一个存在槽，向前 = 最后一个存在槽 */
    ASSERT(sound_layout_flat_move(present, 0, (int16_t)SOUND_SLOT_NONE, 1, &f) && f == 1);
    ASSERT(sound_layout_flat_move(present, 0, (int16_t)SOUND_SLOT_NONE, -1, &f) && f == 108);
    /* 折叠卡的槽即使存在也进不去：从 Master 首槽（4）向前 = Global 折叠 ⇒ 环绕到 108；
       从 3 向后进折叠的 Global ⇒ 落到 4 */
    ASSERT(sound_layout_flat_move(present, 1u << 0, 4, -1, &f) && f == 108);
    /* 全部折叠 ⇒ 无处可去；只剩一个存在槽时 ±1 都回到它自己 */
    ASSERT(!sound_layout_flat_move(present, 0x1FFF, 5, 1, &f));
    uint32_t one[4] = {0}; one[3] = 1ul << (108 - 96);
    ASSERT(sound_layout_flat_move(one, 0, 108, 1, &f) && f == 108);
    ASSERT(sound_layout_flat_move(one, 0, 108, -1, &f) && f == 108);
    ASSERT(sound_layout_present_count(one) == 1);
    ASSERT(sound_layout_first_present(one, &g) && g == 108);
    ASSERT(sound_ring_hit(0, 30) && sound_ring_hit(36, 36) && !sound_ring_hit(0, 29) && !sound_ring_hit(0, 53) && !sound_ring_hit(0, 0));
    /* 外边界：52² = 2704，(36,36)² = 2592 在带内、(36,38)² = 2740 在带外；环带四个象限对称 */
    ASSERT(sound_ring_hit(-52, 0) && sound_ring_hit(0, -30) && !sound_ring_hit(-36, -38));
}

int main(void)
{
    run("卡片成员表（13 卡/109 槽/逐槽 = 期望表/鼓轨规则/引擎门槛）", test_cards);
    run("扁平下标（卡片 × 槽 ⇄ 扁平、越界）", test_index);
    run("staggered 两列布局（行数/卡高/落列/折叠/不重叠）", test_place);
    run("槽矩形 / 存在位图上的焦点移动 / 环带", test_rect_and_move);

    printf("\n%d passed, %d failed\n", g_passed, g_failures);
    return g_failures == 0 ? 0 : 1;
}
