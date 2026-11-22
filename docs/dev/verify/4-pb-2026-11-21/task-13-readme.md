# Task 13 证据索引（P4-B：FM6 页 bank 存取行）

受测固件：`feat/phase4b` @ `e346ffb` + 评审修复轮源码（`App version: e346ffb-dirty`）；
镜像 `0x184210`（分区余量 81%），受测（已烧录）镜像 SHA256 `de7e20cf…`
（见 `task-13fix-bin-sha.txt`）。设备：Tab5 /dev/ttyACM0（ESP32-P4，ST7121 屏批次）。

## 设计要点

- FM6 页（`page_params.c`，布局表声明引擎 9）底部加一行 44px：`BANK`(16..104) /
  `−`(112..176) / 槽号 `B<n+1>`(184..280，居中、UI_ACCENT) / `+`(288..352) /
  `SAVE`(368..468，绿) / `ERASE`(484..584，橙) / 状态行(600..1264)。
- 几何：页面区相对 `y=552..596`。旋钮盒底边 = `52 + 298 + 26 + 160 = 536` ⇒ **16px 间隙**，
  不压缩旋钮区（未改 `GRID_Y0/ROW_PITCH`）。页面区高 648。
- 语义：槽号 = **目标槽** 0..26，与 PTCH（音源，PTCH = 8 + 槽号）独立；`SAVE` →
  `CMD_BANK_PUT`（选中轨 live patch = `fm6_patch[track]` 经 `fm6_pack`），`ERASE` →
  `CMD_BANK_ERASE`。FM6 的 8 个 EDIT 值是**宏**（上游 `fm6_sync` 叠加在 patch 上），
  调 ALG 等不改 `fm6_patch` ⇒ SAVE 存的是 patch 本体（= 上游 "Store in bank"）。
- **锁定页拒绝（R36）**：选中轨引擎 ≠ FM6（含鼓轨——没有引擎）时 SAVE/ERASE 都**直接
  拒绝**：不发状态层命令、不动存储，状态行与 `TAP` 回显显示 `NOT FM6`（红字；页内局部
  状态，不写 `app_state_t.store_status`）。每个合成轨都有 `fm6_patch`（上电 INIT VOICE、
  之后是该轨最后一次载入的 patch），旧行为会在锁定页把这份残留 patch 写进用户银行槽。
- 状态行（非锁定页）复用 `theme.h` 的 `ui_file_status_text`（act 0 SAVE / 2 ERASE），
  与 FILE 页同一套文案；播放中由存储层拒绝 ⇒ `STOP BEFORE SAVE`。
- 擦写在 LVGL 回调里发生（TAP/按钮 → `state_apply` → 存储层），一次 SAVE/ERASE 最坏
  阻塞一次整银行擦写窗口（spec §4.3 接受的 ≤150ms 级 UI 停顿；ADR-009 §3 的锁纪律）。
- TAP：`SLOOP_TAP_BANK_SLOT`（b=槽、d=±1）、`SLOOP_TAP_BANK_STORE`/`_ERASE`（b=槽、v=0/2）；
  console 回显 `-> BANK slot=k delta=±1` / `-> BANK slot=k action=save|erase status="…"`
  （状态串来自 `sloop_ui_bank_status_text()`，与屏幕同源；行未建时返回 NULL）。
- 键盘：←/→ 仍只在旋钮间移动焦点（不改既有旋钮模型）；bank 行只经触摸/console。
- 预算：7 个单对象 label + 3 个静态样式 ⇒ 实测 delta **4,956–5,100B**（`store` ⑯ 4,956 /
  `params` 5,096；旧 2,864B），est `3,584 → 6,144`（512 对齐；对最大实测 5,100 是 +20.5%，区间下沿 4,956 处 +24.0%（高过 +23% 上沿 1 pp，见 ADR-008 §3 注））；EDIT 组 Σ est
  `17,408 → 19,968`。
- 组不变量（base 13,800B，4KB 余量）：最坏 **FILE→STEP** `44,032B` ⇒ 账面 61,928、
  余 1,192B；次坏 **EDIT→STEP** `43,008B` ⇒ 账面 **60,904**、余 **2,216B**；
  再次 **SEQ→PRESETS** `40,448B` ⇒ 58,344、余 4,776B。

## 文件（原始轮 + 评审修复轮）

| 文件 | 内容 |
|---|---|
| `task-13-red.txt` / `task-13-red-transcript.log` / `task-13-red-fm6.png` | **RED**：Task 12 固件（FM6 est 3,584、无 bank 行）上同一 `store.sh`：BANK 行四处像素抽检全红（背景色）、`TAP BANK +` 回 `TAP miss`、`STORE BANK GET 3` 回 `STORE ERR empty` ⇒ **exit=1** |
| `task-13-build1.log` / `task-13-build2.log` / `task-13-build3-rebuild.log` / `task-13-build4-final.log` | 原始轮构建 ×2 + 两次重建：rc=0、警告 0、镜像 `0x184130` |
| `task-13-bin-sha.txt` | 原始轮三次镜像的 SHA256 与 65B/83B build 元数据差异位置 |
| `task-13-flash.log` / `task-13-boot-log.txt` | 原始轮烧录（三段 `Hash of data verified`）与启动日志（`App version: 2e59d3a-dirty`） |
| `task-13-store-green1.txt` / `-store-transcript1.log` / `task-13-store-green2.txt` / `-store-transcript2.log` | 原始轮 `store.sh` **867 PASS / 0 FAIL ×2** |
| `task-13-ui-mem.txt` | 原始轮 ⑯ 的构建日志 / `UI MEM` / TAP 回显摘录 |
| `task-13-fm6-bank.png` / `-saved.png` / `-playing.png` | 原始轮 FM6 页（B1 / B4+SAVE OK / STOP BEFORE SAVE） |
| `task-13-params-fm6.png` / `-fm6-locked.png` | 参数场景的 FM6 页（解锁 / 锁定态） |
| `task-13-nav.txt` / `task-13-params.txt`（+ transcript） | 原始轮 nav 127 / params 146 PASS，0 FAIL（同会话不重启） |
| `task-13-selftest.log` / `-host-test.log` / `-state-test.log` / `-knob-test.log` / `-screenshot-selftest.log` | 原始轮设备 **182/182**、host 全绿、state 19、knob 5、工具自测 rc=0 |
| `task-13-visual-check.txt` | 原始轮截图像素结构检查 |
| `task-13fix-build1.log` / `task-13fix-build2.log` | **修复轮**构建 ×2：rc=0、**警告 0**、0 kconfig 提示；镜像 `0x184210` |
| `task-13fix-bin-sha.txt` | 修复轮受测镜像 SHA256 `de7e20cf…`（0x184210）与版本串 |
| `task-13fix-flash.log` / `task-13fix-boot-log.txt` | 修复轮烧录（三段 `Hash of data verified`）+ 启动日志（`App version: e346ffb-dirty`、内部堆、无 fatal） |
| `task-13fix-store-green1.txt` / `-store-transcript1.log` | **修复轮 `store.sh` 全量：882 PASS / 0 FAIL，exit=0**（新增 15 项：锁定页拒绝 ×5、槽环绕 ×2、PTCH 不变、播放中被拒后槽保持空 ×3、AUTO ON 等） |
| `task-13fix-store-green2.txt` / `-store-transcript2.log` | 修复轮复跑（⑯ 收口 `STORE AUTO ON` 之后，下一轮从 ⑯ 末态 autosave 恢复）：**883 PASS / 0 FAIL**（+1 = ⑪ 的幂等步 TAP；两次 h0 均 `D160C9B7`） |
| `task-13fix-fm6-bank-locked.png` | **锁定页（轨 0 = ANALOG）**：页眉 N/A 红字 + bank 行状态行 `NOT FM6` 红字 |
| `task-13fix-params.txt` / `-params-transcript.log` | 修复轮 `params.sh`（同会话、不重启）：**146 PASS / 0 FAIL**；`UI MEM FM6 delta=5,096 est=6,144`、`EDIT sum_est=19,968`、`pool_free=31,980` |
| `task-13fix-nav.txt` / `-nav-transcript.log` | 修复轮 `nav.sh`（同会话、不重启）：**127 PASS / 0 FAIL** |
| `task-13fix-selftest.log` | 修复轮设备全量 `ENG SELFTEST`：**182/182** |
| `task-13fix-host-test.log` / `-state-test.log` / `-knob-test.log` / `-screenshot-selftest.log` | 修复轮 host 全绿 / state 19 / knob 5 / 工具自测 rc=0 |
| `task-13doc-state-test.log` / `-knob-test.log` / `-host-test.log` | **文档收口轮**（无设备运行）：`state-test` PASSED、`knob-test` PASSED、`host-test` 全绿——firmware 仅改一行注释、脚本仅改 ⑯ 的收尾与措辞 |

## ⑯ 段判据链（修复轮，`task-13fix-store-transcript1.log`）

1. `STORE AUTO OFF` → `ensure_stopped` → bank 归一（used 槽全擦）→ `COUNT used=0`。
2. **锁定页拒绝**：选中轨 0（ANALOG）→ `UI fm6` → `TAP BANK SAVE` / `ERASE` ⇒
   `status="NOT FM6"`、`STORE BANK LIST` 逐槽不变；截屏（红字 `NOT FM6`）。
3. **鼓轨锁定页**：选中轨 3 → `UI fm6` → `TAP BANK SAVE` ⇒ `status="NOT FM6"`、
   `BANK LIST` 不变（同一个 `page_locked` 判定）。
4. 解锁：选中轨 1 → `ENG TRACK 1 1 0` → `ENG TRACK 1 9 0`（FM6 工厂预置 0）
   → `PARAM 63 0`（环顶探针前提；宏不改 patch）→ `STORE BANK LIVE` = L
   （`5F281E3C…TUNE EP`）→ `PARAM 70` 记录 PTCH 基线。
5. `UI fm6`：`UI MEM` 的 FM6 `delta ≤ est 6,144`、EDIT built=1；BANK 行截屏抽检
   （SAVE 绿 / ERASE 橙 / 槽号 `B1` 黄 / `BANK` 灰字 / 旋钮 0 环顶 PANEL_HI）。
6. 槽：`−` 从 B1 → B27（环绕）、`+` 回 B1、`+`×3 到 B4、`−`/`+` 往返；
   **`PARAM 70`（PTCH）在槽 ± 序列前后不变**。
7. `TAP BANK SAVE` ⇒ `status="SAVE OK"`；`STORE BANK GET 3` == `STORE BANK LIVE`（逐字节）、
   `LIVE` 不变、`LIST` B3 USED / `COUNT used=1`；截屏（`B4` + 绿字 `SAVE OK`）。
8. `TAP BANK ERASE` ⇒ `status="ERASE OK"`；`GET 3` → `STORE ERR empty`、`COUNT used=0`。
9. **播放中拒绝（判别力）**：槽 3 此刻为空 → `KEY space` 播放 → `TAP BANK SAVE` ⇒
   `status="STOP BEFORE SAVE"`、`GET 3` 仍 empty、`BANK LIST` 全空（若拒绝被绕过，
   这次 SAVE 会写出非空槽）；截屏（红字）；`KEY space` 停止。
10. 收口：bank 全空、轨 1/轨 0 回 ANALOG 预置 0、选中轨 0、`UI play`、
    `STORE AUTO ON`（恢复默认开关）。
