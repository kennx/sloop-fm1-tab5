/*
 * 参数页布局表（ADR-008 §8）：GLOBAL / FX / ANALOG / FM6 / SAMPLE 五页，每页 8 槽。
 *
 * 槽位来源 = 上游 params.c 的分页语义：
 * - GLOBAL：GLO > GLOBAL 4 槽（BPM/SWING/CLICK/TUNE）+ GLO > DLY 4 槽（TIME/FDBK/COLR/MIX）
 *   —— 一个「全局 + 主延时」页（全局页在上游是两页，这里合成 8 槽一页）；
 * - FX：TRACK 的 FX 4 件（DST/CHO/DLY/REV，上游 FAM_FX）+ 轨道滤波 FILT（P_TFLT）
 *   + 输出条（LVL/PAN/ATK：电平、声像与起音，便于与 FX 一起塑形）；
 * - ANALOG：引擎 edit[0..7]（WAVE/DTN/MIX/NOIS/CUT/RES/DRV/KTR，上游 EDIT 1/2 合并）；
 * - FM6：ENGINES[9]->edit[0..7] 全 8 槽（ALG/FB/MLVL/MRAT/MEG/VMOD/DTUN/PTCH，上游
 *   FM6 EDIT 1+2）——ALG 是 F_INT+33 名（PAT/1..32），PTCH 是 F_INT+35 名（F1..F8/B1..B27）；
 * - SAMPLE：ENGINES[4]->edit[0..7] 的 6 个真实槽（SET/TUNE/BITS/LOOP/CUT/DRV）；上游
 *   edit[5]/edit[7] 是占位（label "-"、min=max=0），留空槽不建旋钮。
 *
 * 页元数据（id/title/est）与 REGISTRY 条目由同一个 X-macro 生成：页签顺序 = 本表
 * 声明顺序。id 必须与 sloop_state.h 的 app_page_t 一致；est 是构建前预算门槛
 * （实测池增量 + ~10% 余量、512 对齐，见 ADR-008 预算表）。
 */
#include "page_layout.h"

#include <stddef.h>

#include "page_params.h"
#include "theme.h"

/* 上游 core.h 的参数 id（布局表只用到这些；枚举是追加式的，值自 SLOOP 2.x 稳定）。
 * id 写错的后果可见：页面 label 来自 sloop_core_desc，截图/门禁会显示成别的名字。 */
enum {
    /* 轨道参数（TP[]，sloop_state 的 P_*） */
    L_P_LEVEL = 0,
    L_P_ATK = 1,
    L_P_DIST = 33,
    L_P_CHOR = 34,
    L_P_DLY = 35,
    L_P_REV = 36,
    L_P_PAN = 39,
    L_P_TFLT = 50,
    /* 全局参数（GP[]） */
    L_G_BPM = 0,
    L_G_SWING = 1,
    L_G_CLOCK = 2,
    L_G_TUNE = 3,
    L_G_DTIME = 4,
    L_G_DFDBK = 5,
    L_G_DCOLOR = 6,
    L_G_DMIX = 7,
    /* 引擎槽（P_E0..P_E7 的相对 id） */
    L_E0 = 0,
    L_E1 = 1,
    L_E2 = 2,
    L_E3 = 3,
    L_E4 = 4,
    L_E5 = 5,
    L_E6 = 6,
    L_E7 = 7,
    /* 引擎页的页声明引擎号（上游 engines.c ENGINES[]：SAMPLE=4、FM6=9） */
    L_ENG_SAMPLE = 4,
    L_ENG_FM6 = 9,
};

#define SLOT(sc, en, pid) { .scope = (uint8_t)(sc), .engine = (uint8_t)(en), .id = (uint8_t)(pid) }
#define EMPTY_SLOT SLOT(SLOOP_DESC_ENGINE, 0, 0xFF)

#define PARAM_PAGES(X)                                                                        \
    X(0, APP_PAGE_GLOBAL, "GLOBAL", 3584,                                                     \
      SLOT(SLOOP_DESC_GLOBAL, 0, L_G_BPM), SLOT(SLOOP_DESC_GLOBAL, 0, L_G_SWING),            \
      SLOT(SLOOP_DESC_GLOBAL, 0, L_G_CLOCK), SLOT(SLOOP_DESC_GLOBAL, 0, L_G_TUNE),            \
      SLOT(SLOOP_DESC_GLOBAL, 0, L_G_DTIME), SLOT(SLOOP_DESC_GLOBAL, 0, L_G_DFDBK),           \
      SLOT(SLOOP_DESC_GLOBAL, 0, L_G_DCOLOR), SLOT(SLOOP_DESC_GLOBAL, 0, L_G_DMIX))           \
    X(1, APP_PAGE_FX, "FX", 3584,                                                             \
      SLOT(SLOOP_DESC_TRACK, 0, L_P_DIST), SLOT(SLOOP_DESC_TRACK, 0, L_P_CHOR),               \
      SLOT(SLOOP_DESC_TRACK, 0, L_P_DLY), SLOT(SLOOP_DESC_TRACK, 0, L_P_REV),                 \
      SLOT(SLOOP_DESC_TRACK, 0, L_P_TFLT), SLOT(SLOOP_DESC_TRACK, 0, L_P_LEVEL),              \
      SLOT(SLOOP_DESC_TRACK, 0, L_P_PAN), SLOT(SLOOP_DESC_TRACK, 0, L_P_ATK))                 \
    X(2, APP_PAGE_ANALOG, "ANALOG", 3584,                                                     \
      SLOT(SLOOP_DESC_ENGINE, 0, L_E0), SLOT(SLOOP_DESC_ENGINE, 0, L_E1),                     \
      SLOT(SLOOP_DESC_ENGINE, 0, L_E2), SLOT(SLOOP_DESC_ENGINE, 0, L_E3),                     \
      SLOT(SLOOP_DESC_ENGINE, 0, L_E4), SLOT(SLOOP_DESC_ENGINE, 0, L_E5),                     \
      SLOT(SLOOP_DESC_ENGINE, 0, L_E6), SLOT(SLOOP_DESC_ENGINE, 0, L_E7))                     \
    X(3, APP_PAGE_FM6, "FM6", 3584,                                                           \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E0), SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E1),     \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E2), SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E3),     \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E4), SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E5),     \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E6), SLOT(SLOOP_DESC_ENGINE, L_ENG_FM6, L_E7))     \
    X(4, APP_PAGE_SAMPLE, "SAMPLE", 3072,                                                     \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E0),                                            \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E1),                                            \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E2),                                            \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E3),                                            \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E4), EMPTY_SLOT,                                \
      SLOT(SLOOP_DESC_ENGINE, L_ENG_SAMPLE, L_E6), EMPTY_SLOT)

/* 页定义（title + 槽位）与注册表条目来自同一张 X 宏表。
 * 宏参数名不能叫 id/title：designator 的 .id/.title 会被参数替换掉。 */
#define X_PAGE(n, pid, ptitle, pest, ...) [n] = { .title = ptitle, .slots = { __VA_ARGS__ } },
static const param_page_t k_pages[PARAM_PAGE_COUNT] = { PARAM_PAGES(X_PAGE) };

#define X_BUILD(n, pid, ptitle, pest, ...)                                                       \
    static lv_obj_t *params_build_##n(lv_obj_t *parent) { return page_params_build(n, parent); } \
    static void params_update_##n(const app_state_t *st) { page_params_update(n, st); }
PARAM_PAGES(X_BUILD)

#define X_ENTRY(n, pid, ptitle, pest, ...) \
    { .id = (uint8_t)(pid), .title = ptitle, .build = params_build_##n, \
      .update = params_update_##n, .est_bytes = (pest) },
static const ui_page_t k_entries[PARAM_PAGE_COUNT] = { PARAM_PAGES(X_ENTRY) };

_Static_assert(sizeof(k_pages) / sizeof(k_pages[0]) == PARAM_PAGE_COUNT, "布局表页数不符");
_Static_assert(sizeof(k_entries) / sizeof(k_entries[0]) == PARAM_PAGE_COUNT, "注册表页数不符");
/* 参数页是 app_page_t 里的**连续段**（GLOBAL..SAMPLE）；其后可追加 FILE 等页
 * （P4-B Task 10 起 APP_STATE_PAGES = 11，尾两页不属于本表）。 */
_Static_assert(PARAM_PAGE_COUNT == APP_PAGE_SAMPLE - APP_PAGE_GLOBAL + 1u,
               "PARAM_PAGE_COUNT 与 app_page_t 的参数页 id 段不同步");

const param_page_t *page_layout_get(uint8_t index)
{
    return index < PARAM_PAGE_COUNT ? &k_pages[index] : NULL;
}

int page_layout_index(uint8_t page_id)
{
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        if (k_entries[i].id == page_id) return (int)i;
    }
    return -1;
}

esp_err_t page_layout_register(void)
{
    esp_err_t first = ESP_OK;
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        esp_err_t ret = ui_page_register(&k_entries[i]);
        if (ret != ESP_OK && first == ESP_OK) first = ret;
    }
    return first;
}
