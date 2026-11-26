/*
 * 参数页布局表（ADR-008 §8；纯数据 + 纯规则，不含 LVGL——host 单测 scripts/knob-test.sh
 * 同源编译）。页元数据（id/title/est/features）与 chip/槽表来自同一张 X-macro 表；注册表
 * 条目（ui_page_t 的 build/update 包装）在 page_params.c。
 *
 * 页与 chip 的槽位来源 = 上游 params.c 的分页语义，权威表 =
 * docs/design/012-feature-parity-checklist.md §3（每 chip 的槽顺序即上游页序）：
 * - SOUND（P4-F Task 5，取代原 ANALOG + FM6 两页）：选中轨引擎的 edit[0..7] = TRACK scope
 *   的 P_E0..P_E7（描述符经 sloop_core_desc_track_live 的规则 4 取该轨引擎的钩子，标签随
 *   LOFI/NOISE/PHYS 的模式换）；页眉 ENGINE/PRESET（鼓轨 KIT）+ 仅 FM6 引擎出现的 bank 行。
 *   5B-1 起该页带 PARAM_FEAT_DASHBOARD：页面不再建这 8 槽（主体 = page_sound.c 的卡片仪表盘，
 *   成员表在 sound_layout.h），表项只留给页眉/bank 行逻辑与 host 测试；
 * - SAMPLE：ENGINES[4]->edit[0..7] 的 6 个真实槽（edit[5]/[7] 是上游占位 ⇒ 空槽）；
 * - 原 GLOBAL / FX / MOD / SCALE / VOICE 五张族页自 5B-1 起退役：它们的全部参数由 SOUND
 *   仪表盘的卡片承载（槽位表 = sound_layout.h），不再进本表、不注册；页 id 保留为
 *   app_page_t 里不注册的枚举值（console `UI <页名>` ⇒ not_registered）。
 *
 * 槽位语义：TRACK → **选中轨**的 TP[]（描述符经 sloop_core_desc_track_live：INSERT/ENV
 * SYNC/引擎钩子的动态标签，范围同静态表）；ENGINE → 页声明引擎的 edit[i]（值取
 * trk[选中轨].p[P_E0+i]）。表里已没有 GLOBAL 槽与 chip 页；条件 chip（when）与鼓轨逐槽
 * 规则留在 page_layout.h 的通用访问器里。
 *
 * id 必须与 sloop_state.h 的 app_page_t 一致（既有 id 不变；APP_PAGE_FM6 是空位、不在表里）；
 * est 是构建前预算门槛（实测池增量 + ~10% 余量、512 对齐，见 ADR-008 预算表；实测超限
 * 先改对象设计，不抬 est）。
 */
#include "page_layout.h"

#include "param_ids.h"

/* 槽位：TRACK（鼓轨锁定）/ ENGINE。空槽 = 旧式整页的占位（SAMPLE 的 edit[5]/[7]）；
 * chip 的 slot_count 之外由零填充表示、不读。 */
#define TS(pid)   { .scope = SLOOP_DESC_TRACK, .engine = 0, .id = (uint8_t)(pid), .drum_ok = 0 }
#define ES(en, i) { .scope = SLOOP_DESC_ENGINE, .engine = (uint8_t)(en), .id = (uint8_t)(i), .drum_ok = 0 }
#define EMPTY_SLOT { .scope = SLOOP_DESC_ENGINE, .engine = 0, .id = 0xFF, .drum_ok = 0 }
#define CHIP_NONE {0}
/* chip：{名, when, 槽数, {槽…}}；表内两页都是无名整页（不画 chip 行），when 恒 ALWAYS */
#define CHIP(nm, wh, ...) \
    { .name = nm, .when = (uint8_t)(wh),                                                   \
      .slot_count = (uint8_t)(sizeof((param_slot_t[]){__VA_ARGS__}) / sizeof(param_slot_t)), \
      .slots = { __VA_ARGS__ } }

/* 布局表（一处定义）：
 * X(下标, 页 id, est, 标题, features, chip 数, chip0, chip1, chip2, chip3, chip4)
 * SOUND est 8,704 = 卡片仪表盘（5B-1：滚动容器 + 13 张自绘卡片）+ 页眉两行 + ± 按钮 +
 * 常驻隐藏的 bank 行：实测 6,044–6,816B + Task 4 预留的两个对象，+10%、512 对齐
 * （2026-11-26 校准；开工时的估算 12,288 是上限，5B 规格 §5.6）。est 只在实测超限时改对象
 * 设计，不抬。宏体内不放注释（每行都要续行）。
 * 宏参数名不能叫 id/title：designator 的 .id/.title 会被参数替换掉。 */
#define PARAM_PAGES(X) \
    X(0, APP_PAGE_ANALOG, 8704, "SOUND", \
      PARAM_FEAT_SOUND | PARAM_FEAT_BANK_FM6 | PARAM_FEAT_DASHBOARD, 0, \
      CHIP(NULL, PARAM_CHIP_ALWAYS, TS(L_P_E0), TS(L_P_E0 + 1), TS(L_P_E0 + 2), TS(L_P_E0 + 3), \
           TS(L_P_E0 + 4), TS(L_P_E0 + 5), TS(L_P_E0 + 6), TS(L_P_E0 + 7)), \
      CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE) \
    X(1, APP_PAGE_SAMPLE, 6144, "SAMPLE", PARAM_FEAT_SAMPLE, 0, \
      CHIP(NULL, PARAM_CHIP_ALWAYS, ES(L_ENG_SAMPLE, L_E0), ES(L_ENG_SAMPLE, L_E1), \
           ES(L_ENG_SAMPLE, L_E2), ES(L_ENG_SAMPLE, L_E3), ES(L_ENG_SAMPLE, L_E4), EMPTY_SLOT, \
           ES(L_ENG_SAMPLE, L_E6), EMPTY_SLOT), \
      CHIP_NONE, CHIP_NONE, CHIP_NONE, CHIP_NONE)

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
