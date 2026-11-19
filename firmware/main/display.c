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
 */
#include "display.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "sloop_display";

/* DPI panel 句柄：SCR（Task 3）经 esp_lcd_dpi_panel_get_frame_buffer 取帧 */
static esp_lcd_panel_handle_t s_panel;

esp_lcd_panel_handle_t app_display_panel(void)
{
    return s_panel;
}

/* 无主题装饰的纯色矩形（彩条/底条/L 形标记共用） */
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

    bsp_display_lock(0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    draw_test_pattern();
    int32_t log_hres = lv_display_get_horizontal_resolution(disp);
    int32_t log_vres = lv_display_get_vertical_resolution(disp);
    bsp_display_unlock();

    bsp_display_backlight_on();
    ESP_LOGI(TAG, "显示就绪: %dx%d 横屏, bring-up 测试图已绘制", (int)log_hres, (int)log_vres);
    return ESP_OK;
}
