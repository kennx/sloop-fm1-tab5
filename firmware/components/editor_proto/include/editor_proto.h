/*
 * sloop-fm1-tab5 — SLOOP 编辑器协议（v12）帧编解码（纯 C，无 IDF 依赖）
 *
 * Tab5 作 USB-MIDI host 与 FM-1 上的 SLOOP 固件对话：请求与回复都是 SysEx 帧
 * `F0 7D 46 4C <cmd> <args…> F7`。编码规则同上游 firmware/src/editor.c 的 ed_begin/ed_b/
 * ed_v/ed_str/ed_pack7/ed_unpack7/ed_bk_u32；权威为上游 web/EDITOR_PROTOCOL.md。
 * 收发、事务与超时不在这里（Task 3 的 midi_stream 与 Task 5 的事务层）。
 */
#ifndef EDITOR_PROTO_H
#define EDITOR_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDP_ARGS_MAX 384
enum { EDP_CMD_INFO = 1, EDP_CMD_GET = 2, EDP_CMD_SET = 3, EDP_CMD_WATCH = 22,
       EDP_CMD_CHANGED = 23, EDP_CMD_RELOAD = 24, EDP_CMD_PING = 25,
       EDP_CMD_STEP_CHANGED = 26, EDP_CMD_TRACK_CHANGED = 32 };
#define EDP_NENGINES_MAX 16
typedef struct { uint8_t cmd; uint16_t argc; uint8_t args[EDP_ARGS_MAX]; } edp_frame_t;
typedef struct {
    char version[25];
    uint8_t nengines, p_count, g_count, nstep, pe0, ntrk, proto;
    char engines[EDP_NENGINES_MAX][9];
} edp_info_t;

void    edp_v14_enc(int16_t v, uint8_t out[2]);          /* 钳位到 −8192..8191 */
int16_t edp_v14_dec(uint8_t lo, uint8_t hi);
size_t  edp_frame_build(uint8_t cmd, const uint8_t *args, size_t nargs,
                        uint8_t *out, size_t cap);        /* 返回帧长；参数 >0x7F 或容量不足返回 0 */
bool    edp_frame_parse(const uint8_t *raw, size_t len, edp_frame_t *out);  /* 首尾之外出现 ≥0x80 的字节即坏帧 */
bool    edp_cmd_is_push(uint8_t cmd);
bool    edp_info_parse(const edp_frame_t *reply, edp_info_t *out);  /* reply->cmd 必须是 INFO */
size_t  edp_pack7_pack(const uint8_t *in, size_t n, uint8_t *out, size_t cap);    /* 0 = 容量不足 */
size_t  edp_pack7_unpack(const uint8_t *in, size_t n, uint8_t *out, size_t cap);  /* 返回解包字节数；容量不足返回 0，不截断 */
void    edp_u35_enc(uint32_t v, uint8_t out[5]);
uint32_t edp_u35_dec(const uint8_t in[5]);

#ifdef __cplusplus
}
#endif

#endif /* EDITOR_PROTO_H */
