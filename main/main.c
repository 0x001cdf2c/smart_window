#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "nvs_flash.h"
#include "msg_bus.h"
#include "esp_hosted.h"
#include "speech_recognition.h"
#include "voice_reply.h"

static const char *TAG = "main";

/* ── 语音唤醒回调 ── */
static void on_wake_word(int wake_word_index, const char *wake_word_name)
{
    ESP_LOGI(TAG, "[唤醒回调] index=%d  name=%s", wake_word_index, wake_word_name);
    voice_reply_say("我在");
}

/* ── 拼音 → 中文命令文本映射 (与 wake_words.h 中 SPEECH_COMMANDS 顺序一致) ── */
static const char *get_command_chinese(const char *pinyin)
{
    if (strstr(pinyin, "da kai chuang lian"))   return "打开窗帘";
    if (strstr(pinyin, "guan bi chuang lian"))  return "关闭窗帘";
    if (strstr(pinyin, "ting zhi"))             return "停止";
    if (strstr(pinyin, "da kai deng guang"))    return "打开灯光";
    if (strstr(pinyin, "guan bi deng guang"))   return "关闭灯光";
    return NULL;
}

/* ── 语音命令回调 ── */
static void on_speech_command(const char *command_str)
{
    ESP_LOGI(TAG, "[命令回调] %s", command_str);

    const char *chinese = get_command_chinese(command_str);
    char buf[64];
    if (chinese) {
        snprintf(buf, sizeof(buf), "收到，%s", chinese);
    } else {
        snprintf(buf, sizeof(buf), "收到命令");
    }
    voice_reply_say(buf);
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
    /* NVS 必须最先初始化, ESP-SR 和 WiFi 都依赖它 */
    nvs_flash_init();

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
        voice_reply_init(GPIO_NUM_9);  /* I2S DOUT GPIO9 → ES8311 DSDIN */
        xTaskCreate(speech_task, "speech", 4096, NULL, 4, NULL);
        sr_ok = true;
        ESP_LOGI(TAG, "语音识别就绪");
    }

    /* 2. ESP-Hosted 初始化 (在 SR 之后, 避免 DRAM 竞争) */
    ESP_LOGI(TAG, "启动 ESP-Hosted...");
    if (esp_hosted_init() != 0) {
        ESP_LOGW(TAG, "ESP-Hosted 初始化失败, 联网功能不可用");
    } else {
        ESP_LOGI(TAG, "ESP-Hosted 就绪");
    }

    /* 3. 初始化消息总线 (连接WiFi + WebSocket + 注册设备) */
    if (msg_bus_init(SERVER_URL, DEVICE_ID) != 0) {
        ESP_LOGW(TAG, "消息总线初始化失败, 联网功能不可用");
    } else {
        msg_bus_on_recv(on_message);
        net_ok = true;
        voice_reply_say("联网成功");
    }

    /* 4. 传感器任务仅在有网络时启动 */
    if (net_ok) {
        xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "系统就绪 (语音=%s, 联网=%s)",
             sr_ok ? "ON" : "OFF", net_ok ? "ON" : "OFF");
}
