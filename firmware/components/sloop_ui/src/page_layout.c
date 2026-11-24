/*
 * 参数页布局表（ADR-008 §8；纯数据 + 纯规则，不含 LVGL——host 单测 scripts/knob-test.sh
 * 同源编译）。页元数据（id/title/est/features）与 chip/槽表来自同一张 X-macro 表；注册表
 * 条目（ui_page_t 的 build/update 包装）在 page_params.c。
 *
 * 页与 chip 的槽位来源 = 上游 params.c 的分页语义，权威表 =
 * docs/design/012-feature-parity-checklist.md §3（每 chip 的槽顺序即上游页序）：
 * - SOUND（P4-F Task 5，取代原 ANALOG + FM6 两页）：选中轨引擎的 edit[0..7] = TRACK scope
 *   的 P_E0..P_E7（描述符经 sloop_core_desc_track_live 的规则 4 取该轨引擎的钩子，标签随
 *   LOFI/NOISE/PHYS 的模式换）；页眉 ENGINE/PRESET（鼓轨 KIT）+ 仅 FM6 引擎出现的 bank 行；
 * - GLOBAL：GLO（= 原 8 槽：GLOBAL 4 + DLY 4）/ REV（REV/CHO 4 + MASTER 4）/ DRUMS（4）/
 *   TEXTURE（7）——全局参数在鼓轨上全部可编辑；
 * - FX：FX（= 原 8 槽：DST/CHO/DLY/REV + FILT + LVL/PAN/ATK；鼓轨上只有 TFLT/LEVEL/PAN
 *   可编辑）/ SLICER / INSERT / INSERT 2（鼓轨上全部可编辑）；
 * - SAMPLE：ENGINES[4]->edit[0..7] 的 6 个真实槽（edit[5]/[7] 是上游占位 ⇒ 空槽）；
 * - MOD / SCALE / VOICE（P4-F Task 4）：每页多个 chip 复用同一组 8 个旋钮（DSYN 模式），
 *   选择是页内 UI 局部态；VOICE·FILTER 是条件 chip（仅 ANALOG 合成轨出现）。
 *
 * 槽位语义：GLOBAL → GP[]（song.g[]）；TRACK → **选中轨**的 TP[]（描述符经
 * sloop_core_desc_track_live：INSERT/ENV SYNC/引擎钩子的动态标签，范围同静态表）；
 * ENGINE → 页声明引擎的 edit[i]（值取 trk[选中轨].p[P_E0+i]）。
 *
 * id 必须与 sloop_state.h 的 app_page_t 一致（既有 id 不变；APP_PAGE_FM6 是空位、不在表里）；
 * est 是构建前预算门槛（实测池增量 + ~10% 余量、512 对齐，见 ADR-008 预算表；实测超限
 * 先改对象设计，不抬 est）。
 */
#include "page_layout.h"

/* 上游 core.h 的参数 id（布局表只用到这些；枚举是追加式的，值自 SLOOP 2.x 稳定；
 * scripts/knob-test.sh 用上游枚举顺序独立复刻并逐槽比对）。
 * id 写错的后果可见：页面 label 来自描述符，截图/门禁会显示成别的名字。 */
enum {
    /* 轨道参数（TP[]） */
    L_P_LEVEL = 0,
    L_P_ATK = 1,
    L_P_DEC = 2,
    L_P_SUS = 3,
    L_P_REL = 4,
    L_P_ED_FLT = 5,
    L_P_ED_PIT = 6,
    L_P_ED_SHP = 7,
    L_P_LRATE = 9,
    L_P_LWAVE = 10,
    L_P_LPHASE = 11,
    L_P_LFADE = 12,
    L_P_LD_PIT = 13,
    L_P_LD_FLT = 14,
    L_P_LD_SHP = 15,
    L_P_LD_AMP = 16,
    L_P_AMODE = 17,
    L_P_ARATE = 18,
    L_P_AOCT = 19,
    L_P_AGATE = 20,
    L_P_ASWING = 21,
    L_P_APROB = 22,
    L_P_AHOLD = 23,
    L_P_AORDER = 24,
    L_P_ROOT = 25,
    L_P_SCALE = 26,
    L_P_QUANT = 27,
    L_P_TRANS = 28,
    L_P_SLEN = 29,
    L_P_SDIV = 30,
    L_P_SSWING = 31,
    L_P_SGATE = 32,
    L_P_DIST = 33,
    L_P_CHOR = 34,
    L_P_DLY = 35,
    L_P_REV = 36,
    L_P_VOICE = 37,
    L_P_GLIDE = 38,
    L_P_PAN = 39,
    L_P_MUTE = 40,
    L_P_GLMODE = 41,
    L_P_PRIO = 42,
    L_P_ALLOC = 43,
    L_P_DETUNE = 44,
    L_P_CHORD = 49,
    L_P_SLCR = 45,
    L_P_SLPAT = 46,
    L_P_SLRATE = 47,
    L_P_SLDEPTH = 48,
    L_P_TFLT = 50,
    L_P_STRUM = 51,
    L_P_VLEAD = 52,
    L_P_ITYPE = 53,
    L_P_IA = 54,
    L_P_IB = 55,
    L_P_IC = 56,
    L_P_IMIX = 57,
    L_P_FTYPE = 58,
    L_P_ESYNC = 59,
    L_P_LSYNC = 60,
    L_P_LTRIG = 61,
    L_P_LPOL = 62,
    /* 全局参数（GP[]） */
    L_G_BPM = 0,
    L_G_SWING = 1,
    L_G_CLOCK = 2,
    L_G_TUNE = 3,
    L_G_DTIME = 4,
    L_G_DFDBK = 5,
    L_G_DCOLOR = 6,
    L_G_DMIX = 7,
    L_G_RSIZE = 8,
    L_G_RDAMP = 9,
    L_G_CRATE = 10,
    L_G_CDEPTH = 11,
    L_G_DRCH = 24,
    L_G_DRLVL = 25,
    L_G_DRREV = 26,
    L_G_DUST = 27,
    L_G_DUCK = 28,
    L_G_FILT = 29,
    L_G_ROLL = 30,
    L_G_DRDLY = 32,
    L_G_TXSRC = 37,
    L_G_TXLVL = 38,
    L_G_TXTONE = 39,
    L_G_TXMOVE = 40,
    L_G_TXRATE = 41,
    L_G_TXWHEN = 42,
    L_G_TXDUCK = 43,
    /* SOUND 页的 8 个引擎槽（= P_E0..P_E7 的绝对 id；TRACK scope 走 live 路径） */
    L_P_E0 = 63,
    /* 引擎槽（P_E0..P_E7 的相对 id） */
    L_E0 = 0,
    L_E1 = 1,
    L_E2 = 2,
    L_E3 = 3,
    L_E4 = 4,
    L_E5 = 5,
    L_E6 = 6,
    L_E7 = 7,
    /* 引擎页的页声明引擎号（上游 engines.c ENGINES[]：ANALOG=0、SAMPLE=4；FM6=9 是
     * page_layout.h 的 PARAM_ENGINE_FM6——SOUND 页 bank 行的可见性谓词） */
    L_ENG_ANALOG = 0,
    L_ENG_SAMPLE = 4,
};

/* 槽位：TRACK（鼓轨锁定）/ TRACK 且鼓轨可编辑 / GLOBAL / ENGINE。空槽 = 旧式整页的
 * 占位（SAMPLE 的 edit[5]/[7]）；chip 的 slot_count 之外由零填充表示、不读。 */
#define TS(pid)   { .scope = SLOOP_DESC_TRACK, .engine = 0, .id = (uint8_t)(pid), .drum_ok = 0 }
#define TD(pid)   { .scope = SLOOP_DESC_TRACK, .engine = 0, .id = (uint8_t)(pid), .drum_ok = 1 }
#define GS(pid)   { .scope = SLOOP_DESC_GLOBAL, .engine = 0, .id = (uint8_t)(pid), .drum_ok = 1 }
#define ES(en, i) { .scope = SLOOP_DESC_ENGINE, .engine = (uint8_t)(en), .id = (uint8_t)(i), .drum_ok = 0 }
#define EMPTY_SLOT { .scope = SLOOP_DESC_ENGINE, .engine = 0, .id = 0xFF, .drum_ok = 0 }
#define CHIP_NONE {0}
/* chip：{名, when, 槽数, {槽…}}；旧式整页 = 无名 chip（不画 chip 行） */
#define CHIP(nm, wh, ...) \
    { .name = nm, .when = (uint8_t)(wh),                                                   \
      .slot_count = (uint8_t)(sizeof((param_slot_t[]){__VA_ARGS__}) / sizeof(param_slot_t)), \
      .slots = { __VA_ARGS__ } }

/* 布局表（一处定义）：
 * X(下标, 页 id, est, 标题, features, chip 数, chip0, chip1, chip2, chip3, chip4)
 * est（chip 页 = 3,584）：chip 行是**一个**自绘对象（每 chip 一个 label 的首版实测
 * MOD 4,544B，会逼 est 到 5,120 并让 EDIT→STEP 有序对账面 63,848 > 63,120；改对象设计
 * 后实测 3,012–3,072B，est = 实测 +17..19%、512 对齐，见 ADR-008 §8.9）。SOUND est
 * 6,144 = 8 旋钮 + 页眉两行 + 常驻隐藏的 bank 行（上限 7,168）。est 只在实测超限时改对象
 * 设计，不抬。宏体内不放注释（每行都要续行）。
 * 宏参数名不能叫 id/title：designator 的 .id/.title 会被参数替换掉。 */
#define PARAM_PAGES(X) \
    X(0, APP_PAGE_GLOBAL, 3584, "GLOBAL", 0, 4, \
      CHIP("GLO", PARAM_CHIP_ALWAYS, GS(L_G_BPM), GS(L_G_SWING), GS(L_G_CLOCK), GS(L_G_TUNE), \
           GS(L_G_DTIME), GS(L_G_DFDBK), GS(L_G_DCOLOR), GS(L_G_DMIX)), \
      CHIP("REV", PARAM_CHIP_ALWAYS, GS(L_G_RSIZE), GS(L_G_RDAMP), GS(L_G_CRATE), GS(L_G_CDEPTH), \
           GS(L_G_DUST), GS(L_G_DUCK), GS(L_G_FILT), GS(L_G_ROLL)), \
      CHIP("DRUMS", PARAM_CHIP_ALWAYS, GS(L_G_DRCH), GS(L_G_DRLVL), GS(L_G_DRREV), GS(L_G_DRDLY)), \
      CHIP("TEXTURE", PARAM_CHIP_ALWAYS, GS(L_G_TXSRC), GS(L_G_TXLVL), GS(L_G_TXTONE), \
           GS(L_G_TXMOVE), GS(L_G_TXRATE), GS(L_G_TXWHEN), GS(L_G_TXDUCK)), \
      CHIP_NONE) \
    X(1, APP_PAGE_FX, 3584, "FX", 0, 4, \
      CHIP("FX", PARAM_CHIP_ALWAYS, TS(L_P_DIST), TS(L_P_CHOR), TS(L_P_DLY), TS(L_P_REV), \
           TD(L_P_TFLT), TD(L_P_LEVEL), TD(L_P_PAN), TS(L_P_ATK)), \
      CHIP("SLICER", PARAM_CHIP_ALWAYS, TD(L_P_SLCR), TD(L_P_SLPAT), TD(L_P_SLRATE), \
           TD(L_P_SLDEPTH)), \
      CHIP("INSERT", PARAM_CHIP_ALWAYS, TD(L_P_ITYPE), TD(L_P_IA), TD(L_P_IB), TD(L_P_IC)), \
      CHIP("INSERT 2", PARAM_CHIP_ALWAYS, TD(L_P_IMIX)), \
      CHIP_NONE) \
    X(2, APP_PAGE_ANALOG, 6144, "SOUND", PARAM_FEAT_SOUND | PARAM_FEAT_BANK_FM6, 0, \
      CHIP(NULL, PARAM_CHIP_ALWAYS, TS(L_P_E0), TS(L_P_E0 + 1), TS(L_P_E0 + 2), TS(L_P_E0 + 3), \
           TS(L_P_E0 + 4), TS(L_P_E0 + 5), TS(L_P_E0 + 6), TS(L_P_E0 + 7)), \
      CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE) \
    X(3, APP_PAGE_SAMPLE, 6144, "SAMPLE", PARAM_FEAT_SAMPLE, 0, \
      CHIP(NULL, PARAM_CHIP_ALWAYS, ES(L_ENG_SAMPLE, L_E0), ES(L_ENG_SAMPLE, L_E1), \
           ES(L_ENG_SAMPLE, L_E2), ES(L_ENG_SAMPLE, L_E3), ES(L_ENG_SAMPLE, L_E4), EMPTY_SLOT, \
           ES(L_ENG_SAMPLE, L_E6), EMPTY_SLOT), \
      CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE) \
    X(4, APP_PAGE_MOD, 3584, "MOD", 0, 5, \
      CHIP("ENV", PARAM_CHIP_ALWAYS, TS(L_P_ATK), TS(L_P_DEC), TS(L_P_SUS), TS(L_P_REL)), \
      CHIP("ENV DEST", PARAM_CHIP_ALWAYS, TS(L_P_ED_FLT), TS(L_P_ED_PIT), TS(L_P_ED_SHP), \
           TS(L_P_ESYNC)), \
      CHIP("LFO", PARAM_CHIP_ALWAYS, TS(L_P_LRATE), TS(L_P_LWAVE), TS(L_P_LPHASE), \
           TS(L_P_LFADE)), \
      CHIP("LFO 2", PARAM_CHIP_ALWAYS, TS(L_P_LSYNC), TS(L_P_LTRIG), TS(L_P_LPOL)), \
      CHIP("LFO DEST", PARAM_CHIP_ALWAYS, TS(L_P_LD_PIT), TS(L_P_LD_FLT), TS(L_P_LD_SHP), \
           TS(L_P_LD_AMP))) \
    X(5, APP_PAGE_SCALE, 3584, "SCALE", 0, 4, \
      CHIP("SEL", PARAM_CHIP_ALWAYS, TS(L_P_ROOT), TS(L_P_SCALE), TS(L_P_QUANT), \
           TS(L_P_CHORD)), \
      CHIP("SEL 2", PARAM_CHIP_ALWAYS, TS(L_P_TRANS), TS(L_P_STRUM), TS(L_P_VLEAD)), \
      CHIP("ARP", PARAM_CHIP_ALWAYS, TS(L_P_AMODE), TS(L_P_ARATE), TS(L_P_AOCT), \
           TS(L_P_AGATE)), \
      CHIP("ARP 2", PARAM_CHIP_ALWAYS, TS(L_P_ASWING), TS(L_P_APROB), TS(L_P_AHOLD), \
           TS(L_P_AORDER)), \
      CHIP_NONE) \
    X(6, APP_PAGE_VOICE, 3584, "VOICE", 0, 4, \
      CHIP("VOICE", PARAM_CHIP_ALWAYS, TS(L_P_VOICE), TS(L_P_GLIDE), TS(L_P_GLMODE), \
           TS(L_P_PRIO)), \
      CHIP("VOICE 2", PARAM_CHIP_ALWAYS, TS(L_P_ALLOC), TS(L_P_DETUNE), TD(L_P_PAN), \
           TS(L_P_MUTE)), \
      CHIP("PATTERN", PARAM_CHIP_ALWAYS, TD(L_P_SLEN), TD(L_P_SDIV), TD(L_P_SSWING), \
           TD(L_P_SGATE)), \
      CHIP("FILTER", PARAM_CHIP_WHEN_ANALOG, TS(L_P_FTYPE), ES(L_ENG_ANALOG, L_E4), \
           ES(L_ENG_ANALOG, L_E5), ES(L_ENG_ANALOG, L_E7)), \
      CHIP_NONE)

/* 页定义来自同一张 X 宏表。 */
#define X_PAGE(n, pid, pest, ptitle, pfeat, nchips, c0, c1, c2, c3, c4)                \
    [n] = { .page_id = (uint8_t)(pid), .title = ptitle, .est_bytes = (pest),          \
            .chip_count = (uint8_t)(nchips), .features = (uint8_t)(pfeat),            \
            .chips = { c0, c1, c2, c3, c4 } },
static const param_page_t k_pages[PARAM_PAGE_COUNT] = { PARAM_PAGES(X_PAGE) };

_Static_assert(sizeof(k_pages) / sizeof(k_pages[0]) == PARAM_PAGE_COUNT, "布局表页数不符");

const param_page_t *page_layout_get(uint8_t index)
{
    return index < PARAM_PAGE_COUNT ? &k_pages[index] : NULL;
}

int page_layout_index(uint8_t page_id)
{
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        if (k_pages[i].page_id == page_id) return (int)i;
    }
    return -1;
}
