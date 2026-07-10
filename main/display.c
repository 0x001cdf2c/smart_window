#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "lvgl.h"

static const char *TAG = "display";

/* ── LVGL cross-core mutex (protects all LVGL API calls from CPU0 vs CPU1 rendering) ── */
static SemaphoreHandle_t g_lvgl_mutex = NULL;

void display_lvgl_lock(void)
{
    if (g_lvgl_mutex) xSemaphoreTakeRecursive(g_lvgl_mutex, portMAX_DELAY);
}

void display_lvgl_unlock(void)
{
    if (g_lvgl_mutex) xSemaphoreGiveRecursive(g_lvgl_mutex);
}

#define DISPLAY_BL      26
#define DISPLAY_RST     27
#define DISPLAY_H_RES   1024
#define DISPLAY_V_RES   600
#define DISPLAY_BPP     24    /* RGB888 */

static esp_lcd_panel_handle_t    g_panel   = NULL;
static esp_lcd_dsi_bus_handle_t  g_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t g_dbi_io  = NULL;
static esp_ldo_channel_handle_t  g_ldo     = NULL;

/* ── LVGL ── */
#define LVGL_BUF_LINES  20
#define FB_WIDTH   1024
#define FB_HEIGHT  600
static lv_display_t *g_lvgl_disp = NULL;
static uint8_t *g_lvgl_buf1 = NULL;
static uint8_t *g_lvgl_buf2 = NULL;
static uint8_t *g_fb = NULL;          /* full framebuffer: 1024x600 RGB888 in PSRAM */
static int g_dirty_x1, g_dirty_y1, g_dirty_x2, g_dirty_y2;
static bool g_dirty = false;

/* ── GT911 Touch ── */
#define TOUCH_I2C_PORT   I2C_NUM_0
#define TOUCH_RST_PIN    2
#define TOUCH_INT_PIN    3
#define TOUCH_ADDR       0x5D

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 0) {
        if (len > 1) {
            i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
        }
        i2c_master_read(cmd, data + len - 1, 1, I2C_MASTER_NACK);
    }
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t gt911_write_reg(uint16_t reg, uint8_t data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int poll_count = 0;
    static int touch_count = 0;
    /* Read 8 bytes: status + reserved + X(2) + Y(2) + size(2) */
    uint8_t buf[8];
    poll_count++;

    if (gt911_read_reg(0x814E, buf, 8) == ESP_OK) {
        uint8_t status = buf[0];
        if ((poll_count % 100) == 0) {
            ESP_LOGI(TAG, "Touch poll #%d: status=0x%02X buf=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                     poll_count, status,
                     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        }
        if (status & 0x80) {
            uint8_t points = status & 0x0F;
            if (points > 0) {
                /* Layout A: X at buf[1:2] (no reserved byte) */
                uint16_t x_a = (uint16_t)buf[1] | ((uint16_t)(buf[2] & 0x0F) << 8);
                uint16_t y_a = (uint16_t)buf[3] | ((uint16_t)(buf[4] & 0x0F) << 8);
                /* Layout B: reserved at buf[1], X at buf[2:3], Y at buf[4:5] */
                uint16_t x_b = (uint16_t)buf[2] | ((uint16_t)(buf[3] & 0x0F) << 8);
                uint16_t y_b = (uint16_t)buf[4] | ((uint16_t)(buf[5] & 0x0F) << 8);

                /* Pick the layout that produces plausible coords (< 1024 and < 600) */
                uint16_t phy_x, phy_y;
                if (x_b < 1024 && y_b < 600) {
                    phy_x = x_b; phy_y = y_b;
                } else {
                    phy_x = x_a; phy_y = y_a;
                }

                /* Map physical (1024×600 landscape) → LVGL (600×1024 portrait)
                   Inverse of flush: phy(px,py) = (lv_y, 599 - lv_x)
                   → lv_x = 599 - py, lv_y = px.
                   Try common GT911 mounting variants (X/Y invert, swap). */
                lv_coord_t lv_x, lv_y;

                /* Variant 3: both GT911 axes inverted relative to physical display */
                lv_x = (lv_coord_t)(phy_y);
                lv_y = (lv_coord_t)(1023 - phy_x);
                touch_count++;
                if ((touch_count % 10) == 0 || touch_count <= 3) {
                    ESP_LOGI(TAG, "Touch #%d: phy(%d,%d) → lvgl(%d,%d) status=0x%02X raw=[%02X %02X %02X %02X %02X]",
                             touch_count, phy_x, phy_y, lv_x, lv_y, status,
                             buf[1], buf[2], buf[3], buf[4], buf[5]);
                }
                if (lv_x >= 0 && lv_x < 600 && lv_y >= 0 && lv_y < 1024) {
                    data->point.x = lv_x;
                    data->point.y = lv_y;
                    data->state = LV_INDEV_STATE_PRESSED;
                } else {
                    if (touch_count <= 3) {
                        ESP_LOGW(TAG, "Touch #%d: coords out of range lv(%d,%d)", touch_count, lv_x, lv_y);
                    }
                }
            }
            /* Clear status register */
            gt911_write_reg(0x814E, 0x00);
        }
    } else if ((poll_count % 100) == 0) {
        ESP_LOGW(TAG, "Touch poll #%d: I2C read failed", poll_count);
    }
}

esp_err_t touch_init(void)
{
    /* Step 1: Hold INT low during reset → GT911 I2C addr = 0x5D */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST_PIN) | (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(TOUCH_INT_PIN, 0);
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Step 2: Switch INT to input (float) — GT911 drives low when ready */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 3: Read product ID to verify */
    uint8_t pid[4] = {0};
    if (gt911_read_reg(0x8140, pid, 4) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 Product ID: %c%c%c%c", pid[0], pid[1], pid[2], pid[3]);
    } else {
        ESP_LOGW(TAG, "GT911 product ID read failed, continuing...");
    }

    /* Step 4: Set resolution config (X=1024, Y=600) — skip full checksum */
    {
        uint8_t cfg_ver = 0;
        if (gt911_read_reg(0x8047, &cfg_ver, 1) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 config version: %d", cfg_ver);
        }
        gt911_write_reg(0x8048, 0x00);  /* X resolution low  → 1024 */
        gt911_write_reg(0x8049, 0x04);  /* X resolution high */
        gt911_write_reg(0x804A, 0x58);  /* Y resolution low  → 600 */
        gt911_write_reg(0x804B, 0x02);  /* Y resolution high */
        gt911_write_reg(0x804C, 0x01);  /* touch number = 1 */
        ESP_LOGI(TAG, "GT911 config: 1024x600 (no checksum update)");
    }

    /* Step 5: LVGL input device */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ESP_LOGI(TAG, "GT911 touch init OK (I2C 0x%02X, RST=%d, INT=%d)",
             TOUCH_ADDR, TOUCH_RST_PIN, TOUCH_INT_PIN);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    /* 1. Backlight PWM on GPIO26 (Timer 1 — Timer 0 is used internally by IDF) */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .gpio_num   = DISPLAY_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 8191,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);

    /* 2. MIPI DSI PHY power (required for ESP32-P4) */
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &g_ldo));

    /* 3. MIPI DSI bus: 2 lanes, 900 Mbps */
    esp_lcd_dsi_bus_config_t bus_cfg = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &g_dsi_bus));

    /* 4. Panel IO over DSI (DBI command mode) */
    esp_lcd_dbi_io_config_t io_cfg = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(g_dsi_bus, &io_cfg, &g_dbi_io));

    /* 5. DPI panel timing: 1024x600 @ ~60Hz */
    esp_lcd_dpi_panel_config_t dpi_cfg =
        EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB888);

    /* 6. EK79007 panel */
    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus    = g_dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num   = 2,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISPLAY_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = DISPLAY_BPP,
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(g_dbi_io, &panel_cfg, &g_panel));

    /* 7. Reset -> init (DPI panel auto-starts after DSI init, no disp_on_off needed) */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));

    ESP_LOGI(TAG, "EK79007 MIPI DSI 1024x600 init OK");
    return ESP_OK;
}

void display_fill(uint16_t color)
{
    if (!g_panel) return;

    /* RGB565 -> RGB888 */
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5)  & 0x3F;
    uint8_t b = color & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    uint8_t row[DISPLAY_H_RES * 3];
    for (int x = 0; x < DISPLAY_H_RES; x++) {
        row[x * 3 + 0] = r;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = b;
    }
    for (int y = 0; y < DISPLAY_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, DISPLAY_H_RES, y + 1, row);
    }
}

void display_test_pattern(void)
{
    if (!g_panel) return;

    /* MIPI DSI built-in color bar pattern -- zero framebuffer, just proof of life */
    ESP_LOGI(TAG, "Hardware color bar pattern...");
    esp_lcd_dpi_panel_set_pattern(g_panel, MIPI_DSI_PATTERN_BAR_VERTICAL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_lcd_dpi_panel_set_pattern(g_panel, MIPI_DSI_PATTERN_NONE);

    /* Draw a simple red-green-blue stripe */
    ESP_LOGI(TAG, "RGB stripes...");
    uint8_t *buf = calloc(1, DISPLAY_H_RES * 3);
    if (!buf) return;

    for (int y = 0; y < DISPLAY_V_RES; y++) {
        for (int x = 0; x < DISPLAY_H_RES; x++) {
            int idx = x * 3;
            if (x < DISPLAY_H_RES / 3) {
                buf[idx + 0] = 0xFF; /* R */
            } else if (x < DISPLAY_H_RES * 2 / 3) {
                buf[idx + 1] = 0xFF; /* G */
            } else {
                buf[idx + 2] = 0xFF; /* B */
            }
        }
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, DISPLAY_H_RES, y + 1, buf);
    }
    free(buf);
    ESP_LOGI(TAG, "Test pattern done");
}

/* ── LVGL flush callback: RGB565 → RGB888 + manual 90° CW rotation ── */
/* LVGL renders at 600×1024 portrait; physical display is 1024×600 landscape.
   Writes rotated pixels into g_fb (full framebuffer). Actual DSI transfer
   happens in lvgl_task after all LVGL rendering is done. */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;

    /* Rotated dirty rect in physical coordinates */
    int phy_x1 = area->y1;
    int phy_x2 = area->y2;
    int phy_y1 = 599 - area->x2;
    int phy_y2 = 599 - area->x1;

    if (!g_dirty) {
        g_dirty_x1 = phy_x1; g_dirty_y1 = phy_y1;
        g_dirty_x2 = phy_x2; g_dirty_y2 = phy_y2;
        g_dirty = true;
    } else {
        if (phy_x1 < g_dirty_x1) g_dirty_x1 = phy_x1;
        if (phy_y1 < g_dirty_y1) g_dirty_y1 = phy_y1;
        if (phy_x2 > g_dirty_x2) g_dirty_x2 = phy_x2;
        if (phy_y2 > g_dirty_y2) g_dirty_y2 = phy_y2;
    }

    /* Copy LVGL RGB565 pixels → framebuffer RGB888 at rotated positions */
    static uint32_t pixel_acc = 0;
    for (int lx = area->x1; lx <= area->x2; lx++) {
        int py = 599 - lx;  /* physical row for this LVGL column */
        uint8_t *fb_row = g_fb + (py * FB_WIDTH + phy_x1) * 3;
        for (int ly = area->y1; ly <= area->y2; ly++) {
            int src_idx = (ly - area->y1) * w + (lx - area->x1);
            uint16_t rgb565 = px_map[src_idx * 2] | ((uint16_t)px_map[src_idx * 2 + 1] << 8);
            uint8_t r5 = (rgb565 >> 11) & 0x1F;
            uint8_t g6 = (rgb565 >> 5)  & 0x3F;
            uint8_t b5 = rgb565 & 0x1F;
            *fb_row++ = (r5 << 3) | (r5 >> 2);  /* R 5→8 bit */
            *fb_row++ = (g6 << 2) | (g6 >> 4);  /* G 6→8 bit */
            *fb_row++ = (b5 << 3) | (b5 >> 2);  /* B 5→8 bit */
        }
    }
    pixel_acc += (uint32_t)w * h;
    if (pixel_acc > 50000) {
        pixel_acc = 0;
        vTaskDelay(1);  /* feed IDLE watchdog during initial full-screen render */
    }
    lv_display_flush_ready(disp);
}

/* ── LVGL 1ms tick ── */
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1);
}

/* ── LVGL timer handler task ── */
static void lvgl_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(100));  /* let system settle */
    while (1) {
        /* Hold LVGL mutex during rendering to prevent CPU0 tasks
           from modifying widgets while we read them for drawing */
        display_lvgl_lock();
        lv_timer_handler();
        /* Snapshot dirty rect and clear, then release mutex before slow DSI transfer */
        bool dirty = g_dirty;
        int dx1 = g_dirty_x1, dy1 = g_dirty_y1, dx2 = g_dirty_x2, dy2 = g_dirty_y2;
        g_dirty = false;
        display_lvgl_unlock();

        /* Flush dirty framebuffer region to display (no LVGL API calls here) */
        if (dirty) {
            int dx = dx2 - dx1 + 1;
            int dy = dy2 - dy1 + 1;
            if (dx1 == 0 && dx == FB_WIDTH) {
                /* Full-width: data is already contiguous in g_fb */
                esp_lcd_panel_draw_bitmap(g_panel,
                    dx1, dy1, dx2 + 1, dy2 + 1,
                    g_fb + dy1 * FB_WIDTH * 3);
            } else {
                /* Pack rows into contiguous buffer for partial x-range */
                size_t row_sz = dx * 3;
                uint8_t *packed = heap_caps_malloc(row_sz * dy, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (packed) {
                    for (int r = 0; r < dy; r++) {
                        memcpy(packed + r * row_sz,
                               g_fb + ((dy1 + r) * FB_WIDTH + dx1) * 3,
                               row_sz);
                    }
                    esp_lcd_panel_draw_bitmap(g_panel,
                        dx1, dy1, dx2 + 1, dy2 + 1,
                        packed);
                    free(packed);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t display_lvgl_init(void)
{
    ESP_LOGI(TAG, "LVGL: lv_init...");
    lv_init();
    ESP_LOGI(TAG, "LVGL: lv_init done");

    /* Cross-core mutex: all LVGL API access must hold this */
    g_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!g_lvgl_mutex) {
        ESP_LOGE(TAG, "LVGL mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    /* Logical: 600×1024 portrait; physical: 1024×600 landscape (manual 90° CW rotation in flush) */
    ESP_LOGI(TAG, "LVGL: display create...");
    g_lvgl_disp = lv_display_create(600, 1024);
    ESP_LOGI(TAG, "LVGL: display create done");

    // lv_display_set_rotation(g_lvgl_disp, LV_DISPLAY_ROTATION_90);  /* disabled: DMA2D crash */
    lv_display_set_flush_cb(g_lvgl_disp, lvgl_flush_cb);

    /* Two partial draw buffers in RGB565 (2 bytes/pixel) */
    uint32_t buf_sz = 600 * LVGL_BUF_LINES * 2;
    ESP_LOGI(TAG, "LVGL: alloc buffers (%lu bytes x2)...", buf_sz);
    g_lvgl_buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_lvgl_buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_lvgl_buf1 || !g_lvgl_buf2) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed (psram)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL: buffers alloc done");
    lv_display_set_buffers(g_lvgl_disp, g_lvgl_buf1, g_lvgl_buf2, buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Full framebuffer: 1024x600 RGB888 in PSRAM (~1.8 MB) */
    g_fb = heap_caps_malloc(FB_WIDTH * FB_HEIGHT * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_fb) {
        ESP_LOGE(TAG, "Framebuffer alloc failed (psram)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL: framebuffer alloc done (%d bytes)", FB_WIDTH * FB_HEIGHT * 3);

    /* 1 ms tick timer */
    ESP_LOGI(TAG, "LVGL: create tick timer...");
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000); /* 1000 us = 1 ms */
    ESP_LOGI(TAG, "LVGL: tick timer started");

    ESP_LOGI(TAG, "LVGL ready (physical 1024x600, logical 600x1024, rotation 90)");
    return ESP_OK;
}

void display_lvgl_task_start(void)
{
    if (!g_lvgl_disp) return;
    ESP_LOGI(TAG, "LVGL: start handler task on CPU1...");
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "LVGL: handler task started");
}
