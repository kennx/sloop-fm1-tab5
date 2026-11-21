# Task 12 证据索引（P4-B：PROJECTS / PRESETS 页面 —— FILE 组）

受测固件：Task 12 源码 + 评审修复轮（R33/R34 + minors）；镜像 `0x1837f0`（分区余量 81%），
SHA256 `69adae26…`（受测镜像与两遍重建逐字节相同，见 `task-12-bin-sha.txt`）。
设备：Tab5 /dev/ttyACM0（ESP32-P4 v1.3，ST7121 屏批次）。

## 文件

| 文件 | 内容 |
|---|---|
| `task-12-red.txt` | **RED**：旧固件（Task 11）上 `UI projects` → `UI ERR not_registered 9`，⓯ 首条断言即红（exit=1） |
| `task-12-build1.log` / `task-12-build2.log` | 首轮源码构建 ×2（0 警告 / 0 kconfig 提示） |
| `task-12-fix-build1.log` / `task-12-fix-build2.log` | **修复轮**源码构建 ×2（两遍逐字节相同，0 警告） |
| `task-12-bin-sha.txt` | 受测/重建镜像 SHA256（修复轮三份相同） |
| `task-12-flash.log` / `task-12-fix-flash.log` | esptool 三段 `Hash of data verified` |
| `task-12-fix-boot-log.txt` | 修复轮启动：`UI 就绪: 11 页注册/1 已建`、内部堆 104,639B、无 fatal |
| `task-12-fix-store-green1.txt` / `-fix-store-transcript1.log` | 修复轮 `store.sh` 全量（含 ⓯ FILE 组）：**818 PASS / 0 FAIL，exit=0** |
| `task-12-store-green1.txt` / `-store-transcript1.log` | 首轮 `store.sh`（807 PASS；保留为 RED→GREEN 历史） |
| `task-12-ui-mem.txt` | ⓯ 的 `UI MEM`/构建日志摘录（FILE 组水位、两页 delta、TAP 回显、过渡峰值） |
| `task-12-fix-nav.txt` / `-fix-nav-transcript.log` | `nav.sh` ①–⑧：**128 PASS / 0 FAIL**（⑦ 全 11 页轮转；**⑧ PLAY↔TRACKS ×10 池往返不泄漏**，Δ pool_free −72B ≤ 128B 噪声） |
| `task-12-fix-params.txt` / `-fix-params-transcript.log` | `params.sh`：**147 PASS / 0 FAIL**（`registered=11`、组作用域池口径） |
| `task-12-fix-tracks.txt` / `-fix-transport.txt` / `-fix-step_edit.txt`（+`-transcript`） | 11 / 43 / 90 PASS，0 FAIL（同会话、不重启） |
| `task-12-di25-leak.txt` | **DI-25/R34 泄漏测量**：修复前 PLAY↔TRACKS 每往返 ≈ −268B（10 次 −2,680B）；修复后 10 次 Δ 0/−84B（原始串口 transcript 两份都在文件里） |
| `task-12-fix-selftest.log` | 修复轮设备全量 `ENG SELFTEST`：**182/182**（`identical=182 mismatch=0 checks=7 deterministic=1`） |
| `task-12-selftest.log` | 首轮设备全量 SELFTEST（182/182，历史） |
| `task-12-host-test.log` / `-state-test.log` / `-knob-test.log` | host 全绿 / state 19 passed / knob 5 passed（修复轮复跑） |
| `task-12-screenshot-selftest.log` / `-manifest.log` | 截屏工具离线自测 rc=0；third_party 233/233 |
| `task-12-projects-empty.png` / `-projects-used.png` | PROJECTS：四卡空态（**初始无选中槽**，提示 `SELECT A SLOT`）；TAP SAVE 后槽 2 USED（卡 2 选中） |
| `task-12-presets.png` | PRESETS：32 格 + 槽 3 已存（`U04 ANALOG`）+ 选中描边 |
| `task-12-save-playing.png` | 播放中 TAP SAVE：状态行 `STOP BEFORE SAVE`（红字） |
| `task-12-nav-tab-cycle.png` | ⑦ TAB 全 11 页轮转后回到 PLAY（组签选中） |
| `task-12-soak60.log` / `-soak60-summary.txt` | `soak.py --duration 60`（11 页访问含 PROJECTS/PRESETS）：PASS |

## 几何（与 `page_projects.c` / `page_presets.c` 的布局宏、`inject.py` 的 `PROJ_*`/`PRE_*` 同源）

- 页面区仍从 y=72 起；两页页内只留**状态行** `(24,20)` 28pt（页名由下行子签承担）。
- PROJECTS：卡 296×400 @ `(24 + k·312, 100)`；卡内按钮 100×40 @ x=98、y=170/234/298；
  选中 = 6px `UI_ACCENT` outline + `UI_PANEL_SEL` 底。**初始无选中槽**（`PROJ_NO_SLOT`）：
  「SELECT A SLOT」提示出现时没有卡被描边；`→` 从无选中落到 A、`←` 落到 D（之后环绕）。
- PRESETS：格 144×72 @ `(30 + c·152, 88 + r·80)`（4×8，右端 1238、底 400）；
  选中 = 4px `UI_ACCENT` outline + `UI_PANEL_SEL` 底；底部按钮 LOAD/SAVE/ERASE
  100×40 @ y=460、x=24/140/256（版面顺序 LOAD/SAVE/ERASE，动作语义由 `k_btn_act`
  映射为 v=0 save / 1 load / 2 erase）。
- 状态行映射（`theme.h` 的 `ui_file_status_text`，屏幕与 `TAP` 回显同源）：
  `SAVE OK`（绿）/ `EMPTY`（灰）/ `STOP BEFORE <ACT>`（红）/ `<ACT> ERROR`（红）/
  `NO STORAGE`（红）/ `DRUM TRACK: NO SOUND`（黄）。
- 播放中：PROJECTS 的 SAVE/LOAD/ERASE 都被拒（project LOAD 会整份替换引擎并停走带）；
  PRESETS 的 LOAD **允许**（`up_load_to` 只读、panic 该轨），SAVE/ERASE 拒绝。

## 实测（修复轮，2026-11-22）

- **池**：PROJECTS delta **7,748B**（est 8,192）、PRESETS **12,228B**（est 12,800）
  （组内切页后重建：7,572 / 12,228）；FILE `sum_delta ≤ sum_est 20,992`；
  FILE 组 build_ms ≤ 33ms。
- **过渡峰值**（ADR-005 §5）：FILE 两页全建 → 跨组首建 STEP（delta 21,728B）后
  **`pool_free 7,840B`**（⓯ 断言 ≥4,096B；弧线 mask 峰值 ≈2.6KB）。
- **跨组往返不泄漏（R34）**：PLAY↔TRACKS ×10 的 `pool_free` 45424 → 45352（Δ=−72B ≤
  128B 噪声，单次最大下降 ≤ 128B）；修复前同探针 10 次 −2,680B（`task-12-di25-leak.txt`）。
- boot base 复测：`pool_free 46,568` − PLAY delta 2,480 ⇒ 14,072B（≤ 文档保守上界 14,112B）。

## 视觉检查（机器侧；人眼目检为外部门禁）

- 四页截屏经像素探针：组签/子签选中态、卡/格选中底与描边（含**初始无选中**的未描边态）、
  按钮底色（SAVE 绿 / LOAD 黄 / ERASE 橙）、状态行文本色（灰/绿/红）、卡/格文本区字形
  像素计数；页面区主色分布合理（PROJECTS 面板 ≈317k px、PRESETS 面板 ≈300k px、选中格
  ≈9.6k px），无错误色块/空白页。人眼复核（§5.4 清单）随阶段 4 收口由人工完成。
