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

/* ── 异步 TTS 播放任务 (TX 通道始终开启, 避免反复开关干扰 I2S 时钟) ── */
static void tts_task(void *arg)
{
    char *text = NULL;
    while (1) {
        if (xQueueReceive(tts_queue, &text, portMAX_DELAY) == pdTRUE) {
            if (!text || text[0] == '\0') { free(text); continue; }

            ESP_LOGI(TAG, "播放: %s", text);

            esp_tts_stream_reset(tts_hdl);
            if (esp_tts_parse_chinese(tts_hdl, text) != 0) {
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

    sr_recover_rx();
    sr_configure_playback();

    /* 预分配立体声缓冲, 避免播放时反复 malloc/free */
    s_stereo_buf = calloc(STEREO_BUF_MAX * 2, sizeof(int16_t));
    if (!s_stereo_buf) {
        ESP_LOGE(TAG, "立体声缓冲分配失败");
        return -1;
    }

    /* TX 通道开启一次, 之后不再开关 (避免干扰 I2S 共享时钟导致 WiFi 断连) */
    esp_err_t ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX 启用失败: %s", esp_err_to_name(ret));
        free(s_stereo_buf);
        return -1;
    }

    gpio_set_level(PA_CTRL_GPIO, 1);

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
