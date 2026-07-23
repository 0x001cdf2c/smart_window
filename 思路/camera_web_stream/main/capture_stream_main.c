/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "wifi_http_server.h"
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "example_video_common.h"
#if CONFIG_EXAMPLE_VIDEO_BUFFER_TYPE_USER
#include "esp_heap_caps.h"
#include "camera_snapshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/jpeg_encode.h"
#include <unistd.h>
#include <stdlib.h>

#define MEMORY_TYPE V4L2_MEMORY_USERPTR
#define MEMORY_ALIGN 64
#else
#define MEMORY_TYPE V4L2_MEMORY_MMAP
#endif

#define BUFFER_COUNT 2

static const char *TAG = "example";

#define SNAPSHOT_WIDTH   1280
#define SNAPSHOT_HEIGHT  720
#define SNAPSHOT_FORMAT  V4L2_PIX_FMT_RGB565
#define FRAME_SIZE       (SNAPSHOT_WIDTH * SNAPSHOT_HEIGHT * 2)
#define JPG_COPY_SIZE    (512 * 1024)
#define JPG_WORK_SIZE    (2 * 1024 * 1024)

static SemaphoreHandle_t s_frame_mutex = NULL;
static uint8_t *s_cached_frame = NULL;

static SemaphoreHandle_t s_jpg_mutex = NULL;
static uint8_t *s_cached_jpg = NULL;
static size_t s_cached_jpg_len = 0;

static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static uint8_t *s_jpg_buf = NULL;
static size_t s_jpg_buf_size = 0;

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static esp_err_t rgb565_to_bmp(
    const uint8_t *rgb565,
    uint32_t width,
    uint32_t height,
    uint8_t **out_buf,
    size_t *out_len
)
{
    const size_t bmp_header_size = 54;
    const size_t row_bytes = width * 3;
    const size_t row_stride = (row_bytes + 3) & ~3;
    const size_t pixel_data_size = row_stride * height;
    const size_t bmp_size = bmp_header_size + pixel_data_size;

    uint8_t *bmp = heap_caps_malloc(bmp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bmp == NULL) {
        bmp = malloc(bmp_size);
    }

    if (bmp == NULL) {
        ESP_LOGE(TAG, "failed to allocate bmp buffer, size=%u", (unsigned)bmp_size);
        return ESP_ERR_NO_MEM;
    }

    memset(bmp, 0, bmp_size);

    bmp[0] = 'B';
    bmp[1] = 'M';

    put_le32(&bmp[2], bmp_size);
    put_le32(&bmp[10], bmp_header_size);

    put_le32(&bmp[14], 40);
    put_le32(&bmp[18], width);
    put_le32(&bmp[22], (uint32_t)(-(int32_t)height));

    put_le16(&bmp[26], 1);
    put_le16(&bmp[28], 24);
    put_le32(&bmp[30], 0);
    put_le32(&bmp[34], pixel_data_size);

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = bmp + bmp_header_size + y * row_stride;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t src_index = (y * width + x) * 2;

            uint16_t p = rgb565[src_index] | ((uint16_t)rgb565[src_index + 1] << 8);

            uint8_t r5 = (p >> 11) & 0x1f;
            uint8_t g6 = (p >> 5) & 0x3f;
            uint8_t b5 = p & 0x1f;

            uint8_t r8 = (r5 << 3) | (r5 >> 2);
            uint8_t g8 = (g6 << 2) | (g6 >> 4);
            uint8_t b8 = (b5 << 3) | (b5 >> 2);

            dst[x * 3 + 0] = b8;
            dst[x * 3 + 1] = g8;
            dst[x * 3 + 2] = r8;
        }
    }

    *out_buf = bmp;
    *out_len = bmp_size;

    return ESP_OK;
}

static void camera_capture_task(void *arg)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(EXAMPLE_CAM_DEV_PATH, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "camera task: failed to open device");
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_format format = {
        .type = type,
        .fmt.pix.width = SNAPSHOT_WIDTH,
        .fmt.pix.height = SNAPSHOT_HEIGHT,
        .fmt.pix.pixelformat = SNAPSHOT_FORMAT,
    };

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "camera task: failed to set format");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = type;
    req.memory = MEMORY_TYPE;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "camera task: failed to request buffers");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buffer[BUFFER_COUNT] = {0};
#if CONFIG_EXAMPLE_VIDEO_BUFFER_TYPE_USER
    uint32_t buffer_size[BUFFER_COUNT] = {0};
#endif

    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = MEMORY_TYPE;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera task: failed to query buffer");
            goto cleanup;
        }

#if CONFIG_EXAMPLE_VIDEO_BUFFER_TYPE_USER
        buffer[i] = heap_caps_aligned_alloc(MEMORY_ALIGN, buf.length,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (buffer[i] == NULL) {
            ESP_LOGE(TAG, "camera task: failed to allocate user buffer");
            goto cleanup;
        }
        buf.m.userptr = (unsigned long)buffer[i];
        buffer_size[i] = buf.length;
#else
        buffer[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, buf.m.offset);
        if (buffer[i] == NULL || buffer[i] == MAP_FAILED) {
            buffer[i] = NULL;
            ESP_LOGE(TAG, "camera task: failed to mmap buffer");
            goto cleanup;
        }
#endif

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera task: failed to queue buffer");
            goto cleanup;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "camera task: failed to start stream");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Camera background task started, streaming...");

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = MEMORY_TYPE;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera task: dequeue failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (buf.flags & V4L2_BUF_FLAG_DONE) {
            if (s_cached_frame && xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                memcpy(s_cached_frame, buffer[buf.index], FRAME_SIZE);
                xSemaphoreGive(s_frame_mutex);
            }

            if (s_jpeg_enc && s_jpg_buf) {
                jpeg_encode_cfg_t enc_cfg = {
                    .width = 1280,
                    .height = 720,
                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                    .image_quality = 60,
                };
                uint32_t out_size = 0;
                esp_err_t jpeg_ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg,
                                           buffer[buf.index], FRAME_SIZE,
                                           s_jpg_buf, s_jpg_buf_size,
                                           &out_size);
                if (jpeg_ret == ESP_OK && out_size > 0) {
                    static int first_ok = 1;
                    if (first_ok) {
                        ESP_LOGI(TAG, "JPEG encode OK, first frame size=%lu", (unsigned long)out_size);
                        first_ok = 0;
                    }
                    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        if (s_cached_jpg) {
                            memcpy(s_cached_jpg, s_jpg_buf, out_size);
                            s_cached_jpg_len = out_size;
                        }
                        xSemaphoreGive(s_jpg_mutex);
                    }
                } else {
                    static int first_fail = 1;
                    if (first_fail) {
                        ESP_LOGW(TAG, "JPEG encode FAILED: ret=%d, size=%lu", jpeg_ret, (unsigned long)out_size);
                        first_fail = 0;
                    }
                }
            }
        }

#if CONFIG_EXAMPLE_VIDEO_BUFFER_TYPE_USER
        buf.m.userptr = (unsigned long)buffer[buf.index];
        buf.length = buffer_size[buf.index];
#endif

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "camera task: requeue failed");
        }
    }

cleanup:
#if CONFIG_EXAMPLE_VIDEO_BUFFER_TYPE_USER
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffer[i]) {
            heap_caps_free(buffer[i]);
        }
    }
#else
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffer[i]) {
            munmap(buffer[i], buffer_len[i]);
        }
    }
#endif
    close(fd);

    if (s_frame_mutex) {
        xSemaphoreTake(s_frame_mutex, portMAX_DELAY);
        if (s_cached_frame) {
            heap_caps_free(s_cached_frame);
            s_cached_frame = NULL;
        }
        xSemaphoreGive(s_frame_mutex);
    }

    if (s_jpg_mutex) {
        xSemaphoreTake(s_jpg_mutex, portMAX_DELAY);
        if (s_cached_jpg) {
            heap_caps_free(s_cached_jpg);
            s_cached_jpg = NULL;
        }
        xSemaphoreGive(s_jpg_mutex);
    }

    vTaskDelete(NULL);
}

esp_err_t camera_capture_start(void)
{
    s_frame_mutex = xSemaphoreCreateMutex();
    if (s_frame_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_cached_frame = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_cached_frame == NULL) {
        s_cached_frame = malloc(FRAME_SIZE);
    }
    if (s_cached_frame == NULL) {
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_jpg_mutex = xSemaphoreCreateMutex();
    if (s_jpg_mutex == NULL) {
        heap_caps_free(s_cached_frame);
        s_cached_frame = NULL;
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cached_jpg = heap_caps_malloc(JPG_COPY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_cached_jpg == NULL) {
        s_cached_jpg = malloc(JPG_COPY_SIZE);
    }
    if (s_cached_jpg == NULL) {
        vSemaphoreDelete(s_jpg_mutex);
        s_jpg_mutex = NULL;
        heap_caps_free(s_cached_frame);
        s_cached_frame = NULL;
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_engine_cfg_t eng_cfg = { .intr_priority = 0, .timeout_ms = 200 };
    if (jpeg_new_encoder_engine(&eng_cfg, &s_jpeg_enc) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder");
        heap_caps_free(s_cached_jpg);
        s_cached_jpg = NULL;
        vSemaphoreDelete(s_jpg_mutex);
        s_jpg_mutex = NULL;
        heap_caps_free(s_cached_frame);
        s_cached_frame = NULL;
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_jpg_buf = (uint8_t *)jpeg_alloc_encoder_mem(JPG_WORK_SIZE, &mem_cfg, &s_jpg_buf_size);
    if (s_jpg_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate JPEG work buffer");
        jpeg_del_encoder_engine(s_jpeg_enc);
        s_jpeg_enc = NULL;
        heap_caps_free(s_cached_jpg);
        s_cached_jpg = NULL;
        vSemaphoreDelete(s_jpg_mutex);
        s_jpg_mutex = NULL;
        heap_caps_free(s_cached_frame);
        s_cached_frame = NULL;
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        camera_capture_task, "cam_task", 8192, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        free(s_jpg_buf);
        s_jpg_buf = NULL;
        jpeg_del_encoder_engine(s_jpeg_enc);
        s_jpeg_enc = NULL;
        heap_caps_free(s_cached_jpg);
        s_cached_jpg = NULL;
        vSemaphoreDelete(s_jpg_mutex);
        s_jpg_mutex = NULL;
        heap_caps_free(s_cached_frame);
        s_cached_frame = NULL;
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t camera_get_frame_rgb565(const uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL || s_frame_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_cached_frame == NULL) {
        xSemaphoreGive(s_frame_mutex);
        return ESP_FAIL;
    }

    *out_buf = s_cached_frame;
    *out_len = FRAME_SIZE;
    return ESP_OK;
}

void camera_release_frame(void)
{
    if (s_frame_mutex) {
        xSemaphoreGive(s_frame_mutex);
    }
}

esp_err_t camera_get_jpg_copy(uint8_t *out_buf, size_t buf_size, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL || s_jpg_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "camera_get_jpg_copy: mutex timeout");
        return ESP_FAIL;
    }

    if (s_cached_jpg == NULL || s_cached_jpg_len == 0) {
        xSemaphoreGive(s_jpg_mutex);
        static int no_data_log = 1;
        if (no_data_log) {
            ESP_LOGW(TAG, "camera_get_jpg_copy: no cached JPEG data (len=%u, ptr=%p)",
                     (unsigned)s_cached_jpg_len, (void *)s_cached_jpg);
            no_data_log = 0;
        }
        return ESP_FAIL;
    }

    if (s_cached_jpg_len > buf_size) {
        xSemaphoreGive(s_jpg_mutex);
        ESP_LOGE(TAG, "JPEG too large: %u > %u", (unsigned)s_cached_jpg_len, (unsigned)buf_size);
        return ESP_FAIL;
    }

    memcpy(out_buf, s_cached_jpg, s_cached_jpg_len);
    *out_len = s_cached_jpg_len;
    xSemaphoreGive(s_jpg_mutex);
    return ESP_OK;
}

esp_err_t camera_capture_bmp(uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_frame_mutex == NULL) {
        return ESP_FAIL;
    }

    *out_buf = NULL;
    *out_len = 0;

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_FAIL;
    }

    if (s_cached_frame == NULL) {
        xSemaphoreGive(s_frame_mutex);
        return ESP_FAIL;
    }

    uint8_t *frame_copy = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame_copy == NULL) {
        xSemaphoreGive(s_frame_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(frame_copy, s_cached_frame, FRAME_SIZE);
    xSemaphoreGive(s_frame_mutex);

    esp_err_t ret = rgb565_to_bmp(frame_copy, SNAPSHOT_WIDTH, SNAPSHOT_HEIGHT, out_buf, out_len);
    heap_caps_free(frame_copy);

    return ret;
}

void app_main(void)
{
    esp_err_t ret = ESP_OK;

    ret = example_video_init();
    ESP_GOTO_ON_ERROR(ret, clean1, TAG, "Camera init failed");

    ret = camera_capture_start();
    ESP_GOTO_ON_ERROR(ret, clean1, TAG, "Camera capture start failed");

    wifi_http_server_start();

    ESP_LOGI(TAG, "Camera web snapshot service is ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

clean1:
    return;
}
