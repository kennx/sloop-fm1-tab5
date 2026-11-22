/* P4-C Task 3：上游 seq.c/drums.c 的公共薄适配；由 selftest.c 末尾 include。 */
#include "sloop_seq.h"

#include <string.h>

static int step_on(const step_t *st) { return st->time == ST_NOTE && st->n; }

static uint32_t note_len(const track_t *t, uint32_t i)
{
    uint32_t len = (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP), n = 1;
    while (i + n < len && t->step[i + n].time == ST_TIE)
        n++;
    return n;
}

static uint32_t note_set_len(track_t *t, uint32_t i, uint32_t want)
{
    uint32_t len = (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP), n = note_len(t, i);
    for (; n > want && n > 1u; n--) {
        if (!t->step[i + n - 1u].n)
            memset(&t->step[i + n - 1u], 0, sizeof(step_t));
        t->step[i + n - 1u].time = ST_REST;
    }
    for (; n < want && i + n < len && !t->step[i + n].n; n++) {
        memset(&t->step[i + n], 0, sizeof(step_t));
        t->step[i + n].time = ST_TIE;
    }
    return n;
}

static uint32_t note_span(const track_t *t, uint32_t i)
{
    uint8_t s[4];
    uint32_t ns;
    if (!step_on(&t->step[i]) || !(ns = span_notes(&t->step[i], s)))
        return note_len(t, i);
    return span_len(t, i, s, ns);
}

static uint32_t note_span_set(track_t *t, uint32_t i, uint32_t want)
{
    uint8_t s[4];
    uint32_t ns;
    span_why = 3;
    if (!step_on(&t->step[i]) || !(ns = span_notes(&t->step[i], s)))
        return note_set_len(t, i, want);
    return span_set(t, i, s, ns, want);
}

static void step_clear(step_t *st)
{
    memset(st, 0, sizeof *st);
    st->time = ST_REST;
}

static uint8_t drop2(uint32_t v, uint32_t k)
{
    return (uint8_t)((v & ((1u << (2u * k)) - 1u)) | ((v >> (2u * k + 2u)) << (2u * k)));
}

static void note_del_one(step_t *st, uint32_t k)
{
    uint32_t j, c;
    for (j = k; j + 1u < st->n; j++)
        st->note[j] = st->note[j + 1u];
    st->lvl = drop2(st->lvl, k);
    st->rat = drop2(st->rat, k);
    c = (st->flags & SF_CONT_ALL) / SF_CONT0;
    c = (c & ((1u << k) - 1u)) | ((c >> (k + 1u)) << k);
    st->flags = (uint8_t)((st->flags & ~SF_CONT_ALL) | c * SF_CONT0);
    st->n--;
}

static void note_del(track_t *t, uint32_t i, uint32_t k)
{
    step_t *st = &t->step[i];
    uint32_t j, c;
    if (!step_on(st))
        return;
    if (st->n <= 1u) {
        note_span_set(t, i, 1);
        step_clear(st);
        return;
    }
    {
        uint32_t w = note_span(t, i);
        if (w > 1u && w != note_len(t, i)) {
            note_span_set(t, i, 1);
            note_del_one(st, k);
            note_span_set(t, i, w);
            return;
        }
    }
    for (j = k; j + 1u < st->n; j++)
        st->note[j] = st->note[j + 1u];
    st->lvl = drop2(st->lvl, k);
    st->rat = drop2(st->rat, k);
    c = (st->flags & SF_CONT_ALL) / SF_CONT0;
    c = (c & ((1u << k) - 1u)) | ((c >> (k + 1u)) << k);
    st->flags = (uint8_t)((st->flags & ~SF_CONT_ALL) | c * SF_CONT0);
    st->n--;
}

static int step_free(const track_t *t, uint32_t i)
{
    return !step_on(&t->step[i]) && t->step[i].time != ST_TIE;
}

static int32_t note_shift(track_t *t, uint32_t i, int32_t dir, int copy)
{
    uint32_t len = trk_len(t), n = note_len(t, i), j, to;
    if (!step_on(&t->step[i]) || note_span(t, i) != n)
        return -1;
    to = copy ? i + n : dir > 0 ? i + 1u : i - 1u;
    if ((!copy && dir < 0 && i == 0u) || to + n > len)
        return -1;
    for (j = 0; j < n; j++)
        if ((to + j < i || to + j >= i + n) && !step_free(t, to + j))
            return -1;
    if (copy) {
        uint32_t free = 0, need = 0;
        for (j = 0; j < NLOCK; j++)
            free += t->lock[j].step == LOCK_FREE;
        for (j = 0; j < NLOCK; j++)
            if (t->lock[j].step == i && lock_find(t, to, t->lock[j].param, 0) < 0)
                need++;
        if (need > free)
            return -2;
    }
    if (copy) {
        for (j = 0; j < n; j++)
            t->step[to + j] = t->step[i + j];
    } else if (dir > 0) {
        for (j = n; j > 0; j--)
            t->step[i + j] = t->step[i + j - 1u];
        step_clear(&t->step[i]);
    } else {
        for (j = 0; j < n; j++)
            t->step[i - 1u + j] = t->step[i + j];
        step_clear(&t->step[i + n - 1u]);
    }
    t->micro[to] = t->micro[i];
    step_fill_set(t, to, step_fill(t, i));
    for (j = 0; j < NLOCK; j++)
        if (t->lock[j].step == i) {
            if (!copy)
                t->lock[j].step = (uint8_t)to;
            else {
                int k = lock_find(t, to, t->lock[j].param, 1);
                if (k >= 0) {
                    t->lock[k].step = (uint8_t)to;
                    t->lock[k].param = t->lock[j].param;
                    t->lock[k].val = t->lock[j].val;
                }
            }
        }
    if (!copy) {
        t->micro[i] = 0;
        step_fill_set(t, i, FC_NORM);
    }
    return (int32_t)to;
}

static uint32_t step_sess;

static void layer_undo_mark(track_t *t)
{
    if (!step_sess)
        step_sess = (undo_sess += 4u) | 3u;
    undo_mark(t, step_sess);
}

static void pattern_rotate(track_t *t, int32_t d)
{
    uint32_t len = trk_len(t), i;
    step_t keep;
    layer_undo_mark(t);
    if (d > 0) {
        keep = t->step[len - 1u];
        for (i = len - 1u; i > 0; i--)
            t->step[i] = t->step[i - 1u];
        t->step[0] = keep;
    } else {
        keep = t->step[0];
        for (i = 0; i + 1u < len; i++)
            t->step[i] = t->step[i + 1u];
        t->step[len - 1u] = keep;
    }
}

static void pattern_length(track_t *t, int32_t d)
{
    uint32_t len = trk_len(t), i;
    layer_undo_mark(t);
    if (d > 0 && len * 2u <= NSTEP) {
        for (i = 0; i < len; i++)
            t->step[len + i] = t->step[i];
        t->p[P_SLEN] = (int16_t)(len * 2u);
    } else if (d < 0 && len >= 2u) {
        t->p[P_SLEN] = (int16_t)(len / 2u);
    }
}

static void pattern_transpose(track_t *t, int32_t d)
{
    uint32_t i, j;
    if (is_drum(t))
        return;
    layer_undo_mark(t);
    for (i = 0; i < NSTEP; i++)
        for (j = 0; j < t->step[i].n && j < 4u; j++)
            t->step[i].note[j] = (uint8_t)clamp(t->step[i].note[j] + (d > 0 ? 1 : -1), 0, 127);
}

static int undo_swap(int redo)
{
    track_t *t;
    int16_t len;
    if (!undo.valid || (uint32_t)!!redo != undo.undone)
        return 0;
    t = &trk[undo.trk % NTRK];
    {
        uint32_t i;
        for (i = 0; i < NSTEP; i++) {
            step_t x = t->step[i];
            int8_t m = t->micro[i];
            t->step[i] = undo.st[i];
            undo.st[i] = x;
            t->micro[i] = undo.micro[i];
            undo.micro[i] = m;
        }
    }
    len = t->p[P_SLEN];
    t->p[P_SLEN] = undo.len;
    undo.len = len;
    undo.undone = (uint8_t)!redo;
    return 1;
}

void sloop_seq_read_dstep(uint32_t step, sloop_dstep_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (step >= NSTEP || s_frozen) return;
    engine_lock();
    if (!s_frozen) memcpy(out, &TDRUM->dstep[step], sizeof(*out));
    engine_unlock();
}

esp_err_t sloop_seq_write_dstep(uint32_t step, const sloop_dstep_t *in)
{
    if (step >= NSTEP || in == NULL) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(&TDRUM->dstep[step], in, sizeof(TDRUM->dstep[step]));
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_set_micro(uint32_t track, uint32_t step, int8_t value)
{
    if (track >= NTRK || step >= NSTEP || value < MICRO_MIN || value > MICRO_MAX)
        return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    trk[track].micro[step] = value;
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_set_fill(uint32_t track, uint32_t step, uint8_t value)
{
    if (track >= NTRK || step >= NSTEP || value > 3u) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    step_fill_set(&trk[track], step, value);
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_lock_set(uint32_t track, uint32_t step, uint32_t param, int16_t value)
{
    if (track >= NTRK || step >= NSTEP || param >= P_COUNT || !p_lockable(param))
        return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = lock_set(&trk[track], step, param, value) ? ESP_OK : ESP_ERR_NO_MEM;
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_lock_del(uint32_t track, uint32_t step, uint32_t param)
{
    if (track >= NTRK || step >= NSTEP || param > P_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    lock_del(&trk[track], step, param);
    engine_unlock();
    return ESP_OK;
}

uint32_t sloop_seq_lock_list(uint32_t track, sloop_lock_t *out, uint32_t cap)
{
    if (track >= NTRK || cap == 0u || out == NULL || s_frozen) return 0;
    uint32_t n = 0;
    engine_lock();
    if (!s_frozen) {
        const track_t *t = &trk[track];
        for (uint32_t i = 0; i < NLOCK && n < cap; i++) {
            if (t->lock[i].step >= NSTEP) continue;
            out[n].step = t->lock[i].step;
            out[n].param = t->lock[i].param;
            out[n].value = t->lock[i].val;
            n++;
        }
    }
    engine_unlock();
    return n;
}

bool sloop_seq_frozen(void)
{
    return s_frozen != 0;
}

esp_err_t sloop_seq_step_edit(const sloop_step_edit_t *cmd)
{    if (cmd == NULL || cmd->track >= NTRK || cmd->step >= NSTEP ||
        (int32_t)cmd->op < 0 || cmd->op > SEQ_STEP_CLEAR)
        return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    track_t *t = &trk[cmd->track];
    uint32_t step = cmd->step;
    esp_err_t ret = ESP_OK;
    step_sess = 0;
    if (cmd->op != SEQ_STEP_CLEAR && is_drum(t)) {
        ret = ESP_ERR_INVALID_ARG;
    } else if (cmd->op == SEQ_STEP_SPAN) {
        if (cmd->value < 1 || cmd->value > NSTEP)
            ret = ESP_ERR_INVALID_ARG;
        else if (!step_on(&t->step[step]))
            ret = ESP_ERR_NOT_FOUND;
        else {
            layer_undo_mark(t);
            note_span_set(t, step, (uint32_t)cmd->value);
        }
    } else if (cmd->op == SEQ_STEP_NOTE_DEL) {
        if (!step_on(&t->step[step]))
            ret = ESP_ERR_NOT_FOUND;
        else if (cmd->index >= t->step[step].n)
            ret = ESP_ERR_INVALID_ARG;
        else {
            layer_undo_mark(t);
            note_del(t, step, cmd->index);
        }
    } else if (cmd->op == SEQ_STEP_NOTE_MOVE || cmd->op == SEQ_STEP_NOTE_COPY) {
        if (cmd->target >= NSTEP)
            ret = ESP_ERR_INVALID_ARG;
        else if (!step_on(&t->step[step]))
            ret = ESP_ERR_NOT_FOUND;
        else {
            layer_undo_mark(t);
            int32_t at = (int32_t)step;
            if (cmd->op == SEQ_STEP_NOTE_COPY) {
                at = note_shift(t, (uint32_t)at, 1, 1);
                if (at == -2)
                    ret = ESP_ERR_NO_MEM;
                else if (at < 0)
                    ret = ESP_ERR_NOT_ALLOWED;
            }
            while (ret == ESP_OK && at != cmd->target) {
                at = note_shift(t, (uint32_t)at, cmd->target > at ? 1 : -1, 0);
                if (at < 0) ret = ESP_ERR_NOT_ALLOWED;
            }
        }
    } else if (cmd->op == SEQ_STEP_LVL || cmd->op == SEQ_STEP_RAT) {
        if (cmd->value < 0 || cmd->value > 3 ||
            (step_on(&t->step[step]) && cmd->index >= t->step[step].n))
            ret = ESP_ERR_INVALID_ARG;
        else if (!step_on(&t->step[step]))
            ret = ESP_ERR_NOT_FOUND;
        else {
            layer_undo_mark(t);
            uint32_t sh = 2u * cmd->index;
            if (cmd->op == SEQ_STEP_LVL)
                t->step[step].lvl = (uint8_t)((t->step[step].lvl & ~(3u << sh)) | (uint32_t)cmd->value << sh);
            else
                t->step[step].rat = (uint8_t)((t->step[step].rat & ~(3u << sh)) | (uint32_t)cmd->value << sh);
        }
    } else if (cmd->op == SEQ_STEP_SLIDE) {
        if (cmd->value < 0 || cmd->value > 1)
            ret = ESP_ERR_INVALID_ARG;
        else if (!step_on(&t->step[step]))
            ret = ESP_ERR_NOT_FOUND;
        else {
            layer_undo_mark(t);
            t->step[step].flags = (uint8_t)(cmd->value ? t->step[step].flags | SF_SLIDE :
                                                         t->step[step].flags & ~SF_SLIDE);
        }
    } else {
        layer_undo_mark(t);
        if (is_drum(t)) {
            memset(&t->dstep[step], 0, sizeof(t->dstep[step]));
        } else {
            while (step_on(&t->step[step])) note_del(t, step, 0);
        }
        lock_del(t, step, P_COUNT);
        t->micro[step] = 0;
        step_fill_set(t, step, FC_NORM);
    }
    step_sess = 0;
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_pattern(uint32_t track, sloop_pattern_op_t op, int32_t arg)
{
    if (track >= NTRK || (int32_t)op < 0 || op > SEQ_PATTERN_REDO) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    track_t *t = &trk[track];
    esp_err_t ret = ESP_OK;
    step_sess = 0;
    switch (op) {
    case SEQ_PATTERN_ROTATE:
        pattern_rotate(t, arg);
        break;
    case SEQ_PATTERN_LENGTH:
        pattern_length(t, arg);
        break;
    case SEQ_PATTERN_TRANSPOSE:
        if (is_drum(t))
            ret = ESP_ERR_INVALID_ARG;
        else
            pattern_transpose(t, arg);
        break;
    case SEQ_PATTERN_CLEAR:
        layer_undo_mark(t);
        steps_clear(t);
        break;
    case SEQ_PATTERN_UNDO:
    case SEQ_PATTERN_REDO:
        if (!undo_swap(op == SEQ_PATTERN_REDO)) ret = ESP_ERR_NOT_FOUND;
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    step_sess = 0;
    engine_unlock();
    return ret;
}

/* ------------------------------------------------- P4-C Task 6: 录音/鼓 --- */
/* 物理面板按钮在 Tab5 上的位（上游 ui_layers.c 由 panel.btn 派生；Tab5 没有面板，
 * 这里给自由录音的 PLAY drop 一个稳定的位）。**位 0 只在本 TU 内有效**：Tab5 不编
 * 上游 panel.c/main.c，seq.c 只用 `ft_btn_mask`/`ft_drop_mask` 与（本固件全零的）
 * `ly_bit`/`dyn_bit` 读 `fm1_in.buttons`，因此不会与层键/OCT 键别名；若将来接入
 * 面板驱动，必须改用 `panel.btn[B_PLAY]`（否则位 0 可能与某个按钮冲突）。
 * REC 不走按钮电平——键盘的 REC 按键一次 down 调 rec_toggle，与上游 editor 的 REC
 * 同一条 API 路径。 */
#define SLOOP_BTN_PLAY 1u

static void ft_masks_ensure(void)
{
    if (!ft_drop_mask) ft_drop_mask = SLOOP_BTN_PLAY;
}

/* 选中轨变更后的 rec 位跟随（上游 rec_follow）：由 sloop_core_select 在**已持引擎
 * 锁**时调用（互斥锁不可递归获取，本函数不再取锁）。 */
static void seq_rec_follow_locked(uint32_t track)
{
    if (song.rec) song.rec = (uint8_t)(1u << (track % NTRK));
}

static uint32_t rec_mask_track(uint8_t mask)
{
    for (uint32_t k = 0; k < NTRK; k++)
        if (mask & (1u << k)) return k;
    return 0;
}

void sloop_seq_rec_read(sloop_rec_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_frozen) return;
    engine_lock();
    if (!s_frozen) {
        out->quant_off = rec_quant_off;
        out->tempo_mode = rec_tempo;
        out->count_mode = rec_count;
        if (ft_on) {
            out->state = SEQ_REC_FREE_TAKE;
            out->track = (uint8_t)(ft_trk % NTRK);
        } else if (ci_on) {
            out->state = SEQ_REC_COUNT_IN;
            out->track = (uint8_t)(song.sel % NTRK);
        } else if (rec_wait) {
            out->state = SEQ_REC_ARMED;
            out->track = (uint8_t)(song.sel % NTRK);
        } else if (song.rec) {
            out->state = SEQ_REC_RECORDING;
            out->track = (uint8_t)rec_mask_track(song.rec);
        }
        out->take_bars = ft_bars;              /* 一次性：take 结果与开始闪示都由此消费 */
        ft_bars = 0;
        rec_go = 0;                            /* 投影里没有独立字段：ARMED→RECORDING 的
                                                * 状态迁移已表达「刚开始录音」；消费掉是为了
                                                * 不把一次性标志留给之后无关的读者 */
        out->fill_held = fill_held;            /* Task 6 deferral：FILL 表演态的设备观测口 */
        out->fill_arm = fill_arm;
    }
    engine_unlock();
}

esp_err_t sloop_seq_rec_toggle(void)
{
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (ft_on) {
        transport_req = 1;                     /* 自由录音进行中：REC 关闭它（上游 editor op 4） */
    } else if (ft_owns_press()) {
        /* 上游 rec_toggle 首行：刚关闭/丢弃 take 的那次按键（300ms 内）被吞掉，
         * 不 arm、不改 rec 位。Tab5 没有 fm1_ms 之外的按键去抖窗口。 */
    } else if (song.rec || rec_wait) {
        song.rec = 0;                          /* 再按一次：关录音 / 撤销待命，播放继续 */
        rec_wait = 0;
    } else if (song.playing) {
        rec_begin();                           /* 走带中：立即录当前选中轨 */
    } else {
        rec_wait = 1;                          /* 停止态：待命，首个音符（或 PLAY）启动 */
    }
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_rec_config(uint8_t quant_off, uint8_t tempo_mode, uint8_t count_mode)
{
    if (quant_off > 1u || tempo_mode > 1u || count_mode > 1u) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    rec_quant_off = quant_off;
    rec_tempo = tempo_mode;
    rec_count = count_mode;
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_fill_control(bool held, bool next_bar)
{
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* 上游两个独立变量（seq.c fill_held / fill_arm）：held 只由 FILL 保持键的
     * down/up 写；next_bar 是 NEXT BAR 按键的**脉冲**，只翻转 fill_arm，不读不写
     * held——否则按住 FILL 时点 NEXT 会清掉保持电平。 */
    if (next_bar)
        fill_arm = (uint8_t)!fill_arm;
    else
        fill_held = held ? 1u : 0u;
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_drum_hit(uint32_t lane, uint32_t velocity)
{
    if (lane >= DRUM_LANES || velocity > 127u) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (velocity == 0u) {
        if (ft_on && ft_trk == TRK_DRUM) ft_note_off(lane); /* 自由录音的鼓音符段到此为止 */
    } else {
        drum_input(lane, vel_lvl(velocity), 0, 1);
    }
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_play_button(bool down)
{
    if (!down) {
        /* 松开只清单个 volatile 位（上游 UI 也这样写 fm1_in.buttons）：不取引擎锁，
         * 因此在诊断冻结期间也能把位清干净，不会把按钮位挂住。 */
        fm1_in.buttons &= ~SLOOP_BTN_PLAY;
        return ESP_OK;
    }
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ft_masks_ensure();
    if (ft_owns_press()) {
        if (ft_on) fm1_in.buttons |= SLOOP_BTN_PLAY; /* 上游 ft_block 的上升沿：丢弃 take */
    } else {
        /* 上游 ui_studio.c 的 B_PLAY；arrangement 的「EMPTY SECTION」守卫归 Task 8
         * 的 SONG 入口（本任务不注册 arrangement 状态）。 */
        transport_req = song.playing ? 2 : 1;
    }
    engine_unlock();
    return ESP_OK;
}

esp_err_t sloop_seq_note_del(uint32_t track, uint32_t step, uint32_t note)
{
    if (track >= NTRK || step >= NSTEP || note == 0u || note > 127u) return ESP_ERR_INVALID_ARG;
    if (is_drum(&trk[track])) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    track_t *t = &trk[track];
    step_t *st = &t->step[step];
    if (step_on(st)) {
        uint32_t index = 0, n = st->n > 4u ? 4u : st->n;
        while (index < n && st->note[index] != note) index++;
        if (index < n) {
            step_sess = 0;
            layer_undo_mark(t);
            note_del(t, step, index);
            step_sess = 0;
            ret = ESP_OK;
        }
    }
    engine_unlock();
    return ret;
}

/* --------------------------------------- P4-C Task 8: sections / chain / SONG ---
 * 全部是上游 seq.c（FELUCCA_ARRANGER 段）与 arranger_scene.c 的运行态薄封装：
 * 不复制顺序时钟/场景应用/录音状态机，也不在渲染路径写 flash。公共边界只暴露 POD
 * 与 esp_err_t；越界在取锁前拒绝，冻结在取锁后复核（与 Task 3/4 同口径）。
 * 「正在播放」的门限返回 ESP_ERR_NOT_ALLOWED（console: blocked），
 * ESP_ERR_INVALID_STATE 只表示诊断冻结（与 sloop_seq.h 的全局约定一致）。 */

/* SONG REC 的结果（srec_done）由快照读取对账一次：一次 take 结束（含 STOP 收尾）后
 * 置 song dirty，延迟写才会把新歌曲顺序落盘。s_srec_marked 记住已对账的值，避免每拍
 * 30ms 的 sync 反复 mark 让 dirty 的「写成功才清」永远清不掉。 */
static uint8_t s_srec_marked;

static void song_result_reconcile(void)
{
    if (srec_done == s_srec_marked) return;
    s_srec_marked = srec_done;
    if (srec_done != 0u && srec_done != 0xFFu) sloop_store_song_dirty_mark();
}

void sloop_seq_song_read(sloop_song_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (s_frozen) return;
    engine_lock();
    if (!s_frozen) {
        out->enabled = arrangement_enabled ? 1u : 0u;
        out->running = arrangement_clock.running ? 1u : 0u;
        out->index = arrangement_clock.index;
        out->bar = arrangement_clock.bar;
        out->ready = (uint8_t)arrangement_ready();
        out->live = live_sec < 0 ? 0xFFu : (uint8_t)live_sec;
        out->chain_count = chain_n;
        out->chain_index = chain_i;
        out->chain_bars = chain_bars;
        for (uint32_t i = 0; i < 8u; i++) out->chain_scenes[i] = chain_sec[i];
        out->srec = srec;
        out->srec_done = srec_done;
        out->config.count = arrangement.count;
        out->config.loop = arrangement.loop;
        for (uint32_t i = 0; i < SLOOP_SONG_ENTRIES; i++) {
            out->config.entry[i].scene = arrangement.entry[i].scene;
            out->config.entry[i].bars = arrangement.entry[i].bars;
        }
    }
    engine_unlock();
    if (s_frozen) {
        memset(out, 0, sizeof(*out)); /* 取锁期间被冻结：不给半份投影 */
        return;
    }
    song_result_reconcile(); /* 先对账：本次 take 的 dirty 在同一份快照里可见 */
    out->dirty = (sloop_store_song_dirty() || sloop_store_sections_dirty()) ? 1u : 0u;
}

esp_err_t sloop_seq_section(uint32_t slot, bool store, bool erase)
{
    if (slot >= ARR_SCENES || (store && erase)) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    if (erase) return sloop_store_section_erase(slot);
    if (!store) return sloop_store_section_load(slot);
    /* live STORE：播放中也允许（上游 section_store 在 IRQ off 下 proj_capture）。
     * 捕获只改 RAM 槽与 live_sec；flash 由 1s 任务在停止+安静+idle 后锁外写。 */
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    proj_capture(&proj_slot[slot]);
    live_sec = (int8_t)slot;
    engine_unlock();
    sloop_store_section_dirty_mark(slot);
    return ESP_OK;
}

esp_err_t sloop_seq_section_request(uint32_t slot)
{
    if (slot >= ARR_SCENES) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (!song.playing || arrangement_clock.running) {
        ret = ESP_ERR_NOT_ALLOWED; /* SONG 播放中（SONG PLAYS）或没有可切换的 live loop */
    } else if (!(arrangement_ready() & (1u << slot))) {
        ret = ESP_ERR_NOT_FOUND; /* EMPTY <section> */
    } else {
        chain_n = 0; /* 上游第一次 section tap：任何 chain 到此为止 */
        live_req = (int8_t)slot;
    }
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_chain_begin(uint32_t first)
{
    if (first >= ARR_SCENES) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (!song.playing || arrangement_clock.running) {
        ret = ESP_ERR_NOT_ALLOWED; /* chain 只属于 live loop，不属于 SONG */
    } else if (!(arrangement_ready() & (1u << first))) {
        ret = ESP_ERR_NOT_FOUND;
    } else {
        chain_n = 0; /* 单 section tap 同时是「取消 chain」：A 继续播放，chain 不再回来 */
        live_req = (int8_t)first;
    }
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_chain_commit(const uint8_t *scenes, uint32_t count)
{
    if (count > CHAIN_MAX || (count != 0u && scenes == NULL)) return ESP_ERR_INVALID_ARG;
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (!song.playing || arrangement_clock.running) {
        ret = ESP_ERR_NOT_ALLOWED;
    } else {
        /* 先整份校验：半条 chain 不得装上（空/越界 scene 原子拒绝） */
        uint32_t ready = arrangement_ready();
        for (uint32_t i = 0; i < count && ret == ESP_OK; i++) {
            if (scenes[i] >= ARR_SCENES)
                ret = ESP_ERR_INVALID_ARG;
            else if (!(ready & (1u << scenes[i])))
                ret = ESP_ERR_NOT_FOUND;
        }
        if (ret == ESP_OK) {
            if (count == 0u) {
                chain_n = 0; /* cancel：保留已排队的首项请求（单 section tap 的语义） */
            } else {
                for (uint32_t i = 0; i < count; i++) chain_sec[i] = (uint8_t)(scenes[i] & 3u);
                chain_i = 0;
                chain_bars = (uint8_t)section_bars(scenes[0]); /* 首项已在播放/被请求 */
                chain_n = (uint8_t)count;
            }
        }
    }
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_song_mode(bool enabled)
{
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (srec)
        ret = ESP_ERR_NOT_ALLOWED; /* 上游 UI："REC IS ON" */
    else
        arrangement_enabled = enabled ? 1u : 0u;
    engine_unlock();
    return ret;
}

esp_err_t sloop_seq_song_rec(void)
{
    if (s_frozen) return ESP_ERR_INVALID_STATE;
    engine_lock();
    if (s_frozen) {
        engine_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    if (srec) {
        /* 再按一次 / STOP：录到的顺序写入歌曲（空录制 = NO SONG，不覆盖原歌曲） */
        srec_stop();
    } else if (arrangement_clock.running) {
        ret = ESP_ERR_NOT_ALLOWED; /* 上游 UI："STOP FIRST" */
    } else {
        arrangement_enabled = 0; /* SONG REC 只录 live sections（上游强制 loop 模式） */
        srec_done = 0;           /* 新一轮 take：清掉上一次结果的显示/对账 */
        srec = 1;                /* 下一个小节起录（live_block 的 srec==1 分支） */
    }
    engine_unlock();
    if (ret == ESP_OK) song_result_reconcile();
    return ret;
}
