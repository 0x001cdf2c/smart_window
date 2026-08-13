#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 微型马达+风扇 → ADS1115 AIN2 电压检测 */

bool airflow_sensor_init(void);
bool airflow_sensor_read(bool *has_airflow);
int16_t airflow_sensor_read_raw(void);    /* raw ADC value */
int airflow_sensor_read_pct(void);        /* 0-100% wind speed */
