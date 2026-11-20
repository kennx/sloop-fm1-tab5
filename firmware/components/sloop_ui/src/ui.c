/*
 * sloop_ui — 单容器屏 + 顶部页签 + 30ms 状态同步/页面刷新
 *
 * 布局：页签条 y∈[0,72)（bg UI_PANEL，选中页签 UI_ACCENT），页面区
 * y∈[72,720)。四个页对象创建一次，切页只改 LV_OBJ_FLAG_HIDDEN（不重建，
 * 避免每次切页重新分配 LVGL 对象/丢失焦点）：PLAY（page_play.c）/ TRACKS
 * （page_tracks.c）/ STEP（page_step.c）/ TRANSPORT（page_transport.c）。
 *
 * 线程纪律：所有 LVGL 调用在 bsp_display_lock 内——公共入口自行加锁，
 * 页签回调与 30ms 定时器运行在 LVGL 任务上下文中（esp_lvgl_port 的
 * lvgl_port_task 已持递归锁），不再重复加锁，与 display.c 的约定一致。
 * 定时器每 30ms 先 state_sync_from_engine() 拉引擎真值，再对各页做脏检查
 * 刷新（四页都刷新，切页时无需补刷）。
 *
 * InputManager（input.c）在 sloop_ui_start 末尾启动：5ms 轮询 Tab5 Keyboard
 * 事件 → 层模型 → EditCmd；页签条右侧的 LAYER 提示由本文件的定时器按
 * state.layer_held 刷新。
 */
#include "sloop_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "theme.h"

static const char *TAG = "sloop_ui";

static const char *const k_tab_names[APP_STATE_PAGES] = {"PLAY", "TRACKS", "STEP", "TRANSPORT"};

static lv_obj_t *s_tabs[APP_STATE_PAGES];
static lv_obj_t *s_tab_labels[APP_STATE_PAGES];
static lv_obj_t *s_pages[APP_STATE_PAGES];
static lv_obj_t *s_layer_hint;
static uint8_t s_layer_shown = 0xFF; /* 脏检查（0xFF = 尚未刷新） */
static lv_timer_t *s_timer;

/* 音符指示邮箱：输入层只写、30ms 定时器消费（见 sloop_ui_flash_note） */
static portMUX_TYPE s_flash_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_flash_note_pending;
static bool s_flash_has_pending;
static uint8_t s_page;
static bool s_started;

/* 无主题装饰的容器（root/页面/页签条共用） */
static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static void set_tab_selected(uint8_t page)
{
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        bool selected = i == page;
        lv_obj_set_style_bg_color(s_tabs[i], lv_color_hex(selected ? UI_ACCENT : UI_PANEL_HI), 0);
        lv_obj_set_style_text_color(s_tab_labels[i], lv_color_hex(selected ? UI_BG : UI_TEXT_DIM), 0);
    }
}

/* LVGL 上下文内调用（页签回调或已持锁的公共入口） */
static void show_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES) return;
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        lv_obj_set_hidden(s_pages[i], i != page);
    }
    set_tab_selected(page);
    s_page = page;
    state_set_page(page); /* 页面是 UI 局部态（sloop_state 的 UI 写路径） */
}

static void tab_clicked_cb(lv_event_t *e)
{
    show_page((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void create_tab_bar(lv_obj_t *root)
{
    lv_obj_t *bar = make_plain(root);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, UI_W, UI_TAB_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_pos(btn, UI_TAB_X0 + i * (UI_TAB_W + UI_TAB_GAP), UI_TAB_Y);
        lv_obj_set_size(btn, UI_TAB_W, UI_TAB_H);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_PANEL_HI), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, tab_clicked_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, k_tab_names[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_DIM), 0);
        lv_obj_center(label);

        s_tabs[i] = btn;
        s_tab_labels[i] = label;
    }

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "SLOOP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -24, 22);

    /* 层键提示（InputManager 写 layer_held，30ms 定时器刷新）：按住 Sym/Aa/Ctrl/Alt
     * 时在页签条右侧显示 LAYER FX/EDIT/SEL/GLO */
    s_layer_hint = lv_label_create(bar);
    lv_label_set_text(s_layer_hint, "");
    lv_obj_set_style_text_font(s_layer_hint, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_layer_hint, lv_color_hex(UI_TEXT_DIM), 0);
    lv_obj_align(s_layer_hint, LV_ALIGN_TOP_RIGHT, -110, 22);
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

static void create_pages(lv_obj_t *root)
{
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        lv_obj_t *page = make_plain(root);
        lv_obj_set_pos(page, 0, UI_TAB_BAR_H);
        lv_obj_set_size(page, UI_W, UI_H - UI_TAB_BAR_H);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        s_pages[i] = page;
    }
    page_play_create(s_pages[APP_PAGE_PLAY]);
    page_tracks_create(s_pages[APP_PAGE_TRACKS]);
    page_step_create(s_pages[APP_PAGE_STEP]);
    page_transport_create(s_pages[APP_PAGE_TRANSPORT]);
}

/* LVGL 任务上下文（持锁）：先同步引擎真值，再消费音符指示，最后刷新各页 */
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
    if (flash) page_play_flash(note);

    page_play_update(&st);
    page_tracks_update(&st);
    page_step_update(&st);
    page_transport_update(&st);
}

esp_err_t sloop_ui_start(void)
{
    if (s_started) return ESP_OK;
    if (!bsp_display_lock(5000)) {
        ESP_LOGE(TAG, "LVGL 锁超时，UI 未启动");
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr); /* 移除 bring-up 测试图；失败回退由调用方重画 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *root = make_plain(scr);
    if (root != NULL) {
        lv_obj_set_pos(root, 0, 0);
        lv_obj_set_size(root, UI_W, UI_H);
        lv_obj_set_style_bg_color(root, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        create_tab_bar(root);
        create_pages(root);
    }

    bool ok = root != NULL;
    for (uint8_t i = 0; ok && i < APP_STATE_PAGES; i++) ok = s_pages[i] != NULL;
    if (ok) {
        show_page(APP_PAGE_PLAY);
        s_timer = lv_timer_create(ui_timer_cb, 30, NULL);
        ok = s_timer != NULL;
    }
    if (!ok) {
        /* 部分对象创建失败（内存不足）：清空控件树，交调用方回退测试图 */
        if (s_timer != NULL) {
            lv_timer_delete(s_timer);
            s_timer = NULL;
        }
        lv_obj_clean(scr);
        bsp_display_unlock();
        ESP_LOGE(TAG, "UI 对象创建失败（LVGL 内存不足）");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;

    /* LVGL 对象走内置 TLSF 池（CONFIG_LV_USE_STDLIB_MALLOC=0），池耗尽会静默
     * 建不出对象——STEP 网格 64 格是这个池的最大用户，就绪行打一次占用。
     * 池遍历必须持 LVGL 锁：30ms 定时器已创建，LVGL 任务随时会分配文本/样式，
     * 无锁遍历会读到撕裂的池结构。 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "UI 就绪: %u 页 @%dx%d, LVGL 池已用 %u/%u 字节, 内部堆剩余 %u 字节",
             (unsigned)APP_STATE_PAGES, UI_W, UI_H, (unsigned)(mon.total_size - mon.free_size),
             (unsigned)mon.total_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bsp_display_unlock();

    /* 输入任务在 LVGL 锁外启动（面板就绪后）；失败只丢键盘输入，不影响 UI */
    esp_err_t input_ret = input_start();
    if (input_ret != ESP_OK) {
        ESP_LOGE(TAG, "输入任务启动失败: %s（触摸仍可用）", esp_err_to_name(input_ret));
    }
    return ESP_OK;
}

void sloop_ui_switch_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES) {
        ESP_LOGW(TAG, "切页忽略: 非法页 %u", (unsigned)page);
        return;
    }
    if (!s_started) {
        ESP_LOGW(TAG, "切页忽略: UI 未启动");
        return;
    }
    if (!bsp_display_lock(1000)) {
        ESP_LOGW(TAG, "切页 %u 失败: LVGL 锁超时", (unsigned)page);
        return;
    }
    show_page(page);
    bsp_display_unlock();
}

uint8_t sloop_ui_current_page(void)
{
    return s_started ? s_page : 0xFF;
}

void sloop_ui_flash_note(uint8_t note)
{
    /* 只入邮箱，不取任何锁（CRITICAL 区仅两条赋值）：输入任务/console 调用不会
     * 被 LVGL 渲染的帧等待拖住；定时器下一拍（≤30ms）点亮，连续触发保留最新一个。 */
    portENTER_CRITICAL(&s_flash_mux);
    s_flash_note_pending = note;
    s_flash_has_pending = true;
    portEXIT_CRITICAL(&s_flash_mux);
}
