/*
 * sloop_ui InputManager：Tab5 Keyboard 事件码 → 层模型 → EditCmd（ADR-006）
 *
 * 唯一入口 sloop_ui_input_key(code, down)，两条路径共用：
 *  - input 任务（本文件）：tab5_kb_poll() 按 INPUT_POLL_TICKS 取事件（100Hz tick 下
 *    10ms，见下方 tick 地板说明），优先级 4（低于音频 5）；
 *  - console 注入 `KEY <code> <down|up>`（console.c），与物理按键同入口。
 * code 为 tab5_keyboard.h 的事件码（row<<4|col），键名见 tab5_kb_key_name()。
 *
 * 映射表（k_bindings）：{code, layer, action, param}。查表两遍——先找「所持层」
 * 的条目（同码多条按表层序，GLO > SEL > EDIT），没有再落基础层。因此层内功能
 * 覆盖同键的基础功能（如 EDIT 层 c/d/e/g 是音名键而不是演奏键），而方向键/走带
 * 这类未进层的键在任何层下都可用。
 *
 * 层键：Sym=FX（MVP 无功能，只置位 layer_held 位）、Aa=EDIT、Ctrl=SEL、Alt=GLO；
 * 按住生效（DI-4 子集：键盘固件逐键独立上报 down/up，组合不吞事件——见
 * docs/design/006-keyboard-mapping.md）。layer_held 写 AppState（state_set_layer）。
 * 演奏键只在「无层或仅 FX」时发声：EDIT/SEL/GLO 按住期间演奏键不弹音符。
 *
 * 演奏路由（P4-C Task 6）：选中合成轨走 24 音矩阵（`CMD_NOTE_ON/OFF`）；选中鼓轨
 * 走 `input_map.h` 的 16 lane 表（`CMD_DRUM_HIT`，松开 v=0），其它演奏键在鼓轨忽略
 * （不回落合成轨 0）。space 是**物理 PLAY 键**（`CMD_PLAY_BUTTON`，free take drop），
 * `0` = REC 切换，`-` 按住 = FILL，`+` 点按 = FILL NEXT（ADR-006）。
 *
 * 参数页路由（ADR-008 §8）：当前页是参数页（GLOBAL/FX/ANALOG…）时，←/→ 在槽位间
 * 移动焦点、↑/↓ 调焦点槽的值（经 sloop_ui_param_*，只写状态层；控件由 30ms UI
 * 定时器刷新），不再驱动 STEP 焦点列/轨焦点。
 *
 * FILE 页路由（P4-B Task 12）：当前页是 PROJECTS/PRESETS 时 ←/→ 选槽、ENTER = LOAD
 * （经 sloop_ui_file_*，同样只写状态层/页内局部态；SAVE/ERASE 只走触摸按钮与 console，
 * 避免与演奏键冲突——spec §4.5）。
 *
 * STEP 页路由（P4-C Task 9）：↑/↓ 走焦点环 T1,T2,T3,DRM L1..L16（sloop_ui_step_*，
 * lane 是 UI 局部态）；ENTER 切换聚焦步的选中 drum lane 位（CMD_DRUM_STEP_WRITE）。
 * 鼓轨不是只读行：触摸点 lane dot = 选中该 lane 并切换，点格内其它位置 = 切换当前
 * 选中 lane（与控件回调同一动作函数）。
 *
 * TRANSPORT 页路由（P4-C Task 10）：←/→ 移动工具焦点字段（LVL,RAT,MIC,SPAN,SLIDE,
 * ROT,LEN,TRANS），↑/↓ 调焦点字段（LVL/RAT/MIC/SPAN ±1、SLIDE/ROT/LEN/TRANS 见
 * page_transport.c），ENTER = +1/切换——与参数页「焦点 + ↑/↓ 调值」同一模型。
 * 动作仍是状态层命令（CMD_REC_TOGGLE/CMD_FILL_CONTROL/CMD_STEP_EDIT/…）。
 *
 * SONG 页路由（P4-C Task 11）：←/→ 在歌曲条目间移动选中项、↑/↓ 在 A–D section 卡
 * 间移动、ENTER = LOAD 选中 section（与页面 LOAD chip 同一条 CMD_SECTION_LOAD）。
 * 选中项是 UI 局部态（page_song.c 静态字节），状态改写只走状态层命令。
 *
 * 键码 / 动作枚举 / 绑定表 k_bindings / 查表 input_map_find_binding 在 input_map.h（P4-F Task 6 起：
 * 纯数据，host 单测 scripts/knob-test.sh 同源编译，校验「同层同键至多一条」与 EDIT 层
 * 步工具键的独占性）；本文件只留任务、层状态与派发。
 *
 * EDIT 层步工具（P4-F Task 6，ADR-006）：Aa+k / Aa+l 把「焦点轨上最后碰过的参数」锁到
 * STEP 页聚焦步 / 解锁，Aa+9（或 Aa+;）循环该步的 fill 条件——都经 sloop_ui_step_tool
 * → CMD_LOCK_FOCUS / CMD_FILL_STEP；结果文本显示在 STEP 页状态行（零新增 LVGL 对象）。
 *
 * 锁纪律：输入路径不调用任何 LVGL 入口（音符指示与切页都是无锁邮箱，由 30ms
 * UI 定时器消费），只取 input 互斥 → state 互斥 → 引擎互斥；console 注入与
 * input 任务并发时由本地互斥串行。
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_map.h"
#include "sloop_state.h"
#include "sloop_store.h"
#include "sloop_ui.h"
#include "tab5_keyboard.h"

static const char *TAG = "sloop_input";

#define INPUT_TASK_STACK 4096
#define INPUT_TASK_PRIO  4 /* 低于音频任务（5），与键盘解码任务同级 */
#define INPUT_POLL_MS    5
/* tick 下限 1：CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(5) 取整成 0，vTaskDelay(0)
 * 只 yield 不阻塞——input 任务会退化成全速自旋，每秒数十万次跨核 yield IPI
 * （esp_crosscore_int_send 的全局自旋锁），与 UI/DSP 并发时打死 FreeRTOS SMP
 * 端口的 IPI 路径（vPortYield busy-wait、IDLE 饿死；DI-11 的真因）。 */
#define INPUT_POLL_TICKS (pdMS_TO_TICKS(INPUT_POLL_MS) > 0 ? pdMS_TO_TICKS(INPUT_POLL_MS) : 1)
#define INPUT_BATCH      8

/* 演奏基音：低八度行（z 排）= C3，高八度行（q 排）= +12 半音（C4=60） */
#define INPUT_BASE_NOTE 48u
#define INPUT_NOTE_MAX  127u

static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static bool s_started;

/* 诊断期静默（sloop_ui_quiesce）：标志置位后 input 任务只排空事件队列并计数，
 * 不再派发（因此不碰 state/引擎锁）；console 注入经 sloop_ui_input_key 也被拦下。
 * s_quiesce_ack 只在 input 任务回到「批次之间」时自增——quiesce 用它确认在途批次
 * 已收尾（置标志时任务可能正派发一批事件，只等标志不够）。 */
static volatile bool s_quiesce;
static volatile uint32_t s_quiesce_ack;
/* 丢弃计数由 input 任务与 console（KEY 注入被拒）两个上下文写：原子自增/清零，
 * 避免丢计数（诊断量级，不需要严格一致） */
static atomic_uint s_quiesce_dropped;

/* 按下的演奏键：note-off 用按下时的音符/轨号，不受层/选轨变化影响；鼓键单独记
 * （鼓是一次性 one-shot，PANIC 不发送 note-off，只补发自由录音需要的松开）。 */
static bool s_note_on[128];
static uint8_t s_note_val[128];
static uint8_t s_note_track[128];
static bool s_drum_on[128];
static bool s_play_down;

static void input_lock(void)
{
    if (s_mutex != NULL) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void input_unlock(void)
{
    if (s_mutex != NULL) xSemaphoreGive(s_mutex);
}

/* 演奏键：选中合成轨 = 24 音半音矩阵；选中鼓轨 = 16 lane 鼓键表（ADR-006），其它
 * 演奏键在鼓轨上忽略——**不回退 track 0**。释放**永远**按按下时记录的轨号收尾
 * （s_note_on/s_note_track），即使期间切到鼓轨也不进鼓分支（否则音符挂住）。 */
static void act_note(uint8_t code, int8_t offset, bool down)
{
    if (!down) {
        if (!s_note_on[code]) return;
        s_note_on[code] = false;
        const edit_cmd_t cmd = { .type = CMD_NOTE_OFF, .a = s_note_track[code],
                                 .v = s_note_val[code] };
        state_apply(&cmd);
        ESP_LOGD(TAG, "note off track=%u note=%u", (unsigned)s_note_track[code],
                 (unsigned)s_note_val[code]);
        return;
    }

    app_state_t st;
    state_get_copy(&st);
    const int route = input_map_key_route(st.core.selected_track, code, true);
    if (route == INPUT_ROUTE_IGNORE) return;

    if (route == INPUT_ROUTE_DRUM) {
        int8_t lane = input_map_drum_lane(code);
        if (s_drum_on[code]) return; /* 键盘不自动重复，防御重复 down */
        const edit_cmd_t cmd = { .type = CMD_DRUM_HIT, .a = (uint32_t)lane,
                                 .v = APP_STATE_VEL_DEFAULT };
        state_apply(&cmd);
        s_drum_on[code] = true;
        ESP_LOGD(TAG, "drum hit lane=%d", (int)lane);
        return;
    }

    uint8_t note = (uint8_t)(INPUT_BASE_NOTE + offset);
    if (note > INPUT_NOTE_MAX) return;
    if (s_note_on[code]) return; /* 键盘不自动重复，防御重复 down */
    uint8_t track = st.core.selected_track;
    const edit_cmd_t cmd = { .type = CMD_NOTE_ON, .a = track, .v = note };
    state_apply(&cmd);
    s_note_on[code] = true;
    s_note_val[code] = note;
    s_note_track[code] = track;
    /* state_apply 已返回：此刻不持 state/引擎锁（sloop_ui.h 的契约） */
    sloop_ui_flash_note(note);
    ESP_LOGD(TAG, "note on track=%u note=%u", (unsigned)track, (unsigned)note);
}

/* 鼓键松开：velocity 0 结束自由录音里的鼓音符段（上游 KS_DRUM 的 key-up）；不在
 * 自由录音时是无操作。 */
static void act_drum_release(uint8_t code)
{
    int8_t lane = input_map_drum_lane(code);
    s_drum_on[code] = false;
    if (lane < 0) return;
    const edit_cmd_t cmd = { .type = CMD_DRUM_HIT, .a = (uint32_t)lane, .v = 0 };
    state_apply(&cmd);
}

/* space：物理 PLAY 键（上游面板按钮路径）。自由录音进行中由引擎丢弃 take（drop）；
 * API PLAY（CMD_PLAY）仍是 close。按下/松开两沿都下发（按钮电平）。 */
static void act_play_button(bool down)
{
    if (s_play_down == down) return;
    s_play_down = down;
    const edit_cmd_t cmd = { .type = CMD_PLAY_BUTTON, .a = down ? 1u : 0u };
    state_apply(&cmd);
}

static void act_focus(bool step_axis, int8_t delta)
{
    app_state_t st;
    state_get_copy(&st);
    uint8_t track = st.focus_track < APP_STATE_TRACKS ? st.focus_track : 0u;
    uint8_t wlen = st.window.window_len;
    if (wlen < 1u) wlen = 1u;
    if (wlen > APP_STATE_STEPS) wlen = APP_STATE_STEPS;
    uint8_t step = st.focus_step < wlen ? st.focus_step : 0u;
    if (step_axis) {
        /* 焦点的步轴在窗口列内环绕（隐藏列不可聚焦，不绕到窗口外） */
        step = (uint8_t)((step + wlen + delta) % wlen);
    } else {
        /* 轨轴只在三条合成轨间环绕；当前停在鼓轨时按方向进入 2 或 0 */
        if (track >= 3u) {
            track = delta < 0 ? 2u : 0u;
        } else {
            track = (uint8_t)((track + 3u + delta) % 3u);
        }
    }
    state_set_focus(track, step);
}

/* EDIT 层：删除聚焦步里所有该音级的音符（按音级匹配，跨八度；命令按精确音符）。
 * 步号在派发时解析（命令显式携带绝对步号，状态层不读焦点）：焦点是**窗口列**，
 * 绝对步 = step_window_abs()。 */
static void act_erase(int8_t pitch_class)
{
    app_state_t st;
    state_get_copy(&st);
    uint8_t k = st.focus_track;
    uint8_t i = st.focus_step;
    if (k >= 3u || i >= st.window.window_len) return;

    const uint8_t abs = step_window_abs(&st.window, i);
    const uint8_t n = st.core.step[k][i].n > 4u ? 4u : st.core.step[k][i].n;
    if (!state_step_is_on(n, st.core.step[k][i].time)) return;
    for (uint8_t j = 0; j < n; j++) {
        uint8_t note = st.core.step[k][i].note[j];
        if (note == 0 || (int8_t)(note % 12u) != pitch_class) continue;
        const edit_cmd_t cmd = { .type = CMD_ERASE_NOTE, .a = k, .b = abs, .v = note };
        state_apply(&cmd);
        ESP_LOGD(TAG, "erase track=%u step=%u note=%u", (unsigned)k, (unsigned)abs,
                 (unsigned)note);
    }
}

static void act_layer(uint8_t bit, bool down)
{
    app_state_t st;
    state_get_copy(&st);
    uint8_t mask = down ? (uint8_t)(st.layer_held | bit) : (uint8_t)(st.layer_held & ~bit);
    if (mask == st.layer_held) return;
    state_set_layer(mask);
    ESP_LOGD(TAG, "layer %s 0x%02X", down ? "down" : "up", (unsigned)mask);
}

static void dispatch(const key_binding_t *b, bool down)
{
    switch (b->action) {
    case ACT_LAYER:
        act_layer((uint8_t)b->param, down);
        return;
    case ACT_NOTE:
        act_note(b->code, b->param, down);
        return;
    case ACT_PLAY_TOGGLE: /* 物理 PLAY：两沿都下发（按钮电平，free take 的 drop 需要） */
        act_play_button(down);
        return;
    case ACT_FILL_HOLD: { /* FILL 保持：两沿都下发 */
        const edit_cmd_t cmd = { .type = CMD_FILL_CONTROL, .a = down ? 1u : 0u };
        state_apply(&cmd);
        return;
    }
    default:
        break;
    }
    if (!down) return; /* 其余动作只在按下沿触发 */

    switch (b->action) {
    case ACT_REC: {
        const edit_cmd_t cmd = { .type = CMD_REC_TOGGLE };
        state_apply(&cmd);
        break;
    }
    case ACT_FILL_NEXT: { /* 一次按下 = 上游 GLO + key 10 的**脉冲**：只翻转 fill_arm，
                           * 不读写 fill_held（held 只由 `-` 的 down/up 写）。 */
        const edit_cmd_t cmd = { .type = CMD_FILL_CONTROL, .a = 0, .b = 1 };
        state_apply(&cmd);
        break;
    }
    case ACT_PAGE_PLAY: sloop_ui_request_page(APP_PAGE_PLAY); break; /* esc：回 PLAY（组随页复位） */
    case ACT_PAGE_NEXT: {
        /* 只记目标页：切页在 30ms UI 定时器（LVGL 上下文）执行，输入路径不等锁。
         * 目标页 = 组顺序扁平轮转的下一张（ADR-005 §4 的 18 页顺序，P4-F Task 6 起：
         * PLAY→TRACKS→SOUND→MOD→SCALE→VOICE→GLOBAL→FX→TOOLS→SAMPLE→DSYN→FM6P→STEP→
         * TRANSPORT→SONG→PROJECTS→PRESETS→SYSTEM→PLAY，跳过未注册页）。
         * 「一拍一页」：目标页按**当前页**计算、邮箱只保留最后请求——同一 tick 内
         * 连按 N 次 tab 只前进一页（不是 N 页），多页跳转请用 console `UI`。 */
        uint8_t next = sloop_ui_page_order_next(sloop_ui_current_page());
        if (next < APP_STATE_PAGES) sloop_ui_request_page(next);
        break;
    }
    case ACT_FOCUS_STEP:
        /* SYSTEM 页：←/→ 调焦点行的值；FILE 页：←/→ 选槽（PROJECTS 卡 / PRESETS 格）；
         * 参数页：←/→ 在 8 槽间移动焦点（不再驱动 STEP 焦点列）；TRANSPORT 页：←/→
         * 移动工具焦点字段；SONG 页：←/→ 移动歌曲条目 */
        if (sloop_ui_system_page_active()) {
            (void)sloop_ui_system_key(b->code);
        } else if (sloop_ui_file_page_active()) {
            sloop_ui_file_slot_move(b->param);
        } else if (sloop_ui_param_page_active()) {
            sloop_ui_param_focus_move(b->param);
        } else if (sloop_ui_dsyn_page_active()) {
            sloop_ui_dsyn_focus_move(b->param);
        } else if (sloop_ui_fm6p_page_active()) {
            sloop_ui_fm6p_focus_move(b->param);
        } else if (sloop_ui_transport_page_active()) {
            sloop_ui_transport_focus_move(b->param);
        } else if (sloop_ui_song_page_active()) {
            sloop_ui_song_entry_move(b->param);
        } else {
            act_focus(true, b->param);
        }
        break;
    case ACT_FILE_LOAD:
        /* enter：SYSTEM 页 = 焦点行 +1/切换；STEP 页 = 切换聚焦步的选中 drum lane 位；
         * FILE 页 = LOAD 选中槽；TRANSPORT 页 = 焦点字段 +1（SLIDE 为切换）；
         * SONG 页 = LOAD 选中 section；参数页 = 焦点槽 +1（P4-F Task 4；锁定槽无动作）。
         * 其它页保持无动作。 */
        if (sloop_ui_system_page_active()) {
            (void)sloop_ui_system_key(b->code);
        } else if (sloop_ui_step_page_active()) {
            sloop_ui_step_drum_toggle();
        } else if (sloop_ui_file_page_active()) {
            sloop_ui_file_load();
        } else if (sloop_ui_transport_page_active()) {
            sloop_ui_transport_adjust(1);
        } else if (sloop_ui_song_page_active()) {
            sloop_ui_song_load();
        } else if (sloop_ui_param_page_active()) {
            sloop_ui_param_adjust(1);
        }
        break;
    case ACT_FOCUS_TRACK:
        /* SYSTEM 页：↑/↓ 在八个设置行间移动焦点；参数页：↑/↓ 调整焦点槽的值
         * （up=+1；输入层只写状态层，控件由 UI 定时器刷新）；STEP 页：焦点环
         * T1,T2,T3,DRM L1..L16（鼓轨上逐 lane，ENTER 切位）；TRANSPORT 页：↑/↓
         * 调焦点工具字段（与参数页同一「焦点 + 调值」模型）；SONG 页：↑/↓ 在
         * A–D section 卡间移动 */
        if (sloop_ui_system_page_active()) {
            (void)sloop_ui_system_key(b->code);
        } else if (sloop_ui_param_page_active()) {
            sloop_ui_param_adjust((int8_t)-b->param);
        } else if (sloop_ui_dsyn_page_active()) {
            sloop_ui_dsyn_adjust((int8_t)-b->param);
        } else if (sloop_ui_fm6p_page_active()) {
            sloop_ui_fm6p_adjust((int8_t)-b->param);
        } else if (sloop_ui_transport_page_active()) {
            sloop_ui_transport_adjust((int8_t)-b->param);
        } else if (sloop_ui_step_page_active()) {
            sloop_ui_step_focus_vertical(b->param);
        } else if (sloop_ui_song_page_active()) {
            sloop_ui_song_section_move(b->param);
        } else {
            act_focus(false, b->param);
        }
        break;
    case ACT_SELECT: {
        /* SEL 层 1–4 = 选轨（与 TRACKS 卡片点击同一命令，同时设焦点轨） */
        const edit_cmd_t cmd = { .type = CMD_SELECT_TRACK, .a = (uint32_t)b->param };
        state_apply(&cmd);
        break;
    }
    case ACT_MUTE:
    case ACT_SOLO: {
        const edit_cmd_t cmd = { .type = b->action == ACT_MUTE ? CMD_MUTE : CMD_SOLO,
                                 .a = (uint32_t)b->param };
        state_apply(&cmd);
        break;
    }
    case ACT_ERASE: act_erase(b->param); break;
    /* EDIT 层步工具（STEP 页聚焦步的参数锁 / fill 条件；非 STEP 页为空操作） */
    case ACT_STEP_LOCK: sloop_ui_step_tool(SLOOP_STEP_TOOL_LOCK); break;
    case ACT_STEP_UNLOCK: sloop_ui_step_tool(SLOOP_STEP_TOOL_UNLOCK); break;
    case ACT_STEP_FILL: sloop_ui_step_tool(SLOOP_STEP_TOOL_FILL); break;
    default: break;
    }
}

void sloop_ui_input_key(uint8_t code, bool down)
{
    if (code > 0x7F) return;
    /* 诊断期：丢弃并计数（不派发 = 不碰 state/引擎锁）。批内竞态由 input 任务在
     * 批次边界确认，见 input_quiesce_begin()。 */
    if (s_quiesce) {
        atomic_fetch_add(&s_quiesce_dropped, 1u);
        return;
    }
    /* 自动存档的「无输入 ≥2.5s」计时（上游 ui_input_ms）：物理键与 console 注入同入口，
     * 按下与松开都算活动。只写时间戳，不取锁。 */
    sloop_store_note_activity((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    input_lock();
    if (s_quiesce) { /* 等锁期间进入静默（input 任务/另一注入者持有互斥） */
        atomic_fetch_add(&s_quiesce_dropped, 1u);
        input_unlock();
        return;
    }
    /* 释放先于层路由：按住演奏键期间再按层键（如 c 尚未松开就按 Aa）时，松开必须
     * 仍然按「演奏键释放」处理，否则会被 EDIT 层的同名条目吞掉、音符挂住。
     * 鼓键同样先处理松开（自由录音的鼓音符段以 velocity 0 结束）。 */
    if (!down && s_drum_on[code]) {
        act_drum_release(code);
        input_unlock();
        return;
    }
    if (!down && s_note_on[code]) {
        act_note(code, 0, false);
        input_unlock();
        return;
    }
    app_state_t st;
    state_get_copy(&st);
    const key_binding_t *b = input_map_find_binding(code, st.layer_held);
    /* EDIT/SEL/GLO 按住时演奏键不发声：否则 Aa+c（擦除 C）与 Aa+z（弹 C）行为不一致，
     * 且层内的误触会带出音符。FX 是 MVP 无功能层，保持演奏透明。 */
    if (b != NULL && b->action == ACT_NOTE && b->layer == 0 &&
        (st.layer_held & (APP_LAYER_EDIT | APP_LAYER_SEL | APP_LAYER_GLO)) != 0) {
        b = NULL;
    }
    if (b != NULL) {
        dispatch(b, down);
    } else if (down) {
        ESP_LOGD(TAG, "未映射键 0x%02X (%s)", (unsigned)code, tab5_kb_key_name(code));
    }
    input_unlock();
}

void sloop_ui_input_panic(void)
{
    input_lock();
    unsigned released = 0;
    for (unsigned code = 0; code < 128u; code++) {
        if (!s_note_on[code]) continue;
        s_note_on[code] = false;
        const edit_cmd_t cmd = { .type = CMD_NOTE_OFF, .a = s_note_track[code],
                                 .v = s_note_val[code] };
        state_apply(&cmd);
        released++;
    }
    for (unsigned code = 0; code < 128u; code++) {
        if (!s_drum_on[code]) continue;
        act_drum_release((uint8_t)code); /* 自由录音里的鼓音符段也收口 */
    }
    if (s_play_down) act_play_button(false); /* 物理 PLAY 位不得挂住（否则下次 take 没有 drop 沿） */
    state_set_layer(0);
    sloop_ui_cancel_pending_page(); /* 撤销待执行切页（专用清空，无读当前页竞态） */
    input_unlock();
    ESP_LOGW(TAG, "PANIC 释放 %u 个音符，层键/待切页已清", released);
}

static void input_task(void *arg)
{
    (void)arg;
    tab5_kb_event_t ev[INPUT_BATCH];
    while (1) {
        int n = tab5_kb_poll(ev, INPUT_BATCH);
        if (s_quiesce) {
            atomic_fetch_add(&s_quiesce_dropped, (unsigned)n); /* 静默期：排空队列、不派发 */
            s_quiesce_ack++;                  /* 已回到批次边界（不持任何锁/不占 I2C） */
            /* 阻塞等恢复通知（input_quiesce_end）：诊断期不再按轮询周期 yield */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            /* 静默期积压的事件一律丢弃、不补发：诊断期 UI 已暂停，补发可能只有
             * note-on 而 note-off 已被驱动 FIFO 覆盖 → 挂音 */
            while ((n = tab5_kb_poll(ev, INPUT_BATCH)) > 0) {
                atomic_fetch_add(&s_quiesce_dropped, (unsigned)n);
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            sloop_ui_input_key(ev[i].code, ev[i].pressed);
        }
        vTaskDelay(INPUT_POLL_TICKS);
    }
}

esp_err_t input_start(void)
{
    if (s_started) return ESP_OK; /* 幂等：sloop_ui_start 重入/重复调用不建第二个任务 */
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "创建 InputManager 互斥失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (xTaskCreate(input_task, "sloop_input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO, &s_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "创建输入任务失败");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    /* 打印生效周期（不是请求的 INPUT_POLL_MS）：周期以 tick 为单位，100Hz 下
     * 1 tick = 10ms，请求的 5ms 表达不出来（INPUT_POLL_MS 只是意图） */
    const unsigned period_ms = (unsigned)(INPUT_POLL_TICKS * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "InputManager 就绪：%u 条映射，轮询 %ums（%u tick @%uHz），任务优先级 %u",
             (unsigned)INPUT_MAP_BINDING_COUNT, period_ms, (unsigned)INPUT_POLL_TICKS,
             (unsigned)configTICK_RATE_HZ, (unsigned)INPUT_TASK_PRIO);
    return ESP_OK;
}

uint32_t sloop_ui_input_stack_free(void)
{
    return s_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

bool input_quiesce_begin(uint32_t timeout_ms)
{
    if (s_task == NULL) {
        s_quiesce = true; /* 无 input 任务（未启动/创建失败）：仅拦 console 注入 */
        return true;
    }
    /* base 必须在置位前取：置位之后 input 任务才可能自增 ack */
    uint32_t base = s_quiesce_ack;
    s_quiesce = true;
    TickType_t remaining = pdMS_TO_TICKS(timeout_ms);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);
    while (s_quiesce_ack == base) {
        if (xTaskCheckForTimeOut(&timeout, &remaining) != pdFALSE) return false;
        vTaskDelay(1);
    }
    return true;
}

uint32_t input_quiesce_end(void)
{
    s_quiesce = false;
    s_quiesce_ack = 0;
    if (s_task != NULL) xTaskNotifyGive(s_task); /* 唤醒静默期阻塞的 input 任务 */
    uint32_t dropped = atomic_exchange(&s_quiesce_dropped, 0u); /* 恢复后清零（本轮量才有意义） */
    return dropped;
}

uint32_t input_quiesce_dropped(void)
{
    return atomic_load(&s_quiesce_dropped);
}
