/*
 * sloop-fm1-tab5 — 显示组装与 bring-up 测试图
 *
 * 组装流程镜像 BSP bsp_display_start_with_config() 的内部实现
 * （managed_components/espressif__m5stack_tab5/src/bsp_display.c），
 * 但改用 bsp_display_new_with_handles() 以保留 panel 句柄——
 * bsp_display_start() 不外露该句柄，而 SCR 截屏需要它取 DPI 帧缓冲。
 *
 * 屏幕玻璃为竖向扫描 720×1280（BSP_LCD_H_RES/V_RES），LVGL 旋转 90° 得到
 * 横屏 1280×720 逻辑坐标（Tab5 横屏标准用法；ST7121 TDDI 面板不支持硬件
 * 镜像/交换，只能软旋转，见 BSP bsp_display_start() 默认 sw_rotate）。
 *
 * 测试图固定布局（横屏坐标，深底色 0x101418，Task 4 像素抽检锚定于此）：
 * - 顶带 y∈[0,240)：8 条竖直彩条各 160px 宽，从左到右 白/黄/青/绿/品红/红/蓝/黑；
 * - 中带 y∈[240,480)：实心圆（圆心 (200,360)，r=80，绿色）、空心方框
 *   （外沿 x∈[420,580]、y∈[280,440]，边宽 8px，青色）、右对角线
 *   （(760,450)→(1240,270)，线宽 4px，黄色）、文本 "SLOOP TAB5 BRINGUP"
 *   （默认 Montserrat 14，白色，顶部水平居中 y=250）；
 * - 底带 y∈[480,720)：4 条水平条各 1280×60，蓝/绿/黄/橙（四轨主题色）；
 * - 四角白色 L 形标记（臂长 32px、边宽 8px）验证无裁切；左上角标记与
 *   白色彩条同色不可辨，该角裁切判定依赖白条自身贴边。
 *
 * 阶段 3 起本测试图不再是开机画面：sloop_ui 成功后清空控件树接管屏幕，
 * 失败时由 app_display_draw_test_pattern() 重画本图兜底。
 *
 * 触摸反馈（Task 5）：30ms 轮询触摸 indev，按下时把 lv_layer_top 上的
 * 白色十字准星（臂 32px、线宽 2px）移到触点并限流输出坐标日志
 * （位移 ≥8px 才记录）；松开后准星留在最后位置，便于截屏核对。
 * Task 7 起该反馈只在 bring-up 路径有效：sloop_ui 启动成功后由 main.c 调
 * app_display_touch_feedback_enable(false) 关闭（准星会盖在生产页面上、被当作
 * 缺陷；DI-10 的幽灵按压事件仍可能到达，见 docs/deferred-issues.md）。
 */
#include "display.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"
#include "sloop_hal.h"

static const char *TAG = "sloop_display";

/* DPI panel 句柄：SCR（Task 3）经 esp_lcd_dpi_panel_get_frame_buffer 取帧 */
static esp_lcd_panel_handle_t s_panel;

esp_lcd_panel_handle_t app_display_panel(void)
{
    return s_panel;
}

/* 无主题装饰的纯色矩形（彩条/底条/L 形标记/触摸准星共用） */
static lv_obj_t *make_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    lv_obj_t *rect = lv_obj_create(parent);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_color(rect, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rect, 0, 0);
    lv_obj_set_style_radius(rect, 0, 0);
    lv_obj_set_style_pad_all(rect, 0, 0);
    return rect;
}

static void draw_test_pattern(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* 顶带：8 条竖直彩条 */
    static const uint32_t bar_colors[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
    };
    for (int i = 0; i < 8; i++) {
        make_rect(scr, i * 160, 0, 160, 240, bar_colors[i]);
    }

    /* 中带：实心圆 */
    lv_obj_t *circle = make_rect(scr, 120, 280, 160, 160, 0x00FF00);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);

    /* 中带：空心方框（青色边在 160×160 外沿内绘） */
    lv_obj_t *square = lv_obj_create(scr);
    lv_obj_set_pos(square, 420, 280);
    lv_obj_set_size(square, 160, 160);
    lv_obj_set_style_bg_opa(square, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(square, 8, 0);
    lv_obj_set_style_border_color(square, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_radius(square, 0, 0);
    lv_obj_set_style_pad_all(square, 0, 0);

    /* 中带：右对角线 */
    static const lv_point_precise_t diag_points[] = {{760, 450}, {1240, 270}};
    lv_obj_t *diag = lv_line_create(scr);
    lv_line_set_points(diag, diag_points, 2);
    lv_obj_set_style_line_width(diag, 4, 0);
    lv_obj_set_style_line_color(diag, lv_color_hex(0xFFFF00), 0);

    /* 中带：文本 */
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "SLOOP TAB5 BRINGUP");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 250);

    /* 底带：四轨主题色水平条 */
    static const uint32_t track_colors[4] = {0x3B7BD8, 0x3FA34D, 0xE0B32A, 0xE06A1B};
    for (int i = 0; i < 4; i++) {
        make_rect(scr, 0, 480 + i * 60, 1280, 60, track_colors[i]);
    }

    /* 四角 L 形标记（最后绘制，位于最上层） */
    make_rect(scr, 0, 0, 32, 8, 0xFFFFFF);
    make_rect(scr, 0, 0, 8, 32, 0xFFFFFF);
    make_rect(scr, 1248, 0, 32, 8, 0xFFFFFF);
    make_rect(scr, 1272, 0, 8, 32, 0xFFFFFF);
    make_rect(scr, 0, 712, 32, 8, 0xFFFFFF);
    make_rect(scr, 0, 688, 8, 32, 0xFFFFFF);
    make_rect(scr, 1248, 712, 32, 8, 0xFFFFFF);
    make_rect(scr, 1272, 688, 8, 32, 0xFFFFFF);
}

/* UI 启动失败时的回退入口（main.c 调用）：清掉可能残留的半成品控件树后重画
 * bring-up 测试图，保证屏幕不空白。 */
void app_display_draw_test_pattern(void)
{
    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "测试图回退放弃：LVGL 锁超时");
        return;
    }
    lv_obj_clean(lv_screen_active());
    draw_test_pattern();
    bsp_display_unlock();
}

/* ==================== 触摸反馈（Task 5：十字准星 + 坐标日志） ==================== */

/* 准星几何：两条正交白色细条组成十字，臂长 32px、线宽 2px */
enum { CROSS_ARM = 32, CROSS_THICK = 2 };

static lv_indev_t *s_touch_indev;    /* lvgl_port_add_touch 注册的触摸 indev */
static lv_obj_t *s_cross_h;          /* 准星横条（lv_layer_top 上） */
static lv_obj_t *s_cross_v;          /* 准星竖条（lv_layer_top 上） */
static int32_t s_last_log_x = -1000; /* 上次日志坐标，屏外初值保证首按必记录 */
static int32_t s_last_log_y = -1000;
static bool s_feedback_enabled = true; /* UI 接管屏幕后由 main.c 置 false */

static lv_indev_t *touch_indev_init(lv_display_t *disp)
{
    /* 镜像 BSP bsp_display_indev_touch_init()（bsp_display.c）：本文件走的
     * bsp_display_new_with_handles() 路径只组装 LCD，不含触摸——
     * bsp_display_get_input_dev() 背后的静态指针仅由 bsp_display_start_with_config()
     * 路径赋值，在此路径恒为 NULL，须自行挂接触摸 indev。 */
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t ret = bsp_touch_new(NULL, &tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_touch_new 失败: %s", esp_err_to_name(ret));
        return NULL;
    }
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
    if (indev == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_touch 失败");
        return NULL;
    }
    ESP_LOGI(TAG, "触摸 indev 已挂接（随显示旋转映射到横屏坐标）");
    return indev;
}

/* 本回调运行于 LVGL 任务（esp_lvgl_port 的 lvgl_port_task 已持 lvgl
 * 递归锁调用 lv_timer_handler），不可也无须再 bsp_display_lock。 */
static void touch_feedback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_feedback_enabled || s_touch_indev == NULL || s_cross_h == NULL) {
        return;
    }
    if (lv_indev_get_state(s_touch_indev) != LV_INDEV_STATE_PRESSED) {
        return; /* 松开后准星留在最后位置，便于截屏核对 */
    }

    lv_point_t pt;
    lv_indev_get_point(s_touch_indev, &pt);

    lv_obj_set_hidden(s_cross_h, false);
    lv_obj_set_hidden(s_cross_v, false);
    lv_obj_set_pos(s_cross_h, pt.x - CROSS_ARM / 2, pt.y - CROSS_THICK / 2);
    lv_obj_set_pos(s_cross_v, pt.x - CROSS_THICK / 2, pt.y - CROSS_ARM / 2);

    /* 坐标限流：距上次日志位移 ≥8px（欧氏距离）才输出 */
    int32_t dx = pt.x - s_last_log_x;
    int32_t dy = pt.y - s_last_log_y;
    if (dx * dx + dy * dy >= 8 * 8) {
        ESP_LOGI(TAG, "touch (%d,%d)", (int)pt.x, (int)pt.y);
        s_last_log_x = pt.x;
        s_last_log_y = pt.y;
    }
}

void app_display_touch_feedback_start(void)
{
    if (s_touch_indev == NULL) {
        ESP_LOGE(TAG, "触摸 indev 缺失，反馈未启动");
        return;
    }

    bsp_display_lock(0);
    lv_obj_t *layer = lv_layer_top();
    s_cross_h = make_rect(layer, 0, 0, CROSS_ARM, CROSS_THICK, 0xFFFFFF);
    s_cross_v = make_rect(layer, 0, 0, CROSS_THICK, CROSS_ARM, 0xFFFFFF);
    lv_obj_set_hidden(s_cross_h, true);
    lv_obj_set_hidden(s_cross_v, true);
    lv_timer_create(touch_feedback_timer_cb, 30, NULL);
    bsp_display_unlock();

    ESP_LOGI(TAG, "触摸反馈就绪: 30ms 轮询, 准星待命（初始隐藏）, 坐标日志 ≥8px 限流");
}

/* UI 接管屏幕后关闭准星与坐标日志（Task 7）：准星画在 lv_layer_top 上，会盖住
 * 生产页面并出现在截图里。仅供阶段 1 bring-up 路径保留；DI-10（幽灵按压）不在
 * 此处修复——indev 事件照常到达，只是不再有可见反馈。 */
void app_display_touch_feedback_enable(bool on)
{
    s_feedback_enabled = on;
    if (!on && s_cross_h != NULL && bsp_display_lock(1000)) {
        lv_obj_set_hidden(s_cross_h, true);
        lv_obj_set_hidden(s_cross_v, true);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "触摸反馈 %s（准星 + 坐标日志）", on ? "ON" : "OFF");
}

esp_err_t app_display_init(void)
{
    /* 本机为 ST7121 批次（docs/dev/device-notes.md §1），DSI lane 速率须用
     * 965Mbps——镜像 BSP 内部对该批次的适配（bsp_display.c 中
     * BSP_LCD_ST712X_MIPI_DSI_LANE_BITRATE_MBPS，bsp_get_board_version()
     * 为 BSP 内部静态接口，应用侧无法先探测再建总线）。 */
    const bsp_display_config_t new_cfg = {
        .dsi_bus = {
            .phy_clk_src = 0, /* 0 = 由驱动选择默认时钟源（同 BSP） */
            .lane_bit_rate_mbps = 965,
        },
    };

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    bsp_lcd_handles_t handles = {0};
    ret = bsp_display_new_with_handles(&new_cfg, &handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new_with_handles 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    s_panel = handles.panel;

    /* 与 BSP bsp_display_lcd_init() 一致：面板创建后显式开显示，再挂 LVGL */
    esp_lcd_panel_disp_on_off(s_panel, true);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = handles.io,
        .panel_handle = handles.panel,
        .buffer_size = BSP_LCD_H_RES * 30, /* 约 10% 行的绘制缓冲（参照 BSP 默认） */
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_spiram = 1,
            .sw_rotate = 1,
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false, /* sdkconfig 未启用 CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi 失败");
        return ESP_FAIL;
    }

    /* 触摸 indev：本路径 BSP 未挂接（见 touch_indev_init 注释），失败仅降级
     * 为无触摸模式，不阻塞显示/console 的 bring-up 验证 */
    s_touch_indev = touch_indev_init(disp);
    if (s_touch_indev == NULL) {
        ESP_LOGE(TAG, "触摸初始化失败，继续以无触摸模式运行");
    }

    bsp_display_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    draw_test_pattern();
    int32_t log_hres = lv_display_get_horizontal_resolution(disp);
    int32_t log_vres = lv_display_get_vertical_resolution(disp);
    bsp_display_unlock();

    /* 背光 = 平台设置的亮度（NVS 存值，默认 100；app_main 已先调 sloop_hal_settings_init） */
    bsp_display_brightness_set(sloop_hal_settings_bright());
    ESP_LOGI(TAG, "显示就绪: %dx%d 横屏, bring-up 测试图已绘制", (int)log_hres, (int)log_vres);

    app_display_touch_feedback_start();
    return ESP_OK;
}
