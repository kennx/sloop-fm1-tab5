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
| **影响面** | 参数页（文本密集：参数名/值/单位标签多）在池临近耗尽时可能出现「个别文本缺失但无错误出口」。P4-B 交付态（组作用域，ADR-005 §5）：稳态只驻留一个组（`UI MEM` 的 pool_free 约 26–45KB），最坏过渡 FILE 全建 → 首建 STEP 实测 free 7,308–7,840B，仍未观察到该现象；余量最小时是跨组切换的那一帧。 |
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
| **触发条件** | 第一次出现「`ENG DESC` 末枚举名乱码/越界字符串」，或上游新增带 names 的 `F_ENUM` 引擎参数时。Task 11（3 引擎页 + desc 收口）已交付而未触发（`desc-check.py` 的 10 项抽样与 4 条越界不覆盖此边界，上游名字表当前与值域一致），故按「上游变更/现场乱码」触发。 |
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

## DI-20: `ENG SELFTEST` 之后实时状态被回滚到工厂态（工作工程自动存档 / FM6 银行读取钩子）

| 项 | 内容 |
|---|---|
| **背景** | 机制（代码推导，非现场观测）：`ENG SELFTEST` 跑完会调 `selftest_restore()`，把本 TU 的 `.data/.bss`（`linker.lf` 的 `SURROUND(sloop_selftest_bss)` 窗口）与 DSP 状态**回滚到 init 快照**——自检本义就是「从工厂态重跑 golden」。于是自检结束时：①引擎的实时状态（`trk[].p[]/step[]`、`song.g[]`）= 工厂态；②自动存档的「上次写过的 hash」`s_auto_hash` 在 `.noinit`（ADR-009 §8，正是为了不被 restore 清零），仍是自检前那个用户工程的 hash。两者不同 ⇒ 自检结束后的第一个满足门限的窗口（idle 2.5s + gap 20s + 无发声）会把工厂态 `st_save` 进 `OBJ_AUTOSAVE`，覆盖用户上次的工作工程；下次上电 `autosave_resume` 恢复的就是工厂态。**同一回滚也清掉 FM6 银行的读取钩子**（Task 7 评审 R25）：`eng_fm6.c` 的 `fm6_bank_read` 指针与 `fm6_bank.c` 的 `fm6_bank_cur/used` 都在同一快照窗口内，只有 `fm6_bank_boot()` 会重新安装它们——而它只由 `sloop_store_init`（上电）与 bank 公共入口（`STORE BANK …` / `sloop_store_bank_*`，ADR-009 §8 的入口重扫）调用。故**自检之后、下一次 bank API 调用或重启之前**，PTCH ≥ 8 的轨载入的是 `FM6_INIT`（钩子为 NULL 时的兜底）而不是银行槽里的 patch；bank 对象本身与 flash 字节不受影响，入口重扫后即恢复（`store` 场景 ⑫d 已覆盖）。 |
| **影响面** | 只有诊断入口 `ENG SELFTEST`（`device-selftest.py` 门禁也会跑它）。正常使用（上电/编辑/播放/切页）不触发。后果有两级：①「跑一次自检 = 用户工作工程被工厂态覆盖」，掉电重启后回到工厂态；②「跑一次自检 = 引用银行槽的音色改为播 init voice」，直到下一次 `STORE BANK …`（或任何 `sloop_store_bank_*` 调用）或重启。**门禁侧已防护**（Task 14 终审）：`device-selftest.py` 在自检前下发 `STORE AUTO OFF` 且**不恢复**，门禁会话的工作工程 autosave 不再被工厂态覆盖（bank 段断言本来就走 bank 入口）；用户手动跑 `ENG SELFTEST` 仍会踩到①的两级静默变化。 |
| **为何可延迟** | 自检是**诊断命令**、不在用户操作路径上；P4-B 的注入门禁都在脚本内显式 `STORE AUTO OFF`（并已改为「每次硬复位后重新下发」），`device-selftest.py` 也已按 (b) 的变体在自检前关 AUTO 且不恢复。产品侧（用户手动自检）的语义仍待 P4-E 决策。修法牵涉一个产品判断（自检算不算一次会话语义上的「新状态」），与 P4-E 的设置/会话层同批做更合适；现在改会把「诊断命令」与「存储语义」两件事耦在一起。 |
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

## DI-26: `ENG SELFTEST` 每次全量运行泄漏 ~40KB 内部堆（重复自检后 19KB，音频重启超时）

| 项 | 内容 |
|---|---|
| **背景** | P4-B Task 14 收口矩阵（注入六场景 → `device-selftest` → soak 同一会话）暴露：**每次全量 `ENG SELFTEST`（182 例）净减 ~40KB 内部堆**，且不随重启前的其它命令出现。实测 2026-11-22（固件 `6167039-dirty`）：复位后 `internal_free=99,739` → 全量 #1 后 `59,443` → 全量 #2 后 `19,323`；两次后 `ENG AUDIO ON` 超时（收 14 字节无提示符），需重启恢复。单用例重复 20 次 `-4,612B`（≈230B/执行 job）；未知用例（0 job）每次 ~40B；音频 OFF/ON ×3 与普通命令（`ENG MEM`/`ENG PERF`/`ENG SNAP`）无此量级变化——泄漏只在 regress job 执行路径上。同一会话里紧随其后的 soak 因此跌破 60KB 门禁（59,355B）；**复位后同一 soak 命令 PASS**（99,211→99,263B）。测量留档：`docs/dev/verify/4-pb-2026-11-21/task-14-selftest-heap-leak.txt`、`task-14-soak-fail-after-selftest*.txt`。 |
| **影响面** | 仅诊断入口 `ENG SELFTEST`（含 `scripts/device-selftest.py` 门禁）。正常使用（上电/编辑/播放/切页/存储）不触发。后果：①**门禁口径**——同一开机内「selftest → soak」不能连跑（soak 的 HEAP_FLOOR 会假红，除非复位）；②**设备可用性**——连续两次全量自检后内部堆 19KB，音频流无法重启（`ENG AUDIO ON` 超时），需断电/复位；③单次自检后仍有 59KB，日常一次诊断不受功能影响，但已低于 60KB 门禁线。 |
| **为何可延迟** | 不在用户操作路径上；根因未定位（本条目按实测登记，不做猜测性修法）。门禁已用「selftest 之后复位再 soak」的矩阵顺序规避（AGENTS §5.1/§5.5 已注明），证据完整。修法涉及 selftest 快照/恢复与堆所有权（P4-B 起 store TU 编入同一快照窗口），需要单独一轮带堆跟踪的排查，避免在收口任务里盲改。 |
| **解决方向** | ① 先定位：对 `ENG SELFTEST` 单用例做堆跟踪（`heap_caps_get_info` 前后快照 / `esp_heap_trace` 或二分 job 名单），确认是 regress job 内的库调用（如 `snprintf("%.2f")` 的 newlib 缓冲）还是 `selftest_restore()` 回滚了某个堆相关的全局；② 若为恢复回滚：把「自检期间分配、恢复后仍被使用的指针」移出快照窗口（`.noinit` 或重扫），参照 Task 7 的 `fm6_bank_boot`/`up_boot` 入口重扫；③ 若为库缓冲：改为定长格式化或预热；④ 无论修法，补一条门禁断言：连续两次全量 `ENG SELFTEST` 后 `internal_free` 下降 ≤1KB。 |
| **触发条件** | 任何「同一开机内先跑 `ENG SELFTEST` 再跑 soak/长时观测」的门禁或演示；或需要连续多次自检的排障会话。**P4-F 收口前必须处理**（否则每条矩阵都要额外复位，且连续自检会把内部堆降到 19KB、音频无法重启）。 |
| **参考** | 实测记录 `docs/dev/verify/4-pb-2026-11-21/task-14-selftest-heap-leak.txt`、失败 soak `task-14-soak-fail-after-selftest*.txt`、`firmware/components/sloop_core/src/selftest.c`（`selftest_restore`/快照窗口/`selftest_fork`）、`third_party/sloop-fm1/tests/regress.c`（job 的 `calloc/free` 与浮点 `snprintf`）、ADR-009 §8（`.bss` 快照窗口与入口重扫） |
| **登记** | 2026-11-22（P4-B Task 14 收口矩阵：同一会话 selftest → soak 的 heap floor 失败；复位后 soak PASS） |

## DI-27: 一次性 take 结果（`take_bars`）在设备脚本里不可稳定观测

| 项 | 内容 |
|---|---|
| **背景** | P4-C Task 6 按 spec/brief 让 `sloop_seq_rec_read` **消费**上游 `ft_bars`/`rec_go`（读即清零），`state_sync_from_engine` 每 30ms 读一次并把结果写进 `app_state_t.rec`。UI 定时器同一拍先 sync 再 draw，所以界面能看到；但 console `SEQ REC`/`STATE SNAP` 在注入命令到达时通常已晚于下一次 sync，读到 `take=none`/`rec_bars=0`（实测不稳定的窗口是 30ms + 串口往返 ~50ms）。 |
| **影响面** | 仅**观测/注入**面（Task 12 的 sequence 场景、将来的 take 提示 UI 若要「保持显示到用户确认」）。录音与 free-take 的实际结果（LEN/BPM/步内容/播放）不受影响；音频与状态机无影响。 |
| **为何可延迟** | 效果可由持久量断言（`ENG STEP`/`ENG SNAP`/`STATE SNAP bpm`/`PARAM P_SLEN`），不需要 one-shot 令牌；UI 的消费顺序正确。当前 30ms 窗口对人工使用无感。 |
| **解决方向** | (a) 把 take 结果 latch 到 `s_status`-style 的投影（`take_bars` + 一个 generation/`last_take` 字段），由 UI 显示后显式清；或 (b) console `SEQ REC` 读投影而不再直接消费快照（投影仍会被 sync 覆盖，需配合 (a)）；或 (c) 增加 `SEQ REC LAST` 保存最近一次 take 的环形记录。优先 (a)，代价是 `sloop_rec_snapshot_t` 加一个 generation 字段（POD 变化需同步 host 测试）。 |
| **触发条件** | Task 12 注入场景若需要断言「本次 take 是 1 bar / 被丢弃」；或 UI 需要把 take 结果做成可停留的提示。 |
| **参考** | `firmware/components/sloop_core/src/seq_tab5.c` 的 `sloop_seq_rec_read`、`firmware/components/sloop_state/src/state.c` 的 `state_sync_from_engine`、`docs/design/010-sequencer-arranger.md` §9 |
| **登记** | 2026-11-22（P4-C Task 6 评审 minor：take 结果观测竞态）；2026-11-23 Task 12/13：`sequence` 场景按**持久效果**（LEN/步表/播放/`ENG STEP`）断言、不读 one-shot 令牌，最终矩阵绿——条目保留（触发条件未消失：任何需要断言「本次 take 是 N bar / 被丢弃」的新脚本或 UI 提示仍会踩到） |

## DI-28: STEP 鼓 lane 位编辑是「30ms 投影读 + 整份 POD 写」，录音并发可能丢一位

| 项 | 内容 |
|---|---|
| **背景** | P4-C Task 9 的鼓格编辑按 brief 走 `CMD_DRUM_STEP_WRITE`：UI 从 `app_state_t.core.dstep_raw[c]`（每 30ms `state_sync_from_engine` 取的窗口投影）拷出整份 16 lane 字节，翻一位后把整份 POD 交给状态层写回。读（快照）与写（adapter 的引擎锁内 memcpy）是两次独立的引擎锁获取；若实时录音（`sloop_seq_drum_hit` → 上游 `drum_input/rec_hit`）在两者之间写了同一步的其他 lane，整份写回会把新录的位覆盖掉。合成轨的 `CMD_TOGGLE_STEP` 也是「先读步再写」的同类窗口（`cmd_toggle_step` 的 `sloop_core_read_step` + `write_step`/`SEQ_STEP_CLEAR` 两次取锁），只是它读的是紧邻的引擎真值而非 30ms 投影。**P4-C Task 10 fix round 1 扩展**：TRANSPORT 页鼓轨的 LVL/RAT 字段（`tr_action` 的 `SLOOP_TR_LVL_*`/`SLOOP_TR_RAT_*` 鼓分支）走同一条路径——从 30ms 投影 `dstep_raw[fs]` 读整份 POD、改选中 lane 的 2-bit 字段、`CMD_DRUM_STEP_WRITE` 整份写回；同一窗口同样会把并发录音写进同一步其他 lane 的 hit 覆盖掉（合成轨的 LVL/RAT 走 `CMD_STEP_EDIT`，adapter 单锁读改写，不受影响）。 |
| **影响面** | 只有「同一鼓步正被另一个写入者（实时录音/自动化）修改」且用户同时点/按该步 lane 位（或 TRANSPORT 改该步某个 lane 的 LVL/RAT）的窄窗口；丢的是新录的一位鼓 hit（下一次录音或再点可恢复）。无音频/状态机/持久化影响；注入脚本串行执行，不受影响。 |
| **为何可延迟** | brief 明确要求复用 `CMD_DRUM_STEP_WRITE`（整份原始 POD）；改成单锁 RMW 需要新增 adapter 入口（`sloop_seq_toggle_dstep_lane` 之类）+ 状态层命令 + host/state 门禁用例，属于接口扩张而不是本 UI 任务的完成条件。现有同类窗口（合成轨 toggle）也未被专门处理，修法应一次覆盖两者而不是只修鼓。 |
| **解决方向** | (a) 在 `seq_tab5.c` 增加「引擎锁内读-改-写单 lane」的窄 adapter（`on` 位、`lvl`、`rat` 三种字段都按 lane 参数化），状态层新增一条命令（返回新值供 TAP 回显），STEP 鼓格与 TRANSPORT LVL/RAT 都改调它；(b) 或让状态层在**引擎锁内**完成 read-modify-write（把当前 `sloop_seq_read_dstep` + `write_dstep` 合成一个入口，避免投影陈旧）；(c) 顺带把合成轨 toggle 改成同一形态（上游 `CMD_TOGGLE_STEP` 的读改写在 adapter 内一次取锁）。优先 (b)+(a)，可同时消灭三处窗口。 |
| **触发条件** | 任何「录音中进行步/lane 编辑」的门禁或人测场景；或 Task 12 的 sequence 场景要在 REC 进行中点击鼓格。 |
| **参考** | `firmware/components/sloop_ui/src/page_step.c` 的 `drum_toggle`、`firmware/components/sloop_ui/src/page_transport.c` 的 `tr_action`（鼓轨 LVL/RAT 分支）、`firmware/components/sloop_state/src/cmd.c` 的 `cmd_drum_step`/`cmd_toggle_step`、`firmware/components/sloop_core/src/seq_tab5.c` 的 `sloop_seq_read_dstep/write_dstep`、ADR-010 §6 |
| **登记** | 2026-11-22（P4-C Task 9 评审 minor：鼓 lane 位编辑的读改写窗口）；2026-11-22 Task 10 fix round 1 扩展到 TRANSPORT 鼓 lane LVL/RAT |

## DI-29: `ENG SELFTEST` 汇总行可能被并发 `ESP_LOG` 从中间插入，device-selftest 假红

| 项 | 内容 |
|---|---|
| **背景** | P4-C Task 9 设备复验时，同一固件（`192b4fe`）两次全量自检里有一次出现：`ENG SELFTEST cases=182 identical=182 mismatch=0 health_fail=0 checks=7 check_fail=0 deterministic=1 expected=182 expected_checks` 被紧随其后的 `I (...) sloop_ui: 诊断期静默 OFF：…` 日志行**插入到同一物理行**；`scripts/device-selftest.py` 的 `SUMMARY = ^ENG SELFTEST cases=` 命中后立即停止读取，尾部 `expected_checks=7 main_rc=1 result=PASS` 丢失，`summary_fields()` 取不到 `result` ⇒ 打印 `DEVICE SUMMARY result=None`、`result=FAIL`、exit 1。另一次运行汇总行完整，182/182 PASS；截断那次设备侧计数也全部是 182 identical / mismatch 0 / deterministic 1（只缺尾字段）。证据：`.superpowers/sdd/2026-11-22-p4c-sequencer-arranger/task-9-selftest-interleaved.log`（摘要 `task-9-selftest-interleaved-summary.txt`）。 |
| **影响面** | 仅 `scripts/device-selftest.py` 门禁（和按日志 grep 的排障）：会把「设备实际 PASS」报成 FAIL，导致阶段矩阵假红/需要重跑。自检本身、引擎与音频无影响。 |
| **为何可延迟** | 概率低（当前只在同一行写入窗口撞上 UI quiesce OFF 日志时发生），重跑即可恢复；修法要么改门禁脚本的读取/解析协议，要么改 console 输出原子性，二者都需要单独验证，不宜在 UI 任务里顺手改门禁工具。 |
| **解决方向** | (a) 脚本命中 `SUMMARY` 前缀后继续读到 `sloop>` 提示符，把同一物理行的续行合并后再解析（并显式校验 `cases==expected`、`mismatch==0`、`deterministic=1`）；`result=` 缺失时按「计数一致 + 显式 WARN」处理而不是直接 FAIL。(b) 固件把汇总行放到没有并发 UI 日志的窗口（quiesce OFF 之前/之后若干拍），或用一次 `write()` 输出整行。(c) 给 console 输出加行锁/行缓冲（影响面最大）。优先 (a)+(b)，但门禁仍须保留 `result=PASS` 与 `identical==expected` 两条判据。 |
| **触发条件** | 任何 `ENG SELFTEST` 门禁；UI 定时器恢复日志、存储 tick 等并发 `ESP_LOG` 恰好落在汇总 `printf` 写入中途。 |
| **参考** | `scripts/device-selftest.py`（`SUMMARY`/`summary_fields`/`compare`）、`firmware/components/sloop_core/src/selftest.c`（汇总 printf）、`firmware/components/sloop_ui/src/ui.c`（quiesce OFF 日志）、DI-26（自检堆泄漏） |
| **登记** | 2026-11-22（P4-C Task 9 设备复验：同一固件两次自检一次假红） |

## DI-30: TRANSPORT 聚焦步字段没有「音符光标」与精确 span 读口

| 项 | 内容 |
|---|---|
| **背景** | P4-C Task 10 的聚焦步字段（LVL/RAT/SPAN/SLIDE）在 TRANSPORT 页编辑，数据源是 `app_state_t.core.step[ft][fs]`（窗口投影）。① STEP 页没有「和弦里第几个音符」的光标（上游 `ui.note_k`），所以 LVL/RAT 固定作用在**第 1 个音符**（`index=0`），和弦里其余音符的 2-bit 字段没有 UI 入口；② 快照不含上游 2.8.3 的 POLY span（音符长到后续和弦下方时以 CONT 延续），SPAN± 只能以**可见 TIE 链长度**为基准，POLY 场景下目标值可能短于实际 span。 |
| **影响面** | 只在多音符和弦的 LVL/RAT 与「长音压在后续和弦下方」的 SPAN 上出现近似；单音符步、无 CONT 的普通长音完全准确。引擎/协议/持久化无影响（写的仍是上游字段，只是目标 index/长度由 UI 近似给出）。 |
| **为何可延迟** | 两项都需要新的状态投影（音符光标 = UI 局部态 + 快照里逐音符可读的 span），属 UI/快照接口扩张；当前 STEP 页的深度呈现（Task 9）已能看见每音符 lvl 与 CONT 标记，用户可用 `SEQ EDIT LVL/RAT/SPAN` 诊断面做精确编辑。 |
| **解决方向** | (a) STEP/TRANSPORT 增加音符光标（上游 `note_k` 语义：KNOB1 走音符，含空步），TRANSPORT 的 LVL/RAT 用它当 `edit.index`；(b) `sloop_core_snapshot_t` 增每音符 span（或 `sloop_seq` 加一个只读 `sloop_seq_note_span(track, step, index)`，引擎锁内算 `span_len`），SPAN± 以它为基准；(c) 两项都要补 state/注入用例（和弦 k=1 的 LVL、CONT 长音的 span 往返）。 |
| **触发条件** | 用户报告「和弦里第二个音的力度改不了」；或 sequence 场景要覆盖 POLY span 的 UI 编辑；或 SONG/sequence 场景需要按音符编辑。 |
| **参考** | `firmware/components/sloop_ui/src/page_transport.c`（`tr_action` 的 LVL/RAT/SPAN）、`firmware/components/sloop_core/src/seq_tab5.c`（`span_len`/`span_set`）、`third_party/sloop-fm1/firmware/src/ui.c`（`ui.note_k`/`note_span`）、ADR-005「TRANSPORT 录音、fill 与 pattern 工具区」§2 |
| **登记** | 2026-11-22（P4-C Task 10：聚焦步字段的已知近似） |

## DI-31: `SEQ_STEP_CLEAR` 对孤立 TIE 步是 no-op（上游 `step_clear` 会写回 REST）

| 项 | 内容 |
|---|---|
| **背景** | 适配层 `sloop_seq_step_edit(SEQ_STEP_CLEAR)` 的实现是「while `step_on` → `note_del`，再清 lock/micro/fill」；`step_on` 要求 `n>0 && time==ST_NOTE`，所以对**只有 `time=ST_TIE`、n=0 的孤立 TIE 步**循环一次都不进，time 仍停在 TIE。上游 `ui.c` 的 `step_clear()` 是 `memset(st,0)` + `time=ST_REST`，任何步（含 TIE 尾巴）都回到 REST。 |
| **影响面** | 用户在 `SEQ EDIT CLEAR <track> <step>` 或状态层 `STEP_CLEAR` 上清一个孤立 TIE 步时看不到变化；锚点步被单独清空后留下的 TIE 尾巴不会被同一条命令顺手清掉，后续 SPAN 以可见 TIE 链为基准时会多算（Task 11 `transport` 场景首轮中断后复跑踩到：SPAN 基点从 1 变 2）。正常 UI 路径（STEP 格 toggle）对 TIE 步按「空步 → 开音」处理，不直接依赖 CLEAR；`PATTERN CLEAR`（整轨）会整份 `step_clear`，不受影响。 |
| **为何可延迟** | 不是 Task 11 引入的行为，改动会触碰 P4-C Task 4 已冻结的 CLEAR 语义与 `seq-test.sh` 的既有断言；当前唯一可复现入口是「锚点步被清空留下 orphan TIE + 后续 STEP_CLEAR」这一窄路径，且注入脚本已用「开一步再清」自愈。 |
| **解决方向** | 把 `SEQ_STEP_CLEAR` 改成上游 `step_clear` 口径：`memset(&t->step[step],0)` + `time=ST_REST`（保留 LEN），补 `seq-test` 用例「孤立 TIE 步 CLEAR → time=REST」与 state/注入断言；确认 `PATTERN CLEAR` 与 undo 标记不被重复 memset。 |
| **触发条件** | 用户报告「清不掉某个 TIE 步」；或 Task 12 的 `sequence` 场景在中断复跑时需要稳定的 CLEAR 归一；或任何改动 CLEAR 语义的任务。 |
| **参考** | `firmware/components/sloop_core/src/seq_tab5.c`（`SEQ_STEP_CLEAR` 分支）、`third_party/sloop-fm1/firmware/src/ui.c`（`step_clear`）、`scripts/inject/inject.py` 的 `run_transport_tools`（归一注释） |
| **登记** | 2026-11-23（P4-C Task 11：中断复跑时发现；本轮只做脚本自愈，不改 CLEAR 语义）；2026-11-23 Task 12/13：`sequence` 的 ⓪ 归一与 DI-31 用例改用「开一步再清」，最终矩阵绿——CLEAR 语义本身仍未改，条目保留 |

## DI-32: `STORE RAW` 只能写 project 对象，设置对象（PER1/2/3/side）无法从设备注入

| 项 | 内容 |
|---|---|
| **背景** | `STORE RAW END <obj> <slot>` 的对象名解析只接受 `project`（`store_tab5.c` 的 raw 上传入口与 `scripts/store_raw.py --obj`）；`OBJ_SETTINGS`（PER1/PER2/PER3 + side）没有注入通道，故 P4-C 的 settings 迁移/损坏路径只有 host 覆盖（`scripts/persist-host-test.c`），设备侧无法上传旧设置 fixture、也无法用损坏设置验证「加载拒绝且原对象不覆盖」。 |
| **影响面** | 仅门禁覆盖：PER1/2 迁移的设备侧证据缺失；产品路径（`song_set` → 延迟 `st_save3`）不受影响，`STORE SONG`/`STORE TEST` 已覆盖设备写入与 A/B+CRC。 |
| **为何可延迟** | 迁移逻辑是纯字节函数（`src/song_persist.h`），host 侧真实 `storage.c` + 8KB 假 flash 已逐字节回归；设备侧缺的是「同一函数在真 flash 上跑」的冗余证明。Task 13 收口矩阵不依赖它。 |
| **解决方向** | 给 `STORE RAW END` 增加 `settings` 对象名（映射 `OBJ_SETTINGS`，走 `st_save3` 的 A/B+CRC 路径），写后按 `song_reload` 语义重载；`scripts/store_raw.py --obj settings`；在 `store` 或 `sequence` 场景加一条「上传 PER2 → `STORE SONG INFO kind=per2` → `SAVE` → `INFO kind=per3`」与一条「上传截断 settings → `SAVE` 回 `invalid` 且 `INFO` 不变」。 |
| **触发条件** | 需要设备级证明 settings 迁移/损坏路径时；或 P4-E 改动 `OBJ_SETTINGS` 布局（theme/lights/MIDI 尾段）后重跑 song 持久化门禁时。 |
| **参考** | `firmware/components/sloop_core/src/store_tab5.c`（`sloop_store_raw_*`）、`scripts/store_raw.py`、`scripts/persist-host-test.c`、`docs/design/009-storage-backend.md` §6.1 |
| **登记** | 2026-11-23（P4-C Task 13 收口审计：Task 7 报告 §6.2 遗留） |

## DI-33: warm reset 后 live section 不保留（上游保留 live section 及其 dirty 延迟写语义）

| 项 | 内容 |
|---|---|
| **背景** | 上游在 warm reset 后保留 RAM 中的 live section（并靠 dirty 延迟写补落盘）；本固件 `sloop_store_init` 先按四槽 fetch 覆盖 RAM 槽，再进入正常使用——捕获后若在 1s 任务的门限（停走带 + 无发声 + idle ≥1.5s + gap ≥5s）之前发生软复位/看门狗复位，该段在重启后回到 flash 旧值（硬复位同理，但语义上本就没有「保留」期待）。 |
| **影响面** | 仅「section 捕获后 ≤5s 内复位」的窄窗口；已落盘的段、歌曲顺序与工程槽不受影响（PER3 与 FUN6 各自 A/B 提交）。用户可见形态是「刚存的段重启后不见了」。 |
| **为何可延迟** | 正常使用路径（STOP 后等待 ≥5s 或继续编辑）会由 1s 任务落盘；`sequence` 场景实测 2.1s 内排空 dirty。与上游的差异只在异常复位，且完全对齐需要跨复位的 RAM 槽保活（PSRAM 分配/`.noinit` 指针所有权），属存储启动策略变更，不宜在收口任务里动。 |
| **解决方向** | 二选一：(a) 在可控重启路径（`esp_restart` 前/关机信号）同步 flush 所有 dirty 段与歌曲；(b) `sloop_store_init` 在检测到 warm boot 且 `.noinit` 槽指针有效时保留 RAM 槽并与 flash 比较，差异则保留 RAM 值 + 置 dirty（需先证明 PSRAM 内容在软复位后仍有效并处理分配器重入）。优先 (a)，并在 `sequence` 场景加「捕获后立即复位 → 段内容保持」的判别用例。 |
| **触发条件** | 用户报告「刚捕获的段在重启后丢失」；或 P4-E/P4-F 要求与上游 warm reset 语义一致；或引入关机/重启 UI 时。 |
| **参考** | `firmware/components/sloop_core/src/store_tab5.c`（`sloop_store_init` 的槽 fetch、`sloop_store_sections_tick`）、`.superpowers/sdd/2026-11-22-p4c-sequencer-arranger/task-8-report.md` §8.5、ADR-009 §6.1 |
| **登记** | 2026-11-23（P4-C Task 13 收口审计：Task 8 报告 §8.5 遗留） |

## DI-34: 注入场景脚本仍按 12 页 / 6 组 / EDIT Σ 19,968 断言（P4-D Task 8 后的导航口径）

| 项 | 内容 |
|---|---|
| **背景** | P4-D Task 8 把 `APP_STATE_PAGES` 扩到 14、组表改为 7 组（SAMPLE 从 EDIT 移入 SYNTH），组签宽 168→142px、SAMPLE est 3,072→6,144。固件与 host 门禁已对齐，但 `scripts/inject/nav.sh`（⑦ 全 12 页轮转、组签/子签列表）、`scripts/inject/params.sh`（EDIT 组 Σest 19,968、SAMPLE 页在 EDIT 子行的导航与 `UI SUB` 下标）与 `scripts/soak.py`（页访问表「全部 12 页 / 六个组签」）仍是旧口径；`AGENTS.md` §5.5 已就地注明「场景脚本在 Task 11 对齐」。 |
| **影响面** | 仅设备注入门禁：Task 11 之前对**本固件**跑 `nav`/`params`/`soak` 会出现假红（页序/组 Σ/`UI SUB` 下标不符），固件本身无缺陷；`store`/`song`/`sequence`/`transport`/`step_edit`/`tracks` 不受页数影响（未断言 12 页/6 组）。host 门禁（build/state/synth/seq/knob）不受影响。 |
| **为何可延迟** | Task 8 的任务口径是「host 构建 + 组表/占位页落位」，设备注入矩阵按计划在 Task 11 统一跑并更新；同一提交里改脚本会让本任务无法用现有脚本自证（脚本与固件同时改，失败无法归因）。 |
| **解决方向** | Task 11 同批更新：`nav.sh` ⑦ 改 14 页轮转（…SAMPLE→DSYN→FM6P→STEP…）与 `UI TAB synth`/`UI SUB 0..2`；`params.sh` 的 EDIT Σ 改 16,896、SAMPLE 页改经 SYNTH 组进入并新增 SYNTH 组 Σ 20,480 的 `UI MEM` 断言；`soak.py` 页表加 DSYN/FM6P 与 SYNTH 组。完成后按 §5.5 跑九条场景并删除本条目。 |
| **触发条件** | Task 11 首次设备注入矩阵之前（若 Task 11 延期，任何对 P4-D 固件重跑设备导航门禁之前）。 |
| **参考** | `firmware/components/sloop_ui/src/ui.c`（`k_groups[]`）、`docs/design/005-ui-architecture.md` §5.1、`.superpowers/sdd/2026-11-23-p4d-sampling-dsyn-fm6/task-8-report.md` §4 |
| **登记** | 2026-11-23（Task 8 评审：脚本与固件口径分叉） |

## DI-35: DSYN 的 kit 级 CRUSH 没有写入口（KIT 页只读）

| 项 | 内容 |
|---|---|
| **背景** | spec §4.1 把 `CRUSH`（0..255，低 nibble 丢位数、高 nibble S&H−1）归 DSYN 的 KIT 页，并要求「写后同样过 `dsu_fix_kit`」；但 P4-D Task 2 的 core 只实现了 24 个**声音字段**的 `sloop_core_dsyn_set`（`dsnd_t` 22B），Task 6 的状态层也只有 `CMD_DSYN_SET/LOAD_KIT/STORE/AUDITION` 四条命令——没有任何 kit 级 crush 的读写路径（`sloop_core_dsyn_read` 只读出到 `crush` 指针）。Task 9 的页面因此把 CRUSH 作为只读显示（旋钮值 + 拖拽/± 回 `CRUSH READ ONLY`、不下发命令），KIT SRC 仍可套用工厂 kit（`CMD_DSYN_LOAD_KIT`）。 |
| **影响面** | 只有 KIT 页的 CRUSH 编辑：用户无法在 Tab5 上改 bit-crush / S&H 深度；`DSYN GET/INFO` 仍能读、上游编辑器/备份可写。DSYN 的 24 个声音字段、试听、STORE、DSU 持久化都不受影响。 |
| **为何可延迟** | 补写入口要扩三处接口：core（`sloop_core_dsyn_{set,get}_crush`，引擎锁 + `dsu_fix_kit` + `dsu_dirty`）、状态层（新命令 + `synth_status` 映射 + state-test/synth-test 用例）、console（`DSYN CRUSH` 诊断面）与页面（可编辑旋钮）；属于接口扩张而不是 UI 任务，且 P4-D 的任务边界（Task 9 brief 的 Consumes 清单）只列了既有四条 `CMD_DSYN_*`。 |
| **解决方向** | (a) core 增加 `int16_t sloop_core_dsyn_field_crush(uint32_t kit, int16_t v)`（钳 0..255、`dsu_fix_kit`、`dsu_dirty`）；(b) 状态层加 `CMD_DSYN_CRUSH{a=kit,v=value}`（复用 `CMD_DSYN_SET` 的 a/b/n/v 编码更省：`n=0xFF` 表示 kit 级字段，但需先冻结协议，勿混进 field 编号）；(c) KIT 页把 CRUSH 槽标为可编辑（写后回读 + dirty），删掉 `CRUSH READ ONLY` 令牌；(d) synth-test 覆盖写后 name/crush/src 与 16×22B 的逐字节变化（crush 不影响声音字节）与越界钳位。 |
| **触发条件** | 用户报告「调不了 crush」；或 P4-E 要补齐上游编辑器 DSYN_PUT part 16（name + crush + src）的全部字段；或 FM-1 联动要双向同步 crush。 |
| **参考** | `firmware/components/sloop_core/src/sound_tab5.c`（`dsyn_get/dsyn_put`、`sloop_core_dsyn_read`）、`firmware/components/sloop_ui/src/page_dsyn.c`（KIT 页、`UI_DSYN_ACT_READONLY`）、`third_party/sloop-fm1/firmware/src/editor_dsyn.c`（DSYN_PUT part 16/17）、`docs/superpowers/specs/2026-11-23-p4d-sampling-dsyn-fm6-design.md` §4.1、ADR-008 §8.7 |
| **登记** | 2026-11-23（P4-D Task 9：KIT 页只读 CRUSH） |
