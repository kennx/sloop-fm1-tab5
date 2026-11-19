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
| 2026-11-19 | 阶段 1 / P1-6 | AUD TONE 音频（机器侧） | ✅ 通过（机器侧） | 0f6fbf5 | `1-bringup-2026-11-19/console-transcript.txt` + `boot-log-excerpt.txt` | ES8388 codec 44100Hz/2ch/16bit 打开 rc=0；`AUD TONE 440 1500` 命令往返成功（播放中→完成）；**扬声器/耳机听音（音高/爆音）人测待办** |
| 2026-11-19 | 阶段 1 / Task 8 | §5.5 scripts 门禁（selftest） | ✅ 通过 | 0f6fbf5 | `1-bringup-2026-11-19/build-gates.txt`（门禁 4 段） | `screenshot.py --selftest` 6 组断言通过；系统 python3 与 IDF venv python3.14 双跑均退出码 0 |

> 证据相对路径均以 `docs/dev/verify/` 为根；SHA256 为 16 位前缀，全值见 `1-bringup-2026-11-19/SHA256SUMS`。
> 「提交号」列＝**受测固件提交**（如 8f8e01b=键盘代码、70b75ed=触摸代码、0f6fbf5=阶段 1 终版）；证据晋升与登记本身发生在其后的 docs 提交（44254a2 及终审修复提交），两列所指不同属正常。
