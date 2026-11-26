/*
 * sloop-fm1-tab5 — USB-MIDI 事件包 ⇄ SysEx 帧（纯 C，无 IDF 依赖）
 *
 * Tab5 作 USB-MIDI host 与 FM-1 上的 SLOOP 固件对话，编辑器协议帧走 SysEx。本层只做字节层
 * 转换：4 字节事件包 → MIDI 字节 → SysEx 帧重组，以及 SysEx 帧 → 事件包序列。
 * 事件包约定同上游 firmware/src/usb.c 的 midi_in_event / ota_wire_send。
 * USB 传输、协议帧解析与事务不在这里。
 */
#ifndef MIDI_STREAM_H
#define MIDI_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_STREAM_SYSEX_MAX 1024
typedef struct {
    uint8_t buf[MIDI_STREAM_SYSEX_MAX];
    size_t len;
    bool in_sysex;
    uint32_t drops;            /* 丢弃的帧数（超长帧被整体丢弃算 1） */
} sysex_asm_t;

/* 一个 4 字节 USB-MIDI 事件包 → 0..3 个 MIDI 字节（写入 out，返回个数）。CIN 0x0/0x1 返回 0。 */
size_t midi_stream_decode(const uint8_t pkt[4], uint8_t out[3]);
typedef enum { SYSEX_NONE = 0, SYSEX_FRAME, SYSEX_DROPPED } sysex_result_t;
/* 喂入一段 MIDI 字节。返回本次消费的字节数（0 < 返回值 ≤ n；n == 0 时返回 0、*result = SYSEX_NONE）。
   *result：SYSEX_NONE / SYSEX_FRAME / SYSEX_DROPPED。
   得到完整帧：返回值停在 F7 之后，帧在 a->buf、长度在 a->len（直到下一个 0xF0 才覆盖）；
   调用方必须把同一次调用里剩余的字节继续喂入（循环到 n 为 0）。
   SysEx 之外的字节一律忽略；SysEx 内再遇 0xF0 = 重新开始；
   超过 MIDI_STREAM_SYSEX_MAX：丢弃整帧（drops++）、回到「等待 0xF0」，返回值停在溢出字节之后。 */
size_t sysex_feed(sysex_asm_t *a, const uint8_t *bytes, size_t n, sysex_result_t *result);
/* SysEx 帧（F0..F7）→ 事件包序列，3 字节一组：非末组 CIN 4；末组 3 字节 CIN 7、
   2 字节 CIN 6、1 字节 CIN 5（与 FM-1 固件 usb.c 的发送端一致）。
   返回写入的字节数；out_cap 不足或帧不是 F0 开头 F7 结尾返回 0。 */
size_t midi_stream_encode_sysex(const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_STREAM_H */
