#include "sloop_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sloop_hal.h"

#include "golden.inc"

#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC
#endif

typedef struct {
    uint8_t *start;
    size_t size;
    uint8_t *snapshot;
} selftest_region_t;

typedef struct {
    const char *selector;
    selftest_region_t region[2];
    void *results;
    uint8_t *crashed;
    uint8_t valid[512 / 8];
    uint32_t fork_count;
    uint32_t read_index;
    int recapture;
    int skip;
    int pipe_valid;
    size_t pipe_size;
    uint8_t pipe_data[1024];
} selftest_control_t;

static selftest_control_t ctl __attribute__((section(".noinit")));

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} selftest_file_t;

static selftest_file_t golden_file;

static int selftest_printf(const char *fmt, ...);
static FILE *selftest_fopen(const char *path, const char *mode);
static char *selftest_fgets(char *line, int size, FILE *stream);
static int selftest_fclose(FILE *stream);
static int selftest_pipe(int fd[2]);
static int selftest_close(int fd);
static pid_t selftest_fork(void);
static ssize_t selftest_write(int fd, const void *data, size_t size);
static ssize_t selftest_read(int fd, void *data, size_t size);
static pid_t selftest_waitpid(pid_t pid, int *status, int options);
static void selftest_exit(int status);
static FILE *selftest_popen(const char *cmd, const char *mode);
static int selftest_pclose(FILE *stream);
static char *selftest_getenv(const char *name);

static uint32_t selftest_data_begin __attribute__((section(".data.0_begin"))) = 0x11223344u;

#define printf selftest_printf
#define fopen selftest_fopen
#define fgets selftest_fgets
#define fclose selftest_fclose
#define pipe selftest_pipe
#define close selftest_close
#define fork selftest_fork
#define write selftest_write
#define read selftest_read
#define waitpid selftest_waitpid
#define _exit selftest_exit
#define popen selftest_popen
#define pclose selftest_pclose
#define getenv selftest_getenv
#define SMP_USER_XIP(k) \
    (sloop_hal_user_samples() + ((k) & (SLOOP_HAL_USER_SLOT_COUNT - 1u)) * SLOOP_HAL_USER_SLOT_SIZE)
#define felucca_memset selftest_felucca_memset
#define felucca_memcpy selftest_felucca_memcpy
#define felucca_memcmp selftest_felucca_memcmp
#define bootguard selftest_bootguard
#include "regress.c"
#undef printf
#undef fopen
#undef fgets
#undef fclose
#undef pipe
#undef close
#undef fork
#undef write
#undef read
#undef waitpid
#undef _exit
#undef popen
#undef pclose
#undef getenv
#undef SMP_USER_XIP
#undef felucca_memset
#undef felucca_memcpy
#undef felucca_memcmp
#undef bootguard
#undef __attribute__

static uint32_t selftest_data_end __attribute__((section(".data.z_end"))) = 0x99aabbccu;

extern uint8_t _sloop_selftest_bss_start, _sloop_selftest_bss_end;

static int selftest_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

static FILE *selftest_fopen(const char *path, const char *mode)
{
    if (strcmp(mode, "r") != 0 || strcmp(path, "tests/golden.txt") != 0) return NULL;
    golden_file.data = sloop_golden_txt;
    golden_file.size = sizeof(sloop_golden_txt);
    golden_file.pos = 0;
    return (FILE *)&golden_file;
}

static char *selftest_fgets(char *line, int size, FILE *stream)
{
    selftest_file_t *file = (selftest_file_t *)stream;
    if (file != &golden_file || file->pos >= file->size || size <= 0) return NULL;
    int n = 0;
    while (n + 1 < size && file->pos < file->size) {
        char c = (char)file->data[file->pos++];
        line[n++] = c;
        if (c == '\n') break;
    }
    line[n] = '\0';
    return line;
}

static int selftest_fclose(FILE *stream)
{
    (void)stream;
    return 0;
}

static int selftest_pipe(int fd[2])
{
    fd[0] = 0;
    fd[1] = 1;
    ctl.pipe_valid = 0;
    ctl.pipe_size = 0;
    return 0;
}

static void selftest_capture(void)
{
    for (uint32_t i = 0; i < 2; i++) {
        memcpy(ctl.region[i].snapshot, ctl.region[i].start, ctl.region[i].size);
    }
}

static void selftest_restore(void)
{
    for (uint32_t i = 0; i < 2; i++) {
        memcpy(ctl.region[i].start, ctl.region[i].snapshot, ctl.region[i].size);
    }
}

static int selftest_close(int fd)
{
    (void)fd;
    return 0;
}

static pid_t selftest_fork(void)
{
    uint32_t index = ctl.fork_count++;
    ctl.skip = 1;
    if (index >= nj || J[index].kind == J_CPU) return 1;
    if (ctl.selector != NULL && strcmp(ctl.selector, J[index].name) != 0) return 1;
    if (ctl.recapture) {
        selftest_capture();
        ctl.recapture = 0;
    } else {
        selftest_restore();
    }
    ctl.skip = 0;
    return 0;
}

static ssize_t selftest_write(int fd, const void *data, size_t size)
{
    (void)fd;
    if (size > sizeof(ctl.pipe_data)) return -1;
    memcpy(ctl.pipe_data, data, size);
    ctl.pipe_size = size;
    ctl.pipe_valid = 1;
    return (ssize_t)size;
}

static ssize_t selftest_read(int fd, void *data, size_t size)
{
    (void)fd;
    if (ctl.skip || !ctl.pipe_valid) return 0;
    size_t n = size < ctl.pipe_size ? size : ctl.pipe_size;
    memcpy(data, ctl.pipe_data, n);
    ctl.pipe_size -= n;
    if (n == sizeof(res_t)) {
        const job_t *job = (const job_t *)((uint8_t *)data - offsetof(job_t, r));
        ctl.read_index = (uint32_t)(job - J);
        if (ctl.read_index < MAXJ) {
            res_t *results = ctl.results;
            results[ctl.read_index] = *(const res_t *)data;
            ctl.valid[ctl.read_index / 8] |= (uint8_t)(1u << (ctl.read_index % 8));
        }
    }
    return (ssize_t)n;
}

static pid_t selftest_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    if (ctl.skip) {
        *status = 1 << 8;
        return pid;
    }
    *status = 0;
    if (ctl.read_index < MAXJ) {
        res_t *results = ctl.results;
        for (uint32_t i = 0; i < nj; i++) {
            if ((ctl.valid[i / 8] & (1u << (i % 8))) != 0) {
                J[i].r = results[i];
                J[i].crashed = ctl.crashed[i];
            }
        }
    }
    vTaskDelay(1);
    return pid;
}

static void selftest_exit(int status)
{
    (void)status;
}

static FILE *selftest_popen(const char *cmd, const char *mode)
{
    (void)cmd;
    (void)mode;
    return NULL;
}

static int selftest_pclose(FILE *stream)
{
    (void)stream;
    return 0;
}

static char *selftest_getenv(const char *name)
{
    return strcmp(name, "JOBS") == 0 ? "1" : NULL;
}

static int selftest_health(const res_t *r)
{
    return r->over == 0 && r->near == 0 && fabs(r->dc) <= LIM_DC && r->peak >= LIM_PEAK_MIN &&
           r->free_s >= 0 && abs(r->tail_dc) <= LIM_TAIL_DC && r->tail_peak <= LIM_TAIL && r->vmax <= NVOICE;
}

static esp_err_t selftest_snapshot_init(void)
{
    uint8_t *data_first = (uint8_t *)&selftest_data_begin;
    uint8_t *data_last = (uint8_t *)&selftest_data_end;
    ctl.region[0].start = data_first < data_last ? data_first : data_last;
    ctl.region[0].size = (size_t)(data_first < data_last ? data_last - data_first : data_first - data_last) +
                         sizeof(uint32_t);
    ctl.region[1].start = &_sloop_selftest_bss_start;
    ctl.region[1].size = (size_t)(&_sloop_selftest_bss_end - &_sloop_selftest_bss_start);
    for (uint32_t i = 0; i < 2; i++) {
        ctl.region[i].snapshot = heap_caps_malloc(ctl.region[i].size, MALLOC_CAP_SPIRAM);
        if (ctl.region[i].snapshot == NULL) return ESP_ERR_NO_MEM;
    }
    ctl.results = heap_caps_calloc(MAXJ, sizeof(res_t), MALLOC_CAP_SPIRAM);
    ctl.crashed = heap_caps_calloc(MAXJ, 1, MALLOC_CAP_SPIRAM);
    if (ctl.results == NULL || ctl.crashed == NULL) return ESP_ERR_NO_MEM;
    selftest_capture();
    return ESP_OK;
}

_Static_assert(SLOOP_CORE_NENGINES == NENGINES, "NENGINES drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_P_COUNT == P_COUNT, "P_COUNT drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_G_COUNT == G_COUNT, "G_COUNT drifted from the public smoke-test constant");

static esp_err_t core_status = ESP_ERR_INVALID_STATE;

/* 引擎状态锁：上游引擎在 FM-1 上是单线程（输入中断写 volatile 输入结构、音频
 * ISR 渲染）。Tab5 上渲染在音频任务、编辑（TRIG/TRACK）在 console 任务——所有
 * 会改变引擎状态的 API 与 sloop_core_render 共用此锁（所有权规则见 sloop_core.h）。
 * 持锁窗口都是短操作（编辑为状态写，渲染为 ≤ HALF_FRAMES 帧的量化渲染），
 * 优先级继承保证 console（低优先级）持锁时音频任务不会被无限阻塞。 */
static SemaphoreHandle_t s_engine_lock;

static inline void engine_lock(void)
{
    if (s_engine_lock != NULL) xSemaphoreTake(s_engine_lock, portMAX_DELAY);
}

static inline void engine_unlock(void)
{
    if (s_engine_lock != NULL) xSemaphoreGive(s_engine_lock);
}

esp_err_t sloop_core_init(void)
{
    core_status = sloop_hal_init();
    if (core_status != ESP_OK) return core_status;
    memset(sloop_hal_user_samples(), 0xFF, SLOOP_HAL_USER_SLOT_COUNT * SLOOP_HAL_USER_SLOT_SIZE);
    for (uint32_t i = 0; i < G_COUNT; i++) song.g[i] = GP[i].def;
    song.g[G_SYNC] = 0;
    song.master_q12 = 4096;
    for (uint32_t k = 0; k < NTRK; k++) {
        for (uint32_t i = 0; i < P_COUNT; i++) trk[k].p[i] = TP[i].def;
        steps_clear(&trk[k]);
    }
    dsu_defaults();
    fm6_init();
    memset(&ctl, 0, sizeof(ctl));
    if (s_engine_lock == NULL) {
        s_engine_lock = xSemaphoreCreateMutex();
        if (s_engine_lock == NULL) {
            core_status = ESP_ERR_NO_MEM;
            return core_status;
        }
    }
    core_status = selftest_snapshot_init();
    return core_status;
}

esp_err_t sloop_core_status(void)
{
    return core_status;
}

void sloop_core_render(int32_t *block, uint32_t frames)
{
    engine_lock();
    fm6_poll();
    for (uint32_t b = 0; b < frames; b += CTL) {
        uint32_t n = frames - b < CTL ? frames - b : CTL;
        mix_block(block + 2u * b, n);
    }
    engine_unlock();
}

void sloop_core_track(uint32_t k, uint32_t engine, uint32_t preset)
{
    if (k >= NPART || engine >= NENGINES) return;
    engine_lock();
    track_t *t = &trk[k];
    if (t->eng_req != engine || t->preset != preset) sound_change(t);
    t->eng_req = (uint8_t)engine;
    t->preset = (uint8_t)preset;
    engine_unlock();
}

void sloop_core_note(uint32_t track, uint32_t note, bool on)
{
    if (track >= NTRK || note > 127) return;
    engine_lock();
    if (on) input_on(&trk[track], note, 100);
    else input_off(&trk[track], note);
    engine_unlock();
}

void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity)
{
    if (kit >= DSU_N || lane >= DRUM_LANES) return;
    engine_lock();
    int16_t old = TDRUM->p[P_E0];
    TDRUM->p[P_E0] = (int16_t)(DRUM_SYN + kit);
    drum_on(LANE_NOTE[lane], (uint8_t)clamp(velocity, 1, 127));
    TDRUM->p[P_E0] = old;
    engine_unlock();
}

void sloop_core_play(bool on)
{
    engine_lock();
    transport_req = on ? 1 : 2;
    engine_unlock();
}

uint32_t sloop_core_rate(void)
{
    return FS;
}

uint32_t sloop_core_frames(void)
{
    return HALF_FRAMES;
}

int sloop_core_selftest(const char *case_name)
{
    if (sloop_hal_audio_streaming() || ctl.results == NULL) {
        printf("ENG SELFTEST cases=0 result=FAIL reason=busy_or_uninitialized\n");
        return 1;
    }
    UBaseType_t console_priority = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 0);
    selftest_restore();
    ctl.selector = case_name;
    ctl.fork_count = 0;
    ctl.recapture = 1;
    memset(ctl.valid, 0, sizeof(ctl.valid));
    memset(ctl.crashed, 0, MAXJ);
    nj = 0;
    char *argv[] = {"regress", "tests/golden.txt", "tests/cpu_baseline.txt"};
    printf("ENG SELFTEST BEGIN%s%s\n", case_name ? " " : "", case_name ? case_name : "");
    main(3, argv);

    kv_t *golden = heap_caps_calloc(MAXJ, sizeof(*golden), MALLOC_CAP_SPIRAM);
    uint32_t ng = golden != NULL ? load_kv("tests/golden.txt", golden, MAXJ) : 0;
    uint32_t cases = 0, identical = 0, mismatch = 0, health_fail = 0, checks = 0, check_fail = 0;
    for (uint32_t i = 0; i < nj; i++) {
        job_t *j = &J[i];
        if (case_name != NULL && strcmp(case_name, j->name) != 0) continue;
        if (j->kind == J_CHECK) {
            checks++;
            check_fail += j->crashed || !j->r.ok;
            continue;
        }
        if (j->kind == J_CPU || strcmp(j->name, "repeat") == 0) continue;
        cases++;
        printf("%s %016llx\n", j->name, (unsigned long long)j->r.hash);
        const char *want = kv_get(golden, ng, j->name);
        char hash[24];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)j->r.hash);
        if (want != NULL && strcmp(want, hash) == 0 && !j->crashed) identical++;
        else mismatch++;
        if (j->crashed || !selftest_health(&j->r)) {
            health_fail++;
            printf("ENG SELFTEST HEALTH %s peak=%d dc=%.1f free=%.2f tail=%d tail_dc=%d over=%u near=%u crashed=%d\n",
                   j->name, (int)j->r.peak, j->r.dc, j->r.free_s, (int)j->r.tail_peak, (int)j->r.tail_dc,
                   (unsigned)j->r.over, (unsigned)j->r.near, j->crashed);
        }
    }
    int deterministic = 1;
    if (case_name == NULL) {
        deterministic = 0;
        for (uint32_t i = 0; i < nj; i++) {
            if (strcmp(J[i].name, "repeat") == 0) {
                deterministic = !J[i].crashed && J[i].r.hash == J[0].r.hash;
                break;
            }
        }
    }
    int pass = cases > 0 && mismatch == 0 && health_fail == 0 && check_fail == 0 && deterministic && golden != NULL;
    printf("ENG SELFTEST cases=%u identical=%u mismatch=%u health_fail=%u checks=%u check_fail=%u deterministic=%d result=%s\n",
           (unsigned)cases, (unsigned)identical, (unsigned)mismatch, (unsigned)health_fail,
           (unsigned)checks, (unsigned)check_fail, deterministic, pass ? "PASS" : "FAIL");
    heap_caps_free(golden);
    selftest_restore();
    nj = 0;
    vTaskPrioritySet(NULL, console_priority);
    return pass ? 0 : 1;
}
