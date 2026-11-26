/*
 * Sound 仪表盘的卡片成员表与布局纯函数（sound_layout.h；纯数据 + 纯函数，不含 LVGL——
 * host 单测 scripts/sound-test.sh 同源编译）。
 *
 * 布局（5B 规格 §5.2）：两列 staggered——卡片依次放进「当前累计高度较小」的列（相等放
 * 左列），卡片间距 8；每行 4 个槽，卡片高 = 36 + 行数×132 + 12（折叠时 36）。全部是
 * **内容坐标**（原点 = 列 0 / y 0），页面坐标由调用方加 (16, 视口顶) 再减滚动量。
 *
 * 扁平下标 = 成员表的 running sum（卡片顺序 × 槽顺序）：键盘 ←/→ 在它上面 ±1、存在位图
 * 与焦点都用它做键。G_DRCH 同时是 Drums 卡首槽与 MIDI 卡第 4 槽——两个不同的扁平下标
 * 指向同一个全局参数（Sloop Go 同）。
 */
#include "sound_layout.h"

#include "param_ids.h"

/* 槽：TS = 轨道参数（鼓轨上锁定）/ TD = 轨道参数且鼓轨可编辑 / TA = 仅 ANALOG 引擎出现 /
 * GS = 全局参数（鼓轨上全部可编辑，同 page_layout.c 的 GS）。 */
#define TS(pid) { .scope = SLOOP_DESC_TRACK, .id = (uint8_t)(pid), .gate = 0, .drum_ok = 0 }
#define TD(pid) { .scope = SLOOP_DESC_TRACK, .id = (uint8_t)(pid), .gate = 0, .drum_ok = 1 }
#define TA(pid) { .scope = SLOOP_DESC_TRACK, .id = (uint8_t)(pid), .gate = 1, .drum_ok = 0 }
#define GS(pid) { .scope = SLOOP_DESC_GLOBAL, .id = (uint8_t)(pid), .gate = 0, .drum_ok = 1 }

/* 成员表（一处定义）：X(下标, 标题, 槽…)。与 Sloop Go 的一处有意偏离：Master 卡多一个
 * G_ROLL（上游 MASTER 页有、Tab5 现有 GLOBAL·REV chip 也有，退役 GLOBAL 页后它只在这里）。
 * MIDI 卡照 Sloop Go：MIDI 2 只收 OTHER 与 BEND（上游该页的 IN/OUT 是 FM-1 自身的设置）。
 * Envelope 卡照 Sloop Go：ENV DEST 的第 4 槽是 P_ED_FX（TRIM；FM-1 不给它页面、旧 MOD·ENV
 * DEST chip 的第 4 槽因此是 ESYNC），ESYNC 另占 ENV SYNC 一槽。
 * 宏体内不放注释（每行都要续行）。 */
#define SOUND_CARD_LIST(X) \
    X(0, "Global", GS(L_G_BPM), GS(L_G_SWING), GS(L_G_CLOCK), GS(L_G_TUNE)) \
    X(1, "Master", GS(L_G_DUST), GS(L_G_DUCK), GS(L_G_FILT), GS(L_G_ROLL), TD(L_P_TFLT)) \
    X(2, "Envelope", TS(L_P_ATK), TS(L_P_DEC), TS(L_P_SUS), TS(L_P_REL), \
      TS(L_P_ED_FLT), TS(L_P_ED_PIT), TS(L_P_ED_SHP), TS(L_P_ED_FX), TS(L_P_ESYNC)) \
    X(3, "LFO", TS(L_P_LRATE), TS(L_P_LWAVE), TS(L_P_LPHASE), TS(L_P_LFADE), \
      TS(L_P_LSYNC), TS(L_P_LTRIG), TS(L_P_LPOL), \
      TS(L_P_LD_PIT), TS(L_P_LD_FLT), TS(L_P_LD_SHP), TS(L_P_LD_AMP)) \
    X(4, "Engine edit", TS(L_P_E0), TS(L_P_E0 + 1), TS(L_P_E0 + 2), TS(L_P_E0 + 3), \
      TS(L_P_E0 + 4), TS(L_P_E0 + 5), TS(L_P_E0 + 6), TS(L_P_E0 + 7), TA(L_P_FTYPE)) \
    X(5, "Voice", TD(L_P_LEVEL), TS(L_P_VOICE), TS(L_P_GLIDE), TS(L_P_GLMODE), TS(L_P_PRIO), \
      TS(L_P_ALLOC), TS(L_P_DETUNE), TD(L_P_PAN), TS(L_P_MUTE)) \
    X(6, "FX", TS(L_P_DIST), TS(L_P_CHOR), TS(L_P_DLY), TS(L_P_REV), \
      TD(L_P_SLCR), TD(L_P_SLPAT), TD(L_P_SLRATE), TD(L_P_SLDEPTH), GS(L_G_FXFADE), \
      GS(L_G_DTIME), GS(L_G_DFDBK), GS(L_G_DCOLOR), GS(L_G_DMIX), \
      GS(L_G_RSIZE), GS(L_G_RDAMP), GS(L_G_CRATE), GS(L_G_CDEPTH), \
      TD(L_P_ITYPE), TD(L_P_IA), TD(L_P_IB), TD(L_P_IC), TD(L_P_IMIX)) \
    X(7, "Scale", TS(L_P_ROOT), TS(L_P_SCALE), TS(L_P_QUANT), TS(L_P_CHORD), \
      TS(L_P_TRANS), TS(L_P_STRUM), TS(L_P_VLEAD)) \
    X(8, "Arp", TS(L_P_AMODE), TS(L_P_ARATE), TS(L_P_AOCT), TS(L_P_AGATE), \
      TS(L_P_ASWING), TS(L_P_APROB), TS(L_P_AHOLD), TS(L_P_AORDER)) \
    X(9, "Pattern", TD(L_P_SLEN), TD(L_P_SDIV), TD(L_P_SSWING), TD(L_P_SGATE)) \
    X(10, "Drums", GS(L_G_DRCH), GS(L_G_DRLVL), GS(L_G_DRREV), GS(L_G_DRDLY)) \
    X(11, "MIDI", GS(L_G_CH1), GS(L_G_CH2), GS(L_G_CH3), GS(L_G_DRCH), GS(L_G_CHOTH), \
      GS(L_G_BEND), GS(L_G_FXCH), GS(L_G_DRMAP), GS(L_G_RMCH), GS(L_G_CLKOUT)) \
    X(12, "Texture", GS(L_G_TXSRC), GS(L_G_TXLVL), GS(L_G_TXTONE), GS(L_G_TXMOVE), \
      GS(L_G_TXRATE), GS(L_G_TXWHEN), GS(L_G_TXDUCK))

/* 第一遍：每张卡一个槽数组；第二遍：卡片条目（槽数由数组长度给出，不另抄）。
 * 宏参数不能叫 title：designator 的 .title 会被参数替换掉（同 page_layout.c）。 */
#define SOUND_CARD_SLOTS(n, ttl, ...) \
    static const sound_slot_t k_card_slots_##n[] = { __VA_ARGS__ };
SOUND_CARD_LIST(SOUND_CARD_SLOTS)

#define SOUND_CARD_ENTRY(n, ttl, ...) \
    [n] = { .title = ttl, .slots = k_card_slots_##n,                                     \
            .slot_count = (uint8_t)(sizeof(k_card_slots_##n) / sizeof(sound_slot_t)) },
const sound_card_t k_sound_cards[SOUND_CARDS_MAX] = { SOUND_CARD_LIST(SOUND_CARD_ENTRY) };
#undef SOUND_CARD_ENTRY
#undef SOUND_CARD_SLOTS
#undef SOUND_CARD_LIST
#undef GS
#undef TA
#undef TD
#undef TS

#define SOUND_CARD_GAP      8
#define SOUND_SLOTS_PER_ROW 4

_Static_assert(SOUND_CARDS_MAX <= 16, "collapsed 是 16 位，一位一张卡");

uint16_t sound_layout_slot_index(uint8_t card, uint8_t slot)
{
    uint16_t base = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        if (c == card) {
            return slot < k_sound_cards[c].slot_count ? (uint16_t)(base + slot) : SOUND_SLOT_NONE;
        }
        base = (uint16_t)(base + k_sound_cards[c].slot_count);
    }
    return SOUND_SLOT_NONE;
}

bool sound_layout_slot_locate(uint16_t flat, uint8_t *card, uint8_t *slot)
{
    uint16_t base = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        uint16_t n = k_sound_cards[c].slot_count;
        if (flat < (uint16_t)(base + n)) {
            *card = c;
            *slot = (uint8_t)(flat - base);
            return true;
        }
        base = (uint16_t)(base + n);
    }
    return false;
}

int16_t sound_layout_place(uint16_t collapsed, sound_card_pos_t out[SOUND_CARDS_MAX])
{
    int16_t bottom[2] = {0, 0};
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        sound_card_pos_t *p = &out[c];
        uint8_t rows = (uint8_t)((k_sound_cards[c].slot_count + SOUND_SLOTS_PER_ROW - 1u) /
                                 SOUND_SLOTS_PER_ROW);
        uint8_t col = bottom[1] < bottom[0] ? 1u : 0u;
        p->card = c;
        p->col = (int16_t)col;
        p->y = bottom[col];
        p->rows = rows;
        p->h = (collapsed & (uint16_t)(1u << c)) != 0u
                   ? SOUND_TITLE_H
                   : (int16_t)(SOUND_TITLE_H + rows * SOUND_CELL_H + SOUND_CARD_PAD);
        bottom[col] = (int16_t)(bottom[col] + p->h + SOUND_CARD_GAP);
    }
    return (int16_t)((bottom[0] > bottom[1] ? bottom[0] : bottom[1]) - SOUND_CARD_GAP);
}

bool sound_layout_slot_rect(const sound_card_pos_t *pos, uint8_t slot,
                            int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (pos->card >= SOUND_CARDS_MAX || slot >= k_sound_cards[pos->card].slot_count) return false;
    if (pos->h <= SOUND_TITLE_H) return false; /* 折叠：只有标题行 */
    *x = (int16_t)(pos->col * (SOUND_COL_W + SOUND_COL_GAP) + SOUND_CARD_PAD +
                   (slot % SOUND_SLOTS_PER_ROW) * SOUND_CELL_W);
    *y = (int16_t)(pos->y + SOUND_TITLE_H + (slot / SOUND_SLOTS_PER_ROW) * SOUND_CELL_H);
    *w = SOUND_CELL_W;
    *h = SOUND_CELL_H;
    return true;
}

static bool present_bit(const uint32_t present[4], uint16_t flat)
{
    return ((present[flat >> 5] >> (flat & 31)) & 1u) != 0u;
}

/* 扁平下标 flat 的槽现在是否可聚焦：存在，且所在卡片没折叠。 */
static bool slot_open(const uint32_t present[4], uint16_t collapsed, int32_t flat)
{
    uint16_t base = 0;
    for (uint8_t c = 0; c < SOUND_CARDS_MAX; c++) {
        if (flat < base + k_sound_cards[c].slot_count)
            return (collapsed & (uint16_t)(1u << c)) == 0u && present_bit(present, (uint16_t)flat);
        base = (uint16_t)(base + k_sound_cards[c].slot_count);
    }
    return false;
}

bool sound_layout_flat_move(const uint32_t present[4], uint16_t collapsed, int16_t from, int8_t delta,
                            uint16_t *out)
{
    if (delta == 0) return false;
    int32_t dir = delta > 0 ? 1 : -1;
    /* from 只算「起点」不计入结果；无焦点（SOUND_SLOT_NONE）= 从头：向后从槽 0 起、向前
     * 从槽 0 的前一位起（环绕到末尾）。 */
    int32_t i = from >= 0 ? from : (dir > 0 ? -1 : 0);
    for (uint16_t step = 0; step < SOUND_SLOT_TABLE_MAX; step++) {
        i += dir;
        if (i < 0) i = SOUND_SLOT_TABLE_MAX - 1;
        else if (i >= SOUND_SLOT_TABLE_MAX) i = 0;
        if (slot_open(present, collapsed, i)) {
            *out = (uint16_t)i;
            return true;
        }
    }
    return false;
}

bool sound_layout_first_present(const uint32_t present[4], uint16_t *out)
{
    for (uint16_t i = 0; i < SOUND_SLOT_TABLE_MAX; i++) {
        if (present_bit(present, i)) {
            *out = i;
            return true;
        }
    }
    return false;
}

uint16_t sound_layout_present_count(const uint32_t present[4])
{
    uint16_t n = 0;
    for (uint8_t w = 0; w < 4; w++) {
        for (uint32_t bits = present[w]; bits != 0u; bits &= bits - 1u) n++;
    }
    return n;
}

bool sound_ring_hit(int32_t dx, int32_t dy)
{
    int32_t r2 = dx * dx + dy * dy;
    return r2 >= SOUND_RING_MIN_R * SOUND_RING_MIN_R && r2 <= SOUND_RING_MAX_R * SOUND_RING_MAX_R;
}
