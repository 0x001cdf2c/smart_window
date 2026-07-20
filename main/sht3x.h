#pragma once

#include <stdbool.h>

typedef struct {
    float temperature;
    float humidity;
} sht3x_data_t;

typedef enum {
    SHT3X_INDOOR  = 0,
    SHT3X_OUTDOOR = 1,
} sht3x_slot_t;

bool sht3x_init(void);
bool sht3x_read(sht3x_slot_t slot, sht3x_data_t *out);
