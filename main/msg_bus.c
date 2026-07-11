#include "msg_bus.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_hosted_ota.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"

static const char *TAG = "msg_bus";

/* ---------- 配置 ---------- */
#define RECV_BUF_SIZE  8192
#define RECONNECT_MS   5000

/* ---------- 状态 ---------- */
static char                   g_device_id[64];
static char                   g_server_url[256];
static esp_websocket_client_handle_t g_ws = NULL;
static msg_bus_callback_t     g_callback = NULL;
static EventGroupHandle_t     g_evt = NULL;

#define BIT_WIFI_CONNECTED   BIT0
#define BIT_WS_CONNECTED     BIT1

#define OTA_CHUNK_SIZE 1500
#define SLAVE_RST_GPIO  54

/* ── C6 从机 OTA 升级 (通过 SDIO) ── */
static int slave_ota_update(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "slave_fw");
    if (!part) {
        ESP_LOGW(TAG, "未找到 slave_fw 分区, 跳过 C6 OTA");
        return -1;
    }

    /* 检查分区是否为空 */
    uint8_t buf[256];
    esp_partition_read(part, 0, buf, sizeof(buf));
    bool empty = true;
    for (int i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0xFF) { empty = false; break; }
    }
    if (empty) {
        ESP_LOGW(TAG, "slave_fw 分区为空, 跳过 C6 OTA");
        return -1;
    }

    /* 读镜像头 */
    esp_image_header_t img_hdr;
    esp_partition_read(part, 0, &img_hdr, sizeof(img_hdr));
    if (img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "slave_fw 分区无有效固件镜像");
        return -1;
    }

    /* 读镜像版本 */
    esp_app_desc_t new_desc = {0};
    esp_partition_read(part, sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
                       &new_desc, sizeof(new_desc));
    ESP_LOGI(TAG, "分区内 C6 固件版本: %s", new_desc.version);

    /* 查询 C6 实际运行的固件版本, 已是最新则跳过 */
    esp_hosted_coprocessor_fwver_t c6_ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&c6_ver) == 0) {
        char c6_ver_str[32];
        snprintf(c6_ver_str, sizeof(c6_ver_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 c6_ver.major1, c6_ver.minor1, c6_ver.patch1);
        ESP_LOGI(TAG, "C6 当前运行固件版本: %s", c6_ver_str);

        if (strcmp(new_desc.version, c6_ver_str) == 0) {
            ESP_LOGI(TAG, "C6 已是目标版本 %s, 跳过 OTA", c6_ver_str);
            return 1;
        }
    } else {
        ESP_LOGW(TAG, "无法获取 C6 固件版本, 继续尝试 OTA");
    }

    /* 计算固件总大小 */
    size_t total = sizeof(esp_image_header_t);
    size_t off = total;
    for (int i = 0; i < img_hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        esp_partition_read(part, off, &seg, sizeof(seg));
        total += sizeof(seg) + seg.data_len;
        off += sizeof(seg) + seg.data_len;
    }
    total = (total + 15) & ~15;
    total += 1;
    if (img_hdr.hash_appended) {
        total = ((total + 15) & ~15) + 32;
    }

    ESP_LOGI(TAG, "===== 开始 C6 OTA (%s) =====", new_desc.version);
    ESP_LOGI(TAG, "固件大小: %u bytes", (unsigned)total);

    esp_err_t ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin 失败: %s", esp_err_to_name(ret));
        return -1;
    }

    uint8_t *chunk = malloc(OTA_CHUNK_SIZE);
    size_t sent = 0;
    while (sent < total) {
        size_t n = (total - sent > OTA_CHUNK_SIZE) ? OTA_CHUNK_SIZE : (total - sent);
        esp_partition_read(part, sent, chunk, n);
        ret = esp_hosted_slave_ota_write(chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write 失败 @%u: %s", (unsigned)sent, esp_err_to_name(ret));
            free(chunk);
            esp_hosted_slave_ota_end();
            return -1;
        }
        sent += n;
        if (sent % (OTA_CHUNK_SIZE * 20) == 0 || sent == total) {
            ESP_LOGI(TAG, "OTA 进度: %u/%u (%.0f%%)",
                     (unsigned)sent, (unsigned)total,
                     (float)sent * 100 / total);
        }
    }
    free(chunk);

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA end 失败: %s", esp_err_to_name(ret));
        return -1;
    }

    ESP_LOGI(TAG, "OTA 写入完成");

    /* 激活新固件 (C6 FW > v2.5.X 需要, 会自动复位 C6) */
    bool activate_supported = false;
    if (c6_ver.major1 > 2 || (c6_ver.major1 == 2 && c6_ver.minor1 > 5)) {
        activate_supported = true;
    }
    if (activate_supported) {
        ret = esp_hosted_slave_ota_activate();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "C6 新固件已激活, C6 将自动重启");
        } else {
            ESP_LOGW(TAG, "activate 失败: %s, 改用 GPIO 复位", esp_err_to_name(ret));
            activate_supported = false;
        }
    }

    if (!activate_supported) {
        /* 手动拉 GPIO54 复位 C6 */
        gpio_config_t io_cfg = {
            .pin_bit_mask = BIT64(SLAVE_RST_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_cfg);
        gpio_set_level(SLAVE_RST_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(SLAVE_RST_GPIO, 1);
        ESP_LOGI(TAG, "已复位 C6 (GPIO%d)", SLAVE_RST_GPIO);
    }

    /* 保存 OTA 完成标记到 NVS */
    nvs_handle_t nvs;
    if (nvs_open("ota", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "c6_ver", new_desc.version);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "C6 OTA 标记已保存: %s", new_desc.version);
    }

    ESP_LOGI(TAG, "===== C6 OTA 完成, 等待 C6 重启 =====");
    return 0;
}

/* ================================================================
 * WiFi
 * ================================================================ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi 断开, reason=%d", ev->reason);
        xEventGroupClearBits(g_evt, BIT_WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_evt, BIT_WIFI_CONNECTED);
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi 已连接, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static int wifi_init(const char *ssid, const char *pass)
{
    esp_netif_init();
    esp_event_loop_create_default();

    g_evt = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_ps(WIFI_PS_NONE);  /* 禁用省电, 避免 C6 断连 */

    /* 设置有效 MAC 地址 (P4 通过 SDIO 传给 C6) */
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    esp_err_t mac_ret = esp_wifi_set_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "esp_wifi_set_mac returned: %s", esp_err_to_name(mac_ret));

    /* ── C6 从机 OTA 升级 (transport 已就绪) ── */
    int ota_ret = slave_ota_update();
    if (ota_ret == 0) {
        /* OTA 执行了, C6 已复位, 等新固件起来后重启 P4 */
        ESP_LOGI(TAG, "等待 C6 新固件启动...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* ── 基础测试: 先用 SoftAP 验证 C6 WiFi 硬件是否工作 ── */
    ESP_LOGI(TAG, "===== 测试 C6 SoftAP 模式 =====");
    esp_netif_create_default_wifi_ap();
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "C6_Test_AP",
            .ssid_len = 10,
            .password = "12345678",
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "SoftAP 已启动, 请用手机搜索 'C6_Test_AP'");
    ESP_LOGI(TAG, "===== SoftAP 测试结束 =====");

    /* 切回 STA 模式, 注册事件处理器后再扫描 */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    esp_wifi_start();

    /* 等待 WiFi 启动完成再扫描 (不注册自动重连, 避免干扰扫描) */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ── 扫描周围 WiFi ── */
    ESP_LOGI(TAG, "===== 开始扫描 WiFi =====");
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "扫描到 %d 个 AP", ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);

        for (int i = 0; i < ap_count; i++) {
            ESP_LOGI(TAG, "  [%d] SSID:%-24s RSSI:%d  CH:%d  AUTH:%d",
                     i + 1,
                     ap_list[i].ssid,
                     ap_list[i].rssi,
                     ap_list[i].primary,
                     ap_list[i].authmode);
        }

        /* 检查目标 SSID 是否在扫描结果中 */
        bool found = false;
        for (int i = 0; i < ap_count; i++) {
            if (strcmp((char *)ap_list[i].ssid, ssid) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            ESP_LOGI(TAG, "目标热点 [%s] 在扫描结果中 ✓", ssid);
        } else {
            ESP_LOGE(TAG, "目标热点 [%s] 不在扫描结果中 ✗", ssid);
        }

        free(ap_list);
    }
    ESP_LOGI(TAG, "===== 扫描结束 =====");

    /* 配置并连接 */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, 32);
    strncpy((char *)wifi_cfg.sta.password, pass, 64);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(g_evt, BIT_WIFI_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    return (bits & BIT_WIFI_CONNECTED) ? 0 : -1;
}

/* ================================================================
 * WebSocket 事件回调
 * ================================================================ */
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        xEventGroupSetBits(g_evt, BIT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开 op=%d len=%d data=%.*s",
                 ev->op_code, ev->data_len,
                 ev->data_len > 0 ? ev->data_len : 0,
                 ev->data_ptr ? (char *)ev->data_ptr : "");
        xEventGroupClearBits(g_evt, BIT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x01 || ev->op_code == 0x02) { /* text/binary */
            const char *ptr = ev->data_ptr;
            const char *end = ptr + ev->data_len;

            /* 一帧可能含多条 JSON (服务器合并转发), 逐个解析 */
            while (ptr < end) {
                /* 跳过空白 */
                while (ptr < end && (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t'))
                    ptr++;
                if (ptr >= end || *ptr != '{') break;

                /* 找匹配的 } */
                const char *p = ptr;
                int depth = 0;
                while (p < end) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
                    p++;
                }

                int len = p - ptr;
                cJSON *root = cJSON_ParseWithLength(ptr, len);
                if (root) {
                    cJSON *t = cJSON_GetObjectItem(root, "type");
                    cJSON *pld = cJSON_GetObjectItem(root, "payload");
                    if (t && pld && g_callback) {
                        if (cJSON_IsString(pld)) {
                            g_callback(t->valuestring, pld->valuestring, strlen(pld->valuestring));
                        } else {
                            char *payload_str = cJSON_PrintUnformatted(pld);
                            g_callback(t->valuestring, payload_str, strlen(payload_str));
                            free(payload_str);
                        }
                    }
                    cJSON_Delete(root);
                }
                ptr = p;
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket 错误");
        break;
    }
}

/* ================================================================
 * 公开 API
 * ================================================================ */
int msg_bus_init(const char *server_url, const char *device_id)
{
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);

    if (wifi_init(WIFI_SSID, WIFI_PASSWORD) != 0) {
        ESP_LOGE(TAG, "WiFi 连接失败");
        return -1;
    }

    /* WebSocket 配置 */
    esp_websocket_client_config_t ws_cfg = {0};
    ws_cfg.uri = g_server_url;
    ws_cfg.reconnect_timeout_ms = RECONNECT_MS;
    ws_cfg.buffer_size = RECV_BUF_SIZE;
    ws_cfg.task_stack = 6144;
    ws_cfg.task_prio = 7;

    g_ws = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(g_ws);

    /* 等待 WebSocket 连接 */
    EventBits_t bits = xEventGroupWaitBits(g_evt, BIT_WS_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (!(bits & BIT_WS_CONNECTED)) {
        ESP_LOGE(TAG, "WebSocket 连接超时");
        return -1;
    }

    /* 注册设备 */
    char reg[256];
    snprintf(reg, sizeof(reg),
             "{\"type\":\"register\",\"payload\":{\"device_id\":\"%s\",\"role\":\"device\"}}",
             g_device_id);
    esp_websocket_client_send_text(g_ws, reg, strlen(reg), portMAX_DELAY);
    ESP_LOGI(TAG, "设备已注册: %s", g_device_id);

    return 0;
}

int msg_bus_send(const char *type, const char *payload)
{
    if (!g_ws) return -1;

    /* 构建 JSON: {"type":"send","payload":{"type":<user_type>, ...<user_payload>}} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "send");

    cJSON *pl = cJSON_Parse(payload);
    if (!pl) pl = cJSON_CreateString(payload);
    if (cJSON_IsObject(pl)) {
        cJSON_AddStringToObject(pl, "type", type);
    }
    cJSON_AddItemToObject(root, "payload", pl);

    char *str = cJSON_PrintUnformatted(root);
    int len = strlen(str);
    int ret = esp_websocket_client_send_text(g_ws, str, len, pdMS_TO_TICKS(5000));
    cJSON_Delete(root);
    free(str);

    return (ret > 0) ? 0 : -1;
}

void msg_bus_on_recv(msg_bus_callback_t cb)
{
    g_callback = cb;
}

void msg_bus_poll(void)
{
    /* esp_websocket_client 内部有自己的任务, 这里放上层自定义轮询 */
}

bool msg_bus_is_connected(void)
{
    if (!g_ws || !g_evt) return false;
    return (xEventGroupGetBits(g_evt) & BIT_WS_CONNECTED) != 0;
}

int msg_bus_send_raw_msg(const char *msg, int len)
{
    if (!g_ws) return -1;
    int ret = esp_websocket_client_send_text(g_ws, msg, len, pdMS_TO_TICKS(5000));
    return (ret > 0) ? 0 : -1;
}
