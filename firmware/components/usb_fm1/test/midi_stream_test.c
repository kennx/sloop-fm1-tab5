/* usb_fm1 host 单测（scripts/usb-fm1-test.sh）：USB-MIDI 事件包 ⇄ SysEx 帧。
 * 事件包约定取自上游 firmware/src/usb.c：接收端 midi_in_event（CIN 4..7 为 SysEx，4/7 带 3 字节、
 * 6 带 2、5 带 1），发送端 ota_wire_send（3 字节一组：非末组 CIN 4、末组 CIN 7，余 1 → 5、余 2 → 6）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midi_stream.h"

static int g_total;
static const char *g_case;

#define ASSERT(expr)                                            \
    do {                                                        \
        if (!(expr)) {                                          \
            printf("FAIL %s: line %d: %s\n", g_case, __LINE__, #expr); \
            exit(1);                                            \
        }                                                       \
    } while (0)

static void run(const char *name, void (*fn)(void))
{
    g_case = name;
    fn();
    g_total++;
    printf("PASS %s\n", name);
}

static void test_decode(void) {
    uint8_t o[3];
    ASSERT(midi_stream_decode((uint8_t[]){0x09,0x90,0x3C,0x64}, o) == 3 && o[0]==0x90 && o[1]==0x3C && o[2]==0x64);
    ASSERT(midi_stream_decode((uint8_t[]){0x0C,0xC0,0x05,0x00}, o) == 2 && o[0]==0xC0 && o[1]==0x05);
    ASSERT(midi_stream_decode((uint8_t[]){0x0F,0xF8,0x00,0x00}, o) == 1 && o[0]==0xF8);
    ASSERT(midi_stream_decode((uint8_t[]){0x00,0x90,0x3C,0x64}, o) == 0);   /* CIN 0 */
    ASSERT(midi_stream_decode((uint8_t[]){0x10,0x90,0x3C,0x64}, o) == 0);   /* CIN 1 */
}
static const uint8_t PING7[] = {0xF0,0x7D,0x46,0x4C,0x19,0x00,0xF7};
/* 喂入一段字节直到全部消费；返回期间出现的完整帧数。 */
static int feed_bytes(sysex_asm_t *a, const uint8_t *p, size_t n) {
    int frames = 0;
    while (n) {
        sysex_result_t r = SYSEX_NONE;
        size_t used = sysex_feed(a, p, n, &r);
        ASSERT(used > 0 && used <= n);
        if (r == SYSEX_FRAME) { frames++; ASSERT(!a->in_sysex); }
        p += used; n -= used;
    }
    return frames;
}
static int feed_pkt(sysex_asm_t *a, const uint8_t pkt[4]) {   /* 喂一个事件包，返回完整帧数 */
    uint8_t o[3];
    return feed_bytes(a, o, midi_stream_decode(pkt, o));
}
static void test_assemble(void) {       /* PING 回复 F0 7D 46 4C 19 00 F7 的三种切法 */
    static const uint8_t ping[] = {0xF0,0x7D,0x46,0x4C,0x19,0x00,0xF7};
    static const uint8_t pk[][4] = {{0x04,0xF0,0x7D,0x46},{0x04,0x4C,0x19,0x00},{0x05,0xF7,0x00,0x00}};
    static const uint8_t pk2[][4] = {{0x04,0xF0,0x7D,0x46},{0x04,0x4C,0x19,0x00},{0x06,0xF7,0x00,0x00}};
    static const uint8_t pk3[][4] = {{0x04,0xF0,0x7D,0x46},{0x04,0x4C,0x19,0x00},{0x07,0xF7,0x01,0x02}};
    for (int v = 0; v < 3; v++) {
        sysex_asm_t a = {0}; int frames = 0;
        const uint8_t (*p)[4] = v == 0 ? pk : v == 1 ? pk2 : pk3;
        for (int i = 0; i < 3; i++) frames += feed_pkt(&a, p[i]);
        ASSERT(frames == 1 && a.len == 7 && memcmp(a.buf, ping, 7) == 0 && !a.in_sysex);
    }
}
static void test_assemble_edge(void) {
    /* ① 一次 transfer 里的两帧（粘包） */
    static const uint8_t two[][4] = {{0x04,0xF0,0x7D,0x46},{0x04,0x4C,0x19,0x00},{0x07,0xF7,0xF0,0x7D},
                                     {0x04,0x46,0x4C,0x19},{0x06,0x00,0xF7,0x00}};
    sysex_asm_t a = {0}; int frames = 0;
    for (int i = 0; i < 5; i++) frames += feed_pkt(&a, two[i]);
    ASSERT(frames == 2 && !a.in_sysex && a.drops == 0);
    ASSERT(a.len == 7 && memcmp(a.buf, PING7, 7) == 0);        /* 第 2 帧 */
    /* 跨包：F7 之后同一包里紧跟下一帧的 F0 7D——首次调用停在 F7 之后，buf/len 仍是刚完成的第 1 帧 */
    sysex_asm_t s = {0}; sysex_result_t r = SYSEX_NONE;
    feed_pkt(&s, two[0]); feed_pkt(&s, two[1]);
    uint8_t o[3]; size_t k = midi_stream_decode(two[2], o), used = sysex_feed(&s, o, k, &r);
    ASSERT(used == 1 && r == SYSEX_FRAME && s.len == 7 && memcmp(s.buf, PING7, 7) == 0 && !s.in_sysex);
    ASSERT(feed_bytes(&s, o + used, k - used) == 0 && s.in_sysex && s.len == 2 && memcmp(s.buf, PING7, 2) == 0);
    /* ② SysEx 之外的通道消息被忽略，且不置 in_sysex */
    ASSERT(feed_pkt(&a, (uint8_t[]){0x09,0x90,0x3C,0x64}) == 0);
    ASSERT(!a.in_sysex && a.len == 7);
    /* ③ SysEx 内再遇 F0 = 重新开始，最终帧以第二个 F0 起 */
    feed_pkt(&a, (uint8_t[]){0x04,0xF0,0x7D,0x46}); feed_pkt(&a, (uint8_t[]){0x04,0xF0,0x7D,0x46});
    ASSERT(feed_pkt(&a, (uint8_t[]){0x04,0x4C,0x19,0x00}) == 0 && a.in_sysex && a.len == 6);
    ASSERT(feed_pkt(&a, (uint8_t[]){0x05,0xF7,0x00,0x00}) == 1 && a.len == 7 && memcmp(a.buf, PING7, 7) == 0);
    /* ④ 超长帧：整体丢弃、重新同步，之后一帧正常恢复 */
    uint8_t big[MIDI_STREAM_SYSEX_MAX + 8]; memset(big, 0x55, sizeof big);
    sysex_asm_t b = {0}; feed_pkt(&b, (uint8_t[]){0x04,0xF0,0x7D,0x46});
    used = sysex_feed(&b, big, sizeof big, &r);
    ASSERT(r == SYSEX_DROPPED && used > 0 && used < sizeof big && b.drops == 1 && !b.in_sysex);
    ASSERT(feed_bytes(&b, big + used, sizeof big - used) == 0 && b.drops == 1);   /* 余下字节不是 SysEx：忽略 */
    feed_pkt(&b, (uint8_t[]){0x04,0xF0,0x7D,0x46}); feed_pkt(&b, (uint8_t[]){0x04,0x4C,0x19,0x00});
    ASSERT(feed_pkt(&b, (uint8_t[]){0x05,0xF7,0x00,0x00}) == 1 && b.len == 7 && b.drops == 1);
    /* ⑤ 只有 F0 没有 F7：一直 NONE；n == 0 不动状态 */
    sysex_asm_t c = {0};
    ASSERT(feed_pkt(&c, (uint8_t[]){0x04,0xF0,0x7D,0x46}) == 0);
    ASSERT(feed_pkt(&c, (uint8_t[]){0x04,0x4C,0x19,0x00}) == 0 && c.in_sysex && c.len == 6);
    r = SYSEX_FRAME;
    ASSERT(sysex_feed(&c, PING7, 0, &r) == 0 && r == SYSEX_NONE && c.in_sysex && c.len == 6);
}
static void test_encode(void) {
    uint8_t out[32];
    size_t n = midi_stream_encode_sysex(PING7, 7, out, sizeof out);
    static const uint8_t want[] = {0x04,0xF0,0x7D,0x46, 0x04,0x4C,0x19,0x00, 0x05,0xF7,0x00,0x00};
    ASSERT(n == 12 && memcmp(out, want, 12) == 0);
    static const uint8_t req[] = {0xF0,0x7D,0x46,0x4C,0x19,0xF7};           /* 余 0：末组 CIN 7 */
    n = midi_stream_encode_sysex(req, 6, out, sizeof out);
    ASSERT(n == 8 && out[0] == 0x04 && out[4] == 0x07 && out[7] == 0xF7);
    static const uint8_t r8[] = {0xF0,0x7D,0x46,0x4C,0x19,0x00,0x00,0xF7};  /* 余 2 → CIN 6 */
    n = midi_stream_encode_sysex(r8, 8, out, sizeof out);
    ASSERT(n == 12 && out[8] == 0x06 && out[9] == 0x00 && out[10] == 0xF7 && out[11] == 0x00);
    ASSERT(midi_stream_encode_sysex(PING7, 7, out, 11) == 0);               /* 容量不足 */
    ASSERT(midi_stream_encode_sysex(PING7 + 1, 6, out, sizeof out) == 0);   /* 不以 F0 开头 */
    ASSERT(midi_stream_encode_sysex(req, 5, out, sizeof out) == 0);         /* 不以 F7 结尾 */
}
static void test_roundtrip(void) {
    uint8_t frame[MIDI_STREAM_SYSEX_MAX], pk[MIDI_STREAM_SYSEX_MAX * 4 / 3 + 8];
    frame[0] = 0xF0; for (size_t i = 1; i < sizeof frame - 1; i++) frame[i] = (uint8_t)(i & 0x7F);
    frame[sizeof frame - 1] = 0xF7;
    size_t n = midi_stream_encode_sysex(frame, sizeof frame, pk, sizeof pk);
    sysex_asm_t a = {0}; int frames = 0;
    for (size_t i = 0; i < n; i += 4) frames += feed_pkt(&a, pk + i);
    ASSERT(frames == 1 && a.len == sizeof frame && memcmp(a.buf, frame, sizeof frame) == 0 && a.drops == 0);
}

int main(void)
{
    run("decode", test_decode);
    run("assemble", test_assemble);
    run("assemble_edge", test_assemble_edge);
    run("encode", test_encode);
    run("roundtrip", test_roundtrip);

    printf("USB FM1 TEST PASSED (%d/%d)\n", g_total, g_total);
    return 0;
}
