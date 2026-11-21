/*
 * sloop_ui — 页面注册表 + 懒建（ADR-008）+ 顶部页签 + 30ms 状态同步/页面刷新
 *
 * 布局：页签条 y∈[0,72)（bg UI_PANEL，选中页签 UI_ACCENT），页面区
 * y∈[72,720)。页签由页面注册表派生（注册顺序 = 页签顺序），每页控件在**首次
 * 进入时构建**（PLAY 在 start 内构建，保证开机首帧非空白），之后只改
 * LV_OBJ_FLAG_HIDDEN（已建页不销毁，受每页 est_bytes 预算保护）：PLAY
 * （page_play.c）/ TRACKS（page_tracks.c）/ STEP（page_step.c）/ TRANSPORT
 * （page_transport.c）/ 参数页 GLOBAL|FX|ANALOG|FM6|SAMPLE（page_params.c + knob.c，
 * 布局表 page_layout.c；注册顺序接在四基础页之后，9 页时页签收窄到 114px，ADR-008 §8）。
 *
 * 懒建与失败可见性：构建前用 lv_mem_monitor 取池剩余，低于该页 est_bytes 即
 * 拒绝；构建中任一控件分配失败（build 返回 NULL）则删除半成品容器、记 ERROR，
 * 并在页面区显示错误状态条（`PAGE <TITLE> BUILD FAILED` + `UI ERR page_build <id>`）。
 * 同步切页把构建失败上报为 ESP_ERR_NO_MEM（console 打印 `UI ERR page_build <id>`）。
 * 失败页下次访问重试；`UI FORCEFAIL <page>` 可一次性强制失败（验证用）。
 * 池增量进日志与 `UI MEM`（sloop_ui_page_pool_stats）。
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
 * 与控件回调共用页内动作函数（EditCmd 层等价，ADR-005）；当前页未建/构建失败时
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
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "page_layout.h"
#include "page_params.h"
#include "sloop_core.h"
#include "sloop_state.h"
#include "theme.h"

static const char *TAG = "sloop_ui";

/* 内置页面注册表（ADR-008）：注册顺序 = 页签顺序。est_bytes 为构建前预算门槛
 * （池剩余低于该值拒绝构建），数值 = 实测池 free 减少量 + 13–23% 余量、512 对齐
 * （ADR-008 预算表）；各页 est 之和必须小于 LVGL 池总量，否则后建的页可能永远
 * 被预算拒绝。
 * STEP 9 页口径：est 从 24,576 收到 24,064（实测 delta 21,808B + 10.2%）——
 * 九个页面全建后 free 约 24.2KB，再按 13% 余量取 24,576 会让「STEP 最后建」
 * 被预算门槛误拒（实际 delta 仍装得下，余 ~2.4KB）。9 页是当前池的容量天花板：
 * 再加页/扩控件前必须先按 ADR-008 §5 rule 4 做设计决策（DI-17）。 */
static const ui_page_t k_builtin_pages[] = {
    {APP_PAGE_PLAY, "PLAY", page_play_build, page_play_update, 3072},
    {APP_PAGE_TRACKS, "TRACKS", page_tracks_build, page_tracks_update, 7168},
    {APP_PAGE_STEP, "STEP", page_step_build, page_step_update, 24064},
    {APP_PAGE_TRANSPORT, "TRANSPORT", page_transport_build, page_transport_update, 4608},
};

static const ui_page_t *s_registry[APP_STATE_PAGES];
static uint8_t s_reg_count;

static lv_obj_t *s_root;
static lv_obj_t *s_tabs[APP_STATE_PAGES];       /* 按注册（页签）顺序 */
static lv_obj_t *s_tab_labels[APP_STATE_PAGES];
static lv_obj_t *s_page_obj[APP_STATE_PAGES];   /* 按页 id 索引；NULL = 未建/失败 */
static uint8_t s_page_state[APP_STATE_PAGES];   /* sloop_ui_page_state_t */
static int32_t s_page_delta[APP_STATE_PAGES];   /* 最近一次成功构建的池 free 减少量（字节） */
static const char *s_page_fail_reason[APP_STATE_PAGES]; /* 最近一次构建失败原因 */
static bool s_forcefail[APP_STATE_PAGES];       /* UI FORCEFAIL：一次性注入 */
static lv_obj_t *s_page_error;                  /* 构建失败状态条（页面区居中） */
static lv_obj_t *s_layer_hint;
static uint8_t s_layer_shown = 0xFF; /* 脏检查（0xFF = 尚未刷新） */
static lv_timer_t *s_timer;

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

static void set_tab_selected(uint8_t page)
{
    for (uint8_t i = 0; i < s_reg_count; i++) {
        bool selected = s_registry[i]->id == page;
        lv_obj_set_style_bg_color(s_tabs[i], lv_color_hex(selected ? UI_ACCENT : UI_PANEL_HI), 0);
        lv_obj_set_style_text_color(s_tab_labels[i], lv_color_hex(selected ? UI_BG : UI_TEXT_DIM), 0);
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
        ESP_LOGE(TAG, "页面构建失败 page=%s(%u) 原因=%s 池剩余=%uB 预算=%uB（UI ERR page_build %u）",
                 def->title, (unsigned)page, reason, (unsigned)mon_before.free_size,
                 (unsigned)def->est_bytes, (unsigned)page);
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
    ESP_LOGI(TAG, "页面构建 page=%s(%u) 池增量=%dB 预算=%uB 池剩余=%uB", def->title,
             (unsigned)page, (int)delta, (unsigned)def->est_bytes,
             (unsigned)mon_after.free_size);

    /* 立即按当前状态刷新一次：首帧不等 30ms 定时器（切页/截屏时序确定） */
    app_state_t st;
    state_get_copy(&st);
    def->update(&st);
    return ESP_OK;
}

/* LVGL 上下文内调用（页签回调或已持锁的公共入口）。返回构建失败时的
 * ESP_ERR_NO_MEM（页面已切到错误状态条），其余 ESP_OK。 */
static esp_err_t show_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES || page_def(page) == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ensure_page_built(page);
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (s_page_obj[i] != NULL) lv_obj_set_hidden(s_page_obj[i], i != page);
    }
    if (ret == ESP_OK) {
        if (s_page_error != NULL) lv_obj_set_hidden(s_page_error, true);
    } else {
        show_page_error(page, s_page_fail_reason[page]);
    }
    set_tab_selected(page);
    s_page = page;
    state_set_page(page); /* 页面是 UI 局部态（sloop_state 的 UI 写路径） */
    return ret;
}

static void tab_clicked_cb(lv_event_t *e)
{
    /* 失败页的构建错误已由 show_page 记 ERROR/显示状态条；异步路径无回显通道 */
    (void)show_page((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static bool create_tab_bar(void)
{
    lv_obj_t *bar = make_plain(s_root);
    if (bar == NULL) return false;
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_W, UI_TAB_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    /* 页签宽度：4 页时保持 UI_TAB_W（像素与阶段 3 一致）；注册页更多时按可用宽度
     * 收窄，并给右侧 SLOOP 标题/层提示留出 UI_TAB_RIGHT_RESERVE，避免溢出 */
    int32_t tab_w = UI_TAB_W;
    if (s_reg_count > 0) {
        int32_t fit = (UI_W - 2 * UI_TAB_X0 - UI_TAB_RIGHT_RESERVE -
                       (int32_t)(s_reg_count - 1) * UI_TAB_GAP) /
                      (int32_t)s_reg_count;
        if (fit < tab_w) tab_w = fit;
    }

    for (uint8_t i = 0; i < s_reg_count; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        if (btn == NULL) return false;
        lv_obj_set_pos(btn, UI_TAB_X0 + i * (tab_w + UI_TAB_GAP), UI_TAB_Y);
        lv_obj_set_size(btn, tab_w, UI_TAB_H);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HI), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, tab_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_registry[i]->id);

        lv_obj_t *label = lv_label_create(btn);
        if (label == NULL) return false;
        lv_label_set_text(label, s_registry[i]->title);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_center(label);

        s_tabs[i] = btn;
        s_tab_labels[i] = label;
    }

    lv_obj_t *title = lv_label_create(bar);
    if (title == NULL) return false;
    lv_label_set_text(title, "SLOOP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -24, 22);

    /* 层键提示（InputManager 写 layer_held，30ms 定时器刷新）：按住 Sym/Aa/Ctrl/Alt
     * 时在页签条右侧显示 LAYER FX/EDIT/SEL/GLO */
    s_layer_hint = lv_label_create(bar);
    if (s_layer_hint == NULL) return false;
    lv_label_set_text(s_layer_hint, "");
    lv_obj_set_style_text_font(s_layer_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_layer_hint, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_align(s_layer_hint, LV_ALIGN_TOP_RIGHT, -110, 22);
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
    /* 参数页（GLOBAL/FX/ANALOG/FM6/SAMPLE…）：布局表与注册表条目同源（page_layout.c
     * 的 X 宏表），注册顺序接在四基础页之后 →
     * 页签顺序 = PLAY|TRACKS|STEP|TRANSPORT|GLOBAL|FX|ANALOG|FM6|SAMPLE */
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

/* LVGL 任务上下文（持锁）：先同步引擎真值，再消费音符指示，最后刷新已建页 */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    state_sync_from_engine();
    app_state_t st;
    state_get_copy(&st);
    update_layer_hint(st.layer_held);

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

    /* 只刷新已建页：懒建页在 ensure_page_built 里已按最新状态刷过首帧 */
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (s_page_state[i] != SLOOP_UI_PAGE_BUILT) continue;
        const ui_page_t *def = page_def(i);
        if (def != NULL) def->update(&st);
    }
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
    memset(s_page_obj, 0, sizeof(s_page_obj));
    memset(s_page_state, 0, sizeof(s_page_state));
    memset(s_page_delta, 0, sizeof(s_page_delta));
    memset(s_forcefail, 0, sizeof(s_forcefail));
    memset(s_page_fail_reason, 0, sizeof(s_page_fail_reason));
    s_page_error = NULL;
    s_layer_hint = NULL;

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
    return hit ? SLOOP_TAP_RESULT_OK : SLOOP_TAP_RESULT_MISS;
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

void sloop_ui_quiesce(bool on)
{
    if (!s_started) return; /* UI 未启动：空操作（selftest 的钩子也没注册到本函数） */
    if (on == s_quiesced) return;
    if (on) {
        /* 顺序：先输入、后定时器。输入侧在定时器暂停前已不再派发，随后自检冻结
         * 引擎跑用例时系统里没有其它引擎使用方（ADR-007）。 */
        if (!input_quiesce_begin(50)) {
            ESP_LOGW(TAG, "输入静默确认超时（50ms）：输入任务未在批次边界观察到标志");
        }
        s_quiesced = true;
        if (!bsp_display_lock(1000)) {
            ESP_LOGE(TAG, "诊断期静默：LVGL 锁超时，30ms 定时器未暂停（输入已静默）");
            return;
        }
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
        return;
    }

    s_quiesced = false;
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "诊断期恢复：LVGL 锁超时，30ms 定时器未恢复（需复位）");
    } else {
        if (s_indev_disabled) {
            /* LVGL 没有「enabled 查询」接口，只能整体还原；本机 indev 只有触摸一个 */
            lv_indev_enable(NULL, true);
            s_indev_disabled = false;
        }
        esp_err_t resume_ret = lvgl_port_resume(); /* 先恢复 LVGL 定时器（含刷新/indev） */
        if (s_timer != NULL) lv_timer_resume(s_timer); /* 下一拍 state_sync + 已建页刷新 */
        bsp_display_unlock();
        if (resume_ret != ESP_OK) ESP_LOGE(TAG, "诊断期恢复：LVGL 定时器恢复失败 %s", esp_err_to_name(resume_ret));
    }
    uint32_t dropped = input_quiesce_end();
    ESP_LOGI(TAG, "诊断期静默 OFF：UI 定时器恢复、输入处理恢复（丢弃 %u 条键盘事件）",
             (unsigned)dropped);
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

uint8_t sloop_ui_current_page(void)
{
    return s_started ? s_page : 0xFF;
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
    out->registered = s_reg_count;
    for (uint8_t i = 0; i < s_reg_count; i++) {
        const ui_page_t *def = s_registry[i];
        sloop_ui_page_stat_t *stat = &out->pages[i];
        stat->id = def->id;
        stat->title = def->title;
        stat->state = s_page_state[def->id];
        stat->est_bytes = def->est_bytes;
        stat->delta_bytes = s_page_delta[def->id];
        if (stat->state == SLOOP_UI_PAGE_BUILT) {
            out->built++;
        } else if (stat->state == SLOOP_UI_PAGE_FAILED) {
            out->failed++;
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
        if (s_page == page) (void)show_page(page);
    }
    bsp_display_unlock();
    return ESP_OK;
}

