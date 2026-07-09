#pragma once

#include <stdbool.h>

typedef struct {
    float temperature;
    float humidity;
} sht3x_data_t;

bool sht3x_init(void);
bool sht3x_read(sht3x_data_t *out);
