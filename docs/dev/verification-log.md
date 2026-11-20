# 验证记录（verification log）

> **用途**：门禁证据登记册——「谁在哪个提交、何时、用什么证据、得出了什么结论」的固定落点（配套策略见 AGENTS.md §5.4 与 `docs/design/002-port-roadmap.md` §8）。
> **登记规则**：每次 §5.5 门禁判定（通过或失败）追加一行；证据文件晋升至 `docs/dev/verify/<阶段>-<日期>/` 后在此登记相对路径与 SHA256；**失败证据同样登记**（文件名含 `-fail`），修复后另起一行记录复核结论，不删旧行。

| 日期 | 阶段/任务 | 门禁项 | 判定 | 提交号 | 证据（路径 + SHA256） | 结论/备注 |
|---|---|---|---|---|---|---|
| 2026-11-19 | 阶段 0 | 固件代码（烧录验证） | ✅ 通过 | 3394356 | （登记册建立前完成；证据为启动日志，摘录于 `docs/dev/device-notes.md` §4） | 骨架首烧成功：v1.3 / 16MB flash / 32MB PSRAM 确认，无 fatal/abort |
| 2026-11-19 | 阶段 1 / Task 8 | §5.5 固件代码（构建零新增警告） | ✅ 通过 | 0f6fbf5 | `1-bringup-2026-11-19/build-gates.txt` SHA256 8fb1fa31… | build.sh 两遍退出码 0；第 1 遍仅 §5.5 已豁免的 kconfig CMake 提示对，第 2 遍 0 警告 |
| 2026-11-19 | 阶段 1 / Task 8 | §5.5 固件代码（烧录验证） | ✅ 通过 | 0f6fbf5 | `1-bringup-2026-11-19/build-gates.txt` + `boot-log-excerpt.txt` 4d2c36d8… | verify.sh 一键绿：三段 Hash of data verified，启动无 fatal/abort/backtrace，App version=0f6fbf5 终版自证；首跑因 IDF venv 缺 Pillow 截屏失败（环境问题，非固件），装 `scripts/requirements.txt` 依赖后复跑通过 |
| 2026-11-19 | 阶段 1 / Task 8 | §5.4 UI 视觉（测试图） | ✅ 通过（机器侧） | 0f6fbf5 | `1-bringup-2026-11-19/test-pattern.png` bbe03a48… + `screenshot-check.txt` b79ed2c5… | `--check` 像素抽检 13/13（彩条/圆/方框/四轨色）+ 角标/方框补检 6/6；双帧（间隔一次硬复位）cmp 字节一致（`screenshot-cmp.txt`），无撕裂/脏区 |
| 2026-11-19 | 阶段 1 / P1-1 | 分区表落地 | ✅ 通过 | 0f6fbf5 | `1-bringup-2026-11-19/parttable-flash-decode.txt` cf9db627… | esptool 直读 flash 解码与 `partitions.csv` 一致（nvs 24K/phy 4K/factory 8M/storage 8128K）；启动日志 `Loaded app from partition at offset 0x10000` |
| 2026-11-19 | 阶段 1 / P1-5 | 键盘自检（机器侧） | ✅ 通过（机器侧） | 8f8e01b | `1-bringup-2026-11-19/console-transcript.txt` 46970ac9… + `boot-log-excerpt.txt` | 0x6D 应答 OK、键盘固件 FW 0x01（寄存器 0xFE）、INT 空闲电平正确；**70 键全矩阵+组合为人测待办（DI-4，见 docs/deferred-issues.md）**；`kb-test.log` 未产出（DI-4 人测产物，执行后落 `firmware/build/verify/` 再晋升） |
| 2026-11-19 | 阶段 1 / P1-4 | 触摸四角校准 | ⏳ 人测待办 | 70b75ed | （无——准星截图需手指按压，机器不可代） | 触摸 indev/准星/坐标日志路径已落地（启动日志「触摸反馈就绪」）；四角+中心按压校准与按住截屏核对待人工执行 |
| 2026-11-19 | 阶段 1 / P1-2 | §5.4 视觉复看（测试图目检） | ⏳ 人测待办 | 211bee5 | `1-bringup-2026-11-19/test-pattern.png` bbe03a48…（待人工目检对象） | AI 会话无图像工具：文本可读性（"SLOOP TAB5 BRINGUP"）、圆/对角线渲染等目检项未被像素抽检覆盖，须人眼复看已晋升 PNG（AGENTS §5.4(2)） |
| 2026-11-19 | 阶段 1 / P1-6 | AUD TONE 音频（机器侧） | ✅ 通过（机器侧） | 0f6fbf5 | `1-bringup-2026-11-19/console-transcript.txt` + `boot-log-excerpt.txt` | ES8388 codec 44100Hz/2ch/16bit 打开 rc=0；`AUD TONE 440 1500` 命令往返成功（播放中→完成） |
| 2026-11-19 | 阶段 1 / P1-6 | AUD TONE 听音（人测） | ✅ 通过（人测，先失败后修复） | 4278509 | （人耳确认，无文件证据；寄存器取证见 device-notes §5） | **首测无声**（默认态）；systematic-debugging 定位根因 = es8388 驱动初始化把 LOUT2/ROUT2 音量写 0 + out_vol(70)→-11dB；四路输出音量 0x21 + DAC 0dB 后**人测出声**；修复（开机默认即实证出声态）后复测出声。音高精度未细究（本次以出声为判据），阶段 2 音频调优时复核。决定性寄存器未二分 → DI-6 |
| 2026-11-19 | 阶段 1 / Task 8 | §5.5 scripts 门禁（selftest） | ✅ 通过 | 0f6fbf5 | `1-bringup-2026-11-19/build-gates.txt`（门禁 4 段） | `screenshot.py --selftest` 6 组断言通过；系统 python3 与 IDF venv python3.14 双跑均退出码 0 |
| 2026-11-20 | 阶段 2 / P2-2 | host-test.sh 门禁生效 | ✅ 通过 | 0fef6c2 | `2-engine-2026-11-20/host-baseline.txt` SHA256 b53adae15ca289cb296463871177e57c3498735cb20771e7d4927428c5b52ae5 | 完整 CC0 资产随树（修复 e3b7a32，manifest 233/233 OK）；host-test.sh 连跑两次退出码 0，每次 182 golden 0 changed/0 gone、0 health/voice/crash，末行 ALL HOST TESTS PASSED (subset)；golden SHA256 189c9a0c…，未启用更新模式 |
| 2026-11-20 | 阶段 2 / P2-3 | 固件构建/烧录 + ENG INFO | ✅ 通过 | 5532c45 | `2-engine-2026-11-20/task-3-core-smoke.log` SHA256 7db9aa6ef93dd392c46ea83f48c49d8c1a51bcd1e85053917011ee6b44103920；`task-3-size-before.txt` 7f97a85ddad451f7e79e0e5fa2df10609fa34228827e13b18258ae2e3e9b7e3c；`task-3-size-after.txt` 0348e83dc8383be56adbb42d8d315fdb3cabee1fc342dff43957eaa7d162e816 | build.sh 连跑第二遍零警告；烧录 Hash of data verified；RTS 复位启动无 fatal/abort/backtrace；ENG INFO init=ESP_OK rate=44100 frames=256 NENGINES=12 P_COUNT=71 G_COUNT=50；host 182 golden 0 changed/0 gone；image +375598B、rodata +326928B、DIRAM +274400B |
| 2026-11-20 | 阶段 2 / P2-3 修复 | CTL 量子渲染 + ENG SMOKE | ✅ 通过 | d956b6d | `2-engine-2026-11-20/task-3-fix-render-smoke.log` SHA256 6095c9028a6998c51fca6abfcce2fa709f2fd6b711e951742ec39d48ce638697 | ASan 修复前复现 wet_l 越界、修复后通过；build.sh 双跑第二遍零警告；烧录三段 Hash verified；ENG INFO 保持正确；ENG SMOKE 256 帧 nonzero=444 PASS，on/off 哈希与 host ASan 运行一致；3 秒后续收流无 crash/watchdog；host 182 golden 不变，manifest 233/233 |

| 2026-11-20 | 阶段 2 / P2-4 中间调试 | 设备等价（UNISON 隔离缺陷） | ❌ 失败（已修复） | 工作树中间态 | `2-engine-2026-11-20/task-4-selftest-compare-fail.log` SHA256 33a2045b672f5a2fc3126a9b7802ea1933bbe0e2635217e9b62ece273f748225 | 首轮 179/182 一致，3 个 UNISON 不等、health/check 全绿；根因=data 快照哨兵范围漏掉 UNISON LCG 种子。排序哨兵后单例连跑一致，再全量复核 182/182；未使用容差放行 |
| 2026-11-20 | 阶段 2 / P2-4 | 同源设备等价 + 构建/烧录/启动 | ✅ 通过 | 463c832 | `2-engine-2026-11-20/task-4-summary.txt` SHA256 ec0fcdb71d7b815dd1b25c4db555632f44ccee9e40d3c02fff19630452111a00；`task-4-selftest-compare.log` 3e3fafc476409108dc8cfa3ef588c7b9d7724aed82e8569f14177bd0350787fd；全值见 `task-4-SHA256SUMS` | 原样 regress.c；设备/PC 均 182 identical、0 mismatch/missing/extra、health_fail=0、7 项 check 全过、deterministic=1；build×2 第二遍零警告，三段 Hash verified，启动无 fatal/abort/backtrace/task_wdt；host 182 golden 不变，manifest 233/233 |
| 2026-11-20 | 阶段 2 / P2-4 | ENG TRIG / PERF（机器侧） | ✅ 通过（机器侧） | 463c832 | `2-engine-2026-11-20/task-4-audio-perf.log` SHA256 a89a16368252f3e18b1f2e9a02e4c9bf0381b8f60255d791448bf199961f9052 | idle PERF 100 块 mean=548us/9.44%、max=681us/11.73%、xrun=0；AUD TONE 共享句柄完成；synth 500ms 与 drum 各触发 OK、播放中 xrun=0；人耳听音待 DI-7 收尾 |

> 证据相对路径均以 `docs/dev/verify/` 为根；SHA256 为 16 位前缀，全值见 `1-bringup-2026-11-19/SHA256SUMS`。
> 「提交号」列＝**受测固件提交**（如 8f8e01b=键盘代码、70b75ed=触摸代码、0f6fbf5=阶段 1 终版）；证据晋升与登记本身发生在其后的 docs 提交（44254a2 及终审修复提交），两列所指不同属正常。
| 2026-11-20 | 阶段 2 / P2-5 | 样本资产与许可 | ✅ 通过（机器侧） | ea2f03b | `2-engine-2026-11-20/task-5-assets.txt` | CC0 资产随树（283,212B / 8 集）；task-5 真机 SAMPLE 触发 OK；人耳确认 ⏳ |
