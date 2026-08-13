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
#include "smoke_sensor.h"
#include "airflow_sensor.h"
#include "rain_sensor.h"
#include "adc_ads1115.h"
#include "display.h"
#include "ui.h"
#include "camera_capture.h"
#include "http_server_cam.h"
#include "i2c_bus.h"
#include "control_algorithm.h"
#include "wind_scanner.h"
#include "local_nlp.h"
#include <time.h>
#include <mbedtls/base64.h>
#include "lwip/apps/sntp.h"

static const char *TAG = "main";

/* ── 命令队列 (解耦 WebSocket 回调与命令处理, 避免 TTS 阻塞导致丢消息) ── */
#define CMD_QUEUE_LEN 10
static QueueHandle_t g_cmd_queue = NULL;

/* ── 自动模式控制 ── */
static bool g_auto_running = false;

/* ── 传感器数据缓存 (供控制算法查询) ── */
static float s_last_temp  = 0.0f;
static float s_last_humi  = 0.0f;
static float s_last_temp_out = 0.0f;
static float s_last_humi_out = 0.0f;
static float s_last_light = 0.0f;
static int   s_last_smoke = 0;
static int   s_last_airflow = 0;    /* 0-100% wind speed from motor/fan on AIN2 */
static int   s_last_rain = 0;
static bool  s_rain_manual = false;   /* true = user manually overrode rain shelter */

/* Forward declarations (referenced in network_init_task) */
static void on_message(const char *type, const char *data, uint16_t data_len);
static void sensor_task(void *arg);
static void airflow_fast_task(void *arg);
static void video_stream_task(void *arg);

/* ── UI 按钮动作处理: 由 ui.c 的事件回调触发 ── */
static void on_ui_action(const char *action)
{
    ESP_LOGI(TAG, "[UI动作] %s", action);

    if (strcmp(action, "open") == 0) {
        servo_set_angle(0.0f);
        control_adaptive_record(servo_get_angle());
        if (control_get_mode() != CONTROL_MODE_ADAPTIVE) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            ui_update_mode(false);
            control_set_mode(CONTROL_MODE_MANUAL);
        }
    } else if (strcmp(action, "close") == 0) {
        servo_set_angle(90.0f);
        control_adaptive_record(servo_get_angle());
        if (control_get_mode() != CONTROL_MODE_ADAPTIVE) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            ui_update_mode(false);
            control_set_mode(CONTROL_MODE_MANUAL);
        }
    } else if (strcmp(action, "cw") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        float a = servo_get_angle() + 10.0f;
        if (a > 180.0f) a = 180.0f;
        servo_set_angle(a);
        ui_update_mode(false);
    } else if (strcmp(action, "cw_hold") == 0) {
        float a = servo_get_angle() + 2.0f;
        if (a > 180.0f) a = 180.0f;
        servo_set_angle(a);
    } else if (strcmp(action, "ccw") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        float a = servo_get_angle() - 10.0f;
        if (a < 0.0f) a = 0.0f;
        servo_set_angle(a);
        ui_update_mode(false);
    } else if (strcmp(action, "ccw_hold") == 0) {
        float a = servo_get_angle() - 2.0f;
        if (a < 0.0f) a = 0.0f;
        servo_set_angle(a);
    } else if (strcmp(action, "manual") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        ui_update_mode(false);
    } else if (strcmp(action, "auto") == 0) {
        g_auto_running = true;
        servo_set_mode(SERVO_MODE_AUTO);
        ui_update_mode(true);
    } else if (strcmp(action, "mode_manual") == 0) {
        s_rain_manual = false;
        control_set_mode(CONTROL_MODE_MANUAL);
        display_lvgl_lock();
        ui_update_mode_highlight("manual");
        display_lvgl_unlock();
    } else if (strcmp(action, "mode_env") == 0) {
        s_rain_manual = false;
        control_set_mode(CONTROL_MODE_ENV);
        display_lvgl_lock();
        ui_update_mode_highlight("env");
        display_lvgl_unlock();
    } else if (strcmp(action, "mode_adaptive") == 0) {
        s_rain_manual = false;
        control_set_mode(CONTROL_MODE_ADAPTIVE);
        display_lvgl_lock();
        ui_update_mode_highlight("adaptive");
        display_lvgl_unlock();
    } else if (strcmp(action, "mode_natural") == 0) {
        s_rain_manual = false;
        control_set_mode(CONTROL_MODE_NATURAL);
        wind_scanner_start();
        display_lvgl_lock();
        ui_update_mode_highlight("natural");
        display_lvgl_unlock();
    } else if (strcmp(action, "timer_del_last") == 0) {
        oneshot_t shots[ONE_SHOT_MAX];
        int n = control_timer_get_one_shots(shots, ONE_SHOT_MAX);
        if (n > 0) {
            control_timer_remove_one_shot(n - 1);
            ESP_LOGI(TAG, "Screen: deleted last one-shot timer (idx %d)", n - 1);
        }
    } else if (strcmp(action, "rain_expand") == 0) {
        s_rain_manual = true;
        servo_rain_shelter_set(true);
    } else if (strcmp(action, "rain_collapse") == 0) {
        s_rain_manual = true;
        servo_rain_shelter_set(false);
    }
}

/* ── 网络初始化任务 (后台运行, 避免 app_main 阻塞导致 IDLE 看门狗超时) ── */
static void network_init_task(void *arg)
{
    ESP_LOGI(TAG, "启动 ESP-Hosted...");
    if (esp_hosted_init() != 0) {
        ESP_LOGW(TAG, "ESP-Hosted 初始化失败, 联网功能不可用");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "ESP-Hosted 就绪");
    vTaskDelay(pdMS_TO_TICKS(10));

    if (msg_bus_init(SERVER_URL, DEVICE_ID) != 0) {
        ESP_LOGW(TAG, "消息总线初始化失败, 联网功能不可用");
        vTaskDelete(NULL);
        return;
    }

    msg_bus_on_recv(on_message);

    display_lvgl_lock();
    ui_update_connection(true);
    display_lvgl_unlock();

    /* NTP time sync (Beijing time, UTC+8) */
    ESP_LOGI(TAG, "Syncing NTP time...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_setservername(1, "pool.ntp.org");
    sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time_t now;
        time(&now);
        if (now > 1700000000) {  /* after ~2023, synced */
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                     timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            break;
        }
    }

    voice_reply_say("联网成功");

    /* 摄像头 + HTTP 视频流 (网络就绪后启动) */
    if (camera_capture_init() == ESP_OK) {
        http_server_cam_start();
        ESP_LOGI(TAG, "摄像头视频流已启动");
        /* 视频流转发 (通过 WebSocket 推送到云端, 局域网外可看) */
        xTaskCreate(video_stream_task, "video_stream", 6144, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "摄像头初始化失败");
    }

    ESP_LOGI(TAG, "网络初始化完成");
    vTaskDelete(NULL);
}

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
    if (strstr(pinyin, "tian qi"))             return "天气查询";
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
            servo_set_angle(0.0f);
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        } else if (strcmp(chinese, "关闭窗帘") == 0) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            servo_set_angle(90.0f);
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        } else if (strcmp(chinese, "停止") == 0) {
            g_auto_running = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            snprintf(buf, sizeof(buf), "收到，已停止");
        } else if (strcmp(chinese, "天气查询") == 0) {
            msg_bus_send("weather_query", "{}");
            snprintf(buf, sizeof(buf), "正在查询天气");
        } else {
            snprintf(buf, sizeof(buf), "收到，%s", chinese);
        }
    } else {
        snprintf(buf, sizeof(buf), "收到命令");
    }
    voice_reply_say(buf);
}

/* ── 音频流回调: 唤醒后将 PCM 音频发往服务端做 ASR ── */
static void on_audio_stream(const int16_t *samples, int count, bool is_end)
{
    if (!samples || count == 0) return;
    if (!msg_bus_is_connected()) return;

    /* base64 编码 PCM 数据 */
    size_t raw_len = count * sizeof(int16_t);
    size_t b64_len = ((raw_len + 2) / 3) * 4 + 1;
    char *b64_buf = malloc(b64_len);
    if (!b64_buf) return;

    size_t out_len = 0;
    mbedtls_base64_encode((unsigned char *)b64_buf, b64_len, &out_len,
                          (const unsigned char *)samples, raw_len);
    b64_buf[out_len] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sample_rate", "16000");
    cJSON_AddNumberToObject(root, "channels", 1);
    cJSON_AddNumberToObject(root, "bits", 16);
    cJSON_AddBoolToObject(root, "is_end", is_end);
    cJSON_AddStringToObject(root, "audio", b64_buf);
    char *json_str = cJSON_PrintUnformatted(root);

    msg_bus_send("asr_audio", json_str);

    cJSON_Delete(root);
    free(json_str);
    free(b64_buf);

    ESP_LOGI(TAG, "[音频流] %d samples, is_end=%d, b64=%d bytes",
             count, is_end, (int)out_len);
}

/* ── 处理 ASR 返回的文字: 本地 NLP 分类器 → 高置信度直接执行, 其余发服务端 DeepSeek ── */
static void handle_asr_text(const char *text)
{
    if (!text || strlen(text) == 0) return;

    ESP_LOGI(TAG, "[ASR结果] %s", text);

    nlp_result_t r = local_nlp_classify(text);

    if (local_nlp_should_use_cloud(&r)) {
        /* 定时/闲聊/低置信度/非"执行"开窗 → 转发云端 DeepSeek */
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddStringToObject(pl, "text", text);
        if (r.intent == NLP_INTENT_TIMER) {
            cJSON_AddBoolToObject(pl, "has_timer", true);
        }
        cJSON_AddBoolToObject(pl, "force_execute", r.force_execute);
        char *js = cJSON_PrintUnformatted(pl);
        msg_bus_send("asr_text", js);
        free(js);
        cJSON_Delete(pl);
        return;
    }

    /* force_execute 或 高置信度本地执行 */
    ESP_LOGI(TAG, "[NLP本地] intent=%s conf=%.0f%% force=%d",
             r.intent_name, r.confidence * 100, r.force_execute);
    switch (r.intent) {
    case NLP_INTENT_OPEN:
        servo_set_angle(0.0f);
        control_adaptive_record(servo_get_angle());
        voice_reply_say(r.reply);
        break;
    case NLP_INTENT_CLOSE:
        servo_set_angle(90.0f);
        control_adaptive_record(servo_get_angle());
        voice_reply_say(r.reply);
        break;
    case NLP_INTENT_STOP:
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        voice_reply_say(r.reply);
        break;
    default:
        break;
    }
}

/* ── 发送自适应摘要到云端大模型, 获取增强预测 ── */
static void adaptive_send_to_cloud(void)
{
    static time_t s_last_send = 0;
    time_t now = time(NULL);
    if (now - s_last_send < 10) return;  /* debounce: max once per 10s */
    s_last_send = now;

    const schedule_plan_t *plan = control_adaptive_get_plan();
    int rec_cnt = 0;
    const recent_op_t *recs = control_adaptive_get_recent_ops(&rec_cnt);

    if (!plan || plan->count == 0) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "adaptive_query");

    /* 当前传感器 */
    cJSON_AddNumberToObject(root, "temp", s_last_temp);
    cJSON_AddNumberToObject(root, "humidity", s_last_humi);
    cJSON_AddNumberToObject(root, "light", (int)s_last_light);

    /* 最近操作记录 (最多10条) */
    cJSON *ops = cJSON_AddArrayToObject(root, "recent_ops");
    for (int i = 0; i < rec_cnt && i < 10; i++) {
        cJSON *op = cJSON_CreateObject();
        cJSON_AddStringToObject(op, "time", recs[i].time_str);
        cJSON_AddNumberToObject(op, "angle", recs[i].angle);
        cJSON_AddNumberToObject(op, "day_offset", recs[i].day_offset);
        cJSON_AddItemToArray(ops, op);
    }

    /* 本地预测计划 */
    cJSON *preds = cJSON_AddArrayToObject(root, "predictions");
    for (int i = 0; i < plan->count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "time", plan->entries[i].time_str);
        cJSON_AddNumberToObject(p, "angle", plan->entries[i].angle);
        cJSON_AddNumberToObject(p, "confidence", plan->entries[i].confidence);
        cJSON_AddItemToArray(preds, p);
    }

    char *js = cJSON_PrintUnformatted(root);
    msg_bus_send("adaptive_query", js);
    ESP_LOGI(TAG, "[自适应] 已发送摘要到云端");
    free(js);
    cJSON_Delete(root);
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
        }

    } else if (strcmp(cmd, "auto_mode") == 0) {
        g_auto_running = true;
        servo_set_mode(SERVO_MODE_AUTO);
        ESP_LOGI(TAG, "切换为自动模式");
        voice_reply_say("自动模式");

    } else if (strcmp(cmd, "open_blinds") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        servo_set_angle(0.0f);
        voice_reply_say("打开窗户");
        control_adaptive_record(servo_get_angle());

    } else if (strcmp(cmd, "close_blinds") == 0) {
        g_auto_running = false;
        servo_set_mode(SERVO_MODE_MANUAL);
        servo_set_angle(90.0f);
        voice_reply_say("关闭窗户");
        control_adaptive_record(servo_get_angle());

    } else if (strcmp(cmd, "rain_expand") == 0) {
        s_rain_manual = true;
        servo_rain_shelter_set(true);

    } else if (strcmp(cmd, "rain_collapse") == 0) {
        s_rain_manual = true;
        servo_rain_shelter_set(false);

    } else if (strcmp(cmd, "voice_cmd") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && text_item->valuestring) {
            if (strstr(text_item->valuestring, "打开")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
                servo_set_angle(0.0f);
            } else if (strstr(text_item->valuestring, "关闭")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
                servo_set_angle(90.0f);
            } else if (strstr(text_item->valuestring, "停止")) {
                g_auto_running = false;
                servo_set_mode(SERVO_MODE_MANUAL);
            }
        }

    } else if (strcmp(cmd, "weather") == 0) {
        cJSON *city = cJSON_GetObjectItem(root, "city");
        cJSON *wthr = cJSON_GetObjectItem(root, "weather");
        cJSON *high = cJSON_GetObjectItem(root, "high");
        cJSON *low  = cJSON_GetObjectItem(root, "low");
        cJSON *rain = cJSON_GetObjectItem(root, "rain_pct");

        char weather_buf[192];
        if (city && city->valuestring && wthr && wthr->valuestring) {
            int h = high ? (int)high->valuedouble : 0;
            int l = low  ? (int)low->valuedouble : 0;
            int r = rain ? (int)rain->valuedouble : 0;

            if (r > 0) {
                snprintf(weather_buf, sizeof(weather_buf),
                    "%s今天%s，%d到%d度，降水概率百分之%d",
                    city->valuestring, wthr->valuestring, l, h, r);
            } else {
                snprintf(weather_buf, sizeof(weather_buf),
                    "%s今天%s，%d到%d度",
                    city->valuestring, wthr->valuestring, l, h);
            }
            display_lvgl_lock();
            ui_update_weather(city->valuestring, wthr->valuestring, h, l, r);
            display_lvgl_unlock();
        } else {
            snprintf(weather_buf, sizeof(weather_buf), "暂无天气数据");
        }
        ESP_LOGI(TAG, "[天气] %s", weather_buf);
        voice_reply_say(weather_buf);

    } else if (strcmp(cmd, "set_temp") == 0) {
        /* 预留: 温度阈值设置 */
        ESP_LOGI(TAG, "温度设置(预留)");

    } else if (strcmp(cmd, "status_report") == 0) {
        float ang = servo_get_angle();
        char buf[128];
        const char *mn = control_mode_name(control_get_mode());
        snprintf(buf, sizeof(buf),
            "{\"type\":\"status\",\"angle\":%.1f,\"mode\":\"%s\",\"ctrl_mode\":\"%s\"}",
            ang,
            servo_get_mode() == SERVO_MODE_AUTO ? "auto" : "manual", mn);
        msg_bus_send("status", buf);

    } else if (strcmp(cmd, "set_mode") == 0) {
        cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
        if (mode_item && mode_item->valuestring) {
            g_auto_running = false;
            s_rain_manual = false;
            servo_set_mode(SERVO_MODE_MANUAL);
            servo_natural_wind_boost(false);  /* 切换模式时取消送风 */
            if (strcmp(mode_item->valuestring, "env") == 0) {
                control_set_mode(CONTROL_MODE_ENV);
            } else if (strcmp(mode_item->valuestring, "adaptive") == 0) {
                control_set_mode(CONTROL_MODE_ADAPTIVE);
            } else if (strcmp(mode_item->valuestring, "timer") == 0) {
                control_set_mode(CONTROL_MODE_TIMER);
            } else if (strcmp(mode_item->valuestring, "natural") == 0) {
                control_set_mode(CONTROL_MODE_NATURAL);
                wind_scanner_start();
            } else {
                control_set_mode(CONTROL_MODE_MANUAL);
            }
            ESP_LOGI(TAG, "Control mode -> %s", mode_item->valuestring);
        }

    } else if (strcmp(cmd, "set_timer") == 0) {
        cJSON *ot = cJSON_GetObjectItem(root, "open_time");
        cJSON *ct = cJSON_GetObjectItem(root, "close_time");
        cJSON *en = cJSON_GetObjectItem(root, "enabled");
        cJSON *rp = cJSON_GetObjectItem(root, "repeat");
        control_timer_set(
            ot ? ot->valuestring : NULL,
            ct ? ct->valuestring : NULL,
            en ? cJSON_IsTrue(en) : true,
            rp ? (strcmp(rp->valuestring, "daily") == 0) : true);
        control_set_mode(CONTROL_MODE_TIMER);

    } else if (strcmp(cmd, "delete_timer") == 0) {
        cJSON *idx = cJSON_GetObjectItem(root, "index");
        if (idx && idx->valueint >= 0) {
            control_timer_remove_one_shot(idx->valueint);
            ESP_LOGI(TAG, "Web: deleted timer idx %d", idx->valueint);
        }
    } else if (strcmp(cmd, "load_demo") == 0) {
        cJSON *recs = cJSON_GetObjectItem(root, "records");
        if (recs && cJSON_IsArray(recs)) {
            int n = cJSON_GetArraySize(recs);
            if (n > 100) n = 100;
            uint8_t *angles = malloc(n);
            uint8_t *hours  = malloc(n);
            uint8_t *mins   = malloc(n);
            if (angles && hours && mins) {
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(recs, i);
                    if (item && cJSON_IsArray(item) && cJSON_GetArraySize(item) >= 3) {
                        angles[i] = (uint8_t)(cJSON_GetArrayItem(item, 0)->valueint);
                        hours[i]  = (uint8_t)(cJSON_GetArrayItem(item, 1)->valueint);
                        mins[i]   = (uint8_t)(cJSON_GetArrayItem(item, 2)->valueint);
                    }
                }
                control_adaptive_demo_load(angles, hours, mins, n);
                control_set_mode(CONTROL_MODE_ADAPTIVE);
                ESP_LOGI(TAG, "Demo: loaded %d records", n);
            }
            free(angles); free(hours); free(mins);
        }

    } else if (strcmp(cmd, "run_predict") == 0) {
        control_adaptive_predict_sensor_aware(s_last_temp, s_last_humi, s_last_light);
        adaptive_send_to_cloud();
        ESP_LOGI(TAG, "Manual re-predict triggered");

    } else if (strcmp(cmd, "natural_boost") == 0) {
        servo_natural_wind_boost(true);
        ESP_LOGI(TAG, "Web: natural boost ON");
    } else if (strcmp(cmd, "natural_boost_off") == 0) {
        servo_natural_wind_boost(false);
        ESP_LOGI(TAG, "Web: natural boost OFF");

    } else if (strcmp(cmd, "env_suggestion") == 0) {
        /* Return current env evaluation */
        char buf[192];
        env_action_t act = control_env_evaluate(
            s_last_temp, s_last_temp_out, s_last_humi, s_last_light,
            (float)s_last_smoke, (float)s_last_rain);
        float target = control_env_target_angle(
            s_last_temp, s_last_humi, s_last_light);
        snprintf(buf, sizeof(buf),
            "{\"action\":\"%s\",\"angle\":%.1f,\"temp\":%.1f,\"humi\":%.1f,\"light\":%.0f}",
            act == ENV_ACTION_OPEN ? "open" :
            act == ENV_ACTION_CLOSE ? "close" : "none",
            target, s_last_temp, s_last_humi, s_last_light);
        msg_bus_send("env_suggestion", buf);

    } else if (strcmp(cmd, "ai_suggestion") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && text_item->valuestring) {
            ESP_LOGI(TAG, "AI suggestion received: %s", text_item->valuestring);
        }

    } else if (strcmp(cmd, "asr_result") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && text_item->valuestring) {
            handle_asr_text(text_item->valuestring);
        }
    } else if (strcmp(cmd, "asr_command") == 0) {
        /* DeepSeek 判断需要开关窗 → 执行 + 语音回复 */
        cJSON *act = cJSON_GetObjectItem(root, "action");
        cJSON *rep = cJSON_GetObjectItem(root, "reply");
        if (act && act->valuestring) {
            if (strcmp(act->valuestring, "open") == 0) {
                servo_set_angle(0.0f);
                control_adaptive_record(servo_get_angle());
            } else if (strcmp(act->valuestring, "close") == 0) {
                servo_set_angle(90.0f);
                control_adaptive_record(servo_get_angle());
            } else if (strcmp(act->valuestring, "timer") == 0) {
                cJSON *tm = cJSON_GetObjectItem(root, "time");
                cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
                if (tm && tm->valuestring && cmd && cmd->valuestring) {
                    control_timer_add_one_shot(tm->valuestring, cmd->valuestring,
                                               rep ? rep->valuestring : NULL);
                    ESP_LOGI(TAG, "Voice timer: %s %s", tm->valuestring, cmd->valuestring);
                }
            }
        }
        if (rep && rep->valuestring) {
            voice_reply_say(rep->valuestring);
        }

    } else if (strcmp(cmd, "asr_reply") == 0) {
        /* DeepSeek 的闲聊回复 → 直接语音播出 */
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && text_item->valuestring) {
            voice_reply_say(text_item->valuestring);
        }

    } else if (strcmp(cmd, "adaptive_plan") == 0) {
        /* 云端大模型返回增强预测计划 */
        cJSON *plan_arr = cJSON_GetObjectItem(root, "plan");
        cJSON *rep = cJSON_GetObjectItem(root, "reply");
        if (plan_arr && cJSON_IsArray(plan_arr)) {
            schedule_plan_t *local = (schedule_plan_t *)control_adaptive_get_plan();
            int n = cJSON_GetArraySize(plan_arr);
            if (n > SCHEDULE_MAX_ENTRIES) n = SCHEDULE_MAX_ENTRIES;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(plan_arr, i);
                if (item) {
                    cJSON *t = cJSON_GetObjectItem(item, "time");
                    cJSON *a = cJSON_GetObjectItem(item, "angle");
                    if (t && t->valuestring && a) {
                        snprintf(local->entries[i].time_str, sizeof(local->entries[i].time_str),
                                 "%s", t->valuestring);
                        local->entries[i].angle = (uint8_t)cJSON_GetNumberValue(a);
                        local->entries[i].confidence = 90;  /* cloud plan = high confidence */
                    }
                }
            }
            local->count = n;
            ESP_LOGI(TAG, "[自适应] 云端计划已应用: %d条", n);
        }
        if (rep && rep->valuestring && rep->valuestring[0]) {
            voice_reply_say(rep->valuestring);
        }
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

        /* 双温湿度传感器 (室内 SHT3x@0x44, 室外 SHT3x@0x45) */
        sht3x_data_t sht_in = {0}, sht_out = {0};
        float t_in = 0, h_in = 0, t_out = 0, h_out = 0;
        if (sht3x_read(SHT3X_INDOOR, &sht_in)) {
            t_in = sht_in.temperature; h_in = sht_in.humidity;
            ESP_LOGI(TAG, "SHT3x[内]: T=%.1f H=%.0f", t_in, h_in);
        }
        if (sht3x_read(SHT3X_OUTDOOR, &sht_out)) {
            t_out = sht_out.temperature; h_out = sht_out.humidity;
            ESP_LOGI(TAG, "SHT3x[外]: T=%.1f H=%.0f", t_out, h_out);
        }
        /* 真实光照数据 (BH1750) */
        float lux_f = 0;
        bh1750_read(&lux_f);
        int light = (int)lux_f;

        /* 烟雾/雨水/气流 — 真实传感器读数 */
        {
            int smoke = 0, rain = 0;
            smoke_sensor_read(&smoke);
            rain_sensor_read(&rain);
            int air_pct = airflow_sensor_read_pct();
            s_last_smoke   = smoke;
            s_last_rain    = rain;
            s_last_airflow = air_pct;
            ESP_LOGI(TAG, "Smoke=%d Rain=%d Airflow=%d%%", smoke, rain, air_pct);
        }

        /* 缓存传感器值供命令响应使用 */
        s_last_temp     = t_in;
        s_last_humi     = h_in;
        s_last_temp_out = t_out;
        s_last_humi_out = h_out;
        s_last_light    = lux_f;

        /* ── 环境感知模式: 传感器驱动窗户 ── */
        if (control_get_mode() == CONTROL_MODE_ENV) {
            env_action_t act = control_env_evaluate(t_in, t_out, h_in, lux_f,
                                                       (float)s_last_smoke,
                                                       (float)s_last_rain);
            if (act == ENV_ACTION_OPEN) {
                float tgt = control_env_target_angle(t_in, h_in, lux_f);
                servo_set_angle(tgt);
            } else if (act == ENV_ACTION_CLOSE) {
                servo_set_angle(90.0f);
            }
            /* ENV_ACTION_NONE: 保持当前角度不变 */
        }

        /* ── 用户自适应模式: 执行预测计划 ── */
        if (control_get_mode() == CONTROL_MODE_ADAPTIVE) {
            /* 传感器感知预测: 每5分钟重新计算一次 (串口输出炫酷匹配过程) */
            static time_t s_last_sensor_predict = 0;
            time_t t_now = time(NULL);
            if (t_now - s_last_sensor_predict >= 300) {
                control_adaptive_predict_sensor_aware(s_last_temp, s_last_humi, s_last_light);
                s_last_sensor_predict = t_now;

                /* 发送精简摘要给云端大模型, 获取增强预测指令 */
                adaptive_send_to_cloud();
            }

            const schedule_plan_t *plan = control_adaptive_get_plan();
            if (plan && plan->count > 0) {
                time_t now = time(NULL);
                struct tm tm;
                localtime_r(&now, &tm);
                char now_str[6];
                snprintf(now_str, sizeof(now_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
                for (int i = 0; i < plan->count; i++) {
                    if (strcmp(now_str, plan->entries[i].time_str) == 0) {
                        servo_set_angle((float)plan->entries[i].angle);
                        control_adaptive_record(plan->entries[i].angle);
                    }
                }
            }
        }

        /* ── 雨棚自动控制: rain > 50% 展开, 否则收起; 手动操作后自动失效 ── */
        if (!s_rain_manual) {
            if (s_last_rain > 50) {
                servo_rain_shelter_set(true);
            } else {
                servo_rain_shelter_set(false);
            }
        }

        /* ── 自然风模式: 扫描风向, 每5分钟重新扫描 ── */
        if (control_get_mode() == CONTROL_MODE_NATURAL) {
            static time_t s_last_scan = 0;
            time_t now = time(NULL);

            /* Apply new scan result if available */
            int best = 0;
            if (wind_scanner_try_apply(&best)) {
                servo_set_natural_wind_angle((float)best);
                ESP_LOGI(TAG, "自然风: 最佳角度=%d°", best);
                s_last_scan = now;
            }

            /* Trigger re-scan every 5 minutes if not already scanning */
            if (!wind_scanner_is_scanning() && (now - s_last_scan) >= 300) {
                wind_scanner_start();
            }
        }

        /* Update LVGL UI (thread-safe with lv_lock) */
        display_lvgl_lock();
        ui_update_sensor(t_in, h_in, t_out, h_out, light);
        ui_update_smoke(s_last_smoke);
        ui_update_rain(s_last_rain);
        ui_update_airflow(s_last_airflow);
        ui_update_mode(servo_get_mode() == SERVO_MODE_AUTO);

        /* Build mode info for screen */
        {
            control_mode_t cm = control_get_mode();
            const char *mn;
            switch (cm) {
            case CONTROL_MODE_MANUAL:   mn = "手动"; break;
            case CONTROL_MODE_ENV:      mn = "环境"; break;
            case CONTROL_MODE_ADAPTIVE: mn = "自适应"; break;
            case CONTROL_MODE_TIMER:    mn = "定时"; break;
            case CONTROL_MODE_NATURAL:  mn = "自然风"; break;
            default:                    mn = "未知"; break;
            }
            char detail[64] = "";
            char sched_text[256] = "";

            switch (cm) {
            case CONTROL_MODE_MANUAL:
                snprintf(detail, sizeof(detail), "手动控制中");
                break;
            case CONTROL_MODE_ENV:
                snprintf(detail, sizeof(detail), "自动: 温%.1f 湿%.0f 光%d",
                         t_in, h_in, light);
                break;
            case CONTROL_MODE_ADAPTIVE: {
                const schedule_plan_t *plan = control_adaptive_get_plan();
                int rec_cnt = 0;
                const recent_op_t *recs = control_adaptive_get_recent_ops(&rec_cnt);

                /* Left column: predicted schedule */
                char pred_left[128] = "预测:\n";
                if (plan && plan->count > 0) {
                    snprintf(detail, sizeof(detail), "学习: %d 条预测", plan->count);
                    int off = strlen(pred_left);
                    for (int i = 0; i < plan->count; i++) {
                        const char *act_name =
                            (strcmp(plan->entries[i].action, "open") == 0)  ? "开" :
                            (strcmp(plan->entries[i].action, "close") == 0) ? "关" : "半";
                        off += snprintf(pred_left + off, sizeof(pred_left) - off,
                                        "%s%d %s (%d%%)\n",
                                        act_name,
                                        plan->entries[i].angle,
                                        plan->entries[i].time_str,
                                        plan->entries[i].confidence);
                    }
                } else {
                    snprintf(detail, sizeof(detail), "学习: 收集中...");
                    snprintf(pred_left, sizeof(pred_left), "预测:\n等待数据...");
                }

                /* Right column: recent operations (newest first) */
                char recent_right[128] = "最近操作:\n";
                if (rec_cnt > 0) {
                    int off = strlen(recent_right);
                    int show = rec_cnt > 5 ? 5 : rec_cnt;
                    for (int i = 0; i < show && off < (int)sizeof(recent_right) - 16; i++) {
                        const char *act_name = recs[i].action;  /* already "开窗"/"关窗"/"半开" */
                        off += snprintf(recent_right + off, sizeof(recent_right) - off,
                                        "%s%d %s\n",
                                        recs[i].time_str, recs[i].angle, act_name);
                    }
                } else {
                    snprintf(recent_right + strlen(recent_right),
                             sizeof(recent_right) - strlen(recent_right),
                             "暂无记录");
                }

                ui_update_adaptive_info(detail, pred_left, recent_right);
                break;
            }
            case CONTROL_MODE_TIMER: {
                char open_t[6], close_t[6];
                bool en, rep;
                control_timer_get_config(open_t, close_t, &en, &rep);
                if (en)
                    snprintf(detail, sizeof(detail), "定时开: %s-%s %s",
                             open_t, close_t, rep ? "每日" : "单次");
                else
                    snprintf(detail, sizeof(detail), "定时已关闭");
                break;
            }
            case CONTROL_MODE_NATURAL: {
                int best = wind_scanner_get_best();
                int spd = wind_scanner_get_best_speed();
                int cur = wind_scanner_get_current_angle();
                if (wind_scanner_is_scanning() && cur >= 0)
                    snprintf(detail, sizeof(detail), "扫描: %d度", cur);
                else if (wind_scanner_is_scanning())
                    snprintf(detail, sizeof(detail), "扫描中...");
                else if (best >= 0)
                    snprintf(detail, sizeof(detail), "最佳: %d度 风速%d%%", best, spd);
                else
                    snprintf(detail, sizeof(detail), "等待扫描...");
                break;
            }
            default:
                break;
            }
            if (cm != CONTROL_MODE_ADAPTIVE) {
                ui_update_mode_info(mn, detail, sched_text);
            }
        }

        /* ── 定时计划显示 (语音/Web设置的one-shot定时) ── */
        {
            char timer_text[256] = "";
            oneshot_t shots[ONE_SHOT_MAX];
            int n = control_timer_get_one_shots(shots, ONE_SHOT_MAX);
            if (n > 0) {
                int off = 0;
                for (int i = 0; i < n && off < (int)sizeof(timer_text) - 16; i++) {
                    const char *act_name = (strcmp(shots[i].action, "open") == 0) ? "开窗" : "关窗";
                    off += snprintf(timer_text + off, sizeof(timer_text) - off,
                                   "%s %s  ", shots[i].time, act_name);
                }
                ui_update_timer_schedule(timer_text);
            } else {
                ui_update_timer_schedule(NULL);
            }
        }
        display_lvgl_unlock();

        char buf[256];
        const char *ctrl_mn = control_mode_name(control_get_mode());
        snprintf(buf, sizeof(buf),
            "{\"temp\":%.1f,\"humidity\":%.1f,\"temp_out\":%.1f,\"humidity_out\":%.1f,\"light\":%d,"
            "\"smoke\":%d,\"rain\":%d,\"airflow\":%d,\"angle\":%.1f,\"mode\":\"%s\",\"ctrl_mode\":\"%s\"}",
            t_in, h_in, t_out, h_out, light, s_last_smoke, s_last_rain, s_last_airflow,
            angle, mode_str, ctrl_mn);
        msg_bus_send("sensor_data", buf);

        /* 推送自适应模式计划表 */
        if (control_get_mode() == CONTROL_MODE_ADAPTIVE) {
            const schedule_plan_t *plan = control_adaptive_get_plan();
            if (plan && plan->count > 0) {
                char sch[512];
                int off = snprintf(sch, sizeof(sch), "{\"entries\":[");
                for (int i = 0; i < plan->count; i++) {
                    off += snprintf(sch + off, sizeof(sch) - off,
                        "%s{\"time\":\"%s\",\"action\":\"%s\",\"angle\":%d,\"confidence\":%d}",
                        i > 0 ? "," : "",
                        plan->entries[i].time_str,
                        plan->entries[i].action,
                        plan->entries[i].angle,
                        plan->entries[i].confidence);
                }
                off += snprintf(sch + off, sizeof(sch) - off, "]}");
                msg_bus_send("schedule", sch);
            }
        }

        /* 推送一次性定时计划到 Web UI */
        {
            oneshot_t shots[ONE_SHOT_MAX];
            int n = control_timer_get_one_shots(shots, ONE_SHOT_MAX);
            cJSON *tsch = cJSON_CreateObject();
            cJSON_AddStringToObject(tsch, "type", "timer_schedule");
            cJSON_AddStringToObject(tsch, "source", "timer");
            cJSON *arr = cJSON_AddArrayToObject(tsch, "entries");
            for (int i = 0; i < n; i++) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "time", shots[i].time);
                cJSON_AddStringToObject(e, "action", shots[i].action);
                cJSON_AddItemToArray(arr, e);
            }
            char *js = cJSON_PrintUnformatted(tsch);
            msg_bus_send("timer_schedule", js);
            free(js);
            cJSON_Delete(tsch);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ── 气流高速采样 (每100ms, 用于实时观测发电机电压/校准) ──
 * 脚本 airflow_voltage_monitor.py 匹配 "AIN2 raw=" 提取 raw 并换算电压。 */
static void airflow_fast_task(void *arg)
{
    while (1) {
        int16_t raw = airflow_sensor_read_raw();
        ESP_LOGI(TAG, "AIN2 raw=%d", (int)raw);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── Timer fire callback: routes through command queue for thread safety ── */
static void on_timer_fire(const char *action, const char *reply)
{
    char *cmd = NULL;
    if (strcmp(action, "open") == 0) {
        cmd = strdup("{\"type\":\"open_blinds\"}");
    } else if (strcmp(action, "close") == 0) {
        cmd = strdup("{\"type\":\"close_blinds\"}");
    }
    if (cmd) {
        xQueueSend(g_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Timer fire queued: %s", action);
    }
    if (reply && reply[0]) {
        voice_reply_say(reply);
    }
}

/* ── 定时器 tick: 每秒检查一次定时计划 ── */
static void timer_tick_task(void *arg)
{
    while (1) {
        control_timer_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
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

/* ── 视频流转发任务: 通过 WebSocket 推送到云端服务器 ── */
#define VIDEO_FRAME_INTERVAL_MS  50    /* ~15-20 fps */
#define VIDEO_JPG_BUF_SIZE       (128 * 1024)
#define VIDEO_B64_BUF_SIZE       (192 * 1024)
#define VC_CHUNK_SIZE            3072

static void video_stream_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint8_t *jpg_buf = heap_caps_malloc(VIDEO_JPG_BUF_SIZE,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *b64_buf = heap_caps_malloc(VIDEO_B64_BUF_SIZE,
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *chunk_msg = heap_caps_malloc(VC_CHUNK_SIZE + 256,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!jpg_buf || !b64_buf || !chunk_msg) {
        ESP_LOGE(TAG, "video buf alloc failed");
        free(jpg_buf); free(b64_buf); free(chunk_msg);
        vTaskDelete(NULL);
        return;
    }

    int frame_seq = 0;
    while (1) {
        if (!msg_bus_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t jpg_len = 0;
        if (camera_get_jpg_copy(jpg_buf, VIDEO_JPG_BUF_SIZE, &jpg_len) != ESP_OK || jpg_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t b64_len = 0;
        if (mbedtls_base64_encode((unsigned char *)b64_buf, VIDEO_B64_BUF_SIZE,
                                  &b64_len, jpg_buf, jpg_len) != 0) {
            vTaskDelay(pdMS_TO_TICKS(VIDEO_FRAME_INTERVAL_MS));
            continue;
        }

        int total = (int)((b64_len + VC_CHUNK_SIZE - 1) / VC_CHUNK_SIZE);
        for (int ci = 0; ci < total; ci++) {
            int off = ci * VC_CHUNK_SIZE;
            int sz = (int)(b64_len - off);
            if (sz > VC_CHUNK_SIZE) sz = VC_CHUNK_SIZE;

            /* Build chunk JSON: header + base64 data + suffix */
            int hdr_len = snprintf(chunk_msg, 256,
                "{\"type\":\"send\",\"payload\":{\"type\":\"vc\",\"s\":%d,\"c\":%d,\"t\":%d,\"d\":\"",
                frame_seq, ci, total);
            memcpy(chunk_msg + hdr_len, b64_buf + off, sz);
            memcpy(chunk_msg + hdr_len + sz, "\"}}", 3);
            int msg_len = hdr_len + sz + 3;

            msg_bus_send_raw_msg(chunk_msg, msg_len);
        }

        if (frame_seq < 3 || frame_seq % 30 == 0) {
            ESP_LOGI(TAG, "frame #%d: %u B JPEG, %u B b64, %d chunks",
                     frame_seq, (unsigned)jpg_len, (unsigned)b64_len, total);
        }
        frame_seq++;
        vTaskDelay(pdMS_TO_TICKS(VIDEO_FRAME_INTERVAL_MS));
    }
}

void app_main(void)
{
    nvs_flash_init();

    control_algorithm_init();
    /* Register callback so timer fires go through command queue (thread-safe) */
    control_set_timer_fire_callback(on_timer_fire);

    esp_log_level_set("wifi", ESP_LOG_INFO);

    /* 命令队列: 解耦 WebSocket 回调与处理, 避免快速连续消息被吞 */
    g_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char *));
    xTaskCreate(command_task, "cmd_task", 4096, NULL, 5, NULL);

    bool sr_ok = false;

    /* 0. 初始化 6 路舵机 (GPIO20-23=百叶, GPIO32-33=雨棚) */
    int servo_gpios[6] = {20, 21, 22, 23, 32, 33};
    if (servo_init(servo_gpios) == 0) {
        /* 雨棚舵机面对面安装, #5 反转使得两舵机同向转动 */
        servo_set_inverted(5, true);
        servo_set_angle(90.0f);
        servo_set_mode(SERVO_MODE_MANUAL);
        g_auto_running = false;
        ESP_LOGI(TAG, "舵机 x6 就绪 (GPIO20-23,32-33, 初始=90°)");
    } else {
        ESP_LOGW(TAG, "舵机初始化失败");
    }

    /* ── Phase 1: I2C bus + Display ── */
    i2c_bus_init();

    /* Targeted I2C scan — only probe known addresses to avoid bus lockup */
    {
        i2c_master_bus_handle_t bus = i2c_bus_get_handle();
        const uint8_t known_addrs[] = {0x18, 0x23, 0x2C, 0x44, 0x48, 0x5D};
        int found = 0;
        ESP_LOGI(TAG, "I2C scan start (targeted)...");
        for (int i = 0; i < sizeof(known_addrs); i++) {
            if (i2c_master_probe(bus, known_addrs[i], 10) == ESP_OK) {
                ESP_LOGI(TAG, "I2C device at 0x%02X", known_addrs[i]);
                found++;
            }
        }
        ESP_LOGI(TAG, "I2C scan done: %d/%d device(s) found",
                 found, (int)sizeof(known_addrs));
    }

    if (display_init() == ESP_OK) {
        ESP_LOGI(TAG, "display OK, starting LVGL...");
        display_lvgl_init();
        ESP_LOGI(TAG, "LVGL init done, starting UI...");
        ui_init();
        ui_set_action_handler(on_ui_action);
        ESP_LOGI(TAG, "UI done");
        display_lvgl_task_start();  /* start LVGL rendering on CPU1 */
        ESP_LOGI(TAG, "LVGL task started");
    } else {
        ESP_LOGW(TAG, "显示屏初始化失败");
    }

    /* ── Phase 2: GT911 touch (BEFORE any I2C traffic from ES8311) ── */
    touch_init();

    /* ── Phase 3: Speech recognition (ES8311 — first I2C user after touch reset) ── */
    if (sr_init() != 0) {
        ESP_LOGW(TAG, "语音识别初始化失败");
    } else {
        sr_on_wake_cb(on_wake_word);
        sr_on_command_cb(on_speech_command);
        sr_on_audio_cb(on_audio_stream);
        sr_start();
        xTaskCreate(speech_task, "speech", 4096, NULL, 4, NULL);
        sr_ok = true;
        ESP_LOGI(TAG, "语音识别就绪");
    }
    /* TTS 播放独立于语音识别 — ES8311/I2S 已在 sr_init 中初始化 */
    voice_reply_init(GPIO_NUM_9);

    /* ── Phase 4: I2C sensors (after ES8311, matching original proven order) ── */
    if (!sht3x_init()) {
        ESP_LOGW(TAG, "SHT3x 初始化失败");
    }
    if (!bh1750_init()) {
        ESP_LOGW(TAG, "BH1750 初始化失败");
    }
    if (!ads1115_init()) {
        ESP_LOGW(TAG, "ADS1115 初始化失败 — 烟雾/雨水传感器不可用");
    }
    smoke_sensor_init();
    airflow_sensor_init();
    rain_sensor_init();
    /* 传感器任务 — 所有 I2C 设备就绪后启动 (msg_bus_send 在未连接时安全返回 -1) */
    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Sensor task started");

    /* 气流高速采样任务 (100ms) — 校准用, 输出 "AIN2 raw=" 供监视脚本解析 */
    xTaskCreate(airflow_fast_task, "airflow_fast", 2048, NULL, 3, NULL);

    /* I2C bus handle stays valid — all devices share it.
       Sensors+touch already have their handles; camera SCCB creates its own later. */

    /* 3. 网络初始化 (后台任务, 避免阻塞 app_main 导致 IDLE0 看门狗超时) */
    xTaskCreate(network_init_task, "net_init", 5120, NULL, 4, NULL);

    /* 4. 自动模式扫风任务 (始终运行, 由 g_auto_running 控制是否转动) */
    xTaskCreate(auto_sweep_task, "auto_sweep", 3072, NULL, 4, NULL);

    /* 5. 定时器 tick (每秒检查一次) */
    xTaskCreate(timer_tick_task, "timer_tick", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "系统就绪 (语音=%s, 网络=后台连接中, 舵机x6=GPIO20-23,32-33)",
             sr_ok ? "ON" : "OFF");

    /* Wind scanner: init last to avoid I2C bus timing conflict */
    vTaskDelay(pdMS_TO_TICKS(500));
    if (wind_scanner_init()) {
        ESP_LOGI(TAG, "Wind scanner ready");
    } else {
        ESP_LOGE(TAG, "Wind scanner init failed");
    }
}
