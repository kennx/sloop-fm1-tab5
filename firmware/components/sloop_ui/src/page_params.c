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
 * SOUND 页（P4-F Task 5，取代原 ANALOG + FM6 两页；spec §4 / A10）：主体 = **选中轨引擎**的
 * 8 槽（布局表的 TRACK scope P_E0..P_E7，描述符经 sloop_core_desc_track_live 取该轨引擎的
 * 钩子——LOFI/NOISE/PHYS 的标签随模式换），8 个旋钮对象常驻、按上下文显隐。页眉两行是
 * **三区对象**（同 DSYN 的 LANE / FM6P 的 BANK）：`ENGINE − 名 +` / `PRESET − 名 +`，
 * 左右 42px 是 ∓1 区（环绕）、中段是标签（TAP miss）；名字/下标/预置数全部读状态层的
 * app_state_t.sound（UI 不读上游 ENGINES[]）。动作 = 两条状态层命令：CMD_SET_ENGINE /
 * CMD_SET_PRESET（a = 选中轨）。选中鼓轨时两行换成单行 `KIT − 名 +`：经 CMD_SET_PARAM 写
 * 鼓轨的 P_E0（上游 DRUMS 屏 KNOB 1），旋钮区按鼓轨逐槽规则全部锁定（页眉标 NO SOUND）。
 * 底部 bank 行只在该轨引擎为 FM6 时显示（A10）：对象随页常驻、按引擎显隐（零分配），
 * 其余引擎整行隐藏 ⇒ TAP 落在该带 = miss。
 *
 * chip 页（P4-F Task 4：MOD / SCALE / VOICE；Task 5 起 GLOBAL / FX；ADR-008 §8.9）：一页 = 多个 chip 共用同一组
 * 8 个旋钮（DSYN 模式）。chip 行在页眉行（y=4..36）：**一个自绘可点击对象**画出整行
 * chip（宽度按名字长度；点击按指针 x 落到哪个 chip），标题挪到 chip 行右侧；页眉/±
 * 按钮几何不变。整行一个对象而不是每 chip 一个 label：实测每 chip 一个 label 约
 * 336B（5 chip 的 MOD 页 delta 4,544B），而 EDIT→STEP 有序对的账面要求 chip 页 est
 * ≤ 4,608（ADR-005 §5）——按「超预算先改对象设计」收成单对象（STEP 格/旋钮同一纪律）。选中 chip 是**页内 UI 局部态**
 * （`UI <page> CHIP <n>` 与 chip 行共用，跨组页面重建保留）。切 chip 在 30ms 拍内
 * **零分配**重配：8 个旋钮对象常驻，只换 desc（label/fmt/范围）、显隐与焦点；chip 行
 * 只改「可见掩码/选中下标」两个字节并失效重绘——不新建/删除对象、不改样式。
 * 布局表 chip_count ≥ 2 的页是 chip 页；chip_count == 0 的无名单 chip 是整页（SAMPLE 与
 * SOUND），不画 chip 行。
 *
 * 槽位描述符与鼓轨规则：TRACK scope 一律取 `sloop_core_desc_track_live(选中轨, id)`
 * （INSERT/ENV SYNC/引擎钩子的动态标签，范围同静态表——FX 页的 `ATK/ESYNC` 手工标注
 * 因此删除）；GLOBAL/ENGINE scope 仍取静态 `sloop_core_desc`。鼓轨规则**逐槽**
 * （布局表 drum_ok，012 §3）：选中鼓轨时未标 drum_ok 的 TRACK/ENGINE 槽显示 -- 且
 * 拒绝编辑（不发命令），标了的（TFLT/LEVEL/PAN、PATTERN 四槽）照常；全局参数不受
 * 影响。旧式引擎页（ANALOG/FM6/SAMPLE）的整页锁定语义不变（page_locked）。
 * 条件 chip：`when` 谓词按选中轨上下文求值（VOICE·FILTER 仅 ANALOG 合成轨出现）；
 * 选中的 chip 变隐藏时生效 chip 落到首个可见 chip 并回写选择。
 *
 * 引擎页的适用性（SAMPLE）：槽位描述符取**页声明的引擎**（布局表），值是
 * trk[选中轨].p[P_E0+i]；选中轨的引擎与页不符（或选中鼓轨）时页面不可编辑、显示 --，
 * 避免拿 SAMPLE 的 label 写另一个引擎的槽位。SOUND 页没有整页锁：它的槽恒是选中轨
 * 自己的引擎，鼓轨由逐槽规则锁定。
 *
 * bank 存取行（SOUND 页、仅 FM6 引擎；P4-B Task 13 原属独立 FM6 页）：页面区底部一行
 * `BANK - B12 + | SAVE | ERASE`
 * + 状态行（页面区相对 y=552..596；旋钮盒底边 536，留 16px，不挤压旋钮环/文本）。
 * 槽号是**目标槽**（0..26，显示为 `B<n+1>`），与 PTCH 旋钮（音源，PTCH = 8 + 槽号）
 * 相互独立：SAVE = 选中轨的 live patch（`CMD_BANK_PUT`），ERASE = 擦目标槽
 * （`CMD_BANK_ERASE`）。播放中由存储层拒绝（状态 3 ⇒ `STOP BEFORE SAVE`），状态行与
 * FILE 页同一映射（theme.h 的 ui_file_status_text，act 0 = SAVE / 2 = ERASE）。
 * 键盘：←/→ 仍只在旋钮间移动焦点（不改既有旋钮模型），bank 行与页眉行**只经触摸**
 * （TAP 注入与按钮回调共用同一动作函数）；SAVE/ERASE 与 FILE 页同样只走触摸 + console。
 * 行的可见性是比原「锁定页拒绝」更强的守卫：选中轨不是 FM6（含鼓轨）时整行不显示、
 * 不建命中区（TAP miss），不可能用别的引擎的轨把该轨最后一次载入的 patch 写进用户银行槽；
 * 原页面本地的 `NOT FM6` 拒绝态因此不再存在（鼓轨 PUT 的状态层拒绝不变）。
 * **行可见性按此刻的状态重新判定**（`bank_row_visible`，不信任上一拍 update 的显隐标志：
 * 换引擎与本回调之间可能隔着不到一拍），行隐藏时命令不下发。
 * 擦写发生在 LVGL 回调里（TAP/按钮 → state_apply → 存储层），一次 SAVE 最坏阻塞
 * 一次整银行擦写窗口（spec §4.3 接受的 ≤150ms 级 UI 停顿）；bank 是 4KB 扇区重写，
 * 与工程/预置保存同一条「擦写期间不持引擎锁」纪律（ADR-009 §3）。
 *
 * 槽管理行（仅 SAMPLE 页，P4-D Task 8）：同一底部行几何（页面区相对 y=552..596）——
 * `U1..U4` 四个单对象 label（`USED name z<n> <KB>K` / `EMPTY`）+ `ERASE` chip + 状态行。
 * 点槽 = 把 SET 宏（P_E0）切到 USR 段的起点 + k（起点由 SAMPLE 的 SET 描述符派生，
 * 本版本 = 8..11，不是工厂槽号 32）经 `CMD_SET_PARAM` 下发；`ERASE` 只在当前 SET 落在
 * USR1..USR4 且页面未锁定时发 `CMD_SAMPLE_ERASE`（槽号 = SET − 起点），否则状态行
 * `NOT USR` 且**不下发命令**。状态行是页面本地结果（theme.h 的 ui_sample_status_text
 * 单一映射：`SET U<n>`/`ERASE OK`/`NOT USR`/`STOP BEFORE ERASE`/`NO STORAGE`/
 * `ERASE ERROR`）。
 * 槽信息读取是**文档化的只读例外**（与 FILE 页同口径）：`sloop_store_sample_info` 会
 * 对每个已用槽算 ≤80KiB 的数据 CRC，因此只在建页、槽动作与键变化时立即读；30ms 拍的
 * 非强制路径**最短 250ms**、key 稳定时 1s——绝不每 30ms 轮询（P4-D Task 4 评审的
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

/* bank 存取行（SOUND 页且引擎为 FM6；P4-B Task 13）：旋钮盒底边 = GRID_Y0 + ROW_PITCH +
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
 * 槽位 x = 16 + k×208（k=0..3 ⇒ 16/224/432/640，各宽 200 ⇒ 末槽右端 **840**）、
 * ERASE 856..956、状态行 972..1264。 */
#define SAMPLE_ROW_Y      552
#define SAMPLE_ROW_H      44
#define SAMPLE_SLOT_X0    16
#define SAMPLE_SLOT_W     200
#define SAMPLE_SLOT_PITCH 208
#define SAMPLE_ERASE_X    856
#define SAMPLE_STATUS_X   972
#define SAMPLE_STATUS_W   292

/* chip 行（仅 chip 页）：页眉行 y=4..36，与上下文行同一行；一个自绘可点击对象，
 * 每个 chip 宽 = page_layout_chip_width(名字)（11px/字符 + 16），x = 16 起按序排、
 * 间距 8（chip_x_at）。建控件、绘制与 TAP 命中同源。标题（上下文行）挪到行右侧。 */
#define CHIP_Y   4
#define CHIP_H   32
#define CHIP_X0  16
#define CHIP_GAP 8

/* 页声明引擎号（布局表 L_ENG_SAMPLE，上游 ENGINES[] 的 SAMPLE）：SAMPLE 页整页锁的判据。
 * bank 行的引擎谓词是 page_layout.h 的 PARAM_ENGINE_FM6。 */
#define PARAM_ENGINE_SAMPLE 4u

/* 引擎槽在 trk[].p[] 里的绝对 id 起点（上游 P_E0 = P_COUNT - 8） */
#define PARAM_ENGINE_BASE (SLOOP_CORE_P_COUNT - 8u)

typedef struct {
    knob_t knobs[PARAM_PAGE_SLOTS];
    bool present[PARAM_PAGE_SLOTS]; /* 该槽有描述符且已建控件 */
    lv_obj_t *header;
    lv_obj_t *btn_dec;
    lv_obj_t *btn_inc;
    uint8_t shown_chip;  /* 已重配到页面的生效 chip；0xFF = 尚未 */
    uint8_t chip_vis_shown;   /* chip 行最近渲染的可见掩码（bit c） */
    int8_t chip_sel_shown;    /* chip 行最近渲染的选中 chip；-1 = 尚未 */
    lv_obj_t *chip_row; /* chip 行（仅 chip 页）：整行一个自绘可点击对象 */
    uint8_t shown_track;
    uint8_t shown_engine;
    uint8_t shown_sel_engine; /* 选中轨引擎（SOUND 的 8 槽随它换标签；鼓轨记 0） */
    int8_t shown_locked;  /* -1 = 尚未渲染上下文 */
    int8_t shown_esync;   /* 选中轨 ENV SYNC 位（ATK/DEC/REL 槽描述符随它变） */
    uint8_t shown_itype; /* 选中轨 INSERT TYPE（INSERT A/B/C 槽描述符随它变，Task 5） */
    uint8_t focus_shown;  /* 最近渲染的焦点槽 */
    bool ctx_valid;
    /* bank 存取行（建在带 PARAM_FEAT_BANK_FM6 的页 = SOUND，仅 FM6 引擎时显示；其它页为 NULL） */
    lv_obj_t *bank_label;
    lv_obj_t *bank_dec;
    lv_obj_t *bank_inc;
    lv_obj_t *bank_value;
    lv_obj_t *bank_save;
    lv_obj_t *bank_erase;
    lv_obj_t *bank_status;
    uint8_t bank_shown_slot;  /* 已显示的目标槽（0..26）；0xFF = 未显示 */
    uint32_t bank_status_key; /* (act | status<<8 | ready<<16) 脏检查键 */
    int8_t bank_vis_shown;   /* bank 行最近渲染的可见性（SOUND 页随引擎变；-1 = 尚未） */
    /* 槽管理行（仅 SAMPLE 页；其它页为 NULL） */
    lv_obj_t *sample_label[SLOOP_STORE_SAMPLES];
    lv_obj_t *sample_erase;
    lv_obj_t *sample_status;
    /* SOUND 页眉（仅 SOUND 页；其它页为 NULL）：row 0 = ENGINE（鼓轨 = KIT）、row 1 = PRESET */
    lv_obj_t *sound_row[2];
    sloop_sound_snapshot_t sound_shown; /* 页眉行最近渲染的引擎/预置/kit 名 */
    int8_t sound_rows_shown;            /* 最近渲染的页眉行数（1 鼓轨 / 2 合成轨；-1 = 尚未） */
} page_ui_t;

static page_ui_t s_ui[PARAM_PAGE_COUNT];

/* bank 目标槽（UI 局部态；TAP/按钮都在 LVGL 上下文读写，页面重建时归零）与本页最后
 * 一次 bank 动作（0 = SAVE / 2 = ERASE / 其它 = 尚无动作）——动作语义下标，与
 * theme.h 的 ui_file_status_text 的文案表一致。 */
static uint8_t s_bank_slot;
static uint8_t s_bank_act = 0xFF;
static char s_bank_echo[32]; /* console TAP 回显专用（REPL 任务，避开 LVGL 标签缓冲） */

/* 槽管理行（仅 SAMPLE 页）：槽信息缓存 + 状态行（页面本地结果，不写 app_state_t）。
 * s_sample_info_key = (synth_status << 8) | SET：变化即立刻重读（动作结果的可见性）；
 * 其余情况下 update 每 ~1s 至多重读一次（info 的 CRC 成本见文件头）。 */
static sloop_store_sample_info_t s_sample_info[SLOOP_STORE_SAMPLES];
static uint16_t s_sample_info_key;
static uint32_t s_sample_info_ms;
static bool s_sample_info_valid;
static uint8_t s_sample_act = 0xFF; /* 0 = 槽选中 / 2 = ERASE / 其它 = 尚无动作 */
static uint8_t s_sample_sel_slot;   /* 最近一次选中的 USR 槽（0..3；`SET U<n>` 文本用） */
static uint8_t s_sample_lock_refused; /* 上一次拒绝是「选中轨引擎 ≠ SAMPLE」：解锁时复位状态行 */
static ui_sample_status_t s_sample_status = UI_SAMPLE_STATUS_NONE;
static uint32_t s_sample_row_sig = 0xFFFFFFFFu; /* 整行渲染脏检查键 */
static char s_sample_echo[32];                  /* console TAP 回显专用 */

/* 焦点槽（UI 局部态）：TAP 在 LVGL 上下文写、键盘在输入任务读/写 → 临界区保护。 */
static portMUX_TYPE s_focus_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_focus[PARAM_PAGE_COUNT];
/* 每 chip 页选中的 chip（UI 局部态，同上保护；非 chip 页恒 0）。0xFF = 未选 → 首个。 */
static uint8_t s_chip[PARAM_PAGE_COUNT];

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

static uint8_t chip_get(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT) return 0;
    portENTER_CRITICAL(&s_focus_mux);
    uint8_t c = s_chip[index];
    portEXIT_CRITICAL(&s_focus_mux);
    return c;
}

static void chip_set(uint8_t index, uint8_t chip)
{
    if (index >= PARAM_PAGE_COUNT) return;
    portENTER_CRITICAL(&s_focus_mux);
    s_chip[index] = chip;
    portEXIT_CRITICAL(&s_focus_mux);
}

/* ---------------------------------------------------------------- 槽位解析 --- */
/* 上游 TP[] 里会按状态换描述符的两个开关 id（core.h 枚举值；页面上下文键的一部分）：
 * P_ESYNC 开 ⇒ ATK/DEC/REL 显示音符名（ESYNC_DESC），P_ITYPE ⇒ INSERT A/B/C 的 label/
 * 范围含义（Task 5 的 INSERT chip）。两者都经 sloop_core_desc_track_live 取上游动态表。 */
#define PARAM_TP_ESYNC 59u
#define PARAM_TP_ITYPE 53u

/* 槽位描述符：TRACK 取**选中轨**的动态描述符（上游 track_desc），GLOBAL/ENGINE 取静态表 */
static bool slot_desc(const param_page_t *pg, uint8_t chip, uint8_t slot, sloop_desc_t *out)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL) return false;
    switch (s->scope) {
    case SLOOP_DESC_ENGINE:
        return sloop_core_desc(SLOOP_DESC_ENGINE, s->engine, s->id, out);
    case SLOOP_DESC_GLOBAL:
        return sloop_core_desc(SLOOP_DESC_GLOBAL, 0, s->id, out);
    default: {
        /* 窄读选中轨：本函数在点击路径的第三层帧里被调（回调 → refresh → update → 这里），
         * 在栈上放整份 app_state_t（≈1.7KB）会压穿当时 7,168B 的 LVGL 任务栈（Task 5 评审
         * R10；现为 10,240B，副本份数的限制不变）。 */
        uint8_t track = state_selected_track();
        return track < APP_STATE_TRACKS && sloop_core_desc_track_live(track, s->id, out);
    }
    }
}

static void sel_context(const app_state_t *st, bool *drum, uint8_t *engine);

/* 旧式整页声明的引擎（ENGINE 槽位的 engine 字段）；chip 页/非引擎页返回 0xFF */
static uint8_t page_engine(const param_page_t *pg)
{
    if (pg->chip_count != 0) return 0xFF;
    for (uint8_t i = 0; i < PARAM_PAGE_SLOTS; i++) {
        if (pg->chips[0].slots[i].id != 0xFF && pg->chips[0].slots[i].scope == SLOOP_DESC_ENGINE) {
            return pg->chips[0].slots[i].engine;
        }
    }
    return 0xFF;
}

/* bank 存取行建在布局表标了 PARAM_FEAT_BANK_FM6 的页（SOUND）；是否**显示**另由
 * 选中轨上下文决定（page_layout_bank_row_visible：合成轨 + 引擎 FM6，A10）。建控件、
 * 更新、TAP 命中表与回显同源。 */
static bool bank_row_present(const param_page_t *pg)
{
    return pg != NULL && (pg->features & PARAM_FEAT_BANK_FM6) != 0u;
}

static bool bank_row_visible(const param_page_t *pg, const app_state_t *st)
{
    bool drum;
    uint8_t engine;
    sel_context(st, &drum, &engine);
    return page_layout_bank_row_visible(pg, drum, engine);
}

/* 槽管理行建在布局表标了 PARAM_FEAT_SAMPLE 的页（SAMPLE）。 */
static bool sample_row_present(const param_page_t *pg)
{
    return pg != NULL && (pg->features & PARAM_FEAT_SAMPLE) != 0u;
}

/* 旧式引擎页整页不可编辑：选中鼓轨（没有引擎槽）或所选轨引擎与页声明不符 */
static bool page_locked(const param_page_t *pg, const app_state_t *st)
{
    uint8_t eng = page_engine(pg);
    if (eng == 0xFF) return false;
    uint8_t track = st->core.selected_track;
    if (track >= 3u) return true;
    return st->core.trk[track].engine != eng;
}

/* page_locked 的「此刻」版本（窄读）：旧式引擎页整页锁 */
static bool page_locked_now(const param_page_t *pg)
{
    uint8_t eng = page_engine(pg);
    if (eng == 0xFF) return false;
    if (state_selected_track() >= 3u) return true;
    return state_selected_engine() != eng;
}

/* 选中轨上下文：是否鼓轨、引擎号（鼓轨无引擎，回 0） */
static void sel_context(const app_state_t *st, bool *drum, uint8_t *engine)
{
    uint8_t track = st->core.selected_track;
    *drum = track >= APP_STATE_TRACKS - 1u;
    *engine = *drum || track >= APP_STATE_TRACKS ? 0u : (uint8_t)st->core.trk[track].engine;
}

/* 槽位是否锁定（显示 --、拒绝编辑）：旧式引擎页的整页锁（只锁 ENGINE 槽）+ 鼓轨逐槽规则 */
static bool slot_locked(const param_page_t *pg, uint8_t chip, uint8_t slot, const app_state_t *st)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL) return true;
    if (page_locked(pg, st)) return s->scope == SLOOP_DESC_ENGINE;
    bool drum;
    uint8_t engine;
    sel_context(st, &drum, &engine);
    (void)engine;
    return page_layout_slot_locked(s, drum);
}

/* slot_locked / slot_value 的「此刻」版本：点击路径（± 按钮、旋钮拖动、页眉行）不持整份
 * 状态副本，逐字段经状态层的窄读访问器取（与 slot_locked/slot_value 同一判定与同一投影）。 */
static bool slot_locked_now(const param_page_t *pg, uint8_t chip, uint8_t slot)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL) return true;
    if (page_locked_now(pg)) return s->scope == SLOOP_DESC_ENGINE;
    return page_layout_slot_locked(s, state_selected_track() >= APP_STATE_TRACKS - 1u);
}

static bool slot_value_now(const param_page_t *pg, uint8_t chip, uint8_t slot, int16_t *out)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL || out == NULL) return false;
    switch (s->scope) {
    case SLOOP_DESC_GLOBAL:
        if (s->id >= SLOOP_CORE_G_COUNT) return false;
        *out = state_read_g(s->id);
        return true;
    case SLOOP_DESC_TRACK:
        if (s->id >= SLOOP_CORE_P_COUNT) return false;
        *out = state_read_p(s->id);
        return true;
    case SLOOP_DESC_ENGINE:
        if (s->id >= 8u) return false;
        *out = state_read_p((uint8_t)(PARAM_ENGINE_BASE + s->id));
        return true;
    default:
        return false;
    }
}

/* 槽位当前值（GLOBAL→g[]、TRACK→p[]、ENGINE→p[P_E0+id]）；不可读返回 false */
static bool slot_value(const param_page_t *pg, uint8_t chip, uint8_t slot, const app_state_t *st,
                       int16_t *out)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL || out == NULL) return false;
    switch (s->scope) {
    case SLOOP_DESC_GLOBAL:
        if (s->id >= SLOOP_CORE_G_COUNT) return false;
        *out = st->g[s->id];
        return true;
    case SLOOP_DESC_TRACK:
        /* SOUND 页的 8 槽是 P_E0..P_E7（TRACK scope 走 live 描述符），值同样是 p[id] */
        if (s->id >= SLOOP_CORE_P_COUNT) return false;
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
static uint8_t slot_abs_id(const param_page_t *pg, uint8_t chip, uint8_t slot)
{
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    return s->scope == SLOOP_DESC_ENGINE ? (uint8_t)(PARAM_ENGINE_BASE + s->id) : s->id;
}

/* 写槽位值（状态层命令 + 引擎钳位）；返回 false = 槽位锁定/不可读（不发命令）。
 * applied（可 NULL）回填写后投影里的真值（钳位结果）。 */
static bool slot_write(uint8_t index, uint8_t chip, uint8_t slot, int16_t value, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || slot >= PARAM_PAGE_SLOTS) return false;
    const param_slot_t *s = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (s == NULL || slot_locked_now(pg, chip, slot)) return false;

    edit_cmd_t cmd = {0};
    if (s->scope == SLOOP_DESC_GLOBAL) {
        cmd.type = CMD_SET_GLOBAL;
        cmd.a = s->id;
        cmd.v = (uint16_t)value;
    } else if (s->scope == SLOOP_DESC_TRACK || s->scope == SLOOP_DESC_ENGINE) {
        cmd.type = CMD_SET_PARAM;
        cmd.a = state_selected_track();
        cmd.b = slot_abs_id(pg, chip, slot);
        cmd.v = (uint16_t)value;
    } else {
        return false;
    }
    state_apply(&cmd);
    if (applied != NULL) {
        int16_t v;
        *applied = slot_value_now(pg, chip, slot, &v) ? v : value;
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
                          uint8_t chip, bool locked)
{
    page_ui_t *ui = &s_ui[index];
    if (ui->header == NULL) return;
    char text[64];
    const param_slot_t *s0 = page_layout_slot(page_layout_chip(pg, chip), 0);
    uint8_t scope = s0 != NULL ? s0->scope : SLOOP_DESC_TRACK;
    if (scope == SLOOP_DESC_GLOBAL) {
        snprintf(text, sizeof(text), "%s", pg->title);
    } else if (page_layout_is_sound(pg) && st->sound.drum) {
        /* 鼓轨没有引擎与预置：旋钮区锁定（各槽 --），页眉只剩 KIT 行 */
        snprintf(text, sizeof(text), "%s · TRACK %u (DRUM) — NO SOUND", pg->title,
                 (unsigned)(track + 1u));
        lv_label_set_text(ui->header, text);
        lv_obj_set_style_text_color(ui->header, lv_color_hex(UI_ERROR), 0);
        return;
    } else if (locked && track >= 3u) {
        snprintf(text, sizeof(text), "%s · TRACK %u (DRUM) — N/A", pg->title, (unsigned)(track + 1));
    } else if (locked) {
        snprintf(text, sizeof(text), "%s · TRACK %u USES ENG %u — N/A", pg->title,
                 (unsigned)(track + 1), (unsigned)st->core.trk[track].engine);
    } else if (track >= 3u) {
        /* 逐槽规则页在鼓轨上不是整页锁：标出鼓轨，锁定的槽各自显示 -- */
        snprintf(text, sizeof(text), "%s · TRACK %u (DRUM)", pg->title, (unsigned)(track + 1));
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
/* 焦点槽 ±1（控件回调、TAP 与键盘 ↑/↓/ENTER 的共同路径）。返回 false = 槽位锁定/无值。 */
static bool param_step(uint8_t index, int8_t delta, int16_t *applied)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL) return false;
    uint8_t chip = chip_get(index);
    uint8_t slot = focus_get(index);
    int16_t v;
    /* 不持状态副本：本函数经 btn_cb/键盘在 LVGL 任务里跑，调用方随后还会 refresh（一份副本） */
    if (!slot_value_now(pg, chip, slot, &v) || slot_locked_now(pg, chip, slot)) return false;
    return slot_write(index, chip, slot, (int16_t)(v + delta), applied);
}

/* 旋钮拖动（LVGL 上下文）：值变化 → 状态层 → 立即刷新本页 */
static void knob_change_cb(knob_t *k, int16_t value)
{
    uint8_t index = ctx_page(k->user);
    (void)slot_write(index, chip_get(index), ctx_slot(k->user), value, NULL);
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
 * patch）/ 2 ERASE（CMD_BANK_ERASE）。只在 bank 行可见（合成轨 + 引擎 FM6）时可达——行
 * 隐藏时没有命中区。结果写 app_state_t.store_status（播放中/未就绪 = 3 …），状态行由
 * update() 映射，console `TAP` 回显走同一映射。 */
static void bank_action(uint8_t index, uint8_t act)
{
    const param_page_t *pg = page_layout_get(index);
    if (!bank_row_present(pg)) return;
    app_state_t st;
    state_get_copy(&st);
    /* 此刻重新判定行的可见性（不依赖上一拍 update 记下的显隐标志）：行隐藏 = 选中轨不是
     * FM6 引擎的合成轨，没有可存/可擦的 FM6 live patch，命令不下发。 */
    if (!bank_row_visible(pg, &st)) return;
    s_bank_act = act;
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
/* USR1..USR4 的 SET 宏起点 = 上游 SMP_NSETS（本版本 8）。不写字面量：从 SAMPLE 引擎的
 * SET 描述符派生——SET 是 F_ENUM 0..(SMP_NSETS+USR−1)，故 base = max + 1 − USR 数。
 * 描述符取不到、或值域里放不下 4 个 USR 时返回 0，调用方一律当作「没有 USR 槽」
 * （只读展示、绝不下发命令）——失败方向是安全的，不依赖注释里的 magic number。 */
static uint8_t sample_usr_set_base(void)
{
    static int16_t cached = -1;
    if (cached < 0) {
        sloop_desc_t d;
        int16_t base = 0;
        if (sloop_core_desc(SLOOP_DESC_ENGINE, PARAM_ENGINE_SAMPLE, 0u, &d) &&
            (int16_t)(d.max + 1 - d.min) > (int16_t)SLOOP_STORE_SAMPLES) {
            base = (int16_t)(d.max + 1 - (int16_t)SLOOP_STORE_SAMPLES);
        }
        cached = base;
    }
    return (uint8_t)cached;
}

/* 选中轨的 SET 宏（P_E0）→ USR 槽号；不在 USR1..USR4 返回 0xFF。 */
static uint8_t sample_selected_slot(const app_state_t *st)
{
    uint8_t base = sample_usr_set_base();
    if (base == 0) return 0xFF;
    int16_t set = st->p[PARAM_ENGINE_BASE];
    if (set < (int16_t)base || set >= (int16_t)(base + SLOOP_STORE_SAMPLES)) return 0xFF;
    return (uint8_t)(set - base);
}

/* 槽信息（文档化只读例外）：force 时立即重读；否则 250ms 内一律跳过，之后 key
 * （synth_status/SET）变过即重读，key 未变则最多 1s 一次。info 会对每个已用槽算
 * ≤80KiB 数据 CRC，是全页最贵的调用，绝不能每拍跑（拖 SET 旋钮时 key 每拍都变）。 */
static void sample_info_poll(const app_state_t *st, bool force)
{
    uint16_t key = (uint16_t)(((uint16_t)st->synth_status << 8) |
                              (uint8_t)st->p[PARAM_ENGINE_BASE]);
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t since = (uint32_t)(now - s_sample_info_ms);
    /* 非强制路径的最小间隔 250ms：拖 SET 旋钮时 key 每拍都变，不能让 4 槽 CRC 扫描
     * 每帧跑一遍；key 稳定时仍限 1s（info 的 CRC 成本见文件头）。 */
    if (!force && s_sample_info_valid &&
        (since < 250u || (key == s_sample_info_key && since < 1000u))) {
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

/* 整行渲染脏检查键：锁定态 + 选择槽 + 动作/状态 + ready + 四个槽的 (used/zones/KB/name)。 */
static uint32_t sample_row_sig(const app_state_t *st)
{
    uint32_t h = 2166136261u;
    h = sig_mix(h, page_locked(page_layout_get((uint8_t)page_layout_index(APP_PAGE_SAMPLE)), st)
                     ? 1u
                     : 0u);
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

/* 渲染整行（4 槽文本/颜色 + 状态行）。选中槽（SET 指向的 USR）用强调色；锁定页
 * （选中轨引擎 ≠ SAMPLE）**不**高亮：那个 SET 值不是该轨的采样选择，高亮会误导。 */
static void sample_render(uint8_t index, const app_state_t *st)
{
    page_ui_t *ui = &s_ui[index];
    const param_page_t *pg = page_layout_get(index);
    uint8_t sel = page_locked(pg, st) ? 0xFFu : sample_selected_slot(st);
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
    ui_sample_status_text(text, sizeof(text), (uint8_t)s_sample_status, s_sample_sel_slot);
    lv_label_set_text(ui->sample_status, text);
    lv_obj_set_style_text_color(ui->sample_status,
                                lv_color_hex(ui_sample_status_color((uint8_t)s_sample_status)), 0);
}

/* 槽选中（控件回调与 TAP 的共同路径）：把 SET 宏切到 SMP_NSETS + k（8..11）经
 * CMD_SET_PARAM 下发。锁定页（选中轨引擎 ≠ SAMPLE，含鼓轨）不下发命令，状态行显示
 * `NOT USR`（页内本地结果，与 `sample_erase` 同一令牌）；描述符取不到（USR 段起点
 * 无法派生）同样落到 `NOT USR` 而不是留下上一次的过期结果。 */
static bool sample_select(uint8_t index, uint8_t slot)
{
    const param_page_t *pg = page_layout_get(index);
    uint8_t base = sample_usr_set_base();
    if (slot >= SLOOP_STORE_SAMPLES) return false;
    app_state_t st;
    state_get_copy(&st);
    bool locked = page_locked(pg, &st);
    if (base == 0 || locked || !slot_write(index, 0, 0, (int16_t)(base + slot), NULL)) {
        s_sample_act = 0u;
        s_sample_status = UI_SAMPLE_STATUS_NOT_USR;
        /* 只有「锁定页」的拒绝随解锁复位；描述符缺失（base==0）是环境问题，留着可见 */
        s_sample_lock_refused = locked ? 1u : 0u;
        refresh(index);
        return false;
    }
    s_sample_act = 0u;
    s_sample_lock_refused = 0;
    s_sample_sel_slot = slot;
    s_sample_status = UI_SAMPLE_STATUS_SET;
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
        s_sample_lock_refused = 1;
        refresh(index);
        return;
    }
    uint8_t slot = sample_selected_slot(&st);
    if (slot == 0xFFu) {
        s_sample_status = UI_SAMPLE_STATUS_NOT_USR;
        s_sample_lock_refused = 0;
        refresh(index);
        return;
    }
    edit_cmd_t cmd = {0};
    cmd.type = CMD_SAMPLE_ERASE;
    cmd.a = slot;
    state_apply(&cmd);
    s_sample_lock_refused = 0;
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

/* ---------------------------------------------------------------- SOUND 页眉 --- */
_Static_assert(sizeof(sloop_sound_snapshot_t) == 6u + SLOOP_ENGINE_NAME + 2u * SLOOP_PRESET_NAME,
               "sound 投影无填充：memcmp 脏检查依赖整字节比较");

/* 页眉三区对象的自绘：圆角底 + `−`（左区居中）+ 行名（左对齐）+ 名字（右对齐）+ `+`
 * （右区居中）。文本读 s_ui[index].sound_shown（update 在名字变化时先写它再失效本对象）。
 * 名字是 RAM 缓冲：draw task 是延迟渲染的，text_local 让 LVGL 把它拷进任务（静态串
 * 的行名/± 不拷，与 chip 行同法）。列的几何在 page_layout.h（TAP 命中同一张表）。 */
static void sound_row_draw(lv_event_t *e)
{
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    uint8_t row = (uint8_t)(uintptr_t)lv_obj_get_user_data(obj);
    const page_ui_t *ui = &s_ui[index];
    bool drum = ui->sound_shown.drum != 0u;
    const char *label = row == 0u ? (drum ? "KIT" : "ENGINE") : "PRESET";
    const char *name =
        row == 0u ? (drum ? ui->sound_shown.kit_name : ui->sound_shown.engine_name)
                  : ui->sound_shown.preset_name;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t zone = SOUND_ROW_ZONE;
    int32_t name_x1 = a.x1 + page_layout_sound_name_x(row, drum);
    int32_t name_x2 = a.x2 - SOUND_ROW_NAME_R;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.base.layer = layer;
    rect.bg_color = lv_color_hex(UI_PANEL_HI);
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 10;
    lv_draw_rect(layer, &rect, &a);

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.layer = layer;
    d.font = &lv_font_montserrat_16;
    /* 16pt 行高 ≈20px：文字区从行顶 +6 起（与 chip 行同法） */
    lv_area_t ta = {.y1 = a.y1 + 6, .y2 = a.y2};

    d.color = lv_color_hex(UI_TEXT);
    d.align = LV_TEXT_ALIGN_CENTER;
    ta.x1 = a.x1;
    ta.x2 = a.x1 + zone - 1;
    d.text = "-";
    lv_draw_label(layer, &d, &ta);
    ta.x1 = a.x2 - zone + 1;
    ta.x2 = a.x2;
    d.text = "+";
    lv_draw_label(layer, &d, &ta);

    d.color = lv_color_hex(UI_TEXT_DIM);
    d.align = LV_TEXT_ALIGN_LEFT;
    ta.x1 = a.x1 + SOUND_ROW_TEXT_X;
    ta.x2 = name_x1 - 1;
    d.text = label;
    lv_draw_label(layer, &d, &ta);

    d.color = lv_color_hex(UI_ACCENT);
    d.align = LV_TEXT_ALIGN_RIGHT;
    d.text_local = true;
    ta.x1 = name_x1;
    ta.x2 = name_x2;
    d.text = name;
    lv_draw_label(layer, &d, &ta);
}

static bool sound_step(uint8_t index, uint8_t row, int8_t delta, sloop_tap_result_t *out);

/* 点击：左右 42px 是 ∓1 区（与 TAP 同一张几何表）；中段是标签，不动作。 */
static void sound_row_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        sound_row_draw(e);
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) return;
        uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
        lv_obj_t *obj = lv_event_get_target_obj(e);
        uint8_t row = (uint8_t)(uintptr_t)lv_obj_get_user_data(obj);
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        int32_t rel = p.x - a.x1;
        int32_t w = page_layout_sound_row_w(row);
        if (rel >= SOUND_ROW_ZONE && rel < w - SOUND_ROW_ZONE) return; /* 标签中段 */
        if (sound_step(index, row, rel < SOUND_ROW_ZONE ? -1 : 1, NULL)) refresh(index);
        break;
    }
    default:
        break;
    }
}

/* 页眉行的 ∓1（控件回调与 TAP 的共同路径）。row 0：合成轨 = 换引擎（CMD_SET_ENGINE，
 * 环绕 0..nengines-1）/ 鼓轨 = 换 kit（CMD_SET_PARAM 写 P_E0，环绕 KIT 描述符的
 * min..max——旋钮槽在鼓轨上是锁定的，kit 只经这一行编辑）；row 1：换预置
 * （CMD_SET_PRESET，环绕 0..npresets-1）。这里只做环绕，值不会越界（越界/冻结由状态层
 * 判并写 sound_status）。out（可 NULL）：a = 写后回读的引擎/预置下标或 kit 号、d = 方向、
 * err = sound_status（KIT 行：0 = 已下发 / FROZEN）。返回 false = 该行在当前上下文不存在。 */
static bool sound_step(uint8_t index, uint8_t row, int8_t delta, sloop_tap_result_t *out)
{
    const param_page_t *pg = page_layout_get(index);
    if (!page_layout_is_sound(pg) || delta == 0) return false;
    /* 只取 49B 的 sound 块与几个标量，不持整份 app_state_t：本函数经页眉行回调在 LVGL 任务
     * 里跑，调用方随后还会 refresh（Task 5 评审 R10）。 */
    sloop_sound_snapshot_t snd;
    state_read_sound(&snd);
    uint8_t track = state_selected_track();
    edit_cmd_t cmd = {0};
    if (row == 0u && snd.drum) {
        sloop_desc_t d;
        if (track >= APP_STATE_TRACKS ||
            !sloop_core_desc_track_live(track, PARAM_ENGINE_BASE, &d)) {
            return false;
        }
        int32_t v = (int32_t)state_read_p(PARAM_ENGINE_BASE) + delta;
        if (v > d.max) v = d.min;
        if (v < d.min) v = d.max;
        cmd.type = CMD_SET_PARAM;
        cmd.a = track;
        cmd.b = PARAM_ENGINE_BASE;
        cmd.v = (uint16_t)(int16_t)v;
        state_apply(&cmd);
        if (out == NULL) return true;
        out->kind = SLOOP_TAP_SOUND_KIT;
        out->a = (uint8_t)state_read_p(PARAM_ENGINE_BASE);
        out->d = delta;
        out->err = sloop_seq_frozen() ? APP_SOUND_STATUS_FROZEN : 0u;
        return true;
    }
    if (snd.drum || row >= page_layout_sound_rows(false)) return false; /* 鼓轨没有 PRESET 行 */
    uint32_t n = row == 0u ? snd.nengines : snd.npresets;
    if (n == 0u) return false;
    int32_t v = (int32_t)(row == 0u ? snd.engine : snd.preset) + delta;
    if (v < 0) v += (int32_t)n;
    if (v >= (int32_t)n) v -= (int32_t)n;
    cmd.type = row == 0u ? CMD_SET_ENGINE : CMD_SET_PRESET;
    cmd.a = track;
    cmd.v = (uint16_t)v;
    state_apply(&cmd);
    if (out == NULL) return true;
    state_read_sound(&snd);
    out->kind = row == 0u ? SLOOP_TAP_SOUND_ENGINE : SLOOP_TAP_SOUND_PRESET;
    out->a = row == 0u ? snd.engine : snd.preset;
    out->d = delta;
    out->err = state_sound_status();
    return true;
}

/* SOUND 页：旋钮区的槽随选中轨变（鼓轨只有 P_E0 = KIT 描述符，合成轨 8 槽都有）——
 * 只翻转「存在性变了的槽」的显隐/描述符（零分配；其余槽由调用方的描述符重解析处理），
 * 焦点夹到存在的槽。 */
static void sound_sync_slots(uint8_t index, uint8_t chip)
{
    page_ui_t *ui = &s_ui[index];
    const param_page_t *pg = page_layout_get(index);
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        knob_t *k = &ui->knobs[slot];
        sloop_desc_t d;
        bool have = k->obj != NULL && slot_desc(pg, chip, slot, &d);
        if (have == ui->present[slot]) continue;
        ui->present[slot] = have;
        lv_obj_set_hidden(k->obj, !have);
        if (!have) {
            knob_set_active(k, false);
            continue;
        }
        k->desc = d;
        k->shown = false;
        k->dragging = false;
        lv_obj_invalidate(k->obj);
    }
    uint8_t f = focus_get(index);
    if (!ui->present[f]) {
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (ui->present[slot]) {
                focus_set(index, slot);
                break;
            }
        }
        ui->focus_shown = 0xFF;
    }
}

/* SOUND 页眉行（仅 SOUND 页）：两个三区对象常驻（row 1 在鼓轨隐藏；显隐由 update 落地）。
 * 对象 user_data = 行号，事件回调 user_data = 页下标。首帧文本由紧随的 update 渲染。 */
static void build_sound_rows(uint8_t index, lv_obj_t *parent)
{
    page_ui_t *ui = &s_ui[index];
    ui->sound_rows_shown = -1;
    for (uint8_t row = 0; row < 2u; row++) {
        lv_obj_t *obj = lv_obj_create(parent);
        ui->sound_row[row] = obj;
        if (obj == NULL) return;
        lv_obj_remove_style_all(obj);
        lv_obj_set_scrollable(obj, false);
        lv_obj_set_pos(obj, page_layout_sound_row_x(row), SOUND_ROW_Y);
        lv_obj_set_size(obj, page_layout_sound_row_w(row), SOUND_ROW_H);
        lv_obj_set_clickable(obj, true);
        lv_obj_set_user_data(obj, (void *)(uintptr_t)row);
        lv_obj_add_event_cb(obj, sound_row_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)index);
        lv_obj_set_hidden(obj, true);
    }
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
    uint8_t chip = chip_get((uint8_t)idx);
    uint8_t f = focus_get((uint8_t)idx);
    for (uint8_t n = 0; n < PARAM_PAGE_SLOTS; n++) {
        f = (uint8_t)((f + PARAM_PAGE_SLOTS + delta) % PARAM_PAGE_SLOTS);
        sloop_desc_t d;
        if (slot_desc(pg, chip, f, &d)) break;
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
    /* 槽位锁定（引擎页不适用/鼓轨逐槽规则）：旋钮显示 -- 且不可编辑。用独立错误码让
     * console 回 `ignored (locked)`（与环带外的 NOT_FOUND 区分）。 */
    app_state_t st;
    state_get_copy(&st);
    if (slot_locked(page_layout_get((uint8_t)idx), chip_get((uint8_t)idx), slot, &st)) {
        return ESP_ERR_NOT_ALLOWED;
    }
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
    if (s_btn_style_ready) {
        lv_style_set_text_color(&s_st_btn, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_btn, lv_color_hex(UI_PANEL_HI));
        return;
    }
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
    if (done) {
        lv_style_set_text_color(&s_st_bank, lv_color_hex(UI_TEXT));
        lv_style_set_bg_color(&s_st_bank, lv_color_hex(UI_PANEL_HI));
        lv_style_set_bg_color(&s_st_bank_save, lv_color_hex(ui_track_color(1)));
        lv_style_set_bg_color(&s_st_bank_erase, lv_color_hex(ui_track_color(3)));
        return;
    }
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
    ui->bank_shown_slot = 0xFF;
    ui->bank_status_key = 0xFFFFFFFFu;
    ui->bank_vis_shown = -1; /* 可见性由紧随的 update 按选中轨引擎落地 */

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

    /* 对象常驻：建完先整行隐藏，可见（FM6 引擎）时 update 再逐个显示（不在 30ms 拍里
     * 建/删对象）。必须在七个对象都建好之后——建之前隐藏是对空指针的空操作。 */
    lv_obj_t *objs[] = {ui->bank_label, ui->bank_value, ui->bank_dec, ui->bank_inc,
                        ui->bank_save, ui->bank_erase, ui->bank_status};
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) lv_obj_set_hidden(objs[i], true);
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
 * ERASE chip（ctx = index<<4|4）与状态行；槽信息由建页后的首个 update 读（之后至多 1s
 * 一次）。首帧文本由紧随的 update 渲染（s_sample_row_sig 强制失效）。 */
static lv_obj_t *build_sample_row(uint8_t index, lv_obj_t *parent)
{
    page_ui_t *ui = &s_ui[index];
    s_sample_act = 0xFF;
    s_sample_sel_slot = 0;
    s_sample_lock_refused = 0;
    s_sample_status = UI_SAMPLE_STATUS_NONE;
    s_sample_row_sig = 0xFFFFFFFFu;
    /* 槽信息由紧随的 update 首拍读（s_sample_info_valid=false ⇒ 立即轮询）。这里**不**在栈上
     * 放 app_state_t：页面构建经 TAB 走 LVGL 任务（栈 7,168B），ui_timer_cb 已持一份状态
     * 副本，再嵌一份会把该任务栈压穿（P4-F Task 5 的 nav ⑦ 实测：Stack protection fault）。 */
    s_sample_info_valid = false;

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

static bool param_chip_select(uint8_t index, uint8_t chip);

/* chip 行几何：第 c 个 chip 的左缘（对象内相对坐标）。隐藏 chip 不占位之外的空间——
 * 条件 chip 隐藏时它那一格留空（下标不塌陷，可见 chip 位置恒定，TAP/绘制同一张表）。 */
static int16_t chip_x_at(const param_page_t *pg, uint8_t chip)
{
    int32_t x = 0;
    for (uint8_t c = 0; c < chip && c < pg->chip_count; c++) {
        x += page_layout_chip_width(pg->chips[c].name) + CHIP_GAP;
    }
    return (int16_t)x;
}

static int16_t chip_row_width(const param_page_t *pg)
{
    return (int16_t)(chip_x_at(pg, pg->chip_count) - CHIP_GAP);
}

/* 自绘：每个可见 chip 一个圆角底 + 居中 16pt 名字；选中的 chip 用强调色底/背景色字。
 * 名称是布局表里的静态字符串（text_local=0，不复制）。 */
static void chip_row_draw(lv_event_t *e)
{
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    const param_page_t *pg = page_layout_get(index);
    const page_ui_t *ui = &s_ui[index];
    if (pg == NULL) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target_obj(e);
    /* 绘制坐标是屏幕绝对坐标：行对象放在 (CHIP_X0, CHIP_Y)，chip 在行内横向排布 */
    lv_area_t row;
    lv_obj_get_coords(obj, &row);
    for (uint8_t c = 0; c < pg->chip_count; c++) {
        if ((ui->chip_vis_shown & (1u << c)) == 0) continue;
        bool sel = (int8_t)c == ui->chip_sel_shown;
        int32_t x = row.x1 + chip_x_at(pg, c);
        int32_t w = page_layout_chip_width(pg->chips[c].name);
        lv_area_t area = {.x1 = x, .y1 = row.y1, .x2 = x + w - 1, .y2 = row.y2};
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.base.layer = layer;
        rect.bg_color = lv_color_hex(sel ? UI_ACCENT : UI_PANEL_HI);
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = 10;
        lv_draw_rect(layer, &rect, &area);
        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.base.layer = layer;
        label.font = &lv_font_montserrat_16;
        label.color = lv_color_hex(sel ? UI_BG : UI_TEXT);
        label.align = LV_TEXT_ALIGN_CENTER;
        label.text = pg->chips[c].name;
        label.text_local = false;
        /* 16pt 行高 ≈20px：文字区从行顶 +6 起（垂直居中，与 DSYN chip 的 pad_top 6 同） */
        lv_area_t ta = {.x1 = x, .y1 = row.y1 + 6, .x2 = x + w - 1, .y2 = row.y2};
        lv_draw_label(layer, &label, &ta);
    }
}

/* 点击：按指针落在哪个可见 chip（x 区间与绘制/TAP 同源）切生效 chip。 */
static void chip_row_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DRAW_MAIN:
        chip_row_draw(e);
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_active();
        if (indev == NULL) return;
        uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
        const param_page_t *pg = page_layout_get(index);
        if (pg == NULL) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_area_t a;
        lv_obj_get_coords(lv_event_get_target_obj(e), &a);
        int32_t rx = p.x - a.x1;
        for (uint8_t c = 0; c < pg->chip_count; c++) {
            if ((s_ui[index].chip_vis_shown & (1u << c)) == 0) continue;
            int32_t x = chip_x_at(pg, c);
            if (rx >= x && rx < x + page_layout_chip_width(pg->chips[c].name)) {
                (void)param_chip_select(index, c);
                return;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* 生效 chip：选中 chip 若在当前上下文隐藏（条件 chip），落到首个可见 chip 并回写选择。
 * 非 chip 页恒 0。**不取 LVGL 锁**：上下文取自调用方的快照 st，选择经 s_focus_mux 读写，
 * 不碰 LVGL 对象——所以两个调用方都不以持锁为前提：update 拍（LVGL 上下文）随后按返回值
 * 决定是否 apply_chip 重配旋钮；page_params_chip_get（console `UI <page> CHIP`，无 LVGL
 * 锁）只读返回值，回写的选择留给下一拍生效。 */
static uint8_t chip_effective(uint8_t index, const param_page_t *pg, const app_state_t *st)
{
    uint8_t chip = chip_get(index);
    if (pg->chip_count <= 1u) return 0;
    bool drum;
    uint8_t engine;
    sel_context(st, &drum, &engine);
    if (chip < page_layout_chip_count(pg) &&
        page_layout_chip_visible(&pg->chips[chip], drum, engine)) {
        return chip;
    }
    int vis = page_layout_visible_chip_at(pg, 0, drum, engine);
    chip = vis >= 0 ? (uint8_t)vis : 0u;
    chip_set(index, chip);
    return chip;
}

/* chip 切换（chip 行回调、TAP、console `UI <page> CHIP` 的共同路径）：只改页内选择
 * 并立即刷新本页（update 拍内零分配重配 8 个旋钮）。越界/当前上下文隐藏的 chip
 * （条件 chip 不成立）拒绝且不改选择。 */
static bool param_chip_select(uint8_t index, uint8_t chip)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || chip >= page_layout_chip_count(pg)) return false;
    app_state_t st;
    state_get_copy(&st);
    bool drum;
    uint8_t engine;
    sel_context(&st, &drum, &engine);
    if (!page_layout_chip_visible(&pg->chips[chip], drum, engine)) return false;
    chip_set(index, chip);
    refresh(index);
    return true;
}

/* 把 8 个常驻旋钮重配到 chip（零分配）：换 desc（label/fmt/范围）、强制重绘、显隐，
 * 焦点夹到存在的槽。旧式整页也走这里（不存在的槽没有控件对象，直接跳过）。 */
static void apply_chip(uint8_t index, uint8_t chip)
{
    page_ui_t *ui = &s_ui[index];
    const param_page_t *pg = page_layout_get(index);
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        knob_t *k = &ui->knobs[slot];
        sloop_desc_t d;
        bool have = k->obj != NULL && slot_desc(pg, chip, slot, &d);
        ui->present[slot] = have;
        if (!have) {
            if (k->obj != NULL) {
                lv_obj_set_hidden(k->obj, true);
                knob_set_active(k, false);
            }
            continue;
        }
        k->desc = d;
        k->shown = false; /* 新描述符：名称/数值强制重绘 */
        k->dragging = false;
        lv_obj_set_hidden(k->obj, false);
        lv_obj_invalidate(k->obj);
    }
    uint8_t f = focus_get(index);
    if (!ui->present[f]) {
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (ui->present[slot]) {
                focus_set(index, slot);
                break;
            }
        }
    }
    ui->focus_shown = 0xFF; /* 焦点可能被夹过 */
    ui->ctx_valid = false;  /* 页眉/描述符重解析 */
}

lv_obj_t *page_params_build(uint8_t index, lv_obj_t *parent)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || parent == NULL) return NULL;
    page_ui_t *ui = &s_ui[index];
    /* 懒建重试时控件是全新的：复位上下文/焦点缓存（焦点槽与 chip 选择本身保留） */
    memset(ui, 0, sizeof(*ui));
    ui->shown_locked = -1;
    ui->focus_shown = 0xFF;
    ui->shown_sel_engine = 0xFF; /* 首拍强制重解析槽描述符 */
    ui->shown_chip = 0xFF;
    ui->chip_sel_shown = -1;
    const bool paged = pg->chip_count > 1u;
    const bool sound = page_layout_is_sound(pg);
    /* 常驻页（chip 页 / SOUND）：8 个旋钮对象都建，desc/显隐由 update 首拍落地 */
    const bool resident = paged || sound;

    ui->header = lv_label_create(parent);
    UI_BUILD_CHECK(ui->header);
    lv_obj_set_style_text_font(ui->header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->header, lv_color_hex(UI_TEXT_DIM), 0);
    lv_label_set_long_mode(ui->header, LV_LABEL_LONG_CLIP);
    lv_label_set_text(ui->header, "");

    /* chip 行（仅 chip 页）：位置/宽度写进 page_ui_t，TAP 命中与建控件同源 */
    int16_t header_x = HEADER_X;
    int16_t header_w = 1000;
    if (paged) {
        int16_t x;
        ui->chip_row = lv_obj_create(parent);
        UI_BUILD_CHECK(ui->chip_row);
        lv_obj_remove_style_all(ui->chip_row);
        lv_obj_set_scrollable(ui->chip_row, false);
        lv_obj_set_pos(ui->chip_row, CHIP_X0, CHIP_Y);
        lv_obj_set_size(ui->chip_row, chip_row_width(pg), CHIP_H);
        lv_obj_set_clickable(ui->chip_row, true);
        lv_obj_add_event_cb(ui->chip_row, chip_row_event_cb, LV_EVENT_ALL, (void *)(uintptr_t)index);
        x = (int16_t)(CHIP_X0 + chip_row_width(pg));
        header_x = (int16_t)(x - CHIP_GAP + 16);
        header_w = (int16_t)(BTN_DEC_X - 16 - header_x);
    } else if (sound) {
        /* 上下文行在页眉最后一行右侧：先按合成轨的两行放，update 首拍按鼓轨（单行 KIT）
         * 挪位。构建路径不取状态副本——本函数经 TAB 在 LVGL 任务里跑，栈上放不下
         * app_state_t（见 build_sample_row 的注）。 */
        header_x = page_layout_sound_header_x(false);
        header_w = (int16_t)(BTN_DEC_X - 16 - header_x);
    }
    lv_obj_set_pos(ui->header, header_x, HEADER_Y);
    lv_obj_set_size(ui->header, header_w, 30);

    /* ± 按钮：共用同一回调，user_data 编码 (页下标 << 4 | 方向位)；方向位 0=减 1=加。
     * 旋钮上下文另有 +1 偏移（见 KNOB_CTX），按钮不需要（解码不经过 NULL 哨兵）。 */
    ui->btn_dec = make_button(parent, BTN_DEC_X, BTN_Y, "-", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 0u));
    UI_BUILD_CHECK(ui->btn_dec);
    ui->btn_inc = make_button(parent, BTN_INC_X, BTN_Y, "+", btn_cb,
                              (void *)(uintptr_t)(((unsigned)index << 4) | 1u));
    UI_BUILD_CHECK(ui->btn_inc);

    /* 旋钮：旧式整页只为有描述符的槽建控件；chip 页与 SOUND 页 8 个对象常驻（desc/
     * 显隐由 update 首拍的 apply_chip 落地；SOUND 的槽集合随选中轨变），不新建/删除对象。 */
    uint8_t chip = chip_get(index);
    if (chip >= page_layout_chip_count(pg)) chip = 0;
    static const sloop_desc_t placeholder = {"?", NULL, SLOOP_DESC_FMT_INT, 0, 1, 0, NULL, 0};
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        sloop_desc_t desc = placeholder;
        if (!resident && !slot_desc(pg, chip, slot, &desc)) continue;
        knob_t *k = &ui->knobs[slot];
        int16_t cx = (int16_t)(GRID_X0 + (slot % 4u) * CELL_PITCH);
        int16_t cy = (int16_t)(GRID_Y0 + (slot / 4u) * ROW_PITCH);
        if (!knob_create(k, parent, cx, cy, &desc, KNOB_CTX(index, slot), knob_change_cb,
                         knob_focus_cb)) {
            return NULL;
        }
        if (resident) {
            lv_obj_set_hidden(k->obj, true);
        } else {
            ui->present[slot] = true;
        }
    }

    /* 焦点槽落在第一个有控件的槽上（重试后槽集合可能变化；chip 页由 apply_chip 夹） */
    uint8_t f = focus_get(index);
    if (!resident && !ui->present[f]) {
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (ui->present[slot]) {
                focus_set(index, slot);
                break;
            }
        }
    }

    /* SOUND 页眉行（ENGINE/PRESET；鼓轨 KIT）：页眉带 y=4..36，不与 ± 按钮（x ≥ 1096）重叠 */
    if (sound) {
        build_sound_rows(index, parent);
        UI_BUILD_CHECK(ui->sound_row[0]);
        UI_BUILD_CHECK(ui->sound_row[1]);
    }
    /* bank 存取行（SOUND 页；仅 FM6 引擎时显示）：旋钮之后建，几何在页面区底部 */
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
    /* 槽管理行同一**口径**：锁定页（选中轨引擎 ≠ SAMPLE）的 `NOT USR` 拒绝随解锁复位；
     * 描述符缺失导致的 `NOT USR` 不是上下文问题，保留到下一次动作。 */
    if (!locked && s_sample_lock_refused) {
        s_sample_lock_refused = 0;
        s_sample_status = UI_SAMPLE_STATUS_NONE;
        s_sample_act = 0xFF;
    }

    /* chip 页：生效 chip（条件 chip 隐藏时夹到首个可见）变化 ⇒ 零分配重配 8 旋钮 */
    uint8_t chip = chip_effective(index, pg, st);
    if (chip != ui->shown_chip) {
        ui->shown_chip = chip;
        apply_chip(index, chip);
    }
    bool drum;
    uint8_t sel_engine;
    sel_context(st, &drum, &sel_engine);
    if (pg->chip_count > 1u && ui->chip_row != NULL) {
        /* chip 行：可见掩码/选中下标变化才失效重绘（两个字节 + invalidate，零分配）。 */
        uint8_t vis = 0;
        for (uint8_t c = 0; c < pg->chip_count; c++) {
            if (page_layout_chip_visible(&pg->chips[c], drum, sel_engine)) vis |= (uint8_t)(1u << c);
        }
        if (vis != ui->chip_vis_shown || (int8_t)chip != ui->chip_sel_shown) {
            ui->chip_vis_shown = vis;
            ui->chip_sel_shown = (int8_t)chip;
            lv_obj_invalidate(ui->chip_row);
        }
    }

    /* SOUND 页眉：行数随鼓轨变（KIT 取代 ENGINE+PRESET）、名字随引擎/预置变——变化才
     * 失效重绘（零分配：只改显隐与 invalidate）。上下文行挪到最后一行右侧。 */
    if (page_layout_is_sound(pg) && ui->sound_row[0] != NULL) {
        bool sdrum = st->sound.drum != 0u;
        uint8_t rows = page_layout_sound_rows(sdrum);
        if (ui->sound_rows_shown != (int8_t)rows) {
            int16_t hx = page_layout_sound_header_x(sdrum);
            ui->sound_rows_shown = (int8_t)rows;
            lv_obj_set_hidden(ui->sound_row[0], false);
            lv_obj_set_hidden(ui->sound_row[1], rows < 2u);
            lv_obj_set_pos(ui->header, hx, HEADER_Y);
            lv_obj_set_size(ui->header, (int16_t)(BTN_DEC_X - 16 - hx), 30);
            lv_obj_invalidate(ui->sound_row[0]);
            lv_obj_invalidate(ui->sound_row[1]);
        }
        if (memcmp(&ui->sound_shown, &st->sound, sizeof(st->sound)) != 0) {
            ui->sound_shown = st->sound;
            lv_obj_invalidate(ui->sound_row[0]);
            lv_obj_invalidate(ui->sound_row[1]);
        }
    }

    uint8_t engine = page_engine(pg);
    bool esync = st->p[PARAM_TP_ESYNC] != 0;
    uint8_t itype = (uint8_t)st->p[PARAM_TP_ITYPE];
    /* 上下文键含**选中轨引擎**：SOUND 页的槽描述符取该轨引擎（换引擎 = 全部标签换），
     * 页声明引擎（page_engine）在 SOUND 页恒为 0xFF、看不出换引擎。 */
    if (!ui->ctx_valid || ui->shown_track != track || ui->shown_engine != engine ||
        ui->shown_sel_engine != sel_engine || ui->shown_locked != (int8_t)locked ||
        ui->shown_esync != (int8_t)esync || ui->shown_itype != itype) {
        ui->ctx_valid = true;
        ui->shown_track = track;
        ui->shown_engine = engine;
        ui->shown_sel_engine = sel_engine;
        ui->shown_locked = (int8_t)locked;
        ui->shown_esync = (int8_t)esync;
        ui->shown_itype = itype;
        update_header(index, pg, st, track, chip, locked);
        /* SOUND：槽集合随选中轨变（鼓轨 ⇄ 合成轨），先对齐显隐再重解析描述符 */
        if (page_layout_is_sound(pg)) sound_sync_slots(index, chip);
        /* 动态描述符翻转（ENV SYNC 的 ATK/DEC/REL、INSERT TYPE 的 A/B/C；引擎钩子随
         * 换轨/换引擎）：重新解析并重绘，不要求用户重进页面。 */
        for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
            if (!ui->present[slot]) continue;
            sloop_desc_t d;
            if (!slot_desc(pg, chip, slot, &d)) continue;
            knob_t *k = &ui->knobs[slot];
            if (d.label == k->desc.label && d.fmt == k->desc.fmt && d.unit == k->desc.unit) continue;
            k->desc = d;
            lv_obj_invalidate(k->obj);
        }
    }

    uint8_t focus = focus_get(index);
    if (ui->focus_shown != focus) apply_focus(index, focus);

    /* 锁定槽（引擎页不适用/鼓轨逐槽规则）：-- 且不接受拖拽；可编辑槽按投影值刷新 */
    for (uint8_t slot = 0; slot < PARAM_PAGE_SLOTS; slot++) {
        if (!ui->present[slot]) continue;
        knob_t *k = &ui->knobs[slot];
        if (slot_locked(pg, chip, slot, st)) {
            if (!k->shown || !k->locked) knob_show_locked(k);
            continue;
        }
        int16_t v;
        if (!slot_value(pg, chip, slot, st, &v)) continue;
        if (!k->shown || k->locked || k->value != v) knob_show(k, v);
    }
    sloop_ui_zoom_knob(ui->present[focus] ? &ui->knobs[focus] : NULL);

    /* bank 行（SOUND 页；仅选中轨引擎为 FM6 时可见，A10）：目标槽 `B<n+1>` + 状态行
     * （最后一次 bank 动作 × store_status，与 FILE 页同一映射）。可见性变化 = 上下文切换：
     * 整行显隐（对象常驻，零分配）并清掉上一次动作的结果。脏检查：槽号变化写槽文本，
     * 三元组 (act, status, ready) 变化写状态行——30ms 一拍无条件重设文本会让 LVGL 持续
     * 失效重绘。行隐藏时不刷文本（落在这条带的 TAP 不命中）。 */
    if (bank_row_present(pg) && ui->bank_label != NULL) {
        bool vis = bank_row_visible(pg, st);
        if (ui->bank_vis_shown != (int8_t)vis) {
            /* 行出现/消失 = 上下文切换：清掉上一次动作的结果，槽文本/状态行强制重写 */
            lv_obj_t *objs[] = {ui->bank_label, ui->bank_value, ui->bank_dec, ui->bank_inc,
                                ui->bank_save, ui->bank_erase, ui->bank_status};
            for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
                lv_obj_set_hidden(objs[i], !vis);
            }
            ui->bank_vis_shown = (int8_t)vis;
            ui->bank_shown_slot = 0xFF;
            ui->bank_status_key = 0xFFFFFFFFu;
            s_bank_act = 0xFF;
        }
        if (vis && ui->bank_shown_slot != s_bank_slot) {
            char text[8];
            snprintf(text, sizeof(text), "B%u", (unsigned)(s_bank_slot + 1u));
            lv_label_set_text(ui->bank_value, text);
            ui->bank_shown_slot = s_bank_slot;
        }
        bool ready = sloop_store_ready();
        uint32_t key =
            (uint32_t)s_bank_act | ((uint32_t)st->store_status << 8) | ((uint32_t)(ready ? 1u : 0u) << 16);
        if (vis && key != ui->bank_status_key) {
            ui->bank_status_key = key;
            if (s_bank_act > 2u) {
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
    uint8_t chip = chip_get(index);
    uint8_t slot = focus_get(index);
    int16_t applied = 0;
    bool ok = param_step(index, delta, &applied); /* 锁定槽：false ⇒ PARAM_LOCKED（未下发） */
    if (out != NULL) {
        out->a = slot;
        out->b = ok ? slot_abs_id(pg, chip, slot) : 0u;
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
        out->a = page_layout_get(index)->page_id; /* SOUND（页 id 沿用 APP_PAGE_ANALOG） */
        out->b = s_bank_slot;
        out->d = delta;
    }
}

static void bank_tap_action(uint8_t index, uint8_t act, sloop_tap_result_t *out)
{
    bank_action(index, act);
    if (out != NULL) {
        out->kind = act == 0u ? SLOOP_TAP_BANK_STORE : SLOOP_TAP_BANK_ERASE;
        out->a = page_layout_get(index)->page_id;
        out->b = s_bank_slot;
        out->v = act;
    }
}

static void sample_tap_slot(uint8_t index, uint8_t slot, sloop_tap_result_t *out)
{
    bool ok = sample_select(index, slot);
    if (out != NULL) {
        out->kind = SLOOP_TAP_SAMPLE_SLOT;
        out->a = APP_PAGE_SAMPLE;
        /* 页面拒绝（锁定页/描述符不可用）不伪造槽号：b=0xFF、v=0，console 回
         * `slot=- select status="…"`（与 BANK 行的 status 回显同形）。 */
        out->b = ok ? slot : 0xFFu;
        out->v = ok ? 1 : 0;
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
    /* SOUND 页眉行（仅 SOUND 页；矩形与建控件同源）：左右区 = ∓1，中段是标签 ⇒ miss */
    if (page_layout_is_sound(pg) && ui->sound_row[0] != NULL && ui->sound_rows_shown > 0) {
        for (uint8_t row = 0; row < (uint8_t)ui->sound_rows_shown; row++) {
            int16_t rx = page_layout_sound_row_x(row);
            int16_t rw = page_layout_sound_row_w(row);
            if (!rect_hit(x, y, rx, SOUND_ROW_Y, rw, SOUND_ROW_H)) continue;
            int32_t rel = x - rx;
            if (rel >= SOUND_ROW_ZONE && rel < rw - SOUND_ROW_ZONE) return false;
            if (sound_step(index, row, rel < SOUND_ROW_ZONE ? -1 : 1, out)) refresh(index);
            return true;
        }
    }
    /* chip 行（仅 chip 页；矩形与绘制同源，隐藏 chip 的那一格是空位 ⇒ 落到后面 = miss）：
     * 切生效 chip（零分配重配）。 */
    if (pg->chip_count > 1u && ui->chip_row != NULL &&
        rect_hit(x, y, CHIP_X0, CHIP_Y, chip_row_width(pg), CHIP_H)) {
        for (uint8_t c = 0; c < pg->chip_count; c++) {
            if ((ui->chip_vis_shown & (1u << c)) == 0) continue;
            int16_t cx = (int16_t)(CHIP_X0 + chip_x_at(pg, c));
            if (!rect_hit(x, y, cx, CHIP_Y, page_layout_chip_width(pg->chips[c].name), CHIP_H)) {
                continue;
            }
            bool sel = param_chip_select(index, c);
            if (out != NULL) {
                out->kind = SLOOP_TAP_PARAM_CHIP;
                out->a = pg->page_id;
                out->b = sel ? c : 0xFFu;
            }
            return true;
        }
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

    /* bank 行（SOUND 页且引擎为 FM6 ⇒ 行可见；否则整行隐藏，落在这里的 TAP = miss）：
     * 槽 ± 只改目标槽，SAVE/ERASE 发状态层命令 */
    if (bank_row_present(pg) && ui->bank_label != NULL && ui->bank_vis_shown == 1) {
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
    s_sample_act = 0xFF;
    s_sample_lock_refused = 0;
    s_sample_status = UI_SAMPLE_STATUS_NONE;
    s_sample_info_valid = false;
    s_sample_row_sig = 0xFFFFFFFFu;
}

const char *page_params_bank_status_text(uint8_t index)
{
    if (index >= PARAM_PAGE_COUNT || !bank_row_present(page_layout_get(index))) return NULL;
    /* 行未建（页未构建/已随源组销毁）时没有可回显的页面状态 */
    /* 行未建，或选中轨不是 FM6 引擎（行隐藏、没有命中区）时没有可回显的页面状态 */
    if (s_ui[index].bank_label == NULL || s_ui[index].bank_vis_shown != 1) return NULL;
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
    return ui_sample_status_text(s_sample_echo, sizeof(s_sample_echo), (uint8_t)s_sample_status,
                                 s_sample_sel_slot);
}

/* ---------------------------------------------------------------- chip 公共入口 --- */
/* 生效 chip（console `UI <page> CHIP` 的读；条件 chip 隐藏时 = 夹后的首个可见 chip）。
 * 只读状态层快照与布局表、写页内选择（临界区），不碰 LVGL 对象。 */
uint8_t page_params_chip_get(uint8_t index)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL) return 0;
    app_state_t st;
    state_get_copy(&st);
    return chip_effective(index, pg, &st);
}

/* 写 chip 选择（调用方已持 LVGL 锁：已建页立即重配刷新）。非 chip 页/越界回
 * INVALID_ARG；条件 chip 在当前上下文隐藏回 NOT_ALLOWED 且不改选择。 */
esp_err_t page_params_chip_set(uint8_t index, uint8_t chip)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL || pg->chip_count <= 1u) return ESP_ERR_INVALID_ARG;
    if (chip >= pg->chip_count) return ESP_ERR_INVALID_ARG;
    if (s_ui[index].header != NULL) {
        return param_chip_select(index, chip) ? ESP_OK : ESP_ERR_NOT_ALLOWED;
    }
    /* 页未建/已随源组销毁：只校验并写选择，建页时由 update 首拍落地 */
    app_state_t st;
    state_get_copy(&st);
    bool drum;
    uint8_t engine;
    sel_context(&st, &drum, &engine);
    if (!page_layout_chip_visible(&pg->chips[chip], drum, engine)) return ESP_ERR_NOT_ALLOWED;
    chip_set(index, chip);
    return ESP_OK;
}

/* 槽位诊断（console `UI <page> CHIP` 的逐槽行）：id = 状态层命令里的绝对 id；label =
 * 旋钮**正在显示**的名称（已建页取 knob 的 desc，未建页取描述符——同一个描述符来源）；
 * locked = 显示 -- 且拒绝编辑。槽不存在回 false。读 desc 不取 LVGL 锁：label 指向上游
 * 静态串，UI 任务重配只换指针（32 位对齐读是原子的），最坏读到相邻拍的描述符。 */
bool page_params_slot_info(uint8_t index, uint8_t chip, uint8_t slot, uint8_t *id,
                           const char **label, bool *locked)
{
    const param_page_t *pg = page_layout_get(index);
    const param_slot_t *sl = page_layout_slot(page_layout_chip(pg, chip), slot);
    if (sl == NULL) return false;
    app_state_t st;
    state_get_copy(&st);
    sloop_desc_t d;
    const char *name = s_ui[index].present[slot] ? s_ui[index].knobs[slot].desc.label : NULL;
    if (name == NULL && slot_desc(pg, chip, slot, &d)) name = d.label;
    if (id != NULL) *id = slot_abs_id(pg, chip, slot);
    if (label != NULL) *label = name != NULL ? name : "-";
    if (locked != NULL) *locked = slot_locked(pg, chip, slot, &st);
    return true;
}

/* 槽位是否锁定（console/脚本读；chip 越界 = 锁定）。不取 LVGL 锁。 */
bool page_params_slot_locked(uint8_t index, uint8_t chip, uint8_t slot)
{
    const param_page_t *pg = page_layout_get(index);
    if (pg == NULL) return true;
    app_state_t st;
    state_get_copy(&st);
    return slot_locked(pg, chip, slot, &st);
}

/* ---------------------------------------------------------------- 注册表胶水 --- */
/* 布局表的每页 build/update 包装（下标 0..PARAM_PAGE_COUNT-1）与函数指针表：页 id/
 * 标题/est 不在这里重复，注册时从 page_layout_get() 取。 */
#define PARAM_PAGE_BUILDERS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6)

#define X_BUILD(n)                                                                       \
    static lv_obj_t *params_build_##n(lv_obj_t *parent)                                \
    {                                                                                    \
        return page_params_build(n, parent);                                            \
    }                                                                                    \
    static void params_update_##n(const app_state_t *st) { page_params_update(n, st); }
PARAM_PAGE_BUILDERS(X_BUILD)

#define X_BUILD_FN(n) [n] = params_build_##n,
static lv_obj_t *(*const k_build[PARAM_PAGE_COUNT])(lv_obj_t *) = { PARAM_PAGE_BUILDERS(X_BUILD_FN) };
#define X_UPDATE_FN(n) [n] = params_update_##n,
static void (*const k_update[PARAM_PAGE_COUNT])(const app_state_t *) = {
    PARAM_PAGE_BUILDERS(X_UPDATE_FN)
};

esp_err_t page_layout_register(void)
{
    static ui_page_t k_entries[PARAM_PAGE_COUNT];
    esp_err_t first = ESP_OK;
    for (uint8_t i = 0; i < PARAM_PAGE_COUNT; i++) {
        const param_page_t *pg = page_layout_get(i);
        if (pg == NULL) continue;
        k_entries[i] = (ui_page_t){.id = pg->page_id, .title = pg->title, .build = k_build[i],
                                   .update = k_update[i], .est_bytes = pg->est_bytes};
        esp_err_t ret = ui_page_register(&k_entries[i]);
        if (ret != ESP_OK && first == ESP_OK) first = ret;
    }
    return first;
}
