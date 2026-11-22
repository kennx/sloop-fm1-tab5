/* P4-C Task 3：上游 seq.c/drums.c 的公共薄适配；由 selftest.c 末尾 include。 */
#include "sloop_seq.h"

#include <string.h>

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
