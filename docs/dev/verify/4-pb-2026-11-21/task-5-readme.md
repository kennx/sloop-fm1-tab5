# P4-B Task 5 证据索引（工厂预置应用与换音语义）

日期：2026-11-21 ｜ 受测固件：`f2ea4e8-dirty`（工作区＝本任务源码；任务提交前的现场，与既有
P4-B 任务同一口径） ｜ 设备：Tab5 `/dev/ttyACM0`
详细报告：`.superpowers/sdd/2026-11-21-p4b-storage-presets/task-5-report.md`

改动本体：上游 `ui.c:581-691` 的预置机制（`apply_preset_to/set_engine_of/TRK_DEF/param_kept/
arp_param/arp_is_sounds`）原语义移植进 `store_tab5.c`，纯写值抽成 `preset_values`（应用与预览
共用）；`sloop_core_track` 改为「引擎变 → 默认值 + 预置 0，预置变 → 整份声音写 p[]」；新增
`sloop_core_apply_preset/set_engine/preset_preview`、console `ENG PRESET`（71 项期望向量）与
`ENG SNAP … voices=`；`project_load` 补上游 `preset==0xFF` 默认声音 + `proj_foreign_parts`。

## RED（本任务前的镜像 = Task 4 提交）

| 文件 | 内容 |
|---|---|
| `task-5-store-red-run.txt` | 新断言在旧固件上失败：`[FAIL] store: ENG PRESET 0 2 1: 回显 0/71 项`（旧固件无该命令，回用法行），exit=1 |
| `task-5-store-red.log` | 上面的串口原始回显 |
| `task-5-red-transcript.txt` | **缺陷本体**的手工 transcript（旧固件）：`ENG TRACK 0 2 1` 后 `ENG SNAP` 的 `engine=2 preset=1` 变了，而 `PARAM 1/63/64` 仍为 `10/0/0`（TP 默认）——音频路径读 p[]，故换预置/换引擎不换声音。换引擎 `ENG TRACK 0 1 0` 同样只有索引变 |

## GREEN（新固件）

| 文件 | 内容 |
|---|---|
| `task-5-store-green1.txt` / `-green2.txt` / `-green3.txt` | `bash scripts/inject/store.sh` ×3 全部退出码 0、**208 PASS / 0 FAIL**；⑩ 换音段：`ENG PRESET 0 2 1` 的 71 项期望向量 == `ENG TRACK 0 2 1` 后逐项回读（71/71）、P_LEVEL/P_SLEN（`param_kept`）保留、换引擎 71/71 且 `P_E0..P_E7=[4,1,1,14,26,35,0,0]`（DIGITAL RHODES 预置写值，覆盖 `ENG DESC` 的 edit[] def）、按住 `KEY 0x42` 时换预置 ⇒ `ENG SNAP voices` 1→0、playing 不变；三次运行 h0/h1/h2 全同（`AF0BF455`/`80D74BF1`） |
| `task-5-store-transcript1/2/3.log` | 上面三次的串口原始回显（逐字节；只含固件回显 + `# [inject]` 工具注记） |
| `task-5-store-green-diff.txt` | 两次 GREEN 的 stdout 规范化 diff（仅「console 就绪 4.1s/4.2s」一行的主机侧计时差） |
| `task-5-tracks.txt` / `.log` / `-tracks-sel3.png` | `bash scripts/inject/tracks.sh`：11 PASS / 0 FAIL（TRACKS 页既有断言 + 3 处像素抽检 + 截屏） |
| `task-5-params-fresh.txt` / `.log` | `bash scripts/inject/params.sh` **独立启动后单跑**：134 PASS / 0 FAIL，九页全建 `pool_free=2,620` |

## 门禁现场发现：九页全建后的 LVGL 池悬崖（登记 DI-19，非本改动引入）

| 文件 | 内容 |
|---|---|
| `task-5-poolcliff-repro.txt` / `.log` | **可复现**：同一会话连续跑 `store.sh → tracks.sh → params.sh`（不重启）时，`UI step` 把池余量压到 **2,196B**，`UI fx` 绘制旋钮弧线 → `Load access fault` panic 重启（Core 1；`UI step` 后 `UI fx` 立即崩） |
| `task-5-poolcliff-first.log` | 同一位置的**首次**现场（Core 0，`MTVAL=0x00000076`）：`池增量=21868B 池剩余=2196B` → `UI page=fx` → `Guru Meditation`；本文件是「符号化」里 PC/RA 链的来源 |
| `task-5-poolcliff-symbols.txt` | panic 现场摘录 + `addr2line`：`get_next_line ← lv_draw_mask_radius(869) ← lv_draw_sw_mask_apply ← lv_draw_sw_arc(185) ← knob_draw`；`MTVAL=0x76` = NULL mask 描述符 + 字段偏移（`lv_malloc_zeroed` 失败后 `p->circle` 未判空） |
| 判断依据 | ① 崩溃发生在 `⑨g` 硬复位之后（引擎状态=工厂 + `PROJ LOAD`，与改前固件相同）；② 本任务不分配任何 LVGL 内存，boot 池用量 15,716/62,820 与改前逐字节相同；③ 触发量是**会话分配历史**决定的池余量（2,196 vs 2,620），不是某条命令。结构性修复归 P4-B Task 7（组作用域页面生命周期，ADR-005），见 `docs/deferred-issues.md` DI-19 |

## 门禁

| 文件 | 内容 |
|---|---|
| `task-5-selftest.log` / `task-5-selftest-run.txt` | `scripts/device-selftest.py`：全量 **182/182**（`identical=182 mismatch=0 checks=7 check_fail=0 deterministic=1`，`SELFTEST COMPARE … result=PASS`）——换音路径不碰 DSP，golden 不变 |
| `task-5-host-test.log` | `bash scripts/host-test.sh`：`ALL HOST TESTS PASSED (subset)`（上游 storage/project/upreset/fm6 与 182 golden） |
| `task-5-screenshot-selftest.log` | `python3 scripts/screenshot.py --selftest`：`selftest OK`（scripts/ 工具路径） |
| `task-5-manifest.txt` | `sha256sum -c third_party/MANIFEST.sha256` → **233/233 OK**（`third_party/**` 零修改） |
| `task-5-build1.log` / `task-5-build2.log` | 构建两遍：均 rc=0、编译警告 **0**（`grep -c warning:` = 0）；镜像 `0x17cfe0`（分区 81% 余量） |
| `task-5-flash.log` | `scripts/flash.sh`：esptool 三段 `Hash of data verified` |
| `task-5-boot-log.txt` | RTS 硬复位后的启动回显：`App version: f2ea4e8`、`sloop_store: ready … base=0x97000 span=0x68000`、`stream started`、`UI 就绪: 9 页注册/1 已建`（池 15,716/62,820，与改前相同）、`InputManager 就绪：55 条映射`；无 fatal/abort/backtrace/panic/task_wdt |
| `task-5-bin-sha.txt` | 受测镜像与最终源码的对应：仅注释改动后重建的应用 `.bin` 变化来自 `.flash.appdesc`（构建时间/ELF SHA），而 `.flash.text/.flash.rodata/.flash.init_array` 两次独立构建逐字节相同 |

## 全值 SHA256（`sha256sum task-5-*`，本 readme 自身除外——它随本文编辑变化）

```
f1f1e883255de9304fbb66e069bcf07bcd6c01378eca8a7556fdd3e882009984  task-5-bin-sha.txt
6b3af32b48e7c354d20f75d5257582fdf8ffd3eb16d49e07973f082f79214cad  task-5-boot-log.txt
75d6e7ceb4f35c4628d8a1d160e45612f1efaf1b68db52a240ef54ea475da387  task-5-build1.log
0b5fc331e59f4de3c726cc23826e1bde79ee6c8e1864e183dd17ada94cc418fc  task-5-build2.log
63d0eee25716f0beb4251bcb35a330bb79fc1d78d0d69923548fe31fc47a4153  task-5-flash.log
9f2a5e748afe8d321c8255f9270b4f638229df71271dc76f7459713bde549041  task-5-host-test.log
110079993867708eee55e2eefaac044254dae3081838881d6c87b8124fcf420a  task-5-manifest.txt
ed4b6141759a754f67fc6eae8640584a96aa0a047139489c7e0bbb15d93829d6  task-5-params-fresh.log
9efe136fe74d964b6381b8bcd17fa14de41d6f790a7f153395ae72dacba4d17f  task-5-params-fresh.txt
a2c110a82c3a65d565f2e46050b8b1b49fab18c110e82d005a58683453f0b9ad  task-5-poolcliff-repro.log
b6a24b0b965fc70976b5005407cb02725750bcda38bf9dbd5041d6806f88a0fc  task-5-poolcliff-first.log
4f3628323ce8429bc37ad718f55c3cea1d24099c2008275a6dc4893d0bee12f0  task-5-poolcliff-repro.txt
547878fa0941fb06e4a358bc6827a1c419851d81079b2fd40f17b830ddbe98ab  task-5-poolcliff-symbols.txt
01eb93fcd75bea0d1729742c67bc1bd41facdeb7c97bc37d97bdfe18f4265aaf  task-5-red-transcript.txt
d6b354ea05eca214d2308b1664df1703ee98ef11beb99842ae6b265a8dcffd4a  task-5-screenshot-selftest.log
58d5acf94167a8e052a8634203ae31dfe3420928171e0661aef3615191ef0673  task-5-selftest.log
5233d8869815ab2976a818c6ba0358cb8f90e96718284d3be79e1d2f75cb16f0  task-5-selftest-run.txt
41bff4deaae5d9a35d493530416deb82f96a9a1b2195040722dcb59c309bd0ae  task-5-store-green1.txt
b1e486d04c0c33bd0cc8ada33754f13d293c23ea741aef7a3d21e4da976f2615  task-5-store-green2.txt
17e4e91e6a8713e6c6dfeb4681c50dcbb41aa6e6b0da69aff5010080bc67ff70  task-5-store-green3.txt
3b9b9b1d5c807f1265d75b73ab7f44a810427d7bf8a07f06e1016c91fc64268a  task-5-store-green-diff.txt
503bf309cf56e10ac0d10c7768a98b61a0c11bceadca810aa0857262e9f73661  task-5-store-red.log
9b1a8eae01a1a56602ff2f03692165d5dd62ae6a9dcaf41c5c9cbd88bfdda7bd  task-5-store-red-run.txt
78afd7737af40a1ce437706a17b28333500761001e88886ab66f9a1820fc9a23  task-5-store-transcript1.log
e331f5c123739e19c3a0393c9a53112332d61faf69f46df9169e525fe30d1ca8  task-5-store-transcript2.log
f8ffdd35a1c3c049033aeb3bd67d5b645b55ed097b445117f3db71b483f01872  task-5-store-transcript3.log
90358628ce1657b64d2d781623190871d9508218141eea8c7ec4c2df74697dfb  task-5-tracks.log
f54ccaf1f2b2802642e064746ce4d87c8aae5677ffabaa0b372da08180f98827  task-5-tracks-sel3.png
2f716ddecfa7965a91838af80dd133abd5bf581cf2f7d19e45ac83099b71aa39  task-5-tracks.txt
```

`third_party/sloop-fm1/**` 零修改：`(cd third_party && sha256sum -c MANIFEST.sha256)` →
233/233 OK（见 `task-5-manifest.txt`）。

## 评审修复轮（R17 / R18 / minors 1–2；受测镜像 `1a94ac3-dirty`——build 时 HEAD；修复 amend 进本任务同一提交，哈希以 `git log` 为准，受测树内容相同）

评审结论 Approved + 2 Important（R17 门禁盲区、R18 两条 `project_load` 分支无覆盖）+ 2 minor。
本节是修复轮的证据；上面各节保留首轮现场。

| 文件 | 内容 |
|---|---|
| `task-5fix-store-green1.txt` / `task-5fix-store-transcript1.log` | `bash scripts/inject/store.sh`：**223 PASS / 0 FAIL**，退出码 0。新增断言：⑩0 冻结态 `ENG PRESET ERR frozen`（`ENG FREEZE on` → 无任何 `P<id>=` 行 → `off` → 向量恢复）；⑩ **R17 独立 oracle**（`ENG TRACK 0 1 0` 后 21 项、`ENG TRACK 0 0 0` 后 23 项与硬编码上游表字面量一致）；⑩e/⑩f **R18** `STORE PROJ SYNTH fmt1\|foreign` 的 fallback 断言（见下）。h0/h1/h2 = `D160C9B7` / `07F7D713` / `D160C9B7`（首轮因 ⑩ 改动引擎状态，h0 与首轮 `AF0BF455` 不同；同一轮内三次相等） |
| `task-5fix-mutation-probe.txt` | **R17 反证**：把 `preset_values` 的 `P_ATK` 故意写成 `env[1]`（错一位）→ 设备 `PARAM 1` 回 85（RHODES）/112（808 BOOM），与上游 oracle 的 0 不符 |
| `task-5fix-mutation-store-red.txt` / `.log` | **同一变异固件上跑 store.sh**：`⑩ ENG TRACK 0 2 1 的 p[] == ENG PRESET 向量: 71/71 项一致`（预览与应用同源，看不出来）**而** `⑩ R17 oracle … 1/21 项与上游表不符（id 上游/实得：[(1, 0, 85)]）`、`[(1, 0, 112)]` → 证明确实需要独立 oracle；该轮随后在 ⑩e 的 `store_expect` 抛错（atk=112），EXIT=1 |
| `task-5fix-build1.log` / `task-5fix-build2.log` | fix 轮构建两遍：rc=0、编译警告 **0**；镜像 `0x17d5a0`（81% 余量） |
| `task-5fix-flash.log` | esptool 三段 `Hash of data verified` |
| `task-5fix-boot-log.txt` | `App version: 1a94ac3-dirty`、`sloop_store: ready`、`stream started`、`UI 就绪 9 页/1 建`（池 15,716/62,820，与改前相同）、`InputManager 55 条映射`；无 fatal/abort/backtrace/panic/task_wdt |
| `task-5fix-tracks.txt` / `.log` / `-tracks-sel3.png` | `tracks.sh`：11 PASS / 0 FAIL；截屏 PNG 与首轮 `task-5-tracks-sel3.png` **逐字节相同**（`f54ccaf1…`，UI 无改动） |
| `task-5fix-selftest.log` / `-run.txt` | 全量 **182/182**（`identical=182 mismatch=0 checks=7 deterministic=1`，`SELFTEST COMPARE … result=PASS`） |
| `task-5fix-bin-sha.txt` | 烧录镜像 sha256 + 可装载段（`.flash.text/.flash.rodata/.flash.init_array`）在烧录后紧接着的再构建间**逐字节相同**（应用 `.bin` 差异只在 `.flash.appdesc`） |

**R18 的回显（`STORE PROJ SYNTH`，诊断槽 3，破坏性，槽 0–2 不碰）**：

```
PROJ 3 SYNTH case=fmt1 OK
PROJ 3 SYNTH case=fmt1 trk k=0 engine=0 preset=0 steps=0 atk=0 e0=3   ← 0xFF ⇒ 808 BOOM 默认预置 + 步清空（工程里的 99 被覆盖）
PROJ 3 SYNTH case=fmt1 trk k=1 engine=1 preset=1 steps=0 atk=10 e0=0  ← 不带 0xFF ⇒ 保留工程里的引擎/预置
PROJ 3 SYNTH case=foreign OK
PROJ 3 SYNTH case=foreign trk k=1 engine=1 preset=0 steps=1 atk=0 e0=4 ← 引擎 14 → proj_foreign_parts ⇒ DIGITAL RHODES，图案保留
PROJ 3 SYNTH case=foreign trk k=0 engine=0 preset=0 …                  ← 不受 foreign 分支影响
```

**fix 轮全值 SHA256**

```
3838a9fac9f12f57e8b33138cb5f9c4df92220536526779bb9d960588e50ae1f  task-5fix-bin-sha.txt
257bf56fc5b87e510af0f218598bbd415edd8d58aa7f013f4df6e0eaef4b2d3e  task-5fix-boot-log.txt
38c57eb6d64799e86137f2ff4d817ab1948df522cbc9d41e0b18d65bcdbcb611  task-5fix-build1.log
6a0d0194ab00449b5f03a4799b68747d0fed1cd54438b45e5d4f674d0e3a20d0  task-5fix-build2.log
77979bc21627172e9d41c31a3da11270984eace23f91a059369b891fea2a3526  task-5fix-flash.log
7a45cdc510784e66a604a723c6b04b1dc39f7ac2ab1a87ed38745d1571a042bb  task-5fix-mutation-probe.txt
b5dc80f607c155cb012e6cd66b1917f33fa447c54b68808408d7341edec0d798  task-5fix-mutation-store-red.log
5d177af478388db34f699e75cd11f48166da69b83475a5cab9f4f3c55862a60f  task-5fix-mutation-store-red.txt
0e14f5a3b6b72e799b8a595a5df8f506a80cf3c5350a4d0e26557f37fffb489c  task-5fix-selftest.log
c134152204b0a0679db3d828d24c472e4ab1c450c0bf9cf25c849fa18666f87c  task-5fix-selftest-run.txt
3c4476afe4ccf297fcfaf77136326403c599abcae1a689db9c49ae5d0b6a417b  task-5fix-store-green1.txt
bc8a30acf1d23e30beead03581031fc38279ce2c39e6b91343043ea7ed82438a  task-5fix-store-transcript1.log
2d3e3805b92afa3bfe6b4db571aac131e6194d08382fac1f0f3b597d687be730  task-5fix-tracks.log
f54ccaf1f2b2802642e064746ce4d87c8aae5677ffabaa0b372da08180f98827  task-5fix-tracks-sel3.png
04f68f1630bbf4d56eb0606fac6080c6af617b786ed449f0665422d2f161dc61  task-5fix-tracks.txt
```
