#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "config.h"
#include "nvs_flash.h"
#include "msg_bus.h"
#include "esp_hosted.h"
#include "speech_recognition.h"
#include "voice_reply.h"
#include "servo_manager.h"
#include "sht3x.h"
#include "bh1750.h"

static const char *TAG = "main";

/* ── 命令队列 (解耦 WebSocket 回调与命令处理, 避免 TTS 阻塞导致丢消息) ── */
#define CMD_QUEUE_LEN 10
static QueueHandle_t g_cmd_queue = NULL;

/* ── 自动模式控制 ── */
static bool g_auto_running = false;

/* ── 语音唤醒回调 ── */
static void on_wake_word(int wake_word_index, const char *wake_word_name)
{
    ESP_LOGI(TAG, "[唤醒回调] index=%d  name=%s", wake_word_index, wake_word_name);
    voice_reply_say("我在");
}

/* ── 拼音 → 中文命令文本映射 ── */
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
        if (strcmp(chinese, "打开窗帘") == 0) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            servo_set_angle(90.0f);
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        } else if (strcmp(chinese, "关闭窗帘") == 0) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            servo_set_angle(0.0f);
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        } else if (strcmp(chinese, "停止") == 0) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            snprintf(buf, sizeof(buf), "收到，已停止");
        } else {
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        }
    } else {
        snprintf(buf, sizeof(buf), "收到命令");
    }
    voice_reply_say(buf);
}

/* ── 解析并执行从 Web/手机发来的命令 ── */
static void handle_web_command(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    /* 如果外层是字符串 (被 cJSON_PrintUnformatted 双重编码), 再解一层 */
    if (cJSON_IsString(root)) {
        cJSON *inner = cJSON_Parse(root->valuestring);
        cJSON_Delete(root);
        root = inner;
        if (!root) return;
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *cmd_type = cJSON_GetObjectItem(root, "type");
    if (!cmd_type || !cmd_type->valuestring) {
        cJSON_Delete(root);
        return;
    }

    const char *cmd = cmd_type->valuestring;
    ESP_LOGI(TAG, "[Web命令] %s", cmd);

    if (strcmp(cmd, "set_angle") == 0) {
        cJSON *angle_item = cJSON_GetObjectItem(root, "angle");
        if (angle_item) {
            float angle = (float)angle_item->valuedouble;
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            servo_set_angle(angle);
            ESP_LOGI(TAG, "手动设置角度: %.1f°", angle);
            char buf[48];
            snprintf(buf, sizeof(buf), "角度已调至%d度", (int)angle);
            voice_reply_say(buf);
        }

    } else if (strcmp(cmd, "auto_mode") == 0) {
        g_auto_running = true;
        servo_set_mode(SERVO_MODE_AUTO);
        ESP_LOGI(TAG, "切换为自动模式");
        voice_reply_say("自动模式");

    } else if (strcmp(cmd, "open_blinds") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        servo_set_angle(90.0f);
        voice_reply_say("打开窗帘");

    } else if (strcmp(cmd, "close_blinds") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        servo_set_angle(0.0f);
        voice_reply_say("关闭窗帘");

    } else if (strcmp(cmd, "voice_cmd") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && text_item->valuestring) {
            if (strstr(text_item->valuestring, "打开")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
                servo_set_angle(90.0f);
            } else if (strstr(text_item->valuestring, "关闭")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
                servo_set_angle(0.0f);
            } else if (strstr(text_item->valuestring, "停止")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
            }
        }

    } else if (strcmp(cmd, "set_temp") == 0) {
        /* 预留: 温度阈值设置 */
        ESP_LOGI(TAG, "温度设置(预留)");

    } else if (strcmp(cmd, "status_report") == 0) {
        float ang = servo_get_angle();
        char buf[96];
        snprintf(buf, sizeof(buf),
            "{\"type\":\"status\",\"angle\":%.1f,\"mode\":\"%s\"}",
            ang,
            servo_get_mode() == SERVO_MODE_AUTO ? "auto" : "manual");
        msg_bus_send("status", buf);
    }

    cJSON_Delete(root);
}

/* ── 命令处理任务: 从队列取出并执行, 避免阻塞 WebSocket 回调 ── */
static void command_task(void *arg)
{
    char *data = NULL;
    while (1) {
        if (xQueueReceive(g_cmd_queue, &data, portMAX_DELAY) == pdTRUE) {
            handle_web_command(data);
            free(data);
        }
    }
}

/* ── 收到消息时被 msg_bus 回调 (快速入队, 立即返回) ── */
static void on_message(const char *type, const char *data, uint16_t data_len)
{
    ESP_LOGI(TAG, "收到消息 type=%s  data=%.*s", type, data_len, data);

    if (strcmp(type, "send") == 0 || strcmp(type, "forward") == 0) {
        char *copy = strdup(data);
        if (copy && g_cmd_queue) {
            if (xQueueSend(g_cmd_queue, &copy, 0) != pdTRUE) {
                free(copy);
                ESP_LOGW(TAG, "命令队列满, 丢弃消息");
            }
        } else {
            free(copy);
        }
    }
}

/* ── 传感器数据上报 (每5秒) ── */
static void sensor_task(void *arg)
{
    while (1) {
        float angle = servo_get_angle();
        const char *mode_str = servo_get_mode() == SERVO_MODE_AUTO ? "auto" : "manual";

        /* 真实温湿度传感器数据 (SHT3x) */
        sht3x_data_t sht;
        float temp_f = 0;
        float humi_f = 0;
        if (sht3x_read(&sht)) {
            temp_f = sht.temperature;
            humi_f = sht.humidity;
        }
        /* 真实光照数据 (BH1750) */
        float lux_f = 0;
        bh1750_read(&lux_f);
        int light = (int)lux_f;

        char buf[192];
        snprintf(buf, sizeof(buf),
            "{\"temp\":%.1f,\"humidity\":%.1f,\"light\":%d,\"angle\":%.1f,\"mode\":\"%s\"}",
            temp_f, humi_f, light, angle, mode_str);
        msg_bus_send("sensor_data", buf);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── 自动模式: 舵机正弦扫描 (0° ↔ 180° 平滑摆动) ── */
static void auto_sweep_task(void *arg)
{
    float phase = 0.0f;
    const float step = 0.04f;   /* 每 50ms 前进约 2.3°, 完整周期约 7.8 秒 */

    while (1) {
        if (g_auto_running && servo_get_mode() == SERVO_MODE_AUTO) {
            /* sin(phase) 输出 [-1,1] → 映射到 [0,180] */
            float angle = 90.0f + 90.0f * sinf(phase);
            servo_set_angle(angle);
            phase += step;
            if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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
    nvs_flash_init();

    esp_log_level_set("wifi", ESP_LOG_INFO);

    /* 命令队列: 解耦 WebSocket 回调与处理, 避免快速连续消息被吞 */
    g_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char *));
    xTaskCreate(command_task, "cmd_task", 4096, NULL, 5, NULL);

    bool sr_ok = false;
    bool net_ok = false;

    /* 0. 初始化 4 路舵机 (GPIO20-23, 统一复位到 0°) */
    int servo_gpios[4] = {20, 21, 22, 23};
    if (servo_init(servo_gpios) == 0) {
        servo_set_angle(0.0f);
        servo_set_mode(SERVO_MODE_MANUAL);
        g_auto_running = false;
        ESP_LOGI(TAG, "舵机 x4 就绪 (GPIO20-23, 角度=0°)");
    } else {
        ESP_LOGW(TAG, "舵机初始化失败");
    }

    /* 1. 语音识别 (初始化 I2C + ES8311) */
    if (sr_init() != 0) {
        ESP_LOGW(TAG, "语音识别初始化失败");
    } else {
        sr_on_wake_cb(on_wake_word);
        sr_on_command_cb(on_speech_command);
        sr_start();
        voice_reply_init(GPIO_NUM_9);
        xTaskCreate(speech_task, "speech", 4096, NULL, 4, NULL);
        sr_ok = true;
        ESP_LOGI(TAG, "语音识别就绪");
    }

    /* 2. 温湿度传感器 (I2C 与 ES8311 共享, 需在 SR 之后) */
    if (!sht3x_init()) {
        ESP_LOGW(TAG, "SHT3x 初始化失败, 温湿度数据不可用");
    } else {
        ESP_LOGI(TAG, "SHT3x 温湿度传感器就绪");
    }

    /* 2b. 光照传感器 (BH1750, I2C 0x23) */
    if (!bh1750_init()) {
        ESP_LOGW(TAG, "BH1750 初始化失败, 光照数据不可用");
    }

    /* 3. ESP-Hosted */
    ESP_LOGI(TAG, "启动 ESP-Hosted...");
    if (esp_hosted_init() != 0) {
        ESP_LOGW(TAG, "ESP-Hosted 初始化失败, 联网功能不可用");
    } else {
        ESP_LOGI(TAG, "ESP-Hosted 就绪");
    }

    /* 3. 消息总线 */
    if (msg_bus_init(SERVER_URL, DEVICE_ID) != 0) {
        ESP_LOGW(TAG, "消息总线初始化失败, 联网功能不可用");
    } else {
        msg_bus_on_recv(on_message);
        net_ok = true;
        voice_reply_say("联网成功");
    }

    /* 4. 传感器上报任务 */
    if (net_ok) {
        xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    }

    /* 5. 自动模式扫风任务 (始终运行, 由 g_auto_running 控制是否转动) */
    xTaskCreate(auto_sweep_task, "auto_sweep", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "系统就绪 (语音=%s, 联网=%s, 舵机x4=GPIO20-23)",
             sr_ok ? "ON" : "OFF", net_ok ? "ON" : "OFF");
}
