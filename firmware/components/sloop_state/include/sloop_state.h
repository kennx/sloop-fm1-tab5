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
 * 音频效果即上游 trk_silent() 口径。
 *
 * 参数投影 p[]/g[]：p 是**选中轨**的 trk[].p[P_COUNT] 投影、g 是 song.g[G_COUNT]
 * （引擎真值，不重钳位）。state_sync_from_engine 用一次 sloop_core_read_params 与
 * 窗口快照同拍整份覆盖；参数写入命令（CMD_SET_PARAM / CMD_SET_GLOBAL）落地后立即
 * 回读一次——UI 不直接调引擎。选轨变化后 p[] **同拍**换成新选中轨（cmd.c 的
 * cmd_select_track 在引擎选轨落地后立即回读，不等下一拍 sync；参数页按此显示）。
 */

/* 页数：4 基础页（PLAY/TRACKS/STEP/TRANSPORT）+ 3 参数页（GLOBAL/FX/ANALOG，由
 * sloop_ui 的策展布局表加入，见 ADR-008）。加页时同步 app_page_t、本宏与 console
 * 的页名表（后者有静态断言兜底）。 */
#define APP_STATE_PAGES 7u
#define APP_STATE_TRACKS 4u     /* 三条合成轨 + 鼓轨 */
#define APP_STATE_STEPS 16u     /* 步窗列数 = 一 bank 的步数（引擎共 64 步 = 4 bank） */
#define APP_STATE_NOTE_DEFAULT 60u
#define APP_STATE_VEL_DEFAULT 100u
/* CMD_SET_PARAM_BATCH 单次最多应用的对数（超出部分忽略并告警）。同时是状态层
 * 解包栈缓冲的上限：128 对 = 384B 临时栈（id 128B + 值 256B）。 */
#define APP_STATE_PARAM_BATCH_MAX 128u

/* CMD_SET_PARAM_BATCH 的载荷元素：小 POD，字段即引擎批量 API 的 (id, value)。
 * 自然对齐布局，sizeof == 4（id 1B + 1B 填充 + int16 2B；host/xtensa 一致，静态
 * 断言保证）；不要按字节流（编辑器协议 U35/pack7）解读本结构。
 * 生命周期：**同步 apply 语义**——数组只需在 state_apply() 返回前有效，状态层在
 * 同一调用内解包并下发引擎，不保存指针。 */
typedef struct {
    uint8_t id;
    int16_t v;
} sloop_param_pair_t;
_Static_assert(sizeof(sloop_param_pair_t) == 4, "sloop_param_pair_t layout drifted");

/* 步窗（UI 局部态，非引擎真值）：引擎步号恒为绝对索引 0..63；STEP 网格的列 c
 * 显示/编辑引擎步 window_start + c。window_start 16 对齐（0/16/32/48），
 * bank = window_start/16，window_len = 1..16（窗口可窄于一个 bank）。
 * 默认 {0, 0, APP_STATE_STEPS}。由 CMD_SET_WINDOW 写、step_window() 读。 */
typedef struct {
    uint8_t bank, window_start, window_len;
} step_window_t;

/* 窗口列 → 引擎绝对步号。STEP 页渲染、TAP 命中与输入层派发共用本函数，
 * 不得另算一份（窗口列与绝对步号的映射只此一处）。 */
static inline uint8_t step_window_abs(const step_window_t *win, uint8_t col)
{
    return (uint8_t)(win->window_start + col);
}

typedef enum {
    CMD_NOTE_ON, CMD_NOTE_OFF, CMD_PLAY, CMD_STOP, CMD_SELECT_TRACK,
    CMD_TOGGLE_STEP, CMD_SET_BPM, CMD_SET_TRACK, CMD_MUTE, CMD_SOLO,
    CMD_ERASE_NOTE, CMD_SET_WINDOW, CMD_SET_PARAM, CMD_SET_PARAM_BATCH,
    CMD_SET_GLOBAL,
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
 *  CMD_SET_WINDOW    a=bank（冗余，由 b 派生） b=start v=len：写步窗（UI 局部态）并
 *                    立即重取窗口列投影。start 向下对齐到 16 的倍数、钳位到 0..48；
 *                    bank = start/16；len 钳位到 1..APP_STATE_STEPS；focus_step 被夹到
 *                    窗口内（隐藏列不可聚焦）。引擎不感知本命令（引擎步号恒为绝对）。
 *  CMD_SET_PARAM     a=track(0..3) b=id(0..P_COUNT-1) v=值。值按 16 位补码解读
 *                    （双极参数如 P_PAN 用 (uint16_t)(int16_t)-1 表达 -1）。经批量
 *                    写入口以 N=1 下发，钳位由引擎按描述符做；不可描述的 id 由引擎
 *                    逐条忽略。投影 p[]/g[] 随即回读引擎真值（钳位后的值）。
 *  CMD_SET_PARAM_BATCH a=track(0..3) p=sloop_param_pair_t 数组 n=元素数：一次
 *                    sloop_core_set_params 下发（真批量，参数页/预置加载用）；
 *                    n > APP_STATE_PARAM_BATCH_MAX 时只应用前 MAX 条（设备侧告警）。
 *                    数组只在 state_apply 返回前有效（同步 apply 语义）。
 *  CMD_SET_GLOBAL    a=id(0..SLOOP_CORE_G_COUNT-1) v=值：全局参数（song.g[]），经
 *                    sloop_core_set_global 按 GP[id] 钳位后写引擎；v 按 16 位补码
 *                    解读（双极全局如 G_FILT 用 (uint16_t)(int16_t)x）。写后立即回读
 *                    g[]（投影 = 钳位后的引擎真值）；id == G_BPM 时同拍更新 core.bpm
 *                    （与 CMD_SET_BPM 同一真值，TRANSPORT/STATE SNAP 不落后一拍）。
 *                    只用于**值参数**——动作类全局（G_LOAD/G_SAVE/G_CLRSEQ…）不得
 *                    经本命令下发（那只会改 song.g[]，不执行动作）。
 */
typedef struct {
    edit_cmd_type_t type;
    uint32_t a, b;
    uint16_t v;
    const void *p; /* CMD_SET_PARAM_BATCH：sloop_param_pair_t 数组（仅 apply 期间有效） */
    uint32_t n;    /* CMD_SET_PARAM_BATCH：元素个数 */
} edit_cmd_t;

typedef struct {
    sloop_core_snapshot_t core; /* 引擎投影：step[k][c]/dstep[c] 为**步窗列**投影
                                 * （= 引擎步 window_start+c；见 step_window_t） */
    step_window_t window;       /* 步窗（UI 局部态；默认 {0,0,16}） */
    int16_t p[SLOOP_CORE_P_COUNT]; /* 选中轨参数投影（引擎真值，sync/参数命令回读） */
    int16_t g[SLOOP_CORE_G_COUNT]; /* 全局参数投影（上一次 sync 的 song.g[]） */
    uint8_t page;               /* 当前页（app_page_t） */
    uint8_t focus_step;         /* STEP 页焦点列（0..window_len-1） */
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
    /* 参数页（sloop_ui 的策展布局表注册；id 与页签顺序一致） */
    APP_PAGE_GLOBAL, APP_PAGE_FX, APP_PAGE_ANALOG,
} app_page_t;
_Static_assert(APP_PAGE_ANALOG + 1 == APP_STATE_PAGES,
               "app_page_t 与 APP_STATE_PAGES 不同步（加页须同时更新两者）");

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
/* 互斥复制步窗（当前 bank/窗口起点/列数）。 */
void step_window(step_window_t *out);

/* UI 局部态写入口（UI 任务的写路径：无 EditCmd 语义，不进引擎/远端）。
 * state_apply 是 input 任务的编辑写路径；两者都走内部互斥。 */
void state_set_page(uint8_t page);
/* 写焦点：track < 4；step 是**窗口列**，须 < window_len（窗口外的隐藏列被忽略，
 * 焦点不会落在不可见的列上）。窗口缩小后旧焦点由 CMD_SET_WINDOW 夹到窗口内。 */
void state_set_focus(uint8_t track, uint8_t step);
void state_set_layer(uint8_t mask);

#endif
