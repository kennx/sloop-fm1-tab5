/*
 * sloop-fm1-tab5 — USB-MIDI host：USB Host Library 开机常驻 + MIDI Streaming 客户端
 *
 * 不用 BSP 的 bsp_usb_host_start()：它建的库任务只调 usb_host_lib_handle_events、
 * 从不派发 client 事件，且栈在内部 RAM。这里按同样三步自己做：VBUS 使能 →
 * usb_host_install → 两个任务（usb_lib 跑库事件、fm1_cli 跑 client 事件），栈走
 * PSRAM（xTaskCreateWithCaps），只有 TCB 落内部 RAM。
 *
 * 设备生命周期全部归 fm1_cli 任务：NEW_DEV、两个 transfer 的完成回调、DEV_GONE 都由
 * usb_host_client_handle_events 在这个任务里派发。拔出时库先 halt+flush 全部端点（在途
 * transfer 以 CANCELED/NO_DEVICE 回报）再投递 DEV_GONE，但两者谁先被本任务处理没有保证，
 * 所以断开路径只置 link down 并标记「待清理」；release/free/close 推迟到两个 transfer 都已
 * 回报、且已离开 transfer 回调之后才做——interface_release 要求端点既不在 pending 队列也没有
 * 在途 URB，而回调返回前库还没把本次 URB 从在途计数里减掉。FM-1 的 UAC 接口（Audio
 * Control / Audio Streaming）不 claim——音频采集是 5E 的事。
 */
#include "usb_fm1.h"
#include "midi_stream.h"

#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_fm1";

#define LIB_TASK_STACK 4096
#define LIB_TASK_PRIO 10
#define CLI_TASK_STACK 4096
#define CLI_TASK_PRIO 4

#define MIDI_STREAMING_SUBCLASS 0x03
#define IN_ERR_MAX 3 /* IN 环连续出错这么多次就停环 */
#define TX_BUF_SIZE (MIDI_STREAM_SYSEX_MAX * 4 / 3 + 8) /* 最大 SysEx 帧编码后的事件包字节数 */

static usb_host_client_handle_t s_client;
static usb_fm1_config_t s_cfg;
static TaskHandle_t s_lib_task, s_cli_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_fm1_status_t s_status;

/* 设备态：fm1_cli 任务独占（open / transfer 回调 / DEV_GONE / 清理都在该任务） */
static usb_device_handle_t s_dev; /* NULL = 未持有设备 */
static uint8_t s_dev_addr, s_intf;
static usb_transfer_t *s_in_xfer, *s_out_xfer;
static size_t s_in_buf_len;
static bool s_in_inflight;
static uint8_t s_in_errs; /* IN 环连续出错次数，fm1_cli 独占 */
static bool s_intf_claimed;
static bool s_teardown_due;
static EXT_RAM_BSS_ATTR sysex_asm_t s_asm;

/* Bulk OUT 的借用态：发送任务置 in-flight，fm1_cli 的回调清；s_mux 保护 */
static bool s_out_inflight;
static uint32_t s_tx_seq, s_tx_done_seq;
static usb_transfer_status_t s_tx_status;
static SemaphoreHandle_t s_tx_mux, s_tx_done;
static StaticSemaphore_t s_tx_mux_mem, s_tx_done_mem;

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void teardown_if_due(void);

static void fm1_cli_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
        teardown_if_due();
    }
}

/* 让 fm1_cli 从 usb_host_client_handle_events 返回一趟：回调/发送方清掉最后一个 in-flight 后
 * 不能就地清理（见文件头），由这里补做 */
static void kick_cli(void)
{
    usb_host_client_unblock(s_client);
}

/* UTF-16LE → ASCII：不可打印码位记 '?'，超长截断 */
static void copy_product(const usb_str_desc_t *str, char out[32])
{
    size_t n = 0;
    if (str != NULL) {
        for (int i = 0; i < (str->bLength - USB_STR_DESC_SIZE) / 2 && n < 31; i++) {
            uint16_t c = str->wData[i];
            out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

static void count_open_fail(uint8_t addr, esp_err_t err)
{
    ESP_LOGE(TAG, "设备 %u 打开/读描述符失败: %s", addr, esp_err_to_name(err));
    portENTER_CRITICAL(&s_mux);
    s_status.open_fail++;
    portEXIT_CRITICAL(&s_mux);
}

static void bump_open_fail(void)
{
    portENTER_CRITICAL(&s_mux);
    s_status.open_fail++;
    portEXIT_CRITICAL(&s_mux);
}

static void link_up(void)
{
    portENTER_CRITICAL(&s_mux);
    s_status.link = true;
    portEXIT_CRITICAL(&s_mux);
    if (s_cfg.on_link) s_cfg.on_link(true, s_cfg.user);
}

/* 先于资源释放通知上层停手；清理时 vid/pid 等字段随后在 teardown 里清 */
static void link_down(void)
{
    bool was_up;
    portENTER_CRITICAL(&s_mux);
    was_up = s_status.link;
    s_status.link = false;
    portEXIT_CRITICAL(&s_mux);
    if (was_up && s_cfg.on_link) s_cfg.on_link(false, s_cfg.user);
}

/* 释放设备。须在两个 transfer 都已回报、且不在 transfer 回调栈内时调用（见文件头） */
static void teardown_if_due(void)
{
    if (!s_teardown_due) return;

    portENTER_CRITICAL(&s_mux);
    bool out_busy = s_out_inflight;
    portEXIT_CRITICAL(&s_mux);
    if (s_in_inflight || out_busy) return; /* 最后一个回报者会再踢一脚 */

    s_teardown_due = false;
    esp_err_t ret = ESP_OK;
    if (s_intf_claimed) {
        s_intf_claimed = false;
        ret = usb_host_interface_release(s_client, s_dev, s_intf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "设备 %u 接口 %u 释放失败: %s", s_dev_addr, s_intf, esp_err_to_name(ret));
        }
    }
    usb_host_transfer_free(s_in_xfer);
    s_in_xfer = NULL;
    usb_host_transfer_free(s_out_xfer);
    s_out_xfer = NULL;
    ret = usb_host_device_close(s_client, s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设备关闭失败 addr=%u: %s", s_dev_addr, esp_err_to_name(ret));
    }
    s_dev = NULL;
    s_dev_addr = 0;

    portENTER_CRITICAL(&s_mux);
    s_status.vid = 0;
    s_status.pid = 0;
    s_status.product[0] = '\0';
    s_status.midi_interface = 0;
    s_status.ep_in = 0;
    s_status.ep_out = 0;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "设备已清理（rx/tx 计数保留）");
}

/* Bulk IN 数据 → 事件包 → 只把 CIN 4..7 的 SysEx 字节喂给重组器 */
static void feed_midi(const uint8_t *buf, size_t len)
{
    for (size_t off = 0; off + 4 <= len; off += 4) {
        uint8_t cin = buf[off] & 0x0F;
        if (cin < 4 || cin > 7) continue;
        uint8_t midi[3];
        size_t n = midi_stream_decode(buf + off, midi);
        for (size_t used = 0; used < n;) {
            sysex_result_t result;
            used += sysex_feed(&s_asm, midi + used, n - used, &result);
            if (result == SYSEX_NONE) continue;
            portENTER_CRITICAL(&s_mux);
            if (result == SYSEX_FRAME) {
                s_status.rx_frames++;
            } else {
                s_status.rx_drops++;
            }
            portEXIT_CRITICAL(&s_mux);
            if (result == SYSEX_FRAME && s_cfg.on_frame) {
                s_cfg.on_frame(s_asm.buf, s_asm.len, s_cfg.user);
            }
        }
    }
}

/* 出错后重提交：库在端点出错后只 flush 不恢复，pipe 停在 HALTED，直接 submit 会被拒；
 * 先清 halt。pipe 其实还 active 时 clear 回 INVALID_STATE，不算失败，照常提交 */
static esp_err_t in_resubmit_after_error(usb_transfer_t *xfer)
{
    esp_err_t ret = usb_host_endpoint_clear(xfer->device_handle, xfer->bEndpointAddress);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    return usb_host_transfer_submit(xfer);
}

/* IN 环规则：COMPLETED ⇒ 交付数据、连续错误清零、重提交。ERROR/TIMED_OUT/OVERFLOW/SKIPPED 按瞬时
 * 错误处理 ⇒ 清 halt 后重提交并累计连续错误，满 IN_ERR_MAX 次停环。STALL/NO_DEVICE/CANCELED 不重提交
 * （已 STALL 的 pipe 重提交会被库立刻再 stall+flush，空转）；设备已消失（待清理）同样不重提交。
 * 停环一律 link down + 清理。 */
static void in_xfer_cb(usb_transfer_t *xfer)
{
    s_in_inflight = false;
    bool retry = false;
    switch (xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        s_in_errs = 0;
        feed_midi(xfer->data_buffer, (size_t)xfer->actual_num_bytes & ~(size_t)3);
        retry = true;
        break;
    case USB_TRANSFER_STATUS_STALL:
        ESP_LOGW(TAG, "Bulk IN STALL，停止 IN 环");
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        ESP_LOGI(TAG, "Bulk IN 停止 status=%d", xfer->status);
        break;
    default:
        s_in_errs++;
        ESP_LOGW(TAG, "Bulk IN 出错 status=%d（连续第 %u/%u 次）", xfer->status, s_in_errs, IN_ERR_MAX);
        retry = s_in_errs < IN_ERR_MAX;
        break;
    }
    if (retry && !s_teardown_due) {
        esp_err_t ret = xfer->status == USB_TRANSFER_STATUS_COMPLETED ? usb_host_transfer_submit(xfer)
                                                                      : in_resubmit_after_error(xfer);
        if (ret == ESP_OK) {
            s_in_inflight = true;
            return;
        }
        ESP_LOGW(TAG, "Bulk IN 重提交失败: %s", esp_err_to_name(ret));
    }
    if (!s_teardown_due) {
        /* IN 环已停且不会重启：链路作废，走同一条清理路径 */
        link_down();
        s_teardown_due = true;
    }
    kick_cli();
}

static void out_xfer_cb(usb_transfer_t *xfer)
{
    portENTER_CRITICAL(&s_mux);
    s_out_inflight = false;
    s_tx_status = xfer->status;
    s_tx_done_seq = (uint32_t)(uintptr_t)xfer->context;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_tx_done);
    if (s_teardown_due) kick_cli();
}

static void handle_new_dev(uint8_t addr)
{
    usb_device_handle_t dev = NULL;
    esp_err_t ret = usb_host_device_open(s_client, addr, &dev);
    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_INVALID_STATE) {
        /* NEW_DEV 排队到处理之间设备被拔：库里设备对象已释放/已标 gone。不是打开故障，
         * 不计 open_fail，免得脚本误判 */
        ESP_LOGW(TAG, "设备 %u 在打开前已移除: %s", addr, esp_err_to_name(ret));
        return;
    }
    if (ret != ESP_OK) {
        count_open_fail(addr, ret);
        return;
    }
    if (s_dev != NULL) {
        ESP_LOGW(TAG, "设备 %u 仍在清理中，忽略新设备 addr=%u", s_dev_addr, addr);
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_device_desc_t *ddesc = NULL;
    const usb_config_desc_t *cdesc = NULL;
    usb_device_info_t info = {0};
    ret = usb_host_get_device_descriptor(dev, &ddesc);
    if (ret == ESP_OK) ret = usb_host_get_active_config_descriptor(dev, &cdesc);
    if (ret != ESP_OK) {
        count_open_fail(addr, ret);
        usb_host_device_close(s_client, dev);
        return;
    }
    ret = usb_host_device_info(dev, &info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设备 %u 读 device_info 失败（product 记为空）: %s", addr, esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "新设备 addr=%u VID=0x%04X PID=0x%04X class=0x%02X", addr, ddesc->idVendor,
             ddesc->idProduct, ddesc->bDeviceClass);

    const uint32_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 找 alt 0 的 MIDI Streaming 接口，取其后的第一个 Bulk IN / Bulk OUT；描述符来自外部设备，
     * 逐个核对 bLength 再转型 */
    const usb_intf_desc_t *midi_intf = NULL;
    const usb_ep_desc_t *ep_in = NULL, *ep_out = NULL;
    bool in_midi = false;
    int offset = 0;
    for (const usb_standard_desc_t *desc = (const usb_standard_desc_t *)cdesc; desc != NULL;
         desc = usb_parse_next_descriptor(desc, cdesc->wTotalLength, &offset)) {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (desc->bLength < sizeof(usb_intf_desc_t)) {
                ESP_LOGW(TAG, "接口描述符 bLength=%u 过短，跳过", desc->bLength);
                in_midi = false;
                continue;
            }
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            ESP_LOGI(TAG, "  接口 %u alt=%u class=0x%02X subclass=0x%02X protocol=0x%02X eps=%u",
                     intf->bInterfaceNumber, intf->bAlternateSetting, intf->bInterfaceClass,
                     intf->bInterfaceSubClass, intf->bInterfaceProtocol, intf->bNumEndpoints);
            in_midi = midi_intf == NULL && intf->bAlternateSetting == 0 &&
                      intf->bInterfaceClass == USB_CLASS_AUDIO &&
                      intf->bInterfaceSubClass == MIDI_STREAMING_SUBCLASS;
            if (in_midi) midi_intf = intf;
        } else if (in_midi && desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            if (desc->bLength < sizeof(usb_ep_desc_t)) {
                ESP_LOGW(TAG, "端点描述符 bLength=%u 过短，跳过", desc->bLength);
                continue;
            }
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_BULK) continue;
            if (USB_EP_DESC_GET_EP_DIR(ep)) {
                if (ep_in == NULL) ep_in = ep;
            } else {
                if (ep_out == NULL) ep_out = ep;
            }
        }
    }

    if (midi_intf == NULL || ep_in == NULL || ep_out == NULL) {
        ESP_LOGW(TAG, "设备 addr=%u %s", addr,
                 midi_intf == NULL ? "无 MIDI 接口" : "MIDI 接口缺 Bulk IN/OUT 端点");
        bump_open_fail();
        usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_dev_addr = addr;
    s_intf = midi_intf->bInterfaceNumber;
    ret = usb_host_interface_claim(s_client, dev, s_intf, 0);
    if (ret == ESP_OK) {
        s_intf_claimed = true;
        s_in_buf_len = (size_t)USB_EP_DESC_GET_MPS(ep_in) * 4;
        ret = usb_host_transfer_alloc(s_in_buf_len, 0, &s_in_xfer);
    }
    if (ret == ESP_OK) ret = usb_host_transfer_alloc(TX_BUF_SIZE, 0, &s_out_xfer);
    if (ret == ESP_OK) {
        s_in_xfer->device_handle = dev;
        s_in_xfer->bEndpointAddress = ep_in->bEndpointAddress;
        s_in_xfer->num_bytes = (int)s_in_buf_len;
        s_in_xfer->callback = in_xfer_cb;
        s_out_xfer->device_handle = dev;
        s_out_xfer->bEndpointAddress = ep_out->bEndpointAddress;
        s_out_xfer->callback = out_xfer_cb;

        char product[32];
        copy_product(info.str_desc_product, product);
        portENTER_CRITICAL(&s_mux);
        s_status.vid = ddesc->idVendor;
        s_status.pid = ddesc->idProduct;
        memcpy(s_status.product, product, sizeof(s_status.product));
        s_status.midi_interface = s_intf;
        s_status.ep_in = ep_in->bEndpointAddress;
        s_status.ep_out = ep_out->bEndpointAddress;
        portEXIT_CRITICAL(&s_mux);

        s_in_errs = 0;
        s_in_inflight = true;
        ret = usb_host_transfer_submit(s_in_xfer);
    }
    if (ret != ESP_OK) {
        s_in_inflight = false;
        count_open_fail(addr, ret);
        s_teardown_due = true;
        teardown_if_due();
        return;
    }

    ESP_LOGI(TAG, "MIDI Streaming 接口 %u 就绪: ep_in=0x%02X mps=%u ep_out=0x%02X mps=%u in_buf=%u 字节, internal_free %u -> %u",
             s_intf, ep_in->bEndpointAddress, (unsigned)USB_EP_DESC_GET_MPS(ep_in),
             ep_out->bEndpointAddress, (unsigned)USB_EP_DESC_GET_MPS(ep_out), (unsigned)s_in_buf_len,
             (unsigned)internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    link_up();
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        handle_new_dev(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (s_dev == NULL) {
            /* IN 环先收到 CANCELED 已走完清理：正常次序之一 */
            ESP_LOGI(TAG, "DEV_GONE：设备已先行清理，忽略");
            break;
        }
        if (msg->dev_gone.dev_hdl != s_dev) {
            ESP_LOGW(TAG, "DEV_GONE 不是当前设备 addr=%u，忽略", s_dev_addr);
            break;
        }
        link_down();
        s_teardown_due = true;
        teardown_if_due();
        break;
    default:
        break;
    }
}

esp_err_t usb_fm1_start(const usb_fm1_config_t *cfg)
{
    if (s_status.host_ok) return ESP_ERR_INVALID_STATE;
    s_cfg = (cfg != NULL) ? *cfg : (usb_fm1_config_t){0};
    s_tx_mux = xSemaphoreCreateMutexStatic(&s_tx_mux_mem);
    s_tx_done = xSemaphoreCreateBinaryStatic(&s_tx_done_mem);

    const uint32_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    esp_err_t ret = bsp_feature_enable(BSP_FEATURE_USB, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB-A VBUS 使能失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB host 安装失败: %s", esp_err_to_name(ret));
        return ret;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ret = usb_host_client_register(&client_config, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB client 注册失败: %s", esp_err_to_name(ret));
        usb_host_uninstall();
        return ret;
    }

    if (xTaskCreateWithCaps(usb_lib_task, "usb_lib", LIB_TASK_STACK, NULL, LIB_TASK_PRIO,
                            &s_lib_task, MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreateWithCaps(fm1_cli_task, "fm1_cli", CLI_TASK_STACK, NULL, CLI_TASK_PRIO,
                            &s_cli_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "USB 任务创建失败");
        if (s_cli_task) {
            usb_host_client_unblock(s_client);
            vTaskDeleteWithCaps(s_cli_task);
            s_cli_task = NULL;
        }
        if (s_lib_task) {
            usb_host_lib_unblock();
            vTaskDeleteWithCaps(s_lib_task);
            s_lib_task = NULL;
        }
        esp_err_t dereg = usb_host_client_deregister(s_client);
        if (dereg != ESP_OK) {
            ESP_LOGE(TAG, "USB client 注销失败: %s", esp_err_to_name(dereg));
        }
        /* 库任务已删：注销后的 NO_CLIENTS / 设备释放没人处理，uninstall 会因设备未释放而
         * 失败、库常驻。这里自己跑几轮收尾 */
        for (int i = 0; i < 3; i++) {
            uint32_t flags = 0;
            esp_err_t ev = usb_host_lib_handle_events(0, &flags);
            if (ev != ESP_OK) {
                ESP_LOGW(TAG, "收尾处理 USB 事件失败: %s", esp_err_to_name(ev));
                break;
            }
            if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
            if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) || flags == 0) break;
            vTaskDelay(1);
        }
        ret = usb_host_uninstall();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB host 卸载失败: %s", esp_err_to_name(ret));
        }
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_mux);
    s_status.host_ok = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "USB host 就绪（USB-A，usb_lib/fm1_cli 栈在 PSRAM）: internal_free %u -> %u 字节",
             (unsigned)internal_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void usb_fm1_get_status(usb_fm1_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t usb_fm1_send_frame(const uint8_t *frame, size_t len, TickType_t timeout)
{
    if (frame == NULL || !s_status.link) return ESP_ERR_INVALID_STATE;

    /* ≥2^31 tick（含 portMAX_DELAY）视为一直等：直接拿它做截止时刻会溢出成立即超时 */
    const bool forever = (int32_t)timeout < 0;
    const TickType_t wait = forever ? portMAX_DELAY : timeout;
    TickType_t deadline = xTaskGetTickCount() + timeout;
    if (xSemaphoreTake(s_tx_mux, wait) != pdTRUE) return ESP_ERR_TIMEOUT;

    portENTER_CRITICAL(&s_mux);
    usb_transfer_t *out = s_out_xfer;
    uint32_t seq = 0;
    bool ok = s_status.link && out != NULL && !s_out_inflight;
    if (ok) {
        s_out_inflight = true;
        seq = ++s_tx_seq;
    }
    bool was_link = s_status.link;
    portEXIT_CRITICAL(&s_mux);
    if (!ok) {
        xSemaphoreGive(s_tx_mux);
        return was_link ? ESP_ERR_NOT_FINISHED : ESP_ERR_INVALID_STATE;
    }

    /* 编码直接写进 transfer 的 DMA 缓冲（tx 互斥串行化，且此时它不在途） */
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    size_t n = midi_stream_encode_sysex(frame, len, out->data_buffer, out->data_buffer_size);
    if (n > 0) {
        out->num_bytes = (int)n;
        out->context = (void *)(uintptr_t)seq;
        ret = usb_host_transfer_submit(out);
    }
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_out_inflight = false;
        portEXIT_CRITICAL(&s_mux);
        kick_cli(); /* 若正处在断开清理的等待里，由 fm1_cli 补做；否则空跑一趟 */
        xSemaphoreGive(s_tx_mux);
        return ret;
    }

    /* 超时时 transfer 仍在途：不碰它、不释放互斥以外的任何东西，回调稍后自行清 in-flight */
    usb_transfer_status_t status = USB_TRANSFER_STATUS_ERROR;
    bool done = false;
    while (!done) {
        TickType_t left = forever ? portMAX_DELAY : deadline - xTaskGetTickCount();
        if (!forever && (int32_t)left <= 0) left = 0;
        if (xSemaphoreTake(s_tx_done, left) != pdTRUE) {
            xSemaphoreGive(s_tx_mux);
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&s_mux);
        if (s_tx_done_seq == seq) {
            done = true;
            status = s_tx_status;
        }
        portEXIT_CRITICAL(&s_mux);
    }
    if (status == USB_TRANSFER_STATUS_COMPLETED) {
        portENTER_CRITICAL(&s_mux);
        s_status.tx_frames++;
        portEXIT_CRITICAL(&s_mux);
        ret = ESP_OK;
    } else {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    xSemaphoreGive(s_tx_mux);
    return ret;
}
