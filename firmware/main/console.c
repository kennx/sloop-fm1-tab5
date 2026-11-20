/*
 * sloop-fm1-tab5 — console 通道与 SCR 帧导出（AGENTS.md §5.3）
 *
 * USB-Serial-JTAG REPL（prompt "sloop> "），注册命令：
 *  - SCR：截屏命令。PC 发 "SCR\n" → 固件回 "SCR 1280 720 rgb565 1843200\n" → 1843200 字节
 *    裸帧（小端 rgb565）→ "SCR OK\n"（PC 侧对端 scripts/screenshot.py）。
 *  - KB：Tab5 Keyboard 排障入口（probe 结果/事件计数/解码日志开关）。
 *  - AUD：音频自检入口（AUD TONE 正弦播放，audio_tone.c 后端）。
 *  - ENG：SLOOP 引擎诊断（INFO/SNAP/STEP、SMOKE、同源 SELFTEST、TRACK/TRIG、PERF
 *    与 AUDIO ON|OFF 音频流启停，DI-8）。
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
#include <stdlib.h>
#include <strings.h>

#include "bsp/m5stack_tab5.h"
#include "audio_tone.h"
#include "display.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloop_core.h"
#include "sloop_hal.h"
#include "tab5_keyboard.h"

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

    if (!bsp_display_lock(5000)) {       /* 有限等待：LVGL 任务僵死时不吊死 console 通道 */
        heap_caps_free(buf);
        printf("SCR ERR lvgl_lock_timeout\n");
        return 1;
    }
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
     * 静默混入帧流，破坏协议字节流。off 为字节偏移，buf 须按字节步进。 */
    bool ok = true;
    for (size_t off = 0; off < SCR_FRAME_BYTES && ok; off += SCR_TX_CHUNK) {
        size_t n = SCR_FRAME_BYTES - off;
        if (n > SCR_TX_CHUNK) {
            n = SCR_TX_CHUNK;
        }
        ok = fwrite((uint8_t *)buf + off, 1, n, stdout) == n;
        vTaskDelay(1);
    }
    ok = ok && fflush(stdout) == 0;
    /* 尾标记在 LF 模式下打印（线上严格为 "SCR OK\n"，AGENTS §5.3），且须
     * 紧跟帧数据；CRLF 行尾与日志级别的恢复放在其后 */
    printf(ok ? "SCR OK\n" : "SCR ERR stdout_write\n");
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);

    heap_caps_free(buf);
    ESP_LOGI(TAG, "SCR: 传输完成，PSRAM 剩余 %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ok ? 0 : 1;
}

/* KB 排障入口：无参打印 probe 结果与事件计数；ON/OFF 开关解码日志 */
static int kb_command(int argc, char **argv)
{
    if (argc >= 2) {
        if (strcasecmp(argv[1], "on") == 0) {
            tab5_kb_set_log(true);
            printf("KB 解码日志 ON\n");
            return 0;
        }
        if (strcasecmp(argv[1], "off") == 0) {
            tab5_kb_set_log(false);
            printf("KB 解码日志 OFF\n");
            return 0;
        }
        printf("用法: KB [ON|OFF]\n");
        return 1;
    }

    bool present = tab5_keyboard_probe();
    tab5_kb_stats_t st;
    tab5_kb_get_stats(&st);
    printf("KB probe: %s (0x%02X @ G%d/G%d, INT=G%d)\n", present ? "OK" : "NOT FOUND",
           TAB5_KB_I2C_ADDR, TAB5_KB_I2C_SDA_GPIO, TAB5_KB_I2C_SCL_GPIO, TAB5_KB_INT_GPIO);
    if (present) {
        printf("KB fw: 0x%02X, events down=%u up=%u dropped=%u pending=%d, INT=%d\n",
               st.fw_version, (unsigned)st.down_count, (unsigned)st.up_count,
               (unsigned)st.dropped, st.pending, st.int_level);
    }
    return 0;
}

/* AUD 排障入口：AUD TONE <hz> [ms=2000] 阻塞播放正弦（hz 域 20..12000，ms 域 1..60000）；
 * AUD REG dump ES8388 寄存器（排障） */
static int aud_command(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "reg") == 0) {
        audio_tone_dump_regs();
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "pa") == 0) {
        audio_tone_dump_pa();
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "l2") == 0) {
        char *end = NULL;
        long v = strtol(argv[2], &end, 0);
        if (end == argv[2] || *end != '\0' || v < 0 || v > 0x21) {
            printf("用法: AUD L2 <0x00..0x21>（LOUT2/ROUT2 音量）\n");
            return 1;
        }
        audio_tone_set_l2vol((uint8_t)v);
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "max") == 0) {
        audio_tone_max_all();
        return 0;
    }
    long hz = 0;
    long ms = 2000;
    bool args_ok = argc >= 3 && argc <= 4 && strcasecmp(argv[1], "tone") == 0;

    if (args_ok) {
        char *end = NULL;
        hz = strtol(argv[2], &end, 10);
        args_ok = end != argv[2] && *end == '\0';
    }
    if (args_ok && argc == 4) {
        char *end = NULL;
        ms = strtol(argv[3], &end, 10);
        args_ok = end != argv[3] && *end == '\0';
    }
    if (!args_ok || hz < 20 || hz > 12000 || ms < 1 || ms > 60000) {
        printf("用法: AUD TONE <20..12000> [ms=1..60000]\n");
        return 1;
    }

    printf("AUD TONE %ldHz %ldms 播放中...\n", hz, ms);
    esp_err_t ret = audio_tone_play((uint32_t)hz, (uint32_t)ms);
    if (ret != ESP_OK) {
        printf("AUD TONE 失败: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("AUD TONE 完成\n");
    return 0;
}

#define ENG_SMOKE_FRAMES 256u
static int32_t eng_smoke_on[ENG_SMOKE_FRAMES * 2u];
static int32_t eng_smoke_off[ENG_SMOKE_FRAMES * 2u];

static uint64_t eng_smoke_hash(const int32_t *samples)
{
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t i = 0; i < ENG_SMOKE_FRAMES * 2u; i++) {
        uint32_t value = (uint32_t)samples[i];
        for (uint32_t b = 0; b < 4; b++) {
            hash ^= (value >> (b * 8u)) & 0xFFu;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

static bool eng_uint(const char *text, uint32_t max, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > max) return false;
    *value = (uint32_t)parsed;
    return true;
}

static int eng_perf(void)
{
    if (sloop_hal_audio_streaming()) {
        printf("ENG PERF streaming=1 xrun=%u audio_stack_free=%u min_free_since_boot_internal=%u min_free_since_boot_psram=%u\n",
               (unsigned)sloop_hal_audio_xruns(),
               (unsigned)sloop_hal_audio_stack_high_water(),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        return 0;
    }
    const uint32_t blocks = 100;
    const int64_t budget_us = (int64_t)ENG_SMOKE_FRAMES * 1000000 / sloop_core_rate();
    uint64_t cycles_sum = 0, us_sum = 0;
    uint32_t cycles_max = 0, us_max = 0;
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t c0 = esp_cpu_get_cycle_count();
        int64_t t0 = esp_timer_get_time();
        sloop_core_render(eng_smoke_on, ENG_SMOKE_FRAMES);
        uint32_t cycles = esp_cpu_get_cycle_count() - c0;
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        cycles_sum += cycles;
        us_sum += us;
        if (cycles > cycles_max) cycles_max = cycles;
        if (us > us_max) us_max = us;
    }
    uint32_t cycles_mean = (uint32_t)(cycles_sum / blocks);
    uint32_t us_mean = (uint32_t)(us_sum / blocks);
    printf("ENG PERF blocks=%u cycles_mean=%u cycles_max=%u us_mean=%u us_max=%u budget_us=%lld duty_mean=%.2f%% duty_max=%.2f%% xrun=%u min_free_since_boot_internal=%u min_free_since_boot_psram=%u\n",
           (unsigned)blocks, (unsigned)cycles_mean, (unsigned)cycles_max, (unsigned)us_mean,
           (unsigned)us_max, (long long)budget_us, (double)us_mean * 100.0 / budget_us,
           (double)us_max * 100.0 / budget_us, (unsigned)sloop_hal_audio_xruns(),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int eng_command(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "info") == 0) {
        printf("ENG INFO init=%s rate=%u frames=%u NENGINES=%u P_COUNT=%u G_COUNT=%u\n",
               esp_err_to_name(sloop_core_status()), (unsigned)sloop_core_rate(),
               (unsigned)sloop_core_frames(), (unsigned)SLOOP_CORE_NENGINES,
               (unsigned)SLOOP_CORE_P_COUNT, (unsigned)SLOOP_CORE_G_COUNT);
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcasecmp(argv[1], "audio") == 0) {
        /* DI-8：开机即启流（main.c），SELFTEST/AUD TONE/离线 PERF 需先 OFF */
        if (argc == 3 && strcasecmp(argv[2], "on") == 0) {
            if (sloop_core_status() != ESP_OK) {
                printf("ENG AUDIO FAIL init=%s\n", esp_err_to_name(sloop_core_status()));
                return 1;
            }
            esp_err_t ret = sloop_hal_audio_start(sloop_core_render);
            if (ret != ESP_OK) {
                printf("ENG AUDIO FAIL audio=%s\n", esp_err_to_name(ret));
                return 1;
            }
            printf("ENG AUDIO streaming=1 xrun=%u OK\n", (unsigned)sloop_hal_audio_xruns());
            return 0;
        }
        if (argc == 3 && strcasecmp(argv[2], "off") == 0) {
            esp_err_t ret = sloop_hal_audio_stop();
            if (ret != ESP_OK) {
                printf("ENG AUDIO FAIL audio=%s\n", esp_err_to_name(ret));
                return 1;
            }
            printf("ENG AUDIO streaming=0 xrun=%u OK\n", (unsigned)sloop_hal_audio_xruns());
            return 0;
        }
        if (argc == 3) {
            printf("用法: ENG AUDIO [ON|OFF]\n");
            return 1;
        }
        printf("ENG AUDIO streaming=%u xrun=%u\n", (unsigned)sloop_hal_audio_streaming(),
               (unsigned)sloop_hal_audio_xruns());
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "smoke") == 0) {
        /* 渲染与音频流互斥（流式时 SMOKE 的块会与渲染任务抢同一引擎锁/状态） */
        if (sloop_hal_audio_streaming()) {
            printf("ENG SMOKE FAIL busy: audio streaming\n");
            return 1;
        }
        if (sloop_core_status() != ESP_OK || sloop_core_frames() != ENG_SMOKE_FRAMES) {
            printf("ENG SMOKE FAIL init=%s frames=%u\n", esp_err_to_name(sloop_core_status()),
                   (unsigned)sloop_core_frames());
            return 1;
        }
        sloop_core_track(0, 0, 0);
        sloop_core_note(0, 60, true);
        sloop_core_render(eng_smoke_on, ENG_SMOKE_FRAMES);
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < ENG_SMOKE_FRAMES * 2u; i++) nonzero += eng_smoke_on[i] != 0;
        sloop_core_note(0, 60, false);
        sloop_core_render(eng_smoke_off, ENG_SMOKE_FRAMES);
        printf("ENG SMOKE frames=%u on_hash=%016llx off_hash=%016llx nonzero=%u result=%s\n",
               (unsigned)ENG_SMOKE_FRAMES, (unsigned long long)eng_smoke_hash(eng_smoke_on),
               (unsigned long long)eng_smoke_hash(eng_smoke_off), (unsigned)nonzero,
               nonzero ? "PASS" : "FAIL");
        return nonzero ? 0 : 1;
    }
    if ((argc == 2 || argc == 3) && strcasecmp(argv[1], "selftest") == 0) {
        return sloop_core_selftest(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;
    }
    if (argc == 5 && strcasecmp(argv[1], "track") == 0) {
        uint32_t k, engine, preset;
        if (!eng_uint(argv[2], 2, &k) || !eng_uint(argv[3], SLOOP_CORE_NENGINES - 1u, &engine) ||
            !eng_uint(argv[4], 255, &preset)) {
            printf("用法: ENG TRACK <k=0..2> <engine=0..11> <preset=0..255>\n");
            return 1;
        }
        if (sloop_core_status() != ESP_OK) {
            printf("ENG TRACK FAIL init=%s\n", esp_err_to_name(sloop_core_status()));
            return 1;
        }
        sloop_core_track(k, engine, preset);
        printf("ENG TRACK k=%u engine=%u preset=%u OK\n", (unsigned)k, (unsigned)engine, (unsigned)preset);
        return 0;
    }
    if (argc == 5 && strcasecmp(argv[1], "trig") == 0) {
        /* 先验参、后启流：参数错误不得启流（否则需先 ENG AUDIO OFF 才能恢复 AUD TONE/离线 PERF） */
        uint32_t a = 0, b = 0;
        bool is_synth = strcasecmp(argv[2], "synth") == 0 && eng_uint(argv[3], 2, &a) &&
                        eng_uint(argv[4], 127, &b);
        bool is_drum = !is_synth && strcasecmp(argv[2], "drum") == 0 && eng_uint(argv[3], 3, &a) &&
                       eng_uint(argv[4], 15, &b);
        if (!is_synth && !is_drum) {
            printf("用法: ENG TRIG synth <track=0..2> <note=0..127> | drum <kit=0..3> <lane=0..15>\n");
            return 1;
        }
        if (sloop_core_status() != ESP_OK) {
            printf("ENG TRIG FAIL init=%s\n", esp_err_to_name(sloop_core_status()));
            return 1;
        }
        esp_err_t ret = sloop_hal_audio_start(sloop_core_render);
        if (ret != ESP_OK) {
            printf("ENG TRIG FAIL audio=%s\n", esp_err_to_name(ret));
            return 1;
        }
        if (is_synth) {
            sloop_core_note(a, b, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            sloop_core_note(a, b, false);
            printf("ENG TRIG synth track=%u note=%u duration_ms=500 xrun=%u OK\n",
                   (unsigned)a, (unsigned)b, (unsigned)sloop_hal_audio_xruns());
            return 0;
        }
        sloop_core_hit(a, b, 100);
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("ENG TRIG drum kit=%u lane=%u velocity=100 xrun=%u OK\n",
               (unsigned)a, (unsigned)b, (unsigned)sloop_hal_audio_xruns());
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "snap") == 0) {
        /* 只读快照（阶段 3 状态层同源）：后续任务的注入断言直接解析这些行 */
        sloop_core_snapshot_t s;
        sloop_core_read(&s);
        printf("ENG SNAP playing=%u bpm=%u sel=%u pos=%u,%u,%u,%u\n", (unsigned)s.playing,
               (unsigned)s.bpm, (unsigned)s.selected_track, (unsigned)s.step_pos[0],
               (unsigned)s.step_pos[1], (unsigned)s.step_pos[2], (unsigned)s.step_pos[3]);
        for (uint32_t k = 0; k < 4; k++) {
            printf("ENG SNAP trk k=%u engine=%u preset=%u level=%u mute=%u armed=%u solo=%u len=%u div=%u\n",
                   (unsigned)k, (unsigned)s.trk[k].engine, (unsigned)s.trk[k].preset,
                   (unsigned)s.trk[k].level, (unsigned)s.trk[k].mute, (unsigned)s.trk[k].armed,
                   (unsigned)s.trk[k].solo, (unsigned)s.trk[k].len, (unsigned)s.trk[k].div);
        }
        return 0;
    }
    if (argc == 4 && strcasecmp(argv[1], "step") == 0) {
        uint32_t k, idx;
        if (!eng_uint(argv[2], 2, &k) || !eng_uint(argv[3], 63, &idx)) {
            printf("用法: ENG STEP <track=0..2> <idx=0..63>\n");
            return 1;
        }
        sloop_step_t st;
        sloop_core_read_step(k, idx, &st);
        printf("ENG STEP k=%u i=%u n=%u note=%u,%u,%u,%u vel=%u time=%u flags=%u lvl=%u rat=%u micro=%d fill=%u\n",
               (unsigned)k, (unsigned)idx, (unsigned)st.n, (unsigned)st.note[0], (unsigned)st.note[1],
               (unsigned)st.note[2], (unsigned)st.note[3], (unsigned)st.vel, (unsigned)st.time,
               (unsigned)st.flags, (unsigned)st.lvl, (unsigned)st.rat, (int)st.micro, (unsigned)st.fill);
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "perf") == 0) return eng_perf();
    printf("用法: ENG INFO|SNAP|STEP <track=0..2> <idx=0..63>|SMOKE|SELFTEST [case]|TRACK <k> <engine> <preset>|TRIG <synth|drum> <a> <b>|PERF|AUDIO [ON|OFF]\n");
    return 1;
}

esp_err_t app_console_init(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "sloop> ";
    repl_cfg.max_history_len = 16;
    repl_cfg.task_stack_size = 16384;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    esp_err_t ret = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 USB-Serial-JTAG REPL 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 先注册命令后启动 REPL（SCR/KB/AUD/ENG） */
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

    /* KB（Tab5 Keyboard 排障） */
    const esp_console_cmd_t kb_cmd = {
        .command = "KB",
        .help = "键盘状态/事件计数；KB ON|OFF 开关解码日志",
        .hint = "[ON|OFF]",
        .func = &kb_command,
    };
    ret = esp_console_cmd_register(&kb_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 KB 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* AUD（音频自检） */
    const esp_console_cmd_t aud_cmd = {
        .command = "AUD",
        .help = "音频自检；AUD TONE <20..12000> [ms=1..60000] 播放正弦纯音",
        .hint = "TONE <hz> [ms]",
        .func = &aud_command,
    };
    ret = esp_console_cmd_register(&aud_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 AUD 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t eng_cmd = {
        .command = "ENG",
        .help = "SLOOP 引擎诊断：INFO/SMOKE/SELFTEST/TRACK/TRIG/PERF/AUDIO",
        .hint = "INFO|SMOKE|SELFTEST [case]|TRACK <k> <engine> <preset>|TRIG <synth|drum> <a> <b>|PERF|AUDIO [ON|OFF]",
        .func = &eng_command,
    };
    ret = esp_console_cmd_register(&eng_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 ENG 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_console_start_repl(repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 REPL 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
