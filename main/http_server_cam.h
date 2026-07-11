#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_server_cam_start(void);
esp_err_t http_server_cam_stop(void);

#ifdef __cplusplus
}
#endif
