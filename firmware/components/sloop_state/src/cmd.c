/* sloop_state 的 EditCmd 分发：执行端 = 本地引擎（sloop_core）。
 * 阶段 5 联动模式替换本文件的分发目标（FM-1 会话），AppState 与接口不变。 */
#include "state_internal.h"

#include <string.h>

#include "sloop_store.h" /* sloop_store_note_activity（自动存档的活动计时） */

#ifdef HOST_TEST
#define state_warn(...) ((void)0) /* host 单测无日志后端（告警路径由设备证据覆盖） */
#else
#include "esp_log.h"
static const char *TAG = "sloop_state";
#define state_warn(...) ESP_LOGW(TAG, __VA_ARGS__)
#endif

/* 上游常量镜像（本组件不包含上游 core.h：host 单测不编译上游源码）：
 * core.h 的 enum { G_BPM, ... } → G_BPM = 0；params.c 的 GP[G_BPM] 范围 40..240；
 * core.h 的 ST_NOTE = 0；NSTEP = 步表长度 64。 */
#define STATE_G_BPM 0u
#define STATE_BPM_MIN 40
#define STATE_BPM_MAX 240
#define STATE_ST_NOTE 0u
#define STATE_STEPS_MAX 64u
#define STATE_WINDOW_STEP 16u   /* 一 bank 的步数 = window_start 对齐粒度 */
#define STATE_WINDOW_START_MAX (STATE_STEPS_MAX - STATE_WINDOW_STEP) /* 48 */
#define STATE_SYNTH_TRACKS 3u /* 兼容 toggle/erase 命令只覆盖合成轨 */
#define STATE_NOTE_MAX 127u

static void cmd_note(app_state_t *s, uint32_t track, uint16_t note, bool on)
{
    if (track >= APP_STATE_TRACKS || note > STATE_NOTE_MAX) return;
    sloop_core_note(track, note, on);
    /* last_note 用 0 表示「还没弹过」：note 0 照常发声，但不记忆（见 sloop_state.h） */
    if (on && note != 0) s->last_note = (uint8_t)note;
}

static void cmd_play(app_state_t *s, bool on)
{
    sloop_core_play(on);
    s->core.playing = on;
}

/* 参数投影回读：p[] 恒为选中轨、g[] 为全局（一次引擎锁，参数命令落地后立即对齐
 * 引擎真值——含引擎钳位）。写非选中轨时 p[] 仍读选中轨，因此不会被别人的值污染。 */
static void state_refresh_params(app_state_t *s)
{
    sloop_core_read_params(s->core.selected_track, s->p, s->g);
}

static void cmd_select_track(app_state_t *s, uint32_t track)
{
    if (track >= APP_STATE_TRACKS) return;
    sloop_core_select(track);
    s->core.selected_track = (uint8_t)track;
    s->focus_track = (uint8_t)track;
    /* 选轨即刷参数投影：p[] 是选中轨的参数，若等下一拍 sync，切页/`STATE SNAP`
     * 可能落在「标题=新轨、参数=旧轨」的一帧里（参数页读 p[]）。同锁内回读，
     * 与其它编辑写路径一样是 state 锁 → 引擎锁的顺序。 */
    state_refresh_params(s);
}

static void state_seq_result(app_state_t *s, esp_err_t ret)
{
    switch (ret) {
    case ESP_OK: s->seq_status = SEQ_STATUS_OK; break;
    case ESP_ERR_NOT_FOUND: s->seq_status = SEQ_STATUS_NOT_FOUND; break;
    case ESP_ERR_INVALID_ARG: s->seq_status = SEQ_STATUS_INVALID; break;
    case ESP_ERR_NO_MEM: s->seq_status = SEQ_STATUS_FULL; break;
    case ESP_ERR_NOT_ALLOWED: s->seq_status = SEQ_STATUS_BLOCKED; break;
    case ESP_ERR_INVALID_STATE: s->seq_status = SEQ_STATUS_FROZEN; break;
    default: s->seq_status = SEQ_STATUS_ERROR; break;
    }
    if (ret != ESP_OK) return;
    uint8_t old_selected = s->core.selected_track;
    sloop_core_read_window(s->window.window_start, &s->core);
    if (s->core.selected_track != old_selected) s->focus_track = s->core.selected_track;
}

static void cmd_toggle_step(app_state_t *s, uint32_t track, uint32_t idx)
{
    if (track >= STATE_SYNTH_TRACKS || idx >= STATE_STEPS_MAX) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }

    sloop_step_t st;
    sloop_core_read_step(track, idx, &st);
    bool on = state_step_is_on(st.n, st.time);
    if (!on) {
        memset(st.note, 0, sizeof(st.note));
        st.note[0] = s->last_note != 0 ? s->last_note : APP_STATE_NOTE_DEFAULT;
        st.n = 1;
        st.vel = APP_STATE_VEL_DEFAULT;
        st.time = STATE_ST_NOTE;
        st.flags = 0;
        st.lvl = 0;
        st.rat = 0;
        sloop_core_write_step(track, idx, &st);
        state_seq_result(s, ESP_OK);
        return;
    }
    const sloop_step_edit_t edit = {
        .track = (uint8_t)track, .step = (uint8_t)idx, .op = SEQ_STEP_CLEAR,
    };
    state_seq_result(s, sloop_seq_step_edit(&edit));
}

static void cmd_erase_note(app_state_t *s, uint32_t track, uint32_t idx, uint16_t note)
{
    if (track >= STATE_SYNTH_TRACKS || idx >= STATE_STEPS_MAX || note == 0 || note > STATE_NOTE_MAX) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }

    sloop_step_t st;
    sloop_core_read_step(track, idx, &st);
    uint32_t n = st.n > 4u ? 4u : st.n;
    uint32_t index = 0;
    while (index < n && st.note[index] != note) index++;
    if (index == n) {
        s->seq_status = SEQ_STATUS_NOT_FOUND;
        return;
    }
    const sloop_step_edit_t edit = {
        .track = (uint8_t)track, .step = (uint8_t)idx,
        .index = (uint8_t)index, .op = SEQ_STEP_NOTE_DEL,
    };
    state_seq_result(s, sloop_seq_step_edit(&edit));
}

static void cmd_set_bpm(app_state_t *s, uint16_t bpm)
{
    int32_t v = bpm;
    if (v < STATE_BPM_MIN) v = STATE_BPM_MIN;
    if (v > STATE_BPM_MAX) v = STATE_BPM_MAX;
    sloop_core_set_global(STATE_G_BPM, (int16_t)v);
    s->core.bpm = (uint16_t)v;
    s->g[STATE_G_BPM] = (int16_t)v; /* 全局投影同源（与 core.bpm 同一钳位） */
}

/* 步窗（UI 局部态）：引擎不感知。start 向下对齐 16 并钳位到 0..48，bank 由
 * start 派生（a 字段冗余）；len 钳位 1..APP_STATE_STEPS；focus_step 夹到窗口内。
 * 落窗后立即按新起点重取一份窗口快照——下一拍 sync 之前 STATE SNAP/TAP 看到的
 * 投影就已是新窗口列（否则最多一个 tick 显示旧 bank 的格）。 */
static void cmd_set_window(app_state_t *s, uint32_t start, uint32_t len)
{
    uint32_t st = start & ~(STATE_WINDOW_STEP - 1u);
    if (st > STATE_WINDOW_START_MAX) st = STATE_WINDOW_START_MAX;
    if (len < 1u) len = 1u;
    if (len > APP_STATE_STEPS) len = APP_STATE_STEPS;

    s->window.bank = (uint8_t)(st / STATE_WINDOW_STEP);
    s->window.window_start = (uint8_t)st;
    s->window.window_len = (uint8_t)len;
    if (s->focus_step >= len) s->focus_step = (uint8_t)(len - 1u);
    uint8_t old_selected = s->core.selected_track;
    sloop_core_read_window(st, &s->core);
    if (s->core.selected_track != old_selected) s->focus_track = s->core.selected_track;
}

static void cmd_track_mix(app_state_t *s, edit_cmd_type_t type, uint32_t track)
{
    if (track >= APP_STATE_TRACKS) return;
    if (type == CMD_MUTE) {
        /* 翻转基于投影（= 上一次引擎真值），写进引擎后同写投影 */
        bool mute = s->core.trk[track].mute == 0;
        sloop_core_set_mute(track, mute);
        s->core.trk[track].mute = mute ? 1u : 0u;
        return;
    }
    /* solo 是整轨掩码：由投影的 4 位重组成当前掩码，翻转本轨后整掩码写回 */
    uint8_t mask = 0;
    for (uint32_t k = 0; k < APP_STATE_TRACKS; k++) {
        if (s->core.trk[k].solo != 0) mask |= (uint8_t)(1u << k);
    }
    mask ^= (uint8_t)(1u << track);
    sloop_core_set_solo(mask);
    for (uint32_t k = 0; k < APP_STATE_TRACKS; k++) s->core.trk[k].solo = (mask >> k) & 1u;
}

/* 单值参数：以批量入口 N=1 下发（钳位与越界判定只在引擎一处）。v 是 uint16——
 * 双极参数按 16 位补码解读（见 sloop_state.h 的 CMD_SET_PARAM 契约）。 */
static void cmd_set_param(app_state_t *s, uint32_t track, uint32_t id, uint16_t value)
{
    if (track >= APP_STATE_TRACKS) return;
    if (id > 0xFFu) { /* 引擎批量 API 的 id 是 uint8：无法表达，忽略（不写） */
        state_warn("参数 id=%u 超出 uint8，忽略（track=%u）", (unsigned)id, (unsigned)track);
        return;
    }
    uint8_t pid = (uint8_t)id;
    int16_t v = (int16_t)value;
    sloop_core_set_params(track, &pid, &v, 1u);
    state_refresh_params(s);
}

/* 批量参数：解包 POD 到两个栈数组（引擎 API 是 ids/values 双数组），一次引擎调用
 * 下发；超过 APP_STATE_PARAM_BATCH_MAX 的尾部丢弃并告警。载荷指针只在本次 apply
 * 内使用（同步 apply 语义，见 sloop_state.h）。 */
static void cmd_set_param_batch(app_state_t *s, uint32_t track, const void *p, uint32_t n)
{
    if (track >= APP_STATE_TRACKS || p == NULL || n == 0u) return;
    if (n > APP_STATE_PARAM_BATCH_MAX) {
        state_warn("参数批量 %u 对超过上限 %u，只应用前 %u 对", (unsigned)n,
                   (unsigned)APP_STATE_PARAM_BATCH_MAX, (unsigned)APP_STATE_PARAM_BATCH_MAX);
        n = APP_STATE_PARAM_BATCH_MAX;
    }
    const sloop_param_pair_t *pairs = p;
    uint8_t ids[APP_STATE_PARAM_BATCH_MAX];
    int16_t values[APP_STATE_PARAM_BATCH_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ids[i] = pairs[i].id;
        values[i] = pairs[i].v;
    }
    sloop_core_set_params(track, ids, values, n);
    state_refresh_params(s);
}

/* 全局参数（GLOBAL 参数页的唯一写路径）：经引擎写入口按 GP[id] 钳位后立即回读
 * g[] 投影。G_BPM 另同拍更新快照的 bpm（TRANSPORT 与 STATE SNAP 与 g[0] 同一真值，
 * 不落后一拍）。动作类全局不在本命令语义内（见 sloop_state.h 的命令契约）。 */
static void cmd_set_global(app_state_t *s, uint32_t id, uint16_t value)
{
    if (id >= SLOOP_CORE_G_COUNT) return;
    sloop_core_set_global(id, (int16_t)value);
    state_refresh_params(s);
    if (id == STATE_G_BPM) s->core.bpm = (uint16_t)s->g[STATE_G_BPM];
}

static void cmd_step_edit(app_state_t *s, const sloop_step_edit_t *edit)
{
    if (edit == NULL || edit->track >= APP_STATE_TRACKS || edit->step >= STATE_STEPS_MAX ||
        (int32_t)edit->op < 0 || edit->op > SEQ_STEP_CLEAR) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    state_seq_result(s, sloop_seq_step_edit(edit));
}

static void cmd_pattern(app_state_t *s, uint32_t track, uint32_t op, uint16_t arg)
{
    if (track >= APP_STATE_TRACKS || (int32_t)op < 0 || op > SEQ_PATTERN_REDO) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    state_seq_result(s, sloop_seq_pattern(track, (sloop_pattern_op_t)op, (int16_t)arg));
}

static void cmd_micro_fill(app_state_t *s, edit_cmd_type_t type,
                           uint32_t track, uint32_t step, uint16_t value)
{
    if (track >= APP_STATE_TRACKS || step >= STATE_STEPS_MAX) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = type == CMD_SET_MICRO
                        ? sloop_seq_set_micro(track, step, (int8_t)value)
                        : sloop_seq_set_fill(track, step, (uint8_t)value);
    state_seq_result(s, ret);
}

static void cmd_lock(app_state_t *s, edit_cmd_type_t type, uint32_t track,
                     uint32_t step, uint32_t param, uint16_t value)
{
    bool valid = track < APP_STATE_TRACKS && step < STATE_STEPS_MAX &&
                 (type == CMD_LOCK_DEL ? param <= SLOOP_CORE_P_COUNT
                                       : param < SLOOP_CORE_P_COUNT);
    if (!valid) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = type == CMD_LOCK_SET
                        ? sloop_seq_lock_set(track, step, param, (int16_t)value)
                        : sloop_seq_lock_del(track, step, param);
    state_seq_result(s, ret);
}

static void cmd_drum_step(app_state_t *s, uint32_t step, const sloop_dstep_t *dstep)
{
    if (step >= STATE_STEPS_MAX || dstep == NULL) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    state_seq_result(s, sloop_seq_write_dstep(step, dstep));
}

/* 存储命令（P4-B，spec §4.3）：槽号/轨号越界 ⇒ 不调存储 API、不改 store_status
 * （命令无效 ≠ 存储失败）。返回码按 app_store_status_t 映射；成功即整份刷新投影
 * （工程/预置载入会改引擎真值）。存储调用在状态锁内、flash 擦写由存储层在引擎锁外
 * 完成（ADR-009 §3）；UI 读副本最多等一次擦写（≤150ms，spec §4.3 接受）。 */
#define STATE_DRUM_TRACK (APP_STATE_TRACKS - 1u)

static void state_store_result(app_state_t *s, esp_err_t ret, bool drum)
{
    switch (ret) {
    case ESP_OK: s->store_status = APP_STORE_STATUS_OK; break;
    case ESP_ERR_NOT_FOUND: s->store_status = APP_STORE_STATUS_EMPTY; break;
    case ESP_ERR_INVALID_STATE: s->store_status = APP_STORE_STATUS_PLAYING; break;
    case ESP_ERR_INVALID_ARG:
        s->store_status = drum ? APP_STORE_STATUS_DRUM : APP_STORE_STATUS_ERROR;
        break;
    default: s->store_status = APP_STORE_STATUS_ERROR; break;
    }
    if (ret == ESP_OK) state_refresh_locked();
}

static void cmd_project(app_state_t *s, edit_cmd_type_t type, uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) return;
    esp_err_t ret;
    switch (type) {
    case CMD_PROJECT_SAVE: ret = sloop_store_project_save(slot); break;
    case CMD_PROJECT_LOAD: ret = sloop_store_project_load(slot); break;
    case CMD_PROJECT_ERASE: ret = sloop_store_project_erase(slot); break;
    default: return; /* 不认识的类型不发存储动作（分发只传上述三种；default 不是 erase） */
    }
    state_store_result(s, ret, false);
}

/* STORE 的目标轨是选中轨（上游 SAVE > USER）；LOAD 的轨号由 b 显式给出（保存的轨
 * 号语义随上游 up_load_to）。鼓轨没有声音 ⇒ 存储层回 ESP_ERR_INVALID_ARG，状态层
 * 按「目标是否鼓轨」把它映射成 drum 而不是 error。 */
static void cmd_preset(app_state_t *s, edit_cmd_type_t type, uint32_t slot, uint32_t track)
{
    if (slot >= SLOOP_STORE_PRESETS) return;
    bool drum = false;
    esp_err_t ret;
    switch (type) {
    case CMD_PRESET_STORE:
        drum = s->core.selected_track == STATE_DRUM_TRACK;
        ret = sloop_store_preset_store(slot);
        break;
    case CMD_PRESET_LOAD:
        if (track >= APP_STATE_TRACKS) return;
        drum = track == STATE_DRUM_TRACK;
        ret = sloop_store_preset_load(slot, track);
        break;
    case CMD_PRESET_ERASE: ret = sloop_store_preset_erase(slot); break;
    default: return; /* 不认识的类型不发存储动作（default 不是 erase） */
    }
    state_store_result(s, ret, drum);
}

/* PUT = 选中轨的 live patch（bank_current 只读引擎状态）。选中鼓轨时 fm6_patch 不
 * 存在（输出全 0），故直接拒绝：drum 且不调存储 API（与 console 的守卫同口径）。 */
static void cmd_bank(app_state_t *s, edit_cmd_type_t type, uint32_t slot)
{
    if (slot >= SLOOP_STORE_BANK_SLOTS) return;
    switch (type) {
    case CMD_BANK_ERASE:
        state_store_result(s, sloop_store_bank_erase(slot), false);
        return;
    case CMD_BANK_PUT:
        break;
    default: return; /* 不认识的类型不发存储动作（default 不是 PUT） */
    }
    if (s->core.selected_track == STATE_DRUM_TRACK) {
        s->store_status = APP_STORE_STATUS_DRUM;
        return;
    }
    uint8_t packed[SLOOP_STORE_BANK_PACKED];
    sloop_store_bank_current(packed);
    state_store_result(s, sloop_store_bank_put(slot, packed), false);
}

void state_apply(const edit_cmd_t *cmd)
{
    if (cmd == NULL) return;
    state_lock();
    app_state_t *s = state_locked();
    switch (cmd->type) {
    case CMD_NOTE_ON: cmd_note(s, cmd->a, cmd->v, true); break;
    case CMD_NOTE_OFF: cmd_note(s, cmd->a, cmd->v, false); break;
    case CMD_PLAY: cmd_play(s, true); break;
    case CMD_STOP: cmd_play(s, false); break;
    case CMD_SELECT_TRACK: cmd_select_track(s, cmd->a); break;
    case CMD_TOGGLE_STEP: cmd_toggle_step(s, cmd->a, cmd->b); break;
    case CMD_SET_BPM: cmd_set_bpm(s, cmd->v); break;
    case CMD_SET_TRACK: cmd_select_track(s, cmd->a); break;
    case CMD_MUTE:
    case CMD_SOLO: cmd_track_mix(s, cmd->type, cmd->a); break;
    case CMD_ERASE_NOTE: cmd_erase_note(s, cmd->a, cmd->b, cmd->v); break;
    case CMD_SET_WINDOW: cmd_set_window(s, cmd->b, cmd->v); break;
    case CMD_SET_PARAM: cmd_set_param(s, cmd->a, cmd->b, cmd->v); break;
    case CMD_SET_PARAM_BATCH: cmd_set_param_batch(s, cmd->a, cmd->p, cmd->n); break;
    case CMD_SET_GLOBAL: cmd_set_global(s, cmd->a, cmd->v); break;
    case CMD_PROJECT_SAVE:
    case CMD_PROJECT_LOAD:
    case CMD_PROJECT_ERASE: cmd_project(s, cmd->type, cmd->a); break;
    case CMD_PRESET_STORE:
    case CMD_PRESET_LOAD:
    case CMD_PRESET_ERASE: cmd_preset(s, cmd->type, cmd->a, cmd->b); break;
    case CMD_BANK_PUT:
    case CMD_BANK_ERASE: cmd_bank(s, cmd->type, cmd->a); break;
    case CMD_STEP_EDIT: cmd_step_edit(s, cmd->p); break;
    case CMD_PATTERN: cmd_pattern(s, cmd->a, cmd->b, cmd->v); break;
    case CMD_SET_MICRO:
    case CMD_SET_FILL: cmd_micro_fill(s, cmd->type, cmd->a, cmd->b, cmd->v); break;
    case CMD_LOCK_SET:
    case CMD_LOCK_DEL: cmd_lock(s, cmd->type, cmd->a, cmd->b, cmd->n, cmd->v); break;
    case CMD_DRUM_STEP_WRITE: cmd_drum_step(s, cmd->b, cmd->p); break;
    default: break;
    }
    /* 自动存档的「无输入 ≥2.5s」计时（上游 ui_input_ms）：任何一次编辑命令都算活动，
     * 与键盘/TAP 同一时间戳（state_now_ms）。只写时间戳，不取其它锁。 */
    sloop_store_note_activity(state_now_ms());
    state_unlock();
}
