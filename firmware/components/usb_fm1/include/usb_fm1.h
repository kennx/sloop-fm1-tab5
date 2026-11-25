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
    void (*on_frame)(const uint8_t *frame, size_t len, void *user); /* 完整 SysEx 帧 F0..F7 */
    void (*on_link)(bool present, void *user);                       /* present: MIDI 接口已就绪 */
    void *user;
} usb_fm1_config_t;

typedef struct {
    bool host_ok;
    bool link;                 /* MIDI 接口已 claim、收发环在跑 */
    uint16_t vid, pid;
    char product[32];          /* iProduct；取不到为空串 */
    uint8_t midi_interface, ep_in, ep_out;
    uint32_t rx_frames, tx_frames, rx_drops, open_fail;
} usb_fm1_status_t;

esp_err_t usb_fm1_start(const usb_fm1_config_t *cfg);
void usb_fm1_get_status(usb_fm1_status_t *out);
/* 同步发送一个完整 SysEx 帧；timeout 内未完成返回 ESP_ERR_TIMEOUT。 */
esp_err_t usb_fm1_send_frame(const uint8_t *frame, size_t len, TickType_t timeout);

#ifdef __cplusplus
}
#endif

#endif /* USB_FM1_H */
