#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "es8311.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "wake_words.h"
#include "speech_recognition.h"

static const char *TAG = "SR";

/* ── I2S 引脚 (Waveshare ESP32-P4-WIFI6-DEV-KIT) ── */
#define I2S_MCK   GPIO_NUM_13
#define I2S_BCK   GPIO_NUM_12
#define I2S_WS    GPIO_NUM_10
#define I2S_DIN   GPIO_NUM_11
#define I2S_DOUT  GPIO_NUM_NC   /* 本应用只用录音，不播放 */

/* ── ES8311 I2C 控制 ── */
#define ES8311_I2C_PORT    I2C_NUM_0
#define ES8311_I2C_SDA     GPIO_NUM_7
#define ES8311_I2C_SCL     GPIO_NUM_8
#define ES8311_I2C_ADDR    ES8311_ADDRRES_0

/* ── PA 功放使能 ── */
#define PA_CTRL_GPIO       GPIO_NUM_53

/* ── 音频参数 ── */
#define SAMPLE_RATE       16000
#define MCLK_MULTIPLE     384
#define MCLK_FREQ_HZ      (SAMPLE_RATE * MCLK_MULTIPLE)  /* 6.144 MHz */
#define AFE_TIMEOUT_MS    2000

/* ── 状态 ── */
static i2s_chan_handle_t rx_chan = NULL;
static es8311_handle_t es8311_hdl = NULL;

static esp_afe_sr_data_t *afe_data = NULL;
static const esp_afe_sr_iface_t *afe_handle = NULL;
static srmodel_list_t *sr_models = NULL;

static model_iface_data_t *mn_data = NULL;
static esp_mn_iface_t *mn_handle = NULL;

static int feed_chunksize;
static int feed_channels;
static int fetch_chunksize;

static sr_on_wake_t   on_wake   = NULL;
static sr_on_command_t on_cmd   = NULL;

static bool mn_active = false;

/* ── ES8311 初始化 (使用 espressif/es8311 库, 匹配 Waveshare 参考代码) ── */
static void es8311_codec_init(void)
{
    /* I2C 初始化 */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = ES8311_I2C_SDA,
        .scl_io_num = ES8311_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(ES8311_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(ES8311_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    /* 创建 ES8311 句柄 */
    es8311_hdl = es8311_create(ES8311_I2C_PORT, ES8311_I2C_ADDR);
    if (!es8311_hdl) {
        ESP_LOGE(TAG, "es8311_create 失败");
        return;
    }
    ESP_LOGI(TAG, "ES8311 句柄创建成功");

    /* 时钟配置 */
    es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = MCLK_FREQ_HZ,   /* 6.144 MHz */
        .sample_frequency = SAMPLE_RATE,  /* 16 kHz */
    };

    /* 初始化 ES8311 */
    esp_err_t ret = es8311_init(es8311_hdl, &es_clk,
                                 ES8311_RESOLUTION_16,  /* ADC 16-bit */
                                 ES8311_RESOLUTION_16); /* DAC 16-bit */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init 失败: %s", esp_err_to_name(ret));
        return;
    }

    /* 配置采样率 */
    ret = es8311_sample_frequency_config(es8311_hdl, MCLK_FREQ_HZ, SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "采样率配置失败: %s", esp_err_to_name(ret));
    }

    /* 配置麦克风 (false = 模拟麦克风) */
    ret = es8311_microphone_config(es8311_hdl, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "麦克风配置失败: %s", esp_err_to_name(ret));
    }

    /* PA 使能 */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = BIT64(PA_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_CTRL_GPIO, 1);

    ESP_LOGI(TAG, "ES8311 就绪 (MCLK=I2S/GPIO%d, %ld Hz, 模拟麦克风)",
             I2S_MCK, (long)MCLK_FREQ_HZ);
}

/* ── I2S 初始化 (匹配 Waveshare 参考: STEREO, 标准宏) ── */
static int i2s_mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_WS,
            .dout = I2S_DOUT,
            .din  = I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "I2S 主模式就绪 (MCLK=GPIO%d, STEREO %d Hz)",
             I2S_MCK, SAMPLE_RATE);
    return 0;
}

/* ── AFE 初始化 (WakeNet) ── */
static int afe_init(void)
{
    sr_models = esp_srmodel_init("model");
    if (!sr_models || sr_models->num <= 0) {
        ESP_LOGE(TAG, "未找到 srmodel 分区, 请创建 models 分区并烧录模型");
        return -1;
    }
    ESP_LOGI(TAG, "找到 %d 个语音模型", sr_models->num);

    /* 从 WAKE_WORDS 中查找第一个可用的唤醒模型 */
    const char *wn_name = NULL;
    for (int i = 0; WAKE_WORDS[i] != NULL; i++) {
        if (esp_srmodel_exists(sr_models, (char *)WAKE_WORDS[i]) >= 0) {
            wn_name = WAKE_WORDS[i];
            ESP_LOGI(TAG, "使用唤醒模型: %s (%s)", wn_name, WAKE_WORD_NAMES[i]);
            break;
        }
    }
    if (!wn_name) {
        ESP_LOGE(TAG, "未找到可用的 WakeNet 模型");
        return -1;
    }

    afe_config_t *cfg = afe_config_init("M", sr_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init 失败");
        return -1;
    }

    cfg->wakenet_init = true;
    cfg->wakenet_model_name = (char *)wn_name;
    cfg->wakenet_mode = DET_MODE_90;
    cfg->afe_type = AFE_TYPE_SR;
    cfg->afe_ringbuf_size = 12;

    /* 减少算法开销 (单麦克风场景) */
    cfg->aec_init = false;
    cfg->se_init = false;

    cfg = afe_config_check(cfg);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_check 失败");
        return -1;
    }

    afe_handle = esp_afe_handle_from_config(cfg);
    if (!afe_handle) {
        ESP_LOGE(TAG, "无法获取 AFE 句柄");
        afe_config_free(cfg);
        return -1;
    }

    afe_data = afe_handle->create_from_config(cfg);
    if (!afe_data) {
        ESP_LOGE(TAG, "创建 AFE 实例失败");
        afe_config_free(cfg);
        return -1;
    }

    feed_chunksize  = afe_handle->get_feed_chunksize(afe_data);
    feed_channels   = afe_handle->get_feed_channel_num(afe_data);
    fetch_chunksize = afe_handle->get_fetch_chunksize(afe_data);

    ESP_LOGI(TAG, "AFE: feed=%d samples, %dch, fetch=%d samples",
             feed_chunksize, feed_channels, fetch_chunksize);

    afe_config_free(cfg);
    return 0;
}

/* ── MultiNet 初始化 ── */
static int multinet_init(void)
{
    if (!sr_models) return -1;

    char *mn_name = esp_srmodel_filter(sr_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGW(TAG, "未找到中文 MultiNet 模型, 仅启用唤醒词检测");
        return -1;
    }
    ESP_LOGI(TAG, "MultiNet 模型: %s", mn_name);

    mn_handle = esp_mn_handle_from_name(mn_name);
    if (!mn_handle) {
        ESP_LOGE(TAG, "无法获取 MultiNet 句柄");
        return -1;
    }

    mn_data = mn_handle->create(mn_name, AFE_TIMEOUT_MS);
    if (!mn_data) {
        ESP_LOGE(TAG, "创建 MultiNet 实例失败");
        return -1;
    }

    /* 添加命令词 */
    esp_mn_commands_alloc(mn_handle, mn_data);
    for (int i = 0; SPEECH_COMMANDS[i] != NULL; i++) {
        esp_mn_commands_add(i + 1, SPEECH_COMMANDS[i]);
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        ESP_LOGW(TAG, "部分命令词未能添加 (num=%d)", err->num);
    }

    int mn_chunksize = mn_handle->get_samp_chunksize(mn_data);
    int cmd_count = 0;
    while (SPEECH_COMMANDS[cmd_count] != NULL) cmd_count++;
    ESP_LOGI(TAG, "MultiNet: chunksize=%d samples, %d 条命令词已加载",
             mn_chunksize, cmd_count);

    /* 打印载入的命令词列表 */
    if (mn_handle->print_active_speech_commands) {
        mn_handle->print_active_speech_commands(mn_data);
    }

    mn_active = false;
    return 0;
}

/* ── 公共 API ── */

int sr_init(void)
{
    /* 1. 外设初始化 */
    es8311_codec_init();
    i2s_mic_init();

    /* 2. AFE + WakeNet */
    if (afe_init() != 0) {
        ESP_LOGE(TAG, "AFE 初始化失败");
        return -1;
    }

    /* 3. MultiNet (可选) */
    multinet_init();

    ESP_LOGI(TAG, "语音识别初始化完成");
    return 0;
}

void sr_on_wake_cb(sr_on_wake_t cb)   { on_wake = cb; }
void sr_on_command_cb(sr_on_command_t cb) { on_cmd = cb; }

void sr_start(void)
{
    if (!afe_data || !afe_handle) {
        ESP_LOGE(TAG, "AFE 未初始化, 无法启动");
        return;
    }
    ESP_LOGI(TAG, "语音识别已启动");
}

/* ── 音频处理: 从 I2S (立体声) 喂数据到 AFE (单声道) ── */
static int16_t *i2s_buf = NULL;   /* 立体声原始数据 */
static int16_t *afe_buf = NULL;   /* 提取左声道后喂给 AFE */

void sr_poll(void)
{
    if (!rx_chan || !afe_data || !afe_handle) return;

    int mono_samples = feed_channels * feed_chunksize;
    int stereo_samples = mono_samples * 2;

    if (!i2s_buf) {
        i2s_buf = heap_caps_calloc(stereo_samples, sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!i2s_buf) {
            ESP_LOGE(TAG, "I2S 立体声缓冲区分配失败");
            return;
        }
    }
    if (!afe_buf) {
        afe_buf = heap_caps_calloc(mono_samples, sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!afe_buf) {
            ESP_LOGE(TAG, "AFE 单声道缓冲区分配失败");
            return;
        }
    }

    /* 从 I2S 读取立体声音频 */
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_chan, i2s_buf,
                                      stereo_samples * sizeof(int16_t),
                                      &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK || bytes_read == 0) return;

    /* 立体声 → 单声道: 取左声道 */
    for (int i = 0; i < mono_samples; i++) {
        afe_buf[i] = i2s_buf[i * 2];
    }

    /* 周期性检测音频信号 */
    {
        static int dbg_cnt = 0;
        if (++dbg_cnt >= 100) {
            dbg_cnt = 0;
            int64_t sum_sq_l = 0, sum_sq_r = 0;
            for (int i = 0; i < mono_samples; i++) {
                int64_t sl = i2s_buf[i * 2];
                int64_t sr = i2s_buf[i * 2 + 1];
                sum_sq_l += sl * sl;
                sum_sq_r += sr * sr;
            }
            float rms_l = sqrtf((float)(sum_sq_l / mono_samples));
            float rms_r = sqrtf((float)(sum_sq_r / mono_samples));
            ESP_LOGI(TAG, "AUDIO: L RMS=%.1f  R RMS=%.1f  raw[0..3]=L%d,R%d L%d,R%d",
                     rms_l, rms_r,
                     (int)i2s_buf[0], (int)i2s_buf[1],
                     (int)i2s_buf[2], (int)i2s_buf[3]);
        }
    }

    /* 喂入 AFE (单声道) */
    afe_handle->feed(afe_data, afe_buf);

    /* ── 命令识别模式下, 同时喂原始音频给 MultiNet ── */
    if (mn_active && mn_handle && mn_data) {
        int mn_chunksize = mn_handle->get_samp_chunksize(mn_data);
        esp_mn_state_t state = mn_handle->detect(mn_data, afe_buf);

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_res = mn_handle->get_results(mn_data);
            if (mn_res && mn_res->num > 0) {
                ESP_LOGI(TAG, "=========================================");
                ESP_LOGI(TAG, "  ✓ 识别命令: %s", mn_res->string);
                ESP_LOGI(TAG, "  置信度: %d%%", (int)(mn_res->prob[0] * 100));
                ESP_LOGI(TAG, "=========================================");
                if (on_cmd) on_cmd(mn_res->string);
            }
            mn_active = false;
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "命令识别超时, 回到唤醒监听");
            mn_active = false;
        }
    }

    /* 获取 AFE 结果 */
    afe_fetch_result_t *res = afe_handle->fetch_with_delay(afe_data, pdMS_TO_TICKS(50));
    if (!res || res->ret_value < 0) return;

    /* 周期性打印 AFE 状态 (每 100 次 fetch, 约 3 秒) */
    {
        static int afe_dbg_cnt = 0;
        if (++afe_dbg_cnt >= 100) {
            afe_dbg_cnt = 0;
            ESP_LOGI(TAG, "AFE state: wakeup=%d VAD=%d data_size=%d mn_active=%d",
                     (int)res->wakeup_state,
                     (int)res->vad_state,
                     (int)res->data_size,
                     (int)mn_active);
        }
    }

    /* ── 唤醒词检测 ── */
    if (res->wakeup_state == WAKENET_DETECTED && res->wake_word_index > 0) {
        /* wakenet_model_index: 多模型时区分哪个模型触发 (从1开始)
         * wake_word_index: 该模型内的唤醒词索引 (从1开始) */
        int model_idx = res->wakenet_model_index - 1;
        int word_idx  = res->wake_word_index - 1;
        int total = sizeof(WAKE_WORD_NAMES) / sizeof(WAKE_WORD_NAMES[0]);
        int name_idx = (model_idx >= 0 && model_idx < total) ? model_idx : word_idx;
        const char *name = (name_idx >= 0 && name_idx < total)
                           ? WAKE_WORD_NAMES[name_idx] : "未知";
        ESP_LOGI(TAG, "=========================================");
        ESP_LOGI(TAG, "  ★ 唤醒词检测: [%s] (model=%d, word=%d)",
                 name, res->wakenet_model_index, res->wake_word_index);
        ESP_LOGI(TAG, "=========================================");

        if (on_wake) on_wake(res->wake_word_index, name);

        /* 启动 MultiNet 命令识别 */
        if (mn_handle && mn_data) {
            mn_active = true;
            mn_handle->clean(mn_data);
            ESP_LOGI(TAG, "等待语音命令...");
        }
    }
}
