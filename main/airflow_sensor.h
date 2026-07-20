#pragma once

#include <stdbool.h>

bool airflow_sensor_init(void);
bool airflow_sensor_read(bool *has_airflow);
