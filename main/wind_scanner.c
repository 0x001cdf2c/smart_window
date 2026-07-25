#include "wind_scanner.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "WIND_SCAN";

#define SCAN_SERVO_GPIO     GPIO_NUM_4
#define WIND_SENSOR_GPIO    GPIO_NUM_48
#define SWEEP_DELAY_MS      15
#define SERVO_PERIOD_US     20000
#define PULSE_MIN_US        500
#define PULSE_MAX_US        2500

typedef enum {
    PHASE_IDLE = 0,
    PHASE_90_TO_180,
    PHASE_180_TO_0,
    PHASE_0_TO_90,
} sweep_phase_t;

static bool                s_scanning = false;
static bool                s_new_result = false;
static int                 s_best_angle = -1;
static int                 s_current_angle = -1;
static SemaphoreHandle_t   s_mutex = NULL;
static TaskHandle_t        s_task = NULL;

static rmt_channel_handle_t   s_rmt_chan = NULL;
static rmt_encoder_handle_t   s_copy_encoder = NULL;
static bool                   s_rmt_active = false;

static uint32_t degree_to_pulse(int degree)
{
    if (degree < 0) degree = 0;
    if (degree > 180) degree = 180;
    return PULSE_MIN_US + (PULSE_MAX_US - PULSE_MIN_US) * degree / 180;
}

/* Send a continuous servo signal at the given angle.
   Call again to change angle; call servo_stop() to cut power. */
static void servo_start(int degree)
{
    uint32_t pulse_us = degree_to_pulse(degree);

    rmt_symbol_word_t sym = {
        .duration0 = (uint16_t)pulse_us,
        .level0    = 1,
        .duration1 = (uint16_t)(SERVO_PERIOD_US - pulse_us),
        .level1    = 0,
    };

    rmt_transmit_config_t tx_cfg = { .loop_count = -1 };

    if (s_rmt_active) {
        rmt_disable(s_rmt_chan);
        rmt_enable(s_rmt_chan);
    }
    esp_err_t err = rmt_transmit(s_rmt_chan, s_copy_encoder, &sym,
                 sizeof(sym), &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
        return;
    }
    s_rmt_active = true;

    ESP_LOGI(TAG, "Servo -> %d deg  pulse=%lu us", (int)degree, (unsigned long)pulse_us);
}

static void servo_stop(void)
{
    if (!s_rmt_active) return;
    rmt_disable(s_rmt_chan);
    rmt_enable(s_rmt_chan);
    s_rmt_active = false;
    ESP_LOGI(TAG, "Servo stopped");
}

static bool read_wind(void)
{
    return (gpio_get_level(WIND_SENSOR_GPIO) == 0);
}

static void scanner_task(void *arg)
{
    int wind_hits[181] = {0};
    int wind_total[181] = {0};

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Scanner woke, starting sweep...");

        for (int i = 0; i <= 180; i++) {
            wind_hits[i] = 0;
            wind_total[i] = 0;
        }

        int deg = 90;
        sweep_phase_t phase = PHASE_90_TO_180;
        servo_start(90);
        ESP_LOGI(TAG, "Scan start: 90→180→0→90");

        while (phase != PHASE_IDLE) {
            if (deg >= 0 && deg <= 180) {
                wind_total[deg]++;
                if (read_wind()) wind_hits[deg]++;
            }

            switch (phase) {
            case PHASE_90_TO_180:
                deg++;
                if (deg > 180) { phase = PHASE_180_TO_0; deg = 179; }
                break;
            case PHASE_180_TO_0:
                deg--;
                if (deg < 0) { phase = PHASE_0_TO_90; deg = 1; }
                break;
            case PHASE_0_TO_90:
                deg++;
                if (deg >= 90) {
                    phase = PHASE_IDLE;
                }
                break;
            default:
                break;
            }

            if (phase != PHASE_IDLE) {
                s_current_angle = deg;
                servo_start(deg);
                vTaskDelay(pdMS_TO_TICKS(SWEEP_DELAY_MS));
            }
        }

        /* Hold at 90° briefly then release */
        servo_start(90);
        vTaskDelay(pdMS_TO_TICKS(200));
        servo_stop();

        int best = 150;
        int best_hits = 0;
        for (int i = 0; i <= 180; i++) {
            if (wind_hits[i] > best_hits) {
                best_hits = wind_hits[i];
                best = i;
            }
        }

        if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_best_angle = best;
        s_current_angle = -1;
        s_new_result = true;
        s_scanning = false;
        if (s_mutex) xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "Scan done: best=%d° (hits=%d/%d)", best,
                 best_hits, best_hits > 0 ? wind_total[best] : 0);
    }
}

bool wind_scanner_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Mutex create failed");
        return false;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .gpio_num            = SCAN_SERVO_GPIO,
        .mem_block_symbols   = 64,
        .resolution_hz       = 1000000,
        .trans_queue_depth   = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_rmt_chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT TX channel create failed");
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return false;
    }

    if (rmt_enable(s_rmt_chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT enable failed");
        return false;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &s_copy_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "RMT copy encoder create failed");
        return false;
    }

    /* Quick centring pulses, then stop — don't hold position when idle */
    for (int i = 0; i < 5; i++) {
        servo_start(90);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    servo_stop();

    xTaskCreate(scanner_task, "wind_scan", 4096, NULL, 5, &s_task);

    ESP_LOGI(TAG, "Wind scanner ready (RMT GPIO%u, sensor GPIO%u, init=90°)",
             SCAN_SERVO_GPIO, WIND_SENSOR_GPIO);
    return true;
}

bool wind_scanner_start(void)
{
    if (s_scanning) return false;
    if (!s_mutex || !s_task) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scanning = true;
    s_new_result = false;
    xSemaphoreGive(s_mutex);

    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "Scan triggered");
    return true;
}

bool wind_scanner_is_scanning(void)
{
    return s_scanning;
}

int wind_scanner_get_current_angle(void)
{
    return s_current_angle;
}

int wind_scanner_get_best(void)
{
    int best = -1;
    if (!s_mutex) return best;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    best = s_best_angle;
    xSemaphoreGive(s_mutex);
    return best;
}

bool wind_scanner_try_apply(int *best_angle)
{
    if (!best_angle || !s_mutex) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has_new = s_new_result;
    if (has_new) {
        *best_angle = s_best_angle;
        s_new_result = false;
    }
    xSemaphoreGive(s_mutex);
    return has_new;
}
