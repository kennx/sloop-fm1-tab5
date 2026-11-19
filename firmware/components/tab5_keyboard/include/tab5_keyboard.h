/*
 * sloop-fm1-tab5 — Tab5 Keyboard（A164，70 键 I2C 键盘）驱动公共接口
 *
 * 键盘挂在 Tab5 侧专用 I2C 总线（SDA=G0 / SCL=G1，与板载 G31/G32 总线
 * 相互独立），中断引脚 INT=G50；默认「普通」模式下按键事件经 I2C 上报。
 * 协议细节与实现依据见 src/tab5_keyboard.c 文件头。
 */
#ifndef TAB5_KEYBOARD_H
#define TAB5_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 键盘 I2C 从机地址与主机侧引脚（docs/tab5-keyboard-hardware.md §6）*/
#define TAB5_KB_I2C_ADDR 0x6D
#define TAB5_KB_I2C_SDA_GPIO 0
#define TAB5_KB_I2C_SCL_GPIO 1
#define TAB5_KB_INT_GPIO 50

/* 按键事件：code 为键位标识（row<<4|col，row 0~4、col 0~13，不含按压位），
 * pressed 表示按下（true）/释放（false），对应 KEY_EVENT 寄存器 bit7 */
typedef struct {
    uint8_t code;
    bool pressed;
} tab5_kb_event_t;

/* 运行状态快照（KB 排障命令输出用，全键核对以 down/up 计数为准）*/
typedef struct {
    bool present;       /* 最近一次探测结果 */
    uint8_t fw_version; /* 0xFE 固件版本寄存器值（present=false 时无效）*/
    uint32_t down_count;
    uint32_t up_count;
    uint32_t dropped;   /* 本地环形缓冲满而丢弃的事件数 */
    int pending;        /* 本地缓冲待取事件数 */
    int int_level;      /* INT 引脚当前电平（0=键盘已拉低）*/
} tab5_kb_stats_t;

/* 建独立 i2c_master 总线（G0/G1），探测 0x6D；成功则装 INT 中断并启动解码
 * 任务（优先级 4，栈 3KB）。键盘不在位返回 ESP_ERR_NOT_FOUND（总线保留，
 * probe 可继续使用）；其余错误见 esp_err_to_name。 */
esp_err_t tab5_keyboard_init(void);

/* 0x6D 应答探测（读 0xFE 固件版本寄存器，无副作用）；可在 init 之前调用 */
bool tab5_keyboard_probe(void);

/* 取走本地积压事件（至多 max 条），返回实际条数（0=无）*/
int tab5_kb_poll(tab5_kb_event_t *out, int max);

/* 键名表查询（无映射返回 "??"）*/
const char *tab5_kb_key_name(uint8_t code);

/* 事件日志开关（默认开）；取运行快照 */
void tab5_kb_set_log(bool enable);
void tab5_kb_get_stats(tab5_kb_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TAB5_KEYBOARD_H */
