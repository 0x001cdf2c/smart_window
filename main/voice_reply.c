#include "voice_reply.h"
#include "speech_recognition.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"

static const char *TAG = "VOICE";

#define PA_CTRL_GPIO  GPIO_NUM_53
#define TTS_QUEUE_LEN 8
#define STEREO_BUF_MAX 4096  /* max chunk samples (mono), doubled for stereo */

extern const unsigned char voice_data_xiaole_dat[];
extern const unsigned int voice_data_xiaole_dat_len;

static i2s_chan_handle_t tx_chan = NULL;
static esp_tts_handle_t  tts_hdl = NULL;
static bool initialized = false;
static QueueHandle_t tts_queue = NULL;
static int16_t *s_stereo_buf = NULL;  /* pre-allocated */

/* 只保留语音集支持的字 (CJK 统一表意 + ASCII 字母数字/空格),
 * 剔除标点/全角符号/表情, 否则 esp_tts_parse_chinese 遇到不支持字符会 assert 崩溃 */
static void tts_sanitize(char *out, size_t out_cap, const char *in)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 1 < out_cap) {
        unsigned int cp = 0;
        int len = 0;
        if (*p < 0x80) {
            cp = *p; len = 1;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4;
        } else {
            p++;  /* 非法序列, 跳过 */
            continue;
        }

        bool keep = false;
        if (cp >= 0x4E00 && cp <= 0x9FFF) keep = true;                     /* CJK 统一表意 */
        else if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') ||
                 (cp >= 'A' && cp <= 'Z') || cp == ' ') keep = true;       /* ASCII 字母数字/空格 */

        if (keep && o + len < out_cap) {
            memcpy(out + o, p, len);
            o += len;
        }
        p += len;
    }
    out[o] = '\0';
}

/* ── 异步 TTS 播放任务 (TX 通道始终开启, 避免反复开关干扰 I2S 时钟) ── */
static void tts_task(void *arg)
{
    char *text = NULL;
    while (1) {
        if (xQueueReceive(tts_queue, &text, portMAX_DELAY) == pdTRUE) {
            if (!text || text[0] == '\0') { free(text); continue; }

            char clean[512];
            tts_sanitize(clean, sizeof(clean), text);
            ESP_LOGI(TAG, "播放: %s", clean);

            esp_tts_stream_reset(tts_hdl);
            if (esp_tts_parse_chinese(tts_hdl, clean) != 0) {
                int chunk_len;
                short *chunk;
                while ((chunk = esp_tts_stream_play(tts_hdl, &chunk_len, 3)) != NULL && chunk_len > 0) {
                    if (chunk_len > STEREO_BUF_MAX) chunk_len = STEREO_BUF_MAX;
                    for (int i = 0; i < chunk_len; i++) {
                        s_stereo_buf[i * 2]     = chunk[i];
                        s_stereo_buf[i * 2 + 1] = chunk[i];
                    }
                    size_t written = 0;
                    i2s_channel_write(tx_chan, s_stereo_buf,
                                      chunk_len * 2 * sizeof(int16_t),
                                      &written, pdMS_TO_TICKS(1000));
                }
            } else {
                ESP_LOGW(TAG, "TTS 解析失败: %s", text);
            }
            vTaskDelay(pdMS_TO_TICKS(60));
            free(text);
        }
    }
}

int voice_reply_init(int dout_gpio)
{
    if (initialized) return 0;

    tx_chan = sr_get_tx_chan();
    if (!tx_chan) {
        ESP_LOGE(TAG, "I2S TX 通道未就绪, 请先初始化语音识别");
        return -1;
    }

    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole,
        (void *)voice_data_xiaole_dat);
    if (!voice) {
        ESP_LOGE(TAG, "TTS 语音模型加载失败");
        tx_chan = NULL;
        return -1;
    }

    tts_hdl = esp_tts_create(voice);
    if (!tts_hdl) {
        ESP_LOGE(TAG, "TTS 创建失败");
        tx_chan = NULL;
        return -1;
    }

    /* TX 通道开启一次, 之后不再开关 (避免干扰 I2S 共享时钟导致 WiFi 断连) */
    i2s_channel_enable(tx_chan);
    sr_recover_rx();
    sr_configure_playback();

    /* PA (功放) 控制: GPIO53 高电平使能 */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = BIT64(PA_CTRL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_CTRL_GPIO, 1);

    /* 预分配立体声缓冲, 避免播放时反复 malloc/free */
    s_stereo_buf = calloc(STEREO_BUF_MAX * 2, sizeof(int16_t));
    if (!s_stereo_buf) {
        ESP_LOGE(TAG, "立体声缓冲分配失败");
        return -1;
    }

    tts_queue = xQueueCreate(TTS_QUEUE_LEN, sizeof(char *));
    xTaskCreate(tts_task, "tts_task", 4096, NULL, 5, NULL);

    initialized = true;
    ESP_LOGI(TAG, "语音回复就绪 (TTS=小乐, 异步, TX 常开)");
    return 0;
}

void voice_reply_say(const char *text)
{
    if (!initialized || !tx_chan || !tts_hdl || !text || text[0] == '\0') return;

    /* 丢掉队列中最旧的待播消息, 避免堆积 */
    char *stale = NULL;
    while (uxQueueMessagesWaiting(tts_queue) > 0) {
        if (xQueueReceive(tts_queue, &stale, 0) == pdTRUE) free(stale);
    }

    char *copy = strdup(text);
    if (copy) {
        xQueueSend(tts_queue, &copy, 0);
    }
}
