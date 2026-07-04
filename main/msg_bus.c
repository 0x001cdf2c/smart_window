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

static const char *TAG = "msg_bus";

/* ---------- 配置 ---------- */
#define RECV_BUF_SIZE  2048
#define RECONNECT_MS   5000

/* ---------- 状态 ---------- */
static char                   g_device_id[64];
static char                   g_server_url[256];
static esp_websocket_client_handle_t g_ws = NULL;
static msg_bus_callback_t     g_callback = NULL;
static EventGroupHandle_t     g_evt = NULL;

#define BIT_WIFI_CONNECTED   BIT0
#define BIT_WS_CONNECTED     BIT1

/* ================================================================
 * WiFi
 * ================================================================ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
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
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    g_evt = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    /* 设置有效 MAC 地址 (P4 通过 SDIO 传给 C6) */
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    esp_err_t mac_ret = esp_wifi_set_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "esp_wifi_set_mac returned: %s", esp_err_to_name(mac_ret));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, 32);
    strncpy((char *)wifi_cfg.sta.password, pass, 64);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

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
        ESP_LOGI(TAG, "WebSocket 断开, 将自动重连");
        xEventGroupClearBits(g_evt, BIT_WS_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == 0x01 || ev->op_code == 0x02) { /* text/binary */
            /* 解析消息 */
            cJSON *root = cJSON_ParseWithLength(ev->data_ptr, ev->data_len);
            if (root) {
                cJSON *t = cJSON_GetObjectItem(root, "type");
                cJSON *p = cJSON_GetObjectItem(root, "payload");
                if (t && p && g_callback) {
                    char *payload_str = cJSON_PrintUnformatted(p);
                    g_callback(t->valuestring, payload_str, strlen(payload_str));
                    free(payload_str);
                }
                cJSON_Delete(root);
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
    ws_cfg.task_stack = 4096;
    ws_cfg.task_prio = 5;

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
    cJSON_AddItemToObject(root, "payload", pl ? pl : cJSON_CreateString(payload));

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
