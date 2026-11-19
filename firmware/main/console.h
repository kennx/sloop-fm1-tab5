/*
 * sloop-fm1-tab5 — console 通道（bring-up）
 *
 * USB-Serial-JTAG REPL + 项目验证命令。当前注册 SCR 帧导出命令
 * （AGENTS.md §5.3 截屏协议，PC 侧对端 scripts/screenshot.py）；
 * 后续 KB/AUD 自检命令在 app_console_init() 内追加注册。
 */
#pragma once

#include "esp_err.h"

esp_err_t app_console_init(void);   /* 初始化 REPL 并注册命令（main 启动时调用） */
