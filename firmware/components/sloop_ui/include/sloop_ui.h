/*
 * sloop-fm1-tab5 — sloop_ui 公共接口（LVGL 页面框架：分组页签 + 页面注册表）
 *
 * 单容器屏 + 顶部两行页签（ADR-005「导航决策」）：上行 9 个组签
 * PLAY|TRACKS|EDIT|MIX|SYNTH|SEQ|SONG|FILE|SYS（无已注册页的组不显示），下行显示当前组的
 * 已注册子页；组内顺序 = 子页顺序，`TAB` 按组顺序扁平轮转
 * （PLAY→TRACKS→SOUND→MOD→SCALE→VOICE→GLOBAL→FX→SAMPLE→DSYN→FM6P→STEP→TRANSPORT→
 * SONG→PROJECTS→PRESETS→SYSTEM→PLAY；P4-F Task 5 起 17 页，Task 6 加 TOOLS 后 GLOBAL→FX→
 * TOOLS→SAMPLE…共 18 页）。
 * 页面**首次访问时构建**，离开组时销毁该组全部已建页（组作用域生命周期：跨组
 * 切换峰值 = 源组已建 Σ + 目标页 est，见 ADR-005 §3/§5）；PLAY 在
 * sloop_ui_start() 内立即构建，保证开机首帧非空白。SYSTEM 为最后的 SYS 单页组。
 * 池增量与构建耗时记入
 * 日志/`UI MEM`（按页与按组两种口径）。
 * 构建失败（控件分配失败/预算不足）显示状态条错误文本 + ERROR 日志，
 * 同步切页返回 ESP_ERR_NO_MEM（console 打印 `UI ERR page_build <id>`）。
 *
 * 线程：所有 LVGL 调用在 bsp_display_lock 内（esp_lvgl_port 递归锁，与
 * display.c 同一纪律）；UI 定时器 30ms 运行在 LVGL 任务里（已持锁）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sloop_state.h"
#include "sloop_store.h"

/* 组数（与 ui_nav.h 的组表 UI_GROUP_LIST 静态比对）：PLAY / TRACKS / EDIT / MIX / SYNTH /
 * SEQ / SONG / FILE / SYS（ADR-005；P4-F Task 4 起 9 组——MIX 承接 GLOBAL/FX，EDIT 留给每轨族） */
#define SLOOP_UI_GROUPS 9

/* 页面注册表条目（ADR-008）。注册顺序 = 子签顺序（组内），组顺序见
 * sloop_ui_page_order_next()。id 为 app_page_t（< APP_STATE_PAGES）。
 * build：在 parent 内建本页控件，成功返回 parent，任一控件分配失败返回 NULL
 *        （页内用 theme.h 的 UI_BUILD_CHECK 逐个检查）。
 * update：只对该页已建控件做脏检查刷新（未建页不会被调用）。
 * est_bytes：构建前预算门槛——LVGL 池剩余低于该值直接拒绝构建（失败可见，不冒险
 *        半途分配失败）。数值来源见 `docs/design/008-param-pages.md` 的实测表。 */
typedef struct {
    uint8_t id;
    const char *title;
    lv_obj_t *(*build)(lv_obj_t *parent);
    void (*update)(const app_state_t *st);
    uint32_t est_bytes;
} ui_page_t;

/* 注册一页（须在 sloop_ui_start() 之前调用；sloop_ui_start 内置注册现有 18 页 / 9 组）。
 * 注册表只增不删：先注册的外部页在内置页注册之后仍保留（内置注册幂等）。
 * 注册表只保存指针：ui_page_t 与 title 必须是静态生命周期（不得指向栈/临时内存）。
 * 返回：ESP_OK；ESP_ERR_INVALID_ARG（参数/页 id 非法或重复）；ESP_ERR_NO_MEM
 * （注册表满）；ESP_ERR_INVALID_STATE（UI 已启动）。 */
esp_err_t ui_page_register(const ui_page_t *page);

/* 已注册页数（注册顺序；输入层的 tab 轮转按组顺序扁平轮转，不按本计数）。
 * 注册表只在 start 前写入，读无需加锁；UI 未启动时返回已注册的外部页数（通常 0）。 */
uint8_t sloop_ui_page_count(void);

/* `TAB` 的下一张页（ADR-005 §4）：按组顺序扁平轮转
 * PLAY→TRACKS→SOUND→MOD→SCALE→VOICE→GLOBAL→FX→TOOLS→SAMPLE→DSYN→FM6P→STEP→
 * TRANSPORT→SONG→PROJECTS→PRESETS→SYSTEM→PLAY，**跳过未注册页**（APP_PAGE_FM6 是空位）。page 未注册/非法时返回
 * 第一个已注册页；无任何已注册页返回 0xFF。页序号只依赖注册表（start 前可调用）。 */
uint8_t sloop_ui_page_order_next(uint8_t page);

/* 组签/子签的程序化入口（console `UI TAB <group>` / `UI SUB <index>`，R32）：与物理
 * 页签点击走**同一**动作函数（`group_select_locked`/`sub_select_locked`），因此覆盖
 * 「点组进上次子页 / 点子签切页」的导航路径（TAP 对 y<72 一律 miss，无法覆盖它）。
 * - sloop_ui_group_count()/sloop_ui_group_name(g)：组表（PLAY/TRACKS/EDIT/MIX/SYNTH/SEQ/
 *   SONG/FILE/SYS），名字为静态生命周期；越界 name 返回 NULL。
 * - group_select(group)：进入该组上次的子页（未记录/已未注册则首个已注册成员）；
 *   空组（没有已注册页的组）返回 ESP_ERR_INVALID_ARG。
 * - sub_select(index)：切到**当前组**第 index 张子签（0 起）；单页组（PLAY/TRACKS/
 *   SONG/SYS）子行为空 ⇒ 任何 index 都返回 ESP_ERR_INVALID_ARG（与视觉/命中一致），
 *   index ≥ 5（下行固定 5 个子签槽位，组内最多 5 页）同样。
 * 两者：ESP_ERR_INVALID_STATE（UI 未启动）、ESP_ERR_TIMEOUT（LVGL 锁超时，未派发）、
 * ESP_ERR_NO_MEM（目标页构建失败，页面已切到错误状态条）。切页内部分别经 show_page，
 * 会作废同一 30ms 拍内排队的异步切页（R30）。 */
uint8_t sloop_ui_group_count(void);
const char *sloop_ui_group_name(uint8_t group);
esp_err_t sloop_ui_group_select(uint8_t group);
esp_err_t sloop_ui_sub_select(uint8_t index);

typedef enum {
    SLOOP_UI_PAGE_UNBUILT = 0, /* 尚未进入过（懒建）/ 已随源组销毁 */
    SLOOP_UI_PAGE_BUILT = 1,   /* 已建，控件保留 */
    SLOOP_UI_PAGE_FAILED = 2,  /* 上次构建失败（下次访问重试） */
} sloop_ui_page_state_t;

typedef struct {
    uint8_t id;
    const char *title;
    uint8_t state;         /* sloop_ui_page_state_t */
    uint32_t est_bytes;    /* 预算（注册表） */
    int32_t delta_bytes;   /* 最近一次成功构建的池 free 减少量（含块头；采样于首帧 update
                            * 之前，不含首帧文本分配）；0 = 未建/从未成功 */
    uint32_t build_ms;     /* 最近一次构建耗时（毫秒；0 = 未建） */
} sloop_ui_page_stat_t;

/* 分组水位（ADR-005 §5）：sum_est = 组内**已注册**成员 est 之和（组的静态预算），
 * sum_delta = 组内**已建**成员最近一次构建的池增量之和（当前占用），
 * build_ms = 最近一次「跨组进入本组」或「组内首次构建兄弟页」的耗时
 * （≤100ms 预算的口径，含目标页构建 + 切换 + 源组销毁；0 = 尚未进入）。 */
typedef struct {
    const char *name;
    uint8_t page_count;                 /* 已注册成员数 */
    uint8_t built;                      /* 已建成员数（离开组后为 0） */
    uint32_t sum_est, sum_delta;
    uint32_t build_ms;
} sloop_ui_group_stat_t;

typedef struct {
    uint32_t pool_total, pool_free, pool_used; /* LVGL 池（lv_mem_monitor） */
    uint32_t lvgl_stack_free; /* LVGL 任务栈的历史最小余量（字节；每拍在任务内取高水位，
                               * 含 30ms 拍/切页构建/控件回调/渲染。门禁 ≥2,048，ADR-008 §8.10） */
    uint8_t registered, built, failed;
    sloop_ui_page_stat_t pages[APP_STATE_PAGES]; /* 按注册（页签）顺序 */
    uint8_t group_count;                         /* 组数（当前 9） */
    uint8_t current_group;                       /* 当前页所属组下标；0xFF = 无 */
    sloop_ui_group_stat_t groups[SLOOP_UI_GROUPS];
} sloop_ui_pool_stats_t;

/* 注册表/池水位统计（console `UI MEM`）：内部取 LVGL 锁调 lv_mem_monitor。
 * 返回 ESP_ERR_INVALID_ARG（out=NULL）、ESP_ERR_INVALID_STATE（UI 未启动）、
 * ESP_ERR_TIMEOUT（LVGL 锁超时）。 */
esp_err_t sloop_ui_page_pool_stats(sloop_ui_pool_stats_t *out);

/* 调试注入（console `UI FORCEFAIL <page>`）：强制该页构建失败（验证失败可见性）。
 * 未建页：挂起到下一次构建（一次性，构建尝试后自动解除）。
 * 已建当前页：删除其控件并立即重建，注入当场被消费、页面进入失败态（state=failed）。
 * 已建非当前页：只删除其控件、回到未建态（回显 state=unbuilt），注入保持待命，
 * 下次访问时构建失败。
 * `off` 只撤销尚未消费的注入，不会重建已失败页（失败页在下次访问时重试）。
 * 返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_ERR_TIMEOUT。 */
esp_err_t sloop_ui_force_page_build_failure(uint8_t page, bool on);

/* 创建页签 + 页面注册表并注册 30ms UI 定时器；PLAY 页立即构建（开机非空白），
 * 其余页首次访问时构建（ADR-008）。须在 state_init() 与显示初始化之后调用。
 * 成功时接管控件树（清掉 bring-up 测试图）；框架级失败（根容器/页签/定时器建不出）
 * 返回错误，调用方回退 app_display_draw_test_pattern()。单页构建失败不算框架失败：
 * 错误状态条留在屏上，本函数仍返回 ESP_OK。 */
esp_err_t sloop_ui_start(void);

/* P4-E Task 5：palette/side theme 的运行时应用与 ZOOM 大读数条。主题应用复用组切换
 * 销毁序；ZOOM 是屏幕层懒建的唯一 label，不计入任何页面 est。 */
struct knob;
void sloop_ui_theme_apply(const sloop_settings_t *s);
void sloop_ui_rebuild_all(void);
void sloop_ui_zoom_knob(const struct knob *k);

/* 切页：0=PLAY 1=TRACKS 2=STEP 3=TRANSPORT 4=GLOBAL 5=FX 6=SOUND 7=(空位，未注册) 8=SAMPLE
 * 9=PROJECTS 10=PRESETS 11=SONG 12=DSYN 13=FM6P 14=SYSTEM 15=MOD 16=SCALE 17=VOICE
 * 18=TOOLS（同 app_page_t）；首次进入时构建该页。
 * 组作用域（ADR-005 §3）：目标页与当前页不同组时，顺序固定为「建目标页 →
 * 切到目标页 → 销毁源组全部已建页（含原当前页）」——因此离开组后源组 built=0；
 * 同组内切页只改显隐（已建页不重建）。快照里的组/子签选中态随页更新。
 * 可从任意任务调用（内部加锁）。返回：ESP_OK = 已落地；ESP_ERR_INVALID_ARG =
 * 非法页 id 或该页未注册（console 打印 `UI ERR not_registered <id>`）；
 * ESP_ERR_INVALID_STATE = UI 未启动；ESP_ERR_TIMEOUT = LVGL 锁超时
 * （**未切页**，console `UI` 据此打印 `UI ERR lock_timeout`）；ESP_ERR_NO_MEM =
 * 目标页构建失败（页面已切到错误状态条，console 打印 `UI ERR page_build <id>`）。 */
esp_err_t sloop_ui_switch_page(uint8_t page);

/* 请求切页（异步）：只记目标页，30ms UI 定时器在 LVGL 上下文执行。可从任意任务
 * 调用、不取任何锁；输入层（esc/tab）用它避免在输入路径上等 LVGL 锁。
 * 越界忽略；连续请求保留最后一个。 */
void sloop_ui_request_page(uint8_t page);

/* 当前页；UI 未启动返回 0xFF。 */
uint8_t sloop_ui_current_page(void);

/* 音符触发指示（PLAY 页大号指示）：把音符交给 30ms UI 定时器点亮，120ms 后熄灭。
 * 契约：不取 LVGL/state/引擎锁——可从任意任务调用（输入任务、console 注入）；
 * 只保留最新一个音符（无队列、无阻塞），点亮最多延迟一拍 UI（30ms）。 */
void sloop_ui_flash_note(uint8_t note);

/* InputManager 入口（input.c）：code = tab5_keyboard 事件码（row<<4|col），
 * down = 按下/释放。物理键盘（input 任务按 tick 地板轮询，100Hz 下 10ms）与 console
 * `KEY` 注入共用；
 * 可从任意任务调用（内部互斥 + 状态层互斥），不得在 ISR 中调用。 */
void sloop_ui_input_key(uint8_t code, bool down);

/* 全停（console `KEY PANIC`）：释放输入层跟踪的所有已按下音符（逐条 note-off）、
 * 清 layer_held、撤销待执行切页。可从任意任务调用，不取 LVGL 锁。 */
void sloop_ui_input_panic(void);

/* 撤销待执行切页（清空异步切页邮箱）。console 同步切页（`UI`）与 `KEY PANIC`
 * 共用：避免「读当前页→请求同页」在切页中途读到旧值而漏撤销。不取 LVGL 锁。 */
void sloop_ui_cancel_pending_page(void);

/* TAP 注入（console `TAP <x> <y>`）：坐标 = 屏幕绝对坐标（含页签条，与触摸 indev
 * 同口径）。按当前页 hit-test 表命中矩形，执行与控件回调**相同的动作函数**
 * （EditCmd 层等价，见 ADR-005；不声称原始指针事件等价）。
 * 返回值区分四条路径：OK（命中，动作/参数填进 out，out 可为 NULL）、MISS（矩形
 * 表未命中，不改状态）、LOCK_TIMEOUT（未派发——console 打印
 * `TAP ERR lvgl_lock_timeout`）、QUIESCED（诊断期静默，未派发——console 打印
 * `TAP ERR quiesced`）。脚本不得把后两者与正常 miss 混判。
 * 内部取 LVGL 锁（命中后要改控件文本/立即刷新），仅 console 注入路径调用。 */
typedef enum {
    SLOOP_TAP_MISS = 0,
    SLOOP_TAP_STEP_CELL,     /* a=track b=绝对步号：设焦点（窗口列）+ CMD_TOGGLE_STEP */
    SLOOP_TAP_STEP_DRUM,     /* 鼓轨 lane 位：a=lane(0..15) b=绝对步号 d=写入后的 bit
                              * v=写后回读的 16 lane 位图 err=0/失败状态
                              * （CMD_DRUM_STEP_WRITE；冻结/非法时 v/d 为写前真值） */
    SLOOP_TAP_TRACK_CARD,    /* a=track：CMD_SELECT_TRACK */
    SLOOP_TAP_TRANSPORT_PLAY,
    SLOOP_TAP_TRANSPORT_STOP,
    SLOOP_TAP_TRANSPORT_BPM, /* v=±1：CMD_SET_BPM */
    SLOOP_TAP_TRANSPORT_TOOL, /* TRANSPORT 工具/聚焦步字段/pattern 动作（P4-C Task 10）：
                               * a=sloop_transport_act_t b=LVL/RAT 的落点（0 合成轨
                               * CMD_STEP_EDIT / 1 鼓轨 CMD_DRUM_STEP_WRITE），其余 0
                               * v=**写后回读**值（lvl/rat 原始 2-bit、micro、span 目标、
                               * slide 0/1、record hold 电平、pattern arg、len 结果）d=±1 方向
                               * err=0 成功 / 1..7 = app_seq_status_t / 8 = 不适用
                               * （鼓轨上的 span/slide：**未下发命令**，v 为写前真值） */
    SLOOP_TAP_PARAM_STEP,    /* 参数页 ± ：a=槽位 b=参数绝对 id v=落地后的值 d=±1（console 回显） */
    SLOOP_TAP_PARAM_FOCUS,   /* 参数页弧线/数值文本：a=槽位（只设焦点，不改值） */
    SLOOP_TAP_PARAM_LOCKED,  /* 参数页槽位不可编辑（引擎页不适用/鼓轨逐槽规则）：a=槽位 */
    SLOOP_TAP_PARAM_CHIP,    /* chip 页的 chip 行：a=页 id b=chip 下标（选中即生效） */
    SLOOP_TAP_FILE_SLOT,     /* FILE 页槽位（PROJECTS 卡 A–D / PRESETS 格）：
                              * a=页 id（PROJECTS/PRESETS）b=槽号（0..3 / 0..31） */
    SLOOP_TAP_FILE_ACTION,   /* FILE 页动作按钮：a=页 id b=槽号 v=0 SAVE/1 LOAD/2 ERASE。
                              * 动作后 store_status 的变化再由状态行/console 回显体现 */
    SLOOP_TAP_SOUND_ENGINE,  /* SOUND 页眉 ENGINE 行 ±（P4-F Task 5）：a=**写后回读**的引擎
                              * 下标(0..11) d=±1 err=sound_status（1 OK / 2 INVALID / 3 FROZEN）。
                              * 一条 CMD_SET_ENGINE（引擎默认值 + 预置 0）；名字由
                              * `sloop_ui_sound_status_text` 同源的 console 回显给出。 */
    SLOOP_TAP_SOUND_PRESET,  /* SOUND 页眉 PRESET 行 ±：a=写后回读的预置下标 d=±1
                              * err=sound_status。一条 CMD_SET_PRESET（预置的整份声音）。 */
    SLOOP_TAP_SOUND_KIT,     /* SOUND 页在**鼓轨**上的 KIT 行 ±：a=写后回读的 kit 号
                              * (P_E0) d=±1 err=0 已下发（CMD_SET_PARAM）/ 3 FROZEN。
                              * 鼓轨没有 ENGINE/PRESET 行。 */
    SLOOP_TAP_BANK_SLOT,     /* SOUND 页 bank 目标槽 ±（仅选中轨引擎为 FM6 时行可见，其余
                              * 引擎整行隐藏 ⇒ TAP miss）：a=页 id（SOUND）b=新槽号(0..26)
                              * d=±1。只改目标槽，不写存储/引擎 */
    SLOOP_TAP_BANK_STORE,    /* SOUND 页 BANK SAVE：a=页 id b=目标槽 v=0（CMD_BANK_PUT；
                              * 动作结果由状态行/console 回显体现） */
    SLOOP_TAP_BANK_ERASE,    /* SOUND 页 BANK ERASE：a=页 id b=目标槽 v=2（CMD_BANK_ERASE） */
    SLOOP_TAP_SAMPLE_SLOT,   /* SAMPLE 页槽管理行 U1..U4（P4-D Task 8）：a=页 id b=槽号(0..3)。
                              * 命中即把该槽写成 SET 宏（P_E0 = base + b，base 由 SAMPLE 的
                              * SET 描述符派生）经 CMD_SET_PARAM 下发；锁定页（选中轨引擎
                              * ≠ SAMPLE）不下发：此时 b=0xFF、v=0，console 回
                              * `slot=- select status="…"`（v=1 = 已下发）。 */
    SLOOP_TAP_SAMPLE_ACTION, /* SAMPLE 页 ERASE chip：a=页 id b=槽号(0..3) v=2（CMD_SAMPLE_ERASE；
                              * 当前 SET 不在 USR1..USR4 时页面本地拒绝、不下发命令）。
                              * 动作结果文本由 `sloop_ui_sample_status_text()` 与屏幕同源给出。 */
    SLOOP_TAP_DSYN_SEL,      /* DSYN 页选择（P4-D Task 9）：SYN1..4 / LANE ± / 字段页 chip /
                              * 旋钮（只设焦点）。a=kit(0..3) b=lane(0..15) d=字段页(0..3)
                              * v=1。`UI DSYN KIT/LANE/PAGE` 与这些 chip 共用同一状态。 */
    SLOOP_TAP_DSYN_FIELD,    /* DSYN 页单字段写（± chip；TAP 不能拖拽）：a=字段码（0..23 =
                              * sloop_dsyn_field_t，0x80 CRUSH，0x81 KIT SRC）v=**写后回读**
                              * err=0 成功 / 2 = 命令失败（synth_status != OK）。字段名由
                              * `sloop_ui_dsyn_field_name()` 给出。 */
    SLOOP_TAP_DSYN_STORE,    /* DSYN 页 STORE chip：CMD_DSYN_STORE；结果文本由
                              * `sloop_ui_dsyn_status_text()` 与屏幕同源给出。 */
    SLOOP_TAP_DSYN_AUDITION, /* DSYN 页 AUDITION chip：a=kit b=lane v=velocity（固定 100） */
    SLOOP_TAP_DSYN_KIT,      /* KIT 页 KIT SRC 的 ±：a=kit b=**新的工厂 kit 序号**（0..31）
                              * v=写后回读的 src（CMD_DSYN_LOAD_KIT；套用即覆盖该 kit）。 */
    SLOOP_TAP_FM6P_SEL,      /* FM6P 页选择（P4-D Task 10）：OP1..OP6 / 字段页 chip / 旋钮
                              * （只设焦点）。a=op(0..5) b=字段页(0..5) v=1。
                              * `UI FM6P OP/PAGE` 与这些 chip 共用同一页内状态。 */
    SLOOP_TAP_FM6P_FIELD,    /* FM6P 单字段写（底部 ±；TAP 不能拖拽）：a=op(0..6，6=全局)
                              * b=字段号 v=**写后回读** err=0 成功 / 1 = 锁定页拒绝（未下发
                              * 命令，v 为写前真值）/ 2 = 命令失败（synth_status != OK）。
                              * 字段名由 `sloop_ui_fm6p_field_name()` 给出。 */
    SLOOP_TAP_FM6P_BANK,     /* BANK 行槽号 ±：a=新槽号(0..26) d=±1（只改页内目标槽）
                              * 锁定页拒绝（不改槽，a 为原值）。 */
    SLOOP_TAP_FM6P_SAVE,     /* SAVE chip：a=目标槽 v=0 已下发 CMD_BANK_PUT / 1 锁定页拒绝
                              * （未下发命令）。结果文本由 `sloop_ui_fm6p_status_text()` 与
                              * 屏幕同源给出。 */
    SLOOP_TAP_SONG_SECTION,  /* SONG 页 A–D 卡：a=页 id b=section(0..3)（UI 局部选中，不写引擎） */
    SLOOP_TAP_SONG_ENTRY,    /* SONG 页歌曲条目 **0 起**（0..15）：a=页 id b=entry（UI
                              * 局部选中；屏幕 `01`/`ENTRY 2` 是 1 起，刻意的两种口径） */
    SLOOP_TAP_SONG_ACTION,   /* SONG 页动作 chip（P4-C Task 11）：
                              * a=页 id b=sloop_song_act_t v=写后回读值（scene/bars/count/
                              * loop/chain 项数；STORE/LOAD/… = 0）d=落点 section(0..3)
                              * err=0 成功 / 1..7 = app_seq_status_t（映射见 console）；
                              * 页面守卫（未就绪/走带中 MODE 或配置）回 7/5 且不发命令。
                              * 动作结果文本由 `sloop_ui_song_status_text()` 与屏幕同源给出。 */
    SLOOP_TAP_SYSTEM,        /* SYSTEM 页：a=行（0..8）v=写后回读值 d=±1 err=settings_status */
    SLOOP_TAP_TOOLS,         /* TOOLS 页三张 chip（P4-F Task 6）：a=动作（0 CLRSEQ / 1 INITSOUND /
                             * 2 NEWPRJ = sloop_tool_t）v=1 本次点按**只进入/保持预备态**
                             * （未执行，1.5s 内再点同一 chip 才执行）/ 0 已执行（一条
                             * CMD_TOOLS）。结果文本由 `ui_tools_status_text` 与屏幕同源给出
                             * （`AGAIN TO CONFIRM` / `PATTERN CLEARED` / `STOP THE SONG FIRST`…）。 */
    SLOOP_TAP_SOUND_FOCUS,   /* SOUND 仪表盘的槽（5B-1）：a=卡片 b=卡内槽 v=成员表扁平下标
                             * （sound_layout_slot_index）。命中**存在**槽的矩形：只设焦点、不改值
                             * （作用域与锁定态由 console 经 sloop_ui_sound_slot_info 取）。 */
    SLOOP_TAP_SOUND_CARD,    /* SOUND 仪表盘的卡片标题行（5B-1）：a=卡片 v=**翻转后**的该卡折叠位
                             * （命中即翻折叠位，重排在下一拍）。 */
    SLOOP_TAP_SOUND_PICK_OPEN, /* SOUND 页眉行的名字区（5B-1 Task 6，中段；左右 42px 仍是 ∓1）：a=行
                             * （0 = ENGINE，鼓轨 = KIT / 1 = PRESET）。命中即打开选择器。 */
    SLOOP_TAP_SOUND_PICK,    /* SOUND 选择器（5B-1 Task 6）：a=0 引擎（d=1 时是鼓 kit）/ 1 预置，
                             * b=下标，v=命令值（**已下发**时；否则 0），d=1 鼓轨，
                             * err=SLOOP_PICK_ERR_*（0 已下发且状态层回 OK / 1 只移动 chip 高亮 /
                             * 2 预览别的引擎时点了预置行，未下发 / 3 确认的就是当前项，未下发、
                             * 已关闭 / 4 上下文已变，未下发、已关闭 / 5 FROZEN / 6 INVALID）。 */
} sloop_tap_kind_t;

/* SLOOP_TAP_SOUND_PICK 的结果码（sloop_tap_result_t.err） */
typedef enum {
    SLOOP_PICK_ERR_NONE = 0,
    SLOOP_PICK_ERR_HIGHLIGHT, /* 只移动 chip 高亮（预览），未下发 */
    SLOOP_PICK_ERR_PREVIEW,   /* chip 高亮 ≠ 当前引擎时点预置行：未下发（换引擎与换预置是两条命令） */
    SLOOP_PICK_ERR_CURRENT,   /* 确认的就是当前引擎/预置/kit：未下发，选择器关闭 */
    SLOOP_PICK_ERR_CONTEXT,   /* 打开后选中轨/引擎已变：未下发，选择器关闭 */
    SLOOP_PICK_ERR_FROZEN,
    SLOOP_PICK_ERR_INVALID,
} sloop_pick_err_t;

/* SONG 页动作（P4-C Task 11）：控件回调与 console `TAP` 共用同一编号与同一个动作
 * 函数；一条动作 = 一条状态层命令（SECTION_* / CHAIN_* / SONG_MODE / SONG_REC /
 * SONG_SET），绝不直接写上游全局。CHAIN 的触摸语义 = SAVE 层 begin/commit：
 * 第一次按 = begin（立即请求选中 section）；链中 tap A–D 卡只进 UI 临时列表；
 * 再按 = commit（一次原子安装）。CANCEL = CMD_CHAIN_CANCEL。
 * BARS-/COUNT- 等按钮各是一条 CMD_SONG_SET（整份 cfg 随命令下发）。 */
typedef enum {
    SLOOP_SONG_STORE = 0, SLOOP_SONG_LOAD, SLOOP_SONG_ERASE, SLOOP_SONG_JUMP,
    SLOOP_SONG_MODE, SLOOP_SONG_CHAIN, SLOOP_SONG_CANCEL, SLOOP_SONG_REC,
    SLOOP_SONG_SCENE, SLOOP_SONG_BARS_DN, SLOOP_SONG_BARS_UP,
    SLOOP_SONG_COUNT_DN, SLOOP_SONG_COUNT_UP, SLOOP_SONG_LOOP,
    SLOOP_SONG_ACT_COUNT,
} sloop_song_act_t;

/* TRANSPORT 页的工具动作（P4-C Task 10）：控件回调与 console `TAP` 共用同一编号与
 * 同一动作函数；一个动作 = 一条 EditCmd（REC / fill / 聚焦步字段 / pattern 工具）。
 * console 回显按本编号给出命令名（见 console.c 的 tap 回显表）。 */
typedef enum {
    SLOOP_TR_REC = 0,    /* CMD_REC_TOGGLE（arm/off/record/close，状态机在上游） */
    SLOOP_TR_FILL_HOLD,  /* CMD_FILL_CONTROL：a=翻转后的保持电平（点按 = 切换） */
    SLOOP_TR_FILL_NEXT,  /* CMD_FILL_CONTROL：b=1 脉冲（只翻转 fill_arm） */
    SLOOP_TR_LVL_DN, SLOOP_TR_LVL_UP,   /* CMD_STEP_EDIT lvl（鼓轨 = CMD_DRUM_STEP_WRITE） */
    SLOOP_TR_RAT_DN, SLOOP_TR_RAT_UP,   /* CMD_STEP_EDIT rat（鼓轨 = CMD_DRUM_STEP_WRITE） */
    SLOOP_TR_MIC_DN, SLOOP_TR_MIC_UP,   /* CMD_SET_MICRO（-32..31，UI 侧钳位） */
    SLOOP_TR_SPAN_DN, SLOOP_TR_SPAN_UP, /* CMD_STEP_EDIT span（鼓轨不适用） */
    SLOOP_TR_SLIDE,                     /* CMD_STEP_EDIT slide（toggle；鼓轨不适用） */
    SLOOP_TR_ROT_DN, SLOOP_TR_ROT_UP,   /* CMD_PATTERN rotate（选中轨） */
    SLOOP_TR_LEN_X2, SLOOP_TR_LEN_D2,   /* CMD_PATTERN length（×2 / ÷2） */
    SLOOP_TR_TRANS_DN, SLOOP_TR_TRANS_UP, /* CMD_PATTERN transpose */
    SLOOP_TR_CLEAR, SLOOP_TR_UNDO, SLOOP_TR_REDO, /* CMD_PATTERN clear/undo/redo */
    /* REC 配置三 chip（P4-F Task 6；上游 REC 屏的 QUANT / MODE / START）：每条 = 一条
     * CMD_REC_CONFIG（整份三元组下发，只翻转自己那一项；其余两项取 rec 投影）。 */
    SLOOP_TR_REC_QNT,     /* quant_off 取反 */
    SLOOP_TR_REC_MODE,    /* tempo_mode 取反（FREE / TEMPO） */
    SLOOP_TR_REC_START,   /* count_mode 取反（NOTE / COUNT） */
    SLOOP_TR_ACT_COUNT,
} sloop_transport_act_t;

/* TRANSPORT 工具动作的 err 值（sloop_tap_result_t.err）：0 成功；1..7 =
 * app_seq_status_t；8 = 不适用（鼓轨上的 span/slide，未下发命令）。 */
#define SLOOP_TR_ERR_NA 8u

typedef struct {
    sloop_tap_kind_t kind;
    uint8_t a, b;
    int16_t v;
    int8_t d;  /* SLOOP_TAP_PARAM_STEP / SLOOP_TAP_BANK_SLOT：±1 步进/选槽方向 */
    uint8_t err; /* SLOOP_TAP_STEP_DRUM / SLOOP_TAP_TRANSPORT_TOOL：0 = 成功；否则是
                  * app_seq_status_t（6 frozen / 5 blocked / 4 full / 3 invalid /
                  * 2 not_found / 7 error）；TRANSPORT 另有 8 = 不适用（未下发）。
                  * 失败时 STEP 鼓格的 v/d 仍是状态层写后回读的**真值**（未变）；TRANSPORT
                  * 的 v 是写后回读值、d 是动作方向（不参与回读），console 打 ERR 令牌。 */
} sloop_tap_result_t;

typedef enum {
    SLOOP_TAP_RESULT_OK = 0,
    SLOOP_TAP_RESULT_MISS,        /* 矩形表未命中（正常未命中，不改状态） */
    SLOOP_TAP_RESULT_NOT_STARTED, /* UI 未启动 */
    SLOOP_TAP_RESULT_LOCK_TIMEOUT, /* LVGL 锁超时：未派发 */
    SLOOP_TAP_RESULT_QUIESCED     /* 诊断期静默：未派发（console 打印 TAP ERR quiesced） */
} sloop_tap_result_status_t;

sloop_tap_result_status_t sloop_ui_input_tap(int16_t x, int16_t y, sloop_tap_result_t *out);

/* FILE 页上下文（InputManager 路由，P4-B Task 12）：当前页是 PROJECTS/PRESETS 时
 * ←/→ 在槽位间移动选中槽、ENTER = LOAD（SAVE/ERASE 只走触摸按钮与 console，避免与
 * 演奏键冲突）。
 * 三个函数都不取 LVGL 锁（可在输入任务调用）：选中槽是 UI 局部态，页面描边由 30ms
 * UI 定时器的 update() 落地；LOAD 走状态层 CMD_PRESET_LOAD/CMD_PROJECT_LOAD。
 * 非 FILE 页/UI 未启动时为空操作。sloop_ui_file_status_text 返回当前 FILE 页状态行
 * 文本（console `TAP` 回显与屏幕文本同源）；非 FILE 页返回 NULL。 */
bool sloop_ui_file_page_active(void);
void sloop_ui_file_slot_move(int8_t delta); /* ←/→：±1 槽位（环绕） */
void sloop_ui_file_load(void);              /* ENTER：LOAD 选中槽 */
const char *sloop_ui_file_status_text(void);

/* 参数页上下文（InputManager 路由，ADR-008）：当前页是参数页（GLOBAL/FX/SOUND…）
 * 时 ←/→ 选槽、↑/↓ 调值，不再驱动 STEP 焦点/轨焦点。
 * 三个函数都不取 LVGL 锁（可在输入任务调用）：焦点是 UI 局部态（临界区保护），
 * 调值走状态层 CMD_SET_PARAM/CMD_SET_GLOBAL，页面控件由 30ms UI 定时器刷新。
 * 非参数页/UI 未启动时为空操作；不可编辑的引擎页（所选轨引擎与页不符）忽略调值。 */
bool sloop_ui_param_page_active(void);
void sloop_ui_param_focus_move(int8_t delta); /* ←/→：±1 槽位（跳过空槽/隐藏槽，环绕） */
void sloop_ui_param_adjust(int8_t delta);     /* ↑/↓ 与 ENTER(+1)：±1 原始步进（钳位在引擎） */

/* 参数页 chip 框架（P4-F Task 4/5；布局表 page_layout.c，ADR-008 §8.9）：MOD/SCALE/VOICE/
 * GLOBAL/FX 页由多个 chip 复用同一组 8 个旋钮，选中 chip 是**页内 UI 局部态**（chip 行、键盘
 * 与 console `UI <page> CHIP <n>` 共用；切 chip 零分配重配，下一个 30ms 拍落地）。
 * - sloop_ui_param_page(page)：该页 id 在参数页布局表里（GLOBAL/FX/SOUND/SAMPLE/
 *   MOD/SCALE/VOICE；APP_PAGE_FM6 是空位 id ⇒ false）；未启动前即可调用（只查表）。
 * - sloop_ui_param_chip_count(page)：页的有名 chip 数（0 = 整页，无 chip 行：SOUND/SAMPLE）。
 * - sloop_ui_param_chip_set(page, chip)：写选中 chip；越界/非 chip 页 =
 *   ESP_ERR_INVALID_ARG，条件 chip 在当前上下文隐藏 = ESP_ERR_NOT_ALLOWED（不改选择），
 *   另有 INVALID_STATE（UI 未启动/诊断期静默）、TIMEOUT（LVGL 锁超时）。
 * - sloop_ui_param_chip_get(page)：生效 chip（隐藏时 = 夹后的首个可见 chip）；非参数页 0。
 * - sloop_ui_param_slot_locked(page, chip, slot)：槽位是否锁定（显示 -- 且拒绝编辑：
 *   引擎页不适用、鼓轨逐槽规则、槽不存在）。只读状态层快照与布局表，不取 LVGL 锁。 */
bool sloop_ui_param_page(uint8_t page);
uint8_t sloop_ui_param_chip_count(uint8_t page);
esp_err_t sloop_ui_param_chip_set(uint8_t page, uint8_t chip);
uint8_t sloop_ui_param_chip_get(uint8_t page);
bool sloop_ui_param_slot_locked(uint8_t page, uint8_t chip, uint8_t slot);
/* 槽位诊断（console `UI <page> CHIP` 的逐槽行）：id = 状态层命令里的绝对 id，label =
 * 旋钮正在显示的名称（"-" = 无控件），locked = 显示 -- 且拒绝编辑。槽不存在回 false。 */
bool sloop_ui_param_slot_info(uint8_t page, uint8_t chip, uint8_t slot, uint8_t *id,
                              const char **label, bool *locked);

/* Sound 仪表盘（SOUND 页；5B-1 Task 3，逻辑在 page_sound.c）：脚本靠它定位槽、滚动视口并
 * 注入槽级拖动。槽一律用成员表的扁平下标（卡片 × 卡内槽 ⇄ 扁平下标：
 * sloop_ui_sound_slot_index）。错误码：UI 未启动或当前页不是仪表盘 = ESP_ERR_INVALID_STATE；
 * 页对象未建（含诊断期静默）= ESP_ERR_INVALID_STATE；下标越界 = ESP_ERR_INVALID_ARG；
 * 槽不存在（当前上下文没有这个槽）= ESP_ERR_NOT_FOUND；持锁入口另有 ESP_ERR_TIMEOUT。
 * 不取锁的入口只读 PSRAM 里的槽状态与单字节当前页，最坏读到相邻拍的解析结果。 */
bool sloop_ui_sound_dashboard_active(void);                 /* 无锁 */
uint16_t sloop_ui_sound_slot_index(uint8_t card, uint8_t slot); /* 卡片/卡内槽 → 扁平下标；越界 0xFFFF。无锁、只查表 */
uint16_t sloop_ui_sound_slot_total(void);                   /* 当前上下文存在的槽数。无锁 */
uint16_t sloop_ui_sound_collapsed(void);                    /* 折叠位掩码（一位一张卡）。无锁 */
esp_err_t sloop_ui_sound_slot_info(uint16_t flat, uint8_t *card, uint8_t *slot, uint8_t *scope,
                                   uint8_t *id, const char **label, bool *locked, int16_t *value); /* 无锁 */
esp_err_t sloop_ui_sound_slot_rect(uint16_t flat, int16_t *x, int16_t *y, int16_t *w, int16_t *h); /* 持锁；屏幕绝对坐标 */
esp_err_t sloop_ui_sound_scroll_get(int16_t *y, int16_t *max_y); /* 持锁 */
esp_err_t sloop_ui_sound_scroll_set(int16_t y);                /* 持锁；钳位 [0, max] */
/* 槽级旋钮注入（console `UI SOUND KNOB <flat> <press|drag> <dx> <dy>`，dx/dy 相对槽圆心）：
 * 与 `UI KNOB` 同语义（press 记锚点不改值；drag = 相对锚点的角度差 → 值差），走与物理触摸
 * 相同的处理路径。ESP_OK = 接受；ESP_ERR_NOT_FOUND = 环带外/无锚点或槽不存在；
 * ESP_ERR_NOT_ALLOWED = 锁定槽。*value 回填槽当前值（可 NULL）。持锁 */
esp_err_t sloop_ui_sound_test_knob(uint16_t flat, int32_t dx, int32_t dy, bool press, int16_t *value);

/* Sound 仪表盘的焦点与折叠（5B-1 Task 4，逻辑在 page_sound.c）。焦点槽恒为一个存在的槽：悬空的
 * 焦点（换轨/换引擎后描述符消失）按「扁平序里下一个可聚焦的槽，否则首个存在槽」重指；折叠卡片
 * 里的槽仍是焦点（± 与读数行照常），←/→ 跳过折叠卡片。滚动到可见与重排都在其后的 30ms 拍里。 */
esp_err_t sloop_ui_sound_focus_get(uint16_t *flat, uint8_t *card, uint8_t *slot); /* 无锁；flat/card/slot 可 NULL */
/* 写焦点并立即滚到可见（持锁）。flat 越界 ESP_ERR_INVALID_ARG；当前上下文不存在的槽
 * ESP_ERR_NOT_FOUND；页对象未建 ESP_ERR_INVALID_STATE；锁超时 ESP_ERR_TIMEOUT。 */
esp_err_t sloop_ui_sound_focus_set(uint16_t flat);
/* 翻第 card 张卡的折叠位（持锁；卡片重排在下一拍）。card ≥ 13 回 ESP_ERR_INVALID_ARG。 */
esp_err_t sloop_ui_sound_card_toggle(uint8_t card);

/* Sound 选择器（5B-1 Task 6，逻辑在 page_sound_picker.c）：点页眉行名字区（TAP 回
 * SLOOP_TAP_SOUND_PICK_OPEN）打开的引擎/预置/鼓 kit 面板；chip 只高亮，列表行选择各发一条
 * CMD_SET_ENGINE / CMD_SET_PRESET / CMD_SET_PARAM（kit）并关闭。下面三个**不取 LVGL 锁**，给键盘
 * 路由（输入任务）用：active = 面板打开；confirm = ENTER（最后移动的那一行：chip 行 ⇒ 换引擎，
 * 列表 ⇒ 选中项）；cancel = ESC（只关面板，不切页）。选择器打开时 ←/→/↑/↓ 仍走
 * sloop_ui_param_focus_move / sloop_ui_param_adjust，由页面层转给选择器。 */
bool sloop_ui_sound_picker_active(void);
void sloop_ui_sound_picker_confirm(void);
void sloop_ui_sound_picker_cancel(void);
/* console `UI SOUND PICK [<n>]`（持锁）：第 n 行的文字与是否当前项 / 屏幕矩形（含 72px 页签条），
 * 以及状态行（打开的行、当前引擎、chip 高亮、列表行数）。未打开（或不在仪表盘页）
 * ESP_ERR_INVALID_STATE；n ≥ 行数 ESP_ERR_NOT_FOUND；锁超时 ESP_ERR_TIMEOUT。 */
esp_err_t sloop_ui_sound_pick_rect(uint8_t n, int16_t *x, int16_t *y, int16_t *w, int16_t *h);
esp_err_t sloop_ui_sound_pick_info(uint8_t n, char *buf, uint32_t buf_n, bool *current);
esp_err_t sloop_ui_sound_pick_status(uint8_t *row, uint8_t *engine, uint8_t *highlight, uint8_t *rows);

/* STEP 页聚焦步与鼓 lane（P4-C Task 9；InputManager 与 console `UI LANE` 共用）：
 *  - 焦点环 = T1,T2,T3,DRM L1..L16（↑/↓ 在三条合成轨间环绕；从 T3 向下进入 lane 1、
 *    从 T1 向上进入 lane 16；鼓轨上 ↑/↓ 逐 lane，越界回合成轨）。←/→ 仍是窗口列。
 *  - lane 选择是 UI 局部态（page_step.c 的静态字节，跨组页面重建保留），不写引擎；
 *    ENTER = 切换**聚焦步**上该 lane 的 on 位（CMD_DRUM_STEP_WRITE，整份原始
 *    16 lane POD，lvl/rat 原样保留）。
 * 这些入口都不取 LVGL 锁（输入任务/console 可调用）：状态改写走状态层命令，
 * 控件由 30ms UI 定时器刷新。非 STEP 页为空操作（`..._active()` 返回 false）。 */
bool sloop_ui_step_page_active(void);
uint8_t sloop_ui_step_lane_get(void);
void sloop_ui_step_lane_set(uint8_t lane);
void sloop_ui_step_focus_vertical(int8_t delta); /* ↑=-1 / ↓=+1（含进/出鼓轨） */
void sloop_ui_step_drum_toggle(void);            /* ENTER：聚焦步的选中 lane 位取反 */

/* EDIT 层步工具（P4-F Task 6；InputManager 的 Aa+k / Aa+l / Aa+9（或 Aa+;）与 console `KEY`
 * 注入共用）：作用于 **STEP 页**的聚焦步（非 STEP 页为空操作）。
 *  - LOCK：把状态层记录的 last_param（焦点轨上最后碰过的参数）以该轨当前值锁到聚焦步
 *    （CMD_LOCK_FOCUS{a=1}）；UNLOCK：删除该键的锁（CMD_LOCK_FOCUS{a=0}）；FILL：聚焦步的
 *    fill 条件循环 NORMAL → FILL ONLY → NO FILL → NORMAL（CMD_FILL_STEP，值取快照里的当前
 *    条件 +1）。可锁性、锁表容量、空步与冻结的判定都在状态层/引擎，本层只回读 seq_status。
 *  - 结果文本进页内邮箱：STEP 页状态行顶替显示 ≈2.4s；sloop_ui_step_tool_text 取同一份
 *    文本（`LOCKED <参数名>` / `UNLOCKED` / `FILL <模式>` / `NO STEP` / `NO PARAM` /
 *    `NO LOCK` / `NOT LOCKABLE` / `LOCK TABLE FULL` / `FROZEN`）。*seq 是结果代号（0 = 还
 *    没有结果；每次工具动作 +1），console `KEY` 据此只在有新结果时追加回显。返回指针指向
 *    组件内静态缓冲（下一次调用覆盖）。不取 LVGL 锁。 */
typedef enum {
    SLOOP_STEP_TOOL_LOCK = 0,
    SLOOP_STEP_TOOL_UNLOCK,
    SLOOP_STEP_TOOL_FILL,
} sloop_step_tool_t;
void sloop_ui_step_tool(uint8_t tool);
const char *sloop_ui_step_tool_text(uint32_t *seq);

/* TRANSPORT 页工具焦点（P4-C Task 10；InputManager 与 console `UI TOOL` 共用）：
 *  - 焦点字段 = LVL,RAT,MIC,SPAN,SLIDE,ROT,LEN,TRANS（0..7）：←/→ 在环上移动、
 *    ↑/↓ 调焦点字段（LVL/RAT/SLIDE/ROT/TRANS 走 ±1 或切换，MIC ±1，LEN ×2/÷2，
 *    SPAN ±1 步）——与参数页的「焦点 + ↑/↓ 调值」同一模型（ADR-008 §8）。
 *  - 动作仍走 page_transport.c 的唯一动作函数（一条动作 = 一条 EditCmd）；触摸点
 *    任一字段 chip 也会把焦点落到该字段。
 *  - 焦点是 UI 局部态（page_transport.c 的静态字节，跨组页面重建保留），不写引擎；
 *    描边由 30ms UI 定时器落地。这些入口都不取 LVGL 锁（输入任务/console 可调用）。
 *    只有调值入口按页面门控（`..._active()` 为假时静默忽略）；焦点读写/移动不受
 *    页面限制（console `UI TOOL` 可在任意页预置，供脚本走查）。
 *  - tool_set 越界返回 false（`UI ERR tool`，不改焦点）。 */
#define SLOOP_UI_TRANSPORT_FIELDS 8u
bool sloop_ui_transport_page_active(void);
uint8_t sloop_ui_transport_tool_get(void);
bool sloop_ui_transport_tool_set(uint8_t field);
void sloop_ui_transport_focus_move(int8_t delta); /* ←/→：±1 字段（环绕） */
void sloop_ui_transport_adjust(int8_t delta);     /* ↑/↓：+1 / -1 步进焦点字段 */

/* 当前 TRANSPORT 页状态行文本（console `TAP` 工具回显的 `status="…"` 与屏幕**同源**：
 * 两处都读 page_transport.c 的同一缓冲，不各写一份词表）。非 TRANSPORT 页/UI 未启动
 * 返回 NULL；返回指针指向组件内静态缓冲（下一次调用覆盖），调用方负责当次使用。 */
const char *sloop_ui_transport_status_text(void);

/* TOOLS 页状态行文本（console `TAP` 回显与屏幕同源；P4-F Task 6）：预备态给出
 * `AGAIN TO CONFIRM`，否则是最近一次执行动作的结果（theme.h 的 ui_tools_status_text
 * 单一映射）。当前页不是 TOOLS、或页未建（已随 MIX 组销毁）时返回 NULL。不取 LVGL 锁。 */
const char *sloop_ui_tools_status_text(void);

/* SOUND 页 bank 存取行（P4-F Task 5 起随 FM6 引擎出现在 SOUND 页底部；原属 FM6 页）的
 * 状态行文本（console `TAP` 回显与屏幕文本同源；P4-B Task 13）：当前页不是 SOUND、
 * bank 行尚未构建（页未建/已随源组销毁）、或行隐藏（选中轨不是 FM6 引擎的合成轨——
 * 那时 TAP 不命中）时返回 NULL。映射复用 theme.h 的 ui_file_status_text（act 0 SAVE /
 * 2 ERASE，与 FILE 页同一套
 * 文案：`SAVE OK` / `ERASE OK` / `STOP BEFORE SAVE` / `NO STORAGE` /
 * `DRUM TRACK: NO SOUND`）。不取 LVGL 锁（只读 UI 局部态 + 状态层快照），可在
 * console/任意任务调用。 */
const char *sloop_ui_bank_status_text(void);

/* SAMPLE 页槽管理行的状态行文本（console `TAP` 回显与屏幕文本同源；P4-D Task 8）：
 * 当前页不是 SAMPLE、或槽管理行尚未构建（页未建/已随源组销毁）时返回 NULL。页面本地
 * 拒绝（SET 不在 USR1..USR4，或选中轨引擎 ≠ SAMPLE）返回 `NOT USR`；其余映射由
 * theme.h 的 ui_sample_status_text 单一给出（`SET U<n>` 槽选中已写入 SET /
 * `ERASE OK` / `STOP BEFORE ERASE` / `NO STORAGE` / `ERASE ERROR`）。不取 LVGL 锁
 * （只读 UI 局部态 + 状态层快照），可在 console/任意任务调用。 */
const char *sloop_ui_sample_status_text(void);

/* DSYN 页（P4-D Task 9；SYNTH 组的鼓合成编辑页）：
 *  - kit/lane 选择是状态层保存的 UI 局部态（`state_set_dsyn_select`）：页面 chip 与
 *    console `UI DSYN KIT/LANE` 共用，TAP 之后 `STATE SNAP` 的 dsyn_kit/dsyn_lane 即
 *    刻反映屏幕；DSYN 与选中轨无关，本页没有锁定态。
 *  - 字段页（SND1/SND2/SND3/KIT）是**页内 UI 局部态**（`sloop_ui_dsyn_page_get/set`
 *    与页面 chip 共用；console `UI DSYN PAGE` 只写这一静态量，30ms UI 定时器下一拍
 *    重配旋钮）。跨组页面重建保留（与 step_window 同类）。
 *  - ←/→ 在 8 槽间移动焦点（跳过当前页的空槽、环绕），↑/↓ 调焦点槽 ±1（经状态层
 *    命令，控件由下一拍刷新）。KIT 页的 CRUSH（0..255）与 24 个声音字段同一路径
 *    （`CMD_DSYN_CRUSH`），KIT SRC 的值变化套用工厂 kit（`CMD_DSYN_LOAD_KIT`）。非 DSYN 页为空操作；都不取 LVGL 锁（输入任务调用）。
 *  - `sloop_ui_dsyn_status_text` 是 console `TAP … status="…"` 的文本源（与屏幕同源，
 *    theme.h 的 ui_dsyn_status_text 单一映射）；页未建（控件已随组作用域销毁）或当前页
 *    不是 DSYN 时返回 NULL（与 BANK/SAMPLE 行同口径）。
 *  - `sloop_ui_dsyn_field_name` 把 TAP 回显的字段码翻译成显示名（0x80/0x81 = KIT 页的
 *    CRUSH / KIT SRC 伪字段），未知码返回 NULL。 */
#define SLOOP_UI_DSYN_PAGES 4u
bool sloop_ui_dsyn_page_active(void);
void sloop_ui_dsyn_focus_move(int8_t delta); /* ←/→：±1 槽（跳过空槽，环绕） */
void sloop_ui_dsyn_adjust(int8_t delta);     /* ↑/↓：焦点槽 ±1 原始单位 */
uint8_t sloop_ui_dsyn_page_get(void);
void sloop_ui_dsyn_page_set(uint8_t page);   /* 0=SND1 1=SND2 2=SND3 3=KIT；越界忽略 */
/* 清掉「最近一次动作」的结果（状态行下一拍回到 DIRTY/空）。console `UI DSYN KIT/LANE`
 * 与页面 chip 同语义：换 kit/lane 是上下文切换，不继续显示旧 lane 的 EDIT OK。 */
void sloop_ui_dsyn_clear_action(void);
const char *sloop_ui_dsyn_status_text(void);
const char *sloop_ui_dsyn_field_name(uint8_t code);

/* FM6P 页（P4-D Task 10；SYNTH 组的 FM6 155B 深度 patch 编辑页）：
 *  - 算子（OP1..OP6）与字段页（EG/LEVEL/PITCH/PEG/VOICE/LFO）都是**页内 UI 局部态**
 *    （`sloop_ui_fm6p_op_get/set`、`..._page_get/set` 与页面 chip 共用；console
 *    `UI FM6P OP/PAGE` 只写这两个静态量，30ms UI 定时器下一拍重配旋钮）。跨组页面
 *    重建保留。算子只影响 EG/LEVEL/PITCH 三页（那三页编辑该算子的字段块），
 *    PEG/VOICE/LFO 三页是全局块（op 6）。
 *  - ←/→ 在 8 槽间移动焦点（跳过当前页的空槽、环绕），↑/↓ 调焦点槽 ±1（经
 *    `CMD_FM6_SET`，控件由下一拍刷新）。锁定页（选中轨不是 FM6，含鼓轨）为空操作；
 *    都不取 LVGL 锁（输入任务调用）。
 *  - `sloop_ui_fm6p_status_text` 是 console `TAP … status="…"` 的文本源（与屏幕同源，
 *    theme.h 的 ui_fm6p_status_text 单一映射）；页未建（控件已随组作用域销毁）或当前
 *    页不是 FM6P 时返回 NULL（与 BANK/SAMPLE/DSYN 行同口径）。
 *  - `sloop_ui_fm6p_field_name` 把 TAP 回显的 (op, field) 翻译成描述符 label
 *    （如 `ALG` / `R1`），空槽/越界返回 NULL。 */
#define SLOOP_UI_FM6P_OPS 6u
#define SLOOP_UI_FM6P_PAGES 6u
bool sloop_ui_fm6p_page_active(void);
void sloop_ui_fm6p_focus_move(int8_t delta); /* ←/→：±1 槽（跳过空槽，环绕） */
void sloop_ui_fm6p_adjust(int8_t delta);     /* ↑/↓：焦点槽 ±1 原始单位（锁定页忽略） */
uint8_t sloop_ui_fm6p_op_get(void);
void sloop_ui_fm6p_op_set(uint8_t op);       /* 0..5 = OP1..OP6；越界忽略 */
uint8_t sloop_ui_fm6p_page_get(void);
void sloop_ui_fm6p_page_set(uint8_t page);   /* 0=EG 1=LEVEL 2=PITCH 3=PEG 4=VOICE 5=LFO */
const char *sloop_ui_fm6p_status_text(void);
const char *sloop_ui_fm6p_field_name(uint8_t op, uint8_t field);

/* SONG 页上下文（InputManager 路由，P4-C Task 11）：当前页是 SONG 时
 *  - ←/→ 在 1..count 个歌曲条目间移动选中项（环绕）；
 *  - ↑/↓ 在 A–D section 卡间移动选中 section（环绕）；
 *  - ENTER = LOAD 选中 section（与页面 LOAD chip 同一条 CMD_SECTION_LOAD）。
 * 这些函数都不取 LVGL 锁（可在输入任务调用）：选中项是 UI 局部态，页面描边/文本由
 * 30ms UI 定时器的 update() 落地；LOAD 走状态层命令。非 SONG 页/UI 未启动时为空操作。
 * sloop_ui_song_status_text 返回最近一次动作的状态文本（console `TAP` 回显与屏幕
 * **同源**）；非 SONG 页/UI 未启动返回 NULL。 */
bool sloop_ui_song_page_active(void);
void sloop_ui_song_entry_move(int8_t delta);    /* ←/→：±1 条目（在 count 内环绕） */
void sloop_ui_song_section_move(int8_t delta);  /* ↑/↓：±1 section（A–D 环绕） */
void sloop_ui_song_load(void);                  /* ENTER：LOAD 选中 section */
const char *sloop_ui_song_status_text(void);

/* SYSTEM 页（P4-E Task 6）：↑/↓ 移焦点、←/→ 调值、ENTER=+1/切换；输入路径不碰 LVGL。 */
bool sloop_ui_system_page_active(void);
esp_err_t sloop_ui_system_key(uint32_t code);
const char *sloop_ui_system_status_text(void);
/* 诊断注入（console `SYS SELFTEST ARM`）：清 SYSTEM 页 SELFTEST chip 的「本 boot 已跑过」标记
 * 与结果缓存（两者都在 .noinit，跨 esptool 硬复位存活），下一次 chip 走完整 worker 路径。
 * 自检进行中返回 ESP_ERR_INVALID_STATE。不碰 LVGL。 */
esp_err_t sloop_ui_system_selftest_arm(void);

/* 调试注入（console `UI KNOB <slot> press|drag <dx> <dy>`，ADR-008 §8.2/§8.5）：
 * 向**当前**参数页第 slot 槽旋钮派发合成的 PRESSED/PRESSING 事件（dx/dy 相对旋钮
 * 中心），走与物理触摸相同的处理路径（press = 设焦点 + 记录锚点且不改值；
 * drag = 相对锚点的角度差 → 值差）。用于在无手指/无触摸注入的环境里机器验证
 * 「回调接线 + 相对拖拽」——物理拖拽手感仍需人测（DI-16）。
 * 内部取 LVGL 锁。返回：ESP_OK（事件被接受；*value = 旋钮当前值）；ESP_ERR_NOT_FOUND
 * （环带外，事件未接受，值不变）；ESP_ERR_NOT_ALLOWED（引擎页不适用，旋钮显示 --，
 * console 回 `ignored (locked)`）；ESP_ERR_INVALID_ARG（非参数页/槽位无控件）；
 * ESP_ERR_INVALID_STATE（UI 未启动或目标页未建）；ESP_ERR_TIMEOUT（LVGL 锁超时，
 * 未派发）。诊断期静默（quiesce）时返回 ESP_ERR_INVALID_STATE（console 打印
 * `UI ERR quiesced` 由调用方区分）。 */
esp_err_t sloop_ui_test_knob(uint8_t slot, int32_t dx, int32_t dy, bool press, int16_t *value);

/* 输入任务栈余量（高水位，字节；未启动返回 0）：input 任务在自己的轮询循环里采样的
 * 历史最小值（R13）。它只在**物理按键**派发过深路径后才会下降——console 的 `KEY` 注入跑在
 * console 任务上，量不到输入任务的栈；`sloop_ui_input_physical_events` 给出物理事件数，
 * 场景据此区分「量过」与「本轮没有物理按键」（后者不得当作通过）。诊断/门禁用。 */
uint32_t sloop_ui_input_stack_free(void);
uint32_t sloop_ui_input_physical_events(void);

/* 诊断期静默（`ENG SELFTEST` 全量运行期间；sloop_core_selftest 经
 * sloop_core_diag_quiesce_hook 调用，console `UI QUIESCE <on|off>` 可用于复验）：
 *  - on：先置输入丢弃标志并等 input 任务回到批次边界（≤50ms，超时记 WARN），
 *    再在 LVGL 锁内暂停 30ms UI 定时器、`lvgl_port_stop()` 停 LVGL 的刷新/indev
 *    定时器、`lv_indev_enable(NULL, false)` 禁用触摸 indev（物理触摸不再派发），
 *    并作废待执行切页。此后键盘事件只被计数、KEY/TAP 注入被拒
 *    （`SLOOP_TAP_RESULT_QUIESCED` / `KEY ERR quiesced`）。
 *  - off：`lvgl_port_resume()` + 恢复 30ms 定时器（下一拍 state_sync + 已建页刷新），
 *    唤醒 input 任务并清零/打印本轮丢弃量。
 * 返回值：true = 已达到目标状态；false = 未达到（on 取 LVGL 锁超时 → 输入静默已
 * 回滚、UI 仍在运行，selftest 据此拒绝执行；off 取锁重试后仍超时 → 保持 quiesce
 * 态，可再次调用 off）。UI 未启动时为空操作并返回 true；幂等。可从任意任务调用，
 * 不得在 ISR 中调用。 */
bool sloop_ui_quiesce(bool on);

/* 当前是否处于诊断期静默（UI 未启动恒为 false）。 */
bool sloop_ui_quiesced(void);

/* 诊断期丢弃的键盘事件数（恢复后清零；KEY STATS 回读）。 */
uint32_t sloop_ui_input_quiesce_dropped(void);


