# Task 11 证据索引（P4-B 导航重构：两行页签 + 组作用域生命周期）

受测固件：Task 11 源码（提交见仓库 `git log`，构建号 0x181730，分区余量 81%）。
设备：Tab5 /dev/ttyACM0（ESP32-P4 v1.3，ST7121 屏批次）。

## 文件

| 文件 | 内容 |
|---|---|
| `task-11-build1.log` / `task-11-build2.log` | 最终源码构建 ×2（第二遍 0 警告/0 kconfig 提示） |
| `task-11-flash.log` | esptool 三段 `Hash of data verified` |
| `task-11-boot-log.txt` | 复位后的启动日志（UI 就绪池水位、无 fatal） |
| `task-11-di19-before-panic.log` | **修复前**：旧固件上 `store.sh → params.sh`（不重启）的池悬崖 panic（`pool_free 2,160` → `UI fx` → MTVAL=0x76） |
| `task-11-di19-after-store.txt` / `task-11-di19-after-params.txt` | **修复后**：最终固件上 `store.sh → params.sh` **同一启动会话、无重启**，两者 exit=0（DI-19 收口验收） |
| `task-11-nav.txt` / `task-11-nav-transcript.log` | nav ①–⑥（新 TAB 顺序、组签/子签探针、组作用域 built=0、build_ms；⑥ R32 `UI TAB`/`UI SUB` 手指导航 + R30 同拍待切页作废） |
| `task-11-params.txt` / `task-11-params-transcript.log` | 参数页五页 + 组作用域池口径（`GROUP` 行、pool_free ≥20KB、delta ≤ est） |
| `task-11-tracks*` / `task-11-transport*` / `task-11-step_edit*` | 其余三条注入场景（页签区探针已换两行坐标） |
| `task-11-page-{play,tracks,global,fx,analog,fm6,sample,step,transport}.png` | 九页截屏（两行页签 + 页面区） |
| `task-11-ui-tab-sub.txt` | 派生摘录：nav ⑥ 手指导航（R32）/⑥d（R30）断言行（原始证据为 nav transcript） |
| `task-11-ui-mem.txt` | 九页逐页 `UI MEM`（GROUP 行 + 页行） |
| `task-11-pool-peak.txt` | 过渡峰值实测（SEQ→EDIT 20,528B free；EDIT→STEP **12,356B** free） |
| `task-11-selftest.log` | 设备全量 `ENG SELFTEST` |
| `task-11-host-test.log` / `task-11-state-test.log` / `task-11-knob-test.log` | host/state/knob 单测 |
| `task-11-screenshot-selftest.log` / `task-11-manifest.log` | 截屏工具离线自测、third_party 完整性 |
| `task-11-layerhint-edit.png` / `task-11-layerhint-all.png` / `task-11-layerhint-check.txt` | 层键提示（Aa；四层同按）与子签无重叠的像素证据 |
| `task-11-bin-sha.txt` | 受测（已烧录）镜像与最终源码重建镜像：**同代码、不同 build 元数据**（不是同一镜像） |

## 视觉/几何要点（Task 11）

- 上行组签：`x = 16 + i×184`，`y∈[4,36)`，176×32；下行子签同 x、`y∈[38,68)`，176×30；
  页面区仍从 y=72 起——`task-11-page-play.png` 的页面区与 P4-B 基线 `task-0-play.png`
  **逐字节相同**（`diff bbox = None`）。
- 单页组（PLAY/TRACKS）子行留空；EDIT 组 5 张子签、SEQ 组 2 张；FILE 在 Task 12
  注册前不显示（`UI MEM GROUP FILE sum_est=0`）。
- SLOOP 标题在上行右侧（x≈1186..1254, y≈10..23），层键提示在下行右侧
  （盒 x∈[936,1170]，LONG_DOT），与页签右端 928 无重叠。
