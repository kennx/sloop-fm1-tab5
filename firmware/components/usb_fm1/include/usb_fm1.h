/*
 * sloop-fm1-tab5 — FM-1 联动模式的 USB-MIDI host 公共接口
 *
 * Tab5 作 USB host 经 USB-A 口接入刷有 SLOOP 固件的 M-VAVE FM-1，编辑器协议的
 * SysEx 帧走 MIDI Streaming 接口的 bulk 端点（ADR-002，docs/design/013-usb-midi-host.md）。
 */
#ifndef USB_FM1_H
#define USB_FM1_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 完整 SysEx 帧 F0..F7。在 fm1_cli 任务上下文调用，不得阻塞、栈要省（4096B PSRAM 栈）。
     * frame 指向重组缓冲，下一帧到来即被覆盖：要留住必须自己拷贝。允许为 NULL。 */
    void (*on_frame)(const uint8_t *frame, size_t len, void *user);
    /* present: MIDI 接口已就绪 / 设备已拔出或 IN 环已停。同样在 fm1_cli 任务上下文调用，不得阻塞；
     * present=false 先于设备资源释放。允许为 NULL。 */
    void (*on_link)(bool present, void *user);
    void *user;
} usb_fm1_config_t;

typedef struct {
    bool host_ok;
    bool link;                 /* MIDI 接口已 claim、收发环在跑 */
    uint16_t vid, pid;         /* link=false 时 vid/pid=0 */
    char product[32];          /* iProduct；取不到为空串 */
    uint8_t midi_interface, ep_in, ep_out; /* 仅 link=true 时有效 */
    uint32_t rx_frames, tx_frames, rx_drops, open_fail; /* rx/tx 计数跨插拔累计，open_fail 是打开失败次数 */
} usb_fm1_status_t;

esp_err_t usb_fm1_start(const usb_fm1_config_t *cfg);
void usb_fm1_get_status(usb_fm1_status_t *out);
/* 同步发送一个完整 SysEx 帧。多个任务可并发调用（内部串行化）。返回：ESP_OK；
 * ESP_ERR_INVALID_ARG（帧不是 F0 开头 F7 结尾或放不进编码缓冲）；ESP_ERR_INVALID_STATE（链路不在）；
 * ESP_ERR_NOT_FINISHED（上一帧超时后仍在途）；ESP_ERR_TIMEOUT（含等内部互斥，timeout 内未完成；
 * ≥2^31 tick 视为一直等）；库的提交错误码；ESP_ERR_INVALID_RESPONSE（传输完成但状态非 COMPLETED）。
 * 不得在 on_frame/on_link 里调用：它们与完成 Bulk OUT 的是同一个任务，会自锁到超时。 */
esp_err_t usb_fm1_send_frame(const uint8_t *frame, size_t len, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* USB_FM1_H */
