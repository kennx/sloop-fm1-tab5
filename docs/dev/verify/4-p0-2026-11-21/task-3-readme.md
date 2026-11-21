# P4-0 Task 3（DI-5 · SCR 重试硬化）证据说明

日期：2026-11-21｜分支：`feat/phase4`｜固件未改动（纯 `scripts/` + 文档）

## 1. 变更与语义

`scripts/screenshot.py`：

- 新增 `--retry N`（默认 3；`0` = 单次尝试的旧行为）。
- 传输层失败才重试：帧头/帧数据/尾标记超时、短帧、坏尾、**长度与分辨率不符（响应头损坏）**、
  读取期间 `OSError`（`TransferError`）。固件明确报错（`SCR ERR`、未知像素格式、响应头数值非法
  → `FirmwareError`）与端口打开失败**不重试**，快速失败语义不变。
- 复位：`esptool --chip esp32p4 -p PORT -b 460800 --after hard_reset flash_id`
  （`flash_id` 只读，不写 flash）→ 轮询 console 提示符 `sloop>`（实测约 4.1–4.6s）
  → 重开串口再截屏。esptool 定位：`$SLOOP_ESPTOOL` > PATH > `$IDF_PYTHON_ENV_PATH`
  > `~/.espressif/python_env/idf5.5*`。
- 重试耗尽：退出码 1，打印尝试次数；已收字节写 `<out>-fail.bin`，一字节未收写
  `<out>-fail.txt`（记录错误 + 尝试次数）；随后做一次 best-effort 复位（失败只打印），
  让批量任务从可用状态继续。
- `--timeout` 默认 **30s**（原 20s）：实测单帧 16.4–17.8s，20s 只有约 12% 余量。
- 协议收发抽出 `read_frame(ser, timeout, on_binary)`，inject 复用同一路径。

`scripts/inject/inject.py`：截屏失败 → 复用同一复位路径 → 重开串口 → **场景从头重跑**
（复位清空页面/走带状态，只有重跑才能让断言与截图同源），预算 `--retry`（默认 3）。

## 2. 离线自测（`python3 scripts/screenshot.py --selftest`，退出码 0）

`task-3-screenshot-selftest.log`（评审修复后重跑为 `task-3-fix-screenshot-selftest.log`）：
断言组含新增的重试用例

- `_FakeSerial` 序列化：`_open_serial` 按尝试顺序弹出假串口，`reset_device` /
  `best_effort_reset` 被替换为记录器（区分 retry/final）。
- 首轮短帧（`SCR 4 2 rgb565 16` + 8 字节、无尾标记）→ 复位 → 次轮成功：`used == 1`、
  仅一次重试复位、stdout 出现 `[retry 1/3]`、像素与基准一致。
- 全部失败（`--retry 1`）：`CaptureError`（`attempts == 2`）、复位序列 retry→final、
  `<out>-fail.bin` 落盘且内容 == 已收字节、错误信息含尝试次数。
- 零字节耗尽：`<out>-fail.txt` 落盘（含「N 次尝试均失败」与最终错误）。
- `--retry 0`：只尝试 1 次、不重试复位（仅一次 best-effort 复位）。
- `SCR ERR`：抛 `FirmwareError`、**不**复位（两条路径都断言）。
- 响应头数值非法（`SCR a b rgb565 16`）：干净 `FirmwareError`、不复位。
- `--retry -1`：argparse 拒绝（退出码 2，stderr 含「--retry 不能为负」）。

## 3. 真机证据

| 项 | 文件 | 结果 |
|---|---|---|
| 20 连拍（`screenshot.py`，每张独立进程） | `task-3-scr-batch20.log` | 20/20 `rc=0`，`retries=0`，单张 16.4–17.8s，总 338.5s；无失败 |
| 强制中断 ×2（直连 harness） | `task-3-forced-retry.log` + `task-3-forced-retry.png` | 第 1、2 次尝试各只收 4096 字节后失败 → `[retry 1/3]`、`[retry 2/3]` 各复位一次 → 第 3 次成功，`重试 2 次后成功`；产物为复位后的 PLAY 帧 |
| 强制中断 ×1（inject 集成） | `task-3-inject-retry.log` + `task-3-inject-retry-transcript.log` | 首张截屏注入失败 → esptool 复位 → `[RETRY] ... (1/3)` → 场景从头重跑 → 全部断言/抽检 PASS，`截屏复位重试 1 次` |
| 常规注入（最终代码） | `task-3-inject-tracks.log` + `task-3-inject-tracks-transcript.log` + `task-3-tracks-sel3.png` | `scripts/inject/tracks.sh` 退出码 0 |
| 门禁 | `task-3-inject-retry.log`/`task-3-inject-tracks.log` | 退出码均 0 |

### 3.1 连拍图像一致性

- shot 01–10：SHA256 `b0a188cf…`＝TRACKS 页（卡 2 选中），与 Task 7/8 归档基线
  `3-mvp-2026-11-20/task-7-tracks-sel3.png` **逐字节相同**。
- shot 11–20：SHA256 `6aef069f…`＝TRANSPORT 页（空闲），原因见 §5。
- 强制中断后经复位重试得到的 TRACKS 截图（`b0a188cf…`）与常规运行的产物逐字节相同。
- 直连 harness 的两次强制中断产物（PLAY 页）SHA256 均为 `a166395c…`（复跑确定）；与
  PLAY 基准 `bde48cc8…` 的差异仅 23,801 px、bbox (48,264)-(898,300)——状态行文本随
  会话状态不同，非传输/解码缺陷。

## 4. 实施期观察

- 实施开始时设备正处于 DI-5 停滞态：旧脚本首张截屏在 1,687,552/1,843,200 字节超时，
  且 console 无响应；`esptool … --after hard_reset flash_id` 一次即恢复（App 4.4s 启动完毕）。
- **纯 pyserial 翻转 RTS/DTR 不能复位 USB-Serial/JTAG**：按 esptool 的 `HardReset`
  与 `USBJTAGSerialReset` 序列逐字节复刻（健康态、停滞态各一次）均无启动日志；
  必须让 esptool 走 bootloader 接入再 `--after hard_reset`。详见 `docs/dev/device-notes.md` §8.1。

## 5. 附属观察（与 SCR 无关，未处理）

连拍第 10→11 张之间页面从 TRACKS 自发切到 TRANSPORT（无任何命令注入，diff bbox 覆盖全屏）。
形态与 DI-10（触摸幽灵按压）一致，属既有未定性问题，本任务不改 UI/触摸代码；记录在此备查。

## 6. 文件索引

- 执行输出：`task-3-screenshot-selftest.log`、`task-3-scr-batch20.log`、`task-3-forced-retry.log`、
  `task-3-inject-retry.log`、`task-3-inject-tracks.log`
- 原始串口回显：`task-3-inject-retry-transcript.log`、`task-3-inject-tracks-transcript.log`
- 截图：`task-3-tracks-sel3.png`（SHA256 `b0a188cf…`）、`task-3-forced-retry.png`
  （SHA256 `a166395c…`，复位后 PLAY 页）
- 强制中断 harness 源码：见任务报告（临时脚本，未入库；等价逻辑已固化在 `--selftest`）

## 7. 评审修复（fix wave，2026-11-21）

评审指出 `[Blast_Radius]`/readme §1 把「长度与分辨率不符」写成不重试的 `FirmwareError`，与代码
（`read_frame` 抛 `TransferError`）不符。**控制器裁决保留代码**：长度与分辨率不符＝响应头损坏，
属传输层问题，复位重试是正确的；错误的是文档，已按代码改写（§1、device-notes §8.1、AGENTS §5.1）。

同一轮修复：

- 零字节耗尽也落失败证据：`<out>-fail.txt`（含「N 次尝试均失败」与最终错误），不再返回空路径。
- 响应头数值非法（`SCR a b rgb565 16`）：`read_header` 内包成干净 `FirmwareError`（不再冒泡裸
  `ValueError`/traceback），`main` 打印「失败: …」退出码 1；inject 同样转为 `InjectError`。
- `wait_boot` 的读写 `OSError` 现在回到「重开端口」循环（截止时间不变），不再逃逸成 traceback。
- 重试耗尽后做一次 best-effort 复位（失败只打印），让批量后续任务从可用状态继续；inject 耗尽时
  同样先落 `<…>-fail.bin` 再复位。
- `inject.py --retry < 0` 拒绝（argparse 错误，退出码 2）。
- `screenshot.py --timeout` 默认 20 → **30s**（实测单帧 16.4–17.8s，20s 余量仅约 12%）。
- 文档勘误：device-notes §8 两处旧口径改过去时并指向 §8.1；§8.1 esptool 定位补 `*/bin/esptool`
  末位兜底；AGENTS §5.1 去掉「DI-5 硬化」（DI 已删）；`3-mvp-2026-11-21/task-8-readme.md` 的 DI-5
  指针注明「已删除」；DI-10 登记本轮无注入自身的 TRACKS→TRANSPORT 漂移观测。

修复后复验（本目录 `task-3-fix-*`）：

| 项 | 文件 | 结果 |
|---|---|---|
| 离线自测（扩展用例） | `task-3-fix-screenshot-selftest.log` 862b92a3… | 退出码 0：新增「响应头数值非法」「零字节 `-fail.txt`」「`--retry -1` 拒绝」；重试复位与 best-effort 复位分开断言 |
| 真机 5 连拍 | `task-3-fix-batch5.log` 01293d8e… | 5/5 `rc=0`、`retries=0`、16.7–17.1s |
| 强制中断 ×1（直连 harness） | `task-3-fix-forced-retry.log` 24ae8d74… + `.png` a166395c… | `[retry 1/3]` → 复位 → 成功，`重试 1 次后成功`；产物与首轮同 hash |
| 强制中断 ×1（inject） | `task-3-fix-inject-retry.log` 1647d0dd… + `-transcript.log` 0c8c375e… | 复位 → 场景重跑 → 全 PASS，`截屏复位重试 1 次`，退出码 0 |
| 常规注入 | `task-3-fix-inject-tracks.log` e7b87b37… + `-transcript.log` 3f087738… | `tracks.sh` 退出码 0 |

未在真机复现「重试耗尽」路径（需要三次连续停滞）：该路径由离线 selftest（短帧 + 零字节两种）覆盖。
