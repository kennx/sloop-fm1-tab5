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
| **为何可延迟** | 自检是**诊断命令**、不在用户操作路径上；P4-B 的注入门禁都在脚本内显式 `STORE AUTO OFF`（并已改为「每次硬复位后重新下发」），`device-selftest.py` 也已按 (b) 的变体在自检前关 AUTO 且不恢复。P4-E Task 7 的 SYSTEM chip 已走独立 worker（自检后 autosave resume + AUTO ON），产品入口已恢复工作工程；本条目仅剩 **console `ENG SELFTEST`** 的既有诊断语义（按控制器裁决保持不变）。修法仍需区分诊断命令与存储语义，不宜顺带改变脚本门禁口径。 |
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
| **参考** | `scripts/device-selftest.py`（`SUMMARY`/`summary_fields`/`compare`）、`firmware/components/sloop_core/src/selftest.c`（汇总 printf）、`firmware/components/sloop_ui/src/ui.c`（quiesce OFF 日志）、`docs/dev/verify/4-pf-2026-11-24/task-2-root-cause-callstacks.txt`（原 DI-26 自检堆泄漏的根因；当时的泄漏让自检后的内部堆只剩 ~28KB） |
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

## DI-34: 采样只支持串口分块上传，无 WAV/SD 卡/麦克风导入 UI

| 项 | 内容 |
|---|---|
| **背景** | P4-D 把上游 4×80KiB 原始采样槽接进设备（flash 权威 + PSRAM 镜像 + `SMP BEGIN/DATA/HDR/END/ERASE`），但上传通道只有 console 行（`scripts/sample_upload.py` 在 PC 侧用 vendored `sampleio.user_slot` 把 WAV 编成 IMA ADPCM）。屏上没有文件浏览器、没有 WAV 解析、没有录音采集。 |
| **影响面** | 用户无法脱离 PC 把采样放进设备；SAMPLE/GRAIN/鼓 KIT 的 USR1–4 只能由脚本填充。设备侧擦除（SAMPLE 页 `ERASE` chip）与选择/播放已闭环。 |
| **为何可延迟** | P4-D spec §1 明确列为非目标；WAV 解析/ADPCM 编码在 PC 脚本侧已有逐字节同一的编码器，设备侧缺的是文件 I/O + UI，属独立功能面（FATFS 已在分区表里，SD 为独立硬件）。 |
| **解决方向** | (a) SD 卡：FATFS 挂载 + 简单文件列表页（复用 FILE 组模式），选中后流式读入 `sloop_store_sample_*`（走带中拒绝的纪律不变）；(b) 麦克风/线路录音：需要 I2S 输入路径与 ADPCM 编码器进固件，工作量更大；(c) 文件解析放设备侧时须保留 header/CRC 校验（复用 `smp_zone_ok` + `st_crc32`）。优先 (a)。 |
| **触发条件** | 用户要求「不接电脑也能换采样」；或 P4-F 全页面验收把采样导入列进闭环。 |
| **参考** | `firmware/components/sloop_core/src/sound_tab5.c`（采样槽实现）、`scripts/sample_upload.py`、`third_party/sloop-fm1/tools/sampleio.py`、ADR-011 §4、P4-D spec §1 非目标 |
| **登记** | 2026-11-23（P4-D Task 12 收口审计） |

## DI-36: FM6 live patch 掉电不保留（会话状态，必须显式存 bank）

| 项 | 内容 |
|---|---|
| **背景** | FM6P 页直接编辑 `fm6_patch[track]` 的 155B live patch（写后 `fm6_slot[t]=p[P_E7]` 防 `fm6_poll` 覆盖），持久化只走既有 FM6 bank 27 槽（`STORE BANK PUT` / 页面 `SAVE` chip）。live patch 本身不写 flash——与上游一致（patch 是编辑器的会话状态，除非存入 bank）。因此未 SAVE 的深度编辑在掉电/复位后回到 bank 槽（或工厂）的 patch。 |
| **影响面** | 仅 FM6 深度音色的掉电保留语义；`synth` 场景已验证「BANK PUT → GET == LIVE → 硬复位后逐字节保持」「ALG 宏清除」「非 FM6 轨拒绝」。不影响工程/歌曲/DSYN。 |
| **为何可延迟** | 上游同语义，且 bank 是 27 槽的显式音色库；把 live patch 自动写 bank 会与槽选择（PTCH）语义纠缠，属产品决策。 |
| **解决方向** | (a) 保持现状，在 FM6P 页把 `SAVE` 与当前槽/`DIRTY` 提示做得更醒目；(b) 若产品要求「掉电保留最后一次编辑」，需要新增独立对象或把 live patch 纳入 settings（都会偏离上游布局，须先写 ADR）；(c) 或在切轨/关机路径自动 `PUT` 到当前 PTCH 槽（仅当 PTCH 是用户槽 8+k）。优先 (a)。 |
| **触发条件** | 用户报告「FM6 调好的音色重启后没了」；或 P4-E 统一持久化语义；或联动模式需要与 FM-1 的 patch 会话状态对齐时。 |
| **参考** | `firmware/components/sloop_core/src/sound_tab5.c`（FM6 段）、`third_party/sloop-fm1/firmware/src/eng_fm6.c`（`fm6_set_patch`/`fm6_poll`）、`scripts/inject/inject.py` 的 `run_synth` ③、ADR-011 §6、P4-D spec §3.6 |
| **登记** | 2026-11-23（P4-D Task 12 收口审计） |

## DI-37: 采样名 / DSYN kit 名 / FM6 patch 名无屏上文本输入

| 项 | 内容 |
|---|---|
| **背景** | 采样槽名（8 字符，`smp_user_hdr_t.name`）、DSYN kit 名（8 字符）、FM6 patch 名（10 字符，`FP_NAME` 145..154）都只读显示/随文件或工厂值保留；P4-D 没有提供字符键盘/文本编辑 UI。console 也没有改名命令。 |
| **影响面** | 仅命名体验：用户无法在设备上给自定义采样/鼓 kit/FM6 音色改名，只能沿用上传文件里的名字或工厂名。播放、持久化、bank 存取不受影响（名字随对象/槽字节保存）。 |
| **为何可延迟** | P4-D spec §1/§7 明确列为非目标（屏幕软键盘 + 文本输入焦点模型是独立 UI 面）；现有名字在画面上可读，足以区分槽位。 |
| **解决方向** | (a) 复用 Tab5 Keyboard 的字符键做一个小型文本输入模态（8/10 字符 + 退格/确认），页面上把名字区域变成可点击入口；(b) 写路径分别落到 `smp_user_hdr_t.name`（须重算 header CRC 并走 `sloop_store_sample_*` 的提交路径）、`dsu.k[k].name`（置 `dsu_dirty`）、`fm6_patch[FP_NAME]`（`fm6_set_patch` 不 sanitize 名字，需保留哨兵语义）；(c) console 加 `SMP NAME`/`DSYN NAME`/`FM6P NAME` 诊断面并用 host/设备用例覆盖。优先 (a)+(c)。 |
| **触发条件** | 用户要求给采样/音色命名；或 P4-F 全页面验收把「命名」列进功能对齐清单。 |
| **参考** | `firmware/components/sloop_core/include/sloop_core.h`（`SLOOP_DSYN_NAME`/`SLOOP_FM6_PATCH_SIZE`）、`sound_tab5.c` 的 FM6 名字只读哨兵与采样 header、`third_party/sloop-fm1/firmware/src/eng_sample.c`、ADR-011 §2/§6、P4-D spec §1 非目标 |
| **登记** | 2026-11-23（P4-D Task 12 收口审计） |

## DI-38: DSYN-only 修改不触发 1s 延迟写（需显式 STORE 或用歌曲保存顺带落盘）

| 项 | 内容 |
|---|---|
| **背景** | P4-D 的 DSU 持久化跟随 settings 对象：任何 settings 保存（显式 `STORE SONG SAVE`、sections/歌曲 1s 延迟写、`DSYN STORE`）都会把当前 RAM `dsu` 写进 PER3 88..1,551；但**只改 DSYN 不会自己触发** 1s 延迟写（上游也不；`dsu_dirty` 只影响下一次 settings 写的 payload，不参与 `autosave`——autosave 是 `project_t`，不含 DSU）。用户在 DSYN 页调完音色后若直接掉电，未 `STORE` 的修改丢失。 |
| **影响面** | 仅 DSYN 音色的掉电保留语义；显式 `STORE`（页面/chip/console）路径完整，`synth` 场景已覆盖 dirty=0 与硬复位/自检后逐字节一致。不会丢歌曲/工程（各自 A/B 对象）。 |
| **为何可延迟** | 上游同语义（DSYN 保存是显式动作）；把 DSYN 纳入 1s 延迟写需要新门限（`dsu_dirty` 与演奏/停止状态的关系）并会与「任何 settings 保存都写当前 dsu」的规则叠加，属产品语义决策，不宜在收口任务里改。 |
| **解决方向** | (a) 保持显式 STORE，但在 DSYN 页把 `DIRTY` 做成醒目提示（当前状态行已有 ` DIRTY` 后缀，可强化）；(b) 或让 `sloop_store_sections_tick` 在 `dsu_dirty` 且停止/无发声/idle 时也走一次 settings 写（与歌曲共用 gap 时间戳），代价是 DSYN 编辑会周期性擦写 flash；(c) 或退出 DSYN 页时提示/自动 STORE。优先 (a) 并评估 (b)。 |
| **触发条件** | 用户报告「调好的鼓音色重启后没了」；或 P4-E 统一「会话/持久化」语义时。 |
| **参考** | `firmware/components/sloop_core/src/song_persist.h`（`song_per_put_dsu`/`song_per_saved`）、`store_tab5.c` 的 `sloop_store_dsu_store`/`sloop_store_sections_tick`、ADR-011 §3、P4-D spec §3.4 |
| **登记** | 2026-11-23（P4-D Task 12 收口审计） |

## DI-39: 输出路由只能 speaker-only（扬声器功放与耳机左声道共用 ES8388 LOUT1）

| 项 | 内容 |
|---|---|
| **背景** | P4-E Task 3（spec §5 D10）实现输出路由时按官方原理图（`Tab5_Schematics_PDF.pdf` 第 3 页，U14/C67/C68/U15）核对通路：ES8388 **LOUT1**(pin 12) 经 C68 22µF → 网络 `HPOUT_L`，**同时**接耳机左声道与 NS4150B 功放输入（100nF → R43 68K → INP；INN 经 R44 68K 接 AGND）；**ROUT1**(pin 11) 经 C67 → `HPOUT_R` 只到耳机右声道；**LOUT2/ROUT2(pin 15/14) 未连接**（真机 `AUD REG` 读回 `0x2E..0x31 = 0x21`）。扬声器与耳机左声道共用 LOUT1，不存在只靠 ES8388 音量寄存器把两者分开的组合——把 LOUT1/ROUT1 写 0（-33.5dB）是两者一起近静音——这是阶段 1 默认态人测无声的**最可能原因**（按原理图推断，未在设备上二分）。本条同时承接已关闭的 DI-6 的未决余项。 |
| **影响面** | 输出路由设置降级为 speaker-only：`sloop_hal_settings_set_output` 只接受 speaker，`AUD OUT headphone` 回 `AUD OUT ERR not_supported` 且状态不变，SYSTEM 页的 OUTPUT 行给不出可用的 HEADPHONE。另：扬声器**只重放左声道**（功放取 LOUT1），右声道/声像在扬声器上丢失。 |
| **为何可延迟** | 默认态（四路输出 0x21）扬声器与耳机都可用，是阶段 1 人测出声的实证态；缺的只是「切换」，不影响可听性。真正的路由要动功放使能（新的硬件副作用），不宜在设置页任务里顺手做。 |
| **解决方向** | (a) 用 **SPK_EN**（PI4IOE5V6408-1 @0x43 的 E1.P1，`bsp_feature_enable(BSP_FEATURE_SPEAKER, …)`）做路由：headphone = 关功放（耳机仍由 LOUT1/ROUT1 驱动）、speaker = 开功放；实现前先核实 BSP 该开关的确切副作用（是否连带其它电源域/静音时序）。(b) 扬声器单声道：在 HAL 输出级做 L/R 混音，或由引擎提供 mono 合成（产品裁决）。(c) DI-6 余项的人测：输出电平 `0x21` 与 `0x1E` 的听感差、底噪与合理默认音量；耳机通路单独听音；44.1kHz 音高复核（I2S 日志已证 44100，缺人耳）。 |
| **触发条件** | SYSTEM 页需要可用的 HEADPHONE 选项时；或人测报告扬声器底噪偏大 / 单声道不可接受时。(c) 在硬件旁做听音时顺手完成，完成后从本条删除。 |
| **参考** | `firmware/components/sloop_hal_tab5/src/sys_settings.c` 文件头、`docs/tab5-hardware.md` §3.2（含原理图链接）、`docs/dev/device-notes.md` §5、`.superpowers/sdd/2026-11-23-p4e-settings-page/task-3-report.md` §2.1、spec `2026-11-23-p4e-settings-page-design.md` §2.4/§5 D10（「扬声器 = LOUT2/ROUT2」的表述已被原理图否定）。已封存的证据包（如 `docs/dev/verify/4-pc-2026-11-22/`、`1-bringup-2026-11-19/`）里的 `DI-6` 是历史编号（R18：证据包不可变），即指本条 |
| **登记** | 2026-11-23（P4-E Task 3 评审修复；关闭 DI-6 并承接其余项） |

## DI-43: 内部堆余量仅 4KB + 平台设置的两处未验证项（worker 栈收缩已收口）

| 项 | 内容 |
|---|---|
| **背景** | P4-E 修复轮固件（`86711a4`，终审时的 HEAD 固件同构）的 1800s soak `internal_min` = **65,536B**，§5.5 门限 61,440B（60KB）⇒ 余量恰 **4,096B**；P4-D 同口径为 83,780B。P4-D→P4-E 少的 ~18KB 里，**SELFTEST chip 的 worker 占 16KB**（**此项已于 2026-11-24 收口**：栈 16,384→5,632，见「登记」）：`sys_selftest.c` 的 `SYS_SELFTEST_STACK 16384`，`main.c` 在 UI 启动后 `xTaskCreate` 一次、之后 forever `vTaskDelay` 等 `CMD_SELFTEST_RUN`（栈与 TCB 常驻内部堆；`.noinit` 标记让它每 boot 只真正跑一次）。**刻意**在开机创建（任何泄漏之前）：这样栈落在内部 L2MEM；放到请求时再创建，DI-26 泄漏之后可能落到 LP SRAM（DI-40 的同款坑）。其余差额按终审笔记归 NVS（平台设置三键；推断，未逐项量测）。**同条登记终审 triage 的两项平台设置遗留**：(a) 播放中写 xrun 的用例只测亮度（LEDC），**VOLUME**（codec 互斥 + I2C）与 **NVS commit** 在流式期间的 xrun 影响没有机读覆盖——1s 任务的静默窗口会让 NVS 提交避开发声，但窗口判定（`audio_quiet`）与提交之间没有 codec 静音；(b) 键盘 RGB 颜色缓冲 `0x60..` 的字节序 **B,G,R** 取自 M5Tab5-Keyboard-Internal-FW 的寄存器语义，console `KBD RGB` 只发白色 `0xFFFFFF`，字节序错了机器看不出来（accent 为黄/品红/青时才会显色差）。 |
| **影响面** | 余量：任何新增的常驻内部 RAM（P4-F 的 USB host / USB-MIDI / 联动协议缓冲几乎必然要内部 DMA 内存）都可能把 soak 的 heap floor 顶穿；soak 门限本身不改。平台设置：(a) 只在「播放中调音量」这个用户路径上可能有可听的爆音/xrun，未观测到；(b) 只影响键盘灯颜色（亮度档位与开关已机读覆盖）。 |
| **为何可延迟** | 当前固件 soak PASS（xrun 0、min 65,536B ≥ 门限），没有功能性缺陷；worker 栈当时没有高水位读数（P4-E 修复波占用了设备门禁窗口）；2026-11-24 已实测峰值 4,148B、栈收到 5,632（见「解决方向」(1)）。平台设置两项是人测可兜底的可见性问题。 |
| **解决方向** | (1) **已收口**（2026-11-24：worker 收尾打印 `sys_selftest: stack hwm=<bytes>`，实测 16KB 栈的 hwm=12,236 ⇒ 峰值 4,148B，栈取 4,148+1,024 向上对齐 512 = 5,632；收尾 hwm=1,484）。(2) **P4-F 动工前重估 soak floor**：按 USB host/MIDI 的实际常驻量重新定 60KB 门限或明确记为「联动模式下不适用」。(3) xrun：给 `sys` ③ 的播放中 20 次写加 VOLUME 变化（0↔70）+ `ENG PERF` 3s 窗口 xrun 增量 = 0 的断言，并在播放中制造一次 NVS dirty 后的提交（或直接在 `sloop_hal_settings_flush` 的 commit 前后取 xrun 计数打印）；若测到 xrun，commit 期间做 codec 静音或把提交挪到停流窗口。(4) 键盘字节序：console 加 `KBD RGB RGB <r> <g> <b>`（或 `KBD RGB PROBE`）发纯红/纯绿，配合屏上色块截图做人眼对照；或读回键盘 FW 的颜色缓冲寄存器（若协议支持回读）。 |
| **触发条件** | (2) P4-F 第一个涉及 USB host/MIDI 的任务开工前（新增内部 RAM 分配之前）；soak 的 `internal_min` 低于 ~62KB 时立即处理。(3)(4) 用户报告播放中调音量有爆音，或键盘灯颜色与屏上 accent 不一致。 |
| **参考** | `firmware/main/sys_selftest.c`（`SYS_SELFTEST_STACK`、worker 循环）、`firmware/main/main.c`（`sys_selftest_start` 调用点）、`scripts/soak.py`（`HEAP_FLOOR`）、`docs/dev/verify/4-pe-2026-11-23/fix-soak-summary.txt`（min 65,536B）、ADR-007（音频任务栈的 L2MEM 分配）、`firmware/components/sloop_hal_tab5/src/sys_settings.c`（`sloop_hal_settings_flush`）、`firmware/components/tab5_keyboard`（`tab5_keyboard_rgb_set`）、P4-E spec §5 D2/D9 |
| **登记** | 2026-11-24（P4-E 整分支终审 I3；合并 triage 的 M10 余项与键盘字节序）；2026-11-24 P4-F Task 2：(1) 收口——开机 `internal_free` 79,799B（栈 5,632 固件的手工 chip 运行前读数；同一固件 `selftest-heap.py` 的 boot 读数为 79,863B，差为两次开机之间的噪声）对 69,111B（16KB 栈固件、`selftest-heap.py` 的 boot 读数）⇒ 约 +10.7KB，栈仍在 L2MEM（`sys_selftest: stack=0x4ff30a84`）；注意 worker 跑完 `vTaskDelete(NULL)` 自删，**chip 首次运行之后这 5,632B 会归还堆**（运行后实测 85,511B），16KB「常驻」只发生在 chip 的第一次运行之前。soak floor 的新读数由阶段收口 soak 重测 |

## DI-44: PRESETS 收藏（fav 16）无 UI

| 项 | 内容 |
|---|---|
| **背景** | 上游 2.7：PRESETS 页 KNOB 4 把当前声音加入/移除收藏（最多 16，置顶列表），随 settings 的 side record（FAVS 段）持久化。Tab5 PRESETS 页无此交互；side record 的 fav 字节逐字节保留。 |
| **影响面** | PRESETS 页缺少「收藏置顶」浏览方式；数据无损。 |
| **为何可延迟** | 非核心路径；P4-E 已保证 fav 字节不丢，随时可补。 |
| **解决方向** | PRESETS 页加收藏切换（chip 或键盘键）+ `CMD_PRESET_FAV`；读 `fav[]`（同 TU 可达）并镜像上游 `fav_toggle` 语义。 |
| **触发条件** | 用户在 32 用户预置 + 工厂预置间来回找同一个声音；或 P4-F 之后的 PRESETS 页改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-45: EDIT 层实时擦除（走带中 MPC 式）未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 EDIT 层：按住 EDIT + 键 = 该音符/鼓声在走带中从播放头经过的每一步被擦除；另有 MUTE mode。Tab5 只有 STEP 格点切换与 `SEQ EDIT DEL`。 |
| **影响面** | 实时演奏中无法「按着键擦掉某个音」；离线编辑不受影响。 |
| **为何可延迟** | 需要新的输入层（修饰键 + 音符键 → 按拍调用擦除），涉及 InputManager 层模型与录音并发（DI-28 同类风险）。 |
| **解决方向** | InputManager 增 EDIT 层：按住某键期间每个 30ms 拍/每步对选中轨调用 `SEQ_STEP_NOTE_DEL`（鼓：按 lane 清位）；人测手感。 |
| **触发条件** | 用户提出现场擦除需求；或 P5 输入层改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-46: 鼓轨 MUTE mode（Sequencer 内静音某鼓声）未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 2.6：EDIT 层 + 黑键进入 MUTE mode，白键静音/恢复 Sequencer 里对应的鼓声（live，不擦除）。Tab5 无。 |
| **影响面** | 现场无法快速静音某个鼓 lane；可用 STEP 页逐格清代替（破坏性）。 |
| **为何可延迟** | 同 DI-45 的输入层工作；非核心。 |
| **解决方向** | 与 DI-45 一并实现：lane 级 live mute，复用上游 `drum_mute` 位掩码（`ui_layers.c:787`，同 TU 可达）。 |
| **触发条件** | 用户提出需求；或与 DI-45 同批。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-47: ARP roll（note repeat 层）未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 ARP 层：按住 ARP + 键 = 以 RATE（1/8…1/64）重复该音，锁定 tempo/swing；录音时写成 ratchet。Tab5 无。 |
| **影响面** | 演奏表现力缺一项；ARP 参数页本阶段补齐（Task 4），但 roll 是输入层功能。 |
| **为何可延迟** | 输入层 + 录音写入路径，和 DI-45/46 同类。 |
| **解决方向** | InputManager 增 ARP 层：按住期间按 RATE 调度重复 note-on；录音中经 `sloop_seq_drum_hit`/note 写入。 |
| **触发条件** | 用户提出需求。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-48: tap tempo 未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 GLO 层白键 16：连敲两次以上设 BPM。Tab5 只有 BPM±。 |
| **影响面** | 排练时无法快速跟拍；BPM 旋钮可替代。 |
| **为何可延迟** | 小功能但需输入层 + 时间窗逻辑。 |
| **解决方向** | TRANSPORT 增 TAP chip / 键盘键：两次间隔 → BPM（40..240 钳位），经 `CMD_SET_BPM`。 |
| **触发条件** | 用户提出需求；或 TRANSPORT 页下次改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-49: SEL 层键盘行为（CHORD / KEYS SNAP·WHITE）未映射到 Tab5 键盘

| 项 | 内容 |
|---|---|
| **背景** | 上游 SEL 层：CHORD 模式下单指走和弦、黑键换和弦；KEYS = OFF/SNAP/WHITE 改变琴键行为。Tab5 键盘直接发原始音高；参数本身本阶段补齐（Task 4 SCALE 页）。 |
| **影响面** | 参数能改，但 Tab5 键盘上听不到 CHORD/KEYS 的效果（若引擎在 note-on 前由 ui_input 做映射）。 |
| **为何可延迟** | 需先核实上游是在 `ui_input.c` 做键位映射还是引擎内处理；属输入层。 |
| **解决方向** | 读 `ui_input.c` 的 keys/chord 处理，移植到 InputManager 的 note-on 路径（纯函数，host 单测）。 |
| **触发条件** | Task 4 落地 SCALE 页后若参数在 Tab5 键盘上无效，则升级为必修。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-50: 开机画面 / 开机音未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游有 splash 与开机音（`splash.c`、boot sound）。Tab5 开机直接进 PLAY 页。 |
| **影响面** | 观感差异；无功能影响。 |
| **为何可延迟** | 纯呈现；固件开机即启流，开机音需小心与流式启动顺序（不阻塞功能）。 |
| **解决方向** | display 就绪后 1–2 帧标题画面；开机音经 `sloop_core` 触发一次 note（停流或混入流）。 |
| **触发条件** | 产品化验收前。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-51: VIS 可视化（12 风格）未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 VIS 屏读取 `vis_tap`（fx.c）画示波器等 12 种风格；Tab5 无。 |
| **影响面** | 少一个页面；`vis_style` 位保留。 |
| **为何可延迟** | PLAY 页是只读呈现，可视化需持续重绘（LVGL 池/帧预算）。 |
| **解决方向** | PLAY 页叠加 canvas 读 `vis_tap`（需 HAL 导出），先做 1 种风格；LVGL 池预算另评。 |
| **触发条件** | 用户提出需求；或 PLAY 页改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-52: Punch-in effects 层未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 FX 层：按住 FX + 键触发 punch-in 效果（滤波/dust/duck/轨滤波等 27 键），KNOB 调 master；`punch_process` 已在引擎 `mix_block` 内运行，Tab5 无 UI/输入层。 |
| **影响面** | 现场效果缺一类；引擎能力已在。 |
| **为何可延迟** | 输入层 + 效果映射；与 DI-45/47 同类。 |
| **解决方向** | InputManager 增 FX 层：按住键 → `punch` 触发（经同 TU 适配 `sloop_core_punch_*`）。 |
| **触发条件** | 用户提出需求；或 P5 输入层。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5 |
| **登记** | 2026-11-24（P4-F Task 1 功能对齐审计） |

## DI-53: 八度移位（OCT−/+）未移植

| 项 | 内容 |
|---|---|
| **背景** | 上游 OCT−/+ 把合成轨的琴键上下移八度（同按回 0）；Tab5 键盘（ADR-006）把字母/数字行固定映射到固定音高，`firmware/` 内没有任何八度逻辑（审计复查发现）。 |
| **影响面** | 70 键音域约两个八度，无法演奏更高/更低的音；鼓轨不受影响。 |
| **为何可延迟** | 输入层功能，不涉及引擎；键位需在 ADR-006 的键表里找空闲键并避开音符/层键。 |
| **解决方向** | InputManager 增八度键（如层内数字键或 PgUp/PgDn），在 `input_map` 的 note-on/off 路径统一加 `oct*12`（释放必须回按下时的八度，参照 `input_map_key_route`）；host 单测 + `KEY` 注入断言 `ENG SNAP` 的 note。 |
| **触发条件** | 用户提出演奏音域需求；或 P4-F 之后的键盘映射改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5、ADR-006 |
| **登记** | 2026-11-24（P4-F Task 1 评审复查） |

## DI-54: 工厂预置无按名浏览列表

| 项 | 内容 |
|---|---|
| **背景** | 上游 PRESETS 页按名浏览全部工厂预置（KNOB 1 选、KNOB 2 换引擎）。P4-F Task 5 只在 SOUND 页眉提供 `ENGINE −/+` 与 `PRESET −/+` 步进选择（审计发现此前连步进都没有、只有 console `ENG TRACK`）。 |
| **影响面** | 几百个工厂预置里找一个要靠步进；数据与换音功能本身已可达。 |
| **为何可延迟** | 列表浏览需要名字列表投影 API 与一页可滚动 UI（池预算另评），不是「能不能换音」的问题。 |
| **解决方向** | `sloop_core_preset_name(engine, k)` 投影上游名字表；PRESETS 页增「工厂」视图（按引擎分组的可滚动列表，est 另评）。 |
| **触发条件** | 用户反馈步进选择太慢；或 PRESETS 页下次改版。 |
| **参考** | `docs/design/012-feature-parity-checklist.md` §5、P4-F spec §4 SOUND 行 |
| **登记** | 2026-11-24（P4-F Task 1 评审复查） |

## DI-55: 诊断期静默（`UI QUIESCE on/off`）每次转换泄漏约 40B 内部堆

| 项 | 内容 |
|---|---|
| **背景** | 定位 DI-26（已修）时的副产物：`UI QUIESCE on` / `UI QUIESCE off` 各使 `ENG MEM internal_free` 减少约 36–44B（连续 4 轮 on/off：−40/−44/−36/−40/−44/−36/0/−8——多数转换一次掉一次，但并非严格每次、量级也有波动，是否恒久泄漏未做长时间观测），`ENG AUDIO ON` 同量级；与自检无关（不跑任何 job 也出现）。`ENG SELFTEST` 命令因此每次仍带 ~36B 的残余（修复后两次全量的第二次差值 = 36B，`scripts/selftest-heap.py` 门禁 ≤1,024B）。临时 `CONFIG_HEAP_TRACING_STANDALONE`（带调用栈）的记录里**没有**对应的分配——不是经 `malloc/calloc/pvPortMalloc/heap_caps_*` 的路径，来源未定位（可能是 FreeRTOS/驱动内部不经堆封装的簿记，或 `heap_caps_get_free_size` 口径下的块合并/拆分）。 |
| **影响面** | 只在诊断期静默的进入/退出与启流路径：`ENG SELFTEST`（含 SYSTEM 页 chip）每次约 36B（修复 DI-26 后 `selftest-heap.py` 第二次全量前后差 = 36B）。正常播放/编辑/切页不触发。 |
| **为何可延迟** | 量级 ~40B/次：soak 口径下每轮自检一次可忽略；DI-26 修复后同一 boot 连续全量自检不再威胁内部堆。 |
| **解决方向** | 用 `heap_caps_get_info`（largest_free_block/free_blocks）与 `multi_heap` 块遍历对照前后差异，或对 `heap_caps_malloc_base` 之前的入口（`heap_caps_aligned_alloc`、`xTaskCreate` 的 TCB 等）逐项排除；定位后按根因修（不要在 quiesce 收尾做「堆整理」掩盖）。 |
| **触发条件** | soak 的 `internal_min` 逼近门限（~62KB）；或需要在同一 boot 内高频进入/退出诊断期静默的排障会话。 |
| **参考** | Task 2 进度记录 `.superpowers/sdd/2026-11-24-p4f-phase4-closeout/task-2-progress.md`（16:22–16:23 两次测量）、`firmware/components/sloop_ui/src/ui.c`（`sloop_ui_quiesce`）、`firmware/components/sloop_ui/src/input.c`（`input_quiesce_begin/end`）、`scripts/selftest-heap.py` |
| **登记** | 2026-11-24（P4-F Task 2 定位 DI-26 时的副产物） |
