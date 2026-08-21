#include "airflow_sensor.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "AIRFLOW";

/* 霍尔风杯风速传感器 → D0 数字脉冲 → GPIO48
 * 风杯每转一圈, 磁铁经过霍尔一次, D0 拉低一段时间 (0=有风)
 * 一段低电压只算一次: 下降沿 (HIGH→LOW = 磁铁靠近) 计一圈
 * 磁铁停在霍尔前时 D0 = LOW + 噪声毛刺 → 只注册下降沿中断(NEGEDGE),
 *   用"两次下降沿最小间隔"过滤噪声: 真实一圈间隔 >100ms, 噪声毛刺间隔极短。
 *   量程 10 m/s ≈ 8.8Hz → 一圈 113ms, 阈值 80ms 余量充足, 不吞量程内圈。
 * 风速 = K × 2πR × 转速(Hz), 参数取自 思路/风速传感示例.txt */
#define AIRFLOW_GPIO           GPIO_NUM_48
#define AIRFLOW_R_M            0.082f          /* 风杯旋转半径 (m) */
#define AIRFLOW_K              2.2f            /* DIY 风杯修正系数 (2.0~2.5) */
#define AIRFLOW_MS_PER_HZ      (AIRFLOW_K * 2.0f * 3.14159265f * AIRFLOW_R_M)  /* ≈1.133 m/s per Hz */
#define AIRFLOW_MAX_MS         10.0f           /* 满量程风速(m/s) = 100% */
#define AIRFLOW_MIN_INTERVAL_MS 80             /* 两次下降沿最小间隔 (滤磁铁位噪声) */

static SemaphoreHandle_t s_sem = NULL;
static volatile uint32_t s_pulse_count = 0;    /* 累计圈数 */
static volatile uint32_t s_last_fall_ms = 0;   /* 最近一次下降沿时刻 */
static float s_wind_ms  = 0.0f;                /* 当前风速 m/s */
static int   s_wind_pct = 0;                   /* 0-100% */

static void IRAM_ATTR airflow_isr(void *arg)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (now_ms - s_last_fall_ms >= AIRFLOW_MIN_INTERVAL_MS) {
        s_pulse_count++;                       /* 距上次下降沿够久 = 新一圈 */
    }
    s_last_fall_ms = now_ms;

    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &hpw);
    portYIELD_FROM_ISR(hpw);
}

static void airflow_task(void *arg)
{
    uint32_t last_count = 0;
    uint32_t last_report_ms = (uint32_t)(esp_timer_get_time() / 1000);

    while (1) {
        /* 阻塞等脉冲唤醒; 无脉冲时最多 1s 超时, 正好做每秒汇总 */
        xSemaphoreTake(s_sem, pdMS_TO_TICKS(1000));

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_report_ms >= 1000) {
            uint32_t count = s_pulse_count;
            uint32_t rps = count - last_count;
            last_count = count;
            last_report_ms = now_ms;
            s_wind_ms = (float)rps * AIRFLOW_MS_PER_HZ;
            s_wind_pct = (int)(s_wind_ms * 100.0f / AIRFLOW_MAX_MS);
            if (s_wind_pct > 100) s_wind_pct = 100;
            if (s_wind_pct < 0) s_wind_pct = 0;
        }
    }
}

bool airflow_sensor_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << AIRFLOW_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 霍尔 D0 常为开漏, 需上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,    /* 只下降沿: 不读电平, 避免噪声误判 */
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed");
        return false;
    }

    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) {
        ESP_LOGE(TAG, "Semaphore create failed");
        return false;
    }

    s_last_fall_ms = (uint32_t)(esp_timer_get_time() / 1000);  /* 避免启动瞬间误计 */

    if (gpio_install_isr_service(0) != ESP_OK) {
        ESP_LOGE(TAG, "ISR service install failed");
        return false;
    }
    gpio_isr_handler_add(AIRFLOW_GPIO, airflow_isr, NULL);

    xTaskCreate(airflow_task, "airflow", 4096, NULL, 1, NULL);

    ESP_LOGI(TAG, "Airflow ready (D0→GPIO%u, 下降沿计数+最小间隔%dms, %.3f m/s/Hz, max=%.0f m/s)",
             (unsigned)AIRFLOW_GPIO, AIRFLOW_MIN_INTERVAL_MS,
             (double)AIRFLOW_MS_PER_HZ, (double)AIRFLOW_MAX_MS);
    return true;
}

bool airflow_sensor_read(bool *has_airflow)
{
    if (!has_airflow) return false;
    *has_airflow = (s_wind_pct > 0);
    return true;
}

int16_t airflow_sensor_read_raw(void)
{
    return (int16_t)s_wind_pct;   /* 兼容旧接口: 返回风速百分比 */
}

int airflow_sensor_read_pct(void)
{
    return s_wind_pct;
}
