#pragma once

#include <stdbool.h>

/* Initialize wind scanner servo (GPIO53) + internal task */
bool wind_scanner_init(void);

/* Trigger a sweep: 90→180→0→90, sample wind at each degree */
bool wind_scanner_start(void);

/* True while a scan is in progress */
bool wind_scanner_is_scanning(void);

/* Current angle the servo is at during scan (-1 if not scanning) */
int  wind_scanner_get_current_angle(void);

/* Last scan's best angle, or -1 if no scan completed */
int  wind_scanner_get_best(void);

/* Wind speed 0-100 at the best angle */
int  wind_scanner_get_best_speed(void);

/* Non-blocking: returns true if new result available, writes to *best_angle */
bool wind_scanner_try_apply(int *best_angle);
