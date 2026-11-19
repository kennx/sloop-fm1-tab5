/*
 * sloop-fm1-tab5 — console 通道（bring-up）
 *
 * USB-Serial-JTAG REPL + 项目验证命令。当前注册 SCR 帧导出
 * （AGENTS.md §5.3 截屏协议，PC 侧对端 scripts/screenshot.py）、
 * KB 键盘排障与 AUD 音频自检命令。
 */
#pragma once

#include "esp_err.h"

esp_err_t app_console_init(void);   /* 初始化 REPL 并注册命令（main 启动时调用） */
