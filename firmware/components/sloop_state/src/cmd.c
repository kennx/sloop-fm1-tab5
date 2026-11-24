/* sloop_state 的 EditCmd 分发：执行端 = 本地引擎（sloop_core）。
 * 阶段 5 联动模式替换本文件的分发目标（FM-1 会话），AppState 与接口不变。 */
#include "state_internal.h"

#include <string.h>

#include "sloop_store.h" /* sloop_store_note_activity（自动存档的活动计时） */
#include "sloop_hal.h"  /* 平台设置（亮度/音量/输出路由；副作用即时、NVS 延迟） */

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
/* P4-E 设置/平台值域（store/HAL 内部再钳一次，纵深防御）：lights word bits0..3 的档位、
 * MIDI 尾段 byte38 bits2..5 的 flt_reso、HAL 的亮度/音量范围（sloop_hal.h）。palette 用
 * sloop_store.h 的 SLOOP_SETTINGS_PALETTES。 */
#define STATE_SET_LIGHTS_MAX 3u
#define STATE_SET_RESO_MAX 10u
#define STATE_SYS_BRIGHT_MIN 10u
#define STATE_SYS_BRIGHT_MAX 100u
#define STATE_SYS_VOL_MAX 100u
#define STATE_P_E0 (SLOOP_CORE_P_COUNT - 8u) /* 上游 P_E0：鼓轨的 kit（SOUND 页 KIT 行） */

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
    /* SOUND 页眉同理：sound 块是选中轨的引擎/预置名，等下一拍 sync 会有一帧「标题=新轨、
     * 页眉=旧轨的声音」。 */
    state_refresh_sound_locked();
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
    if (sloop_seq_frozen()) {
        s->seq_status = SEQ_STATUS_FROZEN;
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
        state_seq_result(s, sloop_seq_frozen() ? ESP_ERR_INVALID_STATE : ESP_OK);
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
    if (sloop_seq_frozen()) {
        s->seq_status = SEQ_STATUS_FROZEN;
        return;
    }
    /* 音高 → index 的查找与删除由 seq adapter 在**同一把引擎锁**内完成：实时录音
     * 可能并发改步，状态层预读的 index 会在两次取锁之间失效（评审 deferral）。 */
    state_seq_result(s, sloop_seq_note_del(track, idx, note));
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

/* 「最后碰过的参数」（P4-F Task 6；上游 ui.lock_par）：只记写在**焦点轨**上的那一对，
 * CMD_LOCK_FOCUS 据此决定锁哪个参数。UI 局部态，sync/刷新不改写。 */
static void state_note_param(app_state_t *s, uint32_t track, uint8_t id)
{
    if (track != s->focus_track) return;
    s->last_param.track = (uint8_t)track;
    s->last_param.id = id;
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
    state_note_param(s, track, pid);
    /* 选中轨的 P_E0 决定 sound 块里鼓轨的 kit 名（SOUND 页 KIT 行）：同拍重取，
     * 页眉与 TAP 回显不落后一拍。其它参数不改 sound 块，不读。 */
    if (pid == STATE_P_E0 && track == s->core.selected_track) state_refresh_sound_locked();
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
    bool writes_e0 = false; /* 批里含 P_E0：与单写同一规则（见 cmd_set_param） */
    for (uint32_t i = 0; i < n; i++) {
        ids[i] = pairs[i].id;
        values[i] = pairs[i].v;
        writes_e0 = writes_e0 || ids[i] == STATE_P_E0;
    }
    sloop_core_set_params(track, ids, values, n);
    state_refresh_params(s);
    state_note_param(s, track, ids[n - 1u]); /* 批量的最后一对 */
    if (writes_e0 && track == s->core.selected_track) state_refresh_sound_locked();
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
    int16_t sv = (int16_t)value;
    if ((type == CMD_SET_MICRO && (sv < -32 || sv > 31)) ||
        (type == CMD_SET_FILL && (sv < 0 || sv > 3))) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = type == CMD_SET_MICRO
                        ? sloop_seq_set_micro(track, step, (int8_t)sv)
                        : sloop_seq_set_fill(track, step, (uint8_t)sv);
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

/* 录音命令（P4-C Task 6）：adapter 是唯一状态机实现；成功后同拍重取快照投影，
 * 让 `STATE SNAP`/UI 在下一拍 sync 之前就看到 armed/recording/off。快照是**一次
 * 性**的（take 结果被消费），所以只在显式命令之后重取，不在每次分发都读。 */
static void state_refresh_rec(app_state_t *s)
{
    sloop_seq_rec_read(&s->rec);
}

static void cmd_rec_toggle(app_state_t *s)
{
    esp_err_t ret = sloop_seq_rec_toggle();
    state_seq_result(s, ret);
    if (ret == ESP_OK) state_refresh_rec(s);
}

static void cmd_rec_config(app_state_t *s, uint32_t quant_off, uint32_t tempo_mode, uint32_t count_mode)
{
    if (quant_off > 1u || tempo_mode > 1u || count_mode > 1u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_seq_rec_config((uint8_t)quant_off, (uint8_t)tempo_mode,
                                         (uint8_t)count_mode);
    state_seq_result(s, ret);
    if (ret == ESP_OK) {
        state_refresh_rec(s);
        /* REC 三项（bits9/10/31）随 settings 落盘（上游语义）：只置 dirty，落盘由 1s 任务 */
        sloop_store_settings_dirty_mark();
    }
}

static void cmd_fill_control(app_state_t *s, uint32_t held, uint32_t next_bar)
{
    if (held > 1u || next_bar > 1u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    state_seq_result(s, sloop_seq_fill_control(held != 0u, next_bar != 0u));
}

/* 鼓演奏：lane/velocity 越界是命令错误（invalid，不调引擎）；成功不改 seq_status
 * （演奏不是一次编辑），也从不回落合成轨 0。velocity 0 = 松开（自由录音的音符段）。 */
static void cmd_drum_hit(app_state_t *s, uint32_t lane, uint32_t velocity)
{
    if (lane > 15u || velocity > 127u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_seq_drum_hit(lane, velocity);
    if (ret != ESP_OK) {
        switch (ret) {
        case ESP_ERR_INVALID_STATE: s->seq_status = SEQ_STATUS_FROZEN; break;
        case ESP_ERR_INVALID_ARG: s->seq_status = SEQ_STATUS_INVALID; break;
        default: s->seq_status = SEQ_STATUS_ERROR; break;
        }
    }
}

/* 物理 PLAY（面板按钮口径）：down/up 都透传；playing 投影留给下一拍 sync（start
 * 请求在音频块边界才被上游消费，提前改写投影会抢跑引擎真值）。 */
static void cmd_play_button(app_state_t *s, uint32_t down)
{
    (void)s;
    sloop_seq_play_button(down != 0u);
}

/* ------------------------------------------------- P4-C Task 8: sections / chain / SONG ---
 * 状态层只校验公共边界（槽号/模式值/数组指针）并分发；上游顺序时钟、场景应用、
 * chain 与 SONG REC 状态机全在 seq adapter 一处。成功同拍刷新 song 快照（UI 不读上游
 * 全局）；SECTION LOAD/ERASE 还会改轨/参数，故走整份投影刷新。
 * 返回码：NOT_ALLOWED → BLOCKED（走带中/SONG 播放中/REC IS ON）；INVALID_STATE 且适配层
 * 冻结 → FROZEN，其余 INVALID_STATE（存储未就绪）→ ERROR（与 sloop_seq.h 的约定一致）。 */
static void state_song_result(app_state_t *s, esp_err_t ret)
{
    switch (ret) {
    case ESP_OK: s->seq_status = SEQ_STATUS_OK; break;
    case ESP_ERR_NOT_FOUND: s->seq_status = SEQ_STATUS_NOT_FOUND; break;
    case ESP_ERR_INVALID_ARG: s->seq_status = SEQ_STATUS_INVALID; break;
    case ESP_ERR_NOT_ALLOWED: s->seq_status = SEQ_STATUS_BLOCKED; break;
    case ESP_ERR_INVALID_STATE:
        s->seq_status = sloop_seq_frozen() ? SEQ_STATUS_FROZEN : SEQ_STATUS_ERROR;
        break;
    default: s->seq_status = SEQ_STATUS_ERROR; break;
    }
}

static void state_refresh_song(app_state_t *s)
{
    sloop_seq_song_read(&s->song);
}

static void cmd_section(app_state_t *s, edit_cmd_type_t type, uint32_t slot)
{
    if (slot >= SLOOP_STORE_PROJECTS) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret;
    switch (type) {
    case CMD_SECTION_STORE: ret = sloop_seq_section(slot, true, false); break;
    case CMD_SECTION_LOAD: ret = sloop_seq_section(slot, false, false); break;
    case CMD_SECTION_ERASE: ret = sloop_seq_section(slot, false, true); break;
    case CMD_SECTION_REQUEST: ret = sloop_seq_section_request(slot); break;
    default: return;
    }
    state_song_result(s, ret);
    if (ret != ESP_OK) return;
    if (type == CMD_SECTION_LOAD || type == CMD_SECTION_ERASE)
        state_refresh_locked(); /* 工程应用会改选中轨/参数/图案 */
    else
        state_refresh_song(s);
}

static void cmd_chain_begin(app_state_t *s, uint32_t first)
{
    if (first >= SLOOP_STORE_PROJECTS) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_seq_chain_begin(first);
    state_song_result(s, ret);
    if (ret == ESP_OK) state_refresh_song(s);
}

static void cmd_chain_commit(app_state_t *s, const void *scenes, uint32_t count)
{
    if (count > 8u || (count != 0u && scenes == NULL)) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_seq_chain_commit(scenes, count);
    state_song_result(s, ret);
    if (ret == ESP_OK) state_refresh_song(s);
}

static void cmd_song_mode(app_state_t *s, uint32_t enabled)
{
    if (enabled > 1u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_seq_song_mode(enabled != 0u);
    state_song_result(s, ret);
    if (ret == ESP_OK) state_refresh_song(s);
}

static void cmd_song_rec(app_state_t *s)
{
    esp_err_t ret = sloop_seq_song_rec();
    state_song_result(s, ret);
    if (ret == ESP_OK) state_refresh_song(s);
}

static void cmd_song_set(app_state_t *s, const sloop_song_config_t *cfg)
{
    if (cfg == NULL) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_store_song_set(cfg); /* 只改内存 + dirty；落盘由 1s 任务完成 */
    state_song_result(s, ret);
    if (ret == ESP_OK) state_refresh_song(s);
}

static void cmd_drum_step(app_state_t *s, uint32_t step, const sloop_dstep_t *dstep)
{
    if (step >= STATE_STEPS_MAX || dstep == NULL) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    state_seq_result(s, sloop_seq_write_dstep(step, dstep));
}

/* --------------------------------- P4-D Task 6: DSYN / FM6 patch / 采样槽 ---
 * 状态层只做公共边界（kit/lane/field/op/slot/velocity）与错误映射，值钳位/字节合成在
 * adapter（sloop_core_dsyn_* / sloop_core_fm6_*）。存储 API 用 INVALID_STATE 同时表示
 * 走带/冻结/未就绪，故借 sloop_seq_frozen() 与 sloop_store_ready() 分开三种文案
 * （spec §4.3）。成功同拍刷新对应投影：DSYN 按当前选择、FM6 按选中轨。 */
static void state_synth_result(app_state_t *s, esp_err_t ret)
{
    switch (ret) {
    case ESP_OK: s->synth_status = APP_SYNTH_STATUS_OK; break;
    case ESP_ERR_INVALID_ARG: s->synth_status = APP_SYNTH_STATUS_INVALID; break;
    case ESP_ERR_NO_MEM: s->synth_status = APP_SYNTH_STATUS_FULL; break;
    case ESP_ERR_NOT_ALLOWED: s->synth_status = APP_SYNTH_STATUS_BLOCKED; break;
    case ESP_ERR_INVALID_STATE:
        if (sloop_seq_frozen()) s->synth_status = APP_SYNTH_STATUS_FROZEN;
        else if (!sloop_store_ready()) s->synth_status = APP_SYNTH_STATUS_ERROR;
        else s->synth_status = APP_SYNTH_STATUS_BLOCKED; /* 走带中（store_playing） */
        break;
    default: s->synth_status = APP_SYNTH_STATUS_ERROR; break;
    }
}

static void cmd_dsyn_set(app_state_t *s, uint32_t kit, uint32_t lane, uint32_t field, uint16_t value)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || field >= SLOOP_DSYN_FIELDS) {
        s->synth_status = APP_SYNTH_STATUS_INVALID; /* 越界不调引擎 */
        return;
    }
    /* 越界已拒；剩下的哨兵只可能是诊断冻结（adapter 在锁外/锁内各查一次）。 */
    if (sloop_core_dsyn_set(kit, lane, field, (int16_t)value) == SLOOP_DSYN_INVALID) {
        s->synth_status = APP_SYNTH_STATUS_FROZEN;
        return;
    }
    s->synth_status = APP_SYNTH_STATUS_OK;
    state_refresh_dsyn_locked();
}

static void cmd_dsyn_load_kit(app_state_t *s, uint32_t kit, uint32_t src)
{
    if (kit >= SLOOP_DSYN_KITS) {
        s->synth_status = APP_SYNTH_STATUS_INVALID;
        return;
    }
    esp_err_t ret = sloop_core_dsyn_load_factory(kit, src);
    switch (ret) {
    case ESP_OK: s->synth_status = APP_SYNTH_STATUS_OK; break;
    case ESP_ERR_INVALID_ARG: s->synth_status = APP_SYNTH_STATUS_INVALID; break;
    case ESP_ERR_INVALID_STATE: s->synth_status = APP_SYNTH_STATUS_FROZEN; break;
    default: s->synth_status = APP_SYNTH_STATUS_ERROR; break;
    }
    if (ret == ESP_OK) state_refresh_dsyn_locked();
}

static void cmd_dsyn_store(app_state_t *s)
{
    state_synth_result(s, sloop_store_dsu_store());
    if (s->synth_status == APP_SYNTH_STATUS_OK) state_refresh_dsyn_locked(); /* dirty → 0 */
}

/* kit 级 CRUSH：值域在状态层先校验（0..255），core 的哨兵因此只剩「冻结」一因
 * （与 cmd_dsyn_set 同口径）；成功后同拍重取 dsyn 投影（crush/dirty）。 */
static void cmd_dsyn_crush(app_state_t *s, uint32_t kit, uint16_t value)
{
    if (kit >= SLOOP_DSYN_KITS || value > 255u) {
        s->synth_status = APP_SYNTH_STATUS_INVALID; /* 越界不调引擎 */
        return;
    }
    if (sloop_core_dsyn_set_crush(kit, (int16_t)value) == SLOOP_DSYN_INVALID) {
        s->synth_status = APP_SYNTH_STATUS_FROZEN;
        return;
    }
    s->synth_status = APP_SYNTH_STATUS_OK;
    state_refresh_dsyn_locked();
}

/* 试听不改数据（上游只写 dsu_aud_* 队列），因此不刷新投影；velocity 0 是「无声」、
 * >127 越界，两者都是命令错误且不调 core（上游 ED_DSYN_PLAY 同口径）。 */
static void cmd_dsyn_audition(app_state_t *s, uint32_t kit, uint32_t lane, uint32_t velocity)
{
    if (kit >= SLOOP_DSYN_KITS || lane >= SLOOP_DSYN_LANES || velocity == 0u || velocity > 127u) {
        s->synth_status = APP_SYNTH_STATUS_INVALID;
        return;
    }
    sloop_core_dsyn_audition(kit, lane, velocity);
    s->synth_status = APP_SYNTH_STATUS_OK;
}

/* core 对「越界 op/field / 非 FM6 轨 / 鼓轨 / 冻结」统一回 SLOOP_FM6_INVALID，命中哨兵时
 * 再读一次 patch 快照区分（INVALID_STATE → FROZEN；INVALID_ARG/NOT_ALLOWED/engine_ok=0
 * → LOCKED；其余 → INVALID）。成功路径只有一次引擎锁。 */
static void cmd_fm6_set(app_state_t *s, uint32_t track, uint32_t op, uint32_t field, uint16_t value)
{
    if (track >= APP_STATE_TRACKS) {
        s->synth_status = APP_SYNTH_STATUS_INVALID;
        return;
    }
    if (sloop_core_fm6_set(track, op, field, (int16_t)value) != SLOOP_FM6_INVALID) {
        s->synth_status = APP_SYNTH_STATUS_OK;
        state_refresh_fm6_locked();
        /* ALG（op 6 field 8）的适配层在同一次引擎锁内清 `p[P_E0]`（ALG 宏 → PAT）：
         * p[] 投影必须同拍跟上，否则页面的 `ALG MACRO n` 提示与 TAP 回显会多留一拍。
         * 其余字段不动宏，不重读（省一次引擎快照）。 */
        if (op == SLOOP_FM6_GLOBAL_OP && field == SLOOP_FM6_ALG_FIELD) {
            sloop_core_read_params(track, s->p, NULL);
        }
        return;
    }
    sloop_fm6_patch_snapshot_t snap;
    esp_err_t ret = sloop_core_fm6_patch(track, &snap);
    if (ret == ESP_ERR_INVALID_STATE) {
        s->synth_status = APP_SYNTH_STATUS_FROZEN;
    } else if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_NOT_ALLOWED ||
               (ret == ESP_OK && !snap.engine_ok)) {
        s->synth_status = APP_SYNTH_STATUS_LOCKED;
    } else {
        s->synth_status = APP_SYNTH_STATUS_INVALID;
    }
}

static void cmd_sample_erase(app_state_t *s, uint32_t slot)
{
    if (slot >= SLOOP_STORE_SAMPLES) {
        s->synth_status = APP_SYNTH_STATUS_INVALID; /* 越界不调存储 */
        return;
    }
    state_synth_result(s, sloop_store_sample_erase(slot));
}

/* ------------------------------------ P4-E Task 4: settings / 平台设置 ---------------
 * 状态层只做值域钳位与返回码映射：生效（上游 static）与 dirty 记账在 store，平台副作用
 * （LEDC / codec）与 nvs_dirty 在 HAL。两者都不受走带限制（播放中也可改）：HAL 调用
 * 不取引擎锁，store 内部只短持引擎锁改上游 static。
 * 成功同拍重取对应投影；失败投影不动（store/HAL 保证失败时状态不变）。 */
static void state_settings_result(app_state_t *s, esp_err_t ret)
{
    switch (ret) {
    case ESP_OK: s->settings_status = APP_SETTINGS_OK; break;
    case ESP_ERR_INVALID_ARG:
    /* headphone：本板输出路由 speaker-only（R16/R19，不另设枚举值） */
    case ESP_ERR_NOT_SUPPORTED: s->settings_status = APP_SETTINGS_INVALID; break;
    case ESP_ERR_INVALID_STATE: s->settings_status = APP_SETTINGS_BLOCKED; break; /* 冻结/未就绪 */
    default: s->settings_status = APP_SETTINGS_ERROR; break;
    }
}

static void cmd_settings_set(app_state_t *s, uint32_t field, uint16_t value)
{
    if (field >= SLOOP_SET_FIELD_COUNT) {
        s->settings_status = APP_SETTINGS_INVALID; /* 越界不调存储 */
        return;
    }
    uint32_t v = value;
    switch (field) {
    case SLOOP_SET_PALETTE:
        if (v >= SLOOP_SETTINGS_PALETTES) v = SLOOP_SETTINGS_PALETTES - 1u;
        break;
    case SLOOP_SET_LIGHTS:
        if (v > STATE_SET_LIGHTS_MAX) v = STATE_SET_LIGHTS_MAX;
        break;
    case SLOOP_SET_RESO:
        if (v > STATE_SET_RESO_MAX) v = STATE_SET_RESO_MAX;
        break;
    case SLOOP_SET_LOWCUT:
    case SLOOP_SET_ZOOM:
    case SLOOP_SET_THEME_ON:
        if (v > 1u) v = 1u;
        break;
    default: return;
    }
    esp_err_t ret = sloop_store_settings_set((sloop_set_field_t)field, v);
    state_settings_result(s, ret);
    if (ret == ESP_OK) state_refresh_settings_locked();
}

static void cmd_sys(app_state_t *s, edit_cmd_type_t type, uint16_t value)
{
    uint32_t v = value;
    esp_err_t ret;
    switch (type) {
    case CMD_SYS_BRIGHT:
        if (v < STATE_SYS_BRIGHT_MIN) v = STATE_SYS_BRIGHT_MIN;
        if (v > STATE_SYS_BRIGHT_MAX) v = STATE_SYS_BRIGHT_MAX;
        ret = sloop_hal_settings_set_bright((uint8_t)v);
        break;
    case CMD_SYS_VOLUME:
        if (v > STATE_SYS_VOL_MAX) v = STATE_SYS_VOL_MAX;
        ret = sloop_hal_settings_set_volume((uint8_t)v);
        break;
    case CMD_SYS_OUTPUT:
        if (v > 1u) {
            s->settings_status = APP_SETTINGS_INVALID; /* 非 speaker|headphone：不入 HAL */
            return;
        }
        ret = sloop_hal_settings_set_output((uint8_t)v);
        break;
    default: return;
    }
    state_settings_result(s, ret);
    if (ret == ESP_OK) state_refresh_sys_locked();
}

/* ------------------------------------ P4-F Task 5: SOUND 页眉（换引擎/换预置） -------
 * 边界在状态层判：core 的 set_engine/apply_preset 对越界、鼓轨与冻结都静默丢弃（void），
 * 不先拒就把「值非法」与「成功」混成同一个结果。预置数按 a 轨自己的引擎取。
 * 成功 = 一次整份刷新（换声音会改该轨全部参数；sound 块随同重取）。 */
static void cmd_set_engine(app_state_t *s, uint32_t track, uint16_t engine)
{
    if (track >= STATE_SYNTH_TRACKS || engine >= SLOOP_CORE_NENGINES) {
        s->sound_status = APP_SOUND_STATUS_INVALID;
        return;
    }
    if (sloop_seq_frozen()) {
        s->sound_status = APP_SOUND_STATUS_FROZEN;
        return;
    }
    sloop_core_set_engine(track, engine);
    s->sound_status = APP_SOUND_STATUS_OK;
    state_refresh_locked();
}

static void cmd_set_preset(app_state_t *s, uint32_t track, uint16_t preset)
{
    sloop_sound_snapshot_t info;
    if (track >= STATE_SYNTH_TRACKS || !sloop_core_sound_read((uint8_t)track, &info) ||
        preset >= info.npresets) {
        s->sound_status = APP_SOUND_STATUS_INVALID;
        return;
    }
    if (sloop_seq_frozen()) {
        s->sound_status = APP_SOUND_STATUS_FROZEN;
        return;
    }
    sloop_core_apply_preset(track, preset);
    s->sound_status = APP_SOUND_STATUS_OK;
    state_refresh_locked();
}

/* ----------------------------------------- P4-F Task 6: TOOLS / 焦点步锁与 fill ---
 * 状态层只做边界与返回码映射；三动作的语义/走带守卫在 sloop_core_tools（清单 012 §4），
 * 键的可锁性与锁表容量在既有 sloop_seq_lock_set（上游 p_lockable）。成功整份刷新（NEW
 * 会改四条轨的参数/步表与全局）。 */
static void cmd_tools(app_state_t *s, uint32_t action)
{
    if (action > (uint32_t)SLOOP_TOOL_NEWPRJ) {
        s->tools_status = APP_TOOLS_STATUS_INVALID; /* 越界不调引擎 */
        return;
    }
    esp_err_t ret = sloop_core_tools(action);
    switch (ret) {
    case ESP_OK: s->tools_status = APP_TOOLS_STATUS_OK; break;
    case ESP_ERR_INVALID_ARG: s->tools_status = APP_TOOLS_STATUS_INVALID; break;
    case ESP_ERR_NOT_ALLOWED: s->tools_status = APP_TOOLS_STATUS_BLOCKED; break;
    case ESP_ERR_INVALID_STATE: s->tools_status = APP_TOOLS_STATUS_FROZEN; break; /* 仅冻结 */
    default: s->tools_status = APP_TOOLS_STATUS_ERROR; break;
    }
    if (ret == ESP_OK) state_refresh_locked();
}

/* 焦点步 → (焦点轨, 引擎绝对步)。焦点列是窗口列；窗口外/越界没有可作用的步。 */
static bool focus_step_abs(const app_state_t *s, uint32_t *ft, uint32_t *abs)
{
    *ft = s->focus_track;
    if (*ft >= APP_STATE_TRACKS || s->focus_step >= s->window.window_len) return false;
    *abs = (uint32_t)s->window.window_start + s->focus_step;
    return *abs < STATE_STEPS_MAX;
}

/* 聚焦步有音（合成轨 n>0 且 time==NOTE；鼓轨 16 lane 位图非 0）：锁与 fill 条件只作用
 * 于有音的步——空步上的锁/条件在 STEP 页不可见（见 sloop_state.h）。 */
static bool focus_step_on(const app_state_t *s, uint32_t ft)
{
    uint32_t col = s->focus_step;
    if (ft == APP_STATE_TRACKS - 1u) /* 鼓轨 */
        return s->core.dstep_raw[col].on[0] != 0u || s->core.dstep_raw[col].on[1] != 0u;
    return state_step_is_on(s->core.step[ft][col].n, s->core.step[ft][col].time);
}

#define STATE_LOCK_LIST_MAX 32u /* ≥ 上游 NLOCK(24)：整条轨的锁一次取完 */

static void cmd_lock_focus(app_state_t *s, uint32_t lock)
{
    uint32_t ft, abs;
    if (lock > 1u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    if (!focus_step_abs(s, &ft, &abs) || !focus_step_on(s, ft)) {
        s->seq_status = SEQ_STATUS_NOT_FOUND; /* NO STEP */
        return;
    }
    if (s->last_param.id == 0xFFu || s->last_param.track != ft) {
        s->seq_status = SEQ_STATUS_NOT_FOUND; /* NO PARAM：这条轨上还没碰过参数 */
        return;
    }
    if (lock != 0u) {
        /* 锁值 = 该轨当前参数（上游 LOCK 旋钮从现值起；不是选中轨的 p[] 投影） */
        int16_t value[SLOOP_CORE_P_COUNT];
        sloop_core_read_params(ft, value, NULL);
        state_seq_result(s, sloop_seq_lock_set(ft, abs, s->last_param.id, value[s->last_param.id]));
        return;
    }
    sloop_lock_t locks[STATE_LOCK_LIST_MAX];
    uint32_t n = sloop_seq_lock_list(ft, locks, STATE_LOCK_LIST_MAX);
    for (uint32_t i = 0; i < n; i++) {
        if (locks[i].step != abs || locks[i].param != s->last_param.id) continue;
        state_seq_result(s, sloop_seq_lock_del(ft, abs, s->last_param.id));
        return;
    }
    s->seq_status = SEQ_STATUS_NOT_FOUND; /* NO LOCK：该键本来就没有锁 */
}

static void cmd_fill_step(app_state_t *s, uint16_t value)
{
    uint32_t ft, abs;
    if (value > 3u) {
        s->seq_status = SEQ_STATUS_INVALID;
        return;
    }
    if (!focus_step_abs(s, &ft, &abs) || !focus_step_on(s, ft)) {
        s->seq_status = SEQ_STATUS_NOT_FOUND; /* NO STEP */
        return;
    }
    state_seq_result(s, sloop_seq_set_fill(ft, abs, (uint8_t)value));
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
    case CMD_REC_TOGGLE: cmd_rec_toggle(s); break;
    case CMD_REC_CONFIG: cmd_rec_config(s, cmd->a, cmd->b, cmd->v); break;
    case CMD_FILL_CONTROL: cmd_fill_control(s, cmd->a, cmd->b); break;
    case CMD_DRUM_HIT: cmd_drum_hit(s, cmd->a, cmd->v); break;
    case CMD_PLAY_BUTTON: cmd_play_button(s, cmd->a); break;
    case CMD_SECTION_STORE:
    case CMD_SECTION_LOAD:
    case CMD_SECTION_ERASE:
    case CMD_SECTION_REQUEST: cmd_section(s, cmd->type, cmd->a); break;
    case CMD_CHAIN_BEGIN: cmd_chain_begin(s, cmd->a); break;
    case CMD_CHAIN_COMMIT: cmd_chain_commit(s, cmd->p, cmd->n); break;
    case CMD_CHAIN_CANCEL: cmd_chain_commit(s, NULL, 0); break;
    case CMD_SONG_MODE: cmd_song_mode(s, cmd->a); break;
    case CMD_SONG_REC: cmd_song_rec(s); break;
    case CMD_SONG_SET: cmd_song_set(s, cmd->p); break;
    case CMD_DSYN_SET: cmd_dsyn_set(s, cmd->a, cmd->b, cmd->n, cmd->v); break;
    case CMD_DSYN_LOAD_KIT: cmd_dsyn_load_kit(s, cmd->a, cmd->b); break;
    case CMD_DSYN_STORE: cmd_dsyn_store(s); break;
    case CMD_DSYN_AUDITION: cmd_dsyn_audition(s, cmd->a, cmd->b, cmd->v); break;
    case CMD_DSYN_CRUSH: cmd_dsyn_crush(s, cmd->a, cmd->v); break;
    case CMD_FM6_SET: cmd_fm6_set(s, cmd->a, cmd->b, cmd->n, cmd->v); break;
    case CMD_SAMPLE_ERASE: cmd_sample_erase(s, cmd->a); break;
    case CMD_SETTINGS_SET: cmd_settings_set(s, cmd->a, cmd->v); break;
    case CMD_SYS_BRIGHT:
    case CMD_SYS_VOLUME:
    case CMD_SYS_OUTPUT: cmd_sys(s, cmd->type, cmd->v); break;
    case CMD_SELFTEST_RUN:
        if (s->selftest_unavailable) {
            s->selftest_req = 0;
            s->selftest_done = 1;
            s->selftest_ran = 0;
            s->selftest_failed = 1;
            s->settings_status = APP_SETTINGS_ERROR;
            break;
        }
        s->selftest_req = 1;
        s->selftest_done = 0;
        s->selftest_ran = 0;
        s->selftest_failed = 0;
        s->settings_status = APP_SETTINGS_OK;
        break;
    case CMD_SET_ENGINE: cmd_set_engine(s, cmd->a, cmd->v); break;
    case CMD_SET_PRESET: cmd_set_preset(s, cmd->a, cmd->v); break;
    case CMD_TOOLS: cmd_tools(s, cmd->a); break;
    case CMD_LOCK_FOCUS: cmd_lock_focus(s, cmd->a); break;
    case CMD_FILL_STEP: cmd_fill_step(s, cmd->v); break;
    default: break;
    }
    /* 自动存档的「无输入 ≥2.5s」计时（上游 ui_input_ms）：任何一次编辑命令都算活动，
     * 与键盘/TAP 同一时间戳（state_now_ms）。只写时间戳，不取其它锁。 */
    sloop_store_note_activity(state_now_ms());
    state_unlock();
}
