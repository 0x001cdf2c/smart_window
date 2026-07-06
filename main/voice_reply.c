#include "voice_reply.h"
#include "speech_recognition.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"

static const char *TAG = "VOICE";

#define PA_CTRL_GPIO  GPIO_NUM_53

/* 嵌入的 xiaole 语音数据 (voice_data_xiaole.c) */
extern const unsigned char voice_data_xiaole_dat[];
extern const unsigned int voice_data_xiaole_dat_len;

static i2s_chan_handle_t tx_chan = NULL;
static esp_tts_handle_t  tts_hdl = NULL;
static bool initialized = false;

int voice_reply_init(int dout_gpio)
{
    if (initialized) return 0;

    /* 获取 speech_recognition 已创建的 I2S TX 通道 */
    tx_chan = sr_get_tx_chan();
    if (!tx_chan) {
        ESP_LOGE(TAG, "I2S TX 通道未就绪, 请先初始化语音识别");
        return -1;
    }

    /* ESP-TTS (小乐): 第二参数传入嵌入的语音数据 */
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

    /* esp_tts_create 可能干扰 I2S RX 通道状态, 恢复之 */
    sr_recover_rx();

    /* 配置 ES8311 播放: 音量 + 取消静音 */
    sr_configure_playback();

    /* PA 使能 */
    gpio_set_level(PA_CTRL_GPIO, 1);

    initialized = true;
    ESP_LOGI(TAG, "语音回复就绪 (TTS=小乐, I2S TX 共享)");
    return 0;
}

void voice_reply_say(const char *text)
{
    if (!initialized || !tx_chan || !tts_hdl || !text || text[0] == '\0') return;

    ESP_LOGI(TAG, "播放: %s", text);

    /* 1. 启用 TX (RX 保持运行提供共享时钟) */
    esp_err_t ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX 启用失败: %s", esp_err_to_name(ret));
        return;
    }

    /* 2. TTS 合成 + 播放 */
    esp_tts_stream_reset(tts_hdl);
    if (esp_tts_parse_chinese(tts_hdl, text) == 0) {
        ESP_LOGW(TAG, "TTS 解析失败: %s", text);
        goto done;
    }

    int chunk_len;
    short *chunk;
    int total_written = 0;
    while ((chunk = esp_tts_stream_play(tts_hdl, &chunk_len, 3)) != NULL && chunk_len > 0) {
        /* 单声道 -> 立体声 (ES8311 需要) */
        size_t stereo_samples = chunk_len * 2;
        int16_t *stereo = malloc(stereo_samples * sizeof(int16_t));
        if (!stereo) break;
        for (int i = 0; i < chunk_len; i++) {
            stereo[i * 2]     = chunk[i];
            stereo[i * 2 + 1] = chunk[i];
        }
        size_t written = 0;
        ret = i2s_channel_write(tx_chan, stereo,
                                stereo_samples * sizeof(int16_t),
                                &written, pdMS_TO_TICKS(5000));
        total_written += written;
        free(stereo);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2S TX 写入失败: %s", esp_err_to_name(ret));
            break;
        }
    }
    ESP_LOGI(TAG, "I2S TX 完成: %d bytes", total_written);

done:
    /* 3. 等待数据发送完毕 */
    vTaskDelay(pdMS_TO_TICKS(80));

    /* 4. 关闭 TX */
    i2s_channel_disable(tx_chan);
}
