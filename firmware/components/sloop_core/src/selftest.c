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
/* hostsim.c 顶部 `#define __attribute__(x)` 把属性清空（让 .pool/.lowram 缓冲落到
 * .bss，host 侧没有这两段）；regress.c 包含它后该空宏仍在本 TU 生效，直到此处。
 * 不清除的话下面 selftest_data_begin/end 的 section 属性被吞掉，两个哨兵落进普通
 * .data/.bss（位置不再包住 selftest 段），快照窗口错位、SELFTEST 结果失真。 */
#undef __attribute__

static uint32_t selftest_data_end __attribute__((section(".data.z_end"))) = 0x99aabbccu;

extern uint8_t _sloop_selftest_bss_start, _sloop_selftest_bss_end;

static int selftest_printf(const char *fmt, ...)
{
    /* 静默上游诊断输出，唯独放行 regress.c main() 末尾的汇总行（含
     * changed/gone、health、voice、CPU、crash 计数）：它是 main_rc 的唯一解释，
     * 出错时设备侧必须看得到原因。哈希行与 ENG SELFTEST 汇总行由本文件另印，
     * 格式不受影响（scripts/device-selftest.py 的解析面不变）。 */
    if (strstr(fmt, "golden renders") == NULL) return 0;
    va_list ap;
    va_start(ap, fmt);
    int result = vprintf(fmt, ap);
    va_end(ap);
    return result;
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
    /* 回填本次派发的 job 的结果（JOBS=1，read_index 只在完整交付时置位）。
     * crashed 不在 shim 里判定：上游 run_jobs 随后用 `got != sizeof(res_t)` 自行
     * 赋值，而本 shim 的 read 只在整块 res_t 交付时返回数据，语义与 host 一致。 */
    res_t *results = ctl.results;
    uint32_t index = ctl.read_index;
    if (index < nj && index < MAXJ && (ctl.valid[index / 8] & (1u << (index % 8))) != 0) {
        J[index].r = results[index];
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

/* 快照窗口的合理上限（DSP 状态 + 编辑表远小于 4MB；超出说明哨兵属性丢失、
 * 两个符号落进了别的段，memcpy 会把无辜内存卷进来） */
#define SELFTEST_REGION_MAX (4u * 1024u * 1024u)

/* vendored regress.c 里 exactly 7 条 J_CHECK（声音判定：peak/dc/free/tail/tail_dc/
 * over+near/vmax）。PASS 要求全量跑的 checks 恰好等于它——只断言 check_fail==0 的话，
 * 上游用例表若静默丢掉检查（改名/改 kind），0 条检查也会「通过」。单例模式被
 * selector 过滤掉全部 J_CHECK，故 expected_checks=0（汇总行打印该值）。
 * golden/regress 升级导致数量变化时同步改这里。 */
#define SELFTEST_EXPECTED_CHECKS 7u

static esp_err_t selftest_region_init(selftest_region_t *region, uint8_t *first, uint8_t *last)
{
    uint8_t *lo = first < last ? first : last;
    uint8_t *hi = first < last ? last : first;
    if (hi <= lo + sizeof(uint32_t)) return ESP_ERR_INVALID_STATE;
    region->start = lo;
    region->size = (size_t)(hi - lo) + sizeof(uint32_t);
    if (region->size > SELFTEST_REGION_MAX) return ESP_ERR_INVALID_STATE;
    region->snapshot = heap_caps_malloc(region->size, MALLOC_CAP_SPIRAM);
    return region->snapshot != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t selftest_snapshot_init(void)
{
    esp_err_t ret = selftest_region_init(&ctl.region[0], (uint8_t *)&selftest_data_begin,
                                         (uint8_t *)&selftest_data_end);
    if (ret != ESP_OK) return ret;
    ret = selftest_region_init(&ctl.region[1], &_sloop_selftest_bss_start, &_sloop_selftest_bss_end);
    if (ret != ESP_OK) {
        heap_caps_free(ctl.region[0].snapshot);
        ctl.region[0].snapshot = NULL;
        return ret;
    }
    ctl.results = heap_caps_calloc(MAXJ, sizeof(res_t), MALLOC_CAP_SPIRAM);
    if (ctl.results == NULL) return ESP_ERR_NO_MEM;
    selftest_capture();
    return ESP_OK;
}

_Static_assert(SLOOP_CORE_NENGINES == NENGINES, "NENGINES drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_P_COUNT == P_COUNT, "P_COUNT drifted from the public smoke-test constant");
_Static_assert(SLOOP_CORE_G_COUNT == G_COUNT, "G_COUNT drifted from the public smoke-test constant");
/* 采样率/块尺寸的三处表述（引擎 FS/HALF_FRAMES、公开 API、HAL 音频流）必须一致 */
_Static_assert(SLOOP_CORE_RATE == FS, "SLOOP_CORE_RATE drifted from the engine's FS");
_Static_assert(SLOOP_CORE_FRAMES == HALF_FRAMES, "SLOOP_CORE_FRAMES drifted from the engine's HALF_FRAMES");
_Static_assert(SLOOP_HAL_AUDIO_RATE == SLOOP_CORE_RATE, "HAL 音频流采样率与引擎不一致");
_Static_assert(SLOOP_HAL_AUDIO_FRAMES == SLOOP_CORE_FRAMES, "HAL 音频流块尺寸与引擎不一致");

static esp_err_t core_status = ESP_ERR_INVALID_STATE;

/* 引擎状态锁：上游引擎在 FM-1 上是单线程（输入中断写 volatile 输入结构、音频
 * ISR 渲染）。Tab5 上渲染在音频任务、编辑（TRIG/TRACK）在 console 任务——所有
 * 会改变引擎状态的 API 与 sloop_core_render 共用此锁（所有权规则见 sloop_core.h）。
 * 持锁窗口都是短操作（编辑为状态写，渲染为 ≤ HALF_FRAMES 帧的量化渲染），
 * 优先级继承保证 console（低优先级）持锁时音频任务不会被无限阻塞。 */
static SemaphoreHandle_t s_engine_lock;

/* 诊断模式（sloop_core_diag_freeze）：冻结快照 + 标志。放在 .noinit —— selftest_restore()
 * 会把组件 data/bss 恢复到 init 快照，普通静态标志会在自检一开始就被回滚成 false。 */
static sloop_core_snapshot_t s_frozen_snap __attribute__((section(".noinit")));
static volatile bool s_frozen __attribute__((section(".noinit")));
/* 诊断期静默钩子（sloop_ui 启动后注册）。同样放 .noinit：注册发生在 sloop_core_init
 * 的快照之后，普通静态会被 selftest_restore() 抹成 NULL——结束时 quiesce(false)
 * 就永远不会执行，UI 定时器将永久停在暂停态。 */
static void (*s_quiesce_hook)(bool on) __attribute__((section(".noinit")));

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
    /* .noinit：冷启动内容未定义，必须显式清诊断冻结标志（否则写 API 可能全被丢弃） */
    s_frozen = false;
    s_quiesce_hook = NULL;
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

/* 渲染不查 s_frozen：正常唯一调用方是音频任务，而自检的前置条件就是「流已停」
 * （`ENG SELFTEST` 在流式时拒绝），诊断期 UI 定时器/LVGL 定时器/输入又全部静默
 * （sloop_ui_quiesce）——自检期间本函数不会被调用，「冻结后没有新的锁使用者」
 * 因此成立。diagnostic 的 SMOKE/PERF 也只在非自检期间由 console 调用。 */
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
    if (k >= NPART || engine >= NENGINES || s_frozen) return;
    engine_lock();
    track_t *t = &trk[k];
    if (t->eng_req != engine || t->preset != preset) sound_change(t);
    t->eng_req = (uint8_t)engine;
    t->preset = (uint8_t)preset;
    engine_unlock();
}

void sloop_core_note(uint32_t track, uint32_t note, bool on)
{
    if (track >= NTRK || note > 127 || s_frozen) return;
    engine_lock();
    if (on) input_on(&trk[track], note, 100);
    else input_off(&trk[track], note);
    engine_unlock();
}

void sloop_core_hit(uint32_t kit, uint32_t lane, uint32_t velocity)
{
    if (kit >= DSU_N || lane >= DRUM_LANES || s_frozen) return;
    engine_lock();
    int16_t old = TDRUM->p[P_E0];
    TDRUM->p[P_E0] = (int16_t)(DRUM_SYN + kit);
    drum_on(LANE_NOTE[lane], (uint8_t)clamp(velocity, 1, 127));
    TDRUM->p[P_E0] = old;
    engine_unlock();
}

void sloop_core_play(bool on)
{
    if (s_frozen) return;
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

/* --------------------------------------------------- 快照与步读写 --- */
/* 只读快照 / 单步读写 / 全局参数写（语义与字段映射见 sloop_core.h）。
 * 与其它会改变引擎状态的 API 共用 s_engine_lock；窗口长度取 LEN 的合法区间，
 * 播放头按「模 len」给（停止时为 0）。 */
/* 填一份快照（调用方持引擎锁或处于冻结态） */
static void snapshot_fill(sloop_core_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->playing = song.playing != 0;
    out->bpm = (uint16_t)clamp(song.g[G_BPM], GP[G_BPM].min, GP[G_BPM].max);
    out->selected_track = song.sel < NTRK ? song.sel : 0;
    for (uint32_t k = 0; k < NTRK; k++) {
        const track_t *t = &trk[k];
        uint32_t len = (uint32_t)clamp(t->p[P_SLEN], 1, NSTEP);
        /* engine 取 eng_req（UI 要求的引擎；预设淡出期间 engine 仍是旧的） */
        out->trk[k].engine = t->eng_req;
        out->trk[k].preset = t->preset;
        out->trk[k].level = (uint8_t)clamp(t->p[P_LEVEL], 0, 127);
        out->trk[k].mute = t->p[P_MUTE] != 0;
        out->trk[k].armed = (song.rec >> k) & 1u;
        out->trk[k].solo = (song.solo >> k) & 1u;
        out->trk[k].len = (uint8_t)len;
        out->trk[k].div = (uint8_t)clamp(t->p[P_SDIV], TP[P_SDIV].min, TP[P_SDIV].max);
        out->step_pos[k] = (uint8_t)(song.playing ? t->seq_idx % len : 0u);
    }
    for (uint32_t k = 0; k < NPART; k++) {
        for (uint32_t i = 0; i < 16; i++) {
            const step_t *s = &trk[k].step[i];
            out->step[k][i].n = s->n;
            memcpy(out->step[k][i].note, s->note, sizeof(out->step[k][i].note));
            out->step[k][i].vel = s->vel;
            out->step[k][i].time = s->time;
            out->step[k][i].flags = s->flags;
        }
    }
    /* 鼓轨聚合：on = 任一声部触发；lvl/rat 取触发声部里的最强值（显示用） */
    for (uint32_t i = 0; i < 16; i++) {
        const dstep_t *d = &TDRUM->dstep[i];
        uint32_t on = (uint32_t)(d->on[0] | d->on[1]);
        out->dstep[i].on = on != 0;
        for (uint32_t lane = 0; lane < DRUM_LANES; lane++) {
            if (!((on >> lane) & 1u)) continue;
            uint8_t lvl = (d->lvl[lane / 4u] >> (2u * (lane % 4u))) & 3u;
            uint8_t rat = (d->rat[lane / 4u] >> (2u * (lane % 4u))) & 3u;
            if (lvl > out->dstep[i].lvl) out->dstep[i].lvl = lvl;
            if (rat > out->dstep[i].rat) out->dstep[i].rat = rat;
        }
    }
}

void sloop_core_read(sloop_core_snapshot_t *out)
{
    if (out == NULL) return;
    if (s_frozen) {
        *out = s_frozen_snap;
        return;
    }
    engine_lock();
    /* 取到锁后再查一次：冻结在持锁时置位，故这里要么在冻结前完成，要么看到标志
     * 直接返回缓存——任何读者都不可能长时间卡在锁上（见 sloop_core_diag_freeze）。 */
    if (s_frozen) {
        engine_unlock();
        *out = s_frozen_snap;
        return;
    }
    snapshot_fill(out);
    engine_unlock();
}

void sloop_core_read_step(uint32_t track, uint32_t idx, sloop_step_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (track >= NPART || idx >= NSTEP || s_frozen) return; /* 冻结态：不取锁，返回全零 */
    engine_lock();
    const track_t *t = &trk[track];
    const step_t *s = &t->step[idx];
    out->n = s->n;
    memcpy(out->note, s->note, sizeof(out->note));
    out->vel = s->vel;
    out->time = s->time;
    out->flags = s->flags;
    out->lvl = s->lvl;
    out->rat = s->rat;
    out->micro = t->micro[idx];
    out->fill = (uint8_t)((t->fill[idx / 4u] >> (2u * (idx % 4u))) & 3u);
    engine_unlock();
}

void sloop_core_write_step(uint32_t track, uint32_t idx, const sloop_step_t *in)
{
    if (in == NULL || track >= NPART || idx >= NSTEP || s_frozen) return;
    engine_lock();
    track_t *t = &trk[track];
    step_t *s = &t->step[idx];
    s->n = in->n > 4u ? 4u : in->n;      /* 上游按 n<=4 解读（POLY），超出部分引擎不会读 */
    memcpy(s->note, in->note, sizeof(s->note));
    s->vel = in->vel;
    s->time = in->time;
    s->flags = in->flags;
    s->lvl = in->lvl;                    /* 每音符 2bit 打包，原样保留 */
    s->rat = in->rat;
    t->micro[idx] = (int8_t)clamp(in->micro, MICRO_MIN, MICRO_MAX);
    uint32_t sh = 2u * (idx % 4u);
    t->fill[idx / 4u] = (uint8_t)((t->fill[idx / 4u] & ~(3u << sh)) | ((uint32_t)(in->fill & 3u) << sh));
    engine_unlock();
}

void sloop_core_set_global(uint32_t id, int16_t value)
{
    if (id >= G_COUNT || s_frozen) return;
    engine_lock();
    song.g[id] = (int16_t)clamp(value, GP[id].min, GP[id].max);
    engine_unlock();
}

void sloop_core_set_mute(uint32_t k, bool mute)
{
    if (k >= NTRK || s_frozen) return;
    engine_lock();
    trk[k].p[P_MUTE] = (int16_t)(mute ? 1 : 0); /* 上游 N_ONOFF：0/1 */
    engine_unlock();
}

void sloop_core_set_solo(uint8_t mask)
{
    if (s_frozen) return;
    engine_lock();
    song.solo = (uint8_t)(mask & 0xFu); /* 每轨 1 位，低 NTRK 位 */
    engine_unlock();
}

void sloop_core_select(uint32_t k)
{
    if (k >= NTRK || s_frozen) return;
    engine_lock();
    song.sel = (uint8_t)k;
    engine_unlock();
}

/* 诊断模式开关（`ENG SELFTEST` 内部成对调用，见 sloop_core.h）：
 *  - freeze(true)：持引擎锁取一份快照后置位。置位后 read 直接返回该快照、
 *    read_step 返回全零、所有写 API 直接丢弃——UI/输入任务不再取引擎锁，自检的
 *    快照恢复与用例运行因此没有并发写者/读者（见 ADR-007）。
 *  - freeze(false)：清位（引擎状态已由 selftest 恢复到 init 快照，下一次 sync 对齐）。
 * 标志与快照放在 .noinit：selftest_restore() 会覆盖组件 data/bss，普通静态会被
 * 快照回滚掉（这是首版实现被「还原」掉的根因）。 */
void sloop_core_diag_freeze(bool on)
{
    if (on) {
        engine_lock();
        snapshot_fill(&s_frozen_snap);
        s_frozen = true;
        engine_unlock();
        return;
    }
    s_frozen = false;
}

void sloop_core_diag_quiesce_hook(void (*hook)(bool on))
{
    s_quiesce_hook = hook;
}

int sloop_core_selftest(const char *case_name)
{
    if (sloop_hal_audio_streaming() || ctl.results == NULL) {
        printf("ENG SELFTEST cases=0 result=FAIL reason=busy_or_uninitialized hint=ENG AUDIO OFF\n");
        return 1;
    }
    /* 诊断期（ADR-007）：静默 UI → 冻结引擎 → 跑用例 → 恢复，成对且只有一条出口。
     * 1) quiesce：暂停 30ms UI 定时器与 LVGL 定时器、input 任务阻塞等恢复通知；
     *    等 input 任务回到批次边界后才算静默生效——置标志时它可能正派发。
     * 2) freeze：置位后 read/write 都不取锁（返回冻结快照/丢弃写），UI 停在最后一帧。
     * 3) 用例之间不再整段持引擎锁：freeze 已保证「置位后无新写者/读者取锁」，自检
     *    自己是唯一写者；restore 只在首尾各取一次短锁。长时间持锁 + 每次 job 后的
     *    vTaskDelay 没有收益，只增加与其它核调度交错的面。
     * 钩子未注册（UI 未启动/host）时 quiesce 为空操作。
     * 注意：DI-11 卡死的真因不在持锁，而在 input 任务的轮询周期（CONFIG_FREERTOS_HZ=100
     * 下 pdMS_TO_TICKS(5)==0 → vTaskDelay(0) 全速自旋 → 跨核 yield IPI 风暴），见 input.c
     * 的 INPUT_POLL_TICKS 与本 ADR 的「DI-11 真因」小节。 */
    if (s_quiesce_hook != NULL) s_quiesce_hook(true);
    sloop_core_diag_freeze(true);
    engine_lock();
    selftest_restore();
    engine_unlock();
    ctl.selector = case_name;
    ctl.fork_count = 0;
    ctl.recapture = 1;
    memset(ctl.valid, 0, sizeof(ctl.valid));
    nj = 0;
    char *argv[] = {"regress", "tests/golden.txt", "tests/cpu_baseline.txt"};
    printf("ENG SELFTEST BEGIN%s%s\n", case_name ? " " : "", case_name ? case_name : "");
    int main_rc = main(3, argv);

    kv_t *golden = heap_caps_calloc(MAXJ, sizeof(*golden), MALLOC_CAP_SPIRAM);
    uint32_t ng = golden != NULL ? load_kv("tests/golden.txt", golden, MAXJ) : 0;
    /* golden 条目数（不含 repeat）= 全量跑应覆盖的用例数；单例模式应恰好命中 1 条。
     * 声音检查（J_CHECK）同理：全量模式必须恰为 SELFTEST_EXPECTED_CHECKS 条，
     * 单例模式被 selector 过滤掉（0 条），故 expected_checks 随模式取值。 */
    uint32_t expected = case_name != NULL ? (kv_get(golden, ng, case_name) != NULL ? 1u : 0u) : ng;
    uint32_t expected_checks = case_name != NULL ? 0u : SELFTEST_EXPECTED_CHECKS;
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
            printf("ENG SELFTEST HEALTH %s peak=%d dc=%.1f free=%.2f tail=%d tail_dc=%d over=%u near=%u crashed=%d msg=%s\n",
                   j->name, (int)j->r.peak, j->r.dc, j->r.free_s, (int)j->r.tail_peak, (int)j->r.tail_dc,
                   (unsigned)j->r.over, (unsigned)j->r.near, j->crashed, j->crashed ? "crashed" : j->r.msg);
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
    /* PASS 的五个条件：覆盖数（cases==expected）、逐例同源、健康、声音检查
     * （checks == expected_checks：全量恰 7 条、单例 0 条）且全过、repeat 确定性。
     * main() 的返回值单独打印（它的
     * 汇总行也放行到串口），但不作为条件：设备侧刻意不跑 J_CPU 批量（成本测量是
     * host 侧信息），被跳过的 job 没有 res_t，上游 run_jobs 按自己的
     * `got != sizeof(res_t)` 记为崩溃，故 main_rc 恒为非 0；设备有意义的计数
     * （hash/health/checks/确定性）在这里逐项重算并判定。 */
    int pass = golden != NULL && ng > 0 && expected > 0 && cases == expected && mismatch == 0 && health_fail == 0 &&
               checks == expected_checks && check_fail == 0 && deterministic;
    printf("ENG SELFTEST cases=%u identical=%u mismatch=%u health_fail=%u checks=%u check_fail=%u deterministic=%d expected=%u expected_checks=%u main_rc=%d result=%s\n",
           (unsigned)cases, (unsigned)identical, (unsigned)mismatch, (unsigned)health_fail,
           (unsigned)checks, (unsigned)check_fail, deterministic, (unsigned)expected,
           (unsigned)expected_checks, main_rc, pass ? "PASS" : "FAIL");
    heap_caps_free(golden);
    engine_lock();
    selftest_restore();
    engine_unlock();
    nj = 0;
    sloop_core_diag_freeze(false);
    if (s_quiesce_hook != NULL) s_quiesce_hook(false);
    return pass ? 0 : 1;
}
