/* sloop_state：AppState 容器、互斥、快照同步与复制（cmd.c 负责 EditCmd 分发）。 */
#include "state_internal.h"

#include <stdbool.h>
#include <string.h>

#include "sloop_store.h" /* sloop_store_note_activity（自动存档的活动计时） */
#include "sloop_hal.h"  /* 平台设置投影（sloop_hal_settings_* 的 getter） */

#ifdef HOST_TEST
/* host 单测单线程：互斥为空实现；设备上用 FreeRTOS mutex（见下）。 */
void state_lock(void) {}
void state_unlock(void) {}
#else
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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

uint32_t state_now_ms(void)
{
#ifdef HOST_TEST
    return 0; /* host 单测没有自动存档（stub_engine.c 的 note_activity 空实现） */
#else
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
#endif
}

static app_state_t s_state;
static bool s_inited;

app_state_t *state_locked(void)
{
    return &s_state;
}

/* ---- P4-D Task 6：DSYN / FM6 patch 投影（调用方持 state_lock） ------------------
 * dsyn 按**当前选择**读：kit/lane 是 UI 局部态（与 step_window 同类），不是引擎真值；
 * dirty 取存储层的 DSU 记账（DSYN 编辑置位、任何成功的 settings 写清）。读失败（诊断
 * 冻结）时保留上一次的 22B/名字，dirty 仍如实刷新。
 * fm6p 按选中轨读：非 FM6 **合成**轨回 NOT_ALLOWED 且快照带 engine_ok=0——保留
 * patch/slot（UI 显示锁定）；鼓轨（TRK_DRUM = 3、NPART = 3 ⇒ 越界）与越界轨回
 * INVALID_ARG、诊断冻结回 INVALID_STATE，这两种情况把快照清零（没有可显示的
 * live patch）。 */
void state_refresh_dsyn_locked(void)
{
    sloop_dsyn_sound_t snd;
    char name[SLOOP_DSYN_NAME + 1];
    uint8_t crush = 0, src = 0;
    if (sloop_core_dsyn_read(s_state.dsyn.kit, s_state.dsyn.lane, &snd, name, &crush, &src) ==
        ESP_OK) {
        memcpy(s_state.dsyn.sound, snd.sound, sizeof s_state.dsyn.sound);
        memcpy(s_state.dsyn.name, name, sizeof s_state.dsyn.name);
        s_state.dsyn.crush = crush;
        s_state.dsyn.src = src;
    }
    s_state.dsyn.dirty = sloop_store_dsu_dirty() ? 1u : 0u;
}

void state_refresh_fm6_locked(void)
{
    sloop_fm6_patch_snapshot_t snap;
    esp_err_t ret = sloop_core_fm6_patch(s_state.core.selected_track, &snap);
    if (ret == ESP_OK || ret == ESP_ERR_NOT_ALLOWED) {
        s_state.fm6p = snap;
    } else {
        memset(&s_state.fm6p, 0, sizeof s_state.fm6p);
    }
}

/* ---- P4-E Task 4：settings 生效副本 / 平台设置投影（调用方持 state_lock）-------------
 * 两个来源都是无锁的 RAM 读（store 的 s_set 不取锁、HAL 的 getter 读字节），不引入新的
 * 锁序。在状态锁内读：与 state_apply 内的 CMD_SETTINGS_SET / CMD_SYS_* 串行化，sync 不会
 * 把这些命令之前的旧值写回投影；console 直调（STORE SETTINGS SET / DISP BRIGHT）不经过
 * 状态锁，差的一拍由下一拍 sync 自愈。 */
void state_refresh_settings_locked(void)
{
    const sloop_settings_t *st = sloop_store_settings();
    s_state.settings.palette = (uint8_t)st->f.palette;
    s_state.settings.lowcut = (uint8_t)st->f.lowcut;
    s_state.settings.zoom = (uint8_t)st->f.zoom;
    s_state.settings.lights = (uint8_t)st->f.lights_lvl;
    s_state.settings.theme_on = (uint8_t)st->f.theme_on;
    s_state.settings.theme_valid = st->theme_valid ? 1u : 0u;
    s_state.settings.reso = (uint8_t)st->f.flt_reso;
    s_state.settings.dirty = st->dirty ? 1u : 0u;
    memcpy(s_state.settings.theme_name, st->theme_name, sizeof s_state.settings.theme_name);
}

void state_refresh_sound_locked(void)
{
    sloop_core_sound_read(s_state.core.selected_track, &s_state.sound);
}

void state_refresh_sys_locked(void)
{
    s_state.sys.bright = sloop_hal_settings_bright();
    s_state.sys.vol = sloop_hal_settings_volume();
    s_state.sys.out = sloop_hal_settings_output();
    s_state.sys.nvs_dirty = sloop_hal_settings_nvs_dirty() ? 1u : 0u;
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
    s_state.focus_track = s_state.core.selected_track;
    /* 参数投影与 core 同拍初化（p = 选中轨，g = 全局） */
    sloop_core_read_params(s_state.core.selected_track, s_state.p, s_state.g);
    /* 录音投影同拍初化（一次性 take 结果在开机时必为空） */
    sloop_seq_rec_read(&s_state.rec);
    /* arranger 只读投影（SONG 页/TRANSPORT 状态行） */
    sloop_seq_song_read(&s_state.song);
    /* DSYN / FM6 patch 投影（选择从 0/0 开始；dirty 取存储层记账） */
    state_refresh_dsyn_locked();
    state_refresh_fm6_locked();
    /* settings 生效副本 / 平台设置投影（开机默认或上一次落盘的值） */
    state_refresh_settings_locked();
    state_refresh_sys_locked();
    /* SOUND 页投影（选中轨的引擎/预置/kit 名） */
    state_refresh_sound_locked();
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
    /* 步窗与 DSYN 选择都是 UI 局部态（state 所有）。sync 在状态锁外读引擎（避免把输入
     * 任务堵在音频块的引擎锁后面），因此取快照与写回之间可能插入一次 CMD_SET_WINDOW /
     * state_set_dsyn_select：写回前复核两者，变了就按新值重取一次（最多两次）——否则会把
     * 旧基的投影留在 AppState 一拍，而 EDIT 擦除/DSYN 旋钮读的是这份投影。 */
    for (uint32_t attempt = 0; attempt < 2u; attempt++) {
        step_window_t win;
        uint8_t dsyn_kit, dsyn_lane;
        state_lock();
        win = s_state.window;
        dsyn_kit = s_state.dsyn.kit;
        dsyn_lane = s_state.dsyn.lane;
        state_unlock();

        sloop_core_snapshot_t snap;
        /* snapshot 的 step[][]/dstep[] 是窗口列投影（基 = win.window_start）。
         * 窗口快照在同一把引擎锁里取完 16 列（不是逐列 read_step），取锁次数仍为 1。 */
        sloop_core_read_window(win.window_start, &snap);
        /* 参数投影同拍取（第二次短锁）：p 的轨别取快照里的选中轨，与 core 一致；
         * 不逐项读 71+50 个参数。写回前与窗口一起复核。
         * 注意这是**多个快照**（窗口 + 参数 + DSYN + FM6 patch）而非一个原子快照：各次
         * 取锁之间若发生选轨/参数写/外部改动，本次写回的字段可能差一拍（≤1 个 sync
         * 周期，下一拍自愈）。与既有 core 快照语义一致；选中轨切换本身在 cmd_select_track
         * 里已即时刷新 p[]，不会出现「标题/参数跨轨」的帧。 */
        int16_t param_p[SLOOP_CORE_P_COUNT];
        int16_t param_g[SLOOP_CORE_G_COUNT];
        sloop_core_read_params(snap.selected_track, param_p, param_g);

        /* DSYN 按选择读、FM6 patch 按选中轨读（spec §4.3：每拍各一次）。 */
        sloop_dsyn_sound_t dsyn_snd;
        char dsyn_name[SLOOP_DSYN_NAME + 1];
        uint8_t dsyn_crush = 0, dsyn_src = 0;
        bool dsyn_ok = sloop_core_dsyn_read(dsyn_kit, dsyn_lane, &dsyn_snd, dsyn_name,
                                            &dsyn_crush, &dsyn_src) == ESP_OK;
        bool dsu_dirty = sloop_store_dsu_dirty();
        sloop_fm6_patch_snapshot_t fm6_snap;
        esp_err_t fm6_ret = sloop_core_fm6_patch(snap.selected_track, &fm6_snap);
        sloop_sound_snapshot_t sound;
        sloop_core_sound_read(snap.selected_track, &sound);

        state_lock();
        if (s_state.window.window_start == win.window_start &&
            s_state.dsyn.kit == dsyn_kit && s_state.dsyn.lane == dsyn_lane) {
            /* 全部字段取引擎真值：选轨/mute/solo 已由引擎写入口落地（sloop_core_select/
             * set_mute/set_solo），本层不再持有，也不做任何例外保持。窗口本身不被 sync
             * 改写；窗口基与快照基一致才写回（否则按新窗口重取）。 */
            if (snap.selected_track != s_state.core.selected_track)
                s_state.focus_track = snap.selected_track;
            s_state.core = snap;
            memcpy(s_state.p, param_p, sizeof(param_p));
            memcpy(s_state.g, param_g, sizeof(param_g));
            if (dsyn_ok) {
                memcpy(s_state.dsyn.sound, dsyn_snd.sound, sizeof s_state.dsyn.sound);
                memcpy(s_state.dsyn.name, dsyn_name, sizeof s_state.dsyn.name);
                s_state.dsyn.crush = dsyn_crush;
                s_state.dsyn.src = dsyn_src;
            }
            s_state.dsyn.dirty = dsu_dirty ? 1u : 0u;
            if (fm6_ret == ESP_OK || fm6_ret == ESP_ERR_NOT_ALLOWED) {
                s_state.fm6p = fm6_snap;
            } else {
                memset(&s_state.fm6p, 0, sizeof s_state.fm6p);
            }
            s_state.sound = sound;
            /* 录音快照只在写回成功的这一拍读一次：take 结果是一次性的（读即消费），
             * 重试循环里读会把结果丢在第一次 attempt 上。 */
            sloop_seq_rec_read(&s_state.rec);
            sloop_seq_song_read(&s_state.song);
            /* settings dirty 由 1s 任务清、NVS 由同一静默窗口提交：每拍重取才能把
             * 「已落盘」（set_dirty / set_nvs 归 0）反映给 UI/console。 */
            state_refresh_settings_locked();
            state_refresh_sys_locked();
            state_unlock();
            return;
        }
        state_unlock();
    }
    /* 连续两次窗口/选择变更（极端交错）：本次不写投影，下一拍按最新值重取并自愈 */
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
    /* 只移焦点（不发声、不改引擎）也是一次面板动作：上游按「面板被碰过」计空闲
     * （ui_input_ms），否则边看边移焦点时自动存档会在用户眼前写 flash。 */
    sloop_store_note_activity(state_now_ms());
}

void state_set_layer(uint8_t mask)
{
    state_lock();
    s_state.layer_held = mask;
    state_unlock();
}

void state_set_dsyn_select(uint8_t kit, uint8_t lane)
{
    state_lock();
    s_state.dsyn.kit = kit < SLOOP_DSYN_KITS ? kit : (uint8_t)(SLOOP_DSYN_KITS - 1u);
    s_state.dsyn.lane = lane < SLOOP_DSYN_LANES ? lane : (uint8_t)(SLOOP_DSYN_LANES - 1u);
    state_refresh_dsyn_locked();
    state_unlock();
}

/* 窄读访问器（契约见 sloop_state.h）：各自在状态锁内读一个字段/一小块，不拷整份 AppState。 */
uint8_t state_selected_track(void)
{
    state_lock();
    uint8_t track = s_state.core.selected_track;
    state_unlock();
    return track;
}

uint8_t state_selected_engine(void)
{
    state_lock();
    uint8_t track = s_state.core.selected_track;
    uint8_t engine = track < APP_STATE_TRACKS - 1u ? s_state.core.trk[track].engine : 0u;
    state_unlock();
    return engine;
}

int16_t state_read_p(uint8_t id)
{
    state_lock();
    int16_t v = id < SLOOP_CORE_P_COUNT ? s_state.p[id] : 0;
    state_unlock();
    return v;
}

int16_t state_read_g(uint8_t id)
{
    state_lock();
    int16_t v = id < SLOOP_CORE_G_COUNT ? s_state.g[id] : 0;
    state_unlock();
    return v;
}

void state_read_sound(sloop_sound_snapshot_t *out)
{
    if (out == NULL) return;
    state_lock();
    *out = s_state.sound;
    state_unlock();
}

uint8_t state_sound_status(void)
{
    state_lock();
    uint8_t st = s_state.sound_status;
    state_unlock();
    return st;
}

bool state_take_selftest_request(void)
{
    state_lock();
    bool pending = s_state.selftest_req != 0;
    s_state.selftest_req = 0;
    state_unlock();
    return pending;
}

void state_set_selftest_result(uint32_t ran, uint32_t failed)
{
    state_lock();
    s_state.selftest_ran = ran;
    s_state.selftest_failed = failed;
    s_state.selftest_done = 1;
    s_state.settings_status = APP_SETTINGS_OK;
    state_unlock();
}

void state_set_selftest_available(bool available)
{
    state_lock();
    s_state.selftest_unavailable = available ? 0 : 1;
    state_unlock();
}

void state_refresh_locked(void)
{
    /* 调用方持 state_lock：窗口在此期间不会被 CMD_SET_WINDOW 改，故不需要 sync 的
     * 窗口复核循环。取快照的锁序与编辑命令一致（state → engine）；引擎锁只包住
     * 快照读取，存储层从不持有引擎锁做 flash 擦写（ADR-009 §3）。 */
    sloop_core_snapshot_t snap;
    int16_t param_p[SLOOP_CORE_P_COUNT];
    int16_t param_g[SLOOP_CORE_G_COUNT];
    sloop_core_read_window(s_state.window.window_start, &snap);
    sloop_core_read_params(snap.selected_track, param_p, param_g);
    if (snap.selected_track != s_state.core.selected_track)
        s_state.focus_track = snap.selected_track;
    s_state.core = snap;
    memcpy(s_state.p, param_p, sizeof(param_p));
    memcpy(s_state.g, param_g, sizeof(param_g));
    state_refresh_dsyn_locked();
    state_refresh_fm6_locked();
    sloop_seq_rec_read(&s_state.rec);
    sloop_seq_song_read(&s_state.song);
    state_refresh_settings_locked();
    state_refresh_sys_locked();
    state_refresh_sound_locked();
}
