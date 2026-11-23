/*
 * 描述符驱动的参数页（ADR-008 §8）：布局表 8 槽旋钮 + 页眉 + ± 细调。
 *
 * 布局（页面区相对坐标；页面区绝对 y 起点 = UI_TAB_BAR_H = 72）：
 * - 页眉：上下文行 (16, 6) 20pt（GLOBAL / TRACK n / ANALOG · TRACK n；引擎不符时红字
 *   "… USES ENG k — N/A"）；
 * - ± 细调：右上 (−) (1096,2) / (+) (1184,2)，80×40，作用于**焦点槽**（点旋钮设焦点）；
 * - 旋钮网格：4 列 × 2 行，cell 300×290，x = 16 + c·316，y = 52 + r·298
 *   （旋钮盒 160×160 在单元内 (70,26)：弧线 + 名称/数值两行文本，自绘见 knob.c）。
 *
 * 值路径（唯一）：槽位值 → 状态层命令 → 引擎（钳位在引擎）→ 立即回读投影刷新本页。
 * 触摸：按下只设焦点+锚点，拖动按相对角度差改值（粗调，knob.c）；± 与键盘 ↑/↓ 是
 * 1 个原始单位（细调）。调试注入 console `UI KNOB` 走同一指针路径（ADR-008 §8.2）。
 * TAP 注入与控件回调共用同一动作函数（param_step/slot_write），只是 TAP 不能拖拽。
 *
 * 引擎页的适用性：槽位描述符取**页声明的引擎**（布局表），值是 trk[选中轨].p[P_E0+i]；
 * 选中轨的引擎与页不符（或选中鼓轨）时页面不可编辑、显示 --，避免拿 ANALOG 的 label
 * 写另一个引擎的槽位。
 *
 * bank 存取行（仅 FM6 页，P4-B Task 13）：页面区底部一行 `BANK - B12 + | SAVE | ERASE`
 * + 状态行（页面区相对 y=552..596；旋钮盒底边 536，留 16px，不挤压旋钮环/文本）。
 * 槽号是**目标槽**（0..26，显示为 `B<n+1>`），与 PTCH 旋钮（音源，PTCH = 8 + 槽号）
 * 相互独立：SAVE = 选中轨的 live patch（`CMD_BANK_PUT`），ERASE = 擦目标槽
 * （`CMD_BANK_ERASE`）。播放中由存储层拒绝（状态 3 ⇒ `STOP BEFORE SAVE`），状态行与
 * FILE 页同一映射（theme.h 的 ui_file_status_text，act 0 = SAVE / 2 = ERASE）。
 * 键盘：FM6 页的 ←/→ 仍只在旋钮间移动焦点（不改既有旋钮模型），bank 行**只经触摸**
 * （TAP 注入与按钮回调共用同一动作函数）；SAVE/ERASE 与 FILE 页同样只走触摸 + console。
 * **页面锁定**（选中轨不是 FM6，含鼓轨）时 SAVE/ERASE **直接拒绝**：不发状态层命令、
 * 不动存储，状态行显示 `NOT FM6`（本地状态，不进 app_state_t.store_status）——避免用
 * 别的引擎的轨把该轨最后一次载入的 patch（上电时是 INIT VOICE）写进用户银行槽。
 * 擦写发生在 LVGL 回调里（TAP/按钮 → state_apply → 存储层），一次 SAVE 最坏阻塞
 * 一次整银行擦写窗口（spec §4.3 接受的 ≤150ms 级 UI 停顿）；bank 是 4KB 扇区重写，
 * 与工程/预置保存同一条「擦写期间不持引擎锁」纪律（ADR-009 §3）。
 *
 * 槽管理行（仅 SAMPLE 页，P4-D Task 8）：同一底部行几何（页面区相对 y=552..596）——
 * `U1..U4` 四个单对象 label（`USED name z<n> <KB>K` / `EMPTY`）+ `ERASE` chip + 状态行。
 * 点槽 = 把 SET 宏（P_E0）切到 `SMP_NSETS + k`（**8..11**，不是工厂槽号 32）经
 * `CMD_SET_PARAM` 下发；`ERASE` 只在当前 SET 落在 USR1..USR4 时发 `CMD_SAMPLE_ERASE`
 * （槽号 = SET − 8），否则状态行 `NOT USR` 且**不下发命令**。状态行是页面本地结果
 * （theme.h 的 ui_sample_status_text 单一映射：`SAVE OK`/`ERASE OK`/`STOP BEFORE ERASE`/
 * `NO STORAGE`/`ERASE ERROR`）。
 * 槽信息读取是**文档化的只读例外**（与 FILE 页同口径）：`sloop_store_sample_info` 会
 * 对每个已用槽算 ≤80KiB 的数据 CRC，因此只在建页、槽动作之后与 update 里至多 ~1s
 * 一次（或 synth_status/SET 变化时）读取——绝不每 30ms 轮询（P4-D Task 4 评审的
 * UI 停顿风险）。
 */
#include "page_params.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "knob.h"
#include "page_layout.h"
#include "sloop_store.h"
#include "theme.h"

/* 网格/控件几何（TAP 命中矩形与建控件同源） */
#define HEADER_X 16
#define HEADER_Y 6
#define GRID_X0  16
#define GRID_Y0  52
#define CELL_W   300
#define CELL_H   290
#define CELL_GAP 16
#define ROW_GAP  8
#define CELL_PITCH (CELL_W + CELL_GAP) /* 316 */
#define ROW_PITCH  (CELL_H + ROW_GAP)  /* 298 */
#define BTN_W     80
#define BTN_H     40
#define BTN_Y     2
#define BTN_DEC_X 1096
#define BTN_INC_X 1184

/* bank 存取行（仅 FM6 页，P4-B Task 13）：旋钮盒底边 = GRID_Y0 + ROW_PITCH +
 * KNOB_OBJ_DY + KNOB_OBJ_SIZE = 536，行取 y=552（16px 间隙）、高 44，页面区高 648
 * ⇒ 不与旋钮环/两行文本重叠，也不需要压缩旋钮区。 */
#define BANK_ROW_Y    552
#define BANK_ROW_H    44
#define BANK_LABEL_X  16
#define BANK_LABEL_W  88
#define BANK_DEC_X    112
#define BANK_VAL_X    184
#define BANK_VAL_W    96
#define BANK_INC_X    288
#define BANK_SLOT_W   64
#define BANK_SAVE_X   368
#define BANK_ERASE_X  484
#define BANK_ACT_W    100
#define BANK_STATUS_X 600
#define BANK_STATUS_W 664

/* SAMPLE 槽管理行（仅 SAMPLE 页，P4-D Task 8）：与 BANK 行同一底部带（y=552、高 44），
 * 4 个槽 label（200px，2 行 16pt：`U1 USED` / `PIANO z1 12K`）+ ERASE chip + 状态行。
 * 16+4×208=848 右端（末槽 640..840）、ERASE 856..956、状态行 972..1264。 */
#define SAMPLE_ROW_Y      552
#define SAMPLE_ROW_H      44
#define SAMPLE_SLOT_X0    16
#define SAMPLE_SLOT_W     200
#define SAMPLE_SLOT_PITCH 208
#define SAMPLE_ERASE_X    856
#define SAMPLE_STATUS_X   972
#define SAMPLE_STATUS_W   292

/* 上游 eng_sample.c 的 SMP_NSETS（生成头 felucca_samples.h 的 8 个工厂 SET 名）；
 * USR1..USR4 的 SET 宏值 = SMP_NSETS + k = 8..11。写成字面量免于在 UI 层包含上游
 * 生成头（它与 console 的 SMP 诊断面同源，改上游版本时由 host 门禁暴露）。 */
#define SAMPLE_USR_SET_BASE 8u

/* 页声明引擎号 9 = FM6（布局表 L_ENG_FM6，上游 ENGINES[] 的 FM6）；bank 行只在这一页。 */
#define PARAM_ENGINE_FM6 9u

/* 引擎槽在 trk[].p[] 里的绝对 id 起点（上游 P_E0 = P_COUNT - 8） */
#define PARAM_ENGINE_BASE (SLOOP_CORE_P_COUNT - 8u)

typedef struct {
    knob_t knobs[PARAM_PAGE_SLOTS];
    bool present[PARAM_PAGE_SLOTS]; /* 该槽有描述符且已建控件 */
    lv_obj_t *header;
    lv_obj_t *btn_dec;
    lv_obj_t *btn_inc;
    uint8_t shown_track;
    uint8_t shown_engine;
    int8_t shown_locked;  /* -1 = 尚未渲染上下文 */
    int8_t shown_esync;   /* 选中轨 ENV SYNC 位（ATK 槽描述符随它变，见 slot_desc） */
    uint8_t focus_shown;  /* 最近渲染的焦点槽 */
    bool ctx_valid;
    /* bank 存取行（仅 FM6 页；其它页为 NULL） */
    lv_obj_t *bank_label;
    lv_obj_t *bank_dec;
    lv_obj_t *bank_inc;
    lv_obj_t *bank_value;
    lv_obj_t *bank_save;
    lv_obj_t *bank_erase;
    lv_obj_t *bank_status;
    uint8_t bank_shown_slot;  /* 已显示的目标槽（0..26）；0xFF = 未显示 */
    uint32_t bank_status_key; /* (act | status<<8 | ready<<16) 脏检查键 */
    /* 槽管理行（仅 SAMPLE 页；其它页为 NULL） */
    lv_obj_t *sample_label[SLOOP_STORE_SAMPLES];
    lv_obj_t *sample_erase;
    lv_obj_t *sample_status;
} page_ui_t;

static page_ui_t s_ui[PARAM_PAGE_COUNT];

/* bank 目标槽（UI 局部态；TAP/按钮都在 LVGL 上下文读写，页面重建时归零）与本页最后
 * 一次 bank 动作（0 = SAVE / 2 = ERASE / 其它 = 尚无动作）——动作语义下标，与
 * theme.h 的 ui_file_status_text 的文案表一致。 */
static uint8_t s_bank_slot;
static uint8_t s_bank_act = 0xFF;
/* 锁定页拒绝（页内状态，不是存储层结果）：1 = 上一次 SAVE/ERASE 因「选中轨引擎 ≠ FM6」
 * 被拒（提示 `NOT FM6`），0 = 无。 */
static uint8_t s_bank_refused;
static char s_bank_echo[32]; /* console TAP 回显专用（REPL 任务，避开 LVGL 标签缓冲） */

/* 槽管理行（仅 SAMPLE 页）：槽信息缓存 + 状态行（页面本地结果，不写 app_state_t）。
 * s_sample_info_key = (synth_status << 8) | SET：变化即立刻重读（动作结果的可见性）；
 * 其余情况下 update 每 ~1s 至多重读一次（info 的 CRC 成本见文件头）。 */
static sloop_store_sample_info_t s_sample_info[SLOOP_STORE_SAMPLES];
static uint16_t s_sample_info_key;
static uint32_t s_sample_info_ms;
static bool s_sample_info_valid;
static uint8_t s_sample_act = 0xFF; /* 0 = 槽选中（SAVE 语义）/ 2 = ERASE / 其它 = 尚无动作 */
static ui_sample_status_t s_sample_status = UI_SAMPLE_STATUS_NONE;
static uint32_t s_sample_row_sig = 0xFFFFFFFFu; /* 整行渲染脏检查键 */
static char s_sample_echo[32];                  /* console TAP 回显专用 */

/* 焦点槽（UI 局部态）：TAP 在 LVGL 上下文写、键盘在输入任务读/写 → 临界区保护。 */
static portMUX_TYPE s_focus_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_focus[PARAM_PAGE_COUNT];

/* 旋钮的页面上下文编码进 knob_t.user：((页下标 << 4) | 槽位) + 1。
 * +1 让「页 0 槽 0」不是 NULL——NULL 是「未设置」的哨兵，初版直接编码成 NULL，
 * 与未接线无法区分。 */
#define KNOB_CTX(page, slot)     ((void *)(uintptr_t)((((unsigned)(page) << 4) | (unsigned)(slot)) + 1u))
static uint8_t ctx_page(const void *p)
{
    return (uint8_t)((((uintptr_t)p) - 1u) >> 4);
}
static uint8_t ctx_slot(const void *p)
{
    return (uint8_t)((((uintptr_t)p) - 1u) & 0x0Fu);
}

static uint8_t focus_get(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT) return 0; /* 与 focus_set 同一边界 */
    portENTER_CRITICAL(&s_focus_mux);
    uint8_t f = s_focus[index];
    portEXIT_CRITICAL(&s_focus_mux);
    return f < PARAM_PAGE_SLOTS ? f : 0u;
}

static void focus_set(uint8_t index, uint8_t slot)
{
    if (index >= PARAM_PAGE_COUNT || slot >= PARAM_PAGE_SLOTS) return;
    portENTER_CRITICAL(&s_focus_mux);
    s_focus[index] = slot;
    portEXIT_CRITICAL(&s_focus_mux);
}

/* ---------------------------------------------------------------- 槽位解析 --- */
/* 上游 TP[] 的 ATK 与 ENV SYNC 开关 id（core.h 枚举值，SLOOP 2.8.5 起稳定；布局表
 * FX 页的 ATK 槽就是 TP[1]）。 */
#define PARAM_TP_ATK   1u
#define PARAM_TP_ESYNC 59u
/* P_ESYNC 打开时上游 track_desc() 把 ATK/DEC/REL 换成 ESYNC_DESC[]：F_INT + 音符名
 * 表、单位 1/384 小节；静态 TP[P_ATK] 仍是 F_TIME（ms）。sloop_core_desc 不投影
 * 动态钩子（需要轨上下文），所以这里显式标注：名称后缀 /ESYNC、按 0..127 原始值
 * 显示（无 ms 后缀），绝不把音符值显示成毫秒。P4-B 接 ENV 页/动态描述符时改为取
 * 上游动态表（ADR-008 §9 的 P4-B 复核项）。 */
static bool track_env_sync_on(void)
{
    app_state_t st;
    state_get_copy(&st);
    return st.core.selected_track < APP_STATE_TRACKS && st.p[PARAM_TP_ESYNC] != 0;
}

static bool slot_desc(const param_page_t *pg, uint8_t slot, sloop_desc_t *out)
{
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF) return false;
    if (!sloop_core_desc((sloop_desc_scope_t)s->scope, s->engine, s->id, out)) return false;
    if (s->scope == SLOOP_DESC_TRACK && s->id == PARAM_TP_ATK && track_env_sync_on()) {
        out->label = "ATK/ESYNC";
        out->fmt = SLOOP_DESC_FMT_INT;
        out->unit = NULL;
        out->enums = NULL;
        out->enum_count = 0;
    }
    return true;
}

/* 页声明的引擎（ENGINE 槽位的 engine 字段）；非引擎页返回 0xFF */
static uint8_t page_engine(const param_page_t *pg)
{
    for (uint8_t i = 0; i < PARAM_PAGE_SLOTS; i++) {
        if (pg->slots[i].id != 0xFF && pg->slots[i].scope == SLOOP_DESC_ENGINE) {
            return pg->slots[i].engine;
        }
    }
    return 0xFF;
}

/* FM6 页 = 声明引擎 9 的引擎页：bank 存取行只在这一页建（TAP 命中表/更新/回显同源）。 */
static bool bank_row_present(const param_page_t *pg)
{
    return pg != NULL && page_engine(pg) == PARAM_ENGINE_FM6;
}

/* SAMPLE 页 = 布局表的 SAMPLE 下标（参数页是 app_page_t 的连续段，page_layout.c 有
 * 静态断言）：槽管理行只在这一页建（TAP 命中表/更新/回显同源）。 */
static bool sample_row_present(const param_page_t *pg)
{
    return pg != NULL && pg == page_layout_get((uint8_t)(APP_PAGE_SAMPLE - APP_PAGE_GLOBAL));
}

/* 引擎页不可编辑：选中鼓轨（没有引擎槽）或所选轨引擎与页声明不符 */
static bool page_locked(const param_page_t *pg, const app_state_t *st)
{
    uint8_t eng = page_engine(pg);
    if (eng == 0xFF) return false;
    uint8_t track = st->core.selected_track;
    if (track >= 3u) return true;
    return st->core.trk[track].engine != eng;
}

/* 槽位当前值（GLOBAL→g[]、TRACK→p[]、ENGINE→p[P_E0+id]）；不可读返回 false */
static bool slot_value(const param_page_t *pg, uint8_t slot, const app_state_t *st, int16_t *out)
{
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF || out == NULL) return false;
    switch (s->scope) {
    case SLOOP_DESC_GLOBAL:
        if (s->id >= SLOOP_CORE_G_COUNT) return false;
        *out = st->g[s->id];
        return true;
    case SLOOP_DESC_TRACK:
        if (s->id >= PARAM_ENGINE_BASE) return false;
        *out = st->p[s->id];
        return true;
    case SLOOP_DESC_ENGINE:
        if (s->id >= 8u) return false;
        *out = st->p[PARAM_ENGINE_BASE + s->id];
        return true;
    default:
        return false;
    }
}

/* 槽位参数在状态层命令里的 id：GLOBAL 是全局 id，TRACK 是 TP id，ENGINE 是 P_E0+i */
static uint8_t slot_abs_id(const param_page_t *pg, uint8_t slot)
{
    const param_slot_t *s = &pg->slots[slot];
    return s->scope == SLOOP_DESC_ENGINE ? (uint8_t)(PARAM_ENGINE_BASE + s->id) : s->id;
}

/* 写槽位值（状态层命令 + 引擎钳位）；返回 false = 页面不可编辑/槽位不可读。
 * applied（可 NULL）回填写后投影里的真值（钳位结果）。 */
static bool slot_write(uint8_t index, uint8_t slot, int16_t value, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || slot >= PARAM_PAGE_SLOTS) return false;
    app_state_t st;
    state_get_copy(&st);
    if (page_locked(pg, &st)) return false;
    const param_slot_t *s = &pg->slots[slot];
    if (s->id == 0xFF) return false;

    edit_cmd_t cmd = {0};
    if (s->scope == SLOOP_DESC_GLOBAL) {
        cmd.type = CMD_SET_GLOBAL;
        cmd.a = s->id;
        cmd.v = (uint16_t)value;
    } else if (s->scope == SLOOP_DESC_TRACK || s->scope == SLOOP_DESC_ENGINE) {
        cmd.type = CMD_SET_PARAM;
        cmd.a = st.core.selected_track;
        cmd.b = slot_abs_id(pg, slot);
        cmd.v = (uint16_t)value;
    } else {
        return false;
    }
    state_apply(&cmd);
    if (applied != NULL) {
        state_get_copy(&st);
        int16_t v;
        *applied = slot_value(pg, slot, &st, &v) ? v : value;
    }
    return true;
}

static void refresh(uint8_t index)
{
    app_state_t st;
    state_get_copy(&st);
    page_params_update(index, &st);
}

/* ---------------------------------------------------------------- 上下文/焦点 --- */
static void update_header(uint8_t index, const param_page_t *pg, const app_state_t *st, uint8_t track,
                          bool locked)
{
    page_ui_t *ui = &s_ui[index];
    if (ui->header == NULL) return;
    char text[64];
    uint8_t scope = pg->slots[0].scope;
    if (scope == SLOOP_DESC_GLOBAL) {
        snprintf(text, sizeof(text), "%s", pg->title);
    } else if (scope == SLOOP_DESC_TRACK) {
        snprintf(text, sizeof(text), "%s · TRACK %u", pg->title, (unsigned)(track + 1));
    } else if (locked && track >= 3u) {
        snprintf(text, sizeof(text), "%s · TRACK %u (DRUM) — N/A", pg->title, (unsigned)(track + 1));
    } else if (locked) {
        snprintf(text, sizeof(text), "%s · TRACK %u USES ENG %u — N/A", pg->title,
                 (unsigned)(track + 1), (unsigned)st->core.trk[track].engine);
    } else {
        snprintf(text, sizeof(text), "%s · TRACK %u", pg->title, (unsigned)(track + 1));
    }
    lv_label_set_text(ui->header, text);
    lv_obj_set_style_text_color(ui->header, lv_color_hex(locked ? UI_ERROR : UI_TEXT_DIM), 0);
}

static void apply_focus(uint8_t index, uint8_t focus)
{
    page_ui_t *ui = &s_ui[index];
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        knob_set_active(&ui->knobs[slot], slot == focus);
    }
    ui->focus_shown = focus;
}

/* ---------------------------------------------------------------- 动作函数 --- */
/* 焦点槽 ±1（控件回调与 TAP 的共同路径）。返回 false = 不可编辑/无值。 */
static bool param_step(uint8_t index, int8_t delta, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL) return false;
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = focus_get(index);
    int16_t v;
    if (!slot_value(pg, slot, &st, &v) || page_locked(pg, &st)) return false;
    return slot_write(index, slot, (int16_t)(v + delta), applied);
}

/* 旋钮拖动（LVGL 上下文）：值变化 → 状态层 → 立即刷新本页 */
static void knob_change_cb(knob_t *k, int16_t value)
{
    uint8_t index = ctx_page(k->user);
    slot_write(index, ctx_slot(k->user), value, NULL);
    refresh(index);
}

/* 旋钮按下/文本点击：设焦点（输入任务下一拍按 s_focus 刷新样式） */
static void knob_focus_cb(knob_t *k)
{
    uint8_t index = ctx_page(k->user);
    uint8_t slot = ctx_slot(k->user);
    focus_set(index, slot);
    apply_focus(index, slot);
}

static void btn_cb(lv_event_t *e)
{
    /* user_data = (页下标 << 4 | 方向位)，方向位 1 = +1、0 = -1（同布局；旋钮上下文另有 +1 偏移） */
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t index = (uint8_t)(ctx >> 4);
    int8_t delta = (ctx & 1u) != 0 ? 1 : -1;
    param_step(index, delta, NULL);
    refresh(index);
}

/* ---------------------------------------------------------------- bank 存取行 --- */
/* 目标槽 ±1（环绕 0..26）：只改 UI 局部态，不碰存储/引擎。 */
static bool bank_slot_step(uint8_t index, int8_t delta)
{
    if (!bank_row_present(page_layout_get(index))) return false;
    int32_t v = (int32_t)s_bank_slot + delta;
    while (v < 0) v += SLOOP_STORE_BANK_SLOTS;
    while (v >= SLOOP_STORE_BANK_SLOTS) v -= SLOOP_STORE_BANK_SLOTS;
    s_bank_slot = (uint8_t)v;
    refresh(index);
    return true;
}

/* bank 动作（控件回调与 TAP 的共同路径）：act = 0 SAVE（CMD_BANK_PUT，选中轨的 live
 * patch）/ 2 ERASE（CMD_BANK_ERASE）。锁定页（选中轨引擎 ≠ FM6，含鼓轨）直接拒绝：
 * 不发命令、不动存储，状态行改显示 `NOT FM6`（本地状态）。其余结果写
 * app_state_t.store_status（播放中/未就绪 = 3 …），状态行由 update() 映射，console
 * `TAP` 回显走同一映射。 */
static void bank_action(uint8_t index, uint8_t act)
{
    const param_page_t *pg = page_layout_get(index);
    if (!bank_row_present(pg)) return;
    s_bank_act = act;
    app_state_t st;
    state_get_copy(&st);
    if (page_locked(pg, &st)) {
        s_bank_refused = 1;
        refresh(index);
        return;
    }
    s_bank_refused = 0;
    edit_cmd_t cmd = {0};
    cmd.type = act == 0 ? CMD_BANK_PUT : CMD_BANK_ERASE;
    cmd.a = s_bank_slot;
    state_apply(&cmd);
    refresh(index);
}

static void bank_cb(lv_event_t *e)
{
    /* user_data = (页下标 << 4 | 操作位)：0 = 槽 −1、1 = 槽 +1、2 = SAVE、3 = ERASE */
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t index = (uint8_t)(ctx >> 4);
    uint8_t op = (uint8_t)(ctx & 0x0Fu);
    if (op <= 1u) bank_slot_step(index, op == 1u ? 1 : -1);
    else bank_action(index, op == 2u ? 0u : 2u);
}

/* ---------------------------------------------------------------- 槽管理行 --- */
/* 选中轨的 SET 宏（P_E0）→ USR 槽号；不在 USR1..USR4（SET 8..11）返回 0xFF。 */
static uint8_t sample_selected_slot(const app_state_t *st)
{
    int16_t set = st->p[PARAM_ENGINE_BASE];
    if (set < (int16_t)SAMPLE_USR_SET_BASE ||
        set >= (int16_t)(SAMPLE_USR_SET_BASE + SLOOP_STORE_SAMPLES)) {
        return 0xFF;
    }
    return (uint8_t)(set - SAMPLE_USR_SET_BASE);
}

/* 槽信息（文档化只读例外）：force 或 synth_status/SET 变化、或距上次 ≥1s 才重读。
 * info 会对每个已用槽算 ≤80KiB 数据 CRC，是全页最贵的调用，绝不能每拍跑。 */
static void sample_info_poll(const app_state_t *st, bool force)
{
    uint16_t key = (uint16_t)(((uint16_t)st->synth_status << 8) |
                              (uint8_t)st->p[PARAM_ENGINE_BASE]);
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (!force && s_sample_info_valid && key == s_sample_info_key &&
        (uint32_t)(now - s_sample_info_ms) < 1000u) {
        return;
    }
    for (uint32_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
        sloop_store_sample_info_t info;
        memset(&info, 0, sizeof(info));
        if (sloop_store_sample_info(k, &info) != ESP_OK) memset(&info, 0, sizeof(info));
        s_sample_info[k] = info;
    }
    s_sample_info_key = key;
    s_sample_info_ms = now;
    s_sample_info_valid = true;
}

static uint32_t sig_mix(uint32_t h, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        h ^= (v >> (i * 8u)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

/* 整行渲染脏检查键：选择槽 + 动作/状态 + ready + 四个槽的 (used/zones/KB/name)。 */
static uint32_t sample_row_sig(const app_state_t *st)
{
    uint32_t h = 2166136261u;
    h = sig_mix(h, sample_selected_slot(st));
    h = sig_mix(h, s_sample_act);
    h = sig_mix(h, (uint32_t)s_sample_status);
    h = sig_mix(h, sloop_store_ready() ? 1u : 0u);
    for (uint32_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
        const sloop_store_sample_info_t *in = &s_sample_info[k];
        h = sig_mix(h, in->used);
        h = sig_mix(h, in->zones);
        h = sig_mix(h, in->data_kb);
        for (unsigned i = 0; i < 8u; i++) h = sig_mix(h, (uint8_t)in->name[i]);
    }
    return h;
}

/* 渲染整行（4 槽文本/颜色 + 状态行）。选中槽（SET 指向的 USR）用强调色。 */
static void sample_render(uint8_t index, const app_state_t *st)
{
    page_ui_t *ui = &s_ui[index];
    uint8_t sel = sample_selected_slot(st);
    for (uint32_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
        const sloop_store_sample_info_t *in = &s_sample_info[k];
        char text[48];
        if (in->used) {
            snprintf(text, sizeof(text), "U%u USED\n%s z%u %uK", (unsigned)(k + 1u), in->name,
                     (unsigned)in->zones, (unsigned)in->data_kb);
        } else {
            snprintf(text, sizeof(text), "U%u EMPTY", (unsigned)(k + 1u));
        }
        lv_label_set_text(ui->sample_label[k], text);
        lv_obj_set_style_text_color(
            ui->sample_label[k],
            lv_color_hex(k == sel ? UI_ACCENT : (in->used ? UI_TEXT : UI_TEXT_DIM)), 0);
    }
    char text[32];
    ui_sample_status_text(text, sizeof(text), (uint8_t)s_sample_status);
    lv_label_set_text(ui->sample_status, text);
    lv_obj_set_style_text_color(ui->sample_status,
                                lv_color_hex(ui_sample_status_color((uint8_t)s_sample_status)), 0);
}

/* 槽选中（控件回调与 TAP 的共同路径）：把 SET 宏切到 SMP_NSETS + k（8..11）经
 * CMD_SET_PARAM 下发。锁定页（选中轨引擎 ≠ SAMPLE，含鼓轨）不下发、不改状态行
 * （与旋钮页同一口径；槽列表仍只读显示）。 */
static bool sample_select(uint8_t index, uint8_t slot)
{
    if (slot >= SLOOP_STORE_SAMPLES) return false;
    if (!slot_write(index, 0, (int16_t)(SAMPLE_USR_SET_BASE + slot), NULL)) return false;
    s_sample_act = 0u;
    s_sample_status = UI_SAMPLE_STATUS_SELECT_OK;
    app_state_t st;
    state_get_copy(&st);
    sample_info_poll(&st, true);
    refresh(index);
    return true;
}

/* ERASE（控件回调与 TAP 的共同路径）：只在当前 SET ∈ USR1..USR4 时发 CMD_SAMPLE_ERASE
 * （槽号 = SET − 8），否则 `NOT USR` 且不下发命令。结果按 synth_status 映射成状态行
 * （BLOCKED = 走带中；ERROR + 未就绪 = NO STORAGE）。
 * 锁定页（选中轨引擎 ≠ SAMPLE）**必须**先拒绝：否则会把别的引擎的 E0 值（恰好落在
 * 8..11）当成 SET 而擦掉一个用户槽（槽不存在于该轨的声音里，用户意图也无从成立）。 */
static void sample_erase(uint8_t index)
{
    const param_page_t *pg = page_layout_get(index);
    app_state_t st;
    state_get_copy(&st);
    s_sample_act = 2u;
    if (page_locked(pg, &st)) {
        s_sample_status = UI_SAMPLE_STATUS_NOT_USR;
        refresh(index);
        return;
    }
    uint8_t slot = sample_selected_slot(&st);
    if (slot == 0xFFu) {
        s_sample_status = UI_SAMPLE_STATUS_NOT_USR;
        refresh(index);
        return;
    }
    edit_cmd_t cmd = {0};
    cmd.type = CMD_SAMPLE_ERASE;
    cmd.a = slot;
    state_apply(&cmd);
    state_get_copy(&st);
    switch (st.synth_status) {
    case APP_SYNTH_STATUS_OK: s_sample_status = UI_SAMPLE_STATUS_ERASE_OK; break;
    case APP_SYNTH_STATUS_BLOCKED: s_sample_status = UI_SAMPLE_STATUS_PLAYING; break;
    case APP_SYNTH_STATUS_ERROR:
        s_sample_status =
            sloop_store_ready() ? UI_SAMPLE_STATUS_ERROR : UI_SAMPLE_STATUS_NO_STORAGE;
        break;
    default: s_sample_status = UI_SAMPLE_STATUS_ERROR; break;
    }
    sample_info_poll(&st, true);
    refresh(index);
}

static void sample_cb(lv_event_t *e)
{
    /* user_data = (页下标 << 4 | 操作位)：0..3 = 槽选中、4 = ERASE */
    uintptr_t ctx = (uintptr_t)lv_event_get_user_data(e);
    uint8_t index = (uint8_t)(ctx >> 4);
    uint8_t op = (uint8_t)(ctx & 0x0Fu);
    if (op < SLOOP_STORE_SAMPLES) (void)sample_select(index, op);
    else sample_erase(index);
}

/* ---------------------------------------------------------------- 键盘路由 --- */
/* 当前页在布局表里的下标；非参数页/UI 未启动返回 -1（不取任何锁） */
static int current_index(void)
{
    return page_layout_index(sloop_ui_current_page());
}

bool sloop_ui_param_page_active(void)
{
    return current_index() >= 0;
}

void sloop_ui_param_focus_move(int8_t delta)
{
    int idx = current_index();
    if (idx < 0 || delta == 0) return;
    const param_page_t *pg = page_layout_get((uint8_t)idx);
    uint8_t f = focus_get((uint8_t)idx);
    for (uint8_t n = 0; n < PARAM_PAGE_SLOTS; n++) {
        f = (uint8_t)((f + PARAM_PAGE_SLOTS + delta) % PARAM_PAGE_SLOTS);
        sloop_desc_t d;
        if (slot_desc(pg, f, &d)) break;
    }
    focus_set((uint8_t)idx, f);
}

void sloop_ui_param_adjust(int8_t delta)
{
    int idx = current_index();
    if (idx < 0 || delta == 0) return;
    param_step((uint8_t)idx, delta, NULL); /* 状态层写；控件由 30ms UI 定时器刷新 */
}

/* ---------------------------------------------------------------- 调试注入 --- */
/* console `UI KNOB`：向当前参数页的第 slot 槽旋钮派发合成 PRESSED/PRESSING
 * （dx/dy 相对旋钮中心）。调用方已持 LVGL 锁（ui.c 的 sloop_ui_test_knob）。
 * ESP_ERR_NOT_FOUND = 事件未被接受（环带外/无锚点）；INVALID_ARG = 非参数页或
 * 槽位无控件；INVALID_STATE = 页未建。*value 回填旋钮当前显示值。 */
esp_err_t page_params_test_knob(uint8_t slot, int32_t dx, int32_t dy, bool press, int16_t *value)
{
    int idx = current_index();
    if (idx < 0 || slot >= PARAM_PAGE_SLOTS) return ESP_ERR_INVALID_ARG;
    page_ui_t *ui = &s_ui[idx];
    if (ui->header == NULL) return ESP_ERR_INVALID_STATE;
    if (!ui->present[slot]) return ESP_ERR_INVALID_ARG;
    /* 引擎页不适用（选中轨引擎与页声明不符/鼓轨）：旋钮显示 -- 且不可编辑。
     * 用独立错误码让 console 回 `ignored (locked)`（与环带外的 NOT_FOUND 区分）。 */
    app_state_t st;
    state_get_copy(&st);
    if (page_locked(page_layout_get((uint8_t)idx), &st)) return ESP_ERR_NOT_ALLOWED;
    knob_t *k = &ui->knobs[slot];
    bool accepted = press ? knob_test_press(k, dx, dy) : knob_test_drag(k, dx, dy);
    if (value != NULL) *value = k->value;
    return accepted ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------- 构建 --- */
/* ± 按钮样式（共享 static style：不给每个按钮建局部样式，池预算纪律同 knob.c） */
static lv_style_t s_st_btn;
static bool s_btn_style_ready;

static void btn_style_init(void)
{
    if (s_btn_style_ready) return;
    s_btn_style_ready = true;
    lv_style_init(&s_st_btn);
    lv_style_set_text_align(&s_st_btn, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_st_btn, &lv_font_montserrat_28);
    lv_style_set_text_color(&s_st_btn, lv_color_hex(UI_TEXT));
    lv_style_set_bg_color(&s_st_btn, lv_color_hex(UI_PANEL_HI));
    lv_style_set_bg_opa(&s_st_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_st_btn, 10);
    lv_style_set_pad_top(&s_st_btn, 4);
}

static lv_obj_t *make_button(lv_obj_t *parent, int16_t x, int16_t y, const char *text,
                             lv_event_cb_t cb, void *user)
{
    btn_style_init();
    lv_obj_t *btn = lv_label_create(parent);
    if (btn == NULL) return NULL;
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_add_style(btn, &s_st_btn, 0);
    lv_obj_set_clickable(btn, true);
    lv_label_set_text(btn, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

/* bank 行按钮样式（基样式 20pt/HI 底 + 两个只设颜色的覆盖样式）：静态共享，只 init
 * 一次（组作用域下页面反复销毁/重建，重设 lv_style 会漏 props 数组）。 */
static lv_style_t s_st_bank;
static lv_style_t s_st_bank_save;
static lv_style_t s_st_bank_erase;

static void bank_style_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    lv_style_init(&s_st_bank);
    lv_style_set_text_align(&s_st_bank, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_st_bank, &lv_font_montserrat_20);
    lv_style_set_text_color(&s_st_bank, lv_color_hex(UI_TEXT));
    lv_style_set_bg_color(&s_st_bank, lv_color_hex(UI_PANEL_HI));
    lv_style_set_bg_opa(&s_st_bank, LV_OPA_COVER);
    lv_style_set_radius(&s_st_bank, 10);
    lv_style_set_pad_top(&s_st_bank, 11);

    lv_style_init(&s_st_bank_save);
    lv_style_set_bg_color(&s_st_bank_save, lv_color_hex(ui_track_color(1)));
    lv_style_init(&s_st_bank_erase);
    lv_style_set_bg_color(&s_st_bank_erase, lv_color_hex(ui_track_color(3)));
}

/* 底部行按钮（BANK 行与 SAMPLE 槽管理行共用：20pt / 44px / 10 圆角，accent 只覆盖
 * 底色；回调由调用方给出——两行的动作函数不同）。 */
static lv_obj_t *make_row_button(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
                                 const char *text, lv_style_t *accent, lv_event_cb_t cb,
                                 void *user)
{
    bank_style_init();
    lv_obj_t *btn = lv_label_create(parent);
    if (btn == NULL) return NULL;
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, BANK_ROW_H);
    lv_obj_add_style(btn, &s_st_bank, 0);
    if (accent != NULL) lv_obj_add_style(btn, accent, 0); /* 后加的样式优先（只覆盖颜色） */
    lv_obj_set_clickable(btn, true);
    lv_label_set_text(btn, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

/* 行内静态/动态文本（BANK 标签、目标槽、状态行、SAMPLE 状态行）：20pt，行内垂直
 * 居中（pad_top 10）。 */
static lv_obj_t *make_row_label(lv_obj_t *parent, int16_t x, int16_t y, int16_t w,
                                uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) return NULL;
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, BANK_ROW_H);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_pad_top(label, 10, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *build_bank_row(uint8_t index, lv_obj_t *parent)
{
    page_ui_t *ui = &s_ui[index];
    s_bank_slot = 0;   /* 页面重建：目标槽归零（与 FILE 页选中槽同口径） */
    s_bank_act = 0xFF; /* 尚无动作：状态行留空 */
    s_bank_refused = 0;
    ui->bank_shown_slot = 0xFF;
    ui->bank_status_key = 0xFFFFFFFFu;

    ui->bank_label = make_row_label(parent, BANK_LABEL_X, BANK_ROW_Y, BANK_LABEL_W, UI_TEXT_DIM,
                                    LV_TEXT_ALIGN_LEFT);
    UI_BUILD_CHECK(ui->bank_label);
    lv_label_set_text(ui->bank_label, "BANK");

    ui->bank_value = make_row_label(parent, BANK_VAL_X, BANK_ROW_Y, BANK_VAL_W, UI_ACCENT,
                                    LV_TEXT_ALIGN_CENTER);
    UI_BUILD_CHECK(ui->bank_value);

    const uintptr_t ctx = (uintptr_t)((unsigned)index << 4);
    ui->bank_dec = make_row_button(parent, BANK_DEC_X, BANK_ROW_Y, BANK_SLOT_W, "-", NULL,
                                   bank_cb, (void *)(ctx | 0u));
    UI_BUILD_CHECK(ui->bank_dec);
    ui->bank_inc = make_row_button(parent, BANK_INC_X, BANK_ROW_Y, BANK_SLOT_W, "+", NULL,
                                   bank_cb, (void *)(ctx | 1u));
    UI_BUILD_CHECK(ui->bank_inc);
    ui->bank_save = make_row_button(parent, BANK_SAVE_X, BANK_ROW_Y, BANK_ACT_W, "SAVE",
                                    &s_st_bank_save, bank_cb, (void *)(ctx | 2u));
    UI_BUILD_CHECK(ui->bank_save);
    ui->bank_erase = make_row_button(parent, BANK_ERASE_X, BANK_ROW_Y, BANK_ACT_W, "ERASE",
                                     &s_st_bank_erase, bank_cb, (void *)(ctx | 3u));
    UI_BUILD_CHECK(ui->bank_erase);

    ui->bank_status = make_row_label(parent, BANK_STATUS_X, BANK_ROW_Y, BANK_STATUS_W, UI_TEXT_DIM,
                                     LV_TEXT_ALIGN_LEFT);
    UI_BUILD_CHECK(ui->bank_status);
    return parent;
}

/* 槽 label：单对象可点击（16pt 两行：`U1 USED` / `PIANO z1 12K`；`EMPTY` 一行），
 * 200×44、左对齐、CLIP；选中槽的强调色由 update 的整行渲染落地。 */
static lv_obj_t *make_sample_slot_label(lv_obj_t *parent, uint8_t k, void *user)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) return NULL;
    lv_obj_set_pos(label, SAMPLE_SLOT_X0 + (int16_t)k * SAMPLE_SLOT_PITCH, SAMPLE_ROW_Y);
    lv_obj_set_size(label, SAMPLE_SLOT_W, SAMPLE_ROW_H);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(label, 2, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_clickable(label, true);
    lv_obj_add_event_cb(label, sample_cb, LV_EVENT_CLICKED, user);
    lv_label_set_text(label, "");
    return label;
}

/* 槽管理行（仅 SAMPLE 页）：选择状态归零 + 建 4 个槽 label（回调 ctx = index<<4|k）、
 * ERASE chip（ctx = index<<4|4）与状态行；建页时读一次槽信息（之后 update 至多 1s
 * 一次）。首帧文本由紧随的 update 渲染（s_sample_row_sig 强制失效）。 */
static lv_obj_t *build_sample_row(uint8_t index, lv_obj_t *parent)
{
    page_ui_t *ui = &s_ui[index];
    s_sample_act = 0xFF;
    s_sample_status = UI_SAMPLE_STATUS_NONE;
    s_sample_row_sig = 0xFFFFFFFFu;
    app_state_t st;
    state_get_copy(&st);
    sample_info_poll(&st, true);

    for (uint8_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
        ui->sample_label[k] = make_sample_slot_label(
            parent, k, (void *)(uintptr_t)(((unsigned)index << 4) | k));
        UI_BUILD_CHECK(ui->sample_label[k]);
    }
    const uintptr_t ctx = (uintptr_t)((unsigned)index << 4);
    ui->sample_erase = make_row_button(parent, SAMPLE_ERASE_X, SAMPLE_ROW_Y, BANK_ACT_W, "ERASE",
                                       &s_st_bank_erase, sample_cb, (void *)(ctx | 4u));
    UI_BUILD_CHECK(ui->sample_erase);
    ui->sample_status = make_row_label(parent, SAMPLE_STATUS_X, SAMPLE_ROW_Y, SAMPLE_STATUS_W,
                                       UI_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    UI_BUILD_CHECK(ui->sample_status);
    return parent;
}

lv_obj_t *page_params_build(uint8_t index, lv_obj_t *parent)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || parent == NULL) return NULL;
    page_ui_t *ui = &s_ui[index];
    /* 懒建重试时控件是全新的：复位上下文/焦点缓存（焦点槽本身保留） */
    memset(ui, 0, sizeof(*ui));
    ui->shown_locked = -1;
    ui->focus_shown = 0xFF;

    ui->header = lv_label_create(parent);
    UI_BUILD_CHECK(ui->header);
    lv_obj_set_pos(ui->header, HEADER_X, HEADER_Y);
    lv_obj_set_size(ui->header, 1000, 30);
    lv_obj_set_style_text_font(ui->header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->header, lv_color_hex(UI_TEXT_DIM), 0);
    lv_label_set_long_mode(ui->header, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui->header, "");

    /* ± 按钮：共用同一回调，user_data 编码 (页下标 << 4 | 方向位)；方向位 0=减 1=加。
     * 旋钮上下文另有 +1 偏移（见 KNOB_CTX），按钮不需要（解码不经过 NULL 哨兵）。 */
    ui->btn_dec = make_button(parent, BTN_DEC_X, BTN_Y, "-", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 0u));
    UI_BUILD_CHECK(ui->btn_dec);
    ui->btn_inc = make_button(parent, BTN_INC_X, BTN_Y, "+", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 1u));
    UI_BUILD_CHECK(ui->btn_inc);

    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        sloop_desc_t desc;
        if (!slot_desc(pg, slot, &desc)) continue;
        knob_t *k = &ui->knobs[slot];
        int16_t cx = (int16_t)(GRID_X0 + (slot % 4u) * CELL_PITCH);
        int16_t cy = (int16_t)(GRID_Y0 + (slot / 4u) * ROW_PITCH);
        if (!knob_create(k, parent, cx, cy, &desc, KNOB_CTX(index, slot), knob_change_cb,
                         knob_focus_cb)) {
            return NULL;
        }
        ui->present[slot] = true;
    }

    /* 焦点槽落在第一个有控件的槽上（重试后槽集合可能变化） */
    uint8_t f = focus_get(index);
    if (!ui->present[f]) {
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (ui->present[slot]) {
                focus_set(index, slot);
                break;
            }
        }
    }

    /* bank 存取行（仅 FM6 页）：旋钮之后建，几何在页面区底部（不覆盖旋钮环） */
    if (bank_row_present(pg) && build_bank_row(index, parent) == NULL) return NULL;
    /* 槽管理行（仅 SAMPLE 页）：同一底部带，页内唯一 */
    if (sample_row_present(pg) && build_sample_row(index, parent) == NULL) return NULL;
    return parent;
}

void page_params_update(uint8_t index, const app_state_t *st)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || st == NULL) return;
    page_ui_t *ui = &s_ui[index];
    if (ui->header == NULL) return; /* 防御：ui.c 只刷新已建页 */

    uint8_t track = st->core.selected_track < APP_STATE_TRACKS ? st->core.selected_track : 0;
    bool locked = page_locked(pg, st);
    /* 解锁（换轨/换引擎/工程载入）后清掉上次的 NOT FM6：拒绝是「上一次动作」的结果，
     * 不能在本页未再动作时跨上下文残留。 */
    if (!locked && s_bank_refused) s_bank_refused = 0;
    uint8_t engine = page_engine(pg);
    bool esync = st->p[PARAM_TP_ESYNC] != 0;
    if (!ui->ctx_valid || ui->shown_track != track || ui->shown_engine != engine ||
        ui->shown_locked != (int8_t)locked || ui->shown_esync != (int8_t)esync) {
        ui->ctx_valid = true;
        ui->shown_track = track;
        ui->shown_engine = engine;
        ui->shown_locked = (int8_t)locked;
        ui->shown_esync = (int8_t)esync;
        update_header(index, pg, st, track, locked);
        /* ENV SYNC 翻转会换 ATK 槽的 label/fmt（ms ↔ 音符值）：重新解析并重绘，
         * 不要求用户重进页面（PARAM 59 注入/FM-1 同步都能触发）。 */
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (!ui->present[slot]) continue;
            sloop_desc_t d;
            if (!slot_desc(pg, slot, &d)) continue;
            knob_t *k = &ui->knobs[slot];
            if (d.label == k->desc.label && d.fmt == k->desc.fmt && d.unit == k->desc.unit) continue;
            k->desc = d;
            lv_obj_invalidate(k->obj);
        }
    }

    uint8_t focus = focus_get(index);
    if (ui->focus_shown != focus) apply_focus(index, focus);

    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        knob_t *k = &ui->knobs[slot];
        if (locked && pg->slots[slot].scope == SLOOP_DESC_ENGINE) {
            if (!k->shown || !k->locked) knob_show_locked(k);
            continue;
        }
        int16_t v;
        if (!slot_value(pg, slot, st, &v)) continue;
        if (!k->shown || k->locked || k->value != v) knob_show(k, v);
    }

    /* bank 行（仅 FM6）：目标槽 `B<n+1>` + 状态行（最后一次 bank 动作 × store_status，
     * 与 FILE 页同一映射）。脏检查：槽号变化写槽文本，四元组
     * (act, status, ready, refused) 变化写状态行——30ms 一拍无条件重设文本会让 LVGL
     * 持续失效重绘。 */
    if (bank_row_present(pg) && ui->bank_label != NULL) {
        if (ui->bank_shown_slot != s_bank_slot) {
            char text[8];
            snprintf(text, sizeof(text), "B%u", (unsigned)(s_bank_slot + 1u));
            lv_label_set_text(ui->bank_value, text);
            ui->bank_shown_slot = s_bank_slot;
        }
        bool ready = sloop_store_ready();
        uint32_t key = (uint32_t)s_bank_act | ((uint32_t)st->store_status << 8) |
                       ((uint32_t)(ready ? 1u : 0u) << 16) |
                       ((uint32_t)s_bank_refused << 17);
        if (key != ui->bank_status_key) {
            ui->bank_status_key = key;
            if (s_bank_refused) {
                /* 锁定页的本地拒绝：与存储层结果无关，红字提示 */
                lv_label_set_text(ui->bank_status, "NOT FM6");
                lv_obj_set_style_text_color(ui->bank_status, lv_color_hex(UI_ERROR), 0);
            } else if (s_bank_act > 2u) {
                lv_label_set_text(ui->bank_status, ""); /* 尚无动作：不显示过期的结果 */
            } else {
                char text[32];
                ui_file_status_text(text, sizeof(text), s_bank_act, st->store_status, ready);
                lv_label_set_text(ui->bank_status, text);
                lv_obj_set_style_text_color(
                    ui->bank_status,
                    lv_color_hex(ui_file_status_color(s_bank_act, st->store_status, ready)), 0);
            }
        }
    }

    /* 槽管理行（仅 SAMPLE）：槽信息至多 ~1s 读一次（或 synth_status/SET 变化时），
     * 然后按签名脏检查整行重渲染（30ms 一拍无条件重设 4 段文本会让 LVGL 持续重绘）。 */
    if (sample_row_present(pg) && ui->sample_label[0] != NULL) {
        sample_info_poll(st, false);
        uint32_t sig = sample_row_sig(st);
        if (sig != s_sample_row_sig) {
            s_sample_row_sig = sig;
            sample_render(index, st);
        }
    }
}

/* ---------------------------------------------------------------- TAP --- */
static bool rect_hit(int16_t x, int16_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void param_tap_button(uint8_t index, int8_t delta, sloop_tap_result_t *out)
{
    const param_page_t *pg = page_layout_get(index);
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = focus_get(index);
    int16_t applied = 0;
    bool ok = !page_locked(pg, &st) && param_step(index, delta, &applied);
    if (out != NULL) {
        out->a = slot;
        out->b = ok ? slot_abs_id(pg, slot) : 0u;
        out->v = ok ? applied : 0;
        out->d = delta;
        out->kind = ok ? SLOOP_TAP_PARAM_STEP : SLOOP_TAP_PARAM_LOCKED;
    }
    refresh(index);
}

static void bank_tap_slot(uint8_t index, int8_t delta, sloop_tap_result_t *out)
{
    if (!bank_slot_step(index, delta)) return;
    if (out != NULL) {
        out->kind = SLOOP_TAP_BANK_SLOT;
        out->a = APP_PAGE_FM6;
        out->b = s_bank_slot;
        out->d = delta;
    }
}

static void bank_tap_action(uint8_t index, uint8_t act, sloop_tap_result_t *out)
{
    bank_action(index, act);
    if (out != NULL) {
        out->kind = act == 0u ? SLOOP_TAP_BANK_STORE : SLOOP_TAP_BANK_ERASE;
        out->a = APP_PAGE_FM6;
        out->b = s_bank_slot;
        out->v = act;
    }
}

static void sample_tap_slot(uint8_t index, uint8_t slot, sloop_tap_result_t *out)
{
    (void)sample_select(index, slot); /* 锁定页不下发；回显仍表达「选中该槽」的意图 */
    if (out != NULL) {
        out->kind = SLOOP_TAP_SAMPLE_SLOT;
        out->a = APP_PAGE_SAMPLE;
        out->b = slot;
    }
}

static void sample_tap_action(uint8_t index, sloop_tap_result_t *out)
{
    /* 回显的槽号 = 本次真正作用的槽；锁定页/非 USR 没有槽（console 打 `slot=-`） */
    const param_page_t *pg = page_layout_get(index);
    app_state_t st;
    state_get_copy(&st);
    uint8_t slot = page_locked(pg, &st) ? 0xFFu : sample_selected_slot(&st);
    sample_erase(index);
    if (out != NULL) {
        out->kind = SLOOP_TAP_SAMPLE_ACTION;
        out->a = APP_PAGE_SAMPLE;
        out->b = slot;
        out->v = 2; /* ERASE（与 FILE/BANK 的动作编号一致） */
    }
}

bool page_params_tap(uint8_t index, int16_t x, int16_t y, sloop_tap_result_t *out)
{
    const param_page_t *pg = page_layout_get(index);
    page_ui_t *ui = &s_ui[index];
    if (pg == NULL || ui->header == NULL) return false;

    if (rect_hit(x, y, BTN_DEC_X, BTN_Y, BTN_W, BTN_H)) {
        param_tap_button(index, -1, out);
        return true;
    }
    if (rect_hit(x, y, BTN_INC_X, BTN_Y, BTN_W, BTN_H)) {
        param_tap_button(index, +1, out);
        return true;
    }
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        int16_t cx = (int16_t)(GRID_X0 + (slot % 4u) * CELL_PITCH);
        int16_t cy = (int16_t)(GRID_Y0 + (slot / 4u) * ROW_PITCH);
        if (!rect_hit(x, y, cx + KNOB_OBJ_DX, cy + KNOB_OBJ_DY, KNOB_OBJ_SIZE, KNOB_OBJ_SIZE)) {
            continue;
        }
        focus_set(index, slot);
        apply_focus(index, slot);
        if (out != NULL) {
            out->kind = SLOOP_TAP_PARAM_FOCUS;
            out->a = slot;
        }
        return true;
    }

    /* 槽管理行（仅 SAMPLE；矩形与建控件同源）：槽 = 选 SET，ERASE = 擦当前 SET 指向的槽 */
    if (sample_row_present(pg) && ui->sample_label[0] != NULL) {
        for (uint8_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
            if (rect_hit(x, y, SAMPLE_SLOT_X0 + (int16_t)k * SAMPLE_SLOT_PITCH, SAMPLE_ROW_Y,
                         SAMPLE_SLOT_W, SAMPLE_ROW_H)) {
                sample_tap_slot(index, k, out);
                return true;
            }
        }
        if (rect_hit(x, y, SAMPLE_ERASE_X, SAMPLE_ROW_Y, BANK_ACT_W, SAMPLE_ROW_H)) {
            sample_tap_action(index, out);
            return true;
        }
    }

    /* bank 行（仅 FM6；矩形的建控件/命中同源）：槽 ± 只改目标槽，SAVE/ERASE 发状态层命令 */
    if (bank_row_present(pg) && ui->bank_label != NULL) {
        if (rect_hit(x, y, BANK_DEC_X, BANK_ROW_Y, BANK_SLOT_W, BANK_ROW_H)) {
            bank_tap_slot(index, -1, out);
            return true;
        }
        if (rect_hit(x, y, BANK_INC_X, BANK_ROW_Y, BANK_SLOT_W, BANK_ROW_H)) {
            bank_tap_slot(index, +1, out);
            return true;
        }
        if (rect_hit(x, y, BANK_SAVE_X, BANK_ROW_Y, BANK_ACT_W, BANK_ROW_H)) {
            bank_tap_action(index, 0, out);
            return true;
        }
        if (rect_hit(x, y, BANK_ERASE_X, BANK_ROW_Y, BANK_ACT_W, BANK_ROW_H)) {
            bank_tap_action(index, 2, out);
            return true;
        }
    }
    return false;
}

/* 组作用域销毁页面时由 ui.c 调用：控件已随页面删除，本模块的控件指针必须置空
 * （否则 `page_params_bank_status_text` 的「行未建」契约不成立，还留着悬垂指针）。 */
void page_params_forget(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT) return;
    memset(&s_ui[index], 0, sizeof s_ui[index]);
    s_bank_refused = 0;
    s_sample_act = 0xFF;
    s_sample_status = UI_SAMPLE_STATUS_NONE;
    s_sample_info_valid = false;
    s_sample_row_sig = 0xFFFFFFFFu;
}

const char *page_params_bank_status_text(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT || !bank_row_present(page_layout_get(index))) return NULL;
    /* 行未建（页未构建/已随源组销毁）时没有可回显的页面状态 */
    if (s_ui[index].bank_label == NULL) return NULL;
    if (s_bank_refused) return "NOT FM6";
    app_state_t st;
    state_get_copy(&st);
    return ui_file_status_text(s_bank_echo, sizeof(s_bank_echo), s_bank_act, st.store_status,
                               sloop_store_ready());
}

const char *page_params_sample_status_text(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT || !sample_row_present(page_layout_get(index))) return NULL;
    /* 行未建（页未构建/已随源组销毁）时没有可回显的页面状态 */
    if (s_ui[index].sample_label[0] == NULL) return NULL;
    return ui_sample_status_text(s_sample_echo, sizeof(s_sample_echo), (uint8_t)s_sample_status);
}
