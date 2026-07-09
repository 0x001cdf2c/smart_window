#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t timestamp_ms;

    struct {
        int raw;
        float brightness_percent;
        bool valid;
    } light;

    struct {
        int raw;
        float wetness_percent;
        bool detected;
        bool valid;
    } rain;

    struct {
        int raw;
        float heat_percent;
        bool overheated;
        bool valid;
    } thermal;

} sensor_data_t;
