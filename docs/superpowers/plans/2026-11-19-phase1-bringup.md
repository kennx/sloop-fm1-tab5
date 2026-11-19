# 阶段 1：Tab5 平台 bring-up 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 Tab5 变成「处处可观测」的目标机——显示/触摸/键盘/音频/截屏/分区全部就位并过门禁，**不含任何 SLOOP 代码**（roadmap §3）。

**Architecture:** 全部经 BSP 公开 API 驱动板载外设；显示用 `bsp_display_new_with_handles()` + esp_lvgl_port 手工组装（为 SCR 拿到 DPI panel 句柄）；键盘为独立 I2C 总线上的自写组件 `tab5_keyboard`；console 切到 USB-Serial-JTAG 并注册 `SCR`/`AUD`/`KB` 命令构成验证通道。

**Tech Stack:** ESP-IDF v5.5.5 / esp32p4 / espressif/m5stack_tab5 BSP ^1.3.3（esp_lvgl_port + esp_codec_dev）/ Python 3 + pyserial + Pillow（screenshot.py）。

**Spec:** `docs/superpowers/specs/2026-11-19-sloop-tab5-port-design.md`（§5 架构、§7 阶段 1 要点、§10 假设 A1）；阶段主计划 `docs/design/002-port-roadmap.md` §3（P1-1…P1-7 与完成判据）。执行本计划前两份都要读。

## Global Constraints

- 构建/烧录/截屏只走 `scripts/`（build.sh / flash.sh / verify.sh），不手敲 `idf.py` 长命令（AGENTS §5）。
- 固件门禁：`scripts/build.sh` 退出码 0 且**不新增**编译警告（kconfig 重生成后的首个 configure pass 有一对已知 CMake 提示，豁免；按第二次构建判定，AGENTS §5.5）；烧录后 `Hash of data verified`、启动无 fatal/abort/backtrace。
- UI 门禁（本阶段起生效）：截图先落 `firmware/build/verify/`，门禁证据晋升 `docs/dev/verify/1-bringup-<日期>/` 并登记 `docs/dev/verification-log.md`（AGENTS §5.4）。
- `scripts/` 改动门禁：`python3 scripts/screenshot.py --selftest` 退出码 0。
- 不修改 `firmware/managed_components/`（只读参考）；不自写面板初始化序列（屏幕复位时序坑，tab5-hardware §6.1）。
- 硬件常量（本机批次，device-notes §1）：P4 rev v1.3；ST7121 批次（BSP 运行时探测，日志确认）；键盘独立 I2C G0(SDA)/G1(SCL) @**0x6D**、INT=**G50**；音频 ES8388 @0x10（BSP 代管）；采样率 **44100**。
- 提交格式按 AGENTS §7：`<type>(<模块路径>): <中文摘要>` + `[Motivation]/[Change_Set]/[Impact_AST]/[Infra_State]` 标签体。
- 代码风格随仓库现状（esp_log、4 空格缩进、中文块注释）。

## Review Focus（本阶段最可能咬人的五类，测试已钉进对应任务）

1. **SCR 二进制流被并发日志污染**（二进制阶段任何 ESP_LOG 插入都会破坏 PNG）→ Task 3 Step 5 传输期静默日志 + 双次截屏逐字节一致测试。
2. **触摸坐标旋转/镜像错误**（坐标系与显示方向不一致）→ Task 5 Step 4 四角+中心触点与截图准星位置一致。
3. **44.1k/48k 采样率错配**（整体音高偏 ~8%）→ Task 7 Step 4 codec open 参数核验 + 440Hz 听感双重校验。
4. **键盘 I2C 误用板载总线**（G31/G32 上无 0x6D，且会干扰其他设备）→ Task 6 Step 3 独立 `i2c_master` 总线（G0/G1），探测命令先于驱动。
5. **SCR 暂存缓冲挤爆内存**（1.8MB + LVGL 帧缓冲/绘制缓冲）→ Task 3 Step 3 缓冲入 PSRAM + 前后剩余堆水位日志对比。

---

## 文件结构（本计划落地后的新增/修改）

```
firmware/
├── partitions.csv                        # 新建（Task 1）
├── sdkconfig.defaults                    # 修改（Task 1 分区；Task 3 console 通道）
├── main/
│   ├── main.c                            # 修改：启动序列编排（display/console 逐任务接入）
│   ├── display.h / display.c             # 新建：显示组装（导出 DPI panel 句柄）+ 测试图 + 触摸准星
│   ├── console.h / console.c             # 新建：console REPL + SCR/AUD/KB 命令注册
│   └── audio_tone.h / audio_tone.c       # 新建：扬声器正弦音（esp_codec_dev）
└── components/
    └── tab5_keyboard/                    # 新建组件（Task 6）
        ├── CMakeLists.txt
        ├── include/tab5_keyboard.h
        └── src/tab5_keyboard.c
scripts/screenshot.py                     # 修改：--check 像素抽检 + selftest 扩展（Task 4）
docs/dev/verify/1-bringup-<日期>/          # 新建：晋升证据（Task 8）
docs/dev/device-notes.md                  # 修改：键盘 §2 → ✅（Task 8）
docs/design/002-port-roadmap.md           # 修改：阶段 1 状态（Task 8）
AGENTS.md                                 # 修改：§3 目录规划 tab5_keyboard 落地（Task 8）
```

职责边界：`display.c` 只管显示与屏上反馈物；`console.c` 只管命令注册与传输（SCR 取帧调 display 句柄、AUD 调 audio_tone）；`audio_tone.c` 只管出声；键盘自成一个组件（阶段 3 InputManager 消费）。

---

### Task 0: 基线与分支

**Files:** 无（只读验证 + git 分支）

- [ ] **Step 1: 从 dev 拉 feature 分支**

```bash
git checkout dev && git pull && git checkout -b feat/bringup
```

- [ ] **Step 2: 基线构建**

Run: `scripts/build.sh`
Expected: 退出码 0，输出无 warning/error（这是「零新增警告」的比较基线）。

### Task 1: P1-1 分区表（单 factory app 8MB + FATFS ~7.8MB + NVS）

**Files:**
- Create: `firmware/partitions.csv`
- Modify: `firmware/sdkconfig.defaults`

**Interfaces:**
- Produces: 分区 `factory`(app, 0x10000, 8MB) / `storage`(data fat, 0x810000, 0x7F0000) / `nvs`(0x9000, 0x6000) / `phy_init`(0xF000, 0x1000)。后续阶段以标签 `storage` 挂 FATFS、`factory` 为 app 槽。

- [ ] **Step 1: 写 `firmware/partitions.csv`**

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xF000,   0x1000,
factory,  app,  factory, 0x10000,  0x800000,
storage,  data, fat,     0x810000, 0x7F0000,
```

（0x810000 + 0x7F0000 = 0x1000000，恰好用满 16MB；姿态依据 roadmap P1-1：无 OTA A/B。）

- [ ] **Step 2: sdkconfig.defaults 追加自定义分区表开关**

```
# 分区表（roadmap P1-1：单 factory app + FATFS 资产 + NVS）
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"
```

- [ ] **Step 3: 构建两次并烧录验证**

Run: `scripts/build.sh && scripts/build.sh`（第一次过 kconfig 重生成的豁免 pass）→ 第二次零新增警告。
Run: `scripts/flash.sh`
Expected: esptool 输出 `Hash of data verified`；启动日志出现自定义分区表条目（`factory` 0x10000 / `storage` 0x810000）、无 fatal/abort/backtrace。

- [ ] **Step 4: Commit**

```
feat(firmware): 定稿分区表（单 factory app 8MB + FATFS 资产分区）

[Motivation] 默认分区表 app 上限 1MB，阶段 2 引擎与阶段 3 LVGL UI 无法容纳（roadmap P1-1）。
[Change_Set] 新建 firmware/partitions.csv；编辑 firmware/sdkconfig.defaults。
[Impact_AST] 链接地址与 app 体积上限变化；无代码接口变化。
[Infra_State] flash 布局变更：烧录后分区为 nvs/phy_init/factory(8MB)/storage(FATFS 0x7F0000)。
```

### Task 2: P1-2 显示点亮 + 测试图

**Files:**
- Create: `firmware/main/display.h`, `firmware/main/display.c`
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`（SRCS 加 display.c）

**Interfaces:**
- Produces:
```c
/* display.h */
esp_err_t app_display_init(void);            /* 组装 DPI panel + LVGL；成功后内部持有 panel 句柄 */
esp_lcd_panel_handle_t app_display_panel(void); /* 供 console SCR 取帧缓冲（Task 3 消费） */
```
- 依赖（`main/CMakeLists.txt` 的 `idf_component_register` 加 `PRIV_REQUIRES espressif__m5stack_tab5 esp_lvgl_port esp_lcd`；managed 组件以 `espressif__` 前缀名为依赖名）。
- 组装方式：`bsp_display_new_with_handles()`（`bsp/display.h:119`，返回 `bsp_lcd_handles_t.panel`）+ `lvgl_port_add_disp()`，镜像 BSP `bsp_display_start_with_config` 的内部流程（`managed_components/espressif__m5stack_tab5/src/bsp_display.c`），因为 `bsp_display_start()` 不外露 panel 句柄。

- [ ] **Step 1: 实现 `app_display_init()`**

要点：`bsp_display_cfg_t` 取 buffer_size=面板 10% 行（`BSP_LCD_H_RES * 30` 像素级即可，参照 BSP 默认）、double_buffer=true、buff_spiram=1；`lvgl_port_init` + `lvgl_port_add_disp`（io 与 panel 用 handles 结构体成员）；`bsp_display_backlight_on()` 收尾。启动日志确认行 `Discovered board version 3 (LCD ST7121` —— 找不到即停（批次探测失败，回 device-notes §1 排查）。

- [ ] **Step 2: 画测试图（`display.c` 内 static 函数，init 成功后调用）**

固定布局（供 Task 4 像素抽检锚定，全部 LVGL 基本对象、深底色 `0x101418`）：
- 顶带 y∈[0,240)：8 条竖直彩条，各 160px 宽，从左到右 白/黄/青/绿/品红/红/蓝/黑；
- 中带 y∈[240,480)：暗底上 左侧实心圆(r=80, 绿色)、空心方框(160px, 青色)、右对角线(黄)，加文本 `"SLOOP TAB5 BRINGUP"`（`lv_font_...` 内置等宽/默认字体即可）；
- 底带 y∈[480,720)：4 条水平条（四轨主题色 蓝 `0x3B7BD8`/绿 `0x3FA34D`/黄 `0xE0B32A`/橙 `0xE06A1B`）；
- 四角 L 形标记（白色，8px 边）验证无裁切。
所有绘制包在 `bsp_display_lock(0)` … `bsp_display_unlock()` 内。

- [ ] **Step 3: 构建烧录，日志验证**

Run: `scripts/build.sh && scripts/flash.sh --monitor`，观察后退出（Ctrl+]）。
Expected: ST7121 探测日志行出现；背光点亮、屏幕显示测试图（此时人眼可先看）；无 fatal/abort。视觉门禁在 Task 3（SCR）落地后正式闭环。

- [ ] **Step 4: Commit**

`feat(firmware/main): 点亮 MIPI-DSI 显示并输出 bring-up 测试图`（标签体按 AGENTS §7，下同，不再逐条展开）。

### Task 3: P1-3a SCR 截屏命令（console 通道 + 帧导出）

**Files:**
- Create: `firmware/main/console.h`, `firmware/main/console.c`
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`, `firmware/sdkconfig.defaults`
- 依赖：`main/CMakeLists.txt` 的 `idf_component_register` 加 `PRIV_REQUIRES console driver usb_serial_jtag esp_lcd`（esp_timer 已随 console 传递）。

**Interfaces:**
- Consumes: `app_display_panel()`（Task 2）。
- Produces: `esp_err_t app_console_init(void);`（main.c 启动时调用；注册 `SCR` 命令；Task 5/7 继续在此追加 `KB`/`AUD`）。
- 协议（AGENTS §5.3，screenshot.py 已实现 PC 侧）：PC 发 `SCR\n` → 回 `SCR 1280 720 rgb565 1843200\n` → 裸帧（小端 rgb565）→ `SCR OK\n`。

- [ ] **Step 1: console 主通道切到 USB-Serial-JTAG**

`firmware/sdkconfig.defaults` 追加：
```
# console 主通道走 USB-Serial-JTAG（SCR/注入命令与日志同在 /dev/ttyACM0）
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```
（choice 切换后 UART_DEFAULT 自动失选；构建后 `grep ESP_CONSOLE_USB_SERIAL_JTAG build/config/sdkconfig` 应为 `=y` 且 UART_DEFAULT 不再置位。）

- [ ] **Step 2: REPL 初始化**

`app_console_init()`：`esp_console_repl_config_t`（prompt `"sloop> "`、history 16）；`esp_console_new_repl_usb_serial_jtag()`；**先注册命令后 `esp_console_start_repl()`**。

- [ ] **Step 3: 实现 SCR 命令**

流程（命令回调内）：
1. `bsp_display_lock(0)` 暂停 LVGL 刷新（§5.3「暂停刷新」取稳帧策略）；
2. `vTaskDelay(pdMS_TO_TICKS(40))`（≥2 帧，等潜在 flush 收尾）；
3. `esp_lcd_dpi_panel_get_frame_buffer(app_display_panel(), 1, &fb)`（默认 `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=1`，单帧缓冲）；
4. `heap_caps_malloc(1843200, MALLOC_CAP_SPIRAM)` 暂存并 memcpy，随后立刻 `bsp_display_unlock()`（不长时间锁屏）；
5. 静默日志 `esp_log_level_set("*", ESP_LOG_NONE)`；`printf` 头行 → `fwrite` 裸数据到 stdout → `fflush` → 恢复 `ESP_LOG_INFO`，打印 `SCR OK`；
6. 释放暂存缓冲。
记录传输前后 `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` 日志（Review Focus 5）。
失败路径：panel 句柄为空 / malloc 失败 → 打印 `SCR ERR <原因>` 并返回（screenshot.py 会以非零退出可见）。

- [ ] **Step 4: 构建烧录，首截屏**

Run: `scripts/build.sh && scripts/build.sh && scripts/flash.sh`
Run: `python3 scripts/screenshot.py /dev/ttyACM0`
Expected: 输出 `帧缓冲: 1280x720 rgb565 (1843200 字节)`、`已保存: …/build/verify/screenshot-*.png`。

- [ ] **Step 5: 视觉检查 + 一致性测试（阶段 1 首次 UI 门禁）**

用视觉能力读取该 PNG：彩条颜色顺序（白黄青绿品红蓝黑）正确、几何与文本可读、四角 L 标记完整、无花屏/撕裂/错位/缺色。
再连截两张：`python3 scripts/screenshot.py /dev/ttyACM0 a.png && python3 scripts/screenshot.py /dev/ttyACM0 b.png && cmp a.png b.png`
Expected: `cmp` 无差异（静态画面下传输无损坏 → 日志静默有效，Review Focus 1）。

- [ ] **Step 6: Commit**

`feat(firmware/main): 落地 SCR 帧缓冲截屏命令（AGENTS §5.3 协议）`

### Task 4: P1-3b screenshot.py 像素抽检 `--check`（离线 TDD）

**Files:**
- Modify: `scripts/screenshot.py`

**Interfaces:**
- Produces: CLI `--check X,Y,R,G,B[,TOL]`（可多次给出，全部通过退出码 0，任一失败打印 实测值/期望值 并退出码 1；TOL 默认 16，容忍位扩展误差）；内部函数 `check_pixel(img, x, y, r, g, b, tol) -> tuple[bool, str]`（返回 (是否通过, 人类可读差异)）。

- [ ] **Step 1: 写失败的 selftest 断言**

在 `run_selftest()` 前新增 `_check_pixel_checks()` 并调用：构造 4×2 合成图（复用 `_W565`/`_EXP565`），断言 `check_pixel(img, 0, 0, 248, 0, 0, 16)` 通过；`check_pixel(img, 1, 0, 10, 200, 10, 16)` 失败且差异文本含 `10`；越界坐标 (9,9) 返回失败而非异常。

- [ ] **Step 2: 跑 selftest 确认失败**

Run: `python3 scripts/screenshot.py --selftest`
Expected: `selftest FAILED: check_pixel 未定义`（或 AttributeError）。

- [ ] **Step 3: 实现 `check_pixel` + `--check` 参数**

argparse 加 `ap.add_argument("--check", action="append", ...)`，解析 `X,Y,R,G,B[,TOL]`；capture 成功后逐一校验。注意 `--check` 不与 `--selftest` 冲突。

- [ ] **Step 4: selftest 通过 + 真机抽检**

Run: `python3 scripts/screenshot.py --selftest`
Expected: `selftest OK`（断言组数更新）。
Run: `python3 scripts/screenshot.py /dev/ttyACM0 --check 80,120,248,248,248 --check 1040,120,0,0,248 --check 120,510,56,120,216`
（锚点：顶带白条中心 / 顶带纯蓝条中心 / 底带蓝条中心；期望值按 RGB565 位扩展后的解码值——白 (248,252,248)、纯蓝 (0,0,248)、蓝 `0x3B7BD8`→(56,120,216)。）
Expected: 退出码 0。

- [ ] **Step 5: Commit**

`feat(scripts): screenshot.py 支持已知色块像素抽检 --check`

### Task 5: P1-4 触摸（十字准星 + 坐标日志）

**Files:**
- Modify: `firmware/main/display.h`, `firmware/main/display.c`

**Interfaces:**
- Consumes: `bsp_display_get_input_dev()`（BSP 在显示组装时已初始化触摸 indev）。
- Produces: `void app_display_touch_feedback_start(void);`（init 后由 main.c 调用）。

- [ ] **Step 1: 实现准星与轮询**

LVGL 定时器（30ms）：`lv_indev_get_state == LV_INDEV_STATE_PRESSED` 时 `lv_indev_get_point` 取坐标 → 移动准星对象（两条 2px 白色细条组成的十字，`lv_layer_top` 上）→ 坐标变化 ≥8px 才 `ESP_LOGI(TAG, "touch (%d,%d)", x, y)`（限流）。松开时准星保留在最后位置（便于截屏核对）。

- [ ] **Step 2: 构建烧录 + 脚本化核对**

Run: `scripts/build.sh && scripts/flash.sh --monitor`（保持监视），依次按 触摸屏 左上/右上/左下/右下/中心 各一下。
Expected: monitor 中五组坐标日志近似 (40,40)/(1240,40)/(40,680)/(1240,680)/(640,360)（±40px）——左上必须最小、右下必须最大（无旋转/镜像，Review Focus 2）。

- [ ] **Step 3: 截屏视觉核对**

按住右下角不放的同时另开终端：`python3 scripts/screenshot.py /dev/ttyACM0`（USB-Serial-JTAG 双端点，monitor 不冲突）。
Expected: PNG 右下角出现白色准星，中心对准触点；读取 PNG 目检确认。

- [ ] **Step 4: Commit**

`feat(firmware/main): 触摸输入十字准星反馈与坐标日志`

### Task 6: P1-5 Tab5 Keyboard 组件（I2C 0x6D + 全键解码）

**Files:**
- Create: `firmware/components/tab5_keyboard/CMakeLists.txt`, `firmware/components/tab5_keyboard/include/tab5_keyboard.h`, `firmware/components/tab5_keyboard/src/tab5_keyboard.c`
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`（依赖 tab5_keyboard）、`firmware/main/console.c`（`KB` 命令）

**Interfaces:**
- Produces:
```c
typedef struct { uint8_t code; bool pressed; } tab5_kb_event_t;
esp_err_t tab5_keyboard_init(void);   /* 独立 i2c_master 总线 G0/G1，INT=G50 注册 ISR→任务通知 */
bool tab5_keyboard_probe(void);       /* 0x6D 应答探测（自检与排障用） */
int tab5_kb_poll(tab5_kb_event_t *out, int max); /* 取走积压事件，返回条数（0=无） */
const char *tab5_kb_key_name(uint8_t code);      /* 键名表（无映射返回 "??") */
```
- 权威参考（实现前必读）：M5Tab5-UserDemo 出厂固件键盘部分（https://github.com/m5stack/M5Tab5-UserDemo ，同为 ESP-IDF）；I2C 协议 PDF 与键盘 STM32 内部固件（链接见 `docs/tab5-keyboard-hardware.md` §5）。寄存器语义/INT 触发方式（电平 vs 边沿、FIFO 深度、多键包格式）**以这两个来源为准**，不要凭猜测实现。

- [ ] **Step 1: 组件骨架 + 探测自检**

建组件（`idf_component_register(SRCS "src/tab5_keyboard.c" INCLUDE_DIRS "include")`，`REQUIRES driver`）。`tab5_keyboard_init()` 用**独立** `i2c_master_bus_config_t`（`i2c_port = -1` 自动分配端口；SDA=GPIO0、SCL=GPIO1，即键盘总线 G0/G1——**不与板载 G31/G32 共总线**，Review Focus 4；时钟 100kHz）。头文件宏：`#define TAB5_KB_I2C_ADDR 0x6D`、`TAB5_KB_I2C_SDA_GPIO 0`、`TAB5_KB_I2C_SCL_GPIO 1`、`TAB5_KB_INT_GPIO 50`。main.c 启动自检：`ESP_LOGI(TAG, "keyboard: %s", tab5_keyboard_probe() ? "OK (0x6D)" : "NOT FOUND")`。

- [ ] **Step 2: 事件解码**

按 UserDemo/协议 PDF 实现 INT→读键值→`tab5_kb_event_t` 解码（含多键同按组合包）；启动一个 FreeRTOS 任务（优先级 4，栈 3KB）等待通知并打日志 `ESP_LOGI(TAG, "KB 0x%02X %s %s", code, pressed?"down":"up", tab5_kb_key_name(code))`。

- [ ] **Step 3: 真机全矩阵核对（阶段 1 门禁 3）**

Run: `scripts/build.sh && scripts/flash.sh --monitor`
依次按下全部 70 键（逐键 down/up），再测典型组合：`Shift(a,至少 Aa)+字母`、`Ctrl+Alt+Sym 修饰组合`、`方向键四连`。
Expected: 日志 70 键逐键有 down/up 且键名正确；组合无丢键（每键独立 down/up）。保存完整 monitor transcript 到 `firmware/build/verify/kb-test.log`。
若 0x6D 无应答（假设 A1 不成立）：`device-notes` §2 登记「键盘未接/模式异常」，任务降级完成（探测+代码就位），门禁 3 摘出并记入 `docs/deferred-issues.md`。

- [ ] **Step 4: `KB` console 命令（排障入口）**

`KB` 无参：打印 probe 结果与最近事件计数；`KB ON/OFF`：开关解码日志。注册进 `console.c`。

- [ ] **Step 5: Commit**

`feat(components/tab5_keyboard): Tab5 Keyboard I2C 驱动与全键解码`

### Task 7: P1-6 音频（`AUD TONE` 正弦，44.1kHz）

**Files:**
- Create: `firmware/main/audio_tone.h`, `firmware/main/audio_tone.c`
- Modify: `firmware/main/main.c`, `firmware/main/CMakeLists.txt`, `firmware/main/console.c`

**Interfaces:**
- Produces: `esp_err_t audio_tone_init(void);`（一次初始化 codec，之后复用）与 `esp_err_t audio_tone_play(uint32_t freq_hz, uint32_t duration_ms);`（阻塞至播完）。

- [ ] **Step 1: 实现 audio_tone**

`bsp_audio_codec_speaker_init()` → 句柄存 static；`esp_codec_dev_sample_info_t fs = { .sample_rate = 44100, .channel = 2, .bits = 16 };` `esp_codec_dev_open(dev, &fs)`（**44100 是上游假设**，research §4.3；写 48000 即错——Review Focus 3）。`audio_tone_play`：int16 交错立体声正弦（查表或 `sinf` 均可，幅度 -6dBFS ≈ 16000 防 NS415B 削顶），512 帧块循环 `esp_codec_dev_write` 至 duration_ms 结束。所有返回码非 0 记 ERROR 日志。

- [ ] **Step 2: `AUD` console 命令**

`AUD TONE <hz> [ms=2000]`（hz 合法域 20..12000，域外打印用法拒绝）；注册进 `console.c`。main.c 启动时 `audio_tone_init()`，日志打印 `speaker codec open 44100Hz/2ch/16bit rc=0`。

- [ ] **Step 3: 构建烧录，听音验证（阶段 1 门禁 5）**

Run: `scripts/build.sh && scripts/flash.sh --monitor`，在 monitor 输入 `AUD TONE 440 3000`，扬声器与 3.5mm 耳机各听一次。
Expected: 启动日志 `speaker codec open 44100Hz…rc=0`；3 秒稳定纯音、起始无爆音、结束无咔哒；440Hz = 标准 A4 音高（若整体明显偏高 ≈ 半音 → 采样率错配，回查 fs 与 I2S 时钟）。

- [ ] **Step 4: Commit**

`feat(firmware/main): ES8388 扬声器正弦音输出与 AUD TONE 命令`

### Task 8: P1-7 阶段收口（门禁全跑 + 证据晋升 + 文档同步 + 合并）

**Files:**
- Create: `docs/dev/verify/1-bringup-<日期>/`（晋升证据集）
- Modify: `docs/dev/verification-log.md`, `docs/dev/device-notes.md`, `docs/design/002-port-roadmap.md`, `AGENTS.md`, `firmware/main/main.c`（清掉已完成的 TODO 注释）

- [ ] **Step 1: 全门禁复跑**

Run: `scripts/build.sh && scripts/build.sh`（零新增警告）→ `scripts/verify.sh`（构建+烧录+截屏一键绿）→ `python3 scripts/screenshot.py --selftest`。
Expected: 三项全部退出码 0；启动日志无 fatal/abort/backtrace。

- [ ] **Step 2: 证据晋升入库**

从 `firmware/build/verify/` 复制（并计算 `sha256sum`）：测试图代表截图（Task 3）、触摸核对截图（Task 5）、`kb-test.log`（Task 6）、双截屏 `cmp` 一致性记录（Task 3 Step 5）。目录 `docs/dev/verify/1-bringup-YYYY-MM-DD/`。

- [ ] **Step 3: 文档同步（AGENTS §6）**

- `verification-log.md`：逐门禁行登记（阶段 1 各判据 / 日期 / 提交号 / 证据 SHA256 / 结论）；
- `device-notes.md` §2：键盘 → ✅（含模式/固件观察）；
- `002-port-roadmap.md` §1 表：阶段 1 状态 → ✅（完成日期+合并号）；
- `AGENTS.md` §3：`components/` 规划行补 `tab5_keyboard`（已落地）；
- `main.c` 头部 TODO 清理（bring-up 两项已完成）。

- [ ] **Step 4: 合并**

```bash
git checkout dev && git merge --no-ff feat/bringup && git branch -d feat/bringup
```
里程碑候选：bring-up 完成可合 main（§7.4）——由负责人决定，合并前重跑 §5.5 全门禁。

- [ ] **Step 5: Commit**

`docs(.): 阶段 1 bring-up 收口并晋升验证证据`

---

## 完成判据对照（roadmap §3）

1. 固件门禁全绿（Task 0/1/3/5/6/7 各 Step + Task 8 复跑）；
2. 测试图截屏过 §5.4 视觉检查（Task 3 Step 5 + Task 4 抽检）；
3. 键盘自检：0x6D + 70 键全矩阵 + 组合（Task 6 Step 3）；
4. 触摸四角校准截图（Task 5 Step 2/3）；
5. `AUD TONE` 听音通过（Task 7 Step 3）；
6. 证据晋升 + verification-log 登记（Task 8 Step 2/3）；
7. 里程碑候选 → main（Task 8 Step 4）。

## 遗留与后续

- 阶段 2 入口清单（另立计划时执行）：§0.5 上游版本复核 → research 基线更新 → D1（引入机制 ADR）→ 精读 `core.h`/`main.c`/`audio.c`/`editor.c` → P2-2 host golden 基线。
- 键盘 RGB LED（2×WS2812）不做（YAGNI，无 roadmap 需求）。
- `SCR <page>` 参数化：阶段 3 P3-5 一并评估（roadmap §5 P3-5）。
