/*
 * sloop-fm1-tab5 — console 通道与 SCR 帧导出（AGENTS.md §5.3）
 *
 * USB-Serial-JTAG REPL（prompt "sloop> "），注册命令：
 *  - SCR：截屏命令。PC 发 "SCR\n" → 固件回 "SCR 1280 720 rgb565 1843200\n" → 1843200 字节
 *    裸帧（小端 rgb565）→ "SCR OK\n"（PC 侧对端 scripts/screenshot.py）。
 *  - KB：Tab5 Keyboard 排障入口（probe 结果/事件计数/解码日志开关）。
 *  - AUD：音频自检入口（AUD TONE 正弦播放，audio_tone.c 后端）。
 *  - ENG：SLOOP 引擎诊断（INFO/SNAP/STEP、SMOKE、同源 SELFTEST、TRACK/PRESET/TRIG、
 *    DESC 描述符投影、PERF、MEM 内部堆水位与 AUDIO ON|OFF 音频流启停，DI-8）。
 *    TRACK 换引擎/预置**并载入参数**；PRESET 打印同一「当前 p[] + 引擎 + 预置」的纯写值
 *    向量（71 行 P<id>=<v>，注入侧的机器比对期望，无副作用；冻结态回 `ERR frozen`）；
 *    FREEZE <on|off> 是诊断冻结的手动入口（必须成对，供注入复验冻结拒绝路径）。
 *  - STATE：状态层自检（STATE SNAP：AppState 的页面/焦点/层键/步窗、走带
 *    bpm/playing 与引擎真值的选轨/mute/solo，以及选中轨参数投影 p[] 与全局 g[]
 *    的抽样字段；与 ENG SNAP 同源）。
 *  - PARAM：参数写诊断注入（阶段 4 P4-A）：PARAM <id> <value> 经状态层
 *    CMD_SET_PARAM 写选中轨（钳位在引擎，回显 applied），PARAM BATCH <id> <value> …
 *    走一次 CMD_SET_PARAM_BATCH；id 在该轨上无描述符时回 PARAM ERR 且不写。
 *    PARAM RAW <id> <value> 跳过 desc 预判直接下发，用于验证引擎「无描述符 id
 *    逐条忽略」的路径（正常 PARAM 在 console 层就拒绝了）。
 *  - UI：切页（UI <play|tracks|step|transport|global|fx|analog|fm6|sample>）；无参打印当前页；QUIESCE <on|off>
 *    手动复验诊断期静默；MEM 打印页面注册表/懒建的 LVGL 池水位（每页 est 预算 vs
 *    实测 delta）；WINDOW [<start> <len>] 写步窗（start 16 对齐 0..48、len 1..16，
 *    无参打印当前窗口）；FORCEFAIL <page> [on|off] 强制构建失败（验证失败可见性，
 *    ADR-008）：对已建当前页删除控件并立即重建到失败态（注入当场消费，回显
 *    state=failed），对已建非当前页只删除控件回到未建态（回显 state=unbuilt，
 *    注入待命），对未建页挂起到下一次构建；off 只撤销尚未消费的注入，
 *    不重建已失败页。
 *  - KEY：键盘注入（KEY <code> <down|up>），与物理键盘同入口
 *    sloop_ui_input_key()；KEY STATS [RESET] 打印注入路径延迟（入口→派发返回，
 *    均值/最大，µs）、输入任务栈余量与诊断期丢弃计数；KEY PANIC 全停（释放已按下
 *    音符、清层键与待切页）。code = tab5_keyboard 事件码（row<<4|col），十进制或 0x。
 *    诊断期静默（UI QUIESCE on / ENG SELFTEST 运行中）KEY 被丢弃并回 "KEY ERR quiesced"。
 *  - TAP：触摸注入 TAP <x> <y>（屏幕绝对坐标，含页签条），经
 *    sloop_ui_input_tap() 按当前页 hit-test 表命中矩形并执行与控件回调相同的
 *    动作函数（EditCmd 层等价，ADR-005）；未命中打印 "TAP miss x,y page=P"，
 *    LVGL 锁超时打印 "TAP ERR lvgl_lock_timeout"，诊断期静默打印
 *    "TAP ERR quiesced"（都返回非零，令牌不同，脚本不得互相误判）。
 *  - STORE：存储/工程诊断（P4-B，ADR-009）。STORE STATS 打印后端与对象状态
 *    （ready/base/span/map/objects/faults/armed）；STORE RAWTEST 对空闲逻辑扇区
 *    0xFB000（分区内 0x64000）做 erase → 写 0x5A 模式 → 校验 → 再 erase
 *    （破坏性）；STORE FAULT <nth> [half] | CLEAR 是 HAL 故障注入入口
 *    （计数只由 erase/prog 递增，arm 期间 read 失败但不计数，Task 8 消费）。
 *    STORE PROJ LIST | SAVE n | LOAD n | ERASE n | HASH n 是工程 A–D 的存读诊断
 *    （与 UI 命令同一套 sloop_store_*）；STORE PROJ SYNTH <fmt1|foreign> 是**测试
 *    专用**的诊断（破坏性：覆盖诊断槽 3）：构造内存工程触发 project_load 的两条
 *    fallback（格式 1 的 preset==0xFF 默认声音 + steps_clear / 未知引擎的
 *    proj_foreign_parts），回读各轨 engine/preset/steps/atk/e0。STORE LIVEHASH 打印
 *    当前引擎状态的捕获 hash（工程往返门禁）。STORE PRESET LIST | SAVE n | LOAD n [track] | ERASE n 是
 *    用户预置 32 槽（upreset.c）的诊断入口：SAVE = 选中轨的声音 → 槽，LOAD 缺省
 *    track = 选中轨；行格式 "PRESET k USED <ENGINE> <NAME>" / "PRESET k EMPTY"，
 *    末尾 "PRESET COUNT used=n total=32"。错误统一回
 *    "STORE ERR <no_storage|playing|empty|invalid|save|drum>"。
 *    偏移是上游 flash-map 逻辑偏移，不是芯片绝对地址。
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
#include <string.h>
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
#include "sloop_state.h"
#include "sloop_store.h"
#include "sloop_ui.h"
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
    /* 流式占空比预算 = 引擎块时长（256/44100 = 5804µs）；离线分支用 ENG_SMOKE_FRAMES 的预算。
     * 流式分支的 render_* 由 sloop_hal_audio_render_stats 给出，窗口 = 本次启流
     * （start 清零），首块含冷缓存开销计入 max。 */
    const int64_t stream_budget_us = (int64_t)SLOOP_CORE_FRAMES * 1000000 / sloop_core_rate();
    if (sloop_hal_audio_streaming()) {
        uint32_t mean_us = 0, max_us = 0, blocks = 0;
        sloop_hal_audio_render_stats(&mean_us, &max_us, &blocks);
        printf("ENG PERF streaming=1 xrun=%u audio_stack_free=%u render_blocks=%u "
               "render_mean_us=%u render_max_us=%u duty_mean=%.2f%% duty_max=%.2f%% "
               "min_free_since_boot_internal=%u min_free_since_boot_psram=%u\n",
               (unsigned)sloop_hal_audio_xruns(),
               (unsigned)sloop_hal_audio_stack_high_water(),
               (unsigned)blocks, (unsigned)mean_us, (unsigned)max_us,
               (double)mean_us * 100.0 / (double)stream_budget_us,
               (double)max_us * 100.0 / (double)stream_budget_us,
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
        /* 诊断模式（静默 UI + 冻结引擎读写）由 sloop_core_selftest 内部成对执行：
         * 全量 182 例跑完后恢复 UI 定时器/输入与引擎读写。UI 未启动时静默为空操作。 */
        return sloop_core_selftest(argc == 3 ? argv[2] : NULL) == 0 ? 0 : 1;
    }
    if (argc == 5 && strcasecmp(argv[1], "track") == 0) {
        uint32_t k, engine, preset;
        if (!eng_uint(argv[2], 2, &k) || !eng_uint(argv[3], SLOOP_CORE_NENGINES - 1u, &engine) ||
            !eng_uint(argv[4], 255, &preset)) {
            printf("用法: ENG TRACK <k=0..2> <engine=0..11> <preset=0..255>（换引擎/预置并载入参数）\n");
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
    if (argc == 5 && strcasecmp(argv[1], "preset") == 0) {
        /* 期望向量（Task 5 注入对端）：同一「当前 p[] + 引擎 + 预置」的**纯写值**，
         * 无副作用（不改引擎/预置、不触发 panic）。`ENG TRACK` 应用后 `PARAM <id>`
         * 逐项回读必须与它 71/71 相同——旧 sloop_core_track 只写索引、p[] 不变。 */
        uint32_t k, engine, preset;
        if (!eng_uint(argv[2], 2, &k) || !eng_uint(argv[3], SLOOP_CORE_NENGINES - 1u, &engine) ||
            !eng_uint(argv[4], 255, &preset)) {
            printf("用法: ENG PRESET <k=0..2> <engine=0..11> <preset=0..255>\n");
            return 1;
        }
        if (sloop_core_status() != ESP_OK) {
            printf("ENG PRESET FAIL init=%s\n", esp_err_to_name(sloop_core_status()));
            return 1;
        }
        int16_t v[SLOOP_CORE_P_COUNT];
        if (!sloop_core_preset_preview(k, engine, preset, v)) {
            /* 诊断冻结（`ENG PRESET` 在 ENG SELFTEST 期间被调用）或越界：不打印零向量，
             * 免得注入侧把全零当成合法期望。 */
            printf("ENG PRESET ERR frozen\n");
            return 1;
        }
        printf("ENG PRESET k=%u engine=%u preset=%u\n", (unsigned)k, (unsigned)engine,
               (unsigned)preset);
        for (uint32_t i = 0; i < SLOOP_CORE_P_COUNT; i++) printf("P%u=%d\n", (unsigned)i, (int)v[i]);
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
            printf("ENG SNAP trk k=%u engine=%u preset=%u level=%u mute=%u armed=%u solo=%u len=%u div=%u voices=%u\n",
                   (unsigned)k, (unsigned)s.trk[k].engine, (unsigned)s.trk[k].preset,
                   (unsigned)s.trk[k].level, (unsigned)s.trk[k].mute, (unsigned)s.trk[k].armed,
                   (unsigned)s.trk[k].solo, (unsigned)s.trk[k].len, (unsigned)s.trk[k].div,
                   (unsigned)s.trk[k].voices);
        }
        for (uint32_t i = 0; i < 16; i++) {
            printf("ENG SNAP dstep i=%u on=%u lvl=%u rat=%u\n", (unsigned)i,
                   (unsigned)s.dstep[i].on, (unsigned)s.dstep[i].lvl, (unsigned)s.dstep[i].rat);
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
    if (argc == 5 && strcasecmp(argv[1], "desc") == 0) {
        /* 描述符投影抽样（scripts/desc-check.py 对端）：scope=track → TP[id]；
         * global → GP[id]；engine → ENGINES[engine]->edit[id]，engine=12 为鼓轨
         * 哨兵（id=0 → DRUM_KIT_DESC）。label/unit/first/last 加引号（上游 label
         * 可含空格，如 "INS A"），空单位/无枚举名打印 "-"。 */
        uint32_t engine = 0, id = 0;
        sloop_desc_scope_t scope = SLOOP_DESC_TRACK;
        const char *scope_name = NULL;
        if (strcasecmp(argv[2], "track") == 0) {
            scope = SLOOP_DESC_TRACK;
            scope_name = "track";
        } else if (strcasecmp(argv[2], "global") == 0) {
            scope = SLOOP_DESC_GLOBAL;
            scope_name = "global";
        } else if (strcasecmp(argv[2], "engine") == 0) {
            scope = SLOOP_DESC_ENGINE;
            scope_name = "engine";
        }
        if (scope_name == NULL || !eng_uint(argv[3], 255, &engine) || !eng_uint(argv[4], 255, &id)) {
            printf("用法: ENG DESC <track|global|engine> <engine=0..12> <id=0..255>（engine=12 为鼓轨 kit 哨兵；越界值回 ERR）\n");
            return 1;
        }
        sloop_desc_t d;
        if (!sloop_core_desc(scope, engine, id, &d)) {
            printf("ENG DESC ERR scope=%s engine=%u id=%u\n", scope_name, (unsigned)engine, (unsigned)id);
            return 1;
        }
        printf("ENG DESC scope=%s engine=%u id=%u label=\"%s\" fmt=%u min=%d max=%d def=%d unit=\"%s\" enums=%u first=\"%s\" last=\"%s\"\n",
               scope_name, (unsigned)engine, (unsigned)id, d.label != NULL ? d.label : "-", (unsigned)d.fmt,
               (int)d.min, (int)d.max, (int)d.def, d.unit != NULL ? d.unit : "-", (unsigned)d.enum_count,
               d.enum_count > 0 ? d.enums[0] : "-", d.enum_count > 0 ? d.enums[d.enum_count - 1] : "-");
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "mem") == 0) {
        /* 阶段 3 内存门禁：UI 起来后的 idle 内部堆须 ≥60KB（spec §4） */
        printf("ENG MEM internal_free=%u internal_min=%u psram_free=%u psram_min=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "freeze") == 0 &&
        (strcasecmp(argv[2], "on") == 0 || strcasecmp(argv[2], "off") == 0)) {
        /* 诊断（评审 minor 1 的可测入口；**必须成对**）：冻结引擎读写（与 `ENG SELFTEST`
         * 内部同一入口）——冻结期间 read 返回冻结快照、写 API 丢弃、`ENG PRESET` 回
         * `ERR frozen`（不打印零向量）。`ENG SELFTEST` 会自行成对执行，无需手动冻结；
         * 手动 on 之后务必 off，否则引擎编辑/UI 投影停在冻结时刻。 */
        bool on = strcasecmp(argv[2], "on") == 0;
        sloop_core_diag_freeze(on);
        printf("ENG FREEZE %s\n", on ? "on" : "off");
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "perf") == 0) return eng_perf();
    printf("用法: ENG INFO|SNAP|STEP <track=0..2> <idx=0..63>|SMOKE|SELFTEST [case]|TRACK <k> <engine> <preset>|PRESET <k> <engine> <preset>|TRIG <synth|drum> <a> <b>|DESC <track|global|engine> <engine> <id>|PERF|MEM|AUDIO [ON|OFF]|FREEZE <on|off>（诊断，成对）\n");
    return 1;
}

/* 上游 P_* 与 G_* 枚举镜像（仅诊断回显用；数值与 params.c 的枚举顺序一致，漂移会被
 * ENG DESC/PARAM 与设备断言暴露）。P_E0 = P_COUNT - 8：TP 表末尾 8 个引擎槽的起点。 */
enum {
    CONSOLE_P_LEVEL = 0, CONSOLE_P_ATK = 1, CONSOLE_P_DEC = 2, CONSOLE_P_SUS = 3,
    CONSOLE_P_REL = 4, CONSOLE_P_SLEN = 29, CONSOLE_P_MUTE = 40,
    CONSOLE_G_BPM = 0, CONSOLE_G_SWING = 1,
};

/* STATE 排障入口：打印 AppState（UI 渲染源），与 ENG SNAP（引擎真值）互补。
 * 选轨/mute/solo 已是引擎真值，两处输出同源（sync 无状态层例外）；参数投影
 * p[]/g[] 抽样打印（P_LEVEL/P_ATK/P_DEC/P_SUS/P_REL/P_SLEN/P_MUTE + G_BPM/G_SWING）。 */
static int state_command(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "snap") == 0) {
        app_state_t st;
        state_get_copy(&st);
        printf("STATE SNAP page=%u focus_track=%u focus_step=%u layer_held=0x%02X selected_track=%u last_note=%u bpm=%u playing=%u window_bank=%u window_start=%u window_len=%u\n",
               (unsigned)st.page, (unsigned)st.focus_track, (unsigned)st.focus_step,
               (unsigned)st.layer_held, (unsigned)st.core.selected_track, (unsigned)st.last_note,
               (unsigned)st.core.bpm, (unsigned)st.core.playing, (unsigned)st.window.bank,
               (unsigned)st.window.window_start, (unsigned)st.window.window_len);
        for (uint32_t k = 0; k < 4; k++) {
            printf("STATE SNAP trk k=%u mute=%u solo=%u\n", (unsigned)k,
                   (unsigned)st.core.trk[k].mute, (unsigned)st.core.trk[k].solo);
        }
        printf("STATE SNAP param trk=%u level=%d atk=%d dec=%d sus=%d rel=%d slen=%d mute=%d\n",
               (unsigned)st.core.selected_track, (int)st.p[CONSOLE_P_LEVEL], (int)st.p[CONSOLE_P_ATK],
               (int)st.p[CONSOLE_P_DEC], (int)st.p[CONSOLE_P_SUS], (int)st.p[CONSOLE_P_REL],
               (int)st.p[CONSOLE_P_SLEN], (int)st.p[CONSOLE_P_MUTE]);
        printf("STATE SNAP global bpm=%d swing=%d\n", (int)st.g[CONSOLE_G_BPM],
               (int)st.g[CONSOLE_G_SWING]);
        return 0;
    }
    printf("用法: STATE SNAP（AppState 页面/焦点/层键、步窗、引擎真值的选轨/mute/solo 与参数投影；同 ENG SNAP）\n");
    return 1;
}

/* PARAM 诊断注入（阶段 4 P4-A Task 9 的设备断言入口）：走与 UI 编辑相同的状态层
 * 路径（CMD_SET_PARAM / CMD_SET_PARAM_BATCH → sloop_core_set_params），写完回读
 * 引擎真值。`PARAM <id>`（无值）是只读形式：打印当前真值 + 描述符范围（参数页
 * 编辑生效的断言入口，不改任何状态）；`PARAM GLOBAL <id> [<value>]` 是全局参数
 * （song.g[]）的读/写入口（与 GLOBAL 参数页同一状态层命令 CMD_SET_GLOBAL）。
 * id 合法性按 desc 契约预判（TRACK scope
 * 覆盖 id < P_E0；P_E0..P_E7 是引擎槽：鼓轨用哨兵 SLOOP_CORE_NENGINES）——钳位不在
 * 本层做，引擎按同一描述符钳位，回显的 applied 即钳位结果。value 接受任意 int16。 */
static bool param_parse_value(const char *text, int16_t *value)
{
    char *end = NULL;
    long parsed = strtol(text, &end, 0);
    if (end == text || *end != '\0' || parsed < INT16_MIN || parsed > INT16_MAX) return false;
    *value = (int16_t)parsed;
    return true;
}

static bool param_id_valid(uint32_t engine, uint32_t id, sloop_desc_t *desc)
{
    const uint32_t p_e0 = SLOOP_CORE_P_COUNT - 8u; /* 上游 P_E0（8 个引擎槽的起点） */
    if (id < p_e0) return sloop_core_desc(SLOOP_DESC_TRACK, 0, id, desc);
    if (id >= SLOOP_CORE_P_COUNT) return false;
    return sloop_core_desc(SLOOP_DESC_ENGINE, engine, id - p_e0, desc);
}

static int param_command(int argc, char **argv)
{
    sloop_core_snapshot_t snap;
    sloop_core_read(&snap); /* 选中轨与引擎号（鼓轨 = 哨兵）取自引擎真值 */
    uint32_t track = snap.selected_track < APP_STATE_TRACKS ? snap.selected_track : 0;
    uint32_t engine = track == 3u ? SLOOP_CORE_NENGINES : snap.trk[track].engine;
    int16_t p[SLOOP_CORE_P_COUNT], g[SLOOP_CORE_G_COUNT];

    /* PARAM GLOBAL <id> [<value>]：全局参数（song.g[]）的读/写诊断入口。写经状态层
     * CMD_SET_GLOBAL（与 GLOBAL 参数页同一路径，钳位在引擎）；读是只读回显。 */
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "global") == 0) {
        uint32_t id;
        sloop_desc_t d;
        if (!eng_uint(argv[2], 0xFFFFu, &id) ||
            !sloop_core_desc(SLOOP_DESC_GLOBAL, 0, id, &d)) {
            printf("PARAM ERR global id=%s reason=invalid_id\n", argv[2]);
            return 1;
        }
        if (argc == 4) {
            int16_t value;
            if (!param_parse_value(argv[3], &value)) {
                printf("用法: PARAM GLOBAL <id> [<value>]\n");
                return 1;
            }
            const edit_cmd_t cmd = { .type = CMD_SET_GLOBAL, .a = id, .v = (uint16_t)value };
            state_apply(&cmd);
            sloop_core_read_params(track, NULL, g);
            printf("PARAM GLOBAL id=%u value=%d applied=%d min=%d max=%d\n", (unsigned)id,
                   (int)value, (int)g[id], (int)d.min, (int)d.max);
            return 0;
        }
        sloop_core_read_params(track, NULL, g);
        printf("PARAM GLOBAL id=%u value=%d min=%d max=%d\n", (unsigned)id, (int)g[id],
               (int)d.min, (int)d.max);
        return 0;
    }

    if (argc == 2 && strcasecmp(argv[1], "batch") != 0) {
        /* 只读：PARAM <id> 打印选中轨该参数的引擎真值与描述符范围（不写） */
        uint32_t id;
        sloop_desc_t d;
        if (!eng_uint(argv[1], 0xFFFFu, &id)) {
            printf("用法: PARAM <id> [<value>] | PARAM GLOBAL <id> [<value>] | PARAM BATCH <id> <value> ... | PARAM RAW <id> <value>\n");
            return 1;
        }
        if (!param_id_valid(engine, id, &d)) {
            printf("PARAM ERR track=%u id=%u reason=invalid_id\n", (unsigned)track, (unsigned)id);
            return 1;
        }
        sloop_core_read_params(track, p, g);
        printf("PARAM track=%u id=%u value=%d min=%d max=%d\n", (unsigned)track, (unsigned)id,
               (int)p[id], (int)d.min, (int)d.max);
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "batch") != 0) {
        uint32_t id;
        int16_t value;
        sloop_desc_t d;
        if (!eng_uint(argv[1], 0xFFFFu, &id) || !param_parse_value(argv[2], &value)) {
            printf("用法: PARAM <id> [<value>] | PARAM GLOBAL <id> [<value>] | PARAM BATCH <id> <value> ... | PARAM RAW <id> <value>\n");
            return 1;
        }
        if (!param_id_valid(engine, id, &d)) {
            printf("PARAM ERR track=%u id=%u reason=invalid_id\n", (unsigned)track, (unsigned)id);
            return 1;
        }
        const edit_cmd_t cmd = { .type = CMD_SET_PARAM, .a = track, .b = id, .v = (uint16_t)value };
        state_apply(&cmd);
        sloop_core_read_params(track, p, g);
        printf("PARAM track=%u id=%u value=%d applied=%d min=%d max=%d\n", (unsigned)track,
               (unsigned)id, (int)value, (int)p[id], (int)d.min, (int)d.max);
        return 0;
    }
    if (argc == 4 && strcasecmp(argv[1], "raw") == 0) {
        uint32_t id;
        int16_t value;
        if (!eng_uint(argv[2], 0xFFFFu, &id) || !param_parse_value(argv[3], &value)) {
            printf("用法: PARAM <id> [<value>] | PARAM GLOBAL <id> [<value>] | PARAM BATCH <id> <value> ... | PARAM RAW <id> <value>\n");
            return 1;
        }
        /* 旁路：不做 desc 预判，直接经状态层命令到引擎批量入口——用于设备上复验
         * 引擎对无描述符 id（越界 / 鼓轨 P_E1..P_E7）的逐条忽略。applied 是回读到
         * 的现有值（id 超出 p[] 范围时打印 n/a），与 value 不同即证明未写。 */
        const edit_cmd_t cmd = { .type = CMD_SET_PARAM, .a = track, .b = id, .v = (uint16_t)value };
        state_apply(&cmd);
        sloop_core_read_params(track, p, g);
        if (id < SLOOP_CORE_P_COUNT) {
            printf("PARAM RAW track=%u id=%u value=%d applied=%d\n", (unsigned)track,
                   (unsigned)id, (int)value, (int)p[id]);
        } else {
            printf("PARAM RAW track=%u id=%u value=%d applied=n/a\n", (unsigned)track,
                   (unsigned)id, (int)value);
        }
        return 0;
    }
    if (argc >= 4 && strcasecmp(argv[1], "batch") == 0 && ((argc - 2) % 2) == 0) {
        uint32_t n = (uint32_t)(argc - 2) / 2u;
        sloop_param_pair_t pairs[APP_STATE_PARAM_BATCH_MAX];
        sloop_desc_t d;
        if (n > APP_STATE_PARAM_BATCH_MAX) {
            printf("PARAM ERR n=%u reason=too_many max=%u\n", (unsigned)n,
                   (unsigned)APP_STATE_PARAM_BATCH_MAX);
            return 1;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t id;
            int16_t value;
            if (!eng_uint(argv[2 + 2u * i], 0xFFFFu, &id) ||
                !param_parse_value(argv[3 + 2u * i], &value)) {
                printf("用法: PARAM <id> [<value>] | PARAM GLOBAL <id> [<value>] | PARAM BATCH <id> <value> ... | PARAM RAW <id> <value>\n");
                return 1;
            }
            if (!param_id_valid(engine, id, &d)) {
                printf("PARAM ERR track=%u id=%u reason=invalid_id\n", (unsigned)track, (unsigned)id);
                return 1;
            }
            pairs[i].id = (uint8_t)id;
            pairs[i].v = value;
        }
        const edit_cmd_t cmd = { .type = CMD_SET_PARAM_BATCH, .a = track, .p = pairs, .n = n };
        state_apply(&cmd);
        sloop_core_read_params(track, p, g);
        printf("PARAM BATCH track=%u n=%u", (unsigned)track, (unsigned)n);
        for (uint32_t i = 0; i < n; i++) {
            printf(" id=%u value=%d applied=%d", (unsigned)pairs[i].id, (int)pairs[i].v,
                   (int)p[pairs[i].id]);
        }
        printf("\n");
        return 0;
    }
    printf("用法: PARAM <id> [<value>] | PARAM GLOBAL <id> [<value>] | PARAM BATCH <id> <value> ... | PARAM RAW <id> <value>（选中轨，经状态层参数命令；钳位在引擎；无 value 时只读回显）\n");
    return 1;
}

/* UI 切页（注入用）：UI <play|tracks|step|transport|global|fx|analog|fm6|sample>；无参打印当前页。
 * 切页本身在 sloop_ui 内部加 LVGL 锁；这里只做参数解析与回显。
 * UI QUIESCE <on|off>：手动复验诊断期静默（`ENG SELFTEST` 内部同一入口）——
 * 用于验证 KEY/TAP 在静默期被拒、恢复后 UI 定时器重新刷新。
 * UI MEM：页面注册表/懒建池水位（ADR-008；每页 est 预算 vs 实测 delta）。
 * UI FORCEFAIL <page> [on|off]：强制该页构建失败（验证失败可见性）。已建当前页
 * 立即重建到失败态（注入当场消费）；已建非当前页回到未建态（注入待命）；未建页挂起
 * 到下一次构建；off 只撤销未消费的注入。
 * UI WINDOW [<start> <len>]：步窗（AppState 的 UI 局部态）——start 为引擎绝对步号
 * （向下对齐 16、钳位 0..48，bank = start/16），len 1..16；无参打印当前窗口。
 * 状态层经 CMD_SET_WINDOW 落地并立即重取窗口列投影，STEP 页下一拍刷新。 */
static const char *const k_page_names[APP_STATE_PAGES] = {
    "play", "tracks", "step", "transport", "global", "fx", "analog", "fm6", "sample",
};
_Static_assert(sizeof(k_page_names) / sizeof(k_page_names[0]) == APP_STATE_PAGES,
               "console 页名表与 app_page_t/APP_STATE_PAGES 不同步");

/* 页名（不区分大小写）或十进制页号 → 页 id；失败返回 false */
static bool ui_parse_page(const char *arg, uint8_t *out)
{
    for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
        if (strcasecmp(arg, k_page_names[i]) == 0) {
            *out = i;
            return true;
        }
    }
    char *end = NULL;
    unsigned long v = strtoul(arg, &end, 10);
    if (end == arg || *end != '\0' || v >= APP_STATE_PAGES) return false;
    *out = (uint8_t)v;
    return true;
}

static int ui_mem_dump(void)
{
    sloop_ui_pool_stats_t ps;
    esp_err_t ret = sloop_ui_page_pool_stats(&ps);
    if (ret == ESP_ERR_INVALID_STATE) {
        printf("UI ERR not_started\n");
        return 1;
    }
    if (ret == ESP_ERR_TIMEOUT) {
        printf("UI ERR lock_timeout\n");
        return 1;
    }
    if (ret != ESP_OK) {
        printf("UI ERR mem (%s)\n", esp_err_to_name(ret));
        return 1;
    }
    static const char *const states[] = {"unbuilt", "built", "failed"};
    uint32_t sum_est = 0;
    for (uint8_t i = 0; i < ps.registered; i++) sum_est += ps.pages[i].est_bytes;
    printf("UI MEM pool_used=%u pool_free=%u pool_total=%u registered=%u built=%u failed=%u sum_est=%u reserve=%d\n",
           (unsigned)ps.pool_used, (unsigned)ps.pool_free, (unsigned)ps.pool_total,
           (unsigned)ps.registered, (unsigned)ps.built, (unsigned)ps.failed, (unsigned)sum_est,
           (int)(ps.pool_total - sum_est));
    for (uint8_t i = 0; i < ps.registered; i++) {
        const sloop_ui_page_stat_t *p = &ps.pages[i];
        printf("UI MEM page id=%u title=%s state=%s est=%u delta=%d\n", (unsigned)p->id,
               p->title, states[p->state < 3 ? p->state : 0], (unsigned)p->est_bytes,
               (int)p->delta_bytes);
    }
    return 0;
}

/* 页状态名（`UI MEM`/`UI FORCEFAIL` 回显共用）；未注册或统计失败返回固定串 */
static const char *ui_page_state_name(uint8_t page)
{
    static const char *const states[] = {"unbuilt", "built", "failed"};
    sloop_ui_pool_stats_t ps;
    if (sloop_ui_page_pool_stats(&ps) != ESP_OK) return "?";
    for (uint8_t i = 0; i < ps.registered; i++) {
        if (ps.pages[i].id != page) continue;
        return states[ps.pages[i].state < 3 ? ps.pages[i].state : 0];
    }
    return "not_registered";
}

/* UI LOCKHOLD <ms>（调试注入，验证 quiesce 失败路径）：辅助任务持 LVGL 锁 ms
 * 毫秒（≥1100），命令等持锁确认后返回；紧随其后的 `ENG SELFTEST` 会在 quiesce
 * 阶段等锁 1s 超时，以 `result=FAIL reason=quiesce` 拒绝（UI 继续运行，不冻结引擎）。 */
static volatile bool s_lockhold_held;

static void lockhold_task(void *arg)
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    if (bsp_display_lock(1000)) {
        s_lockhold_held = true;
        vTaskDelay(pdMS_TO_TICKS(ms));
        bsp_display_unlock();
    }
    s_lockhold_held = false;
    vTaskDelete(NULL);
}

static int ui_lockhold_command(const char *arg)
{
    char *end = NULL;
    long ms = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || ms < 1100 || ms > 30000) {
        printf("用法: UI LOCKHOLD <ms=1100..30000>（辅助任务持 LVGL 锁，quiesce 锁超时探针）\n");
        return 1;
    }
    s_lockhold_held = false;
    if (xTaskCreate(lockhold_task, "ui_lockhold", 2048, (void *)(uintptr_t)ms, 3, NULL) != pdPASS) {
        printf("UI ERR lockhold (task)\n");
        return 1;
    }
    for (int i = 0; i < 100 && !s_lockhold_held; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!s_lockhold_held) {
        printf("UI LOCKHOLD ERR not_acquired\n");
        return 1;
    }
    printf("UI LOCKHOLD ms=%ld held=1\n", ms);
    return 0;
}

static int ui_command(int argc, char **argv)
{
    if (argc == 1) {
        uint8_t page = sloop_ui_current_page();
        if (page >= APP_STATE_PAGES) {
            printf("UI ERR not_started\n");
            return 1;
        }
        printf("UI page=%s (%u)\n", k_page_names[page], (unsigned)page);
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "mem") == 0) return ui_mem_dump();
    if (argc == 6 && strcasecmp(argv[1], "knob") == 0) {
        /* UI KNOB <slot> PRESS|DRAG <dx> <dy>：向当前参数页的旋钮派发合成指针事件
         * （走物理触摸同一路径，ADR-008 §8.2/§8.5）。dx/dy 相对旋钮中心，可负。 */
        char *end = NULL;
        long slot = strtol(argv[2], &end, 0);
        bool bad = end == argv[2] || *end != '\0' || slot < 0 || slot >= 8;
        bool press = strcasecmp(argv[3], "press") == 0;
        long dx = 0, dy = 0;
        if (!bad && (press || strcasecmp(argv[3], "drag") == 0)) {
            end = NULL;
            dx = strtol(argv[4], &end, 0);
            bad = end == argv[4] || *end != '\0' || dx < -1000 || dx > 1000;
            end = NULL;
            dy = strtol(argv[5], &end, 0);
            bad = bad || end == argv[5] || *end != '\0' || dy < -1000 || dy > 1000;
        } else {
            bad = true;
        }
        if (bad) {
            printf("用法: UI KNOB <slot=0..7> <press|drag> <dx> <dy>（dx/dy 相对旋钮中心）\n");
            return 1;
        }
        if (sloop_ui_quiesced()) {
            printf("UI ERR quiesced\n");
            return 1;
        }
        uint8_t page = sloop_ui_current_page();
        if (page >= APP_STATE_PAGES) {
            printf("UI ERR not_started\n");
            return 1;
        }
        if (page < APP_PAGE_GLOBAL) {
            printf("UI ERR not_params\n");
            return 1;
        }
        int16_t value = 0;
        esp_err_t ret = sloop_ui_test_knob((uint8_t)slot, (int32_t)dx, (int32_t)dy, press, &value);
        const char *op = press ? "press" : "drag";
        if (ret == ESP_OK) {
            printf("UI KNOB page=%u slot=%u %s dx=%ld dy=%ld value=%d\n", (unsigned)page,
                   (unsigned)slot, op, dx, dy, (int)value);
            return 0;
        }
        if (ret == ESP_ERR_NOT_FOUND) {
            /* 环带外/无锚点：事件未被接受（值不变）——与 miss 同口径返回非零，令牌独立 */
            printf("UI KNOB page=%u slot=%u %s dx=%ld dy=%ld ignored (ring band)\n",
                   (unsigned)page, (unsigned)slot, op, dx, dy);
            return 1;
        }
        if (ret == ESP_ERR_NOT_ALLOWED) {
            /* 引擎页不适用（选中轨引擎与页声明不符/鼓轨）：旋钮显示 --，事件被拒 */
            printf("UI KNOB page=%u slot=%u %s dx=%ld dy=%ld ignored (locked)\n",
                   (unsigned)page, (unsigned)slot, op, dx, dy);
            return 1;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            printf("UI ERR lock_timeout\n");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            printf("UI ERR page_build %u\n", (unsigned)page);
        } else {
            printf("UI ERR knob_slot %ld\n", slot);
        }
        return 1;
    }
    if ((argc == 2 || argc == 4) && strcasecmp(argv[1], "window") == 0) {
        if (argc == 2) {
            step_window_t w;
            step_window(&w);
            printf("UI WINDOW bank=%u start=%u len=%u\n", (unsigned)w.bank,
                   (unsigned)w.window_start, (unsigned)w.window_len);
            return 0;
        }
        uint32_t start = 0, len = 0;
        if (!eng_uint(argv[2], 63, &start) || !eng_uint(argv[3], APP_STATE_STEPS, &len)) {
            printf("用法: UI WINDOW <start=0..63> <len=1..16>（start 向下对齐 16，bank = start/16）\n");
            return 1;
        }
        /* a=bank 冗余（由 start 派生），b=start，v=len：钳位统一在状态层 */
        const edit_cmd_t cmd = { .type = CMD_SET_WINDOW,
                                 .a = start / 16u,
                                 .b = start,
                                 .v = (uint16_t)len };
        state_apply(&cmd);
        step_window_t w;
        step_window(&w); /* 回显落地值（钳位后），脚本据此派生几何 */
        printf("UI WINDOW bank=%u start=%u len=%u\n", (unsigned)w.bank,
               (unsigned)w.window_start, (unsigned)w.window_len);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "forcefail") == 0) {
        uint8_t page = 0;
        if (!ui_parse_page(argv[2], &page)) {
            printf("UI ERR page (want=play|tracks|step|transport|global|fx|analog|fm6|sample|0..%u)\n",
                   (unsigned)(APP_STATE_PAGES - 1));
            return 1;
        }
        bool on = argc == 3 || strcasecmp(argv[3], "on") == 0;
        if (argc == 4 && !on && strcasecmp(argv[3], "off") != 0) {
            printf("用法: UI FORCEFAIL <page> [on|off]\n");
            return 1;
        }
        esp_err_t ret = sloop_ui_force_page_build_failure(page, on);
        if (ret == ESP_ERR_INVALID_STATE) {
            printf("UI ERR not_started\n");
            return 1;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            printf("UI ERR lock_timeout\n");
            return 1;
        }
        if (ret != ESP_OK) {
            printf("UI ERR forcefail (%s)\n", esp_err_to_name(ret));
            return 1;
        }
        /* 回显注入**之后**的页状态：已建当前页会立即重建到失败态（注入当场被消费），
         * 已建非当前页回到 unbuilt、注入仍待命，未建页同样保持 unbuilt 等下一次
         * 构建失败——不打印会误导的 armed 标志。 */
        printf("UI FORCEFAIL page=%s (%u) %s state=%s\n", k_page_names[page], (unsigned)page,
               on ? "on" : "off", ui_page_state_name(page));
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "quiesce") == 0 &&
        (strcasecmp(argv[2], "on") == 0 || strcasecmp(argv[2], "off") == 0)) {
        bool on = strcasecmp(argv[2], "on") == 0;
        if (sloop_ui_current_page() >= APP_STATE_PAGES) {
            printf("UI ERR not_started\n");
            return 1;
        }
        if (!sloop_ui_quiesce(on)) {
            /* 未达到目标态（取锁超时）：ON 已回滚输入静默（UI 仍在跑），OFF 保持
             * 静默态（可重试）。与成功回显分开，脚本不得把失败当已落地。 */
            printf("UI QUIESCE %s ERR lock_timeout quiesced=%u\n", on ? "on" : "off",
                   (unsigned)sloop_ui_quiesced());
            return 1;
        }
        printf("UI QUIESCE %s quiesced=%u\n", on ? "on" : "off",
               (unsigned)sloop_ui_quiesced());
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "lockhold") == 0) {
        return ui_lockhold_command(argv[2]);
    }
    if (argc == 2) {
        for (uint8_t i = 0; i < APP_STATE_PAGES; i++) {
            if (strcasecmp(argv[1], k_page_names[i]) != 0) continue;
            esp_err_t ret = sloop_ui_switch_page(i);
            if (ret == ESP_ERR_TIMEOUT) {
                /* 与 miss 区分：锁超时没有落地，脚本的 miss/页断言不得误判为通过 */
                printf("UI ERR lock_timeout\n");
                return 1;
            }
            if (ret == ESP_ERR_NO_MEM) {
                /* 目标页构建失败：页面已切到错误状态条（可见），与「未启动」区分 */
                printf("UI ERR page_build %u\n", (unsigned)i);
                return 1;
            }
            if (ret == ESP_ERR_INVALID_ARG) {
                /* 页 id 合法但注册表里没有（扩页时忘了 ui_page_register）：与未启动区分 */
                printf("UI ERR not_registered %u\n", (unsigned)i);
                return 1;
            }
            if (ret != ESP_OK) {
                printf("UI ERR not_started (want=%s)\n", k_page_names[i]);
                return 1;
            }
            uint8_t page = sloop_ui_current_page();
            if (page != i) {
                printf("UI ERR not_started (want=%s)\n", k_page_names[i]);
                return 1;
            }
            printf("UI page=%s (%u)\n", k_page_names[page], (unsigned)page);
            return 0;
        }
    }
    printf("用法: UI <play|tracks|step|transport|global|fx|analog|fm6|sample> | UI MEM | UI WINDOW [<start> <len>] | "
           "UI KNOB <slot> <press|drag> <dx> <dy> | "
           "UI FORCEFAIL <page> [on|off] | UI QUIESCE <on|off> | UI LOCKHOLD <ms>（无参打印当前页）\n"
           "WINDOW: start 向下对齐 16（bank = start/16，0..48），len 1..16；无参打印当前窗口\n"
           "FORCEFAIL: 已建当前页立即重建到失败态（回显 state=failed）；已建非当前页回到未建态"
           "（state=unbuilt，注入待命）；未建页下一次构建失败；"
           "off 只撤销未消费的注入\n"
           "LOCKHOLD: 辅助任务持 LVGL 锁 ms 毫秒，紧随的 ENG SELFTEST 应回 result=FAIL reason=quiesce\n");
    return 1;
}

/* KEY（InputManager 注入）：与物理键盘同一入口 sloop_ui_input_key()。
 * 延迟口径 = 命令入口 → 该调用返回（含 state_apply 的状态互斥与引擎写），
 * 每条命令打印 elapsed_us，KEY STATS 给出累计均值/最大。 */
static uint32_t s_key_calls;
static uint64_t s_key_us_sum;
static uint32_t s_key_us_max;

static int key_command(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "stats") == 0) {
        printf("KEY STATS count=%u mean_us=%u max_us=%u input_stack_free=%u quiesce_dropped=%u\n",
               (unsigned)s_key_calls, (unsigned)(s_key_calls ? s_key_us_sum / s_key_calls : 0),
               (unsigned)s_key_us_max, (unsigned)sloop_ui_input_stack_free(),
               (unsigned)sloop_ui_input_quiesce_dropped());
        return 0;
    }
    if (argc == 2 && strcasecmp(argv[1], "panic") == 0) {
        sloop_ui_input_panic();
        printf("KEY PANIC OK（音符/层键/待切页已清）\n");
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[1], "stats") == 0 && strcasecmp(argv[2], "reset") == 0) {
        s_key_calls = 0;
        s_key_us_sum = 0;
        s_key_us_max = 0;
        printf("KEY STATS reset\n");
        return 0;
    }
    if (argc == 3) {
        char *end = NULL;
        unsigned long code = strtoul(argv[1], &end, 0);
        bool down = strcasecmp(argv[2], "down") == 0;
        if (end != argv[1] && *end == '\0' && code <= 0x7F && (down || strcasecmp(argv[2], "up") == 0)) {
            int64_t t0 = esp_timer_get_time();
            sloop_ui_input_key((uint8_t)code, down);
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            if (sloop_ui_quiesced()) {
                /* 诊断期：与物理键盘同入口被丢弃（计数在 input 侧，KEY STATS
                 * quiesce_dropped 可回读）；独立令牌与正常回显区分 */
                printf("KEY ERR quiesced\n");
                return 1;
            }
            s_key_calls++;
            s_key_us_sum += us;
            if (us > s_key_us_max) s_key_us_max = us;
            printf("KEY 0x%02lX %s %s elapsed_us=%u\n", code, down ? "down" : "up",
                   tab5_kb_key_name((uint8_t)code), (unsigned)us);
            return 0;
        }
    }
    printf("用法: KEY <code=十进制|0xhex> <down|up> | KEY STATS [RESET] | KEY PANIC\n"
           "code = tab5_keyboard 事件码 (row<<4|col)：sym=0x30 Aa=0x31 ctrl=0x40 alt=0x41 "
           "z=0x42 space=0x4D esc=0x00 tab=0x20\n"
           "PANIC = 全停（释放已按下音符、清层键与待切页）\n");
    return 1;
}

/* TAP（触摸注入）：TAP <x> <y>，屏幕绝对坐标（含页签条，与触摸 indev 同口径）。
 * 按当前页 hit-test 表派发与控件回调相同的动作函数（ADR-005：EditCmd 层等价，
 * 不声称原始指针事件等价）。命中打印动作与参数；未命中打印
 * "TAP miss x,y page=P" 并返回 1（不改状态、不崩溃）。坐标上界取 SCR_W/SCR_H
 * （与截屏帧同一屏幕几何）。sloop_ui 未导出宽高，其私有 theme.h 的 UI_W/UI_H
 * 即同值 1280×720；main 不跨组件包含 sloop_ui/src 的私有头，故此处沿用 SCR_W/SCR_H。 */
static int tap_command(int argc, char **argv)
{
    long x = 0, y = 0;
    bool args_ok = argc == 3;
    if (args_ok) {
        char *end = NULL;
        x = strtol(argv[1], &end, 10);
        args_ok = end != argv[1] && *end == '\0' && x >= 0 && x < SCR_W;
    }
    if (args_ok) {
        char *end = NULL;
        y = strtol(argv[2], &end, 10);
        args_ok = end != argv[2] && *end == '\0' && y >= 0 && y < SCR_H;
    }
    if (!args_ok) {
        printf("用法: TAP <x=0..%d> <y=0..%d>（屏幕绝对坐标，含 72px 页签条；按当前页 hit-test 表）\n",
               SCR_W - 1, SCR_H - 1);
        return 1;
    }

    sloop_tap_result_t r;
    sloop_tap_result_status_t status = sloop_ui_input_tap((int16_t)x, (int16_t)y, &r);
    unsigned page = (unsigned)sloop_ui_current_page();
    if (status == SLOOP_TAP_RESULT_LOCK_TIMEOUT) {
        /* 锁超时 = 没派发：单独的错误令牌，避免被脚本的 miss 断言当成「正常未命中」 */
        printf("TAP ERR lvgl_lock_timeout\n");
        return 1;
    }
    if (status == SLOOP_TAP_RESULT_QUIESCED) {
        /* 诊断期静默 = 没派发（与 miss/lock_timeout 分开） */
        printf("TAP ERR quiesced\n");
        return 1;
    }
    if (status != SLOOP_TAP_RESULT_OK) {
        if (page == 0xFF) {
            printf("TAP miss %ld,%ld page=n/a\n", x, y);
        } else {
            printf("TAP miss %ld,%ld page=%u\n", x, y, page);
        }
        return 1;
    }
    switch (r.kind) {
    case SLOOP_TAP_STEP_CELL:
        printf("TAP %ld,%ld page=%u -> STEP track=%u step=%u CMD_TOGGLE_STEP\n", x, y, page,
               (unsigned)r.a, (unsigned)r.b);
        break;
    case SLOOP_TAP_STEP_DRUM:
        printf("TAP %ld,%ld page=%u -> STEP drum step=%u read-only\n", x, y, page, (unsigned)r.b);
        break;
    case SLOOP_TAP_TRACK_CARD:
        printf("TAP %ld,%ld page=%u -> TRACKS select track=%u CMD_SELECT_TRACK\n", x, y, page,
               (unsigned)r.a);
        break;
    case SLOOP_TAP_TRANSPORT_PLAY:
        printf("TAP %ld,%ld page=%u -> TRANSPORT CMD_PLAY\n", x, y, page);
        break;
    case SLOOP_TAP_TRANSPORT_STOP:
        printf("TAP %ld,%ld page=%u -> TRANSPORT CMD_STOP\n", x, y, page);
        break;
    case SLOOP_TAP_TRANSPORT_BPM:
        printf("TAP %ld,%ld page=%u -> TRANSPORT CMD_SET_BPM delta=%+d\n", x, y, page, (int)r.v);
        break;
    case SLOOP_TAP_PARAM_STEP:
        printf("TAP %ld,%ld page=%u -> PARAM slot=%u id=%u delta=%+d value=%d\n", x, y, page,
               (unsigned)r.a, (unsigned)r.b, (int)r.d, (int)r.v);
        break;
    case SLOOP_TAP_PARAM_FOCUS:
        printf("TAP %ld,%ld page=%u -> PARAM slot=%u focus\n", x, y, page, (unsigned)r.a);
        break;
    case SLOOP_TAP_PARAM_LOCKED:
        printf("TAP %ld,%ld page=%u -> PARAM slot=%u locked (engine page N/A)\n", x, y, page,
               (unsigned)r.a);
        break;
    default:
        printf("TAP %ld,%ld page=%u -> (unknown)\n", x, y, page);
        break;
    }
    return 0;
}

/* STORE：存储与工程诊断（P4-B）：后端状态 + 空闲扇区擦写自检 + 故障注入 +
 * 工程 A–D 的存/读/擦/hash（后续任务在同一 handler 里扩展 PRESET/BANK/TEST）。 */
#define STORE_RAWTEST_OFF 0xFB000u /* 逻辑 flash-map 偏移：空闲扇区，sloopstore 内 0x64000 */
#define STORE_RAWTEST_SECTOR 0x1000u
#define STORE_RAWTEST_PATTERN 0x5Au
#define STORE_RAWTEST_CHUNK 256u

static int store_stats(void)
{
    sloop_store_stats_t st;
    sloop_store_stats(&st);
    printf("STORE STATS ready=%u base=0x%X span=0x%X map=0x%lX objects=%u faults=%u armed=%u\n",
           st.ready ? 1u : 0u, (unsigned)sloop_hal_storage_base(),
           (unsigned)sloop_hal_storage_span(), (unsigned long)(uintptr_t)sloop_hal_storage_map(),
           (unsigned)st.objects_ok, (unsigned)st.faults_taken, (unsigned)st.faults_armed);
    return 0;
}

/* 走 mmap 读回（与 Task 3 的 ST_XIP_PTR 同源）：顺带验证擦写后 cache 已失效 */
static bool store_rawtest_verify(void)
{
    uint8_t buf[STORE_RAWTEST_CHUNK];
    for (uint32_t off = 0; off < STORE_RAWTEST_SECTOR; off += sizeof buf) {
        if (sloop_hal_storage_read(STORE_RAWTEST_OFF + off, buf, sizeof buf) != 0) return false;
        for (uint32_t i = 0; i < sizeof buf; i++) {
            if (buf[i] != STORE_RAWTEST_PATTERN) return false;
        }
    }
    return true;
}

static int store_rawtest(void)
{
    if (!sloop_hal_storage_ready()) {
        printf("STORE ERR no_storage\n");
        return 1;
    }

    uint8_t pattern[STORE_RAWTEST_CHUNK];
    memset(pattern, STORE_RAWTEST_PATTERN, sizeof pattern);

    const char *step = NULL;
    if (sloop_hal_storage_erase(STORE_RAWTEST_OFF) != 0) {
        step = "erase";
    }
    for (uint32_t off = 0; step == NULL && off < STORE_RAWTEST_SECTOR; off += sizeof pattern) {
        if (sloop_hal_storage_prog(STORE_RAWTEST_OFF + off, pattern, sizeof pattern) != 0) {
            step = "prog";
        }
    }
    if (step == NULL && !store_rawtest_verify()) step = "verify";
    /* 无论成败都恢复空闲扇区（0xFF） */
    if (sloop_hal_storage_erase(STORE_RAWTEST_OFF) != 0 && step == NULL) step = "cleanup";
    if (step != NULL) {
        printf("STORE RAWTEST FAIL off=0x%X step=%s\n", (unsigned)STORE_RAWTEST_OFF, step);
        return 1;
    }
    printf("STORE RAWTEST PASS off=0x%X part_off=0x%X\n", (unsigned)STORE_RAWTEST_OFF,
           (unsigned)(STORE_RAWTEST_OFF - sloop_hal_storage_base()));
    return 0;
}

/* 故障注入（torn-write 套件入口，Task 8 消费）：计数只由 erase/prog 递增；
 * arm 期间 read 返回 -1 但不消耗序号（spec §4.1「第 n 次写操作」）。 */
static int store_fault(int argc, char **argv)
{
    if (argc == 3 && strcasecmp(argv[2], "clear") == 0) {
        uint32_t taken = sloop_hal_storage_fault_taken();
        sloop_hal_storage_fault_clear();
        printf("STORE FAULT cleared taken=%u\n", (unsigned)taken);
        return 0;
    }
    uint32_t nth = 0;
    bool half = false;
    if ((argc == 3 || argc == 4) && eng_uint(argv[2], 9999, &nth) && nth >= 1) {
        half = argc == 4 && strcasecmp(argv[3], "half") == 0;
        if (argc == 3 || half) {
            sloop_hal_storage_fault_arm(nth, half);
            printf("STORE FAULT armed nth=%u half=%u\n", (unsigned)nth, half ? 1u : 0u);
            return 0;
        }
    }
    printf("用法: STORE FAULT <nth=1..9999> [half] | STORE FAULT CLEAR\n");
    return 1;
}

/* 工程 A–D（0..3）：RAM 槽（PSRAM）+ flash 双层。console 是诊断执行器（ADR-009 §3）：
 * 与 UI 命令共用 sloop_store_* 与同一套锁，只是能把返回码变成脚本可断言的回显。
 * 错误令牌：no_storage（未就绪）/playing（走带中）/empty（空槽）/invalid（槽号越界）/
 * save（flash 擦写或回读校验失败）。 */
static const char *store_err(esp_err_t err)
{
    if (!sloop_store_ready()) return "no_storage";
    switch (err) {
    case ESP_ERR_INVALID_STATE: return "playing";
    case ESP_ERR_NOT_FOUND: return "empty";
    case ESP_ERR_NOT_FINISHED: return "save";
    default: return "invalid";
    }
}

static int store_err_print(esp_err_t err)
{
    printf("STORE ERR %s\n", store_err(err));
    return 1;
}

static int store_proj(int argc, char **argv)
{
    if (argc == 3 && strcasecmp(argv[2], "list") == 0) {
        if (!sloop_store_ready()) return store_err_print(ESP_ERR_INVALID_STATE);
        for (uint32_t i = 0; i < SLOOP_STORE_PROJECTS; i++) {
            if (sloop_store_project_used(i))
                printf("PROJ %u USED hash=%08X\n", (unsigned)i, (unsigned)sloop_store_project_hash(i));
            else
                printf("PROJ %u EMPTY\n", (unsigned)i);
        }
        return 0;
    }
    if (argc == 4 && strcasecmp(argv[2], "synth") == 0) {
        /* 诊断（P4-B Task 5 评审 R18，测试专用；**破坏性：覆盖诊断槽 3**，槽 0–2 不碰）：
         * 构造内存工程并触发 project_load 的两条 fallback（fmt1 / foreign），回读各轨
         * engine/preset/steps/atk/e0 供注入断言。实现在 store_tab5.c（需要 project_t）。 */
        sloop_store_synth_t r;
        esp_err_t err = sloop_store_project_synth(argv[3], &r);
        if (err != ESP_OK) return store_err_print(err);
        printf("PROJ 3 SYNTH case=%s OK\n", argv[3]);
        for (uint32_t k = 0; k < 4; k++)
            printf("PROJ 3 SYNTH case=%s trk k=%u engine=%u preset=%u steps=%u atk=%d e0=%d\n",
                   argv[3], (unsigned)k, (unsigned)r.trk[k].engine, (unsigned)r.trk[k].preset,
                   (unsigned)r.trk[k].steps, (int)r.trk[k].atk, (int)r.trk[k].e0);
        return 0;
    }
    if (argc != 4) {
        printf("用法: STORE PROJ LIST | SAVE n | LOAD n | ERASE n | HASH n（n = 0..3）| "
               "SYNTH <fmt1|foreign>（诊断：覆盖诊断槽 3）\n");
        return 1;
    }
    uint32_t slot;
    if (!eng_uint(argv[3], SLOOP_STORE_PROJECTS - 1u, &slot)) return store_err_print(ESP_ERR_INVALID_ARG);
    esp_err_t err;
    if (strcasecmp(argv[2], "save") == 0) {
        if ((err = sloop_store_project_save(slot)) != ESP_OK) return store_err_print(err);
        printf("PROJ %u SAVED hash=%08X\n", (unsigned)slot, (unsigned)sloop_store_project_hash(slot));
        return 0;
    }
    if (strcasecmp(argv[2], "load") == 0) {
        if ((err = sloop_store_project_load(slot)) != ESP_OK) return store_err_print(err);
        printf("PROJ %u LOADED hash=%08X\n", (unsigned)slot, (unsigned)sloop_store_live_hash());
        return 0;
    }
    if (strcasecmp(argv[2], "erase") == 0) {
        if ((err = sloop_store_project_erase(slot)) != ESP_OK) return store_err_print(err);
        printf("PROJ %u ERASED\n", (unsigned)slot);
        return 0;
    }
    if (strcasecmp(argv[2], "hash") == 0) {
        if (!sloop_store_ready()) return store_err_print(ESP_ERR_INVALID_STATE);
        if (!sloop_store_project_used(slot)) return store_err_print(ESP_ERR_NOT_FOUND);
        printf("PROJ %u HASH=%08X\n", (unsigned)slot, (unsigned)sloop_store_project_hash(slot));
        return 0;
    }
    printf("用法: STORE PROJ LIST | SAVE n | LOAD n | ERASE n | HASH n（n = 0..3）| "
           "SYNTH <fmt1|foreign>（诊断：覆盖诊断槽 3）\n");
    return 1;
}

/* 工程往返门禁（spec §6.3）：当前引擎状态的捕获 hash，不写 flash */
static int store_livehash(void)
{
    if (!sloop_store_ready()) return store_err_print(ESP_ERR_INVALID_STATE);
    printf("LIVEHASH %08X\n", (unsigned)sloop_store_live_hash());
    return 0;
}

/* 上游 params.c 的 N_ENGNAME（12 个引擎；FELUCCA_SLICE 关闭，故无 SLICE 项）。引擎名
 * 没有进 sloop_core 的公共 API，LIST 行需要文本故在此镜像；数量由下面的静态断言锁死
 * 与 SLOOP_CORE_NENGINES 一致，名字漂移由 ENG DESC / 参数页的既有断言暴露。 */
static const char *const STORE_ENGINE_NAMES[] = {"ANALOG", "DIGITAL", "PHASE", "LOFI", "SAMPLE", "VOICE",
                                                 "TRIO",   "WHEEL",   "GRAIN", "FM6",  "PHYS",   "NOISE"};
_Static_assert(sizeof(STORE_ENGINE_NAMES) / sizeof(STORE_ENGINE_NAMES[0]) == SLOOP_CORE_NENGINES,
               "引擎名表与 SLOOP_CORE_NENGINES 一致");

static const char *store_engine_name(uint8_t engine)
{
    return engine < SLOOP_CORE_NENGINES ? STORE_ENGINE_NAMES[engine] : "-";
}

/* 鼓轨（槽/轨号已在 console 层校验过）：存/载都没有「声音」可写可读——令牌与槽号越界
 * 的 invalid 分开，脚本据此区分拒绝原因（upstream: "DRUM TRACK: NO SOUND"）。 */
static int store_err_drum(void)
{
    printf("STORE ERR drum\n");
    return 1;
}

/* 预置/工程命令的轨号上限用 APP_STATE_TRACKS（0..3，含鼓轨）；引擎快照的 trk[] 由
 * sloop_core_read 按上游 NTRK 填满，二者必须相等——否则鼓轨（3）可能在一边合法、在另一边
 * 越界，drum 与 invalid 两个令牌就会随实现漂移。 */
_Static_assert(sizeof(((const sloop_core_snapshot_t *)0)->trk) /
                       sizeof(((const sloop_core_snapshot_t *)0)->trk[0]) == APP_STATE_TRACKS,
               "APP_STATE_TRACKS 与引擎快照的轨数（上游 NTRK）一致");

/* 用户预置 32 槽（upreset.c，P4-B Task 4）。console 诊断执行器口径同 PROJ：与 UI
 * 命令共用 sloop_store_* 与同一套锁。行格式见文件头；鼓轨（存/载没有声音）回
 * "STORE ERR drum"——与槽/轨号越界的 invalid 分开，脚本据此区分拒绝原因。 */
static int store_preset(int argc, char **argv)
{
    if (argc == 3 && strcasecmp(argv[2], "list") == 0) {
        if (!sloop_store_ready()) return store_err_print(ESP_ERR_INVALID_STATE);
        for (uint32_t k = 0; k < SLOOP_STORE_PRESETS; k++) {
            char name[13];
            if (!sloop_store_preset_name(k, name)) {
                printf("PRESET %u EMPTY\n", (unsigned)k);
                continue;
            }
            printf("PRESET %u USED %s %s\n", (unsigned)k,
                   store_engine_name(sloop_store_preset_engine(k)), name);
        }
        printf("PRESET COUNT used=%u total=%u\n", (unsigned)sloop_store_preset_count(),
               (unsigned)SLOOP_STORE_PRESETS);
        return 0;
    }
    if (argc != 4 && argc != 5) {
        printf("用法: STORE PRESET LIST | SAVE n | LOAD n [track] | ERASE n（n = 0..31）\n");
        return 1;
    }
    uint32_t slot;
    if (!eng_uint(argv[3], SLOOP_STORE_PRESETS - 1u, &slot)) return store_err_print(ESP_ERR_INVALID_ARG);
    esp_err_t err;
    if (strcasecmp(argv[2], "save") == 0 && argc == 4) {
        if ((err = sloop_store_preset_store(slot)) != ESP_OK) {
            return err == ESP_ERR_INVALID_ARG ? store_err_drum() : store_err_print(err);
        }
        char name[13];
        sloop_store_preset_name(slot, name);
        printf("PRESET %u SAVED %s %s\n", (unsigned)slot,
               store_engine_name(sloop_store_preset_engine(slot)), name);
        return 0;
    }
    if (strcasecmp(argv[2], "load") == 0) {
        uint32_t track;
        if (argc == 5) {
            if (!eng_uint(argv[4], APP_STATE_TRACKS - 1u, &track)) {
                return store_err_print(ESP_ERR_INVALID_ARG);
            }
        } else {
            sloop_core_snapshot_t snap;
            sloop_core_read(&snap); /* 缺省 = 选中轨（与 UI 同一真值源） */
            track = snap.selected_track < APP_STATE_TRACKS ? snap.selected_track : 0u;
        }
        if ((err = sloop_store_preset_load(slot, track)) != ESP_OK) {
            return err == ESP_ERR_INVALID_ARG ? store_err_drum() : store_err_print(err);
        }
        printf("PRESET %u LOADED track=%u\n", (unsigned)slot, (unsigned)track);
        return 0;
    }
    if (strcasecmp(argv[2], "erase") == 0 && argc == 4) {
        if ((err = sloop_store_preset_erase(slot)) != ESP_OK) return store_err_print(err);
        printf("PRESET %u ERASED\n", (unsigned)slot);
        return 0;
    }
    printf("用法: STORE PRESET LIST | SAVE n | LOAD n [track] | ERASE n（n = 0..31）\n");
    return 1;
}

static int store_command(int argc, char **argv)
{
    if (argc == 2 && strcasecmp(argv[1], "stats") == 0) return store_stats();
    if (argc == 2 && strcasecmp(argv[1], "rawtest") == 0) return store_rawtest();
    if (argc == 2 && strcasecmp(argv[1], "livehash") == 0) return store_livehash();
    if (argc >= 2 && strcasecmp(argv[1], "fault") == 0) return store_fault(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "proj") == 0) return store_proj(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "preset") == 0) return store_preset(argc, argv);
    printf("用法: STORE STATS | STORE RAWTEST（破坏：擦写空闲扇区 0x%X） | "
           "STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n|SYNTH <fmt1|foreign> | "
           "STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n | STORE LIVEHASH | "
           "STORE FAULT <nth=1..9999> [half] | STORE FAULT CLEAR\n",
           (unsigned)STORE_RAWTEST_OFF);
    return 1;
}

esp_err_t app_console_init(void)
{
    /* 存储后端在 sloop_core_init → sloop_store_init 里初始化（ADR-009 §5 启动顺序：
     * 工厂态快照之后才 fetch）；REPL 任务虽先起，但 STORE 命令经 sloop_store_* 的
     * 「未初始化 → no_storage」门控，不会碰到未映射的分区。 */

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

    /* 先注册命令后启动 REPL（SCR/KB/AUD/ENG/STATE/UI） */
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
        .help = "SLOOP 引擎诊断：INFO/SMOKE/SELFTEST/TRACK/TRIG/DESC/PERF/MEM/AUDIO",
        .hint = "INFO|SMOKE|SELFTEST [case]|TRACK <k> <engine> <preset>|TRIG <synth|drum> <a> <b>|DESC <track|global|engine> <engine> <id>|PERF|MEM|AUDIO [ON|OFF]",
        .func = &eng_command,
    };
    ret = esp_console_cmd_register(&eng_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 ENG 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t state_cmd = {
        .command = "STATE",
        .help = "状态层自检：STATE SNAP 打印 AppState（选轨/mute/solo 为引擎真值，与 ENG SNAP 同源；含参数投影）",
        .hint = "SNAP",
        .func = &state_command,
    };
    ret = esp_console_cmd_register(&state_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 STATE 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t param_cmd = {
        .command = "PARAM",
        .help = "参数写诊断：PARAM <id> <value> 经状态层写选中轨；PARAM BATCH <id> <value> ... 真批量；PARAM RAW 跳过 desc 预判",
        .hint = "<id> <value> | BATCH <id> <value> ... | RAW <id> <value>",
        .func = &param_command,
    };
    ret = esp_console_cmd_register(&param_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 PARAM 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t ui_cmd = {
        .command = "UI",
        .help = "UI 切页/查询当前页/池水位/诊断期静默（sloop_ui 页面注册表，ADR-008 懒建）",
        .hint = "<play|tracks|step|transport|global|fx|analog|fm6|sample>|MEM|KNOB <slot> <press|drag> <dx> <dy>|FORCEFAIL <page> [on|off]|QUIESCE <on|off>",
        .func = &ui_command,
    };
    ret = esp_console_cmd_register(&ui_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 UI 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t key_cmd = {
        .command = "KEY",
        .help = "键盘注入（与物理同入口）：KEY <code> <down|up>；KEY STATS [RESET]；KEY PANIC 全停",
        .hint = "<code> <down|up> | STATS [RESET] | PANIC",
        .func = &key_command,
    };
    ret = esp_console_cmd_register(&key_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 KEY 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t tap_cmd = {
        .command = "TAP",
        .help = "触摸注入：TAP <x> <y> 按当前页 hit-test 表派发（与控件回调同一动作函数）",
        .hint = "<x=0..1279> <y=0..719>",
        .func = &tap_command,
    };
    ret = esp_console_cmd_register(&tap_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 TAP 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t store_cmd = {
        .command = "STORE",
        .help = "存储/工程诊断：STORE STATS 后端与对象状态；STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n；"
                "STORE PROJ SYNTH <fmt1|foreign> 触发 project_load 的两条 fallback（诊断，覆盖槽 3）；"
                "STORE LIVEHASH 当前工程捕获 hash；STORE RAWTEST 空闲扇区 0xFB000 擦写校验（破坏性）；"
                "STORE FAULT <nth> [half]|CLEAR 故障注入",
        .hint = "STATS|PROJ LIST|PROJ SAVE n|PROJ LOAD n|PROJ ERASE n|PROJ HASH n|PROJ SYNTH <fmt1|foreign>|LIVEHASH|RAWTEST|FAULT <nth> [half]|FAULT CLEAR",
        .func = &store_command,
    };
    ret = esp_console_cmd_register(&store_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 STORE 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_console_start_repl(repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 REPL 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
