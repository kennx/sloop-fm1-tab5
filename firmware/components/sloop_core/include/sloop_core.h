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
void sloop_core_note(uint32_t track, uint32_t note, bool on);
void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity);
void sloop_core_play(bool on);
uint32_t sloop_core_rate(void);
uint32_t sloop_core_frames(void);
int sloop_core_selftest(const char *case_name);

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
 * 话音数（note-on 后 >0、panic/淡出结束后归 0），用于断言音符是否被释放。 */
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
    struct { uint8_t on; uint8_t lvl; uint8_t rat; } dstep[16];
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
 *  同 param_format）。引擎的 `desc` 钩子（依赖轨道状态的动态描述符）不投影，只给
 *  基础 edit[] 表。 */
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
