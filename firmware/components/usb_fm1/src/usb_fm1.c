/*
 * sloop-fm1-tab5 — USB-MIDI host：USB Host Library 开机常驻 + 设备枚举记录
 *
 * 5A Task 1 只到「枚举并记录」：不 claim 接口、不收发，on_frame/on_link 尚不调用，
 * usb_fm1_send_frame 恒回 ESP_ERR_NOT_SUPPORTED（claim 与 bulk 收发见 Task 4，ADR-002）。
 *
 * 不用 BSP 的 bsp_usb_host_start()：它建的库任务只调 usb_host_lib_handle_events、
 * 从不派发 client 事件，且栈在内部 RAM。这里按同样三步自己做：VBUS 使能 →
 * usb_host_install → 两个任务（usb_lib 跑库事件、fm1_cli 跑 client 事件），栈走
 * PSRAM（xTaskCreateWithCaps），只有 TCB 落内部 RAM。
 *
 * 设备事件：NEW_DEV ⇒ 打开、读描述符、记 VID/PID/iProduct 后立即关闭。已关闭的设备
 * 拔出时库只发 DEV_REMOVED（按地址，须 notify_dev_removed），DEV_GONE 只发给仍持有
 * 该设备的 client——两者都清记录，按地址只认自己记下的那台。
 */
#include "usb_fm1.h"

#include <string.h>

#include "bsp/m5stack_tab5.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_fm1";

#define LIB_TASK_STACK 4096
#define LIB_TASK_PRIO 10
#define CLI_TASK_STACK 4096
#define CLI_TASK_PRIO 4

static usb_host_client_handle_t s_client;
static usb_fm1_config_t s_cfg; /* on_frame/on_link 自 Task 4 起在 usb_lib 任务上下文调用 */
static TaskHandle_t s_lib_task, s_cli_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_fm1_status_t s_status;
static uint8_t s_dev_addr; /* fm1_cli 任务独占读写：DEV_REMOVED 按地址比对 */

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

static void fm1_cli_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
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

static void handle_new_dev(uint8_t addr)
{
    usb_device_handle_t dev = NULL;
    esp_err_t ret = usb_host_device_open(s_client, addr, &dev);
    if (ret != ESP_OK) {
        count_open_fail(addr, ret);
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
    usb_host_device_info(dev, &info);

    ESP_LOGI(TAG, "新设备 addr=%u VID=0x%04X PID=0x%04X class=0x%02X", addr, ddesc->idVendor,
             ddesc->idProduct, ddesc->bDeviceClass);
    int offset = 0;
    for (const usb_standard_desc_t *desc = (const usb_standard_desc_t *)cdesc; desc != NULL;
         desc = usb_parse_next_descriptor(desc, cdesc->wTotalLength, &offset)) {
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            ESP_LOGI(TAG, "  接口 %u class=0x%02X subclass=0x%02X protocol=0x%02X eps=%u",
                     intf->bInterfaceNumber, intf->bInterfaceClass, intf->bInterfaceSubClass,
                     intf->bInterfaceProtocol, intf->bNumEndpoints);
        }
    }

    char product[32];
    copy_product(info.str_desc_product, product);
    portENTER_CRITICAL(&s_mux);
    s_status.vid = ddesc->idVendor;
    s_status.pid = ddesc->idProduct;
    memcpy(s_status.product, product, sizeof(s_status.product));
    portEXIT_CRITICAL(&s_mux);
    s_dev_addr = addr;

    ret = usb_host_device_close(s_client, dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设备关闭失败 addr=%u: %s", addr, esp_err_to_name(ret));
    }
}

static void clear_dev(void)
{
    portENTER_CRITICAL(&s_mux);
    s_status.vid = 0;
    s_status.pid = 0;
    s_status.product[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
    s_dev_addr = 0;
    ESP_LOGI(TAG, "设备已拔出，记录清除");
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        handle_new_dev(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        clear_dev();
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        if (msg->dev_removed.address == s_dev_addr) clear_dev();
        break;
    default:
        break;
    }
}

esp_err_t usb_fm1_start(const usb_fm1_config_t *cfg)
{
    if (s_status.host_ok) return ESP_ERR_INVALID_STATE;
    s_cfg = (cfg != NULL) ? *cfg : (usb_fm1_config_t){0};

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
        .flags.notify_dev_removed = 1,
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
        usb_host_client_deregister(s_client);
        usb_host_uninstall();
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
    (void)frame;
    (void)len;
    (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}
