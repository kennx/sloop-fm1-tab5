/*
 * sloop-fm1-tab5 — Tab5 Keyboard（A164，70 键）驱动：独立 I2C 总线 + INT 触发全键解码
 *
 * 协议实现依据（不含猜测，冲突处以内部固件为准）：
 *  1. Tab5_Keyboard-I2C-Protocol-CN-V1.0.pdf（M5Tab5-Keyboard-Internal-FW 仓库
 *     protocol/ 目录内附同一文件）——寄存器表；
 *  2. m5stack/M5Tab5-Keyboard-Internal-FW（STM32F030 键盘固件源码）——语义权威：
 *     user_i2c_reg.h（寄存器地址）/ user_i2c_callback.c（读写副作用）/
 *     user_sys.c + user_int.c（INT 触发）/ user_scan_handle.c（事件编码）/
 *     user_sys.h（FIFO 深度）/ user_keyboard_handle.c（键位表）。
 *     PDF 与固件唯一冲突：固件版本/I2C 地址寄存器 PDF 标 0xF0，固件实为
 *     0xFE / 0xFF，此处按固件实现。
 *  3. M5Tab5-UserDemo 出厂固件不含 0x6D 键盘驱动（其 keypad_scanner_tca8418
 *     面向旧款 TCA8418@0x34），仅总线/INT 引脚（G0/G1/G50）用法可对照。
 *
 * 寄存器（普通模式相关，7 位地址 0x6D，见 user_i2c_reg.h）：
 *   0x00 INT_CFG   R/W  bit0=普通模式中断使能，复位默认 0x07（main.c reg_init）
 *   0x01 INT_STAT  R/W  bit0=普通模式中断标志；写 0 释放 INT 并清标志
 *   0x02 EVENT_NUM R    当前模式 FIFO 事件数（0~32）；写 0 清空 FIFO 并释放 INT
 *   0x20 KEY_EVENT R    每读一次弹出 1 个事件字节；FIFO 空读回 0xFF
 *   0xFE FW_VER    R    固件版本（V1.0 固件返回 0x01）
 *   （0xFD 为 IAP 升级入口——项目非目标，永不触碰）
 *   RGB（user_i2c_callback.c 读写副作用 + user_rgb.c）：
 *   0x03 RGB_BRIGHTNESS R/W 全局亮度 0..100（上电默认 RGB_BRIGHTNESS_DEFAULT）
 *   0x10 KEYBOARD_MODE  R/W 键盘工作模式（本驱动只用上电默认的 KEY 模式，从不写）
 *   0x11 RGB_MODE       R/W 0 = 绑定（指示灯）/ 1 = 自定义
 *   0x60..0x67 RGB_VALUE R/W 颜色缓冲 rgb_buf_reg[8]：LED i 取 [i*4]=B、[i*4+1]=G、
 *                       [i*4+2]=R，第 4 字节保留（即 RGB1 = 0x60..0x62，RGB2 = 0x64..0x66）
 *
 * 事件字节（user_scan_handle.c keyboard_enqueue）：
 *   bit7=按压(1)/释放(0)，[6:4]=行(0~4)，[3:0]=列(0~13)；
 *   多键同按 = 每键 down/up 独立成事件顺序入队（FIFO 深度 32，user_sys.h）。
 *
 * INT 语义（user_int.c + user_sys.c，TIM6 周期 1.5ms）：
 *   事件入队后 TIM6 拉低 INT（推挽、低有效）；主机读走事件【不会】自动释放
 *   INT，必须写 INT_STAT=0；若此后 FIFO 仍非空，TIM6 下个周期会再次拉低——
 *   即“排空后清 INT”要么线保持高（无事件），要么产生新下降沿（自愈，无丢
 *   边沿风险）。本驱动据此用下降沿中断：ISR→任务通知→读 EVENT_NUM→逐条
 *   读 KEY_EVENT→写 INT_STAT=0；任务启动时先服务一次，覆盖主机启动前 INT
 *   已被拉低（不会再有边沿）的场景；另以 1s 超时 + INT 电平检查兜底 I2C
 *   失败导致的停摆。
 */
#include "tab5_keyboard.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tab5_kb";

/* 寄存器地址（Internal-FW user_i2c_reg.h；完整寄存器表见文件头）*/
#define KB_REG_INT_STAT 0x01
#define KB_REG_EVENT_NUM 0x02
#define KB_REG_KEY_EVENT 0x20
#define KB_REG_FW_VERSION 0xFE
#define KB_REG_RGB_BRIGHTNESS 0x03
#define KB_REG_RGB_MODE 0x11
#define KB_RGB_MODE_USER 1
#define KB_REG_RGB_VALUE 0x60

#define KB_I2C_TIMEOUT_MS 20
#define KB_FIFO_DEPTH 32 /* 键盘侧事件 FIFO 深度（user_sys.h FIFO_DEPTH），本地缓冲同深 */
#define KB_TASK_STACK 3072
#define KB_TASK_PRIO 4

/* 键名表 [row][col]：与键盘固件 user_keyboard_handle.c key_value_map 的
 * firstName 一致（空格键固件原名 " "，此处用 "space" 以保证日志可读）*/
static const char *const k_key_names[5][14] = {
    {"esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "+", "del"},
    {"`", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "[", "]", "\\"},
    {"tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", ";", "'", "backspace"},
    {"sym", "Aa", "a", "s", "d", "f", "g", "h", "j", "k", "l", "up", "_", "enter"},
    {"ctrl", "alt", "z", "x", "c", "v", "b", "n", "m", ".", "left", "down", "right",
     "space"},
};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
/* 组件内唯一的 I2C 串行锁：解码任务（kb_service）与其它任务（console 的 RGB 写入、probe）
 * 共用同一个设备句柄，kb_read_reg/kb_write_reg 的每次传输都在锁内。锁在首次建总线/
 * 设备时创建（kb_bus_ensure）。 */
static SemaphoreHandle_t s_i2c_mutex;
static TaskHandle_t s_task;
static bool s_present;
static uint8_t s_fw_version;
static volatile bool s_log_enabled = true;

static tab5_kb_event_t s_ring[KB_FIFO_DEPTH];
static int s_ring_head;
static int s_ring_count;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_down_count;
static volatile uint32_t s_up_count;
static volatile uint32_t s_dropped;

const char *tab5_kb_key_name(uint8_t code)
{
    uint8_t row = (code >> 4) & 0x07;
    uint8_t col = code & 0x0F;
    if (row >= 5 || col >= 14) {
        return "??";
    }
    return k_key_names[row][col];
}

/* 建独立总线与设备（幂等）：G0/G1 专用键盘总线，i2c_port=-1 自动分配空闲
 * 控制器——板载 G31/G32 总线由 BSP 独占（CONFIG_BSP_I2C_NUM），互不相扰 */
static esp_err_t kb_bus_ensure(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    if (s_i2c_mutex == NULL) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (s_i2c_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = TAB5_KB_I2C_SDA_GPIO,
        .scl_io_num = TAB5_KB_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建键盘 I2C 总线失败: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_i2c_mutex);
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TAB5_KB_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂载键盘 I2C 设备 0x%02X 失败: %s", TAB5_KB_I2C_ADDR,
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(s_i2c_mutex);
    return ret;
}

static esp_err_t kb_read_reg(uint8_t reg, uint8_t *val)
{
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, KB_I2C_TIMEOUT_MS);
    xSemaphoreGive(s_i2c_mutex);
    return ret;
}

static esp_err_t kb_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_transmit(s_dev, buf, sizeof(buf), KB_I2C_TIMEOUT_MS);
    xSemaphoreGive(s_i2c_mutex);
    return ret;
}

static void kb_ring_push(const tab5_kb_event_t *ev)
{
    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_count < KB_FIFO_DEPTH) {
        s_ring[(s_ring_head + s_ring_count) % KB_FIFO_DEPTH] = *ev;
        s_ring_count++;
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

/* 排空键盘 FIFO 并释放 INT（流程依据见文件头「INT 语义」）*/
static void kb_service(void)
{
    uint8_t num = 0;
    esp_err_t ret = kb_read_reg(KB_REG_EVENT_NUM, &num);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读 EVENT_NUM 失败: %s", esp_err_to_name(ret));
        return;
    }

    for (uint8_t i = 0; i < num && i < KB_FIFO_DEPTH; i++) {
        uint8_t raw = 0;
        ret = kb_read_reg(KB_REG_KEY_EVENT, &raw);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "读 KEY_EVENT 失败: %s", esp_err_to_name(ret));
            break;
        }
        if (raw == 0xFF) { /* FIFO 空（user_i2c_callback.c）*/
            break;
        }

        tab5_kb_event_t ev = {
            .code = raw & 0x7F,
            .pressed = (raw & 0x80) != 0,
        };
        kb_ring_push(&ev);
        if (ev.pressed) {
            s_down_count++;
        } else {
            s_up_count++;
        }
        if (s_log_enabled) {
            ESP_LOGI(TAG, "KB 0x%02X %s %s", ev.code, ev.pressed ? "down" : "up",
                     tab5_kb_key_name(ev.code));
        }
    }

    /* 排空后释放 INT；若此刻 FIFO 又有新事件，键盘 TIM6 会再次拉低产生新边沿 */
    ret = kb_write_reg(KB_REG_INT_STAT, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "写 INT_STAT 释放中断失败: %s", esp_err_to_name(ret));
    }
}

static void kb_task(void *arg)
{
    /* 先服务一次：键盘可能在主机启动前已有积压事件（INT 已低、不会再有边沿）*/
    kb_service();
    while (1) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (notified == 0 && gpio_get_level(TAB5_KB_INT_GPIO) != 0) {
            continue; /* 纯超时且 INT 为高：无事件，继续等 */
        }
        kb_service();
    }
}

static void IRAM_ATTR kb_isr_handler(void *arg)
{
    BaseType_t hpw = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &hpw);
    }
    if (hpw) {
        portYIELD_FROM_ISR();
    }
}

bool tab5_keyboard_probe(void)
{
    if (kb_bus_ensure() != ESP_OK) {
        return false;
    }
    uint8_t ver = 0;
    s_present = (kb_read_reg(KB_REG_FW_VERSION, &ver) == ESP_OK);
    if (s_present) {
        s_fw_version = ver;
    }
    return s_present;
}

esp_err_t tab5_keyboard_init(void)
{
    esp_err_t ret = kb_bus_ensure();
    if (ret != ESP_OK) {
        return ret;
    }

    if (!tab5_keyboard_probe()) {
        ESP_LOGW(TAG, "键盘 0x%02X 无应答（未接入 Ext.Port1？），解码任务不启动",
                 TAB5_KB_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    /* 任务先建，保证 ISR 触发时句柄已就绪 */
    if (xTaskCreate(kb_task, "tab5_kb", KB_TASK_STACK, NULL, KB_TASK_PRIO, &s_task) !=
        pdPASS) {
        ESP_LOGE(TAG, "创建解码任务失败");
        return ESP_FAIL;
    }

    /* INT=G50：键盘推挽驱动低有效，内部上拉保证键盘拔出时电平确定 */
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << TAB5_KB_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ret = gpio_config(&int_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置 INT GPIO%d 失败: %s", TAB5_KB_INT_GPIO, esp_err_to_name(ret));
        return ret;
    }

    /* 服务可能已由触摸驱动装过：先直接挂接（服务已在时静默成功），
     * 仅在服务缺失时才补装（esp_lcd_touch.c 亦容忍重复安装，但 gpio 组件
     * 对重复安装会打一条 E 级日志，先挂接可避免每次启动的噪音）*/
    ret = gpio_isr_handler_add(TAB5_KB_INT_GPIO, kb_isr_handler, NULL);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = gpio_install_isr_service(0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "安装 GPIO ISR 服务失败: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = gpio_isr_handler_add(TAB5_KB_INT_GPIO, kb_isr_handler, NULL);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "挂接键盘 INT 中断失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "键盘就绪：0x%02X @ G%d/G%d，INT=G%d，FW 0x%02X", TAB5_KB_I2C_ADDR,
             TAB5_KB_I2C_SDA_GPIO, TAB5_KB_I2C_SCL_GPIO, TAB5_KB_INT_GPIO, s_fw_version);
    return ESP_OK;
}

int tab5_kb_poll(tab5_kb_event_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    portENTER_CRITICAL(&s_ring_mux);
    while (n < max && s_ring_count > 0) {
        out[n++] = s_ring[s_ring_head];
        s_ring_head = (s_ring_head + 1) % KB_FIFO_DEPTH;
        s_ring_count--;
    }
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

/* 档位 → 0x03 全局亮度 */
static const uint8_t k_rgb_brightness[4] = {0, 20, 60, 100};

esp_err_t tab5_keyboard_rgb_set(uint8_t level, uint8_t r, uint8_t g, uint8_t b)
{
    if (level > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = kb_bus_ensure();
    if (ret != ESP_OK) {
        return ret;
    }
    /* 顺序：颜色缓冲 → 自定义模式 → 亮度。键盘侧任一字段变化都置 update_flag 并在
     * rgb_update 里整帧重发；亮度与当前值相同时，模式切换仍会触发一次。 */
    static const uint8_t regs[6] = {KB_REG_RGB_VALUE,     KB_REG_RGB_VALUE + 1, KB_REG_RGB_VALUE + 2,
                                    KB_REG_RGB_VALUE + 4, KB_REG_RGB_VALUE + 5, KB_REG_RGB_VALUE + 6};
    const uint8_t vals[6] = {b, g, r, b, g, r};
    for (uint32_t i = 0; i < sizeof(regs); i++) {
        ret |= kb_write_reg(regs[i], vals[i]);
    }
    ret |= kb_write_reg(KB_REG_RGB_MODE, KB_RGB_MODE_USER);
    ret |= kb_write_reg(KB_REG_RGB_BRIGHTNESS, k_rgb_brightness[level]);
    return ret;
}

void tab5_kb_set_log(bool enable)
{
    s_log_enabled = enable;
}

void tab5_kb_get_stats(tab5_kb_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->present = s_present;
    out->fw_version = s_fw_version;
    out->down_count = s_down_count;
    out->up_count = s_up_count;
    out->dropped = s_dropped;
    out->int_level = gpio_get_level(TAB5_KB_INT_GPIO);
    portENTER_CRITICAL(&s_ring_mux);
    out->pending = s_ring_count;
    portEXIT_CRITICAL(&s_ring_mux);
}
