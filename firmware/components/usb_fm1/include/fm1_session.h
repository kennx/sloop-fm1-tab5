/*
 * sloop-fm1-tab5 — FM-1 事务层：一问一答、保活与自动协商
 *
 * 协议规则（上游 firmware/src/editor.c）：同一时刻只有一个请求在飞，回复的 cmd 与请求相同；
 * 23/24/26/32 是设备主动推送、永远不是回复；设备在上一个请求 3s 后停止推送，所以链路在时
 * 每 1000ms 发一次 PING。帧编解码在 editor_proto，USB-MIDI 收发在 usb_fm1。
 * 协商（INFO → WATCH）失败时的重试间隔从 1000ms 起逐次翻倍、上限 30000ms，协商成功或链路变化即复位。
 */
#ifndef FM1_SESSION_H
#define FM1_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;                /* 最近一次 INFO 成功 */
    char version[25];
    uint8_t nengines, p_count, g_count, nstep, pe0, ntrk, proto;
    char engines[16][9];
} fm1_info_t;
typedef struct {
    bool watch_on; uint8_t watch_val;
    uint32_t pings, timeouts, stray;
    uint32_t total, changed, reload, step_changed, track_changed;
    bool has_last; uint8_t last_cmd, last_a0, last_a1; int16_t last_v;
} fm1_session_stats_t;

esp_err_t fm1_session_start(void);        /* 建 fm1_session 任务（栈 4096，PSRAM，优先级 4）*/
esp_err_t fm1_session_info(fm1_info_t *out, TickType_t timeout);              /* 发 INFO 并更新缓存 */
esp_err_t fm1_session_get(uint8_t scope, uint8_t id, int16_t *value, TickType_t timeout);
esp_err_t fm1_session_set(uint8_t scope, uint8_t id, int16_t value, int16_t *clamped, TickType_t timeout);
esp_err_t fm1_session_watch(uint8_t on, uint8_t *replied, TickType_t timeout);
esp_err_t fm1_session_ping(uint32_t *roundtrip_us, TickType_t timeout);
void fm1_session_stats(fm1_session_stats_t *out);
void fm1_session_stats_reset(void);
/* 无链路 ESP_ERR_INVALID_STATE；超时 ESP_ERR_TIMEOUT；回复 cmd 不符按 stray 计后继续等到超时。 */

/* 以下三条是 brief 接口块之外的接线面：main.c 把两个回调交给 usb_fm1_start()，console 的
 * FM1 STATUS 只读缓存、不发帧。回复内容不符（INFO 解析失败、GET/SET 回显的 scope/id 不对）
 * 回 ESP_ERR_INVALID_RESPONSE；会话任务没建起来回 ESP_ERR_NOT_ALLOWED。 */
void fm1_session_info_cached(fm1_info_t *out);
void fm1_session_on_frame(const uint8_t *frame, size_t len, void *user);   /* fm1_cli 任务上下文，不阻塞 */
void fm1_session_on_link(bool present, void *user);                        /* 同上 */
/* 版本政策（spec §6 5A）：版本串不以 "FELUCCA SLOOP 2.8" 开头或 proto != 12 ⇒ true（需升级 FM-1 固件）。
 * 纯函数；INFO 无效时调用方不置位。协商读到需要升级的版本时每段链路打一条 WARNING。 */
bool fm1_session_need_upgrade(const char *version, uint8_t proto);

#ifdef __cplusplus
}
#endif

#endif /* FM1_SESSION_H */
