# 延迟处理问题清单（deferred issues）

> **性质**：已知但可延迟的缺口/风险收件箱。**条目解决后即从本文档删除**（若涉及方案选型，先沉淀为 `docs/design/` 的 ADR 再删条目）。
> **新增时机**：调研或开发中发现"现在不修、但不该被遗忘"的问题时立即登记（一行命令的事，换的是跨会话不丢失）。
> **每条必填**：背景 / 影响面 / 为何可延迟 / 解决方向 / **触发条件**（何时必须处理）。
> 维护规则已纳入 AGENTS.md §6 文档维护协议。

---

## DI-1: USB-MIDI host 类驱动缺失（联动模式核心通道）

| 项 | 内容 |
|---|---|
| **背景** | ADR-001 依赖核验发现：`usb_host_uac` 1.5.0 仅覆盖 UAC1 音频采集，**不含 USB-MIDI host 类**。FM-1 是组合 USB 设备（MIDI + UAC1），Tab5 做 host 需各自驱动。 |
| **影响面** | **联动模式全部控制面**：编辑器协议 77 命令下发、CHANGED/RELOAD/STEP_CHANGED/TRACK_CHANGED push 上行（双向同步骨架）、LIVE 遥控、LIVE 页普通音符（按轨通道）——全部走 USB-MIDI，缺驱动则一个字节都发不出。音频采集（usb_host_uac）不受影响；**独立模式零影响**。 |
| **为何可延迟** | 开发顺序独立模式先行（显示/键盘 bring-up → sloop_core → UI），不碰 USB host；且技术可行性与工作量已知可控（见下）。 |
| **解决方向** | 新建 `usb_fm1` 组件时先写 **ADR-002** 选型：(a) 基于 ESP-IDF 原生 `usb_host` 栈自写 MIDI 1.0 类驱动——bulk IN/OUT + 4 字节事件包，SysEx 重组参考 Sloop Go `MidiEngine.kt` 与 `docs/research/editor-protocol.md` §8；(b) tinyusb host 路线（MIDI host 成熟度**待查证**）；(c) 社区组件（registry 再查一轮）。**先例**：[fm1-cartridge](https://github.com/andrerezemini/fm1-cartridge)（ESP32-S3 对 FM-1 做 USB-MIDI host + 回传音频）证明链路可行，驱动实现可参考。 |
| **触发条件** | 进入联动模式开发（`usb_fm1` 组件立项）之前必须解决；在此之前仅按需更新本条目认知。 |
| **参考** | `docs/design/001-idf-version.md`（缺口首次记录）、`docs/research/editor-protocol.md` §8、`docs/research/sloop-fm1-go.md` §3、`docs/dev/device-notes.md` §3（FM-1 接入状态） |
| **登记** | 2026-11-19（代码评审会话） |

## DI-2: sdkconfig.defaults 变更不生效（旧生成物 sdkconfig 掩盖）

| 项 | 内容 |
|---|---|
| **背景** | `firmware/sdkconfig`（gitignored 生成物）存在时，改 `sdkconfig.defaults` 后重新构建**不会**应用新默认值——Task 1 实测：分区表自定义项被静默忽略，首次构建仍用旧 1MB 表。须删除 `firmware/sdkconfig` 触发重建。 |
| **影响面** | 所有改 `sdkconfig.defaults` 的任务（阶段 1 已知 Task 3 console 通道切换；后续阶段 Kconfig 变更同理）。症状是「构建成功但配置未生效」，易误判为配置写错。 |
| **为何可延迟** | 手动规避只需一行 `rm firmware/sdkconfig`；无功能缺陷，纯开发环境效率/正确性陷阱。 |
| **解决方向** | `scripts/build.sh` 在 defaults 比 sdkconfig 新时自动删除重建（或至少显著警告）；实现时注意保留用户在 sdkconfig 中的有意覆盖（本项目策略=defaults 为唯一权威，覆盖不入库，直接重建即可）。 |
| **触发条件** | 下一次任何 `sdkconfig.defaults` 变更任务（Task 3）前不修则必须手动规避；顺手修进 build.sh 更佳。 |
| **登记** | 2026-11-19（阶段 1 Task 1 实测） |

## DI-4: Tab5 Keyboard 70 键全矩阵人测未执行（阶段 1 门禁 3）

| 项 | 内容 |
|---|---|
| **背景** | Task 6 键盘驱动已落地并真机验证到「探测 OK + console 可查 + INT 空闲电平正确」（FW 0x01），但逐键 down/up、Shift/Ctrl/Alt/Sym 组合、方向键四连等**需要人手按键盘**，自动化不可达。**2026-11-20 进展（阶段 3 Task 6）**：修饰键组合的「是否独立上报」已由键盘内部固件源码核对（`user_scan_handle.c` 逐键消抖入队、`user_i2c_callback.c` KEY 模式直出原始事件，无组合吞并），层键按 hold 语义落地；**物理逐键/组合按压核对仍未执行**，本条目保留。 |
| **影响面** | 仅验证覆盖面，不影响功能：解码逻辑按协议实现（事件字节 bit7/[6:4]/[3:0]，键名表与键盘固件 key_value_map 一致）。若键名表行列有误（引用源拷贝错误），日志键名会与丝印不符。 |
| **为何可延迟** | 驱动与协议路径已机器验证（I2C 应答、INT 释放、无 I2C 错误）；剩余是物理按键矩阵的确认，须人工在岗。 |
| **解决方向** | 按任务报告「人测脚本」执行：70 键逐键 down/up → Shift+字母 → Ctrl+Alt+Sym 组合 → 方向键×4；核对 `KB` 命令计数（down=up=按键次数）与键名，monitor transcript 存 `firmware/build/verify/kb-test.log`。完成后删除本条目。 |
| **触发条件** | 下一次有人在 Tab5 + Tab5 Keyboard 硬件旁工作时执行；阶段 1 门禁 3 收口前必须完成。 |
| **登记** | 2026-11-19（阶段 1 Task 6）；2026-11-21 阶段 3 收口：注入路径（`KEY` 与物理同入口）在 30min soak 中 150 组 note on/off（300 事件）全部回显正常、无丢键，仍不能替代物理逐键核对，条目保留 |

## DI-6: ES8388 决定性输出寄存器未二分定位（阶段 2 音频调优）

| 项 | 内容 |
|---|---|
| **背景** | 阶段 1 无声修复（4278509）以「DAC 0dB + 四路输出音量全 0x21」的实证组合出声即收口；未二分定位：功放输入取自 LOUT1 还是 LOUT2、0x1E 与 0x21 的听感差、耳机通路是否独立可用。取证见 `docs/dev/device-notes.md` §5。 |
| **影响面** | 阶段 2 sloop_hal_tab5 音频落地的音量/增益策略（当前全开为保守实证态，可能底噪偏大、动态余量受限）；DI-2 同款排查入口已备（`AUD REG/PA/L2/MAX` 命令）。 |
| **为何可延迟** | 当前配置已出声且稳定；引擎移植阶段（P2-3/P2-4）不依赖音量精度，仅依赖通路可用。 |
| **解决方向** | 阶段 2 音频任务内做二分实验（L1/L2 各自开闭 × 0x1E/0x21），定出功放输入通道与合理默认音量；复核 44.1kHz 音高（I2S 日志已证 44100/256×MCLK，缺人耳音高确认）；耳机通路单独人测。 |
| **触发条件** | sloop_hal_tab5 音频组件立项（P2-3）时处理；DI-4 人测若发现键盘问题与此无关。 |
| **登记** | 2026-11-19（听音人测排障会话） |

## DI-7: ENG TRIG 合成/鼓人耳听音待收尾

| 项 | 内容 |
|---|---|
| **背景** | P2-4 机器侧已验证音频任务启动、44.1kHz/2ch/16bit codec 写回路与 TRIG 命令往返；合成音 note-on 500ms 后 off、鼓 kit/lane 各触发一次。机器无法替代人耳确认音色与可听性。**P4-0 Task 1 追加**：DI-9 的 mute/solo 写路径只证到上游 `trk_silent()` 的输入状态（`P_MUTE`、`song.solo`）落引擎（`STATE SNAP`/`ENG SNAP` 同源），**静音/独奏的实际听感**（扬声器/耳机是否真的静音、独奏是否只剩目标轨）仍是人测项。 |
| **影响面** | 仅「首版可听」与 mix 语义的人测结论；设备等价哈希、实时占空比与 xrun 机器门禁不受影响。注意 `task-1-readme.md` §2 的脚本顺序（先 Alt+1 静音轨 0、再 Alt+5 独奏轨 0）在听感上是**全静音**（`trk_silent()` = `P_MUTE` 置位 **或**「有独奏且本位未置」——mute 优先），不能用来判断独奏混音，人测须先清 mute 或对另一轨独奏。 |
| **为何可延迟** | 沿用阶段 1 人测裁决：机器侧门禁先做全，听音由硬件旁人员收尾；不阻塞后续引擎任务。 |
| **解决方向** | 执行 `ENG TRACK 0 0 0`、`ENG TRIG synth 0 60`、`ENG TRIG drum 0 0`，确认识别到 ANALOG 合成音与 808 kick，无爆音/持续异常；另按 `task-1-readme.md` §2 逐状态听：① mute 轨 0 → 轨 0 无声、其它轨可闻；② **先 Alt+1 解除 mute**，再 Alt+5 独奏轨 0 → 只剩轨 0（不要沿用「先 mute 再 solo 同一轨」的组合，那是全静音）；③ 全部还原。记录结论后删除本条目。 |
| **触发条件** | 阶段 2 里程碑「sloop_core 首版可听」合入 dev/main 前必须完成。 |
| **登记** | 2026-11-20（P2-4）；2026-11-21 P4-A 终审：追加 mute/solo 听感人测项（Task 1 只证输入状态；`task-1-readme.md` §2 的 Alt+1→Alt+5 同轨组合是全静音，人测须分离 mute 与 solo 状态） |

## DI-10: 触摸面板幽灵按压污染冒烟采集（未定性）

| 项 | 内容 |
|---|---|
| **背景** | 阶段 3 Task 5 证据采集时发现：空闲状态下触摸面板间歇上报按压（`sloop_display: touch (690,22)` 等），导致页面/焦点被意外切换、STEP 网格格被意外翻转（`ENG STEP` 可复现）。同期 Tab5 Keyboard 报 `INT=1`、`pending=32`（事件队列灌满）。详录见 `docs/dev/device-notes.md` §7。**2026-11-20 进展（阶段 3 Task 7）**：`TAP` 注入 + 三条场景脚本的采集会话未观察到幽灵按压干扰（step_edit 两次独立运行的截图逐字节相同、网格终态与 TAP 操作一致）；同时 bring-up 触摸准星在 UI 就绪后已关闭（`app_display_touch_feedback_enable(false)`），幽灵事件即使到达也不再产生可视噪声。**根因仍未定性**，本条目保留。 |
| **影响面** | 任何「注入 + 截屏/断言」类验证（Task 7 `TAP` 注入、Task 8 soak、阶段 4 以后的触摸相关门禁）都可能被幽灵输入污染；固件的触摸驱动本身在阶段 1 四角校准中已验证（坐标映射正确），故更像排线/接地/电源噪声，非解码缺陷。**升级规则**：若在**不接串口采集/无脚本注入**时（仅正常使用、无人触碰）仍复现幽灵按压，按**用户可见的硬件/固件缺陷**处理（而非采集噪声），进入硬件排障（原始 I2C 读数 + INT 波形），不得再以「采集噪声」为由推迟。 |
| **为何可延迟** | 不影响 Task 5 交付（网格/卡片行为用「注入真实控件事件 + 48 步终态复核」验证，采集时临时关掉触摸 indev）；真值排查需要读控制器原始坐标/INT 波形，属于独立硬件排查工作。 |
| **解决方向** | ① 用 `bsp_touch_new` 句柄 + 裸 I2C 读触摸控制器原始寄存器，观察 INT 抖动与坐标跳变；② 检查触摸排线/接地/供电纹波；③ 若确认是噪声，可在驱动层加坐标稳定性过滤（连续 N 帧同点才算按下），并记录在 `docs/tab5-hardware.md` §6 的批次差异节。 |
| **触发条件** | Task 7 触摸 `TAP` 注入/人测若出现「脚本绿而手测红」或采集数据不稳定时；或 Task 8 soak 出现非预期状态跳变时。 |
| **登记** | 2026-11-20（阶段 3 Task 5）；2026-11-21 阶段 3 收口：30min soak 全程 150 次切页 + 150 次选卡 + 150 次步格 TAP，无 `TAP miss`、无页面漂移、终态与注入动作一致（未观察到幽灵按压干扰，但只说明本次采集窗口无表现，根因仍未定性）。2026-11-21 P4-0 Task 3 **新增无注入漂移观测**：20 连拍期间（每张独立 `screenshot.py` 进程、未发送任何 UI/触摸命令）第 10→11 张页面自发从 TRACKS 切到 TRANSPORT（像素 diff 覆盖全屏），见 `docs/dev/verify/4-p0-2026-11-21/task-3-scr-batch20.log` 与同目录 `task-3-readme.md` §5——形态与幽灵按压一致，仍属采集期污染嫌疑（升级规则未触发：该批有串口采集在跑） |

## DI-12: 全量 SELFTEST 诊断期单个 golden job 超 WDT 窗口会打一条 task_wdt（非卡死）

| 项 | 内容 |
|---|---|
| **背景** | P4-0 Task 2 修复 DI-11 后，全量 182 例在 UI live 下通过（157.2s），但运行末尾仍打一条 `task_wdt`（`- IDLE0`、`CPU 0: console_repl`）：某个 golden job 连续占用 console 任务 >5s，IDLE0 在这段窗口内无法被调度喂狗。summary 在 WDT 后 ~1.2s 正常给出 `result=PASS`，设备无卡死。证据：`docs/dev/verify/4-p0-2026-11-21/task-2-selftest-full-uiquiesce.log` 第 142 行。 |
| **影响面** | 仅诊断期日志噪声：引擎门禁判据（汇总行命中 cases/identical/mismatch/checks/result）不受影响。风险是读者把这条 WDT 与已删除的 DI-11 卡死混为一谈（两者形态不同：DI-11 是 console 无响应 + `sloop_input` 自旋）。 |
| **为何可延迟** | 不影响判定与产品行为；单 job 的耗时由 vendored `regress.c` 决定，工程约定不改上游用例表。 |
| **解决方向** | 若需彻底洁净：诊断期把 `esp_task_wdt` 超时临时调到 >最长 job（`esp_task_wdt_init`，结束后恢复 5s）；或在 shim 的 `selftest_read()/waitpid()` 之外给 job 内循环加喂狗钩子（需上游配合）。取舍：前者临时关闭 IDLE 看门狗，后者动上游。 |
| **触发条件** | 若门禁脚本开始把「诊断期日志出现 task_wdt」判为失败，或需要无人值守采集洁净 transcript 时处理。 |
| **登记** | 2026-11-21（P4-0 Task 2） |

## DI-13: LVGL 二级分配（标签文本缓冲）失败不在页面构建失败判定内

| 项 | 内容 |
|---|---|
| **背景** | ADR-008 的懒建失败路径只能判定**对象分配**（`lv_*_create` 返回 NULL，页内用 `UI_BUILD_CHECK` 检查）。LVGL 标签的文本缓冲在 `lv_label_set_text*` 内部 `lv_malloc`，API 无返回值；分配失败时 LVGL 只打 `LV_LOG_INFO`（默认级别不显示），页面会留下空文本而 `build()` 仍返回成功——不触发错误状态条/`UI ERR page_build`。 |
| **影响面** | 阶段 4 参数页（文本密集：参数名/值/单位标签多）在池临近耗尽时可能出现「个别文本缺失但无错误出口」；现有四页实测余量 13–23%（est Σ=39,424B，四页全建后 free=19,648B），未观察到该现象。 |
| **为何可延迟** | 当前预算余量与共享 `lv_style_t` 纪律下没有触发条件；判定手段需要额外设计（见解决方向），不影响本任务交付与门禁。 |
| **解决方向** | ① 构建末尾做「文本哨兵」：每页把首个标签设成已知串再 `lv_label_get_text()` 比对（失败即当作构建失败删除半成品）；错误状态条自身也属同一类：`UI_ERROR` 文本缓冲分配失败时只剩 ERROR 日志 + console 令牌（画面为无控件页面区，非静默成功；标签对象都建不出时回退 bring-up 测试图），见 ADR-008 §4；② 或在 `theme.h` 提供 `UI_BUILD_TEXT(label, str)` 宏，把「set + 回读非空」合并成一处纪律；③ 诊断期可临时把 `LV_LOG_LEVEL` 调到 INFO 观察 `couldn't allocate memory` 日志。选型须在第一个参数页落地时一并决定（可写入 ADR-008 修订或新 ADR）。 |
| **触发条件** | 第一个参数页落地时；或出现「页面控件在但文本缺失」/`UI MEM` 的 free 低于单页 est 的现象时立即升级处理。 |
| **登记** | 2026-11-21（P4-0 Task 4，ADR-008 §4 已记录该边界） |

## DI-15: `ENG DESC` 枚举名首/末元素回显缺越界兜底（console 诊断边界）

| 项 | 内容 |
|---|---|
| **背景** | Task 8 的 console `ENG DESC` 回显按 `d.enums[d.enum_count - 1]` 取末元素。`enum_count` 对 `F_ENUM` 按 `max - min + 1` 计数（`PE()` 约定，不依赖终止符）；若上游某引擎的 `edit[]` 用了 `F_ENUM` 但 names 表短于值域，console 会读到静态表末尾之外（`sloop_core_desc` 本身只做指针投影，不读元素）。当前 12 引擎 + `DRUM_KIT_DESC` 的名字表长度都与值域一致（`desc-check.py` 10 项抽样 + 4 越界不会覆盖此边界）。 |
| **影响面** | 仅 console 诊断命令（`ENG DESC` 的 `first`/`last` 字段）；引擎参数读写/UI 投影不受影响。上游 `param_desc_t` 变更（新增 F_ENUM 引擎参数且漏名字）时才会触发。 |
| **为何可延迟** | 现状无触发条件；`ENG DESC` 是排障入口，不参与产品行为；修复需要先定「名字表长度可信来源」的口径（终止符 vs 值域），与 Task 10/11 的参数页 desc 扩展同一决策面。 |
| **解决方向** | ① console 侧保守化：先按 0 终止表扫描（上限 64）得到可用长度，`first/last` 只在该长度内取，越界打印 `-`；② 或让 `sloop_core_desc` 返回实际可读的 `enum_count`（F_ENUM 也按终止符校验，不一致时截断并在 desc-check 增加负例）。选型随参数页（Task 10/11）的 desc 扩展一并落地，并补 `desc-check.py` 用例。 |
| **触发条件** | 第一次出现「`ENG DESC` 末枚举名乱码/越界字符串」，或上游新增带 names 的 `F_ENUM` 引擎参数时；最迟在 Task 11（3 引擎页 + desc 收口）前处理。 |
| **登记** | 2026-11-21（P4-A Task 9 评审：由 Task 8 carry 项升级为条目） |

## DI-16: 参数页旋钮的物理触摸手感未人测（接线/相对拖拽已机读覆盖）

| 项 | 内容 |
|---|---|
| **背景** | Task 10 的旋钮控件（`knob.c`）用自绘弧线：按下=焦点+锚点（不改值）、拖动=**相对锚点角度差**改值（270° = 整值域、55..95px 环带）。**评审修复轮已把接线与换算纳入机器门禁**：旋钮纯函数/格式化由 `scripts/knob-test.sh` host 单测覆盖；触摸回调接线与相对拖拽由 console `UI KNOB <slot> press|drag <dx> <dy>` 用合成坐标走同一处理路径、在 `params.sh` 里断言引擎真值（按下不改值、+90° 改值、拖回锚点回原值、环带外忽略）。**仍未覆盖**：真人手指在真机屏上的拖拽手感/命中率（触摸 indev 的坐标变换、按压抖动、多指等）。 |
| **影响面** | 仅交互手感/准确性：接线与换算已被机读门禁钉住；剩余风险是物理触摸链路（indev 坐标映射/按压识别）与手感调参（拖拽灵敏度是否合适）。 |
| **为何可延迟** | 需要真人用手指在屏上拖拽；当前没有可靠的触摸事件注入通道（DI-10 的幽灵按压说明触摸读取路径本身还需观察）。产品化前的人测清单本就包含触摸/视觉/听感项。 |
| **解决方向** | 人测：每个参数页任选 2 个旋钮，按住拖动一圈观察值随角度单调、两端钳位、按中心不跳值、拖回起点回原值；评估 270°=整值域的灵敏度是否可用（如太灵/太钝再调 `KNOB_DRAG_SPAN_DEG`）。若后续需要自动化，给 indev 增加合成 touch down/move/up 注入（需先解决 DI-10）。 |
| **触发条件** | 参数页进入人测/产品化验收（P4-F 全页面矩阵或首次真机手感验收）之前必须完成人测；改拖拽换算（`KNOB_DRAG_SPAN_DEG`/环带）或触摸链路时重测。 |
| **参考** | ADR-008 §8.2/§8.5、`docs/dev/verify/4-p0-2026-11-21/task-10-readme.md` §5、`scripts/knob-test.sh`、console `UI KNOB` |
| **登记** | 2026-11-21（P4-A Task 10） |

## DI-19: 九页全建后 LVGL 池余量 < 弧线 mask 分配 ⇒ 空指针解引用 panic（用户可达）

| 项 | 内容 |
|---|---|
| **背景** | P4-B Task 5 门禁中发现（`docs/dev/verify/4-pb-2026-11-21/task-5-poolcliff-repro.log`）：同一会话里跑 `store.sh → tracks.sh → params.sh`（三脚本都跑完九页懒建）时，`UI step` 把池余量压到 **2,196B**，紧随的 `UI fx` 需要绘制旋钮弧线（`lv_draw_sw_arc`）→ `lv_malloc_zeroed` 拿不到内存（`create_non_cached_mask_radius` 返回 NULL）→ `lv_draw_mask_radius` 未判空直接 `get_next_line(p->circle, …)`（`lv_draw_sw_mask.c:869/1197`）→ `Load access fault`（MTVAL=0x76 = NULL+字段偏移）→ 设备 panic 重启。**同一天同一固件、独立启动后单跑 `params.sh` 是绿的（九页全建 free=2,620）**，说明触发条件是**会话分配历史**（页面访问顺序/状态行文本）决定的池余量，而不是某一条命令；`Task 5` 的换音改动不分配任何 LVGL 内存（boot 池用量 15,716/62,820 与改前逐字节相同），且崩溃发生在 `⑨g` 硬复位之后（引擎状态=工厂 + `PROJ LOAD`，与改前固件相同）。 |
| **影响面** | 产品态：用户在**一次上电会话**里访问全部 9 页（PLAY/TRACKS/STEP/TRANSPORT/4 个参数页/FM6/SAMPLE）后继续操作旋钮，就可能落在池悬崖下并 panic 重启（不是仅测试脚本可达）。ADR-008 已知九页全建余量仅 ~2.6KB，本条目是它的**安全性后果**；`DI-13`（标签文本二次分配失败静默）是同一余量问题的另一面。 |
| **为何可延迟** | 修法绑定在 P4-B Task 11 的导航信息架构（ADR-005）：**组作用域页面生命周期**（切组时销毁源组页面）会把常驻页数从 9 降到 ~2–3，池余量回到 20KB+，本条目的触发条件随之消失；现在就改 `CONFIG_LV_MEM_SIZE` 会碰 ADR-008 的「96KB 链接溢出」结论、且会重排全部 `est/delta` 预算表，收益低于等到 Task 11。在此之前：门禁脚本**每条之间重启设备**（或把 `params.sh` 放在重启后第一条），并把 `UI MEM` 的 `pool_free` 当作硬判据（`params.sh` 已有「九页全建后 free>0」，但 >0 不足以覆盖一次弧线绘制）。 |
| **解决方向** | (a) 结构性：Task 11 组作用域生命周期（首选，ADR-005 §组作用域）；(b) 余量判据：`params.sh`/soak 的池判据从「free>0」改为「free ≥ 单次绘制峰值（实测 ≥2.6KB，建议 ≥4KB）」；(c) 若 TM 仍要九页常驻：给 `knob_draw` 的弧线加预算检查（`lv_mem_monitor` free 不足时退化为矩形/纯文本），或在 `managed_components` 的 LVGL 打「NULL mask → `LV_DRAW_SW_MASK_RES_TRANSP`」补丁（与 ADR-004 零修改原则冲突，需负责人裁决）；(d) 长期：升级 LVGL 到已判空的上游版本并复核该路径。 |
| **触发条件** | Task 11（导航 IA / 组作用域页面生命周期）落地时必须解决并加「九页（或新分组）全建 + 逐页绘制弧线」的注入用例；或在此之前任何一次「全页访问后操作旋钮」的现场 panic 复现。 |
| **参考** | `docs/dev/verify/4-pb-2026-11-21/task-5-poolcliff-first.log`（首次 Core 0：MTVAL=0x76）、`task-5-poolcliff-repro.log`（故意复现，Core 1 同点）、`task-5-poolcliff-symbols.txt`（addr2line：`get_next_line ← lv_draw_mask_radius ← lv_draw_sw_mask_apply ← lv_draw_sw_arc ← knob_draw`）、`task-5-params-fresh.txt`（独立启动单跑绿，free=2,620）、ADR-008 §3/§5（池预算与 9 页余量）、`firmware/managed_components/lvgl__lvgl/src/draw/sw/lv_draw_sw_mask.c:869,1053,1197` |
| **登记** | 2026-11-21（P4-B Task 5 门禁现场发现；Task 5 的换音改动本身不分配 LVGL 内存，boot 池用量与改前相同） |

## DI-20: `ENG SELFTEST` 之后实时状态被回滚到工厂态（工作工程自动存档 / FM6 银行读取钩子）

| 项 | 内容 |
|---|---|
| **背景** | 机制（代码推导，非现场观测）：`ENG SELFTEST` 跑完会调 `selftest_restore()`，把本 TU 的 `.data/.bss`（`linker.lf` 的 `SURROUND(sloop_selftest_bss)` 窗口）与 DSP 状态**回滚到 init 快照**——自检本义就是「从工厂态重跑 golden」。于是自检结束时：①引擎的实时状态（`trk[].p[]/step[]`、`song.g[]`）= 工厂态；②自动存档的「上次写过的 hash」`s_auto_hash` 在 `.noinit`（ADR-009 §8，正是为了不被 restore 清零），仍是自检前那个用户工程的 hash。两者不同 ⇒ 自检结束后的第一个满足门限的窗口（idle 2.5s + gap 20s + 无发声）会把工厂态 `st_save` 进 `OBJ_AUTOSAVE`，覆盖用户上次的工作工程；下次上电 `autosave_resume` 恢复的就是工厂态。**同一回滚也清掉 FM6 银行的读取钩子**（Task 7 评审 R25）：`eng_fm6.c` 的 `fm6_bank_read` 指针与 `fm6_bank.c` 的 `fm6_bank_cur/used` 都在同一快照窗口内，只有 `fm6_bank_boot()` 会重新安装它们——而它只由 `sloop_store_init`（上电）与 bank 公共入口（`STORE BANK …` / `sloop_store_bank_*`，ADR-009 §8 的入口重扫）调用。故**自检之后、下一次 bank API 调用或重启之前**，PTCH ≥ 8 的轨载入的是 `FM6_INIT`（钩子为 NULL 时的兜底）而不是银行槽里的 patch；bank 对象本身与 flash 字节不受影响，入口重扫后即恢复（`store` 场景 ⑫d 已覆盖）。 |
| **影响面** | 只有诊断入口 `ENG SELFTEST`（`device-selftest.py` 门禁也会跑它）。正常使用（上电/编辑/播放/切页）不触发。后果有两级：①「跑一次自检 = 用户工作工程被工厂态覆盖」，掉电重启后回到工厂态；②「跑一次自检 = 引用银行槽的音色改为播 init voice」，直到下一次 `STORE BANK …`（或任何 `sloop_store_bank_*` 调用）或重启。对开发与门禁无害（脚本自己控制 `STORE AUTO`，bank 段的断言都经 bank 入口），对用户是静默数据/音色变化。 |
| **为何可延迟** | 自检是**诊断命令**、不在用户操作路径上；P4-B 的注入门禁都在脚本内显式 `STORE AUTO OFF`（并已改为「每次硬复位后重新下发」）。修法牵涉一个产品判断（自检算不算一次会话语义上的「新状态」），与 P4-E 的设置/会话层同批做更合适；现在改会把「诊断命令」与「存储语义」两件事耦在一起。 |
| **解决方向** | 三选一（P4-E 决策时记入 ADR）：(a) `selftest_restore()` 尾部用 restore 后的引擎状态 `proj_capture` 并刷新 `s_auto_hash`（= 自检结果不进 autosave，最省事）；(b) 自检结束时把 `s_auto_on` 置 0 且**不**恢复（用户须显式 `STORE AUTO ON`）；(c) 自检前把 autosave 对象备份到暂存、结束时写回（成本高，不推荐）。优先 (a)，并在 `store.sh` 的 ⑨f' 之后加一条「自检后 `autosave_hash` 不变」的断言；②的修法同批：`selftest_restore()` 之后补一次 `fm6_bank_boot()`（重新安装 `fm6_bank_read` 并扫 used 位），或在 `sloop_core_selftest` 收尾处调 bank 入口的重扫钩子。 |
| **触发条件** | 用户在设备上跑过 `ENG SELFTEST`（含 `device-selftest.py` 门禁）之后继续使用并掉电重启；或 P4-E 处理「自检/会话」语义时一并解决。 |
| **参考** | ADR-009 §5（自动存档阈值与启动顺序）、ADR-009 §8（`.bss` 快照窗口与 bank 入口重扫、R24 裁决）、`docs/dev/verify/4-pb-2026-11-21/task-7-mutation-probe.txt`（变异探针：删掉入口重扫后 selftest 之后的写丢同银行其它槽）、`firmware/components/sloop_core/src/store_tab5.c` 的 `sloop_store_autosave_tick`/`sloop_store_autosave_resume`、`firmware/components/sloop_core/src/selftest.c` 的 `selftest_restore()` 与 `.noinit` 说明、`firmware/components/sloop_core/linker.lf`（SURROUND 窗口） |
| **登记** | 2026-11-21（P4-B Task 6 落地时按代码推导确认；Task 6 评审 R21 修正措辞——不声称设备实测）；2026-11-22 Task 7 评审 R25 补入「FM6 读取钩子同批回滚」一项（标题与影响面扩为两级症状） |

## DI-21: `STORE TEST` 场景 ⑤ 的 1 字节 prog 依赖未加密分区（flash encryption 下会响亮失败）

| 项 | 内容 |
|---|---|
| **背景** | Task 8 的 torn-write 套件用 `sloop_hal_storage_prog(a + ST_PAYLOAD_OFF, &zero, 1)` 把当前副本 payload 的首字节改成 0x00（NOR prog 只能 1→0），制造「头合法、CRC 失配」的损坏。ESP-IDF 的 `esp_partition_write` 在**未加密**数据分区上允许任意偏移/长度（本机实测 `STORE TEST PASS 6/6`）；打开 flash encryption 后加密写按 16/32 字节块处理，未对齐/非整块的写会被拒绝或断言，场景 ⑤ 会返回失败 ⇒ `STORE TEST FAIL corrupt`。 |
| **影响面** | 只有诊断套件（`STORE TEST`），且仅在启用 flash encryption 的构建上。失败是**响亮**的（`corrupt` 场景 FAIL，不是假绿），不影响用户数据路径；但会挡住整条 torn-write 门禁。 |
| **为何可延迟** | P4-B–E 的 `partitions.csv`/`sdkconfig` 均未启用 flash encryption；这是「未来构建开关」的兼容性问题，不是当前缺陷。 |
| **解决方向** | (a) 把损坏写改成对齐的整块（如从 `ST_PAYLOAD_OFF` 起写 16 字节、内容取反原值的位与），保持 1→0 语义；(b) 或在 `sloop_store_test` 入口检查 `esp_flash_encryption_enabled()` 并跳过场景 ⑤（同时把 `cases` 降为 5、console 打印明确令牌，不能静默少跑）；(c) 或者改用 `st_erase` + 重写成不同内容来制造 CRC 失配（但会失去「只坏 payload、头仍合法」的精确性）。优先 (a)。 |
| **触发条件** | 任何启用 flash encryption（或安全启动配套的加密分区）的构建/发布前必须解决；否则 `STORE TEST` 在该构建上必然红。 |
| **参考** | `firmware/components/sloop_core/src/store_tab5.c` 的 `store_test_corrupt`、`sloop_hal_storage_prog`（`storage_flash.c`）、ESP-IDF `esp_partition_write` 的加密对齐要求、`docs/dev/verify/4-pb-2026-11-21/task-8-store-green1.txt`（未加密实测 PASS） |
| **登记** | 2026-11-22（P4-B Task 8 评审 minor 4a） |

## DI-22: `STORE FAULT` / `STORE RAWTEST` 不持 `s_store_lock`（autosave tick 可消费 arm）

| 项 | 内容 |
|---|---|
| **背景** | `STORE FAULT <nth> [half]` 只写 HAL 的原子计数（`sloop_hal_storage_fault_arm`），`STORE RAWTEST` 直接调 `sloop_hal_storage_erase/prog`；两者都不取 `s_store_lock`。若 `STORE AUTO` 为 ON 且 1s 自动存档任务在「arm」与「目标写命令」之间命中门限，tick 的 `st_save(OBJ_AUTOSAVE)` 会消耗这次 arm（成为第 nth 次写），目标命令就在没有故障的情况下执行——脚本会以为测了掉电窗口，其实没有。 |
| **影响面** | 仅诊断/注入路径：`scripts/inject/store.sh` 的 ⑬a 与 HAL 探针在每一步前都先 `STORE AUTO OFF`（且套件入口自己 `fault_clear`），所以当前门禁不受影响；风险是**未来新脚本忘记关 AUTO** 时静默测错。用户路径不使用 `STORE FAULT`（非用户命令），无产品影响。 |
| **为何可延迟** | ①所有现有注入脚本在 arm 前显式 `STORE AUTO OFF`（AGENTS §5.1 已写）；②`STORE TEST` 内部 arm+目标写都在同一把 `s_store_lock` 内完成，天然不受 tick 影响；③加锁方案会改变 console 诊断语义（`STORE FAULT` 是「跨命令」的预置动作，单次加锁无法覆盖 arm 与后续命令之间的窗口），需要先把「故障作用域」设计清楚。 |
| **解决方向** | (a) 引入显式的故障作用域：`STORE FAULT ARM` 后禁止 autosave（或把 `s_busy`/`s_auto_on` 临时置 0），由 `STORE FAULT CLEAR`/超时解除；(b) 或让 HAL 在 `s_busy` 时不消费 arm（把 tick 的写排除在计数外）；(c) 或在 `STORE RAWTEST`/`STORE FAULT` 帮助与回显里强制提示「先 `STORE AUTO OFF`」并在 arm 时检查 `s_auto_on`（回显警告/拒绝）。优先 (a) 或 (c)。 |
| **触发条件** | 任何新增「先 arm、后由另一条命令触发写」的注入场景；或自动存档阈值/任务周期调整让 tick 更容易插入命令之间。 |
| **参考** | `firmware/main/console.c` 的 `store_fault`/`store_rawtest`、`firmware/components/sloop_hal_tab5/src/storage_flash.c`（计数只由 erase/prog 递增）、`store_tab5.c` 的 `sloop_store_autosave_tick`（`store_trylock`/`s_busy`）、`scripts/inject/inject.py` 的 ⑬a |
| **登记** | 2026-11-22（P4-B Task 8 评审 minor 4b） |
