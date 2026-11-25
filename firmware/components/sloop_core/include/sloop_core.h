#ifndef SLOOP_CORE_H
#define SLOOP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 线程/所有权规则：sloop_core_render 与所有改变引擎状态的 API（track/note/hit/
 * play/set_global/set_params/write_step/set_mute/set_solo/select）以及只读的
 * read/read_params/read_step 共享内部互斥锁，可从不同任务调用；不得在 ISR 中
 * 调用。selftest 与音频流互斥（流式期间返回 busy）。
 * 诊断模式（sloop_core_diag_freeze，console `ENG SELFTEST` 用）：冻结标志置位后，
 * read 返回冻结快照、read_step 返回全零、写 API 直接丢弃（均不取锁）——新到的调用
 * 走冻结分支、不再取锁排队，诊断期不会有新的锁等待者；UI 在自检期间停在最后一帧、
 * 键盘编辑不落地，结束后下一拍自动对齐（实现因此只在首尾各取一次短锁，见 ADR-007）。
 * selftest 开始/结束各恢复一次 init 快照，会丢弃运行期编辑——阶段 3 UI 接管状态后，
 * 这条对用户可见。 */
#define SLOOP_CORE_NENGINES 12u
#define SLOOP_CORE_P_COUNT  71u
#define SLOOP_CORE_G_COUNT  50u
#define SLOOP_CORE_RATE     44100u
#define SLOOP_CORE_FRAMES   256u

esp_err_t sloop_core_init(void);
esp_err_t sloop_core_status(void);
void sloop_core_render(int32_t *block, uint32_t frames);
/* 换引擎 + 预置**并载入参数**（上游 ui.c 的 set_engine/set_engine_of + apply_preset_to）：
 * 引擎变 → 锁还原、引擎默认值、预置 0；预置变 → 预置的整份声音写入 p[]（混音/图案/
 * 调性等 param_kept 参数保留）。引擎与预置都不变时不动参数（与上游 UI 的
 * set_engine/apply_preset 只在动作时调用的口径一致）。音频路径读 p[]，所以这是
 * 「换预置 = 换声音」的唯一入口。对鼓轨/越界/诊断冻结直接丢弃。不得在 ISR 调用。 */
void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset);
/* 同上的分步入口（联动模式与 UI 换音用）：对指定轨应用预置 / 换引擎（换引擎后预置 0）。
 * 引擎锁内执行；越界、冻结与鼓轨丢弃（鼓轨没有引擎与预置）。不得在 ISR 调用。 */
void sloop_core_apply_preset(uint32_t track, uint32_t preset);
void sloop_core_set_engine(uint32_t track, uint32_t engine);
/* 预置预览（只读、无副作用）：以该轨**当前 p[]** 为基底，算出「引擎 engine 的预置
 * preset 应用后」的 71 个参数值（与 sloop_core_track 的写值同一实现 preset_values），
 * 不 sound_change、不 panic、不碰 FM6 状态。console `ENG PRESET` 用它给出可机器比对的
 * 期望向量。返回 false = 诊断冻结（此时 out 全零）或参数越界（track/engine 越界、
 * out == NULL）；console 据此打印 ERR 而不是零向量。鼓轨返回当前 p[]（apply 对鼓轨
 * 同样不改值）。参数锁在生效时 set_engine_of 会先 locks_restore，此时预览只作参考。 */
bool sloop_core_preset_preview(uint32_t track, uint32_t engine, uint32_t preset,
                               int16_t out[SLOOP_CORE_P_COUNT]);

/* TOOLS 三动作（P4-F Task 6；上游 ui_input.c:568-612 的 GO 按钮，语义清单见
 * docs/design/012-feature-parity-checklist.md §4）。作用于**选中轨**；一次动作一次引擎锁。
 *  CLRSEQ   清选中轨的步表（先 undo_mark，再 steps_clear ⇒ TRANSPORT 的 UNDO 可撤销）。
 *           走带中允许（上游同），playing 不变。
 *  INITSOUND 引擎默认值 + 预设 0（= sloop_core_set_engine(选中轨, 当前引擎)，param_kept
 *           的混音/图案参数保留）。走带中允许。选中鼓轨回 ESP_ERR_NOT_ALLOWED（上游
 *           page_desc 不给鼓轨这一项）。
 *  NEWPRJ   上游 project_new（ui_input.c:462-490 的逐行镜像，见 seq_tab5.c）：自行停走带、
 *           panic、四轨参数回默认 + TRK_DEF 默认声音、鼓 kit/tune/decay/mute 复位、除
 *           MIDI 类外全局回默认、清 solo/八度；A–D section 与 settings 对象不碰。
 *           唯一守卫：SONG 播放中且 arrangement_enabled ⇒ ESP_ERR_NOT_ALLOWED
 *           （"STOP THE SONG FIRST"，否则停止会把旧 loop 带回来）；其余走带状态自停后执行。
 * 返回：ESP_OK；ESP_ERR_INVALID_ARG（action 越界）；ESP_ERR_NOT_ALLOWED（见上）；
 * ESP_ERR_INVALID_STATE（诊断冻结，锁外/锁内各查一次）。不得在 ISR 调用。 */
typedef enum {
    SLOOP_TOOL_CLRSEQ = 0,
    SLOOP_TOOL_INITSOUND = 1,
    SLOOP_TOOL_NEWPRJ = 2,
} sloop_tool_t;
esp_err_t sloop_core_tools(uint32_t action);
void sloop_core_note(uint32_t track, uint32_t note, bool on);
void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity);
void sloop_core_play(bool on);
uint32_t sloop_core_rate(void);
uint32_t sloop_core_frames(void);
int sloop_core_selftest(const char *case_name);
/* 最近一次自检运行器的实际计数（ran=0 = 尚无结果）；不假定全量用例数。 */
void sloop_core_selftest_stats(uint32_t *ran, uint32_t *failed);

/* 鼓轨一步的原始状态（上游 `dstep_t`，10 字节）：`on` 为 2×8 lane 位图
 * （lane k = bit k），`lvl`/`rat` 每 lane 2 bit（lane k = 字节 k/4 的
 * 位 2*(k%4)..+1）。`sloop_core_snapshot_t.dstep_raw[]` 按窗口列投影它，
 * `sloop_seq_read/write_dstep` 直接读写同一个上游结构。 */
typedef struct { uint8_t on[2], lvl[4], rat[4]; } sloop_dstep_t;

/* 只读快照：MVP 屏所需状态一次性拷出（内部持引擎锁，数百字节拷贝）。
 * 字段与编辑器协议词表对齐：bpm 是全局 BPM（上游 G_BPM，即编辑器协议里的
 * TEMPO）；trk[k].len/div 是轨道的 LEN/DIV；step_pos[k] 是播放头（seq_idx 模
 * len，停止时 0）。step[][] 与 dstep[] 是**16 列步视窗**：sloop_core_read 从
 * 引擎步 0 起取，sloop_core_read_window 从 window_start 起取（绝对步
 * window_start+c，见 sloop_state 的 step_window_t）。step[][] 只覆盖三条合成轨；
 * 鼓轨（track 3）的步内容由 dstep[] 聚合给出，step[3] 恒为零（上游 step_t 与
 * dstep_t 是同一 union，按 step_t 解读鼓步无意义）。鼓轨的 trk[3].engine/preset
 * 同样无意义（上游对鼓轨报 NENGINES，它没有引擎与预设）；这里保留上游原值、
 * 不置零，显示层按轨别决定是否使用。合成窗口步除原有字段外还投影每音符打包的
 * lvl/rat 与该绝对步的 micro/fill；字段只追加，既有 UI/state 访问保持源码兼容。
 * trk[k].voices 是引擎真值：该轨当前 active 的
 * 话音数（note-on 后 >0、panic/淡出结束后归 0），用于断言音符是否被释放。
 *
 * 鼓轨的**原始** 16-lane 位图另由 `dstep_raw[]` 投影（P4-C Task 9：STEP 页在每个
 * 鼓格里画出 4×4 lane 网格并允许逐 lane 切换 on 位）。`dstep[]` 保持不变（聚合
 * on/最强 lvl/rat，ENG SNAP 与 STATE SNAP FULL 的口径），并补该步的 micro/fill
 * （上游 `trk[3].micro/step_fill`，鼓步条件与 nudge 与合成轨同一语义）。 */
typedef struct {
    bool playing;
    uint16_t bpm;
    uint8_t selected_track;
    struct {
        uint8_t engine, preset, level, mute, armed, solo;
        uint8_t len, div;
        uint8_t voices;
    } trk[4];
    struct {
        uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
        uint8_t lvl; uint8_t rat; int8_t micro; uint8_t fill;
    } step[4][16];
    struct { uint8_t on; uint8_t lvl; uint8_t rat; uint8_t micro; uint8_t fill; } dstep[16];
    sloop_dstep_t dstep_raw[16]; /* 鼓轨窗口列的原始 16 lane on/lvl/rat 字节 */
    uint8_t step_pos[4];
} sloop_core_snapshot_t;
void sloop_core_read(sloop_core_snapshot_t *out);

/* 步窗快照：与 sloop_core_read 同，但 step[k][c]/dstep[c] 取引擎绝对步
 * start + c（start 向下对齐到 16 的倍数并钳位到 0..48，即 4 个 bank 之一）。
 * 一次引擎锁内完成——状态层每拍只需一次取锁就能把 STEP 页窗口列对齐到引擎真值，
 * 不必逐列 read_step。冻结态（诊断期）与 sloop_core_read 相同：返回冻结快照
 * （基 0），UI 本就在 quiesce 中暂停。 */
void sloop_core_read_window(uint32_t start, sloop_core_snapshot_t *out);

/* 单步读写：完整上游 step_t 语义（n/note/vel/time/flags 原样，lvl/rat 为每音符
 * 2bit 打包），外加 micro（-32..31）与 fill（0..3）保留字段。读改写由调用方做；
 * write 是不丢字段的全量写（调用方须把读到的 lvl/rat/micro/fill 原样带回）。
 * 鼓轨（track >= 3）不可用这个结构表达：read 返回全零，write 忽略。越界
 * （track/idx）同样忽略/返回全零。 */
typedef struct {
    uint8_t n; uint8_t note[4]; uint8_t vel; uint8_t time; uint8_t flags;
    uint8_t lvl; uint8_t rat;
    int8_t micro; uint8_t fill;
} sloop_step_t;
void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out);
void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in);

/* 全局参数写：按上游 GP[id] 的 min/max 钳位后写 song.g[id]（id 越界忽略）。
 * 走引擎锁；只写值，不做上游 UI 的联动副作用（如 G_BPM 的显示刷新不需要）。
 * 不要用它执行 GO/动作全局（G_LOAD/G_SAVE/G_CLRSEQ/G_INITSND/G_NEWPRJ/G_ENGSEL/
 * G_INFO 等）——上游这些是瞬时的 UI 按钮/查询，写 song.g[] 只会改值、不执行动作。 */
void sloop_core_set_global(uint32_t id, int16_t value);

/* 批量参数写（阶段 4 P4-A，spec §4.1）：一次引擎锁内按顺序应用 n 对 (id, value)，
 * 每条值按与 sloop_core_desc 同源的描述符钳位——id < P_E0 → TP[id]；P_E0..P_E7 →
 * 合成轨（0..2）取 ENGINES[trk[track].eng_req]->edit[id - P_E0]，鼓轨（3）只有
 * id == P_E0（= DRUM_KIT_DESC，即 kit）；其余 id 无描述符。无描述符的 id 逐条忽略、
 * **不中止整批**（有效项照常落地；真值用 sloop_core_read_params 回读）。越界 track、
 * NULL 数组、n == 0 与冻结态整批丢弃。不做上游 UI 的联动副作用，也不触发
 * sound_change（换音色仍走 sloop_core_track）。不得在 ISR 调用。 */
void sloop_core_set_params(uint32_t track, const uint8_t *ids, const int16_t *values, uint32_t n);

/* 参数快照：一次引擎锁内拷出 trk[track].p[P_COUNT] 与 song.g[G_COUNT]（原始引擎
 * 真值，不重钳位）。p/g 可为 NULL（只取其一）。track 越界时 p 全零、g 照常；冻结态
 * （诊断期）与 read_step 同口径：不取锁、两者全零。 */
void sloop_core_read_params(uint32_t track, int16_t *p, int16_t *g);

/* 参数描述符投影（阶段 4 P4-A，spec §4.1）：只读投影上游 params.c 的 `param_desc_t`
 * 与引擎 `edit[8]` 表，不新建描述符表（single source）。表为静态常量，不取引擎锁、
 * 可在任意任务调用；`out` 的指针指向上游静态数据，调用方不得释放/改写。
 *  scope: SLOOP_DESC_TRACK  → TP[id]（id < P_E0 = 63；P_E0..P_E7 是引擎参数，走 ENGINE）
 *         SLOOP_DESC_GLOBAL → GP[id]（id < G_COUNT）
 *         SLOOP_DESC_ENGINE → ENGINES[engine]->edit[id]（engine < NENGINES，id < 8）
 *  engine 对 TRACK/GLOBAL 无意义（传 0）。**鼓轨哨兵**：engine == SLOOP_CORE_NENGINES
 *  （12，上游 `ed_eng` 对鼓轨报「无引擎」的同一取值）且 id == 0 时返回上游
 *  `DRUM_KIT_DESC`（鼓轨的 P_E0 就是 kit）；哨兵的 id != 0 返回 false（鼓轨没有
 *  P_E1..P_E7）。越界/未命中返回 false 且不改 `out`。
 *  fmt 为上游 F_* 编码（core.h）；unit 原样投影上游可选单位（多数为 NULL——PCT/DB/
 *  TIME 等显示单位由 fmt 决定，见上游 param_format）。enums/enum_count 在 names
 *  非空时给出：F_ENUM 每个取值一个名字；F_INT + names 为 0 终止表（值域均分映射，
 *  同 param_format）。引擎的 `desc` 钩子与 INSERT/ENV SYNC 这类依赖轨道状态的动态
 *  描述符不投影（只给基础表）——需要它们时用 sloop_core_desc_track_live。 */
typedef enum { SLOOP_DESC_TRACK, SLOOP_DESC_GLOBAL, SLOOP_DESC_ENGINE } sloop_desc_scope_t;
/* desc.fmt 的公开镜像（上游 core.h 的 F_* 编码）。`sloop_core_desc` 原样投影上游值，
 * 这里给出名字以便 UI 按常量分支格式化；selftest.c 用静态断言保证两者一致
 * （换上游版本时编译期报错，而不是显示层静默走错分支）。 */
typedef enum {
    SLOOP_DESC_FMT_INT = 0,
    SLOOP_DESC_FMT_PCT,
    SLOOP_DESC_FMT_BIPCT,
    SLOOP_DESC_FMT_TIME,
    SLOOP_DESC_FMT_LFOHZ,
    SLOOP_DESC_FMT_CUTOFF,
    SLOOP_DESC_FMT_DB,
    SLOOP_DESC_FMT_SEMI,
    SLOOP_DESC_FMT_ENUM,
    SLOOP_DESC_FMT_BPM,
    SLOOP_DESC_FMT_NOTE,
    SLOOP_DESC_FMT_ONOFF,
    SLOOP_DESC_FMT_OCT,
    SLOOP_DESC_FMT_STEPS,
    SLOOP_DESC_FMT_SWING,
    SLOOP_DESC_FMT_FILT,
} sloop_desc_fmt_t;
typedef struct {
    const char *label, *unit;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *enums;
    uint8_t enum_count;
} sloop_desc_t;
bool sloop_core_desc(sloop_desc_scope_t scope, uint32_t engine, uint32_t id, sloop_desc_t *out);

/* 随轨道状态取描述符（P4-F Task 3）：直接转调上游 params.c 的 track_desc()，不建任何
 * 镜像/范围表。对轨 0..3（越界回 false）与 id < P_COUNT（越界回 false）应用、且只应用
 * track_desc() 的规则：
 *  1. 鼓轨（轨 3）的 id == P_E0 → DRUM_KIT_DESC（kit，= sloop_core_desc 的鼓轨哨兵）；
 *  2. id == P_IA..P_IC → ins_desc(P_ITYPE)：TYPE 1..4 (SOFT..FUZZ) = DRIVE/TONE/LEVEL，
 *     5 (CRUSH) = BITS/RATE(单位 Hz)/LPF，6..8 (PHASR..CHOR) = RATE/DEPTH/FDBK，
 *     OFF 与越界 TYPE = TP[] 的 INS A/B/C；
 *  3. P_ESYNC 开且 id == P_ATK/P_DEC/P_REL → ESYNC_DESC（F_INT + 音符名表 ESYNC_NAMES，
 *     范围与默认值同 TP[]）；
 *  4. id == P_E0..P_E7 → 该轨 eng_req 引擎的 desc 钩子（非 NULL 且命中时；LOFI/NOISE/PHYS
 *     按模式换 label/names/unit），否则该引擎的 edit[k]。引擎取 t->eng_req（上游同一字段；
 *     淡入淡出期间可能与 t->engine 短暂不同，与 sloop_core_set_params 的钳位表一致）；
 *  5. 其余 id < P_E0 → TP[id]。
 * 不应用上游 page_desc() 的页面规则（page_for_drum / SC_STEP、SC_TRK 作用域 / GLOBAL），
 * 也不做 editor.c ed_tdesc 的静态化。
 * 本函数在 1 与 4 之外对鼓轨的 P_E1..P_E7 回 false（上游 track_desc 对它们会落到引擎
 * 分支）。本函数不建任何范围表：min/max/def/enums 全部取上游返回的那条描述符——动态
 * 规则按上游现状只换 label/names/unit/fmt、范围与默认值同静态表（这是上游的事实，
 * 不是本函数的保证；上游若让动态范围偏离静态表，这里如实跟随）。
 * 上游 static 表与引擎钩子只读 t->p[]/eng_req 的整数值，**不取引擎锁**、可在任意任务
 * 调用（与 sloop_core_desc 同口径）；冻结态同样可读。**冻结期间的错位**：诊断冻结时值投影
 * 不再跟随引擎——`sloop_core_read`/`read_window` 回冻结时的快照、`read_params`/`read_step`
 * 回全零——而本函数的标签仍取 live `trk[]`（冻结只挡写入口；`ENG SELFTEST` 期间 UI 同时
 * 静默，手工 `ENG FREEZE ON` 时参数页可能显示「新标签 + 旧值/零值」）。
 * out 为 NULL 时只判有无描述符。
 * 未命中返回 false 且不改 out。`out` 的指针指向上游静态数据，调用方不得释放/改写。 */
bool sloop_core_desc_track_live(uint8_t track, uint32_t id, sloop_desc_t *out);

/* ---- SOUND 页投影（P4-F Task 5）：选中轨的引擎 / 预置 / 名字 -------------------------
 * UI 不读上游全局：ENGINE/PRESET 页眉的文案、± 的环绕范围与 CMD_SET_PRESET 的越界判定
 * 都读这份投影。引擎取 t->eng_req（与 sloop_core_desc_track_live 的引擎槽同一字段），
 * 预设下标按上游口径 `t->preset % npresets`；名字是上游 ENGINES[]/presets[]/DRUM_KIT_NAMES
 * 的静态串的拷贝（engine_name ≥ 12、preset_name/kit_name ≥ 16 字节放得下上游最长的名字
 * 「DIGITAL」7 字符 / 「PLUGG BASS」10 字符 / 「ACOUSTIC」8 字符；scripts/synth-test.sh
 * 钉住不截断）。鼓轨没有引擎与预置：drum=1、engine/npresets 为 0、kit_name = P_E0 钳位后
 * 的 kit 名（上游 drum_kit() 同法）。不取引擎锁、冻结态同样可读（与
 * sloop_core_desc_track_live 同口径：只读 trk[] 的整数字段与静态表）。越界 track 或
 * out == NULL 返回 false 且 *out 清零（若非 NULL）。 */
#define SLOOP_ENGINE_NAME 12u
#define SLOOP_PRESET_NAME 16u
typedef struct {
    uint8_t engine, preset; /* 选中合成轨的引擎 / 预置下标（鼓轨为 0） */
    uint8_t nengines;       /* SLOOP_CORE_NENGINES */
    uint8_t npresets;       /* 该引擎的工厂预置数（鼓轨 0） */
    uint8_t drum;           /* 1 = 鼓轨（没有引擎/预置；kit 名在 kit_name） */
    char engine_name[SLOOP_ENGINE_NAME];
    char preset_name[SLOOP_PRESET_NAME];
    char kit_name[SLOOP_PRESET_NAME]; /* 鼓轨的 kit 名；合成轨为空串 */
} sloop_sound_snapshot_t;
bool sloop_core_sound_read(uint8_t track, sloop_sound_snapshot_t *out);

/* ---- DSYN：SYN1..SYN4 的 22 字节鼓音色（P4-D Task 2，spec §4.1） -------------------
 * 上游 `dsnd_t`（drum_synth.c）是 22 字节整型结构，音色按字节与编辑器协议一致。
 * 编辑面把它拆成 **24 个虚拟字段**（顺序固定，编号即协议）：`flt` 拆成 FLT_MODE
 * （低 3 位）与 FLT_RES（bit3..7），`src` 的 bit4（CLAP）单独一项。写一个字段不得改动
 * 其它位/字节。范围取自 `dsnd_t` 的字节宽度与 `dsu_fix_sound` 的钳位方向；注意
 * `dsu_fix_sound` **不钳** hold/nhold/t2/level/flt/fenv，所以这些字段的范围在这里
 * 无条件钳位（见下）。
 * 所有写入口持引擎锁；诊断冻结时写回 `SLOOP_DSYN_INVALID`、读回 ESP_ERR_INVALID_STATE。
 * 与上游 editor_dsyn.c 同语义：成功的单字段写与工厂 kit 套用都置 `dsu_dirty`。 */
#define SLOOP_DSYN_KITS   4u
#define SLOOP_DSYN_LANES  16u
#define SLOOP_DSYN_FACTORY_KITS 32u /* 工厂 kit 数（上游生成的 DS_NKITS）；`load_factory` 的 src 上限 */
#define SLOOP_DSYN_FIELDS 24u /* 22 字节 + FLT 拆 MODE/RES 两虚拟字段 */
#define SLOOP_DSYN_NAME   8u
/* 无效字段/越界 kit·lane/诊断冻结的返回值。落在此范围外（最小字段范围是 FENV 的
 * -128..127），所以不会与合法值混淆。 */
#define SLOOP_DSYN_INVALID ((int16_t)INT16_MIN)
typedef enum {
    SLOOP_DSYN_WAVE = 0, SLOOP_DSYN_SRC, SLOOP_DSYN_CLAP, SLOOP_DSYN_PITCH,
    SLOOP_DSYN_FINE, SLOOP_DSYN_BEND, SLOOP_DSYN_BTIME, SLOOP_DSYN_HOLD,
    SLOOP_DSYN_DECAY, SLOOP_DSYN_TLEV, SLOOP_DSYN_T2, SLOOP_DSYN_T2LEV,
    SLOOP_DSYN_CLICK, SLOOP_DSYN_NLEV, SLOOP_DSYN_NHOLD, SLOOP_DSYN_NDEC,
    SLOOP_DSYN_FLT_MODE, SLOOP_DSYN_FLT_RES, SLOOP_DSYN_FCUT, SLOOP_DSYN_FENV,
    SLOOP_DSYN_HPF, SLOOP_DSYN_CHIP, SLOOP_DSYN_DRIVE, SLOOP_DSYN_LEVEL,
} sloop_dsyn_field_t;
typedef struct {
    uint8_t used;      /* 该 lane 有音色数据（上游 kit 总是 16 个，读成功即 1） */
    uint8_t sound[22]; /* 原始 dsnd_t 字节（与上游/编辑器逐字节一致） */
} sloop_dsyn_sound_t;
/* 状态层/UI 的 DSYN 投影（spec §4.1）：kit/lane 是**当前选择**（状态层保存的 UI 局部态，
 * 不是引擎真值），其余字段由 sloop_core_dsyn_read 按该选择读出；dirty 取存储层对
 * 「RAM 的 SYN kit 与最近一次成功写盘不一致」的记账（sloop_store_dsu_dirty）。 */
typedef struct {
    uint8_t kit, lane;
    char name[SLOOP_DSYN_NAME + 1];
    uint8_t crush, src, dirty;
    uint8_t sound[22];
} sloop_dsyn_snapshot_t;
/* 24 字段描述符（自定义表，不来自 params.c）：label/fmt/min/max/enums/unit。
 * FLT_MODE 的 enums 有 8 项，索引 4（只有 DF_ALL 位、没有模式）读作 OFF——写 4 也被
 * 归一为 OFF，与上游「单独 4 是无效组合」一致。 */
bool sloop_core_dsyn_desc(uint32_t field, sloop_desc_t *out);
/* 读一个 lane 的 22B 与 kit 级 name/crush/src（后三者可为 NULL）。越界回
 * ESP_ERR_INVALID_ARG，冻结回 ESP_ERR_INVALID_STATE。`name` 至少 9 字节、NUL 结尾。 */
esp_err_t sloop_core_dsyn_read(uint32_t kit, uint32_t lane, sloop_dsyn_sound_t *out,
                               char name[9], uint8_t *crush, uint8_t *src);
/* 读/写单个虚拟字段（引擎锁内一次）。set 返回写后回读的值（钳位后的真值）；
 * 越界/冻结返回 SLOOP_DSYN_INVALID 且不写。 */
int16_t sloop_core_dsyn_field(uint32_t kit, uint32_t lane, uint32_t field);
int16_t sloop_core_dsyn_set(uint32_t kit, uint32_t lane, uint32_t field, int16_t v);
/* 写 **kit 级** CRUSH（0..255；低 nibble 丢位数、高 nibble 是 S&H−1，见上游 dkit_t）：
 * 引擎锁内钳位 0..255 → 写 `dsu.k[kit].crush` → `dsu_fix_kit`（名称/src/16 lane 原样，
 * crush 本身不在它的钳位范围）→ `dsu_dirty = 1`；返回写后回读值。kit 越界或诊断冻结
 * 返回 SLOOP_DSYN_INVALID（与 24 个声音字段同一个哨兵）。一次动作一次引擎锁。 */
int16_t sloop_core_dsyn_set_crush(uint32_t kit, int16_t v);
/* 纯解码（UI 投影用，不取锁、无副作用）：field 越界返回 SLOOP_DSYN_INVALID。 */
int16_t sloop_dsyn_field_from_sound(const uint8_t sound[22], uint32_t field);
/* 索引字段的显示文本（DECAY 类 → ms，由 DECAY_K 反解；FCUT/HPF → Hz；LEVEL → dB；
 * FLT_MODE → 名字；CHIP → 音名）。返回写入的字符数；0 = 本函数不覆盖该字段
 * （调用方走通用整数/枚举格式化）。 */
int16_t sloop_core_dsyn_text(uint32_t field, int16_t v, char *out, uint32_t n);
/* 套用工厂 kit（覆盖该 kit 的编辑，置 dsu_dirty）。kit 越界或
 * src >= SLOOP_DSYN_FACTORY_KITS（= 上游 DS_NKITS，编译期静态断言）回
 * ESP_ERR_INVALID_ARG，冻结回 ESP_ERR_INVALID_STATE。 */
esp_err_t sloop_core_dsyn_load_factory(uint32_t kit, uint32_t src);
/* 试听一段该 kit/lane（写上游 dsu_aud_* 队列，由音频路径消费；不改数据）。
 * 越界 kit/lane 与 velocity 0（「无声」）或 > 127 一律拒绝（上游 ED_DSYN_PLAY 同口径）。 */
void sloop_core_dsyn_audition(uint32_t kit, uint32_t lane, uint32_t velocity);
bool sloop_core_dsyn_dirty(void);

/* ---- FM6：155B voice 的深度 patch 编辑（P4-D Task 3，spec §4.1） --------------------
 * 上游 `fm6_patch[NPART][FP_SIZE+1]` 是每轨的 live patch（155 字节 + NUL）：6 个算子 ×
 * 21B（`FP_R1..FP_DET`，op 0 = 机器第 6 算子，与 `k * FP_OP` 同序）+ 19B voice
 * （`FP_PR1..FP_TRNSP`，绝对偏移 126..144）+ 10B 名称（`FP_NAME..+9`，145..154）。
 * 编辑面的编号（冻结，后续只能追加）：
 *   op 0..5 → 该算子块的 21 个字段，field 0..20 = `FP_R1..FP_DET`；
 *   op 6（`SLOOP_FM6_GLOBAL_OP`）→ 全局 29 项：0..18 = `FP_PR1..FP_TRNSP`（126..144），
 *              19..28 = 名称的 10 个字节（`FP_NAME + 0..9`，**只读**：屏幕不改名，但
 *              保留这 10 项让 `6×21 + 29` 覆盖 packed 记录里的全部名字字节；
 *              `sloop_core_fm6_set` 对它们返回 `SLOOP_FM6_INVALID`）。
 * 范围取上游 `fm6_max()`（`fm6_sanitize` 的同一张表）：写值先按描述符钳位、再经
 * `fm6_set_patch` 的 sanitize 落 patch。`ALG`（op 6, field 8）在一次引擎锁内额外清
 * `trk[track].p[P_E0]`（ALG 宏 → PAT），否则上游 `fm6_sync` 会用宏覆盖 patch 的算法、
 * 编辑听不到（spec §4.1「ALG 宏覆盖」）。写成功后 `fm6_slot[track] = p[P_E7]`
 * （上游 ED_FM6_PUT 同法），`fm6_poll` 因此不会在 PTCH 未变时用槽 patch 覆盖编辑。
 * 锁：`sloop_core_fm6_patch/field/set` 持引擎锁，非 FM6 轨的判定也在锁内（快照的
 * `engine_ok` 与 patch/slot 同拍）；诊断冻结在锁外先拒（命中时不取锁）。`sloop_core_fm6_desc`
 * 与 `sloop_fm6_field_from_patch` 是纯函数/纯解码——不取锁、无副作用、不受冻结影响。
 * 冻结时写回 `SLOOP_FM6_INVALID`、读/快照回 `ESP_ERR_INVALID_STATE`。
 * 轨号越界（含鼓轨，上游 NPART 之外的轨没有 live patch）：`sloop_core_fm6_patch` 回
 * `ESP_ERR_INVALID_ARG`；合成轨但引擎不是 FM6：`sloop_core_fm6_patch` 回
 * `ESP_ERR_NOT_ALLOWED` 并把 `engine_ok` 置 0（patch/slot 仍原样给出，UI 可显示锁定）、
 * 写/读字段回 `SLOOP_FM6_INVALID`。
 * `sloop_core_fm6_set` 返回写后回读值（钳位后的真值）；任何拒绝都返回哨兵
 * `SLOOP_FM6_INVALID`（= -1，落在所有字段范围之外），状态层据此映射错误码。 */
#define SLOOP_FM6_PATCH_SIZE   155u
#define SLOOP_FM6_OP_FIELDS     21u
#define SLOOP_FM6_GLOBAL_FIELDS 29u
#define SLOOP_FM6_GLOBAL_OP      6u /* op 取值：全局字段块 */
#define SLOOP_FM6_NAME_FIELDS   10u /* 全局字段 19..28 = 名称字节（只读） */
#define SLOOP_FM6_ALG_FIELD      8u /* 全局块里的 ALG（`FP_ALG - FP_PR1` = 134−126）：
                                     * 写它时适配层在同一次引擎锁内清 `p[P_E0]`（ALG 宏
                                     * → PAT），状态层因此要在成功后同拍重取 p[]，否则
                                     * UI 的 `ALG MACRO n` 提示会多留一拍 */
#define SLOOP_FM6_INVALID ((int16_t)-1)
typedef struct {
    uint8_t patch[SLOOP_FM6_PATCH_SIZE]; /* FM6 155B voice，未 pack */
    uint8_t slot;                        /* 当前 PTCH（P_E7），只读 */
    uint8_t engine_ok;                   /* 选中轨引擎 == FM6 */
} sloop_fm6_patch_snapshot_t;
/* 描述符：op 0..5 / op 6 = 全局；每个算子的 MODE 与全局 OKS/LKS 为 onoff，全局
 * ALG（"1".."32"）/LFW（TRI..S&H）/LPMS（"0".."7"）带枚举表；op/field 越界返回 false
 * 且不改 `out`。 */
bool sloop_core_fm6_desc(uint32_t op, uint32_t field, sloop_desc_t *out);
/* 拷出该轨的 155B live patch + slot + engine_ok（一次引擎锁）。 */
esp_err_t sloop_core_fm6_patch(uint32_t track, sloop_fm6_patch_snapshot_t *out);
/* 读/写单个字段（引擎锁内一次）。见上：非 FM6/越界/冻结/名字字段都回哨兵。 */
int16_t sloop_core_fm6_field(uint32_t track, uint32_t op, uint32_t field);
int16_t sloop_core_fm6_set(uint32_t track, uint32_t op, uint32_t field, int16_t v);
/* 纯解码（UI 投影用，不取锁、无副作用）：越界或 patch == NULL 回 `SLOOP_FM6_INVALID`。 */
int16_t sloop_fm6_field_from_patch(const uint8_t patch[SLOOP_FM6_PATCH_SIZE], uint32_t op, uint32_t field);

/* 选轨 / 静音 / 独奏写（SEL/GLO 层的可听路径）：分别写 song.sel、trk[k].p[P_MUTE]
 * （0/1，上游 N_ONOFF）与 song.solo（低 4 位独奏掩码）。走引擎锁，越界
 * （k>=NTRK）与冻结态直接丢弃。听感口径是上游 trk_silent()：P_MUTE 置位，或
 * 「有独奏且本位未置」即静音。 */
void sloop_core_set_mute(uint32_t k, bool mute);
void sloop_core_set_solo(uint8_t mask);
void sloop_core_select(uint32_t k);

/* 诊断模式（`ENG SELFTEST` 内部成对调用，不必由调用方执行）：freeze(true) 先持锁
 * 取一份快照再置位，之后 read 返回该冻结快照、read_step 返回全零、所有改变引擎
 * 状态的 API 直接丢弃（不取锁、不阻塞）；freeze(false) 清位。效果 = 自检期间 UI
 * 冻结在最后一帧、键盘编辑不落地。freeze 只保证「置位后新到的调用不取锁/不写」；
 * 全量自检另由 quiesce 钩子把 UI 定时器/LVGL 定时器/输入派发停掉（见下），保证
 * 自检期间系统里没有其它引擎使用方（见 ADR-007）。不得在 ISR 调用。 */
void sloop_core_diag_freeze(bool on);

/* 诊断期静默钩子：sloop_ui 启动成功后注册 sloop_ui_quiesce（暂停 30ms UI 定时器与
 * LVGL 定时器、输入任务阻塞丢弃事件）。sloop_core_selftest 在诊断期前后成对调用；
 * 返回 false = 静默未生效（UI 取 LVGL 锁超时），selftest 以 `result=FAIL
 * reason=quiesce` 拒绝运行（不冻结引擎——UI 定时器仍在读引擎）。未注册（UI 未启动、
 * host 自测）时为空操作。只保留一个钩子（后注册覆盖）；不得在 ISR 调用。 */
void sloop_core_diag_quiesce_hook(bool (*hook)(bool on));

#endif
