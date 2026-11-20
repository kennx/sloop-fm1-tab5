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
    s_inited = true;
}

void state_get_copy(app_state_t *out)
{
    if (out == NULL) return;
    state_lock();
    *out = s_state;
    state_unlock();
}

void state_sync_from_engine(void)
{
    sloop_core_snapshot_t snap;
    /* 先在状态锁外读引擎：若恰与 state_apply 交错，本次写入的投影最多被一份旧快照
     * 覆盖一个 UI tick（30–60ms），下一次 sync 即自愈。反过来先取状态锁会把输入任务
     * 堵在音频块持有的引擎锁后面（≈5.8ms/块），直接伤 ≤20ms 的按键延迟目标。 */
    sloop_core_read(&snap);

    state_lock();
    /* 选轨与逐轨 mute/solo 暂无引擎写入口（sloop_core 只落了快照/步/全局写），
     * 由本层持有，sync 不覆盖；其余字段取引擎真值（见 sloop_state.h 的 MVP 约定）。 */
    uint8_t selected = s_state.core.selected_track;
    uint8_t mute[APP_STATE_TRACKS];
    uint8_t solo[APP_STATE_TRACKS];
    for (uint32_t k = 0; k < APP_STATE_TRACKS; k++) {
        mute[k] = s_state.core.trk[k].mute;
        solo[k] = s_state.core.trk[k].solo;
    }
    s_state.core = snap;
    s_state.core.selected_track = selected;
    for (uint32_t k = 0; k < APP_STATE_TRACKS; k++) {
        s_state.core.trk[k].mute = mute[k];
        s_state.core.trk[k].solo = solo[k];
    }
    state_unlock();
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
    if (step < APP_STATE_STEPS) s_state.focus_step = step;
    state_unlock();
}

void state_set_layer(uint8_t mask)
{
    state_lock();
    s_state.layer_held = mask;
    state_unlock();
}
