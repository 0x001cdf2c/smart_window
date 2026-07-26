/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_hosted.h"

#include "port_esp_hosted_host_log.h"

#include "esp_private/startup_internal.h"

DEFINE_LOG_TAG(host_init);

/* Constructor removed: esp_hosted_init() must be called explicitly from app_main
 * after SR AFE allocates internal DRAM ring buffers, to avoid RINGBUF exhaustion. */
//ESP_SYSTEM_INIT_FN(esp_hosted_host_init, BIT(0), 120)
