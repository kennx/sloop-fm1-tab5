#ifndef SLOOP_STATE_H
#define SLOOP_STATE_H

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
 * MVP 约定（延迟项见 docs/deferred-issues.md DI-9）：选轨与逐轨 mute/solo 目前
 * 没有引擎写入口（sloop_core 只落了快照/步读写/全局写），由本层持有且 sync 不
 * 覆盖；因此它们的音频效果（真实静音/独奏）尚不生效，UI 状态一致。串口观察这
 * 三类值用 `STATE SNAP`（`ENG SNAP` 只有引擎真值）。 */

#define APP_STATE_PAGES 4u      /* PLAY / TRACKS / STEP / TRANSPORT */
#define APP_STATE_TRACKS 4u     /* 三条合成轨 + 鼓轨 */
#define APP_STATE_STEPS 16u     /* 快照/网格的步视窗列数 */
#define APP_STATE_NOTE_DEFAULT 60u
#define APP_STATE_VEL_DEFAULT 100u

typedef enum {
    CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SELECT_TRACK,
    CMD_TOGGLE_STEP, CMD_SET_BPM, CMD_SET_TRACK, CMD_MUTE, CMD_SOLO,
} edit_cmd_type_t;

/* 字段约定（未列出的字段忽略；越界参数一律忽略——不调引擎、不改状态）：
 *  CMD_NOTE_ON/OFF   a=track(0..3) v=note(0..127)；NOTE_ON 同时更新 last_note
 *  CMD_PLAY/STOP     （STOP = play(false)）
 *  CMD_SELECT_TRACK  a=track(0..3)：选中轨（与 CMD_SET_TRACK 等价）
 *  CMD_SET_TRACK     a=track(0..3)：选中轨（控制方裁定：SET_TRACK 的语义即选轨）
 *  CMD_TOGGLE_STEP   a=track(0..2) b=step(0..63)：空（n==0）→ 开：写 last_note
 *                    （无则 60）、n=1/vel=100/time=NOTE/flags=0；开→关：n=0/
 *                    time=REST。均 read-modify-write，保留 lvl/rat/micro/fill。
 *                    鼓轨（track 3）MVP 只读，忽略。
 *  CMD_SET_BPM       v 钳位到 40..240（上游 params.c 的 GP[G_BPM]）后写 G_BPM
 *  CMD_MUTE/CMD_SOLO a=track(0..3)：翻转该轨 mute/solo 位 */
typedef struct {
    edit_cmd_type_t type;
    uint32_t a, b;
    uint16_t v;
} edit_cmd_t;

typedef struct {
    sloop_core_snapshot_t core; /* 引擎投影；selection/mute/solo 由本层持有（见上） */
    uint8_t page;               /* 当前页（app_page_t） */
    uint8_t focus_step;         /* STEP 页焦点步（0..15） */
    uint8_t focus_track;        /* 焦点轨（0..3） */
    uint8_t last_note;          /* 最近演奏音；0 = 还没弹过（note 0 不记忆） */
    uint8_t layer_held;         /* 层键位掩码（位义由 InputManager 定义） */
} app_state_t;

typedef enum {
    APP_PAGE_PLAY, APP_PAGE_TRACKS, APP_PAGE_STEP, APP_PAGE_TRANSPORT,
} app_page_t;

/* 初始化（设备上同时建互斥；须在引擎 init 之后、任何 apply/read 之前调用）。 */
void state_init(void);
/* 执行一条编辑命令：引擎调用 + AppState 投影同步更新（唯一写者=input 任务）。 */
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
