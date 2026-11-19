/*
 * sloop-fm1-tab5 — console 通道与 SCR 帧导出（AGENTS.md §5.3）
 *
 * USB-Serial-JTAG REPL（prompt "sloop> "），注册 SCR 截屏命令：
 * PC 发 "SCR\n" → 固件回 "SCR 1280 720 rgb565 1843200\n" → 1843200 字节
 * 裸帧（小端 rgb565）→ "SCR OK\n"（PC 侧对端 scripts/screenshot.py）。
 *
 * 帧导出几何（与 display.c 的横屏组装互逆）：
 * DPI 帧缓冲物理为竖扫 720×1280（BSP_LCD_H/V_RES），LVGL 以
 * LV_DISPLAY_ROTATION_90 软旋转把横屏 1280×720 内容写入，映射为
 * FB(px = y, py = SCR_W-1-x)（lv_draw_utils.c rotate90_rgb565 与
 * esp_lvgl_port lvgl_port_rotate_area 的逆推）。SCR 按同一几何反向取出，
 * 保证导出帧与屏幕所见一致（Task 4 像素探针以此判定旋转手性）。
 */
#include "console.h"

#include <stdio.h>

#include "bsp/m5stack_tab5.h"
#include "display.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sloop_console";

/* SCR 导出帧：横屏逻辑分辨率（AGENTS §5.3 头行字段） */
#define SCR_W (1280)
#define SCR_H (720)
#define SCR_FRAME_BYTES (SCR_W * SCR_H * 2)  /* rgb565，2 字节/像素 */
#define SCR_TX_CHUNK (16384)                 /* 分块写出粒度 */

/* 旋转方向修正点（唯一，Task 4 探针若判反只改 scr_rotate_copy 一处下标） */
static void scr_rotate_copy(const uint16_t *fb, uint16_t *out)
{
    for (int32_t y = 0; y < SCR_H; y++) {
        for (int32_t x = 0; x < SCR_W; x++) {
            /* ROTATION_90 几何：px = y、py = SCR_W-1-x；
             * 若需翻转为 ROTATION_270 几何，改为
             * fb[x * BSP_LCD_H_RES + (BSP_LCD_V_RES - 1 - y)] */
            out[y * SCR_W + x] = fb[(SCR_W - 1 - x) * BSP_LCD_H_RES + y];
        }
    }
}

static int scr_command(int argc, char **argv)
{
    esp_lcd_panel_handle_t panel = app_display_panel();
    if (panel == NULL) {
        printf("SCR ERR display_panel_not_ready\n");
        return 1;
    }

    uint16_t *buf = heap_caps_malloc(SCR_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        printf("SCR ERR spiram_malloc_failed\n");
        return 1;
    }

    bsp_display_lock(0);                 /* 暂停 LVGL 刷新（§5.3 稳帧策略） */
    vTaskDelay(pdMS_TO_TICKS(40));       /* ≥2 帧，等潜在 flush 收尾 */
    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
    if (ret != ESP_OK || fb == NULL) {
        bsp_display_unlock();
        heap_caps_free(buf);
        printf("SCR ERR get_frame_buffer_failed\n");
        return 1;
    }
    scr_rotate_copy((const uint16_t *)fb, buf);
    bsp_display_unlock();                /* 不长时间锁屏，传输在锁外进行 */

    ESP_LOGI(TAG, "SCR: 传输前 PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 二进制阶段静默日志（防日志字节混入帧流/尾标记前），并临时把
     * USB-Serial-JTAG TX 行尾改为 LF-only——默认 CRLF 模式会把帧内
     * 0x0A 字节翻写为 0x0D 0x0A，破坏字节流（usb_serial_jtag_vfs.c
     * usb_serial_jtag_write 的换行翻译）。 */
    esp_log_level_set("*", ESP_LOG_NONE);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    printf("SCR %d %d rgb565 %d\n", SCR_W, SCR_H, SCR_FRAME_BYTES);
    /* 分块写出并在块间 vTaskDelay 让出 CPU：整段一次性 fwrite 会让 REPL 任务
     * 连续占核超过任务看门狗 5s 阈值（CONFIG_ESP_TASK_WDT_TIMEOUT_S，默认
     * 监视 IDLE0/1），看门狗经 esp_rom_printf 打印的寄存器转储绕过日志
     * 静默混入帧流，破坏协议字节流。 */
    bool ok = true;
    for (size_t off = 0; off < SCR_FRAME_BYTES && ok; off += SCR_TX_CHUNK) {
        size_t n = SCR_FRAME_BYTES - off;
        if (n > SCR_TX_CHUNK) {
            n = SCR_TX_CHUNK;
        }
        ok = fwrite(buf + off, 1, n, stdout) == n;
        vTaskDelay(1);
    }
    ok = ok && fflush(stdout) == 0;
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    /* 尾标记须紧跟帧数据：恢复日志级别放在其后，避免日志行插进两者之间 */
    printf(ok ? "SCR OK\n" : "SCR ERR stdout_write\n");
    esp_log_level_set("*", ESP_LOG_INFO);

    heap_caps_free(buf);
    ESP_LOGI(TAG, "SCR: 传输完成，PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ok ? 0 : 1;
}

esp_err_t app_console_init(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "sloop> ";
    repl_cfg.max_history_len = 16;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    esp_err_t ret = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 USB-Serial-JTAG REPL 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 先注册命令后启动 REPL；KB/AUD 自检命令（Task 5/7）在此追加 */
    const esp_console_cmd_t scr_cmd = {
        .command = "SCR",
        .help = "导出帧缓冲（AGENTS §5.3 截屏协议，screenshot.py 对端）",
        .hint = NULL,
        .func = &scr_command,
    };
    ret = esp_console_cmd_register(&scr_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 SCR 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_console_start_repl(repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 REPL 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
