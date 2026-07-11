#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t camera_capture_init(void);
esp_err_t camera_capture_deinit(void);
esp_err_t camera_get_jpg_copy(uint8_t *out_buf, size_t buf_size, size_t *out_len);
esp_err_t camera_get_frame_rgb565(const uint8_t **out_buf, size_t *out_len);
void camera_release_frame(void);

#ifdef __cplusplus
}
#endif
