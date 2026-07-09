#pragma once

#include <stdbool.h>

bool bh1750_init(void);
bool bh1750_read(float *lux);
