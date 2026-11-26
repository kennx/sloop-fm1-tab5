/*
 * sloop-fm1-tab5 — FM-1 事务层（接口与协议规则见 fm1_session.h）
 *
 * 三个执行上下文共用下面的状态，由 s_mux 临界区保护：
 *  - fm1_cli 任务：on_frame / on_link，只做解析、计数、入槽、唤醒 fm1_session 任务，不阻塞、不打日志；
 *  - fm1_session 任务：链路建立后先 INFO 再 WATCH 3，此后每 1000ms PING 保活；
 *  - 请求方（console，以及后续的状态同步）：fm1_session_info/get/set/watch/ping。
 * 单在飞由 s_req_mux 保证：持锁期间发帧并等回复。超时后迟到的回复落在没有请求在等的时段，
 * 计 stray。
 */
#include "fm1_session.h"

#include <string.h>

#include "editor_proto.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_fm1.h"

static const char *TAG = "fm1_session";

#define SESSION_TASK_STACK 4096
#define SESSION_TASK_PRIO 4
#define PING_PERIOD_MS 1000
#define REQUEST_TIMEOUT pdMS_TO_TICKS(400)
#define WATCH_AUTO 3u
#define BIT_LINK_UP (1u << 0)
#define BIT_LINK_DOWN (1u << 1)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* s_mux 保护开始 */
static bool s_ready;         /* fm1_session_start 成功；start 之后只读 */
static bool s_link;
static TaskHandle_t s_task;  /* 任务建好之前链路可能已经起来：此时只置 s_link，由任务启动时自查 */
static bool s_pending;       /* 有请求在等回复 */
static bool s_reply_valid;
static bool s_watch_on;
static bool s_auto_watch = true; /* 本段链路是否自动开 WATCH；fm1_session_watch(0) 成功后关到下一次链路建立 */
static uint8_t s_watch_val;
static fm1_session_stats_t s_stats;
/* s_mux 保护结束 */
static EXT_RAM_BSS_ATTR edp_frame_t s_reply;
static EXT_RAM_BSS_ATTR fm1_info_t s_info;
static SemaphoreHandle_t s_req_mux, s_reply_sem;
static StaticSemaphore_t s_req_mux_mem, s_reply_sem_mem;

/* 回复槽 → 请求方的副本：只拷实际用到的 argc 字节 */
static void take_reply(edp_frame_t *out)
{
    out->cmd = s_reply.cmd;
    out->argc = s_reply.argc;
    memcpy(out->args, s_reply.args, s_reply.argc);
}

void fm1_session_on_frame(const uint8_t *frame, size_t len, void *user)
{
    static EXT_RAM_BSS_ATTR edp_frame_t f; /* 只有 fm1_cli 任务走到这里 */
    uint32_t *count = NULL;
    uint8_t a0 = 0, a1 = 0;
    int16_t v = 0;
    bool ok;

    if (!edp_frame_parse(frame, len, &f)) {
        portENTER_CRITICAL(&s_mux);
        s_stats.stray++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (edp_cmd_is_push(f.cmd)) {
        switch (f.cmd) {
        case EDP_CMD_CHANGED:
            count = &s_stats.changed;
            break;
        case EDP_CMD_TRACK_CHANGED:
            count = &s_stats.track_changed;
            break;
        case EDP_CMD_RELOAD:
            count = &s_stats.reload;
            break;
        default:
            count = &s_stats.step_changed;
            break;
        }
        /* CHANGED/TRACK_CHANGED: scope,id,v14；RELOAD: engine,preset；STEP_CHANGED: index */
        ok = f.argc >= (f.cmd == EDP_CMD_RELOAD ? 2u : f.cmd == EDP_CMD_STEP_CHANGED ? 1u : 4u);
        if (ok) {
            a0 = f.args[0];
            a1 = f.cmd == EDP_CMD_STEP_CHANGED ? 0u : f.args[1];
            v = (f.cmd == EDP_CMD_CHANGED || f.cmd == EDP_CMD_TRACK_CHANGED)
                    ? edp_v14_dec(f.args[2], f.args[3]) : 0;
        }
        portENTER_CRITICAL(&s_mux);
        if (ok) {
            (*count)++;
            s_stats.total++;
            s_stats.has_last = true;
            s_stats.last_cmd = f.cmd;
            s_stats.last_a0 = a0;
            s_stats.last_a1 = a1;
            s_stats.last_v = v;
        } else {
            s_stats.stray++;
        }
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    ok = s_pending;
    if (ok) {
        s_reply.cmd = f.cmd;
        s_reply.argc = f.argc;
        memcpy(s_reply.args, f.args, f.argc);
        s_reply_valid = true;
    } else {
        s_stats.stray++; /* 没有请求在等的回复：超时后迟到的那一条 */
    }
    portEXIT_CRITICAL(&s_mux);
    if (ok) xSemaphoreGive(s_reply_sem);
}

void fm1_session_on_link(bool present, void *user)
{
    TaskHandle_t task;

    portENTER_CRITICAL(&s_mux);
    s_link = present;
    if (present) {
        s_auto_watch = true;
    } else {
        s_watch_on = false;
        s_info.valid = false;
    }
    task = s_task;
    portEXIT_CRITICAL(&s_mux);
    if (task != NULL) xTaskNotify(task, present ? BIT_LINK_UP : BIT_LINK_DOWN, eSetBits);
}

/* 一次请求/回复：timeout 覆盖等锁、发送与等回复。cmd 不符或参数不足的回复计 stray 后继续等到超时 */
static esp_err_t request(uint8_t cmd, const uint8_t *args, size_t nargs, size_t reply_argc,
                         TickType_t timeout, edp_frame_t *reply)
{
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    uint8_t frame[16];
    size_t len;
    esp_err_t ret;

    if (!s_ready) return ESP_ERR_NOT_ALLOWED;
    if (xSemaphoreTake(s_req_mux, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;

    portENTER_CRITICAL(&s_mux);
    s_pending = true;
    s_reply_valid = false;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreTake(s_reply_sem, 0);

    len = edp_frame_build(cmd, args, nargs, frame, sizeof frame);
    ret = len == 0 ? ESP_ERR_INVALID_ARG : usb_fm1_send_frame(frame, len, timeout);
    while (ret == ESP_OK) {
        TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) left = 0;
        if (xSemaphoreTake(s_reply_sem, left) != pdTRUE) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        portENTER_CRITICAL(&s_mux);
        bool got = s_reply_valid && s_reply.cmd == cmd && s_reply.argc >= reply_argc;
        s_reply_valid = false;
        if (got && reply != NULL) take_reply(reply);
        if (!got) s_stats.stray++;
        portEXIT_CRITICAL(&s_mux);
        if (got) break;
    }

    portENTER_CRITICAL(&s_mux);
    s_pending = false;
    /* NOT_FINISHED：上一帧超时后仍在途，这一帧发不出去，同属超时 */
    if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NOT_FINISHED) s_stats.timeouts++;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_req_mux);
    return ret;
}

void fm1_session_info_cached(fm1_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t fm1_session_info(fm1_info_t *out, TickType_t timeout)
{
    edp_frame_t reply;
    edp_info_t parsed;
    fm1_info_t info;
    esp_err_t ret = request(EDP_CMD_INFO, NULL, 0, 0, timeout, &reply);

    if (ret != ESP_OK) return ret;
    if (!edp_info_parse(&reply, &parsed)) return ESP_ERR_INVALID_RESPONSE;
    info.valid = true;
    memcpy(info.version, parsed.version, sizeof info.version);
    info.nengines = parsed.nengines;
    info.p_count = parsed.p_count;
    info.g_count = parsed.g_count;
    info.nstep = parsed.nstep;
    info.pe0 = parsed.pe0;
    info.ntrk = parsed.ntrk;
    info.proto = parsed.proto;
    memcpy(info.engines, parsed.engines, sizeof info.engines);
    portENTER_CRITICAL(&s_mux);
    if (s_link) s_info = info; /* 回复到手后链路已断：这一段的 INFO 不留作缓存 */
    portEXIT_CRITICAL(&s_mux);
    if (out != NULL) *out = info;
    return ESP_OK;
}

static esp_err_t get_set(uint8_t cmd, uint8_t scope, uint8_t id, int16_t value, int16_t *out,
                         TickType_t timeout)
{
    edp_frame_t reply;
    uint8_t args[4] = {scope, id};
    size_t nargs = 2;
    esp_err_t ret;

    if (cmd == EDP_CMD_SET) {
        edp_v14_enc(value, args + 2);
        nargs = 4;
    }
    ret = request(cmd, args, nargs, 4, timeout, &reply);
    if (ret != ESP_OK) return ret;
    if (reply.args[0] != scope || reply.args[1] != id) return ESP_ERR_INVALID_RESPONSE;
    *out = edp_v14_dec(reply.args[2], reply.args[3]);
    return ESP_OK;
}

esp_err_t fm1_session_get(uint8_t scope, uint8_t id, int16_t *value, TickType_t timeout)
{
    return get_set(EDP_CMD_GET, scope, id, 0, value, timeout);
}

esp_err_t fm1_session_set(uint8_t scope, uint8_t id, int16_t value, int16_t *clamped, TickType_t timeout)
{
    return get_set(EDP_CMD_SET, scope, id, value, clamped, timeout);
}

esp_err_t fm1_session_watch(uint8_t on, uint8_t *replied, TickType_t timeout)
{
    edp_frame_t reply;
    esp_err_t ret = request(EDP_CMD_WATCH, &on, 1, 1, timeout, &reply);

    if (ret != ESP_OK) return ret;
    portENTER_CRITICAL(&s_mux);
    if (s_link) {
        s_watch_on = reply.args[0] != 0;
        s_watch_val = reply.args[0];
        if (reply.args[0] == 0) s_auto_watch = false;
    }
    portEXIT_CRITICAL(&s_mux);
    *replied = reply.args[0];
    return ESP_OK;
}

esp_err_t fm1_session_ping(uint32_t *roundtrip_us, TickType_t timeout)
{
    const int64_t t0 = esp_timer_get_time();
    esp_err_t ret = request(EDP_CMD_PING, NULL, 0, 0, timeout, NULL);

    if (ret != ESP_OK) return ret;
    *roundtrip_us = (uint32_t)(esp_timer_get_time() - t0);
    portENTER_CRITICAL(&s_mux);
    s_stats.pings++;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

void fm1_session_stats(fm1_session_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    out->watch_on = s_watch_on;
    out->watch_val = s_watch_val;
    portEXIT_CRITICAL(&s_mux);
}

void fm1_session_stats_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(&s_stats, 0, sizeof s_stats);
    portEXIT_CRITICAL(&s_mux);
}

/* 链路建立后的协商：INFO 写缓存，再按 s_auto_watch 开 WATCH 3 */
static bool negotiate(void)
{
    fm1_info_t info;
    uint8_t replied = 0;
    esp_err_t ret = fm1_session_info(&info, REQUEST_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INFO 失败: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "INFO 完成: version=\"%s\" nengines=%u p_count=%u g_count=%u nstep=%u pe0=%u ntrk=%u proto=%u",
             info.version, (unsigned)info.nengines, (unsigned)info.p_count, (unsigned)info.g_count,
             (unsigned)info.nstep, (unsigned)info.pe0, (unsigned)info.ntrk, (unsigned)info.proto);

    portENTER_CRITICAL(&s_mux);
    bool auto_watch = s_auto_watch;
    portEXIT_CRITICAL(&s_mux);
    if (!auto_watch) return true;
    ret = fm1_session_watch(WATCH_AUTO, &replied, REQUEST_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WATCH 失败: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "WATCH 完成: on=%u", (unsigned)replied);
    return true;
}

static void session_task(void *arg)
{
    bool negotiated = false; /* 本段链路已完成 INFO + WATCH */
    uint32_t bits = 0;
    TickType_t wait = 0; /* 启动时先自查一次：链路可能在任务建好之前就已建立 */

    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        wait = pdMS_TO_TICKS(PING_PERIOD_MS);
        portENTER_CRITICAL(&s_mux);
        bool link = s_link;
        portEXIT_CRITICAL(&s_mux);
        if (!link) {
            negotiated = false;
        } else if (!negotiated || (bits & BIT_LINK_UP)) {
            negotiated = negotiate();
        } else {
            uint32_t us = 0;
            esp_err_t ret = fm1_session_ping(&us, REQUEST_TIMEOUT);
            if (ret != ESP_OK) ESP_LOGW(TAG, "PING 失败: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t fm1_session_start(void)
{
    const uint32_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (s_ready) return ESP_ERR_INVALID_STATE;
    s_req_mux = xSemaphoreCreateMutexStatic(&s_req_mux_mem);
    s_reply_sem = xSemaphoreCreateBinaryStatic(&s_reply_sem_mem);
    /* 任务一启动就可能协商：先置 s_ready，再建任务 */
    s_ready = true;
    if (xTaskCreateWithCaps(session_task, "fm1_session", SESSION_TASK_STACK, NULL, SESSION_TASK_PRIO,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_ready = false;
        ESP_LOGE(TAG, "fm1_session 任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fm1_session 就绪（栈 %u 字节在 PSRAM）: internal_free %u -> %u 字节", SESSION_TASK_STACK,
             (unsigned)internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}
