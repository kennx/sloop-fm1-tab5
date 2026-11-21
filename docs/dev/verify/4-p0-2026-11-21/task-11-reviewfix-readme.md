# 阶段 4 P4-0/P4-A 终审修复轮 — 证据说明（task-11-reviewfix-*）

受测固件：`abc561a-dirty`（dev 基线 abc561a + 本修复轮源码），2026-11-21 16:20–16:50 CST。
本目录文件由 `scripts/` 标准流程产出，`SHA256SUMS` 覆盖全部文件（除自身）。

## 1. 修复项与验证

| 项 | 结论 | 证据 |
|---|---|---|
| I1 quiesce 失败不再谎报成功 | `sloop_ui_quiesce` 返回 bool；ON 取锁超时 → 回滚输入静默、不置标志、`ENG SELFTEST` 回 `cases=0 result=FAIL reason=quiesce` 且不冻结引擎；OFF 失败重试 3 次、保持静默态可重试 | `task-11-reviewfix-quiesce-fail.log`（`UI LOCKHOLD 3000` 强制锁占用 → SELFTEST 1.10s 拒绝 → UI/KEY/STATE 仍存活），`-quiesce-fail-run.txt` 断言 10/10 PASS |
| I2 容量算术改口径 | ADR-008 §3/§5 改「真门禁 = 逐页 free ≥ est」；Task 11 est 之和 56,320 的真实账面 −216B 如实记录（终审轮 est 下调后 +712–808B，仍非可用余量）；STEP est 24,064 → 23,040（实测 delta 21,484–21,864B，+5.4%） | `task-11-reviewfix-steplast.log`：冷启动 8 页建完 free 24,112 → STEP 建成功 → free 2,628、delta 21,484、门槛余量 1,072；params 顺序实测 free 下沿 24,044（est 24,064 时被拒 20B） |
| I3 页签/层提示不重叠 | 页签 107px（右侧留 220px）、层提示 16pt + 宽度封顶 + 省略号；四层键同按也不压页签 | `task-11-reviewfix-tab-base.png` / `-tab-layer-all.png`，`-layer-probe.txt`：diff bbox (1065,29)–(1169,41)，x<1043 与 x∈[1043,1050] 变化像素 **0**。10 页不可行的结论写入 ADR-005「导航信息架构」 |
| I4 P_ESYNC 动态描述符 | 选中轨 ENV SYNC 开时 FX 页 ATK 槽显示 `ATK/ESYNC` + 0..127 原始值（不再显示 ms）；`P_ESYNC` 纳入页面上下文键，运行时翻转即重解析/重绘；ADR-008 §9 修正错误前提并登记 P4-B route a | `task-11-reviewfix-fx-esync-{off,on,off2}.png`、`-esync.log`、`-esync-probe.txt`（名称行 diff 12.1 / 值行 10.8、相邻 PAN 0、回关无残影；翻转态旋钮仍可编辑 10→52） |
| 小项 | task-1-readme 听音顺序警告 + DI-7 扩展；verification-log 两处「观察值，未归档」；005 STEP est 更新；knob 名称行 `text_local=0`；`inject.py` `_open()` 失败转 InjectError；sloop_ui.h/main.c 9 页措辞；`KEY PANIC` 诊断期例外入 AGENTS §5.4.1；soak 九页建齐 + `UI MEM` 采样 | 各文件 diff |

## 2. 门禁结果

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 ×2（clean + 增量） | ✅ 退出码 0，warning 0/0 | `task-11-reviewfix-build-clean.log`、`-build-incremental.log` |
| 烧录 | ✅ `Hash of data verified` ×3 | `task-11-reviewfix-flash.log` |
| 启动 | ✅ 无 fatal/abort/Backtrace/task_wdt；`UI 就绪: 9 页注册`、流式已启 | `task-11-reviewfix-boot-log.txt` |
| 全量 SELFTEST（quiesce 诊断期） | ✅ `cases=182 identical=182 mismatch=0 result=PASS`（7 声音检查、deterministic=1） | `task-11-reviewfix-device-selftest.log`、`-run.txt` |
| 五条注入脚本 | ✅ transport / step_edit / tracks / nav / params 全部退出码 0、0 FAIL | `task-11-reviewfix-inject-*.log` / `-run.txt` / `*.png`（params 九页水位：built=9 free=2,596，每页 delta ≤ est） |
| soak 120s（九页全建后） | ✅ VERDICT PASS：5 采样、internal_free 70,363–70,367B、xrun=0、duty_mean ≤10.75%、`ui_pool_free` 2,6xx | `task-11-reviewfix-soak.log`、`-soak-summary.txt` |
| host / state / knob / screenshot 自测 | ✅ 全部退出码 0 | `task-11-reviewfix-{host-test,state-test,knob-test,screenshot}.log` |
| third_party 清单 | ✅ 233/233 OK | `task-11-reviewfix-manifest.txt` |

## 3. 备注

- 冻结源码后再做 clean + 增量构建与烧录，之后的所有设备证据都采自该镜像
  （`App version: abc561a-dirty`，启动无异常）。
- `UI MEM` 数值随 TLSF 块历史漂移：九页全建 free 实测 2,596–2,644B（终审轮）、2,816B（Task 11）。
- 页签探针从 y=36 上移到 y=20：9 页时 107px 页签里 y=36 会命中文形（实测 (369,36) 命中
  TRANSPORT 的 T 竖笔），y=20 在文本行之上、只取填充色。
- soak 的 `ui_pool_free` 列是九页全建后的 LVGL 池剩余（采样表最后一列前一列）。
