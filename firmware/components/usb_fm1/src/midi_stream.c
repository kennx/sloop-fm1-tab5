#include "midi_stream.h"

size_t midi_stream_decode(const uint8_t pkt[4], uint8_t out[3])
{
    /* 每个 CIN 携带的 MIDI 字节数（USB-MIDI 1.0 表；4..7 与上游 midi_in_event 一致） */
    static const uint8_t nbytes[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    size_t n = nbytes[pkt[0] & 0x0F];

    for (size_t i = 0; i < n; i++)
        out[i] = pkt[1 + i];
    return n;
}

size_t sysex_feed(sysex_asm_t *a, const uint8_t *bytes, size_t n, sysex_result_t *result)
{
    *result = SYSEX_NONE;

    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];

        if (b == 0xF0) {
            a->buf[0] = 0xF0;
            a->len = 1;
            a->in_sysex = true;
        } else if (a->in_sysex) {
            if (a->len >= MIDI_STREAM_SYSEX_MAX) {   /* 装不下了：整帧丢弃，等下一个 F0 */
                a->drops++;
                a->in_sysex = false;
                *result = SYSEX_DROPPED;
                return i + 1;
            }
            a->buf[a->len++] = b;
            if (b == 0xF7) {
                a->in_sysex = false;
                *result = SYSEX_FRAME;
                return i + 1;
            }
        }
    }
    return n;
}

size_t midi_stream_encode_sysex(const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap)
{
    size_t n = (len + 2) / 3 * 4;

    if (len < 2 || frame[0] != 0xF0 || frame[len - 1] != 0xF7 || out_cap < n)
        return 0;

    for (size_t i = 0, o = 0; i < len; i += 3, o += 4) {
        size_t k = len - i < 3 ? len - i : 3;

        out[o] = k == 3 ? (i + 3 < len ? 0x04 : 0x07) : 0x04 + k;   /* 4 continues; 5/6/7 end */
        out[o + 1] = frame[i];
        out[o + 2] = k > 1 ? frame[i + 1] : 0;
        out[o + 3] = k > 2 ? frame[i + 2] : 0;
    }
    return n;
}
