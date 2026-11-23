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
 *    bpm/playing、seq_status 与引擎真值的选轨/mute/solo，选中轨参数投影 p[]、全局 g[]
 *    的抽样字段，以及每列合成步完整字段与鼓步聚合；与 ENG SNAP 同源）。
 *  - PARAM：参数写诊断注入（阶段 4 P4-A）：PARAM <id> <value> 经状态层
 *    CMD_SET_PARAM 写选中轨（钳位在引擎，回显 applied），PARAM BATCH <id> <value> …
 *    走一次 CMD_SET_PARAM_BATCH；id 在该轨上无描述符时回 PARAM ERR 且不写。
 *    PARAM RAW <id> <value> 跳过 desc 预判直接下发，用于验证引擎「无描述符 id
 *    逐条忽略」的路径（正常 PARAM 在 console 层就拒绝了）。
 *  - UI：切页（UI <play|tracks|step|transport|global|fx|analog|fm6|sample|dsyn|fm6p|projects|presets|song>）；无参打印当前页；QUIESCE <on|off>
 *    手动复验诊断期静默；MEM 打印页面注册表/懒建的 LVGL 池水位（每页 est 预算 vs
 *    实测 delta）；WINDOW [<start> <len>] 写步窗（start 16 对齐 0..48、len 1..16，
 *    无参打印当前窗口）；LANE [<0..15>] 读/写 STEP 页选中的鼓 lane（UI 局部态，
 *    P4-C Task 9；无参打印当前值）；DSYN [KIT <0..3>|LANE <0..15>|PAGE <0..3>] 读/写
 *    DSYN 页的 kit/lane（状态层 UI 局部态，与页面 chip 共用）与字段页（页面静态量，
 *    P4-D Task 9）；TOOL [<0..7>] 读/写 TRANSPORT 页的键盘工具焦点
 *    （P4-C Task 10；0 lvl/1 rat/2 mic/3 span/4 slide/5 rot/6 len/7 trans）；FORCEFAIL
 *    <page> [on|off] 强制构建失败（验证失败可见性，
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
 *    "TAP ERR quiesced"（都返回非零，令牌不同，脚本不得互相误判）。FILE 页
 *    （PROJECTS/PRESETS）回显 "-> FILE <page> slot=k select" 或
 *    "-> FILE <page> slot=k action=save|load|erase status=\"…\""——后者带页面状态行
 *    文本（与屏幕同源），播放中拒绝/鼓轨/NO STORAGE 可被脚本按文本断言。STEP 鼓格
 *    回显 "-> STEP drum step=k lane=n bit=b CMD_DRUM_STEP_WRITE mask=0x…"（P4-C
 *    Task 9：写的是该步的原始 16 lane 位图；mask/bit 是**写后回读**）。写入被拒
 *    （诊断冻结等）改回 "-> STEP drum step=k lane=n ERR <frozen|invalid|…> mask=0x…"，
 *    mask 是未变的真值（不按本地翻转结果伪造）。TRANSPORT 工具回显
 *    "-> TRANSPORT <CMD> <op> value=<写后回读值> status=\"<屏幕状态行>\""（P4-C Task 10：
 *    REC/fill/聚焦步字段/pattern 工具；失败回 "ERR <token> value=<写前真值>"，鼓轨上的
 *    span/slide 回 "ignored (not_synth)"；status 与页面同源）。
 *    SONG 页（P4-C Task 11）回显 "-> SONG card=B select" / "-> SONG entry=1 select
 *    scene=0 bars=4" / "-> SONG <STORE|LOAD|ERASE|JUMP|SONG|CHAIN|CANCEL|REC|SCENE|
 *    BARS-|…> [section=A] value=<写后回读> status=\"<屏幕状态行>\""（失败另带
 *    "ERR <not_found|blocked|frozen|…>"；空 scene 的 `JUMP C: EMPTY` / `CHAIN: EMPTY`
 *    与 NO STORAGE 都能按文本断言）。
 *  - SEQ：原始鼓步、micro/fill、参数锁、音符跨度与 pattern 工具、录音/鼓演奏诊断
 *    （P4-C，ADR-010）。LOCK SET 的锁满与非法参数分别回 "SEQ ERR full" 与
 *    "SEQ ERR invalid"。`SEQ REC [TOGGLE|CONFIG q t c|FILL HOLD 0|1|FILL NEXT|BUTTON down]`
 *    打印/驱动录音状态机（`SEQ REC` 读即消费一次性 take 结果；FILL HOLD 只写保持电平，
 *    FILL NEXT 是脉冲、只翻转下一小节待命），`SEQ HIT <lane>
 *    [velocity]` 与键盘鼓键同一路径（同一 adapter），`STATE SNAP` 行尾带 rec 投影。
 *    `SEQ SECTION [<A-D> STORE|LOAD|ERASE|REQUEST]`、`SEQ CHAIN [<A-D>…]`、
 *    `SEQ SONG [MODE song|loop|REC]` 与 `SEQ SREC [TOGGLE]` 是 arranger 诊断面
 *    （Task 8）：`SEQ SECTION`/`SEQ SONG`/`SEQ CHAIN` 无参打印 ready/live/dirty/
 *    index/bar/config/chain；STORE 播放中也允许（RAM 捕获 + dirty，flash 由 1s 任务
 *    门限写），LOAD/ERASE 只在停止态，REQUEST 下一小节生效（绝不用 project LOAD 做
 *    播放中切场景）；`SEQ SREC` 打印 srec 状态与最近一次结果（none/no_song/N）。
 *    错误令牌：blocked = 走带中/SONG 播放中/REC IS ON，frozen = 诊断冻结。
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
 *    STORE STATS 另带自动存档三项（autosave=0|1、autosave_hash、autosave_writes）；
 *    STORE AUTO ON|OFF 是自动存档的诊断开关（默认 ON，测试脚本用：其它场景关掉它，
 *    免得空闲时的一次 flash 擦写干扰断言）。
 *    STORE BANK LIST | PUT n | GET n | LIVE | ERASE n 是 FM6 银行 27 槽
 *    （fm6_bank.c）的诊断入口：PUT = 选中轨的 live patch（fm6_patch 经 fm6_pack）→
 *    槽 n，GET/LIVE 打印 128B packed 记录的 hex；行格式 "B<k> USED|EMPTY" 末尾
 *    "BANK COUNT used=n total=27"；PTCH = 8 + n（< 8 是工厂槽）。播放中 PUT/ERASE 回
 *    "STORE ERR playing"；空槽 GET 回 empty；选中鼓轨时 PUT/LIVE 回 "STORE ERR drum"。
 *    STORE TEST 是设备侧 torn-write 套件（ADR-009 §3；**破坏性**：占用 OBJ_SETTINGS，
 *    快照/恢复该对象的**两个完整 4KB 副本扇区**（提交头 + payload + side record + 擦除态，
 *    逐字节校验；P4-C Task 7 起 settings 有真实写入者，payload-only 的豁免终止）：
 *    六场景全过回 "STORE TEST PASS 6/6"，否则回 "STORE TEST FAIL <name>"。需停走带。
 *    STORE SONG [SET c l s:b…|SAVE|LOAD|INFO] 是 PER3 歌曲顺序的诊断面（Task 7）：
 *    与 Song 页/状态层共用 sloop_store_song_*；SET 只改内存并置 dirty，落盘由 1s 任务的
 *    停止/安静/idle 门限或 SAVE（arrangement_save 同义）完成，INFO 报 flash 对象的
 *    kind/len/side/count（设置对象损坏时 SAVE 回 "STORE ERR invalid" 且不覆盖）。
 *    STORE PROJ FIELDS n 是旧格式/工程字段诊断（spec §6.3）：st_load + proj_import 后
 *    打印固定顺序的 key=value 一行（fmt/sel/bpm/swing/dust/drdly + 4 轨 engine/preset/
 *    P_LEVEL/P_ATK/P_E0/P_E1 + 前 8 步 n:note），**不动引擎/RAM 槽**；空槽回 empty、
 *    对象存在但任何格式都不匹配（截断/损坏）回 invalid。
 *    STORE RAW BEGIN <len≤3840> | <hex> | END <project> <slot> | ABORT 是分块上传通道
 *    （旧格式 fixture 的唯一入设备通道；一行 ≤240 hex = 120B，受 USJ 驱动 RX 环 256B
 *    约束）：END 以 st_save 原样写入请求的那一个对象（CRC/提交头由 storage.c 生成）。
 *    暂存与其它存储操作共用 ⇒ **任何其它 STORE 子命令入口都会 abort 未完成的上传**
 *    （R27；穿插命令后 END 回 invalid，不会把别人的字节写进 flash）。
 *    偏移是上游 flash-map 逻辑偏移，不是芯片绝对地址。
 *  - SMP：采样槽 USR1..USR4（P4-D Task 7，spec §4.5）。SMP INFO 打印槽状态
 *    （"SMP k USED name=… zones=… kb=…" / "SMP k EMPTY"）；BEGIN/DATA/HDR/END 是
 *    scripts/sample_upload.py 的上传对端（DATA 行 ≤120B、槽内绝对偏移 ≥512、逐行连续，
 *    console 攒满 256B 才调 sloop_store_sample_write；END 校验 header/zone/CRC 后提交）；
 *    ERASE 走状态层 CMD_SAMPLE_ERASE（与 SAMPLE 页同一路径）。错误令牌
 *    "SMP ERR <no_storage|playing|frozen|size|header|crc|zones|flash|state>"。
 *  - DSYN：SYN1..SYN4 鼓音色（24 虚拟字段）。INFO 打印当前选择（kit/lane/name/crush/src/
 *    dirty）+ 24 个字段值；GET <kit> <lane> 打印该 lane 的 22B 原始字节；SET/KIT/STORE/
 *    AUDITION 走状态层命令（CMD_DSYN_*，与 DSYN 页同一路径），SET 回显写后回读值。
 *    CRUSH <kit> <0..255> 写 kit 级的 bit-crush/S&H（CMD_DSYN_CRUSH，与 KIT 页同一路径）。
 *    `UI DSYN KIT/LANE/PAGE` 与 DSYN 页的 chip 共用同一状态（kit/lane 进状态层，page
 *    是页面静态量）；`TAP` 在该页回 `-> DSYN kit=.. lane=.. page=.. select` /
 *    `-> DSYN field=<name> value=<写后回读> status="…"` / `-> DSYN STORE status="…"` /
 *    `-> DSYN AUDITION lane=n` / `-> DSYN KIT src=n`（P4-D Task 9）。
 *  - FM6P：选中轨（或显式 track）的 155B live patch。INFO 打印 slot/engine_ok/名字/ALG
 *    与 P_E0 宏；GET 打印 155B hex；SET 走 CMD_FM6_SET（非 FM6 轨 → locked）；ALG
 *    <track> <0..32>：0 = PAT（只清 P_E0，patch ALG 不动），1..32 = patch ALG = value-1
 *    并在同一次引擎锁内清 P_E0（写完立即听得到）。`UI FM6P OP/PAGE` 是 FM6P 页的
 *    两个页内选择器（算子 OP1..OP6 / 字段页 EG..LFO）；`TAP` 在该页回
 *    `-> FM6P op=.. page=.. select` / `-> FM6P <field> value=<写后回读> status="…"` /
 *    `-> FM6P BANK slot=k select` / `-> FM6P SAVE status="…"`（P4-D Task 10）。
 *    STATE SNAP 行尾追加 dsyn_kit/dsyn_lane/dsyn_dirty 与 fm6p_slot/fm6p_engine_ok。
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
#include "sloop_seq.h"
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

static bool seq_int(const char *text, int32_t min, int32_t max, int32_t *value)
{
    char *end = NULL;
    long parsed = strtol(text, &end, 0);
    if (end == text || *end != '\0' || parsed < min || parsed > max) return false;
    *value = (int32_t)parsed;
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
    CONSOLE_P_E0 = 63, /* = SLOOP_CORE_P_COUNT - 8（TP 表之后 8 个引擎槽的起点） */
    CONSOLE_G_BPM = 0, CONSOLE_G_SWING = 1,
};

/* STATE 排障入口：打印 AppState（UI 渲染源），与 ENG SNAP（引擎真值）互补。
 * 选轨/mute/solo 已是引擎真值，两处输出同源（sync 无状态层例外）；参数投影
 * p[]/g[] 抽样打印（P_LEVEL/P_ATK/P_DEC/P_SUS/P_REL/P_SLEN/P_MUTE + G_BPM/G_SWING）。
 * 既有行只在行尾追加 seq_status；`STATE SNAP FULL` 才另起 step/dstep 详细行，默认保持
 * 既有响应体量。 */
static int state_command(int argc, char **argv)
{
    if ((argc == 2 && strcasecmp(argv[1], "snap") == 0) ||
        (argc == 3 && strcasecmp(argv[1], "snap") == 0 && strcasecmp(argv[2], "full") == 0)) {
        const bool full = argc == 3;
        app_state_t st;
        state_get_copy(&st);
        static const char *const REC_NAME[] = {"off", "armed", "recording", "count_in", "free_take"};
        const char *rec = st.rec.state <= SEQ_REC_FREE_TAKE ? REC_NAME[st.rec.state] : "?";
        printf("STATE SNAP page=%u focus_track=%u focus_step=%u layer_held=0x%02X selected_track=%u last_note=%u bpm=%u playing=%u window_bank=%u window_start=%u window_len=%u seq_status=%u rec=%s rec_track=%u rec_quant=%u rec_tempo=%u rec_count=%u rec_bars=%u dsyn_kit=%u dsyn_lane=%u dsyn_dirty=%u fm6p_slot=%u fm6p_engine_ok=%u\n",
               (unsigned)st.page, (unsigned)st.focus_track, (unsigned)st.focus_step,
               (unsigned)st.layer_held, (unsigned)st.core.selected_track, (unsigned)st.last_note,
               (unsigned)st.core.bpm, (unsigned)st.core.playing, (unsigned)st.window.bank,
               (unsigned)st.window.window_start, (unsigned)st.window.window_len,
               (unsigned)st.seq_status, rec, (unsigned)st.rec.track,
               (unsigned)st.rec.quant_off, (unsigned)st.rec.tempo_mode,
               (unsigned)st.rec.count_mode, (unsigned)st.rec.take_bars,
               (unsigned)st.dsyn.kit, (unsigned)st.dsyn.lane, (unsigned)st.dsyn.dirty,
               (unsigned)st.fm6p.slot, (unsigned)st.fm6p.engine_ok);
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
        if (full) {
        for (uint32_t k = 0; k < 3; k++) {
            for (uint32_t c = 0; c < APP_STATE_STEPS; c++) {
                printf("STATE SNAP step k=%u c=%u i=%u n=%u note=%u,%u,%u,%u vel=%u time=%u flags=%u lvl=%u rat=%u micro=%d fill=%u\n",
                       (unsigned)k, (unsigned)c, (unsigned)(st.window.window_start + c),
                       (unsigned)st.core.step[k][c].n, (unsigned)st.core.step[k][c].note[0],
                       (unsigned)st.core.step[k][c].note[1], (unsigned)st.core.step[k][c].note[2],
                       (unsigned)st.core.step[k][c].note[3], (unsigned)st.core.step[k][c].vel,
                       (unsigned)st.core.step[k][c].time, (unsigned)st.core.step[k][c].flags,
                       (unsigned)st.core.step[k][c].lvl, (unsigned)st.core.step[k][c].rat,
                       (int)st.core.step[k][c].micro, (unsigned)st.core.step[k][c].fill);
            }
        }
        for (uint32_t c = 0; c < APP_STATE_STEPS; c++) {
            printf("STATE SNAP dstep c=%u i=%u on=%u lvl=%u rat=%u\n", (unsigned)c,
                   (unsigned)(st.window.window_start + c), (unsigned)st.core.dstep[c].on,
                   (unsigned)st.core.dstep[c].lvl, (unsigned)st.core.dstep[c].rat);
        }
        }
        return 0;
    }
    printf("用法: STATE SNAP [FULL]（AppState 页面/焦点/层键、步窗、seq_status、参数投影；FULL 追加详细步）\n");
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

/* UI 切页（注入用）：UI <play|tracks|step|transport|global|fx|analog|fm6|sample|dsyn|fm6p|projects|presets|song>；无参打印当前页。
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
    "projects", "presets", "song",
    /* SYNTH 组（P4-D Task 8：SAMPLE 随组表移入；DSYN 是 Task 9 的实体页，
     * FM6P 由 Task 10 换成实体页） */
    "dsyn", "fm6p",
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
    printf("UI MEM pool_used=%u pool_free=%u pool_total=%u registered=%u built=%u failed=%u sum_est=%u reserve=%d group=%s\n",
           (unsigned)ps.pool_used, (unsigned)ps.pool_free, (unsigned)ps.pool_total,
           (unsigned)ps.registered, (unsigned)ps.built, (unsigned)ps.failed, (unsigned)sum_est,
           (int)(ps.pool_total - sum_est),
           ps.current_group < ps.group_count ? ps.groups[ps.current_group].name : "-");
    /* 分组水位（ADR-005 §5）：sum_est = 组内已注册成员 est（静态预算），
     * sum_delta = 组内已建成员的池增量；离开组后 built/sum_delta 回 0。
     * build_ms = 最近一次跨组进入/组内首建的耗时（≤100ms 预算口径）。 */
    for (uint8_t g = 0; g < ps.group_count; g++) {
        const sloop_ui_group_stat_t *gr = &ps.groups[g];
        printf("UI MEM GROUP %s built=%u sum_est=%u sum_delta=%u build_ms=%u\n", gr->name,
               (unsigned)gr->built, (unsigned)gr->sum_est, (unsigned)gr->sum_delta,
               (unsigned)gr->build_ms);
    }
    for (uint8_t i = 0; i < ps.registered; i++) {
        const sloop_ui_page_stat_t *p = &ps.pages[i];
        printf("UI MEM page id=%u title=%s state=%s est=%u delta=%d build_ms=%u\n", (unsigned)p->id,
               p->title, states[p->state < 3 ? p->state : 0], (unsigned)p->est_bytes,
               (int)p->delta_bytes, (unsigned)p->build_ms);
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
        if (page < APP_PAGE_GLOBAL || page > APP_PAGE_SAMPLE) {
            /* 参数页是 app_page_t 的连续段 GLOBAL..SAMPLE（page_layout.c 有静态断言）；
             * FILE 组两页（PROJECTS/PRESETS）不是参数页，旋钮注入不适用。 */
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
    if ((argc == 2 || argc == 3) && strcasecmp(argv[1], "lane") == 0) {
        /* UI LANE [<n>]：STEP 页选中的鼓 lane（UI 局部态，0..15；无参打印当前值）。
         * 与键盘 ↑/↓（sloop_ui_step_focus_vertical）和触摸点 dot 共用同一状态。 */
        if (argc == 2) {
            printf("UI LANE lane=%u\n", (unsigned)sloop_ui_step_lane_get());
            return 0;
        }
        uint32_t lane = 0;
        if (!eng_uint(argv[2], 15, &lane)) {
            /* 越界/非数字是**取值错误**（与用法不同）：脚本按 `UI ERR lane` 断言，
             * 不改选中 lane。 */
            printf("UI ERR lane\n");
            return 1;
        }
        sloop_ui_step_lane_set((uint8_t)lane);
        printf("UI LANE lane=%u\n", (unsigned)sloop_ui_step_lane_get());
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcasecmp(argv[1], "tool") == 0) {
        /* UI TOOL [<0..7>]：TRANSPORT 页的键盘工具焦点字段（UI 局部态；无参打印当前值）。
         * 与 ←/→（sloop_ui_transport_focus_move）和点字段 chip 共用同一状态。 */
        static const char *const k_tool_names[SLOOP_UI_TRANSPORT_FIELDS] = {
            "lvl", "rat", "mic", "span", "slide", "rot", "len", "trans"};
        if (argc == 2) {
            const uint8_t f = sloop_ui_transport_tool_get();
            printf("UI TOOL field=%u name=%s\n", (unsigned)f,
                   k_tool_names[f < SLOOP_UI_TRANSPORT_FIELDS ? f : 0]);
            return 0;
        }
        uint32_t field = 0;
        if (!eng_uint(argv[2], SLOOP_UI_TRANSPORT_FIELDS - 1u, &field) ||
            !sloop_ui_transport_tool_set((uint8_t)field)) {
            /* 越界/非数字是取值错误（脚本按 `UI ERR tool` 断言，不改焦点） */
            printf("UI ERR tool\n");
            return 1;
        }
        printf("UI TOOL field=%u name=%s\n", (unsigned)field,
               k_tool_names[field < SLOOP_UI_TRANSPORT_FIELDS ? field : 0]);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "dsyn") == 0) {
        /* UI DSYN KIT <0..3> | LANE <0..15> | PAGE <0..3>：DSYN 页的三个选择器
         * （P4-D Task 9）。kit/lane 走状态层的 UI 局部态（与页面 chip 同一入口，
         * 回显即可读、`STATE SNAP` 的 dsyn_kit/dsyn_lane 是同一真值）；PAGE 只是
         * 页面静态量（SND1/SND2/SND3/KIT），30ms UI 定时器下一拍重配旋钮。
         * 裸 `UI dsyn` 仍是**切页**（页名表优先，见本函数末尾的 argc==2 循环）。 */
        app_state_t st;
        state_get_copy(&st);
        uint8_t kit = st.dsyn.kit, lane = st.dsyn.lane;
        uint8_t page = sloop_ui_dsyn_page_get();
        if (argc == 3) {
            printf("用法: UI DSYN <KIT 0..3 | LANE 0..15 | PAGE 0..3>"
                   "（kit/lane 与页面 chip 共用状态层；裸 `UI dsyn` 是切页）\n");
            return 1;
        }
        if (argc == 4) {
            uint32_t v = 0;
            if (strcasecmp(argv[2], "kit") == 0) {
                if (!eng_uint(argv[3], SLOOP_DSYN_KITS - 1u, &v)) {
                    printf("UI ERR dsyn\n");
                    return 1;
                }
                state_set_dsyn_select((uint8_t)v, lane);
            } else if (strcasecmp(argv[2], "lane") == 0) {
                if (!eng_uint(argv[3], SLOOP_DSYN_LANES - 1u, &v)) {
                    printf("UI ERR dsyn\n");
                    return 1;
                }
                state_set_dsyn_select(kit, (uint8_t)v);
            } else if (strcasecmp(argv[2], "page") == 0) {
                if (!eng_uint(argv[3], SLOOP_UI_DSYN_PAGES - 1u, &v)) {
                    printf("UI ERR dsyn\n");
                    return 1;
                }
                sloop_ui_dsyn_page_set((uint8_t)v);
            } else {
                printf("用法: UI DSYN [KIT <0..3> | LANE <0..15> | PAGE <0..3>]\n");
                return 1;
            }
            state_get_copy(&st);
            kit = st.dsyn.kit;
            lane = st.dsyn.lane;
            page = sloop_ui_dsyn_page_get();
        }
        printf("UI DSYN kit=%u lane=%u page=%u\n", (unsigned)kit, (unsigned)lane, (unsigned)page);
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "fm6p") == 0) {
        /* UI FM6P OP <0..5> | PAGE <0..5>：FM6P 页的两个选择器（P4-D Task 10）。
         * 都是页内 UI 局部态（不写引擎、不进状态层），30ms UI 定时器下一拍按新
         * 算子/字段页重配 8 个旋钮；算子只影响 EG/LEVEL/PITCH 三页（PEG/VOICE/LFO
         * 是全局块）。裸 `UI fm6p` 仍是**切页**（页名表优先，见本函数末尾的 argc==2
         * 循环）。 */
        uint8_t op = sloop_ui_fm6p_op_get();
        uint8_t page = sloop_ui_fm6p_page_get();
        if (argc == 3) {
            printf("用法: UI FM6P <OP 0..5（OP1..OP6） | PAGE 0..5（EG/LEVEL/PITCH/PEG/"
                   "VOICE/LFO）>（裸 `UI fm6p` 是切页）\n");
            return 1;
        }
        uint32_t v = 0;
        if (strcasecmp(argv[2], "op") == 0) {
            if (!eng_uint(argv[3], SLOOP_UI_FM6P_OPS - 1u, &v)) {
                printf("UI ERR fm6p\n");
                return 1;
            }
            sloop_ui_fm6p_op_set((uint8_t)v);
        } else if (strcasecmp(argv[2], "page") == 0) {
            if (!eng_uint(argv[3], SLOOP_UI_FM6P_PAGES - 1u, &v)) {
                printf("UI ERR fm6p\n");
                return 1;
            }
            sloop_ui_fm6p_page_set((uint8_t)v);
        } else {
            printf("用法: UI FM6P [OP <0..5> | PAGE <0..5>]\n");
            return 1;
        }
        printf("UI FM6P op=%u page=%u\n", (unsigned)sloop_ui_fm6p_op_get(),
               (unsigned)sloop_ui_fm6p_page_get());
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "forcefail") == 0) {
        uint8_t page = 0;
        if (!ui_parse_page(argv[2], &page)) {
            printf("UI ERR page (want=play|tracks|step|transport|global|fx|analog|fm6|sample|"
                   "dsyn|fm6p|projects|presets|song|0..%u)\n",
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
    /* UI TAB <group> / UI SUB <index>：与物理组签/子签点击同一动作函数（R32）。
     * TAP 对 y<72 一律 miss，这是唯一能机器覆盖手指导航路径的入口。 */
    if (argc == 3 && strcasecmp(argv[1], "tab") == 0) {
        if (sloop_ui_current_page() >= APP_STATE_PAGES) {
            printf("UI ERR not_started\n");
            return 1;
        }
        uint8_t found = 0xFF;
        for (uint8_t g = 0; g < sloop_ui_group_count(); g++) {
            const char *name = sloop_ui_group_name(g);
            if (name != NULL && strcasecmp(argv[2], name) == 0) {
                found = g;
                break;
            }
        }
        if (found == 0xFF) {
            printf("UI ERR group %s\n", argv[2]);
            return 1;
        }
        esp_err_t ret = sloop_ui_group_select(found);
        uint8_t page = sloop_ui_current_page();
        if (ret == ESP_OK) {
            printf("UI TAB %s page=%s (%u)\n", sloop_ui_group_name(found),
                   page < APP_STATE_PAGES ? k_page_names[page] : "?", (unsigned)page);
            return 0;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            printf("UI ERR lock_timeout\n");
        } else if (ret == ESP_ERR_NO_MEM) {
            printf("UI ERR page_build %u\n", (unsigned)page);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            printf("UI ERR not_started\n");
        } else {
            /* 组存在但没有已注册页：组签也不显示，不可点击（空组） */
            printf("UI ERR group %s\n", sloop_ui_group_name(found));
        }
        return 1;
    }
    if (argc == 3 && strcasecmp(argv[1], "sub") == 0) {
        if (sloop_ui_current_page() >= APP_STATE_PAGES) {
            printf("UI ERR not_started\n");
            return 1;
        }
        char *end = NULL;
        long idx = strtol(argv[2], &end, 0);
        /* 下行子签固定 5 个槽位（theme.h UI_GROUP_MAX=5，最大组 EDIT/SYNTH ≤4 页）：
         * 0..4 之外的 index 一律用法错误（与 sloop_ui_sub_select 的上界同口径）。 */
        if (end == argv[2] || *end != '\0' || idx < 0 || idx > 4) {
            printf("用法: UI SUB <index=0..4>（当前组的子签下标；组内最多 5 页）\n");
            return 1;
        }
        esp_err_t ret = sloop_ui_sub_select((uint8_t)idx);
        uint8_t page = sloop_ui_current_page();
        if (ret == ESP_OK) {
            printf("UI SUB %ld page=%s (%u)\n", idx,
                   page < APP_STATE_PAGES ? k_page_names[page] : "?", (unsigned)page);
            return 0;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            printf("UI ERR lock_timeout\n");
        } else if (ret == ESP_ERR_NO_MEM) {
            printf("UI ERR page_build %u\n", (unsigned)page);
        } else if (ret == ESP_ERR_INVALID_STATE) {
            printf("UI ERR not_started\n");
        } else {
            /* 下标越界或单页组（子行为空，没有可点的子签） */
            printf("UI ERR sub %ld\n", idx);
        }
        return 1;
    }
    printf("用法: UI <play|tracks|step|transport|global|fx|analog|fm6|sample|dsyn|fm6p|projects|presets|song> | UI MEM | UI WINDOW [<start> <len>] | "
           "UI LANE [<lane=0..15>] | UI DSYN [KIT <0..3> | LANE <0..15> | PAGE <0..3>] | "
           "UI FM6P [OP <0..5> | PAGE <0..5>] | "
           "UI TOOL [<field=0..7>] | "
           "UI KNOB <slot> <press|drag> <dx> <dy> | UI TAB <group> | UI SUB <index=0..4> | "
           "UI FORCEFAIL <page> [on|off] | UI QUIESCE <on|off> | UI LOCKHOLD <ms>（无参打印当前页）\n"
           "WINDOW: start 向下对齐 16（bank = start/16，0..48），len 1..16；无参打印当前窗口\n"
           "LANE: STEP 页选中的鼓 lane（0..15，UI 局部态）；无参打印当前值\n"
           "TOOL: TRANSPORT 页的键盘工具焦点（0 lvl / 1 rat / 2 mic / 3 span / 4 slide / 5 rot / "
           "6 len / 7 trans）；无参打印当前字段\n"
           "TAB/SUB: 与物理组签/子签点击同一路径（TAP 不路由页签；TAB 进上次子页，SUB 切当前组子签）\n"
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

/* app_seq_status_t → console 令牌（TAP 的鼓格与 TRANSPORT 工具回显共用）。 */
static const char *seq_status_token(uint8_t status)
{
    static const char *const tok[] = {"idle", "ok", "not_found", "invalid",
                                      "full", "blocked", "frozen", "error"};
    return status < sizeof(tok) / sizeof(tok[0]) ? tok[status] : "error";
}

/* TRANSPORT 工具动作 → 回显里的命令名（与 page_transport.c 的 tr_action 一一对应）。 */
static const char *transport_act_cmd(uint8_t act)
{
    switch (act) {
    case SLOOP_TR_REC: return "CMD_REC_TOGGLE";
    case SLOOP_TR_FILL_HOLD: return "CMD_FILL_CONTROL hold";
    case SLOOP_TR_FILL_NEXT: return "CMD_FILL_CONTROL next";
    case SLOOP_TR_LVL_DN:
    case SLOOP_TR_LVL_UP:
    case SLOOP_TR_RAT_DN:
    case SLOOP_TR_RAT_UP:
    case SLOOP_TR_SPAN_DN:
    case SLOOP_TR_SPAN_UP:
    case SLOOP_TR_SLIDE: return "CMD_STEP_EDIT";
    case SLOOP_TR_MIC_DN:
    case SLOOP_TR_MIC_UP: return "CMD_SET_MICRO";
    case SLOOP_TR_ROT_DN:
    case SLOOP_TR_ROT_UP:
    case SLOOP_TR_LEN_X2:
    case SLOOP_TR_LEN_D2:
    case SLOOP_TR_TRANS_DN:
    case SLOOP_TR_TRANS_UP:
    case SLOOP_TR_CLEAR:
    case SLOOP_TR_UNDO:
    case SLOOP_TR_REDO: return "CMD_PATTERN";
    default: return "?";
    }
}

static const char *transport_act_op(uint8_t act)
{
    switch (act) {
    case SLOOP_TR_LVL_DN:
    case SLOOP_TR_LVL_UP: return "lvl";
    case SLOOP_TR_RAT_DN:
    case SLOOP_TR_RAT_UP: return "rat";
    case SLOOP_TR_MIC_DN:
    case SLOOP_TR_MIC_UP: return "micro";
    case SLOOP_TR_SPAN_DN:
    case SLOOP_TR_SPAN_UP: return "span";
    case SLOOP_TR_SLIDE: return "slide";
    case SLOOP_TR_ROT_DN:
    case SLOOP_TR_ROT_UP: return "rotate";
    case SLOOP_TR_LEN_X2:
    case SLOOP_TR_LEN_D2: return "length";
    case SLOOP_TR_TRANS_DN:
    case SLOOP_TR_TRANS_UP: return "transpose";
    case SLOOP_TR_CLEAR: return "clear";
    case SLOOP_TR_UNDO: return "undo";
    case SLOOP_TR_REDO: return "redo";
    default: return NULL;
    }
}

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
    case SLOOP_TAP_STEP_DRUM: {
        /* 鼓格 lane 位：a=lane b=步号 d=写后回读的 bit v=写后回读的 16 lane 位图
         * err=0 成功；否则是 app_seq_status_t（冻结等——echo 不伪造本地翻转结果）。 */
        if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> STEP drum step=%u lane=%u ERR %s mask=0x%04X\n",
                   x, y, page, (unsigned)r.b, (unsigned)r.a, seq_status_token(r.err),
                   (unsigned)(uint16_t)r.v);
            break;
        }
        printf("TAP %ld,%ld page=%u -> STEP drum step=%u lane=%u bit=%u "
               "CMD_DRUM_STEP_WRITE mask=0x%04X\n",
               x, y, page, (unsigned)r.b, (unsigned)r.a, (unsigned)(r.d & 1),
               (unsigned)(uint16_t)r.v);
        break;
    }
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
    case SLOOP_TAP_TRANSPORT_TOOL: {
        /* TRANSPORT 工具：a=sloop_transport_act_t b=1 表示 LVL/RAT 落在鼓轨
         * （CMD_DRUM_STEP_WRITE）v=写后回读值 err=0/app_seq_status_t/8（不适用，
         * 未下发）。令牌与 STEP 鼓格共用 seq_status_token；`status="…"` 是**屏幕状态
         * 行的同一文本**（sloop_ui_transport_status_text，与页面同源，不另写词表）。 */
        const char *cmd = transport_act_cmd((uint8_t)r.a);
        const char *op = transport_act_op((uint8_t)r.a);
        if (r.b != 0u) cmd = "CMD_DRUM_STEP_WRITE";
        char what[40];
        if (op != NULL) snprintf(what, sizeof(what), "%s %s", cmd, op);
        else snprintf(what, sizeof(what), "%s", cmd);
        const char *status_txt = sloop_ui_transport_status_text();
        if (r.err == SLOOP_TR_ERR_NA) {
            printf("TAP %ld,%ld page=%u -> TRANSPORT %s ignored (not_synth) value=%d status=\"%s\"\n",
                   x, y, page, what, (int)r.v, status_txt != NULL ? status_txt : "");
        } else if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> TRANSPORT %s ERR %s value=%d status=\"%s\"\n", x, y,
                   page, what, seq_status_token(r.err), (int)r.v,
                   status_txt != NULL ? status_txt : "");
        } else {
            printf("TAP %ld,%ld page=%u -> TRANSPORT %s value=%d status=\"%s\"\n", x, y, page,
                   what, (int)r.v, status_txt != NULL ? status_txt : "");
        }
        break;
    }
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
    case SLOOP_TAP_FILE_SLOT:
        /* FILE 页（PROJECTS/PRESETS）：槽位选中。状态行文本/引擎结果另由 STORE/STATE
         * 断言（槽列表与 hash 才是存储真值）。 */
        printf("TAP %ld,%ld page=%u -> FILE %s slot=%u select\n", x, y, page,
               page < APP_STATE_PAGES ? k_page_names[page] : "?", (unsigned)r.b);
        break;
    case SLOOP_TAP_FILE_ACTION: {
        /* 动作按钮：把页面状态行文本一起回显（与屏幕同源：sloop_ui_file_status_text）
         * ——播放中拒绝（STOP BEFORE SAVE）、鼓轨（DRUM TRACK: NO SOUND）、NO STORAGE
         * 都能被脚本按文本断言，而不是只看像素。 */
        static const char *const acts[] = {"save", "load", "erase"};
        const char *text = sloop_ui_file_status_text();
        printf("TAP %ld,%ld page=%u -> FILE %s slot=%u action=%s status=\"%s\"\n", x, y, page,
               page < APP_STATE_PAGES ? k_page_names[page] : "?", (unsigned)r.b,
               (unsigned)r.v < 3 ? acts[r.v] : "?", text != NULL ? text : "?");
        break;
    }
    case SLOOP_TAP_BANK_SLOT:
        /* FM6 页 bank 目标槽 ±：只改目标槽（不写存储/引擎），回显落地槽号 */
        printf("TAP %ld,%ld page=%u -> BANK slot=%u delta=%+d\n", x, y, page, (unsigned)r.b,
               (int)r.d);
        break;
    case SLOOP_TAP_BANK_STORE:
    case SLOOP_TAP_BANK_ERASE: {
        /* BANK SAVE/ERASE：与 FILE 页动作按钮同一回显形态（动作 + 状态行文本同源，
         * 播放中 STOP BEFORE SAVE / 未就绪 NO STORAGE / 鼓轨都能按文本断言）。 */
        const char *text = sloop_ui_bank_status_text();
        printf("TAP %ld,%ld page=%u -> BANK slot=%u action=%s status=\"%s\"\n", x, y, page,
               (unsigned)r.b, r.kind == SLOOP_TAP_BANK_STORE ? "save" : "erase",
               text != NULL ? text : "?");
        break;
    }
    case SLOOP_TAP_SAMPLE_SLOT: {
        /* SAMPLE 页槽管理行：选中槽 = 把 SET 宏写到 base + 槽号。v=0 = 页面拒绝
         * （锁定页/描述符不可用，未下发命令），回显走 `status="…"` 形式（BANK 行同形）。 */
        const char *text = sloop_ui_sample_status_text();
        if (r.v == 0) {
            printf("TAP %ld,%ld page=%u -> SAMPLE slot=- select status=\"%s\"\n", x, y, page,
                   text != NULL ? text : "?");
        } else {
            printf("TAP %ld,%ld page=%u -> SAMPLE slot=%u select\n", x, y, page, (unsigned)r.b);
        }
        break;
    }
    case SLOOP_TAP_SAMPLE_ACTION: {
        /* ERASE chip：与 BANK 行同一回显形态（动作 + 状态行文本同源：
         * sloop_ui_sample_status_text）。`NOT USR`（当前 SET 不在 USR1..USR4）、
         * `STOP BEFORE ERASE`（走带中）、`NO STORAGE`（存储未就绪）都能按文本断言。 */
        const char *text = sloop_ui_sample_status_text();
        /* b = 当前 SET 指向的槽；`NOT USR` 时没有槽号（页面未下发命令）→ 回显 `slot=-` */
        if (r.b < SLOOP_STORE_SAMPLES) {
            printf("TAP %ld,%ld page=%u -> SAMPLE slot=%u action=erase status=\"%s\"\n", x, y,
                   page, (unsigned)r.b, text != NULL ? text : "?");
        } else {
            printf("TAP %ld,%ld page=%u -> SAMPLE slot=- action=erase status=\"%s\"\n", x, y,
                   page, text != NULL ? text : "?");
        }
        break;
    }
    case SLOOP_TAP_DSYN_SEL:
        /* DSYN 页选择行：kit/lane/字段页（UI 局部态，不写引擎数据）。`d` = 字段页 0..3
         * （SND1/SND2/SND3/KIT）；旋钮命中只设焦点，也走这一形态。 */
        printf("TAP %ld,%ld page=%u -> DSYN kit=%u lane=%u page=%u select\n", x, y, page,
               (unsigned)r.a, (unsigned)r.b, (unsigned)(uint8_t)r.d);
        break;
    case SLOOP_TAP_DSYN_FIELD: {
        /* DSYN 单字段写（± chip；TAP 不能拖拽）：字段名来自页面模块（0x80/0x81 是 KIT
         * 页的 CRUSH / KIT SRC 伪字段），value 是写后回读，status 与屏幕状态行同源。
         * err=2 = 命令失败（synth_status != OK）。 */
        const char *name = sloop_ui_dsyn_field_name(r.a);
        const char *text = sloop_ui_dsyn_status_text();
        if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> DSYN field=%s value=%d ERR %s status=\"%s\"\n", x, y,
                   page, name != NULL ? name : "?", (int)r.v, "error",
                   text != NULL ? text : "");
        } else {
            printf("TAP %ld,%ld page=%u -> DSYN field=%s value=%d status=\"%s\"\n", x, y, page,
                   name != NULL ? name : "?", (int)r.v, text != NULL ? text : "");
        }
        break;
    }
    case SLOOP_TAP_DSYN_STORE: {
        /* STORE = CMD_DSYN_STORE：结果文本与屏幕状态行同源（STORE OK / STOP BEFORE STORE /
         * NO STORAGE / STORE ERROR / FROZEN） */
        const char *text = sloop_ui_dsyn_status_text();
        printf("TAP %ld,%ld page=%u -> DSYN STORE status=\"%s\"\n", x, y, page,
               text != NULL ? text : "?");
        break;
    }
    case SLOOP_TAP_DSYN_AUDITION:
        printf("TAP %ld,%ld page=%u -> DSYN AUDITION lane=%u\n", x, y, page, (unsigned)r.b);
        break;
    case SLOOP_TAP_DSYN_KIT: {
        /* KIT 页 KIT SRC 的 ±：一次工厂套用（CMD_DSYN_LOAD_KIT，覆盖该 kit 的编辑） */
        const char *text = sloop_ui_dsyn_status_text();
        if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> DSYN KIT src=%d ERR error status=\"%s\"\n", x, y, page,
                   (int)r.v, text != NULL ? text : "");
        } else {
            printf("TAP %ld,%ld page=%u -> DSYN KIT src=%d\n", x, y, page, (int)r.v);
        }
        break;
    }
    case SLOOP_TAP_FM6P_SEL:
        /* FM6P 页选择行：算子/字段页（页内 UI 局部态，不写引擎数据）；旋钮命中只设焦点，
         * 也走这一形态。锁定页（选中轨不是 FM6）时 chip 拒绝、回显原选择。 */
        printf("TAP %ld,%ld page=%u -> FM6P op=%u page=%u select\n", x, y, page,
               (unsigned)r.a, (unsigned)r.b);
        break;
    case SLOOP_TAP_FM6P_FIELD: {
        /* 单字段写（底部 ±；TAP 不能拖拽）：字段名由页面模块从 core 描述符取
         * （`ALG`/`R1`/`PL1`…），value 是写后回读，status 与屏幕状态行同源。
         * err=1 = 锁定页拒绝（未下发命令，v 为写前真值）；err=2 = 命令失败。 */
        const char *name = sloop_ui_fm6p_field_name((uint8_t)r.a, (uint8_t)r.b);
        const char *text = sloop_ui_fm6p_status_text();
        if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> FM6P %s value=%d ERR %s status=\"%s\"\n", x, y, page,
                   name != NULL ? name : "?", (int)r.v, r.err == 1u ? "locked" : "error",
                   text != NULL ? text : "");
        } else {
            printf("TAP %ld,%ld page=%u -> FM6P %s value=%d status=\"%s\"\n", x, y, page,
                   name != NULL ? name : "?", (int)r.v, text != NULL ? text : "");
        }
        break;
    }
    case SLOOP_TAP_FM6P_BANK:
        /* BANK 行槽号 ±：只改页内目标槽（不写存储/引擎），回显落地槽号 */
        printf("TAP %ld,%ld page=%u -> FM6P BANK slot=%u select\n", x, y, page, (unsigned)r.a);
        break;
    case SLOOP_TAP_FM6P_SAVE: {
        /* SAVE = CMD_BANK_PUT：结果文本与屏幕状态行同源（SAVE OK / STOP BEFORE SAVE /
         * NO STORAGE / SAVE ERROR / NOT FM6）。v=1 = 锁定页拒绝（未下发命令）。 */
        const char *text = sloop_ui_fm6p_status_text();
        if (r.v != 0u) {
            printf("TAP %ld,%ld page=%u -> FM6P SAVE ERR locked status=\"%s\"\n", x, y, page,
                   text != NULL ? text : "");
        } else {
            printf("TAP %ld,%ld page=%u -> FM6P SAVE status=\"%s\"\n", x, y, page,
                   text != NULL ? text : "");
        }
        break;
    }
    case SLOOP_TAP_SONG_SECTION:
        /* SONG 页 A–D 卡：只改 UI 局部选中（不写引擎）；链采集中同一 tap 也会把该
         * section 追加进临时列表（页面状态行给出 `CHAIN: …` 预览）。 */
        printf("TAP %ld,%ld page=%u -> SONG card=%c select\n", x, y, page,
               (unsigned)r.b < 4u ? (char)('A' + r.b) : '?');
        break;
    case SLOOP_TAP_SONG_ENTRY: {
        /* 歌曲条目选中：count 内的槽回显其 scene/bars（与 STORE SONG 同源的真值）；
         * count 之外的槽只回 entry 号（屏幕上是 `NN --`）。 */
        sloop_song_snapshot_t s;
        sloop_seq_song_read(&s);
        if (r.b < s.config.count) {
            printf("TAP %ld,%ld page=%u -> SONG entry=%u select scene=%u bars=%u\n", x, y, page,
                   (unsigned)r.b, (unsigned)s.config.entry[r.b].scene,
                   (unsigned)s.config.entry[r.b].bars);
        } else {
            printf("TAP %ld,%ld page=%u -> SONG entry=%u select\n", x, y, page, (unsigned)r.b);
        }
        break;
    }
    case SLOOP_TAP_SONG_ACTION: {
        /* SONG 页动作 chip：动作名 + 落点 section + 写后回读值 + 状态行文本（与屏幕
         * 同源 `sloop_ui_song_status_text`）。失败时另给 ERR 令牌（app_seq_status_t，
         * 与 STEP 鼓格/TRANSPORT 工具共用词表），脚本按文本断言空 scene 的可见拒绝。 */
        static const char *const names[SLOOP_SONG_ACT_COUNT] = {
            "STORE", "LOAD", "ERASE", "JUMP", "SONG", "CHAIN", "CANCEL", "REC",
            "SCENE", "BARS-", "BARS+", "COUNT-", "COUNT+", "LOOP",
        };
        const char *name = r.b < SLOOP_SONG_ACT_COUNT ? names[r.b] : "?";
        const char *text = sloop_ui_song_status_text();
        char where[16] = "";
        if (r.b <= SLOOP_SONG_JUMP) {
            snprintf(where, sizeof(where), " section=%c",
                     r.d >= 0 && r.d < 4 ? (char)('A' + r.d) : '?');
        }
        if (r.err != 0u) {
            printf("TAP %ld,%ld page=%u -> SONG %s%s ERR %s value=%d status=\"%s\"\n", x, y,
                   page, name, where, seq_status_token(r.err), (int)r.v,
                   text != NULL ? text : "");
        } else {
            printf("TAP %ld,%ld page=%u -> SONG %s%s value=%d status=\"%s\"\n", x, y, page, name,
                   where, (int)r.v, text != NULL ? text : "");
        }
        break;
    }
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
    printf("STORE STATS ready=%u base=0x%X span=0x%X map=0x%lX objects=%u faults=%u armed=%u "
           "autosave=%u autosave_hash=%08lX autosave_writes=%u last_xrun=%d last_save_ms=%u\n",
           st.ready ? 1u : 0u, (unsigned)sloop_hal_storage_base(),
           (unsigned)sloop_hal_storage_span(), (unsigned long)(uintptr_t)sloop_hal_storage_map(),
           (unsigned)st.objects_ok, (unsigned)st.faults_taken, (unsigned)st.faults_armed,
           st.autosave_on ? 1u : 0u, (unsigned long)st.autosave_hash,
           (unsigned)st.autosave_writes, (int)st.last_save_xrun, (unsigned)st.last_save_ms);
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
 * 错误令牌：no_storage（未就绪）/playing（走带中）/empty（空槽或空提交 len=0）/
 * invalid（槽号或对象名越界；或对象存在但 proj_import 不认识它的格式——截断/损坏，
 * 与 empty 分开）/save（flash 擦写或回读校验失败）/size（STORE RAW 的长度/块越界）。 */
static const char *store_err(esp_err_t err)
{
    if (!sloop_store_ready()) return "no_storage";
    switch (err) {
    case ESP_ERR_INVALID_STATE: return "playing";
    case ESP_ERR_NOT_FOUND: return "empty";
    case ESP_ERR_NOT_FINISHED: return "save";
    case ESP_ERR_INVALID_SIZE: return "size"; /* STORE RAW 的长度/块越界 */
    default: return "invalid";
    }
}

static int store_err_print(esp_err_t err)
{
    printf("STORE ERR %s\n", store_err(err));
    return 1;
}

/* `STORE PROJ FIELDS n`（spec §6.3）：stored 对象 → FUN6 视图的固定顺序 key=value 一行。
 * 只读 flash（st_load + proj_import），**不动引擎/RAM 槽**；脚本按
 * gen_project_fixtures.py 的 field_keys() 顺序逐项比对（顺序错也算失败）。 */
static int store_fields(uint32_t slot)
{
    sloop_store_fields_t f;
    esp_err_t err = sloop_store_project_fields(slot, &f);
    if (err != ESP_OK) return store_err_print(err);
    printf("PROJ %u FIELDS fmt=%u sel=%u bpm=%d swing=%d dust=%d drdly=%u", (unsigned)slot,
           (unsigned)f.fmt, (unsigned)f.sel, (int)f.bpm, (int)f.swing, (int)f.dust,
           (unsigned)f.drdly);
    for (uint32_t k = 0; k < 4u; k++) {
        printf(" t%u.eng=%u t%u.pre=%u t%u.lvl=%d t%u.atk=%d t%u.e0=%d t%u.e1=%d", (unsigned)k,
               (unsigned)f.trk[k].engine, (unsigned)k, (unsigned)f.trk[k].preset, (unsigned)k,
               (int)f.trk[k].level, (unsigned)k, (int)f.trk[k].atk, (unsigned)k,
               (int)f.trk[k].e0, (unsigned)k, (int)f.trk[k].e1);
        for (uint32_t i = 0; i < 8u; i++)
            printf(" t%u.s%u=%u:%u", (unsigned)k, (unsigned)i, (unsigned)f.trk[k].n[i],
                   (unsigned)f.trk[k].v[i]);
    }
    printf("\n");
    return 0;
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
    if (argc == 4 && strcasecmp(argv[2], "fields") == 0) {
        /* 旧格式/工程字段（spec §6.3）：只读 flash 对象（st_load + proj_import），
         * 不动引擎与 RAM 槽；固定顺序的 key=value 一行（gen_project_fixtures.py 的
         * expected.json 按同一顺序逐项比对）。 */
        uint32_t slot;
        if (!eng_uint(argv[3], SLOOP_STORE_PROJECTS - 1u, &slot))
            return store_err_print(ESP_ERR_INVALID_ARG);
        return store_fields(slot);
    }
    if (argc != 4) {
        printf("用法: STORE PROJ LIST | SAVE n | LOAD n | ERASE n | HASH n（n = 0..3）| "
               "FIELDS n | SYNTH <fmt1|foreign>（诊断：覆盖诊断槽 3）\n");
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
           "FIELDS n | SYNTH <fmt1|foreign>（诊断：覆盖诊断槽 3）\n");
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

/* FM6 银行 27 槽（上游 fm6_bank.c，P4-B Task 7）。console 诊断执行器口径同 PROJ/PRESET：
 * 与 UI 命令共用 sloop_store_* 与同一套锁。
 *   STORE BANK LIST     -> 27 行 "B<k> USED|EMPTY" + "BANK COUNT used=n total=27"
 *   STORE BANK PUT n    -> "BANK n SAVED"（选中轨的 live patch；播放中 playing）
 *   STORE BANK GET n    -> "BANK n GET <128B hex>"；空槽 empty
 *   STORE BANK LIVE     -> "BANK LIVE <128B hex>"（选中轨当前 patch，不写 flash）
 *   STORE BANK ERASE n  -> "BANK n ERASED"
 * 选中鼓轨时 PUT/LIVE 回 "STORE ERR drum"：fm6_patch 只对 NPART 个合成轨存在，鼓轨没有
 * patch 可存（与预置的 DRUM TRACK: NO SOUND 同一类拒绝，令牌与槽号越界分开）。
 * LIVE 只读引擎状态，故**不要求存储就绪**（其余子命令都需要）。 */
static void store_hex(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) printf("%02X", p[i]);
}

static bool store_drum_selected(void)
{
    sloop_core_snapshot_t snap;
    sloop_core_read(&snap); /* 选中轨取自引擎真值（与 PARAM/PRESET 同源） */
    return snap.selected_track >= APP_STATE_TRACKS - 1u;
}

static int store_bank(int argc, char **argv)
{
    if (argc == 3 && strcasecmp(argv[2], "list") == 0) {
        if (!sloop_store_ready()) return store_err_print(ESP_ERR_INVALID_STATE);
        uint32_t used = 0;
        for (uint32_t k = 0; k < SLOOP_STORE_BANK_SLOTS; k++) {
            bool u = sloop_store_bank_used(k);
            printf("B%u %s\n", (unsigned)k, u ? "USED" : "EMPTY");
            used += u ? 1u : 0u;
        }
        printf("BANK COUNT used=%u total=%u\n", (unsigned)used, (unsigned)SLOOP_STORE_BANK_SLOTS);
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[2], "live") == 0) {
        /* 只读引擎状态（sloop_store_bank_current 不依赖存储就绪）：无存储时也能导出当前音色 */
        if (store_drum_selected()) return store_err_drum();
        uint8_t pk[SLOOP_STORE_BANK_PACKED];
        sloop_store_bank_current(pk);
        printf("BANK LIVE ");
        store_hex(pk, sizeof pk);
        printf("\n");
        return 0;
    }
    if (argc != 4) {
        printf("用法: STORE BANK LIST | PUT n | GET n | LIVE | ERASE n（n = 0..%u；"
               "PTCH = 8 + n）\n", (unsigned)(SLOOP_STORE_BANK_SLOTS - 1u));
        return 1;
    }
    uint32_t slot;
    if (!eng_uint(argv[3], SLOOP_STORE_BANK_SLOTS - 1u, &slot)) {
        return store_err_print(ESP_ERR_INVALID_ARG);
    }
    esp_err_t err;
    if (strcasecmp(argv[2], "put") == 0) {
        if (store_drum_selected()) return store_err_drum();
        uint8_t pk[SLOOP_STORE_BANK_PACKED];
        sloop_store_bank_current(pk);
        if ((err = sloop_store_bank_put(slot, pk)) != ESP_OK) return store_err_print(err);
        printf("BANK %u SAVED\n", (unsigned)slot);
        return 0;
    }
    if (strcasecmp(argv[2], "get") == 0) {
        uint8_t pk[SLOOP_STORE_BANK_PACKED];
        if (!sloop_store_bank_get(slot, pk)) return store_err_print(ESP_ERR_NOT_FOUND);
        printf("BANK %u GET ", (unsigned)slot);
        store_hex(pk, sizeof pk);
        printf("\n");
        return 0;
    }
    if (strcasecmp(argv[2], "erase") == 0) {
        if ((err = sloop_store_bank_erase(slot)) != ESP_OK) return store_err_print(err);
        printf("BANK %u ERASED\n", (unsigned)slot);
        return 0;
    }
    printf("用法: STORE BANK LIST | PUT n | GET n | LIVE | ERASE n（n = 0..%u；"
           "PTCH = 8 + n）\n", (unsigned)(SLOOP_STORE_BANK_SLOTS - 1u));
    return 1;
}

/* 歌曲顺序（PER3 子集，P4-C Task 7）。console 是诊断执行器：与 Song 页/状态层共用
 * sloop_store_song_*，只是能把字节级结果变成脚本可断言的回显。
 *   STORE SONG                       -> "STORE SONG count=.. loop=.. dirty=.." + 每项一行
 *                                       "SONG <i> scene=<s> bars=<b>"（i < count）
 *   STORE SONG SET <count> <loop> <scene:bars>… -> "STORE SONG SET count=.. loop=.. dirty=1"
 *                                       （只改 0..count-1，其余项保留当前值；落盘由
 *                                       1s 任务的停止/安静/idle 门限或 STORE SONG SAVE 完成）
 *   STORE SONG SAVE                  -> "STORE SONG SAVED len=<n> side=<0|1>"（上游
 *                                       arrangement_save 同义：播放中回 "STORE ERR playing"）
 *   STORE SONG LOAD                  -> 从 settings 读回（**以 flash 为准**、清 dirty；
 *                                       读不到/对象损坏 → 上游默认歌曲，字节不覆盖、回 invalid）
 *   STORE SONG INFO                  -> flash 对象的事实：kind=per3|per2|per1|empty|invalid、
 *                                       len、side（side record 是否自校验通过）、count/loop
 * 设置对象损坏（未知 magic/长度）时 SAVE 回 "STORE ERR invalid" 且不覆盖。 */
static const char *const STORE_SONG_KINDS[] = {"empty", "per3", "per2", "per1", "invalid"};

static int store_song_usage(void)
{
    printf("用法: STORE SONG | SET <count=1..16> <loop=0|1> <scene=0..3:bars=1..64>… | "
           "SAVE | LOAD | INFO\n");
    return 1;
}

static int store_song(int argc, char **argv)
{
    sloop_song_config_t cfg;
    esp_err_t err;
    if (argc == 2) {
        if ((err = sloop_store_song_get(&cfg)) != ESP_OK) return store_err_print(err);
        printf("STORE SONG count=%u loop=%u dirty=%u\n", (unsigned)cfg.count, (unsigned)cfg.loop,
               sloop_store_song_dirty() ? 1u : 0u);
        for (uint32_t i = 0; i < cfg.count && i < SLOOP_SONG_ENTRIES; i++)
            printf("SONG %u scene=%u bars=%u\n", (unsigned)i, (unsigned)cfg.entry[i].scene,
                   (unsigned)cfg.entry[i].bars);
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[2], "info") == 0) {
        sloop_store_song_info_t info;
        if ((err = sloop_store_song_info(&info)) != ESP_OK) return store_err_print(err);
        printf("STORE SONG INFO kind=%s len=%u side=%u count=%u loop=%u\n",
               STORE_SONG_KINDS[info.kind < 5u ? info.kind : 4u], (unsigned)info.len,
               (unsigned)info.side, (unsigned)info.count, (unsigned)info.loop);
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[2], "save") == 0) {
        if ((err = sloop_store_song_save()) != ESP_OK) return store_err_print(err);
        sloop_store_song_info_t info;
        if ((err = sloop_store_song_info(&info)) != ESP_OK) return store_err_print(err);
        printf("STORE SONG SAVED len=%u side=%u\n", (unsigned)info.len, (unsigned)info.side);
        return 0;
    }
    if (argc == 3 && strcasecmp(argv[2], "load") == 0) {
        err = sloop_store_song_reload();
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return store_err_print(err);
        if ((err = sloop_store_song_get(&cfg)) != ESP_OK) return store_err_print(err);
        printf("STORE SONG LOAD count=%u loop=%u dirty=%u\n", (unsigned)cfg.count, (unsigned)cfg.loop,
               sloop_store_song_dirty() ? 1u : 0u);
        return 0;
    }
    if (argc >= 5 && strcasecmp(argv[2], "set") == 0) {
        if (argc - 5 > (int)SLOOP_SONG_ENTRIES) return store_song_usage();
        uint32_t count, loop;
        if (!eng_uint(argv[3], SLOOP_SONG_ENTRIES, &count) || count == 0 ||
            !eng_uint(argv[4], 1u, &loop))
            return store_err_print(ESP_ERR_INVALID_ARG);
        if ((err = sloop_store_song_get(&cfg)) != ESP_OK) return store_err_print(err);
        cfg.count = (uint8_t)count;
        cfg.loop = (uint8_t)loop;
        for (int i = 0; i < argc - 5; i++) {
            unsigned scene, bars;
            if ((uint32_t)i >= count || sscanf(argv[5 + i], "%u:%u", &scene, &bars) != 2)
                return store_err_print(ESP_ERR_INVALID_ARG);
            cfg.entry[i].scene = (uint8_t)scene;
            cfg.entry[i].bars = (uint8_t)bars;
        }
        if ((err = sloop_store_song_set(&cfg)) != ESP_OK) return store_err_print(err);
        printf("STORE SONG SET count=%u loop=%u dirty=%u\n", (unsigned)cfg.count, (unsigned)cfg.loop,
               sloop_store_song_dirty() ? 1u : 0u);
        return 0;
    }
    return store_song_usage();
}

/* 设备侧 torn-write 套件（ADR-009 §3，**破坏性**：占用 OBJ_SETTINGS，前后快照/恢复
 * **两个完整 4KB 副本扇区**并逐字节校验——settings 的 side record（扇区 64..255）与
 * payload 48..83 的歌曲顺序、DSU/MIDI 尾段都不丢；P4-C Task 7 起替换 payload-only 快照）。
 * 六场景全部通过回 `STORE TEST PASS 6/6`；失败回 `STORE TEST FAIL <name>`（不打印其它
 * 令牌，脚本按这两个前缀断言）。未就绪/走带中沿用 `STORE ERR <no_storage|playing>`。 */
static int store_test_cmd(void)
{
    sloop_store_test_t r;
    esp_err_t err = sloop_store_test(&r);
    if (err == ESP_OK) {
        printf("STORE TEST PASS %u/%u\n", (unsigned)r.passed, (unsigned)r.cases);
        return 0;
    }
    if (r.fail_case != NULL) {
        printf("STORE TEST FAIL %s\n", r.fail_case);
        return 1;
    }
    return store_err_print(err);
}

/* STORE RAW：分块上传任意对象 payload（spec §4.4；旧格式 fixture 的唯一入设备通道）。
 *   BEGIN <len>        → "STORE RAW BEGIN len=<n>"（len 1..SLOOP_STORE_RAW_MAX = 3,840 =
 *                        一个 flash 对象的 payload 上限；超限当场回 size，不等到 END 的 save）
 *   <hex>（≤240 字符） → "STORE RAW received=<n>"（累计字节数；超过 BEGIN 的 len ⇒ size）
 *   END <obj> <slot>   → "STORE RAW END <obj> <slot> saved len=<n>"（obj 目前只有 project）
 *   ABORT              → "STORE RAW ABORTED"
 * 其它 STORE 子命令入口会先 sloop_store_raw_abort()（R27：暂存与 proj_tmp 共用）。
 * 一行 ≤240 hex = 120B：真正的上限不是 console 行缓冲（256），而是 USB-Serial-JTAG
 * **驱动 RX 环 256B**——整行要在一次突发里装进环里，否则设备忙（LVGL/音频）时环溢出、
 * 字符被静默丢掉（实测 490 字符的行被截到 256 字符）。240 hex + "STORE RAW " + '\n'
 * = 251B < 256B，故与调度时序无关。 */
#define STORE_RAW_HEX_MAX 240u
#define STORE_RAW_BUF (STORE_RAW_HEX_MAX / 2u)

static int store_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int store_raw_usage(void)
{
    printf("用法: STORE RAW BEGIN <len=1..%u> | STORE RAW <hex≤%u> | "
           "STORE RAW END <project> <slot=0..%u> | STORE RAW ABORT\n",
           (unsigned)SLOOP_STORE_RAW_MAX, (unsigned)STORE_RAW_HEX_MAX,
           (unsigned)(SLOOP_STORE_PROJECTS - 1u));
    return 1;
}

static int store_raw(int argc, char **argv)
{
    if (argc >= 3 && strcasecmp(argv[2], "begin") == 0) {
        uint32_t len;
        if (argc != 4) return store_raw_usage();
        if (!eng_uint(argv[3], SLOOP_STORE_RAW_MAX, &len) || len == 0)
            return store_err_print(ESP_ERR_INVALID_SIZE);
        esp_err_t err = sloop_store_raw_begin(len);
        if (err != ESP_OK) return store_err_print(err);
        printf("STORE RAW BEGIN len=%u\n", (unsigned)len);
        return 0;
    }
    if (argc >= 3 && strcasecmp(argv[2], "end") == 0) {
        uint32_t slot;
        if (argc != 5) return store_raw_usage();
        if (!eng_uint(argv[4], SLOOP_STORE_PROJECTS - 1u, &slot)) {
            sloop_store_raw_abort();
            return store_err_print(ESP_ERR_INVALID_ARG);
        }
        uint32_t len = sloop_store_raw_received(); /* 成功时 == 声明长度（end 已校验） */
        esp_err_t err = sloop_store_raw_end(argv[3], slot);
        if (err != ESP_OK) return store_err_print(err);
        printf("STORE RAW END %s %u saved len=%u\n", argv[3], (unsigned)slot, (unsigned)len);
        return 0;
    }
    if (argc >= 3 && strcasecmp(argv[2], "abort") == 0) {
        if (argc != 3) return store_raw_usage();
        sloop_store_raw_abort();
        printf("STORE RAW ABORTED\n");
        return 0;
    }
    if (argc == 3) {
        const char *hex = argv[2];
        size_t n = strlen(hex);
        uint8_t buf[STORE_RAW_BUF];
        if (n == 0 || (n & 1u) != 0u || n / 2u > sizeof buf) return store_err_print(ESP_ERR_INVALID_SIZE);
        for (size_t i = 0; i < n; i += 2u) {
            int hi = store_hex_nibble(hex[i]), lo = store_hex_nibble(hex[i + 1u]);
            if (hi < 0 || lo < 0) return store_err_print(ESP_ERR_INVALID_ARG);
            buf[i / 2u] = (uint8_t)((hi << 4) | lo);
        }
        esp_err_t err = sloop_store_raw_append(buf, (uint32_t)(n / 2u));
        if (err != ESP_OK) return store_err_print(err);
        printf("STORE RAW received=%u\n", (unsigned)sloop_store_raw_received());
        return 0;
    }
    return store_raw_usage();
}

static int store_command(int argc, char **argv)
{
    /* R27：RAW 暂存就是 proj_tmp/up_stage 那块共用缓冲——任何其它存储子命令都可能
     * 借用它，故非 RAW 子命令一进来就丢弃未完成的上传（否则 END 会把别人的字节原样
     * 写进 flash）。RAW 自己的 BEGIN/hex/END/ABORT 不走这里的分支。 */
    if (argc >= 2 && strcasecmp(argv[1], "raw") == 0) return store_raw(argc, argv);
    sloop_store_raw_abort();

    if (argc == 2 && strcasecmp(argv[1], "stats") == 0) return store_stats();
    if (argc == 2 && strcasecmp(argv[1], "rawtest") == 0) return store_rawtest();
    if (argc == 2 && strcasecmp(argv[1], "livehash") == 0) return store_livehash();
    if (argc == 2 && strcasecmp(argv[1], "test") == 0) return store_test_cmd();
    if (argc >= 2 && strcasecmp(argv[1], "fault") == 0) return store_fault(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "proj") == 0) return store_proj(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "preset") == 0) return store_preset(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "bank") == 0) return store_bank(argc, argv);
    if (argc >= 2 && strcasecmp(argv[1], "song") == 0) return store_song(argc, argv);
    if (argc == 3 && strcasecmp(argv[1], "auto") == 0) {
        bool on;
        if (strcasecmp(argv[2], "on") == 0) {
            on = true;
        } else if (strcasecmp(argv[2], "off") == 0) {
            on = false;
        } else {
            printf("用法: STORE AUTO <on|off>\n");
            return 1;
        }
        printf("STORE AUTO %s\n", sloop_store_autosave_enable(on) ? "on" : "off");
        return 0;
    }
    printf("用法: STORE STATS | STORE RAWTEST（破坏：擦写空闲扇区 0x%X） | "
           "STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n|FIELDS n|SYNTH <fmt1|foreign> | "
           "STORE PRESET LIST|SAVE n|LOAD n [track]|ERASE n | "
           "STORE BANK LIST|PUT n|GET n|LIVE|ERASE n | STORE SONG [SET c l s:b…|SAVE|LOAD|INFO] | "
           "STORE LIVEHASH | "
           "STORE RAW BEGIN|hex|END|ABORT（分块上传对象 payload） | "
           "STORE AUTO <on|off> | STORE TEST（破坏：占用 OBJ_SETTINGS，快照/恢复两个 "
           "完整 4KB 副本扇区） | "
           "STORE FAULT <nth=1..9999> [half] | STORE FAULT CLEAR\n",
           (unsigned)STORE_RAWTEST_OFF);
    return 1;
}

static int seq_err_print(esp_err_t err)
{
    const char *token = err == ESP_ERR_NO_MEM ? "full" :
                        err == ESP_ERR_INVALID_ARG ? "invalid" :
                        err == ESP_ERR_NOT_ALLOWED ? "blocked" :
                        err == ESP_ERR_NOT_FOUND ? "not_found" :
                        err == ESP_ERR_NOT_FINISHED ? "save" :
                        err == ESP_ERR_INVALID_STATE ? "frozen" : esp_err_to_name(err);
    printf("SEQ ERR %s\n", token);
    return 1;
}

static void seq_dstep_print(uint32_t step)
{
    sloop_dstep_t d;
    sloop_seq_read_dstep(step, &d);
    printf("SEQ DSTEP step=%u on=%02X,%02X lvl=%02X,%02X,%02X,%02X rat=%02X,%02X,%02X,%02X\n",
           (unsigned)step, d.on[0], d.on[1], d.lvl[0], d.lvl[1], d.lvl[2], d.lvl[3],
           d.rat[0], d.rat[1], d.rat[2], d.rat[3]);
}

/* 录音状态行（P4-C Task 6）：读一次快照即消费一次性 take 结果（sloop_seq.h）。
 * take 结果：none / dropped / <n>bar；与 STATE SNAP 的 rec_* 字段同一投影。 */
static void seq_rec_print(void)
{
    static const char *const STATE_NAME[] = {"off", "armed", "recording", "count_in", "free_take"};
    sloop_rec_snapshot_t r;
    sloop_seq_rec_read(&r);
    const char *state = r.state <= SEQ_REC_FREE_TAKE ? STATE_NAME[r.state] : "?";
    char take[16] = "none";
    if (r.take_bars == 0xFFu)
        snprintf(take, sizeof(take), "dropped");
    else if (r.take_bars != 0u)
        snprintf(take, sizeof(take), "%ubar", (unsigned)r.take_bars);
    printf("SEQ REC state=%s track=%u quant=%s tempo=%s count=%s take=%s fill_held=%u fill_arm=%u\n",
           state, (unsigned)r.track, r.quant_off ? "off" : "on", r.tempo_mode ? "tempo" : "free",
           r.count_mode ? "count" : "note", take, (unsigned)r.fill_held, (unsigned)r.fill_arm);
}

/* ------------------------------------------------- P4-C Task 8: sections / chain / SONG ---
 * console 是诊断执行器：与 SONG 页/状态层共用 sloop_seq_section/chain/song_* 与
 * sloop_store_song_*，只把运行态变成脚本可断言的回显。错误令牌沿用 SEQ 族：
 * blocked = 走带中/SONG 播放中/REC IS ON，frozen = 诊断冻结，invalid = 参数/槽号，
 * not_found = 空 section/无此音；存储未就绪的 INVALID_STATE 明确回 no_storage。 */
static int seq_song_err(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE && !sloop_seq_frozen()) {
        printf("SEQ ERR %s\n", sloop_store_ready() ? "playing" : "no_storage");
        return 1;
    }
    return seq_err_print(err);
}

static const char *seq_scene_name(uint8_t scene, char *buf)
{
    if (scene < 4u) {
        buf[0] = (char)('A' + scene);
        buf[1] = 0;
        return buf;
    }
    return "-";
}

static bool seq_scene_arg(const char *text, uint32_t *out)
{
    if (text[0] != 0 && text[1] == 0) {
        if (text[0] >= 'A' && text[0] <= 'D') {
            *out = (uint32_t)(text[0] - 'A');
            return true;
        }
        if (text[0] >= 'a' && text[0] <= 'd') {
            *out = (uint32_t)(text[0] - 'a');
            return true;
        }
    }
    return eng_uint(text, 3, out);
}

static const char *const SEQ_SREC_NAME[] = {"off", "armed", "recording"};

static void seq_song_print(void)
{
    sloop_song_snapshot_t s;
    char live[2];
    sloop_seq_song_read(&s);
    const char *srec = s.srec <= 2u ? SEQ_SREC_NAME[s.srec] : "?";
    printf("SEQ SONG mode=%s running=%u index=%u bar=%u ready=0x%02X live=%s dirty=%u "
           "chain=%u srec=%s count=%u loop=%u\n",
           s.enabled ? "song" : "loop", (unsigned)s.running, (unsigned)s.index, (unsigned)s.bar,
           (unsigned)s.ready, seq_scene_name(s.live, live), (unsigned)s.dirty,
           (unsigned)s.chain_count, srec, (unsigned)s.config.count, (unsigned)s.config.loop);
    for (uint32_t i = 0; i < s.config.count && i < SLOOP_SONG_ENTRIES; i++)
        printf("SEQ SONG %u scene=%u bars=%u\n", (unsigned)i, (unsigned)s.config.entry[i].scene,
               (unsigned)s.config.entry[i].bars);
}

/* SONG REC 状态与最近一次结果（0 = 无、0xFF = NO SONG、否则项数）。 */
static void seq_srec_print(void)
{
    sloop_song_snapshot_t s;
    char last[16] = "none";
    sloop_seq_song_read(&s);
    if (s.srec_done == 0xFFu)
        snprintf(last, sizeof last, "no_song");
    else if (s.srec_done != 0u)
        snprintf(last, sizeof last, "%u", (unsigned)s.srec_done);
    printf("SEQ SREC srec=%s last=%s\n",
           s.srec <= 2u ? SEQ_SREC_NAME[s.srec] : "?", last);
}

static void seq_chain_print(void)
{
    sloop_song_snapshot_t s;
    sloop_seq_song_read(&s);
    printf("SEQ CHAIN count=%u index=%u bars=%u\n", (unsigned)s.chain_count,
           (unsigned)s.chain_index, (unsigned)s.chain_bars);
    for (uint32_t i = 0; i < s.chain_count && i < 8u; i++)
        printf("SEQ CHAIN %u scene=%u\n", (unsigned)i, (unsigned)s.chain_scenes[i]);
}

static void seq_section_print(void)
{
    sloop_song_snapshot_t s;
    char live[2];
    sloop_seq_song_read(&s);
    printf("SEQ SECTION ready=0x%02X live=%s dirty=%u index=%u bar=%u\n", (unsigned)s.ready,
           seq_scene_name(s.live, live), (unsigned)s.dirty, (unsigned)s.index, (unsigned)s.bar);
}

static int seq_command(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "DSTEP") == 0) {
        uint32_t step;
        if (argc == 2) {
            for (step = 0; step < 64u; step++) seq_dstep_print(step);
            return 0;
        }
        if (argc == 3 && eng_uint(argv[2], 63, &step)) {
            seq_dstep_print(step);
            return 0;
        }
        return seq_err_print(ESP_ERR_INVALID_ARG);
    }
    if (argc == 5 && strcasecmp(argv[1], "MICRO") == 0) {
        uint32_t track, step;
        int32_t value;
        if (!eng_uint(argv[2], 3, &track) || !eng_uint(argv[3], 63, &step) ||
            !seq_int(argv[4], -32, 31, &value))
            return seq_err_print(ESP_ERR_INVALID_ARG);
        esp_err_t ret = sloop_seq_set_micro(track, step, (int8_t)value);
        if (ret != ESP_OK) return seq_err_print(ret);
        printf("SEQ MICRO track=%u step=%u value=%ld OK\n", (unsigned)track, (unsigned)step, (long)value);
        return 0;
    }
    if (argc == 5 && strcasecmp(argv[1], "FILL") == 0) {
        uint32_t track, step, value;
        if (!eng_uint(argv[2], 3, &track) || !eng_uint(argv[3], 63, &step) ||
            !eng_uint(argv[4], 3, &value))
            return seq_err_print(ESP_ERR_INVALID_ARG);
        esp_err_t ret = sloop_seq_set_fill(track, step, (uint8_t)value);
        if (ret != ESP_OK) return seq_err_print(ret);
        printf("SEQ FILL track=%u step=%u value=%u OK\n", (unsigned)track, (unsigned)step, (unsigned)value);
        return 0;
    }
    if (argc >= 3 && strcasecmp(argv[1], "EDIT") == 0) {
        sloop_step_edit_t cmd = {0};
        const char *op = argv[2];
        if (strcasecmp(op, "SPAN") == 0)
            cmd.op = SEQ_STEP_SPAN;
        else if (strcasecmp(op, "DEL") == 0)
            cmd.op = SEQ_STEP_NOTE_DEL;
        else if (strcasecmp(op, "MOVE") == 0)
            cmd.op = SEQ_STEP_NOTE_MOVE;
        else if (strcasecmp(op, "COPY") == 0)
            cmd.op = SEQ_STEP_NOTE_COPY;
        else if (strcasecmp(op, "LVL") == 0)
            cmd.op = SEQ_STEP_LVL;
        else if (strcasecmp(op, "RAT") == 0)
            cmd.op = SEQ_STEP_RAT;
        else if (strcasecmp(op, "SLIDE") == 0)
            cmd.op = SEQ_STEP_SLIDE;
        else if (strcasecmp(op, "CLEAR") == 0)
            cmd.op = SEQ_STEP_CLEAR;
        else
            return seq_err_print(ESP_ERR_INVALID_ARG);
        uint32_t track, step, a, b;
        if (argc < 5 || !eng_uint(argv[3], 3, &track) || !eng_uint(argv[4], 63, &step))
            return seq_err_print(ESP_ERR_INVALID_ARG);
        cmd.track = (uint8_t)track;
        cmd.step = (uint8_t)step;
        switch (cmd.op) {
        case SEQ_STEP_SPAN:
            if (argc != 6 || !eng_uint(argv[5], 64, &a) || a == 0)
                return seq_err_print(ESP_ERR_INVALID_ARG);
            cmd.value = (int16_t)a;
            break;
        case SEQ_STEP_NOTE_DEL:
            if (argc != 6 || !eng_uint(argv[5], 3, &a))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            cmd.index = (uint8_t)a;
            break;
        case SEQ_STEP_NOTE_MOVE:
        case SEQ_STEP_NOTE_COPY:
            if (argc != 6 || !eng_uint(argv[5], 63, &a))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            cmd.target = (uint8_t)a;
            break;
        case SEQ_STEP_LVL:
        case SEQ_STEP_RAT:
            if (argc != 7 || !eng_uint(argv[5], 3, &a) || !eng_uint(argv[6], 3, &b))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            cmd.index = (uint8_t)a;
            cmd.value = (int16_t)b;
            break;
        case SEQ_STEP_SLIDE:
            if (argc != 6 || !eng_uint(argv[5], 1, &a))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            cmd.value = (int16_t)a;
            break;
        case SEQ_STEP_CLEAR:
            if (argc != 5) return seq_err_print(ESP_ERR_INVALID_ARG);
            break;
        default:
            return seq_err_print(ESP_ERR_INVALID_ARG);
        }
        esp_err_t ret = sloop_seq_step_edit(&cmd);
        if (ret != ESP_OK) return seq_err_print(ret);
        printf("SEQ EDIT %s track=%u step=%u OK\n", op, (unsigned)track, (unsigned)step);
        return 0;
    }
    if (argc >= 3 && strcasecmp(argv[1], "PATTERN") == 0) {
        sloop_pattern_op_t op;
        int need_arg;
        if (strcasecmp(argv[2], "ROTATE") == 0)
            op = SEQ_PATTERN_ROTATE, need_arg = 1;
        else if (strcasecmp(argv[2], "LENGTH") == 0)
            op = SEQ_PATTERN_LENGTH, need_arg = 1;
        else if (strcasecmp(argv[2], "TRANSPOSE") == 0)
            op = SEQ_PATTERN_TRANSPOSE, need_arg = 1;
        else if (strcasecmp(argv[2], "CLEAR") == 0)
            op = SEQ_PATTERN_CLEAR, need_arg = 0;
        else if (strcasecmp(argv[2], "UNDO") == 0)
            op = SEQ_PATTERN_UNDO, need_arg = 0;
        else if (strcasecmp(argv[2], "REDO") == 0)
            op = SEQ_PATTERN_REDO, need_arg = 0;
        else
            return seq_err_print(ESP_ERR_INVALID_ARG);
        uint32_t track;
        int32_t arg = 0;
        if (argc != (need_arg ? 5 : 4) || !eng_uint(argv[3], 3, &track) ||
            (need_arg && !seq_int(argv[4], -32768, 32767, &arg)))
            return seq_err_print(ESP_ERR_INVALID_ARG);
        esp_err_t ret = sloop_seq_pattern(track, op, arg);
        if (ret != ESP_OK) return seq_err_print(ret);
        printf("SEQ PATTERN %s track=%u arg=%ld OK\n", argv[2], (unsigned)track, (long)arg);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "LOCK") == 0) {
        if (argc == 4 && strcasecmp(argv[2], "LIST") == 0) {
            uint32_t track;
            if (!eng_uint(argv[3], 3, &track)) return seq_err_print(ESP_ERR_INVALID_ARG);
            sloop_lock_t locks[24];
            uint32_t n = sloop_seq_lock_list(track, locks, 24);
            for (uint32_t i = 0; i < n; i++)
                printf("SEQ LOCK step=%u param=%u value=%d\n", locks[i].step, locks[i].param, locks[i].value);
            printf("SEQ LOCK LIST track=%u count=%u\n", (unsigned)track, (unsigned)n);
            return 0;
        }
        uint32_t track, step, param;
        if (argc == 7 && strcasecmp(argv[2], "SET") == 0) {
            int32_t value;
            if (!eng_uint(argv[3], 3, &track) || !eng_uint(argv[4], 63, &step) ||
                !eng_uint(argv[5], SLOOP_CORE_P_COUNT - 1u, &param) ||
                !seq_int(argv[6], -32768, 32767, &value))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_lock_set(track, step, param, (int16_t)value);
            if (ret != ESP_OK) return seq_err_print(ret);
            printf("SEQ LOCK SET track=%u step=%u param=%u value=%ld OK\n",
                   (unsigned)track, (unsigned)step, (unsigned)param, (long)value);
            return 0;
        }
        if (argc == 6 && strcasecmp(argv[2], "DEL") == 0) {
            if (!eng_uint(argv[3], 3, &track) || !eng_uint(argv[4], 63, &step) ||
                !eng_uint(argv[5], SLOOP_CORE_P_COUNT, &param))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_lock_del(track, step, param);
            if (ret != ESP_OK) return seq_err_print(ret);
            printf("SEQ LOCK DEL track=%u step=%u param=%u OK\n", (unsigned)track, (unsigned)step, (unsigned)param);
            return 0;
        }
    }
    if (argc >= 2 && strcasecmp(argv[1], "REC") == 0) {
        if (argc == 2) {
            seq_rec_print();
            return 0;
        }
        if (argc == 3 && strcasecmp(argv[2], "TOGGLE") == 0) {
            esp_err_t ret = sloop_seq_rec_toggle();
            if (ret != ESP_OK) return seq_err_print(ret);
            seq_rec_print();
            return 0;
        }
        if (argc == 6 && strcasecmp(argv[2], "CONFIG") == 0) {
            uint32_t q, t, c;
            if (!eng_uint(argv[3], 1, &q) || !eng_uint(argv[4], 1, &t) || !eng_uint(argv[5], 1, &c))
                return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_rec_config((uint8_t)q, (uint8_t)t, (uint8_t)c);
            if (ret != ESP_OK) return seq_err_print(ret);
            seq_rec_print();
            return 0;
        }
        if (argc == 5 && strcasecmp(argv[2], "FILL") == 0 && strcasecmp(argv[3], "HOLD") == 0) {
            uint32_t held;
            if (!eng_uint(argv[4], 1, &held)) return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_fill_control(held != 0u, false);
            if (ret != ESP_OK) return seq_err_print(ret);
            printf("SEQ REC FILL HOLD %u OK\n", (unsigned)held);
            return 0;
        }
        if (argc == 4 && strcasecmp(argv[2], "FILL") == 0 && strcasecmp(argv[3], "NEXT") == 0) {
            /* 脉冲：只翻转下一小节待命，不读不写 held（上游两个独立变量） */
            esp_err_t ret = sloop_seq_fill_control(false, true);
            if (ret != ESP_OK) return seq_err_print(ret);
            printf("SEQ REC FILL NEXT OK\n");
            return 0;
        }
        if (argc == 4 && strcasecmp(argv[2], "BUTTON") == 0) {
            uint32_t down;
            if (!eng_uint(argv[3], 1, &down)) return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_play_button(down != 0u);
            if (ret != ESP_OK) return seq_err_print(ret);
            printf("SEQ REC BUTTON down=%u OK\n", (unsigned)down);
            return 0;
        }
        return seq_err_print(ESP_ERR_INVALID_ARG);
    }
    if ((argc == 3 || argc == 4) && strcasecmp(argv[1], "HIT") == 0) {
        uint32_t lane, vel = 100;
        if (!eng_uint(argv[2], 15, &lane) || (argc == 4 && !eng_uint(argv[3], 127, &vel)))
            return seq_err_print(ESP_ERR_INVALID_ARG);
        esp_err_t ret = sloop_seq_drum_hit(lane, vel);
        if (ret != ESP_OK) return seq_err_print(ret);
        printf("SEQ HIT lane=%u velocity=%u OK\n", (unsigned)lane, (unsigned)vel);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "SECTION") == 0) {
        if (argc == 2) {
            seq_section_print();
            return 0;
        }
        uint32_t slot;
        if (argc != 4 || !seq_scene_arg(argv[2], &slot)) return seq_err_print(ESP_ERR_INVALID_ARG);
        const char *act = NULL;
        esp_err_t ret;
        if (strcasecmp(argv[3], "store") == 0) {
            act = "STORE";
            ret = sloop_seq_section(slot, true, false);
        } else if (strcasecmp(argv[3], "load") == 0) {
            act = "LOAD";
            ret = sloop_seq_section(slot, false, false);
        } else if (strcasecmp(argv[3], "erase") == 0) {
            act = "ERASE";
            ret = sloop_seq_section(slot, false, true);
        } else if (strcasecmp(argv[3], "request") == 0) {
            act = "REQUEST";
            ret = sloop_seq_section_request(slot);
        } else {
            return seq_err_print(ESP_ERR_INVALID_ARG);
        }
        if (ret != ESP_OK) return seq_song_err(ret);
        char name[2];
        printf("SEQ SECTION %s %s OK\n", seq_scene_name((uint8_t)slot, name), act);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "CHAIN") == 0) {
        if (argc == 2) {
            seq_chain_print();
            return 0;
        }
        if (argc > 10) return seq_err_print(ESP_ERR_INVALID_ARG); /* 1..8 个 scene */
        uint8_t scenes[8];
        uint32_t n = (uint32_t)argc - 2u;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t scene;
            if (!seq_scene_arg(argv[2 + i], &scene)) return seq_err_print(ESP_ERR_INVALID_ARG);
            scenes[i] = (uint8_t)scene;
        }
        esp_err_t ret = sloop_seq_chain_begin(scenes[0]);
        if (ret == ESP_OK && n >= 2u) ret = sloop_seq_chain_commit(scenes, n);
        if (ret != ESP_OK) return seq_song_err(ret);
        seq_chain_print();
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "SONG") == 0) {
        if (argc == 2) {
            seq_song_print();
            return 0;
        }
        if (argc == 3 && strcasecmp(argv[2], "REC") == 0) {
            esp_err_t ret = sloop_seq_song_rec();
            if (ret != ESP_OK) return seq_song_err(ret);
            seq_srec_print();
            return 0;
        }
        if (argc == 4 && strcasecmp(argv[2], "MODE") == 0) {
            bool enabled;
            if (strcasecmp(argv[3], "song") == 0 || strcmp(argv[3], "1") == 0)
                enabled = true;
            else if (strcasecmp(argv[3], "loop") == 0 || strcmp(argv[3], "0") == 0)
                enabled = false;
            else
                return seq_err_print(ESP_ERR_INVALID_ARG);
            esp_err_t ret = sloop_seq_song_mode(enabled);
            if (ret != ESP_OK) return seq_song_err(ret);
            printf("SEQ SONG mode=%s\n", enabled ? "song" : "loop");
            return 0;
        }
        return seq_err_print(ESP_ERR_INVALID_ARG);
    }
    if (argc >= 2 && strcasecmp(argv[1], "SREC") == 0) {
        if (argc == 2) {
            seq_srec_print();
            return 0;
        }
        if (argc == 3 && strcasecmp(argv[2], "TOGGLE") == 0) {
            esp_err_t ret = sloop_seq_song_rec();
            if (ret != ESP_OK) return seq_song_err(ret);
            seq_srec_print();
            return 0;
        }
        return seq_err_print(ESP_ERR_INVALID_ARG);
    }
    printf("用法: SEQ DSTEP [step=0..63] | SEQ MICRO <track=0..3> <step=0..63> <v=-32..31> | "
           "SEQ FILL <track=0..3> <step=0..63> <v=0..3> | SEQ LOCK LIST <track=0..3> | "
           "SEQ LOCK SET <track> <step> <param=0..%u> <value> | "
           "SEQ LOCK DEL <track> <step> <param=0..%u；%u=整步清除> | "
           "SEQ EDIT <SPAN|DEL|MOVE|COPY|LVL|RAT|SLIDE|CLEAR> <track> <step> [index|target|value] | "
           "SEQ PATTERN <ROTATE|LENGTH|TRANSPOSE> <track> <arg> | "
           "SEQ PATTERN <CLEAR|UNDO|REDO> <track> | "
           "SEQ REC [TOGGLE | CONFIG <quant 0|1> <tempo 0|1> <count 0|1> | "
           "FILL HOLD <0|1> | FILL NEXT | BUTTON <0|1>] | "
           "SEQ HIT <lane=0..15> [velocity=1..127；0=松开] | "
           "SEQ SECTION [<A-D|0..3> STORE|LOAD|ERASE|REQUEST] | "
           "SEQ CHAIN [<A-D|0..3>…] | "
           "SEQ SONG [MODE <song|loop>|REC] | SEQ SREC [TOGGLE]\n",
           (unsigned)SLOOP_CORE_P_COUNT - 1u, (unsigned)SLOOP_CORE_P_COUNT, (unsigned)SLOOP_CORE_P_COUNT);
    return 1;
}

/* ---------------------------------- P4-D Task 7: SMP / DSYN / FM6P ---
 * console 是诊断执行器（与 STORE/SEQ 同口径）：采样槽的写入口是存储层
 * `sloop_store_sample_*`（上传会话只存在于 console），DSYN/FM6P 的编辑与采样擦除走
 * 状态层命令（CMD_DSYN_SET/LOAD_KIT/STORE/AUDITION、CMD_FM6_SET、CMD_SAMPLE_ERASE，
 * 与 UI 页面同一条路径）；只读投影直查 core/store（STATE SNAP 与页面读同一投影）。
 *
 * SMP 上传（scripts/sample_upload.py 的对端）两个 console 侧细节：
 *   - 存储 API 的 write 要求 off 256 对齐、n ≤ 256，而 console 一行只有 ≤120B ⇒ DATA
 *     先攒进 256B 块，满块（以及 END 时的余量）才调 sloop_store_sample_write；任何一次
 *     flush 失败都结束会话（清块与偏移，重试必须重新 BEGIN——否则偏移不再 256 对齐）；
 *   - END 的 ESP_ERR_INVALID_ARG 同时表示 header 与 zone 非法，console 只镜像 header 的
 *     前 32 字节（magic/version/nz/data_len）把令牌分派成 header 或 zones——校验本身
 *     仍只在 adapter（不在这里重复 zone 判据）。
 * 错误令牌：SMP ERR <no_storage|playing|frozen|size|header|crc|zones|flash|state>；
 * DSYN/FM6P ERR <invalid|locked|frozen|playing|error>（synth_status 的投影）。 */
#define SMP_HDR_MAGIC   0x504D5346u /* 上游 eng_sample.c 的 SMP_USER_MAGIC（"FSMP"） */
#define SMP_HDR_LEN     480u        /* smp_user_hdr_t（32 + 16×28） */
#define SMP_HDR_FIXED   32u         /* magic/version/nz/name/data_len/crc 段 */
#define SMP_LINE_MAX    240u        /* console 一行 hex 字符上限（= 120B） */
#define SMP_LINE_BUF    (SMP_LINE_MAX / 2u)
#define SMP_BLOCK       256u        /* sloop_store_sample_write 的 off 对齐粒度 */
#define SMP_UNUSED      ((uint8_t)SLOOP_STORE_SAMPLES)
/* op 6（全局块）末尾 SLOOP_FM6_NAME_FIELDS 项 = 名字的 10 个字节：29 - 10 = 19。 */
#define FM6P_NAME_FIELD0 (SLOOP_FM6_GLOBAL_FIELDS - SLOOP_FM6_NAME_FIELDS)

static uint8_t s_smp_open_slot = SMP_UNUSED; /* console 侧会话标记（与 adapter 的 open slot 同值） */
static uint8_t s_smp_fixed[SMP_HDR_FIXED];   /* header 前 32 字节镜像（只为分派错误令牌） */
static uint32_t s_smp_hdr_seen;              /* 已交给 hdr_append 的字节数 */
static uint8_t s_smp_blk[SMP_BLOCK];         /* 攒块（write 的 off 必须 256 对齐） */
static uint32_t s_smp_blk_n;
static uint32_t s_smp_blk_off;
static uint32_t s_smp_expect;                /* 下一块的槽内绝对偏移（连续上传） */

static void smp_session_reset(uint8_t slot)
{
    s_smp_open_slot = slot;
    s_smp_hdr_seen = 0;
    s_smp_blk_n = 0;
    s_smp_blk_off = SLOOP_STORE_SAMPLE_DATA;
    s_smp_expect = SLOOP_STORE_SAMPLE_DATA;
}

static bool store_playing_now(void)
{
    sloop_core_snapshot_t snap;
    sloop_core_read(&snap);
    return snap.playing;
}

/* END 的 INVALID_ARG：header 长度不是恰好 480B、或固定段坏了 = header；固定段合法但
 * adapter 仍拒绝 = zones（zone 判据只在 adapter，不在这里重复）。 */
static const char *smp_end_token(void)
{
    uint32_t magic = 0, data_len = 0;
    uint16_t version = 0;
    if (s_smp_hdr_seen != SMP_HDR_LEN) return "header";
    memcpy(&magic, s_smp_fixed, sizeof magic);
    memcpy(&version, s_smp_fixed + 4, sizeof version);
    memcpy(&data_len, s_smp_fixed + 16, sizeof data_len);
    if (magic != SMP_HDR_MAGIC || version != 1u || s_smp_fixed[6] == 0u || s_smp_fixed[6] > 16u ||
        data_len > SLOOP_STORE_SAMPLE_SIZE - SLOOP_STORE_SAMPLE_DATA)
        return "header";
    return "zones";
}

static int smp_err(esp_err_t err, bool end)
{
    const char *token;
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        if (!sloop_store_ready()) token = "no_storage";
        else if (sloop_seq_frozen()) token = "frozen";
        else if (store_playing_now()) token = "playing";
        else token = "state"; /* 没有对应的 open slot（未 BEGIN / 槽号不符 / 偏移不连续） */
        break;
    case ESP_ERR_INVALID_SIZE: token = "size"; break;
    case ESP_ERR_INVALID_CRC: token = "crc"; break;
    case ESP_ERR_NOT_FINISHED: token = "flash"; break;
    case ESP_ERR_INVALID_ARG: token = end ? smp_end_token() : "state"; break;
    default: token = "state"; break;
    }
    printf("SMP ERR %s\n", token);
    return 1;
}

static bool smp_uint32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || parsed > 0xFFFFFFFFul) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool smp_hex_decode(const char *hex, uint8_t *buf, uint32_t cap, uint32_t *n)
{
    size_t len = strlen(hex);
    if (len == 0 || (len & 1u) != 0u || len / 2u > cap) return false;
    for (size_t i = 0; i < len; i += 2u) {
        int hi = store_hex_nibble(hex[i]), lo = store_hex_nibble(hex[i + 1u]);
        if (hi < 0 || lo < 0) return false;
        buf[i / 2u] = (uint8_t)((hi << 4) | lo);
    }
    *n = (uint32_t)(len / 2u);
    return true;
}

/* 攒满 256B（或 END 的余量）才真正写 flash：API 要求 off 256 对齐。 */
static esp_err_t smp_block_flush(uint8_t slot)
{
    if (s_smp_blk_n == 0) return ESP_OK;
    esp_err_t err = sloop_store_sample_write(slot, s_smp_blk_off, s_smp_blk, s_smp_blk_n);
    if (err == ESP_OK) s_smp_blk_n = 0;
    return err;
}

/* 每槽一行；`sloop_store_sample_info` 真失败（未就绪/状态异常）与「未使用」分开回显——
 * 前者是 SMP ERR <no_storage|state>，后者才是 EMPTY（读不出槽信息不等于空槽）。 */
static int smp_info_print(void)
{
    for (uint32_t k = 0; k < SLOOP_STORE_SAMPLES; k++) {
        sloop_store_sample_info_t info;
        esp_err_t err = sloop_store_sample_info(k, &info);
        if (err != ESP_OK) return smp_err(err, false);
        if (!info.used)
            printf("SMP %u EMPTY\n", (unsigned)k);
        else
            printf("SMP %u USED name=%s zones=%u kb=%u\n", (unsigned)k, info.name,
                   (unsigned)info.zones, (unsigned)info.data_kb);
    }
    return 0;
}

static int smp_command(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "INFO") == 0 && argc == 2) {
        if (!sloop_store_ready()) return smp_err(ESP_ERR_INVALID_STATE, false);
        return smp_info_print();
    }
    if (argc >= 2 && strcasecmp(argv[1], "BEGIN") == 0) {
        uint32_t slot;
        if (argc != 3 || !eng_uint(argv[2], SLOOP_STORE_SAMPLES - 1u, &slot)) {
            printf("用法: SMP BEGIN <slot=0..%u>\n", (unsigned)(SLOOP_STORE_SAMPLES - 1u));
            return 1;
        }
        esp_err_t err = sloop_store_sample_begin(slot);
        /* 失败时 adapter 可能已把 open slot 清掉（BEGIN 的擦除步骤失败），本地也回 UNUSED：
         * 保守地要求重新 BEGIN，而不是把后续 DATA 攒进一个没有 open slot 的会话。 */
        smp_session_reset(err == ESP_OK ? (uint8_t)slot : SMP_UNUSED);
        if (err != ESP_OK) return smp_err(err, false);
        printf("SMP BEGIN slot=%u OK\n", (unsigned)slot);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "DATA") == 0) {
        uint32_t slot, off, n;
        uint8_t buf[SMP_LINE_BUF];
        if (argc != 5 || !eng_uint(argv[2], SLOOP_STORE_SAMPLES - 1u, &slot) ||
            !smp_uint32(argv[3], &off)) {
            printf("用法: SMP DATA <slot=0..%u> <off≥%u> <hex≤%u>\n",
                   (unsigned)(SLOOP_STORE_SAMPLES - 1u), (unsigned)SLOOP_STORE_SAMPLE_DATA,
                   (unsigned)SMP_LINE_MAX);
            return 1;
        }
        if (!smp_hex_decode(argv[4], buf, sizeof buf, &n)) return smp_err(ESP_ERR_INVALID_SIZE, false);
        /* off < 512（或与上一行不连续）按设计回 state：size 只表示长度/块越界的语法错误，
         * 槽内偏移错位是会话错误（重新 BEGIN 才能对齐）。 */
        if (s_smp_open_slot != slot || off != s_smp_expect) return smp_err(ESP_ERR_INVALID_STATE, false);
        /* 护栏：块只可能因 flush 失败停在 256（那条路径已结束会话），这里再拒一次，
         * 保证 `s_smp_blk[s_smp_blk_n++]` 永远不会写到 256+。 */
        if (s_smp_blk_n >= SMP_BLOCK) return smp_err(ESP_ERR_INVALID_STATE, false);
        for (uint32_t i = 0; i < n; i++) {
            if (s_smp_blk_n == 0) s_smp_blk_off = s_smp_expect + i;
            s_smp_blk[s_smp_blk_n++] = buf[i];
            if (s_smp_blk_n == SMP_BLOCK) {
                esp_err_t err = smp_block_flush((uint8_t)slot);
                if (err != ESP_OK) {
                    /* flush 失败时块里仍是最多 256B 未落盘的数据：结束会话（block 清空、
                     * expect 回 512），重试必须重新 BEGIN——否则下一次写入会从 256+ 越界。 */
                    smp_session_reset(SMP_UNUSED);
                    return smp_err(err, false);
                }
            }
        }
        s_smp_expect += n;
        printf("SMP DATA slot=%u off=%u n=%u OK\n", (unsigned)slot, (unsigned)off, (unsigned)n);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "HDR") == 0) {
        uint32_t n;
        uint8_t buf[SMP_LINE_BUF];
        if (argc != 3) {
            printf("用法: SMP HDR <hex≤%u>（4 行 × 120B = %uB header）\n", (unsigned)SMP_LINE_MAX,
               (unsigned)SMP_HDR_LEN);
            return 1;
        }
        if (!smp_hex_decode(argv[2], buf, sizeof buf, &n)) return smp_err(ESP_ERR_INVALID_SIZE, false);
        esp_err_t err = sloop_store_sample_hdr_append(buf, n);
        if (err != ESP_OK) return smp_err(err, false);
        uint32_t take = s_smp_hdr_seen < SMP_HDR_FIXED ? SMP_HDR_FIXED - s_smp_hdr_seen : 0u;
        if (take > n) take = n;
        if (take) memcpy(s_smp_fixed + s_smp_hdr_seen, buf, take);
        s_smp_hdr_seen += n;
        printf("SMP HDR n=%u total=%u OK\n", (unsigned)n, (unsigned)s_smp_hdr_seen);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "END") == 0) {
        uint32_t slot;
        if (argc != 3 || !eng_uint(argv[2], SLOOP_STORE_SAMPLES - 1u, &slot)) {
            printf("用法: SMP END <slot=0..%u>\n", (unsigned)(SLOOP_STORE_SAMPLES - 1u));
            return 1;
        }
        if (s_smp_open_slot != slot) return smp_err(ESP_ERR_INVALID_STATE, false);
        esp_err_t err = smp_block_flush((uint8_t)slot);
        if (err != ESP_OK) {
            smp_session_reset(SMP_UNUSED); /* 同上：余量没落盘，会话结束，重试重新 BEGIN */
            return smp_err(err, false);
        }
        err = sloop_store_sample_end(slot);
        smp_session_reset(SMP_UNUSED);
        if (err != ESP_OK) return smp_err(err, true);
        printf("SMP END slot=%u OK\n", (unsigned)slot);
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "ERASE") == 0) {
        uint32_t slot;
        app_state_t st;
        if (argc != 3 || !eng_uint(argv[2], SLOOP_STORE_SAMPLES - 1u, &slot)) {
            printf("用法: SMP ERASE <slot=0..%u>\n", (unsigned)(SLOOP_STORE_SAMPLES - 1u));
            return 1;
        }
        /* 与 SAMPLE 页的 ERASE chip 同一条状态层命令（不是直接调存储层）。 */
        edit_cmd_t cmd = {.type = CMD_SAMPLE_ERASE, .a = slot};
        state_apply(&cmd);
        state_get_copy(&st);
        switch (st.synth_status) {
        case APP_SYNTH_STATUS_OK:
            /* 只有擦除真的落地才结束本槽的上传会话：被拒（走带中/冻结）时 adapter 的
             * open slot 仍可重试（END 的 CRC 失败重发路径），console 侧不得抢先清掉。 */
            if (s_smp_open_slot == slot) smp_session_reset(SMP_UNUSED);
            printf("SMP ERASE slot=%u OK\n", (unsigned)slot);
            return 0;
        case APP_SYNTH_STATUS_BLOCKED: return smp_err(ESP_ERR_INVALID_STATE, false);
        case APP_SYNTH_STATUS_FROZEN:
            printf("SMP ERR frozen\n");
            return 1;
        case APP_SYNTH_STATUS_INVALID:
            printf("SMP ERR state\n");
            return 1;
        default:
            printf("SMP ERR %s\n", sloop_store_ready() ? "flash" : "no_storage");
            return 1;
        }
    }
    printf("用法: SMP INFO | BEGIN <slot=0..%u> | DATA <slot> <off≥%u> <hex≤%u> | "
           "HDR <hex≤%u> | END <slot> | ERASE <slot>\n",
           (unsigned)(SLOOP_STORE_SAMPLES - 1u), (unsigned)SLOOP_STORE_SAMPLE_DATA,
           (unsigned)SMP_LINE_MAX, (unsigned)SMP_LINE_MAX);
    return 1;
}

static const char *const SYNTH_STATUS_NAMES[] = {"idle", "ok", "invalid", "full",
                                                 "blocked", "frozen", "locked", "error"};

static const char *synth_status_name(uint8_t status)
{
    return status < sizeof(SYNTH_STATUS_NAMES) / sizeof(SYNTH_STATUS_NAMES[0])
               ? SYNTH_STATUS_NAMES[status]
               : "?";
}

/* 状态层 synth_status → spec §4.5 的错误令牌（FULL 本阶段无来源，归入 error）。
 * who 是命令族（"DSYN"/"FM6P"），令牌按 spec 的 `DSYN/FM6P ERR <…>` 回显。 */
static int synth_err(const char *who, uint8_t status)
{
    const char *token = "error";
    switch (status) {
    case APP_SYNTH_STATUS_INVALID: token = "invalid"; break;
    case APP_SYNTH_STATUS_LOCKED: token = "locked"; break;
    case APP_SYNTH_STATUS_FROZEN: token = "frozen"; break;
    case APP_SYNTH_STATUS_BLOCKED: token = "playing"; break;
    default: break;
    }
    printf("%s ERR %s\n", who, token);
    return 1;
}

static void dsyn_fields_print(const uint8_t sound[22])
{
    printf("f=");
    for (uint32_t i = 0; i < SLOOP_DSYN_FIELDS; i++)
        printf("%s%d", i ? "," : "", (int)sloop_dsyn_field_from_sound(sound, i));
}

static int dsyn_info(void)
{
    app_state_t st;
    state_get_copy(&st);
    printf("DSYN INFO kit=%u lane=%u name=%s crush=%u src=%u dirty=%u ",
           (unsigned)st.dsyn.kit, (unsigned)st.dsyn.lane, st.dsyn.name, (unsigned)st.dsyn.crush,
           (unsigned)st.dsyn.src, (unsigned)st.dsyn.dirty);
    dsyn_fields_print(st.dsyn.sound);
    printf("\n");
    return 0;
}

static int dsyn_get(uint32_t kit, uint32_t lane)
{
    sloop_dsyn_sound_t snd;
    char name[SLOOP_DSYN_NAME + 1];
    uint8_t crush = 0, src = 0;
    esp_err_t err = sloop_core_dsyn_read(kit, lane, &snd, name, &crush, &src);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            printf("DSYN ERR frozen\n");
            return 1;
        }
        printf("DSYN ERR invalid\n");
        return 1;
    }
    printf("DSYN GET k=%u l=%u name=%s crush=%u src=%u dirty=%u sound=", (unsigned)kit,
           (unsigned)lane, name, (unsigned)crush, (unsigned)src,
           sloop_store_dsu_dirty() ? 1u : 0u);
    store_hex(snd.sound, sizeof snd.sound);
    printf("\n");
    return 0;
}

/* 一次状态层命令后回读 synth_status：DSYN/FM6P 的写入口共用。 */
static uint8_t synth_apply(const edit_cmd_t *cmd)
{
    app_state_t st;
    state_apply(cmd);
    state_get_copy(&st);
    return st.synth_status;
}

static int dsyn_command(int argc, char **argv)
{
    if (argc >= 2 && strcasecmp(argv[1], "INFO") == 0 && argc == 2) return dsyn_info();
    if (argc >= 2 && strcasecmp(argv[1], "GET") == 0) {
        uint32_t kit, lane;
        if (argc != 4 || !eng_uint(argv[2], SLOOP_DSYN_KITS - 1u, &kit) ||
            !eng_uint(argv[3], SLOOP_DSYN_LANES - 1u, &lane))
            goto usage;
        return dsyn_get(kit, lane);
    }
    if (argc >= 2 && strcasecmp(argv[1], "SET") == 0) {
        uint32_t kit, lane, field;
        int32_t value;
        if (argc != 6 || !eng_uint(argv[2], SLOOP_DSYN_KITS - 1u, &kit) ||
            !eng_uint(argv[3], SLOOP_DSYN_LANES - 1u, &lane) ||
            !eng_uint(argv[4], SLOOP_DSYN_FIELDS - 1u, &field) ||
            !seq_int(argv[5], -32768, 32767, &value))
            goto usage;
        edit_cmd_t cmd = {.type = CMD_DSYN_SET, .a = kit, .b = lane, .n = field, .v = (uint16_t)value};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("DSYN", status);
        printf("DSYN SET k=%u l=%u f=%u value=%d status=\"%s\"\n", (unsigned)kit, (unsigned)lane,
               (unsigned)field, (int)sloop_core_dsyn_field(kit, lane, field), synth_status_name(status));
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "KIT") == 0) {
        uint32_t kit, src;
        if (argc != 4 || !eng_uint(argv[2], SLOOP_DSYN_KITS - 1u, &kit) ||
            !eng_uint(argv[3], SLOOP_DSYN_FACTORY_KITS - 1u, &src))
            goto usage;
        edit_cmd_t cmd = {.type = CMD_DSYN_LOAD_KIT, .a = kit, .b = src};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("DSYN", status);
        printf("DSYN KIT k=%u src=%u status=\"%s\"\n", (unsigned)kit, (unsigned)src,
               synth_status_name(status));
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "STORE") == 0 && argc == 2) {
        edit_cmd_t cmd = {.type = CMD_DSYN_STORE};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("DSYN", status);
        printf("DSYN STORE dirty=%u status=\"%s\"\n", sloop_store_dsu_dirty() ? 1u : 0u,
               synth_status_name(status));
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "CRUSH") == 0) {
        /* kit 级 CRUSH（0..255）：状态层命令 CMD_DSYN_CRUSH（与 KIT 页旋钮同一路径），
         * 范围在解析处先卡住（越界 = 用法/取值错误，不下发）。 */
        uint32_t kit, crush;
        if (argc != 4 || !eng_uint(argv[2], SLOOP_DSYN_KITS - 1u, &kit) ||
            !eng_uint(argv[3], 255, &crush))
            goto usage;
        edit_cmd_t cmd = {.type = CMD_DSYN_CRUSH, .a = kit, .v = (uint16_t)crush};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("DSYN", status);
        /* 回读**目标 kit**（不是选中 kit 的投影）：`DSYN CRUSH 2 40` 后投影仍停在
         * 原来的选择上，读 st.dsyn.crush 会回一个与该命令无关的值。 */
        uint8_t applied = 0;
        if (sloop_core_dsyn_read(kit, 0u, NULL, NULL, &applied, NULL) != ESP_OK) {
            printf("DSYN ERR error\n");
            return 1;
        }
        printf("DSYN CRUSH k=%u crush=%u status=\"%s\"\n", (unsigned)kit, (unsigned)applied,
               synth_status_name(status));
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "AUDITION") == 0) {
        uint32_t kit, lane, vel = APP_STATE_VEL_DEFAULT;
        if ((argc != 4 && argc != 5) || !eng_uint(argv[2], SLOOP_DSYN_KITS - 1u, &kit) ||
            !eng_uint(argv[3], SLOOP_DSYN_LANES - 1u, &lane) ||
            (argc == 5 && (!eng_uint(argv[4], 127, &vel) || vel == 0)))
            goto usage;
        edit_cmd_t cmd = {.type = CMD_DSYN_AUDITION, .a = kit, .b = lane, .v = (uint16_t)vel};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("DSYN", status);
        printf("DSYN AUDITION k=%u l=%u vel=%u status=\"%s\"\n", (unsigned)kit, (unsigned)lane,
               (unsigned)vel, synth_status_name(status));
        return 0;
    }
usage:
    printf("用法: DSYN INFO | GET <kit=0..3> <lane=0..15> | SET <kit> <lane> <field=0..23> <value> | "
           "KIT <kit> <src> | CRUSH <kit> <0..255> | STORE | AUDITION <kit> <lane> [vel=1..127]\n");
    return 1;
}

/* FM6P：选中轨（或缺省参数）的 155B live patch。锁定的非 FM6 合成轨仍可读（engine_ok=0），
 * 鼓轨/越界轨没有 live patch ⇒ invalid。ALG 的语义见 fm6p_alg()。 */
static esp_err_t fm6p_snapshot(uint32_t track, sloop_fm6_patch_snapshot_t *out)
{
    return sloop_core_fm6_patch(track, out);
}

static int fm6p_err(esp_err_t ret)
{
    if (ret == ESP_ERR_INVALID_STATE) {
        printf("FM6P ERR frozen\n");
        return 1;
    }
    if (ret == ESP_ERR_NOT_ALLOWED) {
        printf("FM6P ERR locked\n");
        return 1;
    }
    printf("FM6P ERR invalid\n");
    return 1;
}

static int fm6p_info(uint32_t track)
{
    sloop_fm6_patch_snapshot_t s;
    esp_err_t ret = fm6p_snapshot(track, &s);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_ALLOWED) return fm6p_err(ret);
    char name[SLOOP_FM6_NAME_FIELDS + 1];
    for (uint32_t i = 0; i < SLOOP_FM6_NAME_FIELDS; i++) {
        /* 走公开的纯解码入口（op 6 的 19..28 = 名字字节），不硬编码 FP_NAME 的绝对偏移 */
        int c = sloop_fm6_field_from_patch(s.patch, SLOOP_FM6_GLOBAL_OP, FM6P_NAME_FIELD0 + i);
        name[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    name[SLOOP_FM6_NAME_FIELDS] = '\0';
    for (int i = SLOOP_FM6_NAME_FIELDS - 1; i >= 0 && name[i] == ' '; i--) name[i] = '\0';
    int16_t p[SLOOP_CORE_P_COUNT];
    sloop_core_read_params(track, p, NULL);
    printf("FM6P INFO track=%u slot=%u engine_ok=%u name=\"%s\" alg=%d macro=%u\n", (unsigned)track,
           (unsigned)s.slot, (unsigned)s.engine_ok, name,
           (int)sloop_fm6_field_from_patch(s.patch, SLOOP_FM6_GLOBAL_OP, 8),
           (unsigned)p[CONSOLE_P_E0 /* ALG 宏，0 = PAT */]);
    return 0;
}

static int fm6p_get(uint32_t track)
{
    sloop_fm6_patch_snapshot_t s;
    esp_err_t ret = fm6p_snapshot(track, &s);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_ALLOWED) return fm6p_err(ret);
    printf("FM6P GET track=%u slot=%u engine_ok=%u ", (unsigned)track, (unsigned)s.slot,
           (unsigned)s.engine_ok);
    store_hex(s.patch, sizeof s.patch);
    printf("\n");
    return 0;
}

/* FM6P ALG <track> <0..32>（spec §4.1「ALG 宏覆盖」）：
 *   0      = PAT：只把 P_E0（ALG 宏）置 0，patch 的 FP_ALG 不动（音色回到深度页的值）；
 *   1..32  = patch 的 ALG 写 value-1，并在同一次引擎锁内清 P_E0=0（ADD 的 CMD_FM6_SET
 *            语义），写完立即听得到。 */
static int fm6p_alg(uint32_t track, uint32_t value)
{
    sloop_fm6_patch_snapshot_t s;
    esp_err_t ret = fm6p_snapshot(track, &s);
    if (ret == ESP_ERR_NOT_ALLOWED || (ret == ESP_OK && !s.engine_ok)) {
        printf("FM6P ERR locked\n");
        return 1;
    }
    if (ret != ESP_OK) return fm6p_err(ret);
    if (value == 0) {
        /* PAT：CMD_SET_PARAM 不写 synth_status，故按写后回读的 P_E0 派生结果。冻结时
         * read_params 返回全零（看起来「写成功」），所以前后各查一次冻结。 */
        if (sloop_seq_frozen()) return fm6p_err(ESP_ERR_INVALID_STATE);
        edit_cmd_t cmd = {.type = CMD_SET_PARAM, .a = track, .b = CONSOLE_P_E0, .v = 0};
        state_apply(&cmd);
    } else {
        edit_cmd_t cmd = {.type = CMD_FM6_SET, .a = track, .b = SLOOP_FM6_GLOBAL_OP,
                          .n = 8, .v = (uint16_t)(value - 1u)};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("FM6P", status);
    }
    int16_t p[SLOOP_CORE_P_COUNT];
    sloop_core_read_params(track, p, NULL);
    if (sloop_seq_frozen()) return fm6p_err(ESP_ERR_INVALID_STATE);
    if (p[CONSOLE_P_E0] != 0) {
        printf("FM6P ERR error\n"); /* P_E0 没被清掉：写被丢弃（不应发生） */
        return 1;
    }
    printf("FM6P ALG track=%u value=%u alg=%d macro=%u status=\"%s\"\n", (unsigned)track,
           (unsigned)value, (int)sloop_core_fm6_field(track, SLOOP_FM6_GLOBAL_OP, 8),
           (unsigned)p[CONSOLE_P_E0], synth_status_name(APP_SYNTH_STATUS_OK));
    return 0;
}

static int fm6p_command(int argc, char **argv)
{
    sloop_core_snapshot_t snap;
    sloop_core_read(&snap);
    uint32_t selected = snap.selected_track < APP_STATE_TRACKS ? snap.selected_track : 0u;
    if (argc >= 2 && strcasecmp(argv[1], "INFO") == 0) {
        uint32_t track = selected;
        if (argc == 3 && !eng_uint(argv[2], APP_STATE_TRACKS - 1u, &track)) goto usage;
        if (argc > 3) goto usage;
        return fm6p_info(track);
    }
    if (argc >= 2 && strcasecmp(argv[1], "GET") == 0) {
        uint32_t track = selected;
        if (argc == 3 && !eng_uint(argv[2], APP_STATE_TRACKS - 1u, &track)) goto usage;
        if (argc > 3) goto usage;
        return fm6p_get(track);
    }
    if (argc >= 2 && strcasecmp(argv[1], "SET") == 0) {
        uint32_t track, op, field;
        int32_t value;
        if (argc != 6 || !eng_uint(argv[2], APP_STATE_TRACKS - 1u, &track) ||
            !eng_uint(argv[3], SLOOP_FM6_GLOBAL_OP, &op) ||
            !eng_uint(argv[4], (op == SLOOP_FM6_GLOBAL_OP ? SLOOP_FM6_GLOBAL_FIELDS
                                                          : SLOOP_FM6_OP_FIELDS) - 1u,
                      &field) ||
            !seq_int(argv[5], -32768, 32767, &value))
            goto usage;
        edit_cmd_t cmd = {.type = CMD_FM6_SET, .a = track, .b = op, .n = field,
                          .v = (uint16_t)value};
        uint8_t status = synth_apply(&cmd);
        if (status != APP_SYNTH_STATUS_OK) return synth_err("FM6P", status);
        printf("FM6P SET track=%u op=%u field=%u value=%d status=\"%s\"\n", (unsigned)track,
               (unsigned)op, (unsigned)field, (int)sloop_core_fm6_field(track, op, field),
               synth_status_name(status));
        return 0;
    }
    if (argc >= 2 && strcasecmp(argv[1], "ALG") == 0) {
        uint32_t track, value;
        if (argc != 4 || !eng_uint(argv[2], APP_STATE_TRACKS - 1u, &track) ||
            !eng_uint(argv[3], 32, &value))
            goto usage;
        if (track >= APP_STATE_TRACKS - 1u) return fm6p_err(ESP_ERR_NOT_ALLOWED); /* 鼓轨无 patch */
        return fm6p_alg(track, value);
    }
usage:
    printf("用法: FM6P INFO [track=0..3] | GET [track] | SET <track> <op=0..6（6=全局）> "
           "<field> <value> | ALG <track> <0..32>（0 = PAT：只清 P_E0；1..32 = patch ALG "
           "= value-1 并清 P_E0）\n");
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
        .help = "状态层自检：STATE SNAP [FULL] 打印 AppState（FULL 含详细步；引擎真值与 ENG SNAP 同源）",
        .hint = "SNAP [FULL]",
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
        .hint = "<play|tracks|step|transport|global|fx|analog|fm6|sample|dsyn|fm6p|projects|presets|song>|MEM|KNOB <slot> <press|drag> <dx> <dy>|FORCEFAIL <page> [on|off]|QUIESCE <on|off>",
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

    const esp_console_cmd_t seq_cmd = {
        .command = "SEQ",
        .help = "音序诊断：DSTEP/MICRO/FILL/LOCK；EDIT 音符工具；PATTERN 工具与 undo/redo；"
                "REC/HIT 录音与鼓演奏；SECTION/CHAIN/SONG/SREC arranger（live section、"
                "quick chain、SONG 模式与 SONG REC）",
        .hint = "DSTEP [step]|MICRO/FILL <track> <step> <v>|LOCK ...|EDIT <op> <track> <step> [args]|"
                "PATTERN <op> <track> [arg]|REC ...|HIT <lane> [vel]|SECTION [<A-D> STORE|LOAD|ERASE|REQUEST]|"
                "CHAIN [<A-D>…]|SONG [MODE song|loop|REC]|SREC [TOGGLE]",
        .func = &seq_command,
    };
    ret = esp_console_cmd_register(&seq_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 SEQ 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t smp_cmd = {
        .command = "SMP",
        .help = "采样槽上传/诊断（USR1..USR4）：SMP INFO 槽状态；BEGIN/DATA/HDR/END 分块上传"
                "（scripts/sample_upload.py 对端）；ERASE 走状态层 CMD_SAMPLE_ERASE",
        .hint = "INFO|BEGIN <0..3>|DATA <slot> <off≥512> <hex≤240>|HDR <hex≤240>|END <slot>|ERASE <slot>",
        .func = &smp_command,
    };
    ret = esp_console_cmd_register(&smp_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 SMP 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t dsyn_cmd = {
        .command = "DSYN",
        .help = "DSYN 鼓音色诊断（P4-D）：INFO/GET 只读投影；SET/KIT/CRUSH/STORE/AUDITION 走"
                "状态层命令（与 DSYN 页同一路径）",
        .hint = "INFO|GET <kit=0..3> <lane=0..15>|SET <kit> <lane> <field=0..23> <value>|"
                "KIT <kit> <src>|STORE|AUDITION <kit> <lane> [vel=1..127]",
        .func = &dsyn_command,
    };
    ret = esp_console_cmd_register(&dsyn_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 DSYN 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t fm6p_cmd = {
        .command = "FM6P",
        .help = "FM6 155B patch 诊断（P4-D）：INFO/GET 只读 live patch；SET 走 CMD_FM6_SET；"
                "ALG 0 = PAT（只清 P_E0），1..32 = patch ALG = value-1 并清 P_E0",
        .hint = "INFO [track=0..3]|GET [track]|SET <track> <op=0..6> <field> <value>|"
                "ALG <track> <0..32>（0=PAT）",
        .func = &fm6p_command,
    };
    ret = esp_console_cmd_register(&fm6p_cmd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 FM6P 命令失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_console_cmd_t store_cmd = {
        .command = "STORE",
        .help = "存储/工程诊断：STORE STATS 后端与对象状态；STORE PROJ LIST|SAVE n|LOAD n|ERASE n|HASH n；"
                "STORE PROJ SYNTH <fmt1|foreign> 触发 project_load 的两条 fallback（诊断，覆盖槽 3）；"
                "STORE BANK LIST|PUT n|GET n|LIVE|ERASE n（FM6 银行 27 槽，PTCH = 8 + n）；"
                "STORE LIVEHASH 当前工程捕获 hash；STORE RAWTEST 空闲扇区 0xFB000 擦写校验（破坏性）；"
                "STORE TEST 设备 torn-write 套件（破坏性：占用 OBJ_SETTINGS，快照/恢复两个完整 4KB "
                "副本扇区并逐字节校验，arrangement/DSU/尾段/side 都不丢；需停走带）；"
                "STORE SONG [SET c l s:b…|SAVE|LOAD|INFO] PER3 歌曲顺序（SET 只改内存、按门限落盘）；"
                "STORE AUTO on|off 自动存档开关（默认 on）；STORE FAULT <nth> [half]|CLEAR 故障注入",
        .hint = "STATS|PROJ LIST|PROJ SAVE n|PROJ LOAD n|PROJ ERASE n|PROJ HASH n|PROJ SYNTH <fmt1|foreign>|BANK LIST|BANK PUT n|BANK GET n|BANK LIVE|BANK ERASE n|SONG [SET c l s:b…|SAVE|LOAD|INFO]|LIVEHASH|RAWTEST|TEST|AUTO on|off|FAULT <nth> [half]|FAULT CLEAR",
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
