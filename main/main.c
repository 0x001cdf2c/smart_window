#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "msg_bus.h"
#include "speech_recognition.h"

static const char *TAG = "main";

/* ── 语音唤醒回调 ── */
static void on_wake_word(int wake_word_index, const char *wake_word_name)
{
    ESP_LOGI(TAG, "[唤醒回调] index=%d  name=%s", wake_word_index, wake_word_name);
}

/* ── 语音命令回调 ── */
static void on_speech_command(const char *command_str)
{
    ESP_LOGI(TAG, "[命令回调] %s", command_str);

    /* 可在此处将语音命令转为设备控制, 当前仅输出到监视窗口 */
}

/* ── 收到消息时被 msg_bus 回调 (联网消息) ── */
static void on_message(const char *type, const char *data, uint16_t data_len)
{
    ESP_LOGI(TAG, "收到消息 type=%s  data=%s", type, data);

    if (strcmp(type, "forward") == 0) {
        ESP_LOGI(TAG, "来自手机: %s", data);
    }
}

/* ── 定时发送传感器数据 ── */
static void sensor_task(void *arg)
{
    while (1) {
        msg_bus_send("sensor_data",
            "{\"temp\":25.3,\"humidity\":60.1,\"light\":850}");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── 语音识别轮询任务 ── */
static void speech_task(void *arg)
{
    while (1) {
        sr_poll();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    /* 屏蔽 WiFi 探测日志 */
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    bool sr_ok = false;
    bool net_ok = false;

    /* 1. 初始化语音识别 (独立于联网, 即使无网络也能工作) */
    if (sr_init() != 0) {
        ESP_LOGW(TAG, "语音识别初始化失败");
    } else {
        sr_on_wake_cb(on_wake_word);
        sr_on_command_cb(on_speech_command);
        sr_start();
        xTaskCreate(speech_task, "speech", 4096, NULL, 4, NULL);
        sr_ok = true;
        ESP_LOGI(TAG, "语音识别就绪");
    }

    /* 2. 初始化消息总线 (连接WiFi + WebSocket + 注册设备) */
    if (msg_bus_init(SERVER_URL, DEVICE_ID) != 0) {
        ESP_LOGW(TAG, "消息总线初始化失败, 联网功能不可用");
    } else {
        msg_bus_on_recv(on_message);
        net_ok = true;
    }

    /* 3. 传感器任务仅在有网络时启动 */
    if (net_ok) {
        xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "系统就绪 (语音=%s, 联网=%s)",
             sr_ok ? "ON" : "OFF",
             net_ok ? "ON" : "OFF");
}
