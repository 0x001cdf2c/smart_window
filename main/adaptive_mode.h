/*
 * adaptive_mode.h — 用户自适应模式 (气候先验矩阵 + 优先级瀑布)
 *
 * 决策层次: 雨 > 保温 > 用户习惯 > 气候矩阵后验 (argmax 组合)
 * 气候类型由本地传感器判定 (温度/湿度/风速), 不依赖云端。
 *
 * 贯穿规则: 检测到雨水 (rain > 阈值) → 雨棚必展开, 直到无雨。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "control_algorithm.h"   /* schedule_plan_t */

/* 气候类型 (本地判定, 7 选 1) */
typedef enum {
    CLIMATE_HOT_HUMID = 0,           /* 炎热湿润 */
    CLIMATE_HOT_DRY,                 /* 炎热干燥 */
    CLIMATE_HOT_SUMMER_COLD_WINTER,  /* 夏热冬冷 (兜底默认) */
    CLIMATE_MILD_HUMID,              /* 温和湿润 */
    CLIMATE_COLD_DRY,                /* 寒冷干燥 */
    CLIMATE_COLD_HUMID,              /* 寒冷湿润 */
    CLIMATE_COASTAL_WINDY,           /* 沿海强风 */
    CLIMATE_COUNT,
} climate_type_t;

/* 自适应策略 */
typedef enum {
    STRATEGY_RAIN = 0,           /* 防雨 */
    STRATEGY_INSULATE,           /* 保温 */
    STRATEGY_USER_HABIT,         /* 用户习惯 */
    STRATEGY_VENT_LIGHT,         /* 通风采光 */
    STRATEGY_VENT_SHADE,         /* 通风遮光 */
    STRATEGY_WINDPROOF_LIGHT,    /* 防风采光 */
    STRATEGY_WINDPROOF_SHADE,    /* 防风遮光 */
    STRATEGY_DEFAULT_OPEN,       /* 学习阶段默认全开 (无训练数据) */
} strategy_t;

/* 雨棚动作 */
typedef enum {
    SHELTER_HOLD = 0,            /* 不动 */
    SHELTER_EXPAND,              /* 展开 (遮光/防雨) */
    SHELTER_COLLAPSE,            /* 收起 (采光) */
} shelter_action_t;

/* 传感器输入 */
typedef struct {
    float rain_pct;              /* 雨水 0-100 */
    float temp_in;               /* 室内温度 ℃ */
    float temp_out;              /* 室外温度 ℃ */
    float humi_out;              /* 室外湿度 % */
    float wind_pct;              /* 风速 0-100 */
    float light_lux;             /* 光照 lux */
} adaptive_input_t;

/* 决策输出 */
typedef struct {
    strategy_t       strategy;
    float            blinds_angle;     /* 0=全开 .. 90=全关 */
    shelter_action_t shelter_action;   /* 雨棚动作 */
    climate_type_t   climate;
    float            matrix[4];        /* {通风,防风,采光,遮光} 修正后, 供展示 */
    const char      *strategy_name;
    const char      *climate_name;
} adaptive_decision_t;

void adaptive_mode_init(void);

/* 完整决策: 输入传感器 + 自适应预测计划 + 当前时间, 输出执行器决策 */
adaptive_decision_t adaptive_mode_evaluate(const adaptive_input_t *in,
                                           const schedule_plan_t *plan,
                                           time_t now);

/* 贯穿规则: 有雨 → 雨棚必展开 (用于快速循环, 所有模式) */
bool adaptive_mode_rain_should_expand(float rain_pct);
