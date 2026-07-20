#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int sda;
    int scl;
} soft_i2c_t;

void soft_i2c_init(soft_i2c_t *bus, int sda, int scl);
bool soft_i2c_write(uint8_t addr, const uint8_t *data, int len);
bool soft_i2c_read(uint8_t addr, uint8_t *buf, int len);
