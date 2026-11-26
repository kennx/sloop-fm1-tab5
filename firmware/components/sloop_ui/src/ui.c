/*
 * sloop_ui — 分组页签 + 页面注册表 + 懒建（ADR-008）+ 组作用域生命周期（ADR-005）
 *            + 30ms 状态同步/页面刷新
 *
 * 布局：页签条 y∈[0,72) 内两行（ADR-005「导航决策」）——上行组签
 * PLAY|TRACKS|EDIT|MIX|SYNTH|SEQ|SONG|FILE|SYS（y∈[4,36)，无已注册页的组不显示；组表
 * = ui_nav.h 的 UI_GROUP_LIST，P4-F Task 4 起 9 组 104px），下行当前组的子签
 * （y∈[38,68)，固定 5 槽复用）；页面区仍是 y∈[72,720)。每页控件在**首次
 * 进入时构建**（PLAY 在 start 内构建，保证开机首帧非空白），同组内之后只改
 * LV_OBJ_FLAG_HIDDEN；**离开组时销毁源组全部已建页**（含原当前页），于是跨组
 * 切换的峰值 = 源组已建 Σ + 目标页 est（瞬态，由 ADR-005 §5 的不变量覆盖），
 * 稳态只保留一个组的页面。PLAY（page_play.c）/ TRACKS（page_tracks.c）/
 * STEP（page_step.c）/ TRANSPORT（page_transport.c）/ 参数页
 * SOUND|MOD|SCALE|VOICE（EDIT 组）与 MIX 组的 GLOBAL|FX（page_params.c + knob.c，
 * 布局表 page_layout.c；MOD/SCALE/VOICE 是 chip 页，P4-F Task 4）/
 * SONG（page_song.c，P4-C Task 11）/ DSYN（page_dsyn.c，P4-D Task 9）/ FM6P
 * （page_fm6p.c，P4-D Task 10）/ FILE 组 PROJECTS|PRESETS（page_projects.c /
 * page_presets.c，P4-B Task 12）/ SYSTEM（page_system.c，P4-E Task 6）都在组表
 * k_groups[] 里声明；组顺序 = 子签顺序 =
 * `TAB` 扁平轮转顺序。P4-D Task 8 起 SAMPLE 随 SYNTH 组（SAMPLE/DSYN/FM6P），
 * Task 9/10 落两页实体页面。
 *
 * 懒建与失败可见性：构建前用 lv_mem_monitor 取池剩余，低于该页 est_bytes 即
 * 拒绝；构建中任一控件分配失败（build 返回 NULL）则删除半成品容器、记 ERROR，
 * 并在页面区显示错误状态条（`PAGE <TITLE> BUILD FAILED` + `UI ERR page_build <id>`）。
 * 同步切页把构建失败上报为 ESP_ERR_NO_MEM（console 打印 `UI ERR page_build <id>`）。
 * 失败页下次访问重试；`UI FORCEFAIL <page>` 可一次性强制失败（验证用）。
 * 池增量/构建耗时进日志与 `UI MEM`（sloop_ui_page_pool_stats，按页 + 按组）。
 *
 * 线程纪律：所有 LVGL 调用在 bsp_display_lock 内——公共入口自行加锁，
 * 页签回调与 30ms 定时器运行在 LVGL 任务上下文中（esp_lvgl_port 的
 * lvgl_port_task 已持递归锁），不再重复加锁，与 display.c 的约定一致。
 * 定时器每 30ms 先 state_sync_from_engine() 拉引擎真值，再对**已建**页做脏检查
 * 刷新（未建/构建失败的页不刷新；切换时立即刷新一次，不等下一拍）。
 *
 * InputManager（input.c）在 sloop_ui_start 末尾启动：按 tick 地板轮询 Tab5 Keyboard
 * 事件（100Hz tick 下 10ms）→ 层模型 → EditCmd；页签条右侧的 LAYER 提示由本文件的定时器按
 * state.layer_held 刷新。
 *
 * 注入（console）：`TAP <x> <y>` 经 sloop_ui_input_tap() 按当前页 hit-test 表派发，
 * 与控件回调共用页内动作函数（EditCmd 层等价，ADR-005）；y < 72（页签条）一律
 * miss——页签切换走 LVGL 回调/`UI`/`KEY tab`，不进 TAP 路由。当前页未建/构建失败时
 * 一律 miss（不派发）。`UI <page>` 为同步切页，会先作废异步切页邮箱
 * （sloop_ui_cancel_pending_page），保证回显页即落地页。异步切页按「一拍一页」：
 * 同一定时器周期内的多次 tab 只保留最后一次请求的目标页。
 *
 * 诊断期静默（sloop_ui_quiesce）：`ENG SELFTEST` 全量运行期间暂停 30ms 定时器与
 * LVGL 定时器、让输入任务阻塞丢弃事件（TAP/KEY 注入被拒）——诊断期 UI 明确暂停，
 * 不与自检并发（spec P4-0 口径）。结束后恢复定时器，下一拍重新 state_sync 并刷新
 * 已建页（ADR-007）。
 */
#include "sloop_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "knob.h"
#include "page_dsyn.h"
#include "page_fm6p.h"
#include "page_layout.h"
#include "page_params.h"
#include "page_sound.h"
#include "page_system.h"
#include "page_tools.h"
#include "sound_layout.h"
#include "sloop_core.h"
#include "sloop_hal.h"
#include "sloop_state.h"
#include "sloop_store.h"
#include "theme.h"

static const char *TAG = "sloop_ui";

/* 内置页面注册表（ADR-008）：注册顺序 = 组内子签顺序。est_bytes 为构建前预算门槛
 * （池剩余低于该值拒绝构建），数值 = 实测池 free 减少量 + 10–26% 余量、512 对齐
 * （ADR-008 预算表；STEP 因 9 页末页自由量见下的例外取 +5.4%）；预算门槛的
 * 真口径是逐页 `free_size >= est`，不是「Σ est < 池总量」（ADR-008 §5）。
 * STEP 9 页口径：est = 23,040（实测 delta 21,484–21,864B，+5.4%）——「其它八页
 * 建完、STEP 最后建」时 free 实测 24,044–24,272B（P4-A 终审修复轮：params 场景
 * 顺序下 24,044B），delta+10% 的 24,064 会差 20B 被预算误拒（实际 delta 装得下）。
 * 组作用域（ADR-005）之后同时已建页 ≤ 单组成员（EDIT 4 页 Σest 16,896B；SYNTH 3 页
 * Σest 20,480B——P4-D Task 8 把 SAMPLE 移入 SYNTH 并新增 DSYN/FM6P；FM6 自 Task 13 起
 * 带 bank 存取行、est 6,144B），九页平铺的容量天花板不再是约束。
 * TRANSPORT（P4-C Task 10）：est = **计划上限 9,216B**（SEQ→PRESETS 有序对把上限
 * 锁死，见 ADR-005 §5）；实测 delta 8,248–8,716B（对最大 +5.7%，低于常态 +10–23%
 * ——与 FILE 两页同类例外，理由是把 est 抬到实测 +10% 会顶破 63,120B；ADR-008 §3 注）。 */
static const ui_page_t k_builtin_pages[] = {
    {APP_PAGE_PLAY, "PLAY", page_play_build, page_play_update, 3072},
    {APP_PAGE_TRACKS, "TRACKS", page_tracks_build, page_tracks_update, 7168},
    {APP_PAGE_STEP, "STEP", page_step_build, page_step_update, 23040},
    {APP_PAGE_TRANSPORT, "TRANSPORT", page_transport_build, page_transport_update, 9216},
    /* FILE 组（P4-B Task 12）：est = 实测首次构建 delta + 5.5%/4.4%、512 对齐
     * （PROJECTS 7,764B → 8,192；PRESETS 12,260B → 12,800；ADR-008 §3 表）。两页都
     * 不含 lv_button 的子 label——按钮/格子用单对象（见各页头部的内存注），组预算下
     * FILE 与 SEQ 的最坏有序对（ADR-005 §5）才留得住余量。 */
    {APP_PAGE_PROJECTS, "PROJECTS", page_projects_build, page_projects_update, 8192},
    {APP_PAGE_PRESETS, "PRESETS", page_presets_build, page_presets_update, 12800},
    /* SONG（P4-C Task 11）：独立顶层组，单页。est = spec §4.4 的计划上限
     * **12,288B**（35 个单对象 label：状态 1 + 4 卡 + 8 动作 chip + 16 条目 +
     * 6 配置 chip；实测 delta 见 ADR-008 §3 的 SONG 行）。est 是构建前门槛，
     * 不是精确账本；实测超限先改对象设计，不抬 est/不扩池。 */
    {APP_PAGE_SONG, "SONG", page_song_build, page_song_update, 12288},
    /* SYNTH 组三页（P4-D Task 8 注册，Task 9 落 DSYN、Task 10 落 FM6P 实体页）：
     * est = spec §4.4 的计划上限 **7,168B/页**（8 旋钮 + 页内选择器 + 状态行，约
     * 20–25 个对象）。DSYN/FM6P 实测 delta 见 ADR-008 §3 与各任务报告（≤ est）。 */
    {APP_PAGE_DSYN, "DSYN", page_dsyn_build, page_dsyn_update, 7168},
    {APP_PAGE_FM6P, "FM6P", page_fm6p_build, page_fm6p_update, 7168},
    {APP_PAGE_SYSTEM, "SYSTEM", page_system_build, page_system_update, 10240},
    /* TOOLS（P4-F Task 6，MIX 组第 3 张子签）：状态行 + 3 chip + 3 说明 + 提示共 8 个单对象
     * label，实测 delta 2,568–2,732B（≈320B/对象）。est = 实测 +10% 上取整到 512 的常规档
     * 3,584（ADR-008 预算表；控制器的 7,168 是上限）。 */
    {APP_PAGE_TOOLS, "TOOLS", page_tools_build, page_tools_update, 3584},
};

/* 组表（ADR-005「导航决策」）：组顺序 = 上行页签顺序 = `TAB` 扁平轮转顺序；
 * 组内顺序 = 下行子签顺序。没有已注册成员的组 group_size()==0：组签不显示、
 * `TAB` 也跳过（注册表是可发现性的唯一来源）。
 * 成员上限 UI_GROUP_MAX（= 子行可容纳的子签数，ui_nav.h 有静态断言）；
 * SYS 是 P4-E Task 6 的单页设置组。 */
typedef struct {
    const char *name;
    uint8_t count;
    uint8_t pages[UI_GROUP_MAX];
} ui_group_t;

/* 组表由 ui_nav.h 的 UI_GROUP_LIST 生成（一处定义；host 单测校验顺序/成员/每页可达）：
 * X(名, 成员页 id…)，成员数 = 实参数。 */
#define UI_NAV_NARG(...) UI_NAV_NARG_(__VA_ARGS__, 5, 4, 3, 2, 1, 0)
#define UI_NAV_NARG_(a, b, c, d, e, n, ...) n
#define X_GROUP(NAME, ...) \
    {.name = #NAME, .count = (uint8_t)UI_NAV_NARG(__VA_ARGS__), .pages = {__VA_ARGS__}},
static const ui_group_t k_groups[] = {UI_GROUP_LIST(X_GROUP)};

#define GROUP_COUNT ((uint8_t)(sizeof(k_groups) / sizeof(k_groups[0])))
_Static_assert(GROUP_COUNT == SLOOP_UI_GROUPS, "组表与 SLOOP_UI_GROUPS 不同步");
_Static_assert(GROUP_COUNT == UI_GROUP_COUNT, "组表与 UI_GROUP_COUNT 不同步");

static const ui_page_t *s_registry[APP_STATE_PAGES];
static uint8_t s_reg_count;

static lv_obj_t *s_root;
static lv_obj_t *s_tab_bar;
static lv_obj_t *s_title;
static lv_obj_t *s_zoom;
static const knob_t *s_zoom_knob;
static char s_zoom_text[64];
static bool s_zoom_visible;
static ui_theme_t s_theme;
static uint8_t s_applied_palette = 0xFF;
static uint8_t s_applied_theme_on;
static uint8_t s_applied_theme_valid;
static uint8_t s_applied_lights = 0xFF;
static uint32_t s_applied_accent = 0xFFFFFFFFu;
static lv_obj_t *s_group_tabs[GROUP_COUNT];  /* 上行组签（按组下标） */
static lv_obj_t *s_group_labels[GROUP_COUNT];
static lv_obj_t *s_sub_tabs[UI_GROUP_MAX];   /* 下行子签：固定槽复用，按组成员下标 */
static lv_obj_t *s_sub_labels[UI_GROUP_MAX];
static uint8_t s_group_last_sub[GROUP_COUNT]; /* 点组签进入的子页（0xFF = 未定 → 首个） */
static uint32_t s_group_build_ms[GROUP_COUNT]; /* 最近一次跨组进入/组内首建的耗时 */
static uint8_t s_group; /* 当前组下标（由 s_page 派生；0xFF = 当前页不属于任何组） */
static lv_obj_t *s_page_obj[APP_STATE_PAGES];   /* 按页 id 索引；NULL = 未建/失败 */
static uint8_t s_page_state[APP_STATE_PAGES];   /* sloop_ui_page_state_t */
static int32_t s_page_delta[APP_STATE_PAGES];   /* 最近一次成功构建的池 free 减少量（字节） */
static uint32_t s_page_build_ms[APP_STATE_PAGES]; /* 最近一次构建耗时（毫秒） */
static const char *s_page_fail_reason[APP_STATE_PAGES]; /* 最近一次构建失败原因 */
static bool s_forcefail[APP_STATE_PAGES];       /* UI FORCEFAIL：一次性注入 */
static lv_obj_t *s_page_error;                  /* 构建失败状态条（页面区居中） */
static lv_obj_t *s_layer_hint;
static uint8_t s_layer_shown = 0xFF; /* 脏检查（0xFF = 尚未刷新） */
static lv_timer_t *s_timer;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const ui_theme_t *ui_theme(void)
{
    return &s_theme;
}

/* 音符指示邮箱：输入层只写、30ms 定时器消费（见 sloop_ui_flash_note） */
static portMUX_TYPE s_flash_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_flash_note_pending;
static bool s_flash_has_pending;

/* 切页邮箱：输入层只记目标页、30ms 定时器执行（见 sloop_ui_request_page）。
 * 0xFF = 无待执行切页。 */
static portMUX_TYPE s_page_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_page_pending = 0xFF;
static uint8_t s_page;
static bool s_started;
static bool s_quiesced;
static bool s_indev_disabled; /* 诊断期是否由本函数整体禁用了 indev */

/* 无主题装饰的容器（root/页面/页签条共用） */
static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

/* 注册表查找：页 id → 注册下标；未注册返回 -1 */
static int registry_index(uint8_t page)
{
    for (uint8_t i = 0; i < s_reg_count; i++) {
        if (s_registry[i]->id == page) return (int)i;
    }
    return -1;
}

/* 页 id → 注册表条目；未注册返回 NULL */
static const ui_page_t *page_def(uint8_t page)
{
    int idx = registry_index(page);
    return idx < 0 ? NULL : s_registry[idx];
}

/* 页 id → 组下标；不属于任何组/未注册返回 -1 */
static int group_of(uint8_t page)
{
    for (uint8_t g = 0; g < GROUP_COUNT; g++) {
        for (uint8_t i = 0; i < k_groups[g].count; i++) {
            if (k_groups[g].pages[i] == page) return (int)g;
        }
    }
    return -1;
}

/* 组内已注册成员数（下行子签数；0 = 该组不显示） */
static uint8_t group_size(uint8_t g)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < k_groups[g].count; i++) {
        if (page_def(k_groups[g].pages[i]) != NULL) n++;
    }
    return n;
}

/* 组内第 idx 个已注册成员页 id；越界返回 0xFF */
static uint8_t group_member_at(uint8_t g, uint8_t idx)
{
    for (uint8_t i = 0; i < k_groups[g].count; i++) {
        uint8_t pid = k_groups[g].pages[i];
        if (page_def(pid) == NULL) continue;
        if (idx == 0) return pid;
        idx--;
    }
    return 0xFF;
}

static uint8_t sub_tab_count(uint8_t g);
static void update_layer_hint(uint8_t layer);

static void apply_lights(const app_state_t *st)
{
    if (s_applied_lights == st->settings.lights && s_applied_accent == UI_ACCENT) return;
    if (sloop_hal_keyboard_rgb(st->settings.lights, UI_ACCENT) == ESP_OK) {
        s_applied_lights = st->settings.lights;
        s_applied_accent = UI_ACCENT;
    }
}

/* 上行组签 + 下行子签的选中态与可见性（组签宽度按已注册组数排布）。页签对象固定
 * 创建，切换只改位置/文本/颜色。子行只显示**多页组**的成员（单页组留空，ADR-005
 * §2）——可见子签数直接取 sub_tab_count()（与命中/`UI SUB` 的单一来源）。 */
static void refresh_tabs(void)
{
    int8_t cur = group_of(s_page);
    uint8_t vis = 0;
    for (uint8_t g = 0; g < GROUP_COUNT; g++) {
        bool has = group_size(g) > 0;
        lv_obj_set_hidden(s_group_tabs[g], !has);
        if (!has) continue;
        lv_obj_set_pos(s_group_tabs[g], UI_TAB_X0 + (int32_t)vis * (UI_GROUP_W + UI_TAB_GAP),
                       UI_GROUP_Y);
        bool selected = cur >= 0 && (uint8_t)cur == g;
        lv_obj_set_style_bg_color(s_group_tabs[g],
                                  lv_color_hex(selected ? UI_ACCENT : UI_PANEL_HI), 0);
        lv_obj_set_style_text_color(s_group_labels[g],
                                    lv_color_hex(selected ? UI_BG : UI_TEXT_DIM), 0);
        vis++;
    }

    /* 子行只显示当前组的**多页**成员：单页组（PLAY/TRACKS）下行留空（ADR-005 §2），
     * 选中态由上行组签承担。 */
    uint8_t n = cur >= 0 ? sub_tab_count((uint8_t)cur) : 0;
    for (uint8_t i = 0; i < UI_GROUP_MAX; i++) {
        lv_obj_set_hidden(s_sub_tabs[i], i >= n);
        if (i >= n) continue;
        uint8_t pid = group_member_at((uint8_t)cur, i);
        const ui_page_t *def = pid < APP_STATE_PAGES ? page_def(pid) : NULL;
        if (def == NULL) continue; /* 注册表只增不删，理论不可达 */
        lv_obj_set_pos(s_sub_tabs[i], UI_TAB_X0 + (int32_t)i * (UI_SUB_W + UI_TAB_GAP), UI_SUB_Y);
        lv_label_set_text(s_sub_labels[i], def->title);
        bool selected = pid == s_page;
        lv_obj_set_style_bg_color(s_sub_tabs[i],
                                  lv_color_hex(selected ? UI_ACCENT : UI_PANEL_HI), 0);
        lv_obj_set_style_text_color(s_sub_labels[i],
                                    lv_color_hex(selected ? UI_BG : UI_TEXT_DIM), 0);
    }
}

/* 错误状态条：构建失败的可见出口，不留静默空白页（ADR-008） */
static void show_page_error(uint8_t page, const char *reason)
{
    const ui_page_t *def = page_def(page);
    if (s_page_error == NULL) return;
    lv_label_set_text_fmt(s_page_error, "%s PAGE BUILD FAILED\nUI ERR page_build %u (%s)",
                          def != NULL ? def->title : "?", (unsigned)page,
                          reason != NULL ? reason : "unknown");
    lv_obj_set_hidden(s_page_error, false);
    lv_obj_move_foreground(s_page_error);
}

/* 懒建（ADR-008）：未建页首次访问时构建；已建返回 ESP_OK；失败返回
 * ESP_ERR_NO_MEM（错误状态条已显示、ERROR 已记）。调用方已持 LVGL 锁。 */
static esp_err_t ensure_page_built(uint8_t page)
{
    if (page >= APP_STATE_PAGES) return ESP_ERR_INVALID_ARG;
    const ui_page_t *def = page_def(page);
    if (def == NULL) return ESP_ERR_INVALID_ARG;
    if (s_page_state[page] == SLOOP_UI_PAGE_BUILT) return ESP_OK;

    uint32_t t0 = now_ms();
    lv_mem_monitor_t mon_before;
    lv_mem_monitor(&mon_before);
    uint32_t free_before = (uint32_t)mon_before.free_size;

    const char *reason = NULL;
    lv_obj_t *obj = NULL;
    if (s_forcefail[page]) {
        s_forcefail[page] = false; /* 一次性注入：本次失败后自动解除 */
        reason = "forced";
    } else if (mon_before.free_size < def->est_bytes) {
        /* 预算门槛：剩余不足该页预算就拒绝，不冒险半途分配失败（碎片化池） */
        reason = "budget";
    } else {
        obj = make_plain(s_root);
        if (obj == NULL) {
            reason = "root";
        } else {
            lv_obj_set_pos(obj, 0, UI_TAB_BAR_H);
            lv_obj_set_size(obj, UI_W, UI_H - UI_TAB_BAR_H);
            lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
            if (def->build(obj) == NULL) reason = "build"; /* 页内控件分配失败 */
        }
    }

    if (reason != NULL) {
        if (obj != NULL) lv_obj_delete(obj); /* 回收半成品：失败页不占池 */
        s_page_obj[page] = NULL;
        s_page_state[page] = SLOOP_UI_PAGE_FAILED;
        s_page_fail_reason[page] = reason;
        s_page_build_ms[page] = now_ms() - t0;
        int idx = page_layout_index(page);
        if (idx >= 0) page_params_forget((uint8_t)idx); /* 半成品控件已删：本页指针置空 */
        if (page == APP_PAGE_DSYN) page_dsyn_forget();
        if (page == APP_PAGE_FM6P) page_fm6p_forget();
        if (page == APP_PAGE_SYSTEM) page_system_forget();
        ESP_LOGE(TAG, "页面构建失败 page=%s(%u) 原因=%s 池剩余=%uB 预算=%uB 耗时=%ums"
                 "（UI ERR page_build %u）",
                 def->title, (unsigned)page, reason, (unsigned)mon_before.free_size,
                 (unsigned)def->est_bytes, (unsigned)s_page_build_ms[page], (unsigned)page);
        return ESP_ERR_NO_MEM;
    }

    s_page_obj[page] = obj;
    s_page_state[page] = SLOOP_UI_PAGE_BUILT;
    s_page_fail_reason[page] = NULL;
    lv_mem_monitor_t mon_after;
    lv_mem_monitor(&mon_after);
    /* delta 取池 free_size 的减少量（含 TLSF 块头开销）；lv_mem_monitor 的
     * total_size 不含块头，用 total-free 差会低估每次分配的元数据。
     * 采样点在首帧 update() 之前：不含首帧文本分配（量级 = 文案长度，ADR-008）。 */
    int32_t delta = (int32_t)free_before - (int32_t)mon_after.free_size;
    s_page_delta[page] = delta;
    s_page_build_ms[page] = now_ms() - t0;
    ESP_LOGI(TAG, "页面构建 page=%s(%u) 池增量=%dB 预算=%uB 池剩余=%uB 耗时=%ums", def->title,
             (unsigned)page, (int)delta, (unsigned)def->est_bytes,
             (unsigned)mon_after.free_size, (unsigned)s_page_build_ms[page]);

    /* 立即按当前状态刷新一次：首帧不等 30ms 定时器（切页/截屏时序确定） */
    app_state_t st;
    state_get_copy(&st);
    def->update(&st);
    return ESP_OK;
}

/* 销毁一个组的全部已建页（组作用域生命周期，ADR-005 §3）：控件删除、对象指针
 * 置空、state 回 UNBUILT、delta/build_ms 归零——离开组后该组 built=0。
 * 调用方已持 LVGL 锁；`keep` 是目标页（防御：绝不该属于源组）。 */
static void destroy_group_pages(int g, uint8_t keep)
{
    if (g < 0) return;
    for (uint8_t i = 0; i < k_groups[g].count; i++) {
        uint8_t pid = k_groups[g].pages[i];
        if (pid == keep) continue;
        if (s_page_obj[pid] != NULL) lv_obj_delete(s_page_obj[pid]);
        s_page_obj[pid] = NULL;
        s_page_state[pid] = SLOOP_UI_PAGE_UNBUILT;
        s_page_delta[pid] = 0;
        s_page_build_ms[pid] = 0;
        s_page_fail_reason[pid] = NULL;
        /* 页面模块自己的控件指针也要置空：参数页的 bank 行状态访问器以「控件为 NULL」
         * 表示「行未建」（ADR-008 §8.6），否则会对着已删除的控件读状态。 */
        int idx = page_layout_index(pid);
        if (idx >= 0) page_params_forget((uint8_t)idx);
        if (pid == APP_PAGE_DSYN) page_dsyn_forget();
        if (pid == APP_PAGE_FM6P) page_fm6p_forget();
        if (pid == APP_PAGE_SYSTEM) page_system_forget();
    }
}

/* LVGL 上下文内调用（页签回调或已持锁的公共入口）。返回构建失败时的
 * ESP_ERR_NO_MEM（页面已切到错误状态条），其余 ESP_OK。
 *
 * 组作用域顺序固定（ADR-005 §3）：建目标页 → 切到目标页 → 销毁源组全部已建页
 * （含原当前页）。顺序不可交换：先销毁会出现空白帧，且跨组切换期间输入路径必须
 * 始终有可派发的活页；代价是切换瞬间的峰值 = 源组已建 Σ + 目标页 est（瞬态，
 * 由 ADR-005 §5 的不变量覆盖）。构建失败也照此销毁源组——否则两个组的页面会
 * 在错误态下并存，池预算失守。 */
static esp_err_t show_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES || page_def(page) == NULL) return ESP_ERR_INVALID_ARG;
    sloop_ui_zoom_knob(NULL);
    s_zoom_knob = NULL;
    /* 任何切页（含组签/子签回调）都作废同一 30ms 拍内排队的异步切页请求：tab 刚记下
     * 目标页、紧接着的手指点击必须赢（R30）。定时器路径上邮箱在调用前已被取走，
     * 这里是 no-op；同步 console 切页另有入口处的同一次清空（幂等）。 */
    sloop_ui_cancel_pending_page();
    int src = group_of(s_page);
    int dst = group_of(page);
    bool cross = (src >= 0 && dst != src) || (src < 0 && dst >= 0);
    bool first_build = s_page_state[page] != SLOOP_UI_PAGE_BUILT;
    uint32_t t0 = now_ms();

    esp_err_t ret = ensure_page_built(page);
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (s_page_obj[i] != NULL) lv_obj_set_hidden(s_page_obj[i], i != page);
    }
    if (cross) destroy_group_pages(src, page);
    if (ret == ESP_OK) {
        if (s_page_error != NULL) lv_obj_set_hidden(s_page_error, true);
    } else {
        show_page_error(page, s_page_fail_reason[page]);
    }
    s_page = page;
    s_group = dst >= 0 ? (uint8_t)dst : 0xFF;
    if (dst >= 0) s_group_last_sub[dst] = page; /* 点组签回到上次子页 */
    refresh_tabs();
    state_set_page(page); /* 页面是 UI 局部态（sloop_state 的 UI 写路径） */

    /* 构建延迟预算（≤100ms）只适用于跨组切换与组内首次构建兄弟页 */
    uint32_t dt = now_ms() - t0;
    if ((cross || first_build) && dst >= 0) s_group_build_ms[dst] = dt;
    if (cross || first_build) {
        ESP_LOGI(TAG, "切页 page=%s(%u) 组=%s %s耗时=%ums", page_def(page)->title,
                 (unsigned)page, dst >= 0 ? k_groups[dst].name : "-",
                 cross ? "跨组" : "组内首建", (unsigned)dt);
    }
    return ret;
}

void sloop_ui_rebuild_all(void)
{
    if (!s_started || s_root == NULL) return;
    uint8_t current = s_page;
    uint8_t temporary = current == APP_PAGE_PLAY ? APP_PAGE_TRACKS : APP_PAGE_PLAY;
    /* 与跨组切换完全同序：临时目标先建成活页，再销毁原组；返回时同理。 */
    (void)show_page(temporary);
    (void)show_page(current);
}

void sloop_ui_theme_apply(const sloop_settings_t *s)
{
    if (s == NULL) {
        ui_theme_derive_mono(&s_theme);
        return;
    }
    s_applied_palette = (uint8_t)s->f.palette;
    s_applied_theme_on = (uint8_t)s->f.theme_on;
    s_applied_theme_valid = (uint8_t)s->theme_valid;
    if (s->theme_valid && s->f.theme_on) {
        ui_theme_derive_theme(&s->theme, &s_theme);
    } else if (s->f.palette == 4u) {
        ui_theme_derive_mono(&s_theme);
    } else {
        ui_theme_derive_palette(ui_palette_ramps[s->f.palette < 4u ? s->f.palette : 0u], &s_theme);
    }
    if (s_root == NULL) return;
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_color(s_tab_bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_color(s_page_error, lv_color_hex(UI_ERROR), 0);
    refresh_tabs();
    uint8_t layer = s_layer_shown == 0xFF ? 0 : s_layer_shown;
    s_layer_shown = 0xFF;
    update_layer_hint(layer);
    lv_obj_invalidate(s_root);
    sloop_ui_rebuild_all();
}

/* 组签动作（物理点击与 console `UI TAB <group>` 共用，R32）：进入该组上次的子页
 * （未记录/已未注册则首个已注册成员）；空组（没有已注册页）返回 INVALID_ARG。
 * 调用方已持 LVGL 锁。 */
static esp_err_t group_select_locked(uint8_t g)
{
    if (g >= GROUP_COUNT || group_size(g) == 0) return ESP_ERR_INVALID_ARG;
    uint8_t page = s_group_last_sub[g];
    if (page >= APP_STATE_PAGES || page_def(page) == NULL || group_of(page) != (int)g) {
        page = group_member_at(g, 0);
    }
    if (page >= APP_STATE_PAGES) return ESP_ERR_INVALID_ARG;
    return show_page(page);
}

/* 可注入/可点击的子签数：单页组的子行留空（ADR-005 §2），故为 0——与视觉/命中一致。 */
static uint8_t sub_tab_count(uint8_t g)
{
    uint8_t n = group_size(g);
    return n >= 2 ? n : 0;
}

/* 子签动作（物理点击与 console `UI SUB <index>` 共用）：槽号在点击时解析到当前组的
 * 已注册成员（标签/槽位跨组复用）。调用方已持 LVGL 锁。 */
static esp_err_t sub_select_locked(uint8_t slot)
{
    int cur = group_of(s_page);
    if (cur < 0 || slot >= sub_tab_count((uint8_t)cur)) return ESP_ERR_INVALID_ARG;
    uint8_t page = group_member_at((uint8_t)cur, slot);
    if (page >= APP_STATE_PAGES) return ESP_ERR_INVALID_ARG;
    return show_page(page);
}

/* 物理页签回调：与 `UI TAB`/`UI SUB` 走同一动作函数（R32 的可覆盖性来源）。 */
static void group_clicked_cb(lv_event_t *e)
{
    (void)group_select_locked((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void sub_clicked_cb(lv_event_t *e)
{
    (void)sub_select_locked((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

/* 页签用**单对象可点击 label**（P4-C Task 11）：`lv_button + 子 label` 每个签两个
 * 对象（8 组签 + 5 子签 = 13 个多余对象，实测基础占用 +900B 量级）；label 自己当
 * 按钮省掉子 label，仍用共享样式表达选中态（与 FILE/TRANSPORT/SONG 页同一纪律）。
 * 文本 16pt 单行：pad_top 把行高 (~20px) 压到签高中心。 */
static lv_obj_t *make_tab_button(lv_obj_t *bar, int32_t w, int32_t h,
                                 lv_event_cb_t cb, void *user)
{
    lv_obj_t *tab = lv_label_create(bar);
    if (tab == NULL) return NULL;
    lv_obj_set_clickable(tab, true);
    lv_obj_set_size(tab, w, h);
    lv_obj_set_style_radius(tab, 10, 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(UI_PANEL_HI), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(tab, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tab, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_align(tab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(tab, (h - 20) / 2, 0);
    lv_label_set_text(tab, "");
    lv_obj_add_event_cb(tab, cb, LV_EVENT_CLICKED, user);
    return tab;
}

static bool create_tab_bar(void)
{
    lv_obj_t *bar = make_plain(s_root);
    if (bar == NULL) return false;
    s_tab_bar = bar;
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_W, UI_TAB_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* 上行组签：固定 GROUP_COUNT 槽（9 个；无已注册页的组由 refresh_tabs 隐藏并压缩排布） */
    for (uint8_t g = 0; g < GROUP_COUNT; g++) {
        lv_obj_t *btn = make_tab_button(bar, UI_GROUP_W, UI_GROUP_H, group_clicked_cb,
                                        (void *)(uintptr_t)g);
        if (btn == NULL) return false;
        lv_obj_set_pos(btn, UI_TAB_X0 + (int32_t)g * (UI_GROUP_W + UI_TAB_GAP), UI_GROUP_Y);
        lv_label_set_text(btn, k_groups[g].name);
        lv_obj_set_hidden(btn, true); /* refresh_tabs 按注册状态显示 */
        s_group_tabs[g] = btn;
        s_group_labels[g] = btn; /* 单对象：文本色与底色落在同一 label 上 */
    }

    /* 下行子签：固定 UI_GROUP_MAX 槽，标签/位置按当前组刷新（最大的组 EDIT 4 页，
     * MIX/SYNTH 3 页） */
    for (uint8_t i = 0; i < UI_GROUP_MAX; i++) {
        lv_obj_t *btn = make_tab_button(bar, UI_SUB_W, UI_SUB_H, sub_clicked_cb,
                                        (void *)(uintptr_t)i);
        if (btn == NULL) return false;
        lv_obj_set_pos(btn, UI_TAB_X0 + (int32_t)i * (UI_SUB_W + UI_TAB_GAP), UI_SUB_Y);
        lv_obj_set_hidden(btn, true);
        s_sub_tabs[i] = btn;
        s_sub_labels[i] = btn; /* 单对象（同上） */
    }

    /* SLOOP 标题留在上行右侧（9 个组签右端 1,016；标题从 ~1,191 起，不重叠） */
    lv_obj_t *title = lv_label_create(bar);
    if (title == NULL) return false;
    s_title = title;
    lv_label_set_text(title, "SLOOP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -24, 6);

    /* 层键提示（InputManager 写 layer_held，30ms 定时器刷新）：下行右侧。子签最多
     * 5 张，右端 928；提示盒 x∈[936,1170]（16pt、宽度封顶、LV_LABEL_LONG_DOT），
     * 四层键同按以省略号收尾，绝不压到子签上。 */
    int32_t tab_right = UI_TAB_X0 + (int32_t)UI_GROUP_MAX * (UI_SUB_W + UI_TAB_GAP) - UI_TAB_GAP;
    int32_t hint_w = (UI_W - 110) - tab_right - 8;
    if (hint_w < 0) hint_w = 0;
    s_layer_hint = lv_label_create(bar);
    if (s_layer_hint == NULL) return false;
    lv_label_set_text(s_layer_hint, "");
    lv_obj_set_style_text_font(s_layer_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_layer_hint, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_set_style_text_align(s_layer_hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_layer_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_layer_hint, hint_w, 20); /* 高度 = 一行 16pt：DOT 截断只在单行内，不换行 */
    lv_obj_align(s_layer_hint, LV_ALIGN_TOP_RIGHT, -110, 40);
    refresh_tabs();
    return true;
}

/* 构建失败状态条：页面区居中、默认隐藏（正常页面像素不受影响） */
static bool create_page_error_label(void)
{
    s_page_error = lv_label_create(s_root);
    if (s_page_error == NULL) return false;
    lv_label_set_text(s_page_error, "");
    lv_obj_set_width(s_page_error, UI_W);
    lv_obj_set_style_text_align(s_page_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_page_error, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_page_error, lv_color_hex(UI_ERROR), 0);
    lv_obj_set_pos(s_page_error, 0, UI_TAB_BAR_H + (UI_H - UI_TAB_BAR_H) / 2 - 40);
    lv_obj_set_hidden(s_page_error, true);
    return true;
}

/* 内置页注册（幂等）。注册表只增不删：sloop_ui_start() 之前由外部
 * ui_page_register() 注册的页必须保留，不能被内置注册重跑清空（ADR-008 扩展点）。
 * 框架失败后重试 start 也不会重复注册（done 标志 + 按 id 去重）。 */
static void register_builtin_pages(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (size_t i = 0; i < sizeof(k_builtin_pages) / sizeof(k_builtin_pages[0]); i++) {
        esp_err_t ret = ui_page_register(&k_builtin_pages[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "内置页注册失败 id=%u: %s", (unsigned)k_builtin_pages[i].id,
                     esp_err_to_name(ret));
        }
    }
    /* 参数页（GLOBAL/FX/SOUND/SAMPLE + chip 页 MOD/SCALE/VOICE）：布局表与注册表
     * 条目同源（page_layout.c 的 X 宏表，注册胶水在 page_params.c）。注册顺序只决定
     * `UI MEM` 的行序；页签/`TAB` 顺序由组表 k_groups[] 决定 */
    esp_err_t ret = page_layout_register();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "参数页注册失败: %s", esp_err_to_name(ret));
    }
}

esp_err_t ui_page_register(const ui_page_t *page)
{
    if (page == NULL || page->title == NULL || page->build == NULL || page->update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (page->id >= APP_STATE_PAGES) return ESP_ERR_INVALID_ARG;
    if (s_started) return ESP_ERR_INVALID_STATE;
    if (s_reg_count >= APP_STATE_PAGES) return ESP_ERR_NO_MEM;
    if (page_def(page->id) != NULL) return ESP_ERR_INVALID_ARG;
    s_registry[s_reg_count] = page;
    s_reg_count++;
    return ESP_OK;
}

static void update_layer_hint(uint8_t layer)
{
    if (layer == s_layer_shown) return;
    s_layer_shown = layer;
    static const struct {
        uint8_t bit;
        const char *name;
    } k_names[] = {
        {APP_LAYER_FX, "FX"},
        {APP_LAYER_EDIT, "EDIT"},
        {APP_LAYER_SEL, "SEL"},
        {APP_LAYER_GLO, "GLO"},
    };
    char buf[32] = "";
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if ((layer & k_names[i].bit) == 0) continue;
        if (buf[0] == '\0') {
            snprintf(buf, sizeof(buf), "LAYER %s", k_names[i].name);
        } else {
            strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, k_names[i].name, sizeof(buf) - strlen(buf) - 1);
        }
    }
    lv_label_set_text(s_layer_hint, buf);
    lv_obj_set_style_text_color(s_layer_hint,
                                lv_color_hex(layer != 0 ? UI_ACCENT : UI_TEXT_DIM), 0);
}

void sloop_ui_zoom_knob(const knob_t *k)
{
    const sloop_settings_t *settings = sloop_store_settings();
    bool knob_page = page_layout_index(s_page) >= 0 || s_page == APP_PAGE_DSYN ||
                     s_page == APP_PAGE_FM6P;
    /* 同组兄弟页也会在 30ms 拍更新：非当前页的 knob 不得覆盖/隐藏当前页读数。 */
    if (k != NULL && k->obj != NULL && lv_obj_get_parent(k->obj) != s_page_obj[s_page]) return;
    if (k == NULL || !settings->f.zoom || !knob_page || !k->active || k->obj == NULL) {
        if (s_zoom != NULL && s_zoom_visible) {
            lv_obj_set_hidden(s_zoom, true);
            s_zoom_visible = false;
        }
        return;
    }
    if (s_zoom == NULL) {
        s_zoom = lv_label_create(lv_screen_active());
        if (s_zoom == NULL) return;
        s_zoom_visible = false;
        lv_obj_set_pos(s_zoom, 0, UI_TAB_BAR_H);
        lv_obj_set_size(s_zoom, UI_W, 40);
        lv_obj_set_style_text_font(s_zoom, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(s_zoom, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(s_zoom, 2, 0);
        lv_obj_set_style_bg_opa(s_zoom, LV_OPA_70, 0);
    }
    char value[24];
    if (k->text_cb == NULL || k->text_cb(k, k->value, value, sizeof(value)) <= 0) {
        knob_format_value(&k->desc, k->value, value, sizeof(value));
    }
    char text[64];
    snprintf(text, sizeof(text), "%s  %s", k->desc.label != NULL ? k->desc.label : "?", value);
    if (s_zoom_knob != k || strcmp(text, s_zoom_text) != 0) {
        lv_label_set_text(s_zoom, text);
        snprintf(s_zoom_text, sizeof(s_zoom_text), "%s", text);
        s_zoom_knob = k;
        lv_obj_set_style_text_color(s_zoom, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_style_bg_color(s_zoom, lv_color_hex(UI_PANEL), 0);
    }
    if (!s_zoom_visible) {
        lv_obj_set_hidden(s_zoom, false);
        lv_obj_move_foreground(s_zoom);
        s_zoom_visible = true;
    }
}

/* 已建页刷新：UI 拍里**唯一**的一份状态副本放在这个独立函数的帧里，不留在
 * ui_timer_cb——后者还要调 state_sync_from_engine()（自带一份引擎快照 + 参数数组）与
 * show_page()（自带一份状态副本，页面构建还可能再取）。当时 LVGL 任务栈是 7,168B，三份
 * ~1.7KB 的副本叠起来后余量不足 0.5KB（P4-F Task 5 用高水位实测；再多一份即
 * Stack protection fault）——评审 R10 之后栈是 10,240B，副本份数的限制不变。 */
static uint32_t s_lvgl_stack_free; /* LVGL 任务栈的历史最小余量（字节；0 = 尚未跑过 UI 拍） */

static void ui_refresh_built(void)
{
    app_state_t st;
    state_get_copy(&st);
    apply_lights(&st);
    update_layer_hint(st.layer_held);

    /* 只刷新已建页：懒建页在 ensure_page_built 里已按最新状态刷过首帧 */
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (s_page_state[i] != SLOOP_UI_PAGE_BUILT) continue;
        const ui_page_t *def = page_def(i);
        if (def != NULL) def->update(&st);
    }
}

/* LVGL 任务上下文（持锁）：先同步引擎真值，再消费音符指示/待切页，最后经
 * ui_refresh_built() 刷新已建页。 */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* LVGL 任务栈余量（console `UI MEM` 的 lvgl_stack_free）：本回调恒在 LVGL 任务里跑，
     * 每拍在任务内取高水位。内核按 StackType_t 计数；ESP-IDF 的 RISC-V 端口里它是
     * uint8_t（portmacro.h），乘 sizeof(StackType_t) 即字节。点击路径
     * （页眉行/±/chip/bank 的控件回调）也跑在本任务里，app_state_t ≈1.7KB 的副本叠到
     * 第三份就压穿——ADR-008 §8.10。 */
    s_lvgl_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * (uint32_t)sizeof(StackType_t);
    state_sync_from_engine();
    const sloop_settings_t *settings = sloop_store_settings();
    if (s_applied_palette != settings->f.palette || s_applied_theme_on != settings->f.theme_on ||
        s_applied_theme_valid != settings->theme_valid) {
        sloop_ui_theme_apply(settings);
    }

    bool flash = false;
    uint8_t note = 0;
    portENTER_CRITICAL(&s_flash_mux);
    flash = s_flash_has_pending;
    note = s_flash_note_pending;
    s_flash_has_pending = false;
    portEXIT_CRITICAL(&s_flash_mux);
    /* 未建/构建失败的 PLAY 页没有指示控件可点亮（失败页下次访问重试） */
    if (flash && s_page_state[APP_PAGE_PLAY] == SLOOP_UI_PAGE_BUILT) page_play_flash(note);

    /* 待执行切页（输入/console 只记目标页，切页与渲染都在本任务上下文） */
    uint8_t page = 0xFF;
    portENTER_CRITICAL(&s_page_mux);
    page = s_page_pending;
    s_page_pending = 0xFF;
    portEXIT_CRITICAL(&s_page_mux);
    if (page < APP_STATE_PAGES && page != s_page) (void)show_page(page);

    /* 状态副本与切页/构建分属两个帧（见 ui_refresh_built 的注） */
    ui_refresh_built();
}

esp_err_t sloop_ui_start(void)
{
    if (s_started) return ESP_OK;
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "LVGL 锁超时，UI 未启动");
        return ESP_ERR_TIMEOUT;
    }

    /* 注册表只增不删：内置页注册幂等，外部预注册的页保持原位（ADR-008） */
    register_builtin_pages();
    /* 已注册但不在 k_groups[] 的页：页签与 `TAB` 都到不了（只能 console `UI` 直达）。
     * 这是注册表与组表的契约错误，构建时用 WARN 暴露（不阻止启动，页仍可直达）。 */
    for (uint8_t i = 0; i < s_reg_count; i++) {
        if (group_of(s_registry[i]->id) < 0) {
            ESP_LOGW(TAG, "页 %s(%u) 不在 k_groups[] 中：页签/`TAB` 无法到达",
                     s_registry[i]->title, (unsigned)s_registry[i]->id);
        }
    }
    memset(s_page_obj, 0, sizeof(s_page_obj));
    memset(s_page_state, 0, sizeof(s_page_state));
    memset(s_page_delta, 0, sizeof(s_page_delta));
    memset(s_page_build_ms, 0, sizeof(s_page_build_ms));
    memset(s_forcefail, 0, sizeof(s_forcefail));
    memset(s_page_fail_reason, 0, sizeof(s_page_fail_reason));
    memset(s_group_build_ms, 0, sizeof(s_group_build_ms));
    for (uint8_t g = 0; g < GROUP_COUNT; g++) s_group_last_sub[g] = 0xFF;
    s_group = (uint8_t)group_of(APP_PAGE_PLAY);
    s_page = APP_PAGE_PLAY;
    s_page_error = NULL;
    s_layer_hint = NULL;
    s_tab_bar = NULL;
    s_title = NULL;
    s_zoom = NULL;
    s_zoom_knob = NULL;
    s_zoom_visible = false;
    sloop_ui_theme_apply(sloop_store_settings());

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); /* 移除 bring-up 测试图；失败回退由调用方重画 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    bool ok = false;
    s_root = make_plain(scr);
    if (s_root != NULL) {
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_size(s_root, UI_W, UI_H);
        lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        ok = create_tab_bar() && create_page_error_label();
    }

    if (ok) {
        /* PLAY 启动即建：保证开机首帧非空白；其余页首次访问时懒建（ADR-008）。
         * 单页构建失败不算框架失败——错误状态条已显示，UI 仍可用。 */
        esp_err_t page_ret = show_page(APP_PAGE_PLAY);
        if (page_ret != ESP_OK) {
            ESP_LOGE(TAG, "PLAY 页启动构建失败（错误状态条已显示）: %s",
                     esp_err_to_name(page_ret));
        }
        s_timer = lv_timer_create(ui_timer_cb, 30, NULL);
        ok = s_timer != NULL;
    }
    if (!ok) {
        /* 框架级对象创建失败（内存不足）：清空控件树，交调用方回退测试图 */
        if (s_timer != NULL) {
            lv_timer_delete(s_timer);
            s_timer = NULL;
        }
        lv_obj_clean(scr);
        s_root = NULL;
        s_page_error = NULL;
        s_layer_hint = NULL;
        memset(s_page_obj, 0, sizeof(s_page_obj));
        memset(s_page_state, 0, sizeof(s_page_state));
        bsp_display_unlock();
        ESP_LOGE(TAG, "UI 对象创建失败（LVGL 内存不足）");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;

    /* 诊断期静默钩子：`ENG SELFTEST` 全量运行期间暂停本定时器与输入派发
     * （UI 与自检共存，见 ADR-007 / DI-11）。注册在 selftest 的快照窗口之外
     * （sloop_core 把指针放 .noinit），否则自检开始的快照回滚会把它抹掉。 */
    sloop_core_diag_quiesce_hook(sloop_ui_quiesce);

    /* LVGL 对象走内置 TLSF 池（CONFIG_LV_USE_STDLIB_MALLOC=0），池耗尽会静默
     * 建不出对象——STEP 网格 64 格是这个池的最大用户，就绪行打注册/已建页数与
     * 池占用（懒建另在每页构建时打增量）。池遍历必须持 LVGL 锁：30ms 定时器已
     * 创建，LVGL 任务随时会分配文本/样式，无锁遍历会读到撕裂的池结构。 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    uint8_t built = 0;
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (s_page_state[i] == SLOOP_UI_PAGE_BUILT) built++;
    }
    ESP_LOGI(TAG, "UI 就绪: %u 页注册/%u 已建 @%dx%d, LVGL 池已用 %u/%u 字节, 内部堆剩余 %u 字节",
             (unsigned)s_reg_count, (unsigned)built, UI_W, UI_H,
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bsp_display_unlock();

    /* 输入任务在 LVGL 锁外启动（面板就绪后）；失败只丢键盘输入，不影响 UI */
    esp_err_t input_ret = input_start();
    if (input_ret != ESP_OK) {
        ESP_LOGE(TAG, "输入任务启动失败: %s（触摸仍可用）", esp_err_to_name(input_ret));
    }
    return ESP_OK;
}

void sloop_ui_request_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES) return;
    portENTER_CRITICAL(&s_page_mux);
    s_page_pending = page;
    portEXIT_CRITICAL(&s_page_mux);
    /* 切页也是一次面板动作（自动存档的「无输入」计时；上游 ui_input_ms 同义）。 */
    sloop_store_note_activity((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
}

/* TAP 注入的页级派发：坐标为页面区相对坐标；PLAY 页无可点控件（纯呈现）。
 * 调用方已持 LVGL 锁（动作函数会改控件文本并立即刷新本页）。参数页（布局表）
 * 统一走 page_params_tap（弧线/± 按钮/数值文本）。 */
static bool page_tap(uint8_t page, int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (y < UI_TAB_BAR_H) return false;
    int16_t py = (int16_t)(y - UI_TAB_BAR_H);
    switch (page) {
    case APP_PAGE_STEP: return page_step_tap(x, py, out);
    case APP_PAGE_TRACKS: return page_tracks_tap(x, py, out);
    case APP_PAGE_TRANSPORT: return page_transport_tap(x, py, out);
    case APP_PAGE_PROJECTS: return page_projects_tap(x, py, out);
    case APP_PAGE_PRESETS: return page_presets_tap(x, py, out);
    case APP_PAGE_SONG: return page_song_tap(x, py, out);
    case APP_PAGE_DSYN: return page_dsyn_tap(x, py, out);
    case APP_PAGE_FM6P: return page_fm6p_tap(x, py, out);
    case APP_PAGE_SYSTEM: return page_system_tap(x, py, out);
    case APP_PAGE_TOOLS: return page_tools_tap(x, py, out);
    default: {
        int idx = page_layout_index(page);
        return idx >= 0 && page_params_tap((uint8_t)idx, x, py, out);
    }
    }
}

sloop_tap_result_status_t sloop_ui_input_tap(int16_t x, int16_t y, sloop_tap_result_t *out)
{
    if (out != NULL) *out = (sloop_tap_result_t){0};
    if (!s_started) return SLOOP_TAP_RESULT_NOT_STARTED;
    if (s_quiesced) return SLOOP_TAP_RESULT_QUIESCED; /* 诊断期：不派发、不取锁 */
    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "TAP 注入失败: LVGL 锁超时（未派发）");
        return SLOOP_TAP_RESULT_LOCK_TIMEOUT;
    }
    /* 未建/构建失败的页没有控件可派发：一律 miss，不碰页内静态控件指针 */
    bool hit = false;
    if (s_page < APP_STATE_PAGES && s_page_state[s_page] == SLOOP_UI_PAGE_BUILT) {
        hit = page_tap(s_page, x, y, out);
    }
    bsp_display_unlock();
    if (!hit) return SLOOP_TAP_RESULT_MISS;
    /* 自动存档的「无输入」计时：命中才算活动（上游按面板动作计），未命中不改任何状态 */
    sloop_store_note_activity((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    return SLOOP_TAP_RESULT_OK;
}

esp_err_t sloop_ui_switch_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES) {
        ESP_LOGW(TAG, "切页忽略: 非法页 %u", (unsigned)page);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        ESP_LOGW(TAG, "切页忽略: UI 未启动");
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "切页 %u 失败: LVGL 锁超时", (unsigned)page);
        return ESP_ERR_TIMEOUT;
    }
    /* 同步切页作废异步请求：否则紧邻的一次 tab/esc 会让定时器下一拍把页面切走
     * （console `UI` 的成功回显必须代表落地页）。清邮箱与 show_page 都在
     * LVGL 锁内，定时器无法插入；清空之后新的 request 属于更新的一次请求，保留。 */
    sloop_ui_cancel_pending_page();
    esp_err_t ret = show_page(page);
    bsp_display_unlock();
    return ret;
}

void sloop_ui_cancel_pending_page(void)
{
    portENTER_CRITICAL(&s_page_mux);
    s_page_pending = 0xFF;
    portEXIT_CRITICAL(&s_page_mux);
}

bool sloop_ui_quiesce(bool on)
{
    if (!s_started) return true; /* UI 未启动：空操作（selftest 的钩子也没注册到本函数） */
    if (on == s_quiesced) return true;
    if (on) {
        /* 顺序：先输入、后定时器。输入侧在定时器暂停前已不再派发，随后自检冻结
         * 引擎跑用例时系统里没有其它引擎使用方（ADR-007）。 */
        if (!input_quiesce_begin(50)) {
            ESP_LOGW(TAG, "输入静默确认超时（50ms）：输入任务未在批次边界观察到标志");
        }
        if (!bsp_display_lock(1000)) {
            /* 定时器还在跑：UI 实际是活的，不能对外声称已静默。回滚输入静默，
             * 让自检（sloop_core_selftest）按 FAIL reason=quiesce 拒绝运行——
             * 否则冻结引擎期间定时器会读 base-0 步窗快照，STEP 列显示错位。 */
            ESP_LOGE(TAG, "诊断期静默 ON 失败：LVGL 锁超时（输入静默已回滚，UI 仍在运行）");
            (void)input_quiesce_end();
            return false;
        }
        s_quiesced = true;
        sloop_ui_cancel_pending_page();
        if (s_timer != NULL) lv_timer_pause(s_timer);
        /* 光停自己的 30ms 定时器还不够：LVGL 任务仍会跑 indev 读（触摸 I2C）与
         * 刷新定时器，每 1-30ms 醒一次。诊断期连 LVGL 定时器一起停（lvgl_port_stop
         * 置 lv_timer_enable(false) 并停 tick），LVGL 任务转入 500ms 级休眠——
         * 「诊断期 UI 暂停、不要求渲染与自检并发」（spec P4-0）。
         * 另外单独禁用全部 indev：lvgl_port_stop 只停定时器/心跳，物理触摸事件在
         * lv_timer_handler 里仍会派发（DI-10 幽灵按压会改页面/翻步格）。禁用后
         * lv_indev_read() 在调用 read 回调之前就跳过（indev.c），所以这里无法计数；
         * 触摸侧没有「丢弃计数」可打印。 */
        lv_indev_enable(NULL, false);
        esp_err_t stop_ret = lvgl_port_stop();
        s_indev_disabled = true;
        bsp_display_unlock();
        ESP_LOGI(TAG, "诊断期静默 ON：UI 定时器暂停、LVGL 定时器%s、输入丢弃生效",
                 stop_ret == ESP_OK ? "停用" : "停用失败");
        return true;
    }

    /* OFF：定时器与 LVGL 定时器都是暂停态，取锁失败不能只把标志清掉——那会让
     * UI 永久停摆而对外声称已恢复。重试几次；仍失败则保持 quiesce 态（标志为真
     * → TAP/KEY 仍被拒，状态自洽），由调用方再次 `UI QUIESCE off`。 */
    bool locked = false;
    for (int i = 0; i < 3 && !locked; i++) {
        locked = bsp_display_lock(1000);
        if (!locked) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!locked) {
        ESP_LOGE(TAG, "诊断期恢复失败：LVGL 锁超时（保持静默态，可重试 UI QUIESCE off）");
        return false;
    }
    s_quiesced = false;
    if (s_indev_disabled) {
        /* LVGL 没有「enabled 查询」接口，只能整体还原；本机 indev 只有触摸一个 */
        lv_indev_enable(NULL, true);
        s_indev_disabled = false;
    }
    esp_err_t resume_ret = lvgl_port_resume(); /* 先恢复 LVGL 定时器（含刷新/indev） */
    if (s_timer != NULL) lv_timer_resume(s_timer); /* 下一拍 state_sync + 已建页刷新 */
    bsp_display_unlock();
    if (resume_ret != ESP_OK) ESP_LOGE(TAG, "诊断期恢复：LVGL 定时器恢复失败 %s", esp_err_to_name(resume_ret));
    uint32_t dropped = input_quiesce_end();
    ESP_LOGI(TAG, "诊断期静默 OFF：UI 定时器恢复、输入处理恢复（丢弃 %u 条键盘事件）",
             (unsigned)dropped);
    return true;
}

bool sloop_ui_quiesced(void)
{
    return s_started && s_quiesced;
}

uint32_t sloop_ui_input_quiesce_dropped(void)
{
    return input_quiesce_dropped();
}

uint8_t sloop_ui_page_count(void)
{
    return s_reg_count;
}

uint8_t sloop_ui_page_order_next(uint8_t page)
{
    uint8_t first = 0xFF;
    bool seen = false;
    for (uint8_t g = 0; g < GROUP_COUNT; g++) {
        for (uint8_t i = 0; i < k_groups[g].count; i++) {
            uint8_t pid = k_groups[g].pages[i];
            if (page_def(pid) == NULL) continue; /* 未注册页不参与轮转 */
            if (first == 0xFF) first = pid;
            if (seen) return pid;
            if (pid == page) seen = true;
        }
    }
    return first; /* 未找到当前页（未注册/非法）→ 首个；末尾回卷 → 首个 */
}

uint8_t sloop_ui_current_page(void)
{
    return s_started ? s_page : 0xFF;
}

/* 参数页 chip 框架（P4-F Task 4）：页 id → 布局表下标的薄外壳；逻辑都在 page_params.c。 */
bool sloop_ui_param_page(uint8_t page)
{
    return page < APP_STATE_PAGES && page_layout_index(page) >= 0;
}

uint8_t sloop_ui_param_chip_count(uint8_t page)
{
    int idx = page_layout_index(page);
    const param_page_t *pg = idx >= 0 ? page_layout_get((uint8_t)idx) : NULL;
    return pg != NULL ? pg->chip_count : 0u;
}

esp_err_t sloop_ui_param_chip_set(uint8_t page, uint8_t chip)
{
    int idx = page_layout_index(page);
    if (idx < 0) return ESP_ERR_INVALID_ARG;
    if (!s_started || s_quiesced) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = page_params_chip_set((uint8_t)idx, chip);
    bsp_display_unlock();
    return ret;
}

uint8_t sloop_ui_param_chip_get(uint8_t page)
{
    int idx = page_layout_index(page);
    return idx >= 0 ? page_params_chip_get((uint8_t)idx) : 0u;
}

bool sloop_ui_param_slot_locked(uint8_t page, uint8_t chip, uint8_t slot)
{
    int idx = page_layout_index(page);
    return idx < 0 || page_params_slot_locked((uint8_t)idx, chip, slot);
}

bool sloop_ui_param_slot_info(uint8_t page, uint8_t chip, uint8_t slot, uint8_t *id,
                              const char **label, bool *locked)
{
    int idx = page_layout_index(page);
    return idx >= 0 && page_params_slot_info((uint8_t)idx, chip, slot, id, label, locked);
}

/* Sound 仪表盘（5B-1 Task 3）：页 id → page_sound.c 的薄外壳。当前页是否仪表盘只读单字节
 * s_page（与 sloop_ui_file_page_active 同一纪律）；持锁入口在 LVGL 锁内调页面层。 */
bool sloop_ui_sound_dashboard_active(void)
{
    return s_started && page_layout_index(s_page) >= 0 &&
           page_layout_is_dashboard(page_layout_get((uint8_t)page_layout_index(s_page)));
}

uint16_t sloop_ui_sound_slot_index(uint8_t card, uint8_t slot)
{
    return sound_layout_slot_index(card, slot);
}

uint16_t sloop_ui_sound_slot_total(void)
{
    return sloop_ui_sound_dashboard_active() ? page_sound_slot_total() : 0u;
}

uint16_t sloop_ui_sound_collapsed(void)
{
    return sloop_ui_sound_dashboard_active() ? page_sound_collapsed() : 0u;
}

esp_err_t sloop_ui_sound_slot_info(uint16_t flat, uint8_t *card, uint8_t *slot, uint8_t *scope,
                                   uint8_t *id, const char **label, bool *locked, int16_t *value)
{
    if (!sloop_ui_sound_dashboard_active()) return ESP_ERR_INVALID_STATE;
    return page_sound_slot_info(flat, card, slot, scope, id, label, locked, value);
}

/* 持锁入口的公共前提：未启动 / 非仪表盘页 / 诊断期静默都是 INVALID_STATE */
static bool sound_dash_ready(void)
{
    return s_started && !s_quiesced && sloop_ui_sound_dashboard_active();
}

esp_err_t sloop_ui_sound_slot_rect(uint16_t flat, int16_t *x, int16_t *y, int16_t *w, int16_t *h)
{
    if (!sound_dash_ready()) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = page_sound_slot_rect(flat, x, y, w, h);
    bsp_display_unlock();
    return ret;
}

esp_err_t sloop_ui_sound_scroll_get(int16_t *y, int16_t *max_y)
{
    if (!sound_dash_ready()) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = page_sound_scroll_get(y, max_y);
    bsp_display_unlock();
    return ret;
}

esp_err_t sloop_ui_sound_scroll_set(int16_t y)
{
    if (!sound_dash_ready()) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = page_sound_scroll_set(y);
    bsp_display_unlock();
    return ret;
}

esp_err_t sloop_ui_sound_test_knob(uint16_t flat, int32_t dx, int32_t dy, bool press, int16_t *value)
{
    if (!sound_dash_ready()) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = page_sound_test_knob(flat, dx, dy, press, value);
    bsp_display_unlock();
    return ret;
}

/* FILE 页路由（InputManager 调用，不取 LVGL 锁）：只读单字节 s_page（与
 * sloop_ui_current_page 同一纪律）+ 改页内局部态；控件由 30ms 定时器刷新。 */
bool sloop_ui_file_page_active(void)
{
    return s_started && (s_page == APP_PAGE_PROJECTS || s_page == APP_PAGE_PRESETS);
}

void sloop_ui_file_slot_move(int8_t delta)
{
    if (!s_started) return;
    if (s_page == APP_PAGE_PROJECTS) page_projects_slot_move(delta);
    else if (s_page == APP_PAGE_PRESETS) page_presets_slot_move(delta);
}

void sloop_ui_file_load(void)
{
    if (!s_started) return;
    if (s_page == APP_PAGE_PROJECTS) page_projects_load();
    else if (s_page == APP_PAGE_PRESETS) page_presets_load();
}

const char *sloop_ui_file_status_text(void)
{
    if (!s_started) return NULL;
    if (s_page == APP_PAGE_PROJECTS) return page_projects_status_text();
    if (s_page == APP_PAGE_PRESETS) return page_presets_status_text();
    return NULL;
}

const char *sloop_ui_bank_status_text(void)
{
    if (!s_started) return NULL;
    int idx = page_layout_index(s_page); /* 非参数页（含未注册页）返回 -1 */
    return idx >= 0 ? page_params_bank_status_text((uint8_t)idx) : NULL;
}

const char *sloop_ui_sample_status_text(void)
{
    if (!s_started || s_page != APP_PAGE_SAMPLE) return NULL;
    int idx = page_layout_index(s_page);
    return idx >= 0 ? page_params_sample_status_text((uint8_t)idx) : NULL;
}

const char *sloop_ui_tools_status_text(void)
{
    if (!s_started || s_page != APP_PAGE_TOOLS) return NULL;
    return page_tools_status_text();
}

const char *sloop_ui_dsyn_status_text(void)
{
    if (!s_started || s_page != APP_PAGE_DSYN) return NULL;
    return page_dsyn_status_text();
}

const char *sloop_ui_fm6p_status_text(void)
{
    if (!s_started || s_page != APP_PAGE_FM6P) return NULL;
    return page_fm6p_status_text();
}

const char *sloop_ui_song_status_text(void)
{
    if (!s_started || s_page != APP_PAGE_SONG) return NULL;
    return page_song_status_text();
}

bool sloop_ui_system_page_active(void)
{
    return s_started && s_page == APP_PAGE_SYSTEM;
}

esp_err_t sloop_ui_system_key(uint32_t code)
{
    if (!sloop_ui_system_page_active()) return ESP_ERR_INVALID_STATE;
    return page_system_key(code);
}

const char *sloop_ui_system_status_text(void)
{
    return page_system_status_text();
}

esp_err_t sloop_ui_system_selftest_arm(void)
{
    return page_system_selftest_arm();
}

uint8_t sloop_ui_group_count(void)
{
    return GROUP_COUNT;
}

const char *sloop_ui_group_name(uint8_t group)
{
    return group < GROUP_COUNT ? k_groups[group].name : NULL;
}

esp_err_t sloop_ui_group_select(uint8_t group)
{
    if (group >= GROUP_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = group_select_locked(group);
    bsp_display_unlock();
    return ret;
}

esp_err_t sloop_ui_sub_select(uint8_t index)
{
    if (index >= UI_GROUP_MAX) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = sub_select_locked(index);
    bsp_display_unlock();
    return ret;
}

void sloop_ui_flash_note(uint8_t note)
{
    if (!s_started) return;
    /* 只入邮箱，不取任何锁（CRITICAL 区仅两条赋值）：输入任务/console 调用不会
     * 被 LVGL 渲染的帧等待拖住；定时器下一拍（≤30ms）点亮，连续触发保留最新一个。 */
    portENTER_CRITICAL(&s_flash_mux);
    s_flash_note_pending = note;
    s_flash_has_pending = true;
    portEXIT_CRITICAL(&s_flash_mux);
}

esp_err_t sloop_ui_page_pool_stats(sloop_ui_pool_stats_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    out->pool_total = (uint32_t)mon.total_size;
    out->pool_free = (uint32_t)mon.free_size;
    out->pool_used = (uint32_t)(mon.total_size - mon.free_size);
    out->lvgl_stack_free = s_lvgl_stack_free;
    out->registered = s_reg_count;
    for (uint8_t i = 0; i < s_reg_count; i++) {
        const ui_page_t *def = s_registry[i];
        sloop_ui_page_stat_t *stat = &out->pages[i];
        stat->id = def->id;
        stat->title = def->title;
        stat->state = s_page_state[def->id];
        stat->est_bytes = def->est_bytes;
        stat->delta_bytes = s_page_delta[def->id];
        stat->build_ms = s_page_build_ms[def->id];
        if (stat->state == SLOOP_UI_PAGE_BUILT) {
            out->built++;
        } else if (stat->state == SLOOP_UI_PAGE_FAILED) {
            out->failed++;
        }
    }
    /* 分组水位（ADR-005 §5）：sum_est = 组内已注册成员 est（静态预算），
     * sum_delta = 组内已建成员的池增量（当前占用）——离开组后 built/sum_delta 回 0。 */
    out->group_count = GROUP_COUNT;
    out->current_group = s_group;
    for (uint8_t g = 0; g < GROUP_COUNT && g < SLOOP_UI_GROUPS; g++) {
        sloop_ui_group_stat_t *gs = &out->groups[g];
        gs->name = k_groups[g].name;
        gs->build_ms = s_group_build_ms[g];
        for (uint8_t i = 0; i < k_groups[g].count; i++) {
            uint8_t pid = k_groups[g].pages[i];
            const ui_page_t *def = page_def(pid);
            if (def == NULL) continue;
            gs->page_count++;
            gs->sum_est += def->est_bytes;
            if (s_page_state[pid] == SLOOP_UI_PAGE_BUILT) {
                gs->built++;
                gs->sum_delta += (uint32_t)(s_page_delta[pid] > 0 ? s_page_delta[pid] : 0);
            }
        }
    }
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t sloop_ui_test_knob(uint8_t slot, int32_t dx, int32_t dy, bool press, int16_t *value)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (s_quiesced) return ESP_ERR_INVALID_STATE;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (s_page < APP_STATE_PAGES && s_page_state[s_page] == SLOOP_UI_PAGE_BUILT) {
        int idx = page_layout_index(s_page);
        if (idx >= 0) ret = page_params_test_knob(slot, dx, dy, press, value);
    } else {
        ret = ESP_ERR_INVALID_STATE;
    }
    bsp_display_unlock();
    return ret;
}

esp_err_t sloop_ui_force_page_build_failure(uint8_t page, bool on)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (page >= APP_STATE_PAGES || page_def(page) == NULL) return ESP_ERR_INVALID_ARG;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    s_forcefail[page] = on;
    if (on && s_page_state[page] == SLOOP_UI_PAGE_BUILT) {
        /* 已建页也走一遍失败路径：删掉控件、回到未建，当前页立即重建（强制失败 →
         * 错误状态条可见）；否则下次访问时重建并失败。调试注入专用。 */
        lv_obj_delete(s_page_obj[page]);
        s_page_obj[page] = NULL;
        s_page_state[page] = SLOOP_UI_PAGE_UNBUILT;
        s_page_delta[page] = 0;
        s_page_build_ms[page] = 0;
        /* 控件已删：DSYN/FM6P 的控件指针/状态文本置空（状态行访问器的「未建 → NULL」契约）。
         * 失败态重建由下面的 show_page 负责。 */
        if (page == APP_PAGE_DSYN) page_dsyn_forget();
        if (page == APP_PAGE_FM6P) page_fm6p_forget();
        if (page == APP_PAGE_SYSTEM) page_system_forget();
        if (s_page == page) (void)show_page(page);
    }
    bsp_display_unlock();
    return ESP_OK;
}

