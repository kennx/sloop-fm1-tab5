#include "sloop_core.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloop_hal.h"

#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp
#include "felucca_tables.h"
#define SMP_USER_XIP(k) \
    (sloop_hal_user_samples() + ((k) & (SLOOP_HAL_USER_SLOT_COUNT - 1u)) * SLOOP_HAL_USER_SLOT_SIZE)
static struct { volatile uint32_t notes, buttons; } fm1_in;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "core.h"
#include "engines.c"
#include "drums.c"
#include "params.c"
#include "voice.c"
#include "slicer.c"
#include "cpu1.h"
static int cpu1_run(cpu1_job_t fn, uint32_t arg) { (void)fn; (void)arg; return 0; }
static int cpu1_wait(void) { return 0; }
static int cpu1_on(void) { return 0; }
static uint32_t cpu1_ticks(void) { return 0; }
#include "fx.c"
static void fm1_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#include "usb.c"
#include "midi_uart.c"
#include "seq.c"

_Static_assert(SLOOP_CORE_NENGINES == NENGINES, "NENGINES drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_P_COUNT == P_COUNT, "P_COUNT drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_G_COUNT == G_COUNT, "G_COUNT drifted from the public smoke-test constant");

static esp_err_t core_status = ESP_ERR_INVALID_STATE;

esp_err_t sloop_core_init(void)
{
    uint32_t i, k;
    core_status = sloop_hal_init();
    if (core_status != ESP_OK) return core_status;
    memset(sloop_hal_user_samples(), 0xFF, SLOOP_HAL_USER_SLOT_COUNT * SLOOP_HAL_USER_SLOT_SIZE);
    for (i = 0; i < G_COUNT; i++) song.g[i] = GP[i].def;
    song.g[G_SYNC] = 0;
    song.master_q12 = 4096;
    for (k = 0; k < NTRK; k++) {
        for (i = 0; i < P_COUNT; i++) trk[k].p[i] = TP[i].def;
        steps_clear(&trk[k]);
    }
    dsu_defaults();
    fm6_init();
    core_status = ESP_OK;
    return core_status;
}

esp_err_t sloop_core_status(void)
{
    return core_status;
}

void sloop_core_render(int32_t *block, uint32_t frames)
{
    fm6_poll();
    mix_block(block, frames);
}

void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset)
{
    if (k >= NPART || engine >= NENGINES) return;
    track_t *t = &trk[k];
    if (t->eng_req != engine || t->preset != preset) sound_change(t);
    t->eng_req = (uint8_t)engine;
    t->preset = (uint8_t)preset;
}

void sloop_core_note(uint32_t track, uint32_t note, bool on)
{
    if (track >= NTRK || note > 127) return;
    if (on) input_on(&trk[track], note, 100);
    else input_off(&trk[track], note);
}

void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity)
{
    if (kit >= DSU_N || lane >= DRUM_LANES) return;
    int16_t old = TDRUM->p[P_E0];
    TDRUM->p[P_E0] = (int16_t)(DRUM_SYN + kit);
    drum_on(LANE_NOTE[lane], (uint8_t)clamp(velocity, 1, 127));
    TDRUM->p[P_E0] = old;
}

void sloop_core_play(bool on)
{
    transport_req = on ? 1 : 2;
}

uint32_t sloop_core_rate(void)
{
    return FS;
}

uint32_t sloop_core_frames(void)
{
    return HALF_FRAMES;
}
