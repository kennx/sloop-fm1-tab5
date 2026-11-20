#ifndef SLOOP_STATE_H
#define SLOOP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"

/* 状态层（spec §3.2 / ADR-005）：AppState = 引擎投影（core 快照）+ UI 局部态。
 *
 * 并发规则：两条写路径——input 任务的编辑写（state_apply）与 UI 任务的局部态写
 * （state_set_page/focus/layer）；UI 读一律经 state_get_copy 副本。
 * state_apply/state_sync_from_engine/state_get_copy/state_set_* 由内部互斥锁串行化
 * （HOST_TEST 下为空实现，设备上为 FreeRTOS mutex）。所有函数不得在 ISR 中调用。
 *
 * 执行端：阶段 3 为本地引擎（src/cmd.c 调 sloop_core）；阶段 5 换成 FM-1 会话时
 * 只替换 cmd.c 的分发与 sync 的拉取来源，本接口不变。
 *
 * 选轨与逐轨 mute/solo 与其他字段一样是**引擎真值**：state_apply 经引擎写入口
 * （sloop_core_select/set_mute/set_solo）落地后同写投影，state_sync_from_engine
 * 整份覆盖、无状态层例外。串口 `STATE SNAP` 与 `ENG SNAP` 对这三类值同源，
 * 音频效果即上游 trk_silent() 口径。 */

#define APP_STATE_PAGES 4u      /* PLAY / TRACKS / STEP / TRANSPORT */
#define APP_STATE_TRACKS 4u     /* 三条合成轨 + 鼓轨 */
#define APP_STATE_STEPS 16u     /* 快照/网格的步视窗列数 */
#define APP_STATE_NOTE_DEFAULT 60u
#define APP_STATE_VEL_DEFAULT 100u

typedef enum {
    CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SELECT_TRACK,
    CMD_TOGGLE_STEP, CMD_SET_BPM, CMD_SET_TRACK, CMD_MUTE, CMD_SOLO,
    CMD_ERASE_NOTE,
} edit_cmd_type_t;

/* 字段约定（未列出的字段忽略；越界参数一律忽略——不调引擎、不改状态）：
 *  CMD_NOTE_ON/OFF   a=track(0..3) v=note(0..127)；NOTE_ON 同时更新 last_note
 *  CMD_PLAY/STOP     （STOP = play(false)）
 *  CMD_SELECT_TRACK  a=track(0..3)：选中轨（与 CMD_SET_TRACK 等价）；副作用：同时把
 *                    focus_track 设为该轨（TRACKS 卡片点击依赖它落地「设焦点轨 + 选轨」）
 *  CMD_SET_TRACK     a=track(0..3)：选中轨（控制方裁定：SET_TRACK 的语义即选轨）
 *  CMD_TOGGLE_STEP   a=track(0..2) b=step(0..63)：空 → 开：清空残留字段
 *                    （note[]/lvl/rat/flags）后写 last_note（无则 60）、
 *                    n=1/vel=100/time=NOTE；开→关：整步清空（n=0、
 *                    note[]/vel/lvl/rat/flags=0、time=REST）并折叠其后的
 *                    TIE 连缀（对齐上游 note_del：note_span_set(t,i,1) 把
 *                    承载该音的 n==0 TIE 折回 REST，遇非 TIE / 带音符的 TIE 停）。
 *                    两种路径均 read-modify-write，保留 micro/fill（上游步表外字段）。
 *                    「空/开」判定见 state_step_is_on()：n>0 且 time==NOTE；
 *                    n>0 但 time 为 TIE/REST 的步算空（切换 → 开，不是 → 关）。
 *                    鼓轨（track 3）MVP 只读，忽略。
 *  CMD_SET_BPM       v 钳位到 40..240（上游 params.c 的 GP[G_BPM]）后写 G_BPM
 *  CMD_MUTE/CMD_SOLO a=track(0..3)：翻转该轨 mute/solo 位（solo 以整轨掩码写引擎）
 *  CMD_ERASE_NOTE    a=track(0..2) b=step(0..63) v=note(1..127)：从**显式指定的步**
 *                    删除该音符（步号由调用方在派发时解析，状态层不读 focus_step）。
 *                    read-modify-write：其余音符前移，每音符字段（lvl/rat 的 2bit、
 *                    flags 的 CONT 位）随之前移；最后一个音符删除后整步清空并对齐
 *                    上游折叠 TIE 连缀（同 CMD_TOGGLE_STEP 的关路径）。
 *                    步里没有该音符（或步已空）时不写引擎。鼓轨忽略。
 */
typedef struct {
    edit_cmd_type_t type;
    uint32_t a, b;
    uint16_t v;
} edit_cmd_t;

typedef struct {
    sloop_core_snapshot_t core; /* 引擎投影（含 selection/mute/solo：均为引擎真值） */
    uint8_t page;               /* 当前页（app_page_t） */
    uint8_t focus_step;         /* STEP 页焦点步（0..15） */
    uint8_t focus_track;        /* 焦点轨（0..3） */
    uint8_t last_note;          /* 最近演奏音；0 = 还没弹过（note 0 不记忆） */
    uint8_t layer_held;         /* 层键位掩码（APP_LAYER_*，InputManager 写、UI 读） */
} app_state_t;

/* layer_held 的位义（ADR-006 层键）：InputManager 维护，UI 显示提示用。 */
#define APP_LAYER_FX   0x01u /* Sym */
#define APP_LAYER_EDIT 0x02u /* Aa：擦除聚焦步音符 */
#define APP_LAYER_SEL  0x04u /* Ctrl：1–4 选轨 */
#define APP_LAYER_GLO  0x08u /* Alt：1–4 静音 / 5–8 独奏 */

typedef enum {
    APP_PAGE_PLAY, APP_PAGE_TRACKS, APP_PAGE_STEP, APP_PAGE_TRANSPORT,
} app_page_t;

/* 「有音」判定，与上游 seq.c 的 sp_on 同口径：n>0 且 time==NOTE（ST_NOTE=0）；
 * n>0 但 time 为 TIE/REST 的步算「空」。状态层的步切换与 STEP 网格渲染共用本
 * 判定，两侧口径不得各写一份。 */
static inline bool state_step_is_on(uint8_t n, uint8_t time)
{
    return n > 0 && time == 0;
}

/* 初始化（设备上同时建互斥；须在引擎 init 之后、任何 apply/read 之前调用）。 */
void state_init(void);
/* 执行一条编辑命令：引擎调用 + AppState 投影同步更新。写路径有两条——input 任务
 * 的编辑写（本函数）与 UI 任务的局部态写（state_set_*），都由内部互斥串行化。 */
void state_apply(const edit_cmd_t *cmd);
/* 从执行端拉取快照并合并（UI 定时器 30–60ms 调用）。 */
void state_sync_from_engine(void);
/* 互斥复制一份当前 AppState；UI 只读副本。 */
void state_get_copy(app_state_t *out);

/* UI 局部态写入口（UI 任务的写路径：无 EditCmd 语义，不进引擎/远端）。
 * state_apply 是 input 任务的编辑写路径；两者都走内部互斥。 */
void state_set_page(uint8_t page);
void state_set_focus(uint8_t track, uint8_t step);
void state_set_layer(uint8_t mask);

#endif
