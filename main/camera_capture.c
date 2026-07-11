#include "camera_capture.h"

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "CAM";

#define MEMORY_TYPE     V4L2_MEMORY_USERPTR
#define MEMORY_ALIGN    64
#define BUFFER_COUNT    2

#define FRAME_WIDTH     1280
#define FRAME_HEIGHT    720
#define FRAME_FORMAT    V4L2_PIX_FMT_RGB565
#define FRAME_SIZE      (FRAME_WIDTH * FRAME_HEIGHT * 2)
#define JPG_COPY_SIZE   (512 * 1024)
#define JPG_WORK_SIZE   (2 * 1024 * 1024)

static SemaphoreHandle_t s_frame_mutex = NULL;
static uint8_t *s_cached_frame = NULL;

static SemaphoreHandle_t s_jpg_mutex = NULL;
static uint8_t *s_cached_jpg = NULL;
static size_t s_cached_jpg_len = 0;

static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static uint8_t *s_jpg_buf = NULL;
static size_t s_jpg_buf_size = 0;
static bool s_capture_running = false;

static void camera_capture_task(void *arg)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        vTaskDelete(NULL);
        return;
    }

    struct v4l2_format format = {
        .type = type,
        .fmt.pix.width = FRAME_WIDTH,
        .fmt.pix.height = FRAME_HEIGHT,
        .fmt.pix.pixelformat = FRAME_FORMAT,
    };

    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed to set format");
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
        ESP_LOGE(TAG, "Failed to request buffers");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buffer[BUFFER_COUNT] = {0};
    uint32_t buffer_size[BUFFER_COUNT] = {0};

    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = MEMORY_TYPE;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Failed to query buffer");
            goto cleanup;
        }

        buffer[i] = heap_caps_aligned_alloc(MEMORY_ALIGN, buf.length,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (buffer[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate user buffer");
            goto cleanup;
        }
        buf.m.userptr = (unsigned long)buffer[i];
        buffer_size[i] = buf.length;

        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Failed to queue buffer");
            goto cleanup;
        }
    }

    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "Failed to start stream");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Capture task started, %dx%d RGB565 @ 30fps", FRAME_WIDTH, FRAME_HEIGHT);

    int frame_count = 0;
    int jpg_ok_count = 0;
    while (s_capture_running) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = MEMORY_TYPE;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (buf.flags & V4L2_BUF_FLAG_DONE) {
            frame_count++;

            /* JPEG encode */
            if (s_jpeg_enc && s_jpg_buf && s_cached_jpg) {
                jpeg_encode_cfg_t enc_cfg = {
                    .width = FRAME_WIDTH,
                    .height = FRAME_HEIGHT,
                    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                    .image_quality = 40,
                };
                uint32_t out_size = 0;
                esp_err_t jpg_ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg,
                                         buffer[buf.index], FRAME_SIZE,
                                         s_jpg_buf, s_jpg_buf_size, &out_size);
                if (jpg_ret == ESP_OK && out_size > 0) {
                    jpg_ok_count++;
                    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        memcpy(s_cached_jpg, s_jpg_buf, out_size);
                        s_cached_jpg_len = out_size;
                        xSemaphoreGive(s_jpg_mutex);
                    }
                } else if (frame_count <= 3) {
                    ESP_LOGW(TAG, "JPEG encode failed: ret=%s out_size=%lu",
                             esp_err_to_name(jpg_ret), (unsigned long)out_size);
                }
            }

            if (frame_count <= 3 || frame_count % 100 == 0) {
                ESP_LOGI(TAG, "Frame #%d captured, JPEG ok=%d/%d, last_jpg=%u bytes",
                         frame_count, jpg_ok_count, frame_count,
                         s_cached_jpg ? (unsigned)s_cached_jpg_len : 0);
            }
        }

        buf.m.userptr = (unsigned long)buffer[buf.index];
        buf.length = buffer_size[buf.index];
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

cleanup:
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffer[i]) heap_caps_free(buffer[i]);
    }
    close(fd);
    vTaskDelete(NULL);
}

esp_err_t camera_capture_init(void)
{
    esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb  = false,
            .i2c_handle = i2c_bus_get_handle(),
            .freq       = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin  = -1,
    };

    esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    esp_err_t ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Video init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_frame_mutex = xSemaphoreCreateMutex();
    s_jpg_mutex = xSemaphoreCreateMutex();

    s_cached_frame = heap_caps_malloc(FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cached_frame) s_cached_frame = malloc(FRAME_SIZE);

    s_cached_jpg = heap_caps_malloc(JPG_COPY_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cached_jpg) s_cached_jpg = malloc(JPG_COPY_SIZE);

    if (!s_cached_frame || !s_cached_jpg) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_engine_cfg_t eng_cfg = { .intr_priority = 0, .timeout_ms = 200 };
    if (jpeg_new_encoder_engine(&eng_cfg, &s_jpeg_enc) != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encoder init failed");
        return ESP_FAIL;
    }

    jpeg_encode_memory_alloc_cfg_t mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_jpg_buf = (uint8_t *)jpeg_alloc_encoder_mem(JPG_WORK_SIZE, &mem_cfg, &s_jpg_buf_size);
    if (!s_jpg_buf) {
        ESP_LOGE(TAG, "JPEG work buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_capture_running = true;
    if (xTaskCreatePinnedToCore(camera_capture_task, "cam_task", 8192,
                                 NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera ready");
    return ESP_OK;
}

esp_err_t camera_capture_deinit(void)
{
    s_capture_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_jpeg_enc) { jpeg_del_encoder_engine(s_jpeg_enc); s_jpeg_enc = NULL; }
    if (s_jpg_buf)   { free(s_jpg_buf); s_jpg_buf = NULL; }
    if (s_cached_jpg) { free(s_cached_jpg); s_cached_jpg = NULL; }
    if (s_cached_frame) { free(s_cached_frame); s_cached_frame = NULL; }
    if (s_jpg_mutex)  { vSemaphoreDelete(s_jpg_mutex); s_jpg_mutex = NULL; }
    if (s_frame_mutex){ vSemaphoreDelete(s_frame_mutex); s_frame_mutex = NULL; }

    esp_video_deinit();
    return ESP_OK;
}

esp_err_t camera_get_jpg_copy(uint8_t *out_buf, size_t buf_size, size_t *out_len)
{
    if (!out_buf || !out_len || !s_jpg_mutex) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_FAIL;
    if (!s_cached_jpg || s_cached_jpg_len == 0) {
        xSemaphoreGive(s_jpg_mutex);
        return ESP_FAIL;
    }
    if (s_cached_jpg_len > buf_size) {
        xSemaphoreGive(s_jpg_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(out_buf, s_cached_jpg, s_cached_jpg_len);
    *out_len = s_cached_jpg_len;
    xSemaphoreGive(s_jpg_mutex);
    return ESP_OK;
}

esp_err_t camera_get_frame_rgb565(const uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len || !s_frame_mutex) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_FAIL;
    if (!s_cached_frame) { xSemaphoreGive(s_frame_mutex); return ESP_FAIL; }
    *out_buf = s_cached_frame;
    *out_len = FRAME_SIZE;
    return ESP_OK;
}

void camera_release_frame(void)
{
    if (s_frame_mutex) xSemaphoreGive(s_frame_mutex);
}
