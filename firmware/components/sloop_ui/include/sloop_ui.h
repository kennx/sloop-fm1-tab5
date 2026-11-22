/*
 * sloop-fm1-tab5 — sloop_ui 公共接口（LVGL 页面框架：分组页签 + 页面注册表）
 *
 * 单容器屏 + 顶部两行页签（ADR-005「导航决策」）：上行 5 个组签
 * PLAY|TRACKS|EDIT|SEQ|FILE（无已注册页的组不显示），下行显示当前组的已注册子页；
 * 组内顺序 = 子页顺序，`TAB` 按组顺序扁平轮转
 * （PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→PROJECTS→PRESETS→PLAY）。
 * 页面**首次访问时构建**，离开组时销毁该组全部已建页（组作用域生命周期：跨组
 * 切换峰值 = 源组已建 Σ + 目标页 est，见 ADR-005 §3/§5）；PLAY 在
 * sloop_ui_start() 内立即构建，保证开机首帧非空白。池增量与构建耗时记入
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

#define SLOOP_UI_GROUPS 5 /* PLAY / TRACKS / EDIT / SEQ / FILE（ADR-005） */

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

/* 注册一页（须在 sloop_ui_start() 之前调用；sloop_ui_start 内置注册现有 9 页）。
 * 注册表只增不删：先注册的外部页在内置页注册之后仍保留（内置注册幂等）。
 * 注册表只保存指针：ui_page_t 与 title 必须是静态生命周期（不得指向栈/临时内存）。
 * 返回：ESP_OK；ESP_ERR_INVALID_ARG（参数/页 id 非法或重复）；ESP_ERR_NO_MEM
 * （注册表满）；ESP_ERR_INVALID_STATE（UI 已启动）。 */
esp_err_t ui_page_register(const ui_page_t *page);

/* 已注册页数（注册顺序；输入层的 tab 轮转按组顺序扁平轮转，不按本计数）。
 * 注册表只在 start 前写入，读无需加锁；UI 未启动时返回已注册的外部页数（通常 0）。 */
uint8_t sloop_ui_page_count(void);

/* `TAB` 的下一张页（ADR-005 §4）：按组顺序扁平轮转
 * PLAY→TRACKS→GLOBAL→FX→ANALOG→FM6→SAMPLE→STEP→TRANSPORT→PROJECTS→PRESETS
 * →PLAY，**跳过未注册页**。page 未注册/非法时返回
 * 第一个已注册页；无任何已注册页返回 0xFF。页序号只依赖注册表（start 前可调用）。 */
uint8_t sloop_ui_page_order_next(uint8_t page);

/* 组签/子签的程序化入口（console `UI TAB <group>` / `UI SUB <index>`，R32）：与物理
 * 页签点击走**同一**动作函数（`group_select_locked`/`sub_select_locked`），因此覆盖
 * 「点组进上次子页 / 点子签切页」的导航路径（TAP 对 y<72 一律 miss，无法覆盖它）。
 * - sloop_ui_group_count()/sloop_ui_group_name(g)：组表（PLAY/TRACKS/EDIT/SEQ/FILE），
 *   名字为静态生命周期；越界 name 返回 NULL。
 * - group_select(group)：进入该组上次的子页（未记录/已未注册则首个已注册成员）；
 *   空组（没有已注册页的组）返回 ESP_ERR_INVALID_ARG。
 * - sub_select(index)：切到**当前组**第 index 张子签（0 起）；单页组子行为空 ⇒ 任何
 *   index 都返回 ESP_ERR_INVALID_ARG（与视觉/命中一致），index ≥ 5（下行固定 5 个
 *   子签槽位，组内最多 5 页）同样。
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
    uint8_t registered, built, failed;
    sloop_ui_page_stat_t pages[APP_STATE_PAGES]; /* 按注册（页签）顺序 */
    uint8_t group_count;                         /* 组数（当前 5） */
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

/* 切页：0=PLAY 1=TRACKS 2=STEP 3=TRANSPORT 4=GLOBAL 5=FX 6=ANALOG 7=FM6 8=SAMPLE
 * 9=PROJECTS 10=PRESETS（同 app_page_t）；首次进入时构建该页。
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
    SLOOP_TAP_STEP_DRUM,     /* b=绝对步号：鼓轨只读，只写状态行提示 */
    SLOOP_TAP_TRACK_CARD,    /* a=track：CMD_SELECT_TRACK */
    SLOOP_TAP_TRANSPORT_PLAY,
    SLOOP_TAP_TRANSPORT_STOP,
    SLOOP_TAP_TRANSPORT_BPM, /* v=±1：CMD_SET_BPM */
    SLOOP_TAP_PARAM_STEP,    /* 参数页 ± ：a=槽位 b=参数绝对 id v=落地后的值 d=±1（console 回显） */
    SLOOP_TAP_PARAM_FOCUS,   /* 参数页弧线/数值文本：a=槽位（只设焦点，不改值） */
    SLOOP_TAP_PARAM_LOCKED,  /* 参数页不可编辑（引擎页与所选轨引擎不符）：a=槽位 */
    SLOOP_TAP_FILE_SLOT,     /* FILE 页槽位（PROJECTS 卡 A–D / PRESETS 格）：
                              * a=页 id（PROJECTS/PRESETS）b=槽号（0..3 / 0..31） */
    SLOOP_TAP_FILE_ACTION,   /* FILE 页动作按钮：a=页 id b=槽号 v=0 SAVE/1 LOAD/2 ERASE。
                              * 动作后 store_status 的变化再由状态行/console 回显体现 */
    SLOOP_TAP_BANK_SLOT,     /* FM6 页 bank 目标槽 ±：a=页 id（FM6）b=新槽号(0..26)
                              * d=±1。只改目标槽，不写存储/引擎 */
    SLOOP_TAP_BANK_STORE,    /* FM6 页 BANK SAVE：a=页 id b=目标槽 v=0（CMD_BANK_PUT；
                              * 动作结果由状态行/console 回显体现） */
    SLOOP_TAP_BANK_ERASE,    /* FM6 页 BANK ERASE：a=页 id b=目标槽 v=2（CMD_BANK_ERASE） */
} sloop_tap_kind_t;

typedef struct {
    sloop_tap_kind_t kind;
    uint8_t a, b;
    int16_t v;
    int8_t d; /* SLOOP_TAP_PARAM_STEP / SLOOP_TAP_BANK_SLOT：±1 步进/选槽方向 */
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

/* 参数页上下文（InputManager 路由，ADR-008）：当前页是参数页（GLOBAL/FX/ANALOG…）
 * 时 ←/→ 选槽、↑/↓ 调值，不再驱动 STEP 焦点/轨焦点。
 * 三个函数都不取 LVGL 锁（可在输入任务调用）：焦点是 UI 局部态（临界区保护），
 * 调值走状态层 CMD_SET_PARAM/CMD_SET_GLOBAL，页面控件由 30ms UI 定时器刷新。
 * 非参数页/UI 未启动时为空操作；不可编辑的引擎页（所选轨引擎与页不符）忽略调值。 */
bool sloop_ui_param_page_active(void);
void sloop_ui_param_focus_move(int8_t delta); /* ←/→：±1 槽位（跳过空槽，环绕） */
void sloop_ui_param_adjust(int8_t delta);     /* ↑/↓：±1 原始步进（钳位在引擎） */

/* FM6 页 bank 存取行的状态行文本（console `TAP` 回显与屏幕文本同源；P4-B Task 13）：
 * 当前页不是 FM6、或 bank 行尚未构建（页未建/已随源组销毁）时返回 NULL。锁定页
 * （选中轨引擎 ≠ FM6，含鼓轨）的本地拒绝返回 `NOT FM6`（红字，不进 store_status）；
 * 其余映射复用 theme.h 的 ui_file_status_text（act 0 SAVE / 2 ERASE，与 FILE 页同一套
 * 文案：`SAVE OK` / `ERASE OK` / `STOP BEFORE SAVE` / `NO STORAGE` /
 * `DRUM TRACK: NO SOUND`）。不取 LVGL 锁（只读 UI 局部态 + 状态层快照），可在
 * console/任意任务调用。 */
const char *sloop_ui_bank_status_text(void);

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

/* 输入任务栈余量（高水位，字节；未启动返回 0）。诊断/门禁用。 */
uint32_t sloop_ui_input_stack_free(void);

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


