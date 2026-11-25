/* editor_proto host 单测（scripts/editor-proto-test.sh）：SLOOP 编辑器协议帧编解码。
 * 黄金向量取自上游 firmware/src/editor.c：ed_begin/ed_b/ed_v（帧与 v14）、ED_INFO/ED_WATCH/ED_PING
 * 分支（INFO 回复布局、帧样例）、ed_pack7/ed_unpack7/ed_bk_u32（pack7 与 u35）。 */
#include <stdio.h>
#include <string.h>

#include "editor_proto.h"

static int g_total, g_failed, g_case_failed;

#define ASSERT(expr)                                     \
    do {                                                 \
        if (!(expr)) {                                   \
            g_case_failed = 1;                           \
            printf("  %s:%d: %s\n", __func__, __LINE__, #expr); \
        }                                                \
    } while (0)

static void run(const char *name, void (*fn)(void))
{
    g_case_failed = 0;
    fn();
    g_total++;
    if (g_case_failed) {
        g_failed++;
        printf("FAIL %s\n", name);
    } else {
        printf("PASS %s\n", name);
    }
}

static void test_v14(void)
{ /* edp_v14_enc / edp_v14_dec */
    uint8_t b[2];
    edp_v14_enc(0, b);      ASSERT(b[0] == 0x00 && b[1] == 0x40);
    edp_v14_enc(100, b);    ASSERT(b[0] == 0x64 && b[1] == 0x40);
    edp_v14_enc(-8192, b);  ASSERT(b[0] == 0x00 && b[1] == 0x00);
    edp_v14_enc(8191, b);   ASSERT(b[0] == 0x7F && b[1] == 0x7F);
    edp_v14_enc(9000, b);   ASSERT(edp_v14_dec(b[0], b[1]) == 8191);
    edp_v14_enc(-9000, b);  ASSERT(edp_v14_dec(b[0], b[1]) == -8192);
}

static void test_frame_build(void)
{ /* 黄金帧，取自 editor.c ed_begin/ed_b/ed_v */
    uint8_t out[16];
    size_t n = edp_frame_build(EDP_CMD_PING, NULL, 0, out, sizeof out);
    static const uint8_t ping[] = {0xF0,0x7D,0x46,0x4C,0x19,0xF7};
    ASSERT(n == 6 && memcmp(out, ping, 6) == 0);
    uint8_t w = 3; n = edp_frame_build(EDP_CMD_WATCH, &w, 1, out, sizeof out);
    ASSERT(n == 7 && out[4] == 0x16 && out[5] == 0x03 && out[6] == 0xF7);
    uint8_t set_args[4] = {0, 1, 0x64, 0x40};   /* SET scope0 id1 value=100 → v14 = 64 40 */
    n = edp_frame_build(EDP_CMD_SET, set_args, 4, out, sizeof out);
    static const uint8_t set[] = {0xF0,0x7D,0x46,0x4C,0x03,0x00,0x01,0x64,0x40,0xF7};
    ASSERT(n == 10 && memcmp(out, set, 10) == 0);
    uint8_t bad = 0x80; ASSERT(edp_frame_build(2, &bad, 1, out, sizeof out) == 0);
    ASSERT(edp_frame_build(EDP_CMD_PING, NULL, 0, out, 5) == 0);      /* 容量不足 */
}

static void test_frame_parse(void)
{
    edp_frame_t f; static const uint8_t ping[] = {0xF0,0x7D,0x46,0x4C,0x19,0xF7};
    ASSERT(edp_frame_parse(ping, 6, &f) && f.cmd == 25 && f.argc == 0);
    ASSERT(!edp_frame_parse(ping, 5, &f));                          /* 太短 */
    uint8_t m[6] = {0xF0,0x7D,0x46,0x4D,0x19,0xF7}; ASSERT(!edp_frame_parse(m, 6, &f));  /* 头错 */
    m[3] = 0x4C; m[5] = 0x00;     ASSERT(!edp_frame_parse(m, 6, &f));  /* 无 EOX */
    static const uint8_t bad8[] = {0xF0,0x7D,0x46,0x4C,0x02,0x00,0x81,0x40,0xF7};
    ASSERT(edp_frame_parse(bad8, sizeof bad8, &f) == false);           /* 内部字节 ≥ 0x80 */
    static const uint8_t get[] = {0xF0,0x7D,0x46,0x4C,0x02,0x00,0x01,0x64,0x40,0xF7};
    ASSERT(edp_frame_parse(get, sizeof get, &f) && f.cmd == 2 && f.argc == 4 && f.args[3] == 0x40);
    static uint8_t huge[6 + EDP_ARGS_MAX + 1] = {0xF0,0x7D,0x46,0x4C,0x02};   /* 参数数超限 */
    huge[sizeof huge - 1] = 0xF7;
    ASSERT(edp_frame_parse(huge, sizeof huge, &f) == false);
    ASSERT(edp_frame_parse(get, sizeof get, NULL) == false);                   /* 出参为空 */
}

static void test_push(void)
{
    ASSERT(edp_cmd_is_push(23) && edp_cmd_is_push(24) && edp_cmd_is_push(26) && edp_cmd_is_push(32));
    ASSERT(!edp_cmd_is_push(1) && !edp_cmd_is_push(25) && !edp_cmd_is_push(77));
}

static void test_info(void)
{ /* 回复布局 = editor.c case ED_INFO */
    uint8_t r[300]; size_t n = 0;
    r[n++] = 0xF0; r[n++] = 0x7D; r[n++] = 0x46; r[n++] = 0x4C; r[n++] = 1;
    const char *ver = "FELUCCA SLOOP 2.8.5";
    memcpy(r + n, ver, 20); n += 20;                       /* 含结尾 0 */
    r[n++] = 12; r[n++] = 71; r[n++] = 50; r[n++] = 64; r[n++] = 63;  /* NENGINES P_COUNT G_COUNT NSTEP P_E0 */
    for (int i = 0; i < 12; i++) { memcpy(r + n, "ANALOG", 7); n += 7; }
    r[n++] = 4; r[n++] = 12; r[n++] = 0xF7;                /* NTRK, 协议版本 */
    edp_frame_t f; edp_info_t info;
    ASSERT(edp_frame_parse(r, n, &f) && edp_info_parse(&f, &info));
    ASSERT(strcmp(info.version, ver) == 0 && info.nengines == 12 && info.p_count == 71 &&
           info.g_count == 50 && info.nstep == 64 && info.pe0 == 63 && info.ntrk == 4 && info.proto == 12);
    ASSERT(strcmp(info.engines[0], "ANALOG") == 0 && strcmp(info.engines[11], "ANALOG") == 0);
    f.argc = 8; ASSERT(!edp_info_parse(&f, &info));        /* 截断的回复 ⇒ false */
    edp_frame_parse(r, n, &f); f.cmd = EDP_CMD_PING;       /* 完整回复、argc 复原：只因命令号拒绝 */
    ASSERT(!edp_info_parse(&f, &info));
}

static void test_pack7_u35(void)
{ /* 语义 = editor.c ed_pack7 / ed_unpack7 / ed_bk_u32 */
    uint8_t in[9] = {0x00,0x7F,0x80,0xFF,0x01,0x81,0x02,0x03,0x04}, out[16], back[16];
    static const uint8_t want[] = {0x2C,0x00,0x7F,0x00,0x7F,0x01,0x01,0x02, 0x00,0x03,0x04};
    size_t n = edp_pack7_pack(in, 9, out, sizeof out);
    ASSERT(n == 11 && memcmp(out, want, 11) == 0);        /* 首组高位字节 = bit2|bit3|bit5 = 0x2C */
    ASSERT(edp_pack7_unpack(out, n, back, sizeof back) == 9 && memcmp(back, in, 9) == 0);
    ASSERT(edp_pack7_pack(in, 9, out, 10) == 0);          /* 需要 11 字节 */
    uint8_t u[5]; edp_u35_enc(0, u);        ASSERT(u[0]==0 && u[1]==0 && u[4]==0);
    edp_u35_enc(0xFFFFFFFFu, u);            ASSERT(u[0]==0x7F && u[4]==0x0F);
    edp_u35_enc(256, u);                    ASSERT(u[0]==0 && u[1]==2 && u[2]==0 && edp_u35_dec(u) == 256);
    edp_u35_enc(0xFFFFFFFFu, u);            ASSERT(edp_u35_dec(u) == 0xFFFFFFFFu);
}

int main(void)
{
    run("v14", test_v14);
    run("frame_build", test_frame_build);
    run("frame_parse", test_frame_parse);
    run("push", test_push);
    run("info", test_info);
    run("pack7_u35", test_pack7_u35);

    printf("EDITOR PROTO TEST %s (%d/%d)\n", g_failed ? "FAILED" : "PASSED", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
