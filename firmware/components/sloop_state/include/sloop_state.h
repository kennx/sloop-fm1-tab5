#ifndef SLOOP_STATE_H
#define SLOOP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "sloop_core.h"
#include "sloop_seq.h"

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

/* 页数：4 基础页（PLAY/TRACKS/STEP/TRANSPORT）+ 5 参数页（GLOBAL/FX/ANALOG/FM6/
 * SAMPLE，由 sloop_ui 的策展布局表加入，见 ADR-008）+ 2 FILE 页（PROJECTS/PRESETS，
 * P4-B Task 10 扩 id、Task 12 注册页面并接 TAP/键盘）+ SONG 编排页（P4-C Task 11，
 * 独立顶层组）+ 2 SYNTH 页（DSYN/FM6P，P4-D Task 8 扩 id；SAMPLE 参数页随组表移入
 * SYNTH 组）+ SYSTEM 设置页（P4-E Task 6，独立 SYS 组）。新页 id 一律排在最后以保持既有页 id 不变；加页时同步 app_page_t、
 * 本宏与 console 的页名表（后者有静态断言兜底）。 */
#define APP_STATE_PAGES 15u
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
    /* P4-B 存储命令（单写者 = input 任务；返回码映射 app_state_t.store_status） */
    CMD_PROJECT_SAVE, CMD_PROJECT_LOAD, CMD_PROJECT_ERASE,
    CMD_PRESET_STORE, CMD_PRESET_LOAD, CMD_PRESET_ERASE,
    CMD_BANK_PUT, CMD_BANK_ERASE,
    CMD_STEP_EDIT, CMD_PATTERN, CMD_SET_MICRO, CMD_SET_FILL,
    CMD_LOCK_SET, CMD_LOCK_DEL, CMD_DRUM_STEP_WRITE,
    /* P4-C 录音/鼓演奏（Task 6）：REC 状态机、fill 表演控制、16 lane 鼓输入、
     * 物理 PLAY 键（drop 自由录音；与 API PLAY 的 close 分开） */
    CMD_REC_TOGGLE, CMD_REC_CONFIG, CMD_FILL_CONTROL, CMD_DRUM_HIT, CMD_PLAY_BUTTON,
    /* P4-C arranger（Task 8）：live sections、quick chain、SONG 模式/REC/顺序 */
    CMD_SECTION_STORE, CMD_SECTION_LOAD, CMD_SECTION_ERASE, CMD_SECTION_REQUEST,
    CMD_CHAIN_BEGIN, CMD_CHAIN_COMMIT, CMD_CHAIN_CANCEL,
    CMD_SONG_MODE, CMD_SONG_REC, CMD_SONG_SET,
    /* P4-D DSYN / FM6 patch / 采样槽（Task 6）：单字段编辑、工厂 kit、DSU 落盘、
     * 试听、采样擦除。返回码映射 app_state_t.synth_status。 */
    CMD_DSYN_SET, CMD_DSYN_LOAD_KIT, CMD_DSYN_STORE, CMD_DSYN_AUDITION,
    CMD_FM6_SET, CMD_SAMPLE_ERASE,
    /* P4-D Task 9 fix：kit 级 CRUSH（KIT 页可编辑；追加在末尾以保住既有命令编号） */
    CMD_DSYN_CRUSH,
    /* P4-E Task 4：settings 单字段写与平台设置（spec §7；同样追加在末尾）。返回码映射
     * app_state_t.settings_status。 */
    CMD_SETTINGS_SET, CMD_SYS_BRIGHT, CMD_SYS_VOLUME, CMD_SYS_OUTPUT,
    /* P4-E Task 7：SYSTEM 页自检请求（只排队，绝不在调用上下文跑自检） */
    CMD_SELFTEST_RUN,
} edit_cmd_type_t;

/* 字段约定（未列出的字段忽略；越界参数不调引擎；音序命令另写 SEQ_STATUS_INVALID）：
 *  CMD_NOTE_ON/OFF   a=track(0..3) v=note(0..127)；NOTE_ON 同时更新 last_note
 *  CMD_PLAY/STOP     （STOP = play(false)）
 *  CMD_SELECT_TRACK  a=track(0..3)：选中轨（与 CMD_SET_TRACK 等价）；副作用：同时把
 *                    focus_track 设为该轨（TRACKS 卡片点击依赖它落地「设焦点轨 + 选轨」）
 *  CMD_SET_TRACK     a=track(0..3)：选中轨（控制方裁定：SET_TRACK 的语义即选轨）
 *  CMD_TOGGLE_STEP   a=track(0..2) b=step(0..63)：空 → 开：清空残留字段
 *                    （note[]/lvl/rat/flags）后写 last_note（无则 60）、
 *                    n=1/vel=100/time=NOTE，保留 micro/fill；开→关：分发完整
 *                    SEQ_STEP_CLEAR，由上游处理 CONT/TIE 并清 lock/micro/fill。
 *                    「空/开」判定见 state_step_is_on()：n>0 且 time==NOTE；
 *                    n>0 但 time 为 TIE/REST 的步算空（切换 → 开，不是 → 关）。
 *                    鼓轨（track 3）由 CMD_DRUM_STEP_WRITE / STEP_EDIT(CLEAR) 编辑。
 *  CMD_SET_BPM       v 钳位到 40..240（上游 params.c 的 GP[G_BPM]）后写 G_BPM
 *  CMD_MUTE/CMD_SOLO a=track(0..3)：翻转该轨 mute/solo 位（solo 以整轨掩码写引擎）
 *  CMD_ERASE_NOTE    a=track(0..2) b=step(0..63) v=note(1..127)：从**显式指定的步**
 *                    找到该音高的 index，分发完整 SEQ_STEP_NOTE_DEL（状态层不读
 *                    focus_step、不自行处理 CONT/TIE）；无匹配 = SEQ_STATUS_NOT_FOUND，
 *                    最后一个音符删除后的清步语义由上游动作保证。鼓轨拒绝。
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
 *  CMD_STEP_EDIT     p=sloop_step_edit_t：完整 POD（含 op）同步透传
 *                    sloop_seq_step_edit；指针只在 state_apply 返回前有效。
 *  CMD_PATTERN       a=track b=op v=(uint16_t)arg：arg 还原为 int16 后交给
 *                    sloop_seq_pattern；工具算法与钳位不在状态层复制。
 *  CMD_SET_MICRO/FILL a=track b=step v=value：分别分发 sloop_seq_set_micro/fill。
 *  CMD_LOCK_SET/DEL  a=track b=step n=param v=value（DEL 忽略 value；param=P_COUNT
 *                    清该步全部锁）：分发 sloop_seq_lock_set/del。
 *  CMD_DRUM_STEP_WRITE b=step p=sloop_dstep_t：同步透传原始 16-lane 鼓步，不触碰
 *                    合成轨步表。上述命令先拒绝非法轨/步/param（不调引擎），返回码写入
 *                    seq_status；仅 ESP_OK 时同拍重取详细窗口投影。
 *  CMD_REC_TOGGLE    （无参）：分发 sloop_seq_rec_toggle；成功后同拍重取录音快照
 *                    （armed/recording/off 立即可断言，不等下一拍 sync）。
 *  CMD_REC_CONFIG    a=quant_off b=tempo_mode v=count_mode（各 0/1）：运行期配置；
 *                    越界不调引擎，seq_status = invalid。**成功后置 settings dirty**
 *                    （P4-E：REC 三项随 settings 落盘，上游语义；见下方 P4-E 段）。
 *  CMD_FILL_CONTROL  a=held(0/1) b=next_bar(0/1)：b=0 只写 FILL 保持电平（`-` 的
 *                    down/up）；b=1 是 NEXT BAR 的**脉冲**，只翻转 `fill_arm`、不读
 *                    不写 held（按住 FILL 时点 NEXT 不会清保持）。越界不调引擎。
 *  CMD_DRUM_HIT      a=lane(0..15) v=velocity(1..127 击打；**0 = 松开**，结束自由录音
 *                    的鼓音符段)：分发 sloop_seq_drum_hit（上游 drum_input/rec_hit 同一
 *                    路径）。选中鼓轨时**不回退合成轨 0**——本命令从不调 note 入口。
 *                    成功不改 seq_status（演奏不是一次编辑）；越界 = invalid 且不调引擎。
 *  CMD_PLAY_BUTTON   a=1 按下 / a=0 松开：物理 PLAY 键（上游面板按钮口径）。自由录音
 *                    中按下 = **drop**；其余情况请求普通走带。与 CMD_PLAY（API PLAY，
 *                    经过 sloop_core_play）分属两个入口：API PLAY 对自由录音是 close。
 *                    投影的 playing 不在此改写（下一拍 sync 取引擎真值）。
 *  CMD_SECTION_STORE/LOAD/ERASE/REQUEST a=slot(0..3)：分发 sloop_seq_section（STORE、
 *                    LOAD/ERASE）与 sloop_seq_section_request。STORE 播放中也允许
 *                    （RAM 捕获 + dirty，flash 由 1s 任务门限写）；LOAD/ERASE 仅停止态
 *                    （走带中 adapter 回 NOT_ALLOWED → seq_status=BLOCKED）；REQUEST 只在
 *                    live loop 播放中排队、下一小节生效。槽号越界不调 adapter。
 *                    成功后同拍重读 song 快照；LOAD/ERASE 还整份刷新引擎投影（会改轨）。
 *  CMD_CHAIN_BEGIN a=first(0..3)：sloop_seq_chain_begin（立即排队 live_req 并清旧 chain）。
 *  CMD_CHAIN_COMMIT p=uint8_t 数组 n=1..8：一次原子安装（任一 scene 空/非法则整份拒绝）。
 *  CMD_CHAIN_CANCEL = COMMIT count 0：清 chain、保留已排队的 section（单 tap 语义）。
 *  CMD_SONG_MODE a=0|1：SONG/LOOP 模式；SONG REC 中 adapter 回 NOT_ALLOWED（"REC IS ON"）。
 *  CMD_SONG_REC：SONG REC toggle（从下一小节录；再按 = 收尾并按门限落盘）。
 *  CMD_SONG_SET p=sloop_song_config_t：透传 sloop_store_song_set（只改内存 + dirty，
 *                   落盘由同一 1s 任务完成）；指针只在 state_apply 返回前有效。
 *  上述命令的返回码写 seq_status：NOT_ALLOWED → BLOCKED（走带中/SONG 播放中/REC IS ON）、
 *  INVALID_STATE 且 seq_frozen → FROZEN、其余 INVALID_STATE（存储未就绪）→ ERROR；
 *  成功同拍刷新 app_state_t.song（UI 不读上游全局）。
 *  CMD_PROJECT_SAVE/LOAD/ERASE a=slot(0..3)：分发 sloop_store_project_*。槽号越界不调
 *                    存储 API、不改 store_status。否则返回码按 app_store_status_t 映射
 *                    写 store_status；**成功（ESP_OK）后整份刷新投影**（一次窗口快照 +
 *                    一次参数回读，与 state_sync_from_engine 同口径）——工程载入会改
 *                    引擎真值（含选中轨/参数/图案），失败则投影不动。
 *  CMD_PRESET_STORE/LOAD/ERASE a=slot(0..31) b=track(0..3，仅 LOAD 使用；显式给出，
 *                    不从选中轨推断)：STORE 的目标轨是**选中轨**（上游 SAVE >
 *                    USER 语义）；LOAD 的 b 是载入目标合成轨；ERASE 忽略 b。
 *                    ESP_ERR_INVALID_ARG 且目标轨是鼓轨 ⇒ store_status = drum(5)，
 *                    其它无效参数 ⇒ error(4)。成功同工程命令一样整份刷新投影。
 *  CMD_BANK_PUT/ERASE a=slot(0..26)：PUT = 选中轨的 live patch（sloop_store_bank_current
 *                    → sloop_store_bank_put）；选中鼓轨没有 patch ⇒ drum(5)，且在
 *                    **状态层**就拒绝——该路径连 bank_current 都不调，存储未就绪时也
 *                    先回 drum（与预置命令的次序相反，见下）。ERASE 只按槽号擦
 *                    （与选中轨无关）。
 *  存储命令在**状态锁内**直接调存储层（单写者串行化，flash 擦写期间不持引擎锁——
 *  存储层保证，ADR-009 §3）；UI 读副本最多等一次擦写（spec §4.3 接受 ≤150ms 停顿）。
 *
 *  次序（drum vs 未就绪）：预置命令只做槽号/轨号**越界**判定，鼓轨判定依赖存储层的
 *  ESP_ERR_INVALID_ARG ——而 sloop_store.h 的预置入口先判就绪，故「存储未就绪 + 鼓轨」
 *  回 INVALID_STATE（3 = playing）而非 drum；bank PUT 的鼓轨判定在状态层先做，未就绪
 *  时回 drum(5)。两者都是「先发生的检查说话」——调用方不应假设某个固定次序。
 *
 *  P4-D DSYN / FM6 patch / 采样槽（Task 6，spec §4.3）：
 *  CMD_DSYN_SET      a=kit(0..3) b=lane(0..15) n=field(0..23) v=value：单字段写
 *                    （adapter 按字段范围钳位）；越界**不调引擎**、synth_status=INVALID。
 *                    成功同拍按当前选择重取 dsyn 投影（app_state_t.dsyn）——投影口径与
 *                    state_sync_from_engine 相同，UI 正在编辑的选择才是屏幕显示的那个。
 *  CMD_DSYN_LOAD_KIT a=kit(0..3) b=src(工厂 kit 索引)：套用工厂 kit（覆盖该 kit 的编辑，
 *                    置 dsu_dirty）；kit 越界不调 core、src 越界由 core 回 INVALID_ARG。
 *  CMD_DSYN_STORE    （无参）：sloop_store_dsu_store()（把当前 RAM 的 DSU 与歌曲/side 一起
 *                    写进 settings）。走带中 → BLOCKED、诊断冻结 → FROZEN、存储未就绪 →
 *                    ERROR；成功同拍重取 dsyn（dirty 归 0）。
 *  CMD_DSYN_AUDITION a=kit(0..3) b=lane(0..15) v=velocity(1..127)：试听（写上游队列，
 *                    **不改数据、不刷新投影**）；velocity 0/>127 与越界 kit/lane 不调 core。
 *  CMD_DSYN_CRUSH    a=kit(0..3) v=crush(0..255)：kit 级 bit-crush / sample-and-hold
 *                    （上游 `dkit_t.crush`）。越界 kit 或 v>255 不调 core（INVALID）、
 *                    冻结 → FROZEN；成功同拍重取 dsyn 投影（crush 与 dirty）。
 *  CMD_FM6_SET       a=track(0..3) b=op(0..6，6=全局) n=field v=value：单字段写；
 *                    非 FM6 轨（含鼓轨）→ LOCKED、越界 op/field → INVALID、冻结 → FROZEN；
 *                    成功同拍重取 fm6p 投影（app_state_t.fm6p）。core 对全部拒绝只回
 *                    SLOOP_FM6_INVALID，故命中哨兵时用一次 sloop_core_fm6_patch 复核
 *                    （INVALID_STATE → FROZEN；INVALID_ARG/NOT_ALLOWED/engine_ok=0 →
 *                    LOCKED；其余 → INVALID）。
 *  CMD_SAMPLE_ERASE  a=slot(0..3)：sloop_store_sample_erase；槽号越界不调存储（INVALID）、
 *                    走带中 → BLOCKED、冻结 → FROZEN、未就绪 → ERROR。
 *  上述命令的返回码写 synth_status（app_synth_status_t）：越界/INVALID_ARG → INVALID、
 *  NOT_ALLOWED → BLOCKED、INVALID_STATE 按 seq_frozen/存储就绪分成 FROZEN/BLOCKED/ERROR、
 *  其它 → ERROR。存储 API 用 INVALID_STATE 同时表示走带/冻结/未就绪，故必须借
 *  sloop_seq_frozen() 与 sloop_store_ready() 才能把三种状态行文案分开。
 *
 *  P4-E Task 4：设置与平台（spec §7）：
 *  CMD_SETTINGS_SET  a=sloop_set_field_t v=值：值域**在状态层**钳位（palette 0..4、
 *                    lowcut/zoom/theme_on 0|1、lights 0..3、reso 0..10）后下发
 *                    sloop_store_settings_set（只改内存 + 应用上游 static + 置 settings
 *                    dirty；落盘由 1s 任务的静默窗口完成，store/HAL 内部再钳一次）。
 *                    字段越界 ⇒ 不调存储、settings_status = INVALID。成功同拍刷 settings
 *                    投影（palette/theme 由 UI 据此重着色；theme_name 随同一投影刷新，
 *                    供 SYSTEM 页的 COLOR 第 6 档显示）。
 *  CMD_SYS_BRIGHT / CMD_SYS_VOLUME / CMD_SYS_OUTPUT v=值：值域在状态层钳位（bright
 *                    10..100、vol 0..100、out 0 speaker|1 headphone）后下发 HAL——平台
 *                    副作用即时生效、NVS 延迟提交（1s tick 的同一静默窗口），不取引擎锁、
 *                    不受走带限制（播放中也可改）。out > 1 ⇒ INVALID 且不入 HAL。
 *                    成功同拍刷 sys 投影（nvs_dirty 取 HAL）。
 *  上述命令的返回码写 settings_status（app_settings_status_t）：ESP_OK → OK、
 *  INVALID_ARG / NOT_SUPPORTED（headphone，本板 speaker-only）→ INVALID、
 *  INVALID_STATE（诊断冻结 / 存储未就绪）→ BLOCKED、其它 → ERROR；失败不改投影。
 */
typedef struct {
    edit_cmd_type_t type;
    uint32_t a, b;
    uint16_t v;
    const void *p; /* PARAM_BATCH=pair 数组；STEP_EDIT/DRUM_STEP_WRITE=POD（仅 apply 期间有效） */
    uint32_t n;    /* CMD_SET_PARAM_BATCH：元素个数；CMD_LOCK_SET/DEL：param */
} edit_cmd_t;

/* 存储命令的结果（app_state_t.store_status）：UI 状态行按此映射文案
 * （`STOP BEFORE SAVE` / `DRUM TRACK: NO SOUND` …）。0 表示还没有存储命令；
 * 槽号/轨号越界被拒时**不调存储 API、也不改本字段**（命令无效 ≠ 存储失败）。
 * 语义边界：存储 API 用 ESP_ERR_INVALID_STATE **同时**表示「走带中」与「存储未就绪」
 * （分区/mmap/PSRAM 缺失，见 sloop_store.h），因此 3 不是「正在播放」的充分证据；
 * 需要区分 `NO STORAGE` 的调用方必须另查 sloop_store_ready()。 */
typedef enum {
    APP_STORE_STATUS_IDLE = 0,    /* 尚无存储命令 */
    APP_STORE_STATUS_OK = 1,      /* save/load/erase 成功 */
    APP_STORE_STATUS_EMPTY = 2,   /* 槽为空（ESP_ERR_NOT_FOUND） */
    APP_STORE_STATUS_PLAYING = 3, /* 走带中**或存储未就绪**（ESP_ERR_INVALID_STATE） */
    APP_STORE_STATUS_ERROR = 4,   /* 其它失败（擦写/校验/无效格式） */
    APP_STORE_STATUS_DRUM = 5,    /* 鼓轨没有可存/可载的声音（ESP_ERR_INVALID_ARG；
                                   * 或 bank PUT 在状态层直接判鼓轨、不调存储 API） */
} app_store_status_t;

/* 最近一次音序编辑结果：blocked（CONT/目标占用）、full（锁/复制容量）与 frozen
 * （诊断冻结）必须分开；INVALID_STATE 在 sloop_seq 公共 API 中只表示 frozen。 */
typedef enum {
    SEQ_STATUS_IDLE = 0,       /* 尚无音序命令 */
    SEQ_STATUS_OK = 1,         /* 成功且详细窗口已同拍刷新 */
    SEQ_STATUS_NOT_FOUND = 2,  /* 空步/无匹配音/无 undo/空 section/无此 scene */
    SEQ_STATUS_INVALID = 3,    /* 非法轨、步、op、参数或参数值 */
    SEQ_STATUS_FULL = 4,       /* ESP_ERR_NO_MEM */
    SEQ_STATUS_BLOCKED = 5,    /* ESP_ERR_NOT_ALLOWED */
    SEQ_STATUS_FROZEN = 6,     /* ESP_ERR_INVALID_STATE */
    SEQ_STATUS_ERROR = 7,      /* 其它失败 */
} app_seq_status_t;

/* 最近一次 DSYN / FM6 patch / 采样槽命令的结果（spec §4.3）。存储 API 的 INVALID_STATE
 * 同时表示走带/冻结/未就绪三因，状态层借 sloop_seq_frozen() 与 sloop_store_ready()
 * 拆成 BLOCKED / FROZEN / ERROR；LOCKED = 选中轨不是 FM6（含鼓轨），与「值非法」分开。 */
typedef enum {
    APP_SYNTH_STATUS_IDLE = 0,    /* 尚无合成编辑命令 */
    APP_SYNTH_STATUS_OK = 1,      /* 成功（投影已同拍刷新） */
    APP_SYNTH_STATUS_INVALID = 2, /* 越界 kit/lane/field/op/slot/velocity、格式非法 */
    APP_SYNTH_STATUS_FULL = 3,    /* ESP_ERR_NO_MEM（本阶段无来源，保留口径） */
    APP_SYNTH_STATUS_BLOCKED = 4, /* 走带中（ESP_ERR_NOT_ALLOWED / 存储层 INVALID_STATE） */
    APP_SYNTH_STATUS_FROZEN = 5,  /* 诊断冻结（ENG FREEZE / SELFTEST） */
    APP_SYNTH_STATUS_LOCKED = 6,  /* 选中轨不是 FM6（含鼓轨），没有可编辑的 live patch */
    APP_SYNTH_STATUS_ERROR = 7,   /* 存储未就绪、擦写/校验失败等 */
} app_synth_status_t;

/* 最近一次设置 / 平台命令的结果（app_state_t.settings_status；P4-E spec §7）。
 * INVALID 兼表「值非法」「字段越界」与 headphone 的 NOT_SUPPORTED（本板输出路由
 * speaker-only，R19 不另设枚举值）；BLOCKED = 诊断冻结或存储未就绪（存储 API 的
 * INVALID_STATE 两义，本阶段不拆 FROZEN）。 */
typedef enum {
    APP_SETTINGS_IDLE = 0,    /* 尚无设置/平台命令 */
    APP_SETTINGS_OK = 1,      /* 成功（投影已同拍刷新） */
    APP_SETTINGS_INVALID = 2, /* 字段/值越界、输出路由不支持（NOT_SUPPORTED） */
    APP_SETTINGS_BLOCKED = 3, /* 诊断冻结或存储未就绪（ESP_ERR_INVALID_STATE） */
    APP_SETTINGS_ERROR = 4,   /* 其它失败（平台调用失败等） */
} app_settings_status_t;

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
    uint8_t store_status;       /* 最近一次存储命令的结果（app_store_status_t） */
    uint8_t seq_status;         /* 最近一次音序命令的结果（app_seq_status_t） */
    sloop_rec_snapshot_t rec;   /* 录音状态投影（sloop_seq_rec_read；一次性 take 结果
                                 * 在 sync/命令重取时被消费，见 sloop_seq.h） */
    sloop_song_snapshot_t song; /* arranger 只读投影（enabled/running/index/bar/ready/live/
                                 * dirty/chain/srec + 16 项顺序）：SONG 页与 TRANSPORT 的
                                 * 状态行都读这里，不直接碰上游全局（Task 8） */
    uint8_t synth_status;       /* 最近一次合成编辑命令的结果（app_synth_status_t） */
    sloop_dsyn_snapshot_t dsyn; /* DSYN 投影（按 kit/lane 选择；dirty = DSU 记账） */
    sloop_fm6_patch_snapshot_t fm6p; /* FM6 live patch 投影（选中轨；engine_ok=0 即锁定） */
    /* P4-E Task 4：settings 生效副本投影（sloop_store_settings()；dirty = 待落盘）与平台
     * 设置投影（HAL 的 RAM 三值 + nvs_dirty）。init / sync / 整份刷新各读一次，对应命令
     * 成功时同拍重取；UI 不直接读 store/HAL。 */
    struct {
        uint8_t palette, lowcut, zoom, lights, theme_on, theme_valid, reso, dirty;
        char theme_name[9];
    } settings;
    struct {
        uint8_t bright, vol, out, nvs_dirty; /* bright 10..100 / vol 0..100 / out 0|1 */
    } sys;
    app_settings_status_t settings_status;   /* 最近一次设置/平台命令的结果 */
    /* P4-E Task 7：SELFTEST chip 的请求与最近一次结果。req 是深度 1 的邮箱，由
     * CMD_SELFTEST_RUN 置位、main 的一次性 worker 用 state_take_selftest_request()
     * 消费；done=0 表示尚无结果或正在运行。ran/failed 是 core 运行器的实际计数，
     * 不假定全量用例数；ran=0 表示没有可用结果。 */
    uint8_t selftest_req, selftest_done;
    uint8_t selftest_unavailable; /* main 的 worker 创建失败：请求立即 FAIL，不排队 */
    uint32_t selftest_ran, selftest_failed;
} app_state_t;

/* layer_held 的位义（ADR-006 层键）：InputManager 维护，UI 显示提示用。 */
#define APP_LAYER_FX   0x01u /* Sym */
#define APP_LAYER_EDIT 0x02u /* Aa：擦除聚焦步音符 */
#define APP_LAYER_SEL  0x04u /* Ctrl：1–4 选轨 */
#define APP_LAYER_GLO  0x08u /* Alt：1–4 静音 / 5–8 独奏 */

typedef enum {
    APP_PAGE_PLAY, APP_PAGE_TRACKS, APP_PAGE_STEP, APP_PAGE_TRANSPORT,
    /* 参数页（sloop_ui 的策展布局表注册；id 与页签顺序一致） */
    APP_PAGE_GLOBAL, APP_PAGE_FX, APP_PAGE_ANALOG, APP_PAGE_FM6, APP_PAGE_SAMPLE,
    /* FILE 组（P4-B：Task 10 扩 id、Task 11 组模型、Task 12 注册页面与页签） */
    APP_PAGE_PROJECTS, APP_PAGE_PRESETS,
    /* SONG 编排页（P4-C Task 11：独立顶层组 `SONG`，页签顺序在 SEQ 与 FILE 之间；
     * id 排在最后，既有页 id 不变） */
    APP_PAGE_SONG,
    /* SYNTH 组（P4-D Task 8：SAMPLE 参数页移入该组；DSYN/FM6P 是 P4-D 的合成编辑页，
     * Task 9/10 填实体，本任务先注册占位页） */
    APP_PAGE_DSYN, APP_PAGE_FM6P,
    /* SYS 组（P4-E Task 6）：SYSTEM 设置页，id 追加在最后。 */
    APP_PAGE_SYSTEM,
} app_page_t;
_Static_assert(APP_PAGE_SYSTEM + 1 == APP_STATE_PAGES,
               "app_page_t 与 APP_STATE_PAGES 不同步（加页须同时更新两者）");
_Static_assert(APP_PAGE_SAMPLE - APP_PAGE_GLOBAL + 1 == 5u,
               "参数页 id 段必须连续（page_layout.c 的 PARAM_PAGE_COUNT 依赖它）");

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
/* 写 DSYN 的 kit/lane 选择（UI 局部态；console `UI DSYN KIT/LANE` 与页面 chip 共用）：
 * 越界钳到 0..3 / 0..15，并在同一把状态锁内按新选择重取 dsyn 投影（22B + name/crush/
 * src/dirty）——TAP 之后的 STATE SNAP/旋钮立即看到新选择，不等下一拍 sync。 */
void state_set_dsyn_select(uint8_t kit, uint8_t lane);
void state_set_layer(uint8_t mask);

/* 取走一次 SELFTEST 请求（1 = 有请求，取后清零）；worker 完成后写回实际计数并置 done。 */
bool state_take_selftest_request(void);
void state_set_selftest_result(uint32_t ran, uint32_t failed);
/* main 在 worker 创建后上报可用性；false 时 CMD_SELFTEST_RUN 立即写 0/0 FAIL。 */
void state_set_selftest_available(bool available);

#endif
