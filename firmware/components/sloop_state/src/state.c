/* sloop_state：AppState 容器、互斥、快照同步与复制（cmd.c 负责 EditCmd 分发）。 */
#include "state_internal.h"

#include <stdbool.h>
#include <string.h>

#ifdef HOST_TEST
/* host 单测单线程：互斥为空实现；设备上用 FreeRTOS mutex（见下）。 */
void state_lock(void) {}
void state_unlock(void) {}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;

void state_lock(void)
{
    if (s_mutex != NULL) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void state_unlock(void)
{
    if (s_mutex != NULL) xSemaphoreGive(s_mutex);
}
#endif

static app_state_t s_state;
static bool s_inited;

app_state_t *state_locked(void)
{
    return &s_state;
}

void state_init(void)
{
#ifndef HOST_TEST
    /* 设备上只算一次：重复调用不得清掉 UI 正在用的 AppState（只补建互斥）。 */
    if (s_inited) return;
    if (s_mutex == NULL) s_mutex = xSemaphoreCreateMutex();
#else
    /* host 单测每个用例都要一份干净的 AppState，重复调用等价于重置。 */
#endif
    memset(&s_state, 0, sizeof(s_state));
    sloop_core_read(&s_state.core);
    /* 步窗默认 {0,0,16}：memset 后 len 为 0，必须显式给出（否则窗口为空） */
    s_state.window.bank = 0;
    s_state.window.window_start = 0;
    s_state.window.window_len = APP_STATE_STEPS;
    s_inited = true;
}

void state_get_copy(app_state_t *out)
{
    if (out == NULL) return;
    state_lock();
    *out = s_state;
    state_unlock();
}

void step_window(step_window_t *out)
{
    if (out == NULL) return;
    state_lock();
    *out = s_state.window;
    state_unlock();
}

void state_sync_from_engine(void)
{
    /* 步窗是 UI 局部态（state 所有）。sync 在状态锁外读引擎（避免把输入任务堵在
     * 音频块的引擎锁后面），因此取快照与写回之间可能插入一次 CMD_SET_WINDOW：
     * 写回前复核窗口，变了就按新窗口重取一次（最多两次）——否则会把旧基的投影
     * 留在 AppState 一拍，而 EDIT 擦除读的是这份投影。 */
    for (uint32_t attempt = 0; attempt < 2u; attempt++) {
        step_window_t win;
        state_lock();
        win = s_state.window;
        state_unlock();

        sloop_core_snapshot_t snap;
        /* snapshot 的 step[][]/dstep[] 是窗口列投影（基 = win.window_start）。
         * 窗口快照在同一把引擎锁里取完 16 列（不是逐列 read_step），取锁次数仍为 1。 */
        sloop_core_read_window(win.window_start, &snap);

        state_lock();
        if (s_state.window.window_start == win.window_start) {
            /* 全部字段取引擎真值：选轨/mute/solo 已由引擎写入口落地（sloop_core_select/
             * set_mute/set_solo），本层不再持有，也不做任何例外保持。窗口本身不被 sync
             * 改写；窗口基与快照基一致才写回（否则按新窗口重取）。 */
            s_state.core = snap;
            state_unlock();
            return;
        }
        state_unlock();
    }
    /* 连续两次窗口变更（极端交错）：本次不写投影，下一拍按最新窗口重取并自愈 */
}

void state_set_page(uint8_t page)
{
    if (page >= APP_STATE_PAGES) return;
    state_lock();
    s_state.page = page;
    state_unlock();
}

void state_set_focus(uint8_t track, uint8_t step)
{
    state_lock();
    if (track < APP_STATE_TRACKS) s_state.focus_track = track;
    /* 焦点步是窗口列：窗口外的隐藏列不可聚焦（窗口缩小后旧焦点已由 SET_WINDOW 夹住） */
    if (step < s_state.window.window_len) s_state.focus_step = step;
    state_unlock();
}

void state_set_layer(uint8_t mask)
{
    state_lock();
    s_state.layer_held = mask;
    state_unlock();
}
