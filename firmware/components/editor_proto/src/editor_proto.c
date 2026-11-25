/*
 * sloop-fm1-tab5 — SLOOP 编辑器协议帧编解码（规则同上游 firmware/src/editor.c，见 editor_proto.h）
 */
#include <string.h>

#include "editor_proto.h"

#define EDP_SYSEX_START 0xF0
#define EDP_SYSEX_END   0xF7

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void edp_v14_enc(int16_t v, uint8_t out[2])
{
    uint32_t u = (uint32_t)(clamp32(v, -8192, 8191) + 8192);
    out[0] = (uint8_t)(u & 0x7Fu);
    out[1] = (uint8_t)((u >> 7) & 0x7Fu);
}

int16_t edp_v14_dec(uint8_t lo, uint8_t hi)
{
    return (int16_t)((int32_t)((uint32_t)(lo & 0x7Fu) | (uint32_t)(hi & 0x7Fu) << 7) - 8192);
}

size_t edp_frame_build(uint8_t cmd, const uint8_t *args, size_t nargs, uint8_t *out, size_t cap)
{
    size_t i, len = nargs + 6;

    if (cap < len) {
        return 0;
    }
    for (i = 0; i < nargs; i++) {
        if (args[i] > 0x7Fu) {
            return 0;
        }
    }
    out[0] = EDP_SYSEX_START;
    out[1] = 0x7D;
    out[2] = 0x46;
    out[3] = 0x4C;
    out[4] = cmd;
    if (nargs) {
        memcpy(out + 5, args, nargs);
    }
    out[len - 1] = EDP_SYSEX_END;
    return len;
}

bool edp_frame_parse(const uint8_t *raw, size_t len, edp_frame_t *out)
{
    size_t i;
    if (!out || len < 6 || len - 6 > EDP_ARGS_MAX) {
        return false;
    }
    if (raw[0] != EDP_SYSEX_START || raw[1] != 0x7D || raw[2] != 0x46 || raw[3] != 0x4C ||
        raw[len - 1] != EDP_SYSEX_END) {
        return false;
    }
    for (i = 1; i < len - 1; i++) {     /* 首尾之外必须是 7 位数据字节 */
        if (raw[i] >= 0x80u) {
            return false;
        }
    }
    out->cmd = raw[4];
    out->argc = (uint16_t)(len - 6);
    memcpy(out->args, raw + 5, len - 6);
    return true;
}

bool edp_cmd_is_push(uint8_t cmd)
{
    return cmd == EDP_CMD_CHANGED || cmd == EDP_CMD_RELOAD || cmd == EDP_CMD_STEP_CHANGED ||
           cmd == EDP_CMD_TRACK_CHANGED;
}

/* 读一个 0 结尾的串（上游 ed_str 的发送侧：最多 max 字符再跟一个 0）；越界或缺结尾 0 返回 false */
static bool rd_str(const uint8_t *a, size_t argc, size_t *pos, char *dst, size_t max)
{
    size_t n = 0;

    while (*pos < argc && a[*pos] != 0) {
        if (n == max) {
            return false;
        }
        dst[n++] = (char)a[(*pos)++];
    }
    if (*pos == argc) {
        return false;
    }
    (*pos)++;
    dst[n] = '\0';
    return true;
}

bool edp_info_parse(const edp_frame_t *reply, edp_info_t *out)
{
    edp_info_t info;
    size_t pos = 0, i;

    if (!out || !reply || reply->cmd != EDP_CMD_INFO) {
        return false;
    }
    if (!rd_str(reply->args, reply->argc, &pos, info.version, sizeof info.version - 1)) {
        return false;
    }
    if (reply->argc - pos < 5) {
        return false;
    }
    info.nengines = reply->args[pos];
    info.p_count = reply->args[pos + 1];
    info.g_count = reply->args[pos + 2];
    info.nstep = reply->args[pos + 3];
    info.pe0 = reply->args[pos + 4];
    pos += 5;
    if (info.nengines > EDP_NENGINES_MAX) {
        return false;
    }
    for (i = 0; i < info.nengines; i++) {
        if (!rd_str(reply->args, reply->argc, &pos, info.engines[i], sizeof info.engines[i] - 1)) {
            return false;
        }
    }
    if (reply->argc - pos < 2) {
        return false;
    }
    info.ntrk = reply->args[pos];
    info.proto = reply->args[pos + 1];
    *out = info;
    return true;
}

/* 每组最多 7 个数据字节，前面加 1 个高位字节 */
static size_t pack7_len(size_t n)
{
    return n + (n + 6) / 7;
}

size_t edp_pack7_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t need = pack7_len(n), i = 0, o = 0;

    if (cap < need) {
        return 0;
    }
    while (i < n) {
        size_t k = n - i < 7 ? n - i : 7, j;
        uint8_t m = 0;

        for (j = 0; j < k; j++) {
            m |= (uint8_t)((in[i + j] >> 7) << j);
        }
        out[o++] = m;
        for (j = 0; j < k; j++) {
            out[o++] = in[i + j] & 0x7Fu;
        }
        i += k;
    }
    return need;
}

size_t edp_pack7_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t total = 0, i = 0, o = 0;

    while (i < n) {                       /* 先数解包后的字节数，容量不足就不写 */
        size_t k = n - i - 1 < 7 ? n - i - 1 : 7;
        total += k;
        i += k + 1;
    }
    if (cap < total) {
        return 0;
    }
    i = 0;
    while (i < n) {
        uint8_t m = in[i++];
        size_t k = n - i < 7 ? n - i : 7, j;

        for (j = 0; j < k; j++) {
            out[o++] = (uint8_t)((in[i + j] & 0x7Fu) | ((((uint32_t)m >> j) & 1u) << 7));
        }
        i += k;
    }
    return total;
}

void edp_u35_enc(uint32_t v, uint8_t out[5])
{
    uint32_t i;

    for (i = 0; i < 5u; i++) {
        out[i] = (uint8_t)((v >> (7u * i)) & 0x7Fu);
    }
}

uint32_t edp_u35_dec(const uint8_t in[5])
{
    uint32_t v = 0, i;

    for (i = 0; i < 5u; i++) {
        v |= (uint32_t)(in[i] & 0x7Fu) << (7u * i);
    }
    return v;
}
