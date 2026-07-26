/*
 * adaptive_sim.cpp — 自适应算法模拟器
 *
 * 编译: g++ -std=c++17 -o adaptive_sim adaptive_sim.cpp
 * 运行: ./adaptive_sim
 *       或交互输入: echo "08:30 open\n09:00 close\n..." | ./adaptive_sim
 *
 * 功能:
 *   1. 交互输入多个 "HH:MM open/close" 记录, 模拟用户操作历史
 *   2. 每条记录自动生成 6 类传感器随机数据 (温/湿/光/烟/雨/风)
 *   3. 两种预测模式:
 *      a) 纯时间桶统计 (当前固件算法)
 *      b) 时间+传感器加权 (增强算法)
 *   4. 输出 24 小时直方图 + Top-2 预测计划
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

#define PATTERN_MAX   300
#define SCHEDULE_MAX  4
#define RECENT_MAX    10

/* ── 数据结构 (与固件 control_algorithm.h 对齐) ── */

struct pattern_entry_t {
    int hour, min, day_of_week;
    int action;           /* 1=open, 0=close */
    /* 模拟传感器值 (记录时的快照) */
    float temp_in, humi_in;
    float temp_out, humi_out;
    float light;
    int   smoke, rain;
    bool  airflow;
};

struct schedule_entry_t {
    char time_str[8];
    char action[8];
    int  confidence;       /* 0-100% */
};

struct schedule_plan_t {
    int count;
    schedule_entry_t entries[SCHEDULE_MAX];
};

struct recent_op_t {
    char time_str[8];
    char action[8];
    int  day_offset;
};

/* ── 全局 ── */

static std::vector<pattern_entry_t> g_patterns;
static schedule_plan_t g_schedule;
static recent_op_t g_recent[RECENT_MAX];
static int g_recent_count = 0;

/* ── 传感器模拟器 ── */

struct sensor_sim_t {
    /* 基础范围 */
    float temp_in_base  = 24.0f;   /* 室内基准温度, 受室外+光照影响 */
    float temp_out_base = 22.0f;   /* 室外基准温度, 有日夜变化 */
    float humi_in_base  = 55.0f;   /* 室内基准湿度 */
    float humi_out_base = 60.0f;   /* 室外基准湿度 */
    float light_base    = 5000.0f; /* 基准光照 */
    int   smoke_base    = 2;       /* 烟雾基准 (正常极低) */
    int   rain_base     = 0;       /* 雨水基准 (晴天) */
    bool  airflow_base  = true;    /* 气流基准 (微风) */

    void generate(int hour, pattern_entry_t *e) {
        /*
         * 模拟日夜节律: 6-18 白天, 光照+温度高; 19-5 夜间, 低
         * 模拟天气变化: 加随机扰动
         */

        /* ── 室外温度: 日夜正弦波 ── */
        float day_phase = sinf((hour - 6) * 3.14159f / 12.0f);
        temp_out_base = 18.0f + 12.0f * (day_phase > 0 ? day_phase : 0)
                        + frand(-3.0f, 3.0f);
        if (temp_out_base < 5.0f)  temp_out_base = 5.0f;
        if (temp_out_base > 40.0f) temp_out_base = 40.0f;

        /* ── 室内温度: 跟随室外但有滞后和衰减 ── */
        temp_in_base = 20.0f + (temp_out_base - 15.0f) * 0.6f
                       + frand(-1.5f, 1.5f);
        if (temp_in_base < 10.0f) temp_in_base = 10.0f;
        if (temp_in_base > 35.0f) temp_in_base = 35.0f;

        /* ── 室外湿度: 与温度负相关 ── */
        humi_out_base = 50.0f - (temp_out_base - 20.0f) * 2.0f
                        + frand(-10.0f, 10.0f);
        if (humi_out_base < 25.0f) humi_out_base = 25.0f;
        if (humi_out_base > 95.0f) humi_out_base = 95.0f;

        /* ── 室内湿度: 跟随室外但有滞后 ── */
        humi_in_base = humi_out_base + frand(-8.0f, 8.0f);
        if (humi_in_base < 30.0f) humi_in_base = 30.0f;
        if (humi_in_base > 90.0f) humi_in_base = 90.0f;

        /* ── 光照: 6-18 白天递增 ── */
        if (hour >= 6 && hour <= 18) {
            float noon_phase = sinf((hour - 6) * 3.14159f / 12.0f);
            light_base = 500.0f + 45000.0f * noon_phase + frand(-5000.0f, 5000.0f);
        } else {
            light_base = frand(0.0f, 50.0f);  /* 夜间几乎无光 */
        }
        if (light_base < 0.0f)   light_base = 0.0f;
        if (light_base > 65535.0f) light_base = 65535.0f;

        /* ── 烟雾: 90% 正常(0-5), 8% 轻度(5-20), 2% 触发报警(>15) ── */
        float r = frand(0.0f, 100.0f);
        if (r < 90.0f)       smoke_base = (int)frand(0, 5);
        else if (r < 98.0f)  smoke_base = (int)frand(5, 20);
        else                 smoke_base = (int)frand(15, 40);

        /* ── 雨水: 85% 晴天, 10% 小雨, 5% 触发关窗 ── */
        r = frand(0.0f, 100.0f);
        if (r < 85.0f)       rain_base = 0;
        else if (r < 95.0f)  rain_base = (int)frand(1, 40);
        else                 rain_base = (int)frand(50, 100);

        /* ── 气流: 70% 微风 ── */
        airflow_base = (frand(0.0f, 100.0f) < 70.0f);

        /* ── 写入记录 ── */
        e->temp_in  = temp_in_base;
        e->humi_in  = humi_in_base;
        e->temp_out = temp_out_base;
        e->humi_out = humi_out_base;
        e->light    = light_base;
        e->smoke    = smoke_base;
        e->rain     = rain_base;
        e->airflow  = airflow_base;
    }

private:
    float frand(float lo, float hi) {
        return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
    }
};

/* ── 记录操作 ── */

static void record_action(int hour, int min, const char *act)
{
    pattern_entry_t e = {};
    e.hour        = hour;
    e.min         = min;
    e.day_of_week = rand() % 7;
    e.action      = (strcmp(act, "open") == 0) ? 1 : 0;

    /* 生成传感器数据 */
    sensor_sim_t sim;
    sim.generate(hour, &e);

    if ((int)g_patterns.size() >= PATTERN_MAX)
        g_patterns.erase(g_patterns.begin());
    g_patterns.push_back(e);

    /* 更新最近操作 */
    int keep = g_recent_count;
    if (keep >= RECENT_MAX) keep = RECENT_MAX - 1;
    memmove(&g_recent[1], &g_recent[0], keep * sizeof(recent_op_t));
    if (g_recent_count < RECENT_MAX) g_recent_count++;

    snprintf(g_recent[0].time_str, 8, "%02d:%02d", hour, min);
    snprintf(g_recent[0].action, 8, "%s", act);
    g_recent[0].day_offset = 0;
}

/* ── 模式 A: 纯时间桶预测 (固件当前算法) ── */

static void predict_time_only(void)
{
    memset(&g_schedule, 0, sizeof(g_schedule));

    if ((int)g_patterns.size() < 5) {
        printf("  [时间桶] 样本不足 (%d < 5), 无法预测\n\n", (int)g_patterns.size());
        return;
    }

    int open_buckets[24] = {0};
    int close_buckets[24] = {0};
    int count_buckets[24] = {0};

    for (auto &e : g_patterns) {
        int h = e.hour;
        if (e.action == 1) open_buckets[h]++;
        else close_buckets[h]++;
        count_buckets[h]++;
    }

    /* ── ASCII 直方图 ── */
    int max_cnt = 0;
    for (int h = 0; h < 24; h++) {
        if (count_buckets[h] > max_cnt) max_cnt = count_buckets[h];
    }

    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf(  "  ║      24 小时操作频次分布 (纯时间桶)          ║\n");
    printf(  "  ╠══════════════════════════════════════════════╣\n");

    for (int group = 0; group < 4; group++) {
        int g_start = group * 6;
        int g_end   = g_start + 5;
        printf("  ║ %2d-%2d: ", g_start, g_end);

        /* 合并 6 小时的总数做柱状图 */
        int group_total = 0, group_open = 0, group_close = 0;
        for (int h = g_start; h <= g_end; h++) {
            group_total += count_buckets[h];
            group_open  += open_buckets[h];
            group_close += close_buckets[h];
        }

        int bar_len = max_cnt > 0 ? (group_total * 20 / max_cnt) : 0;
        if (bar_len > 20) bar_len = 20;

        /* 柱状图: █=open ░=close ·=无数据 */
        if (group_total > 0) {
            int open_bars  = group_open  * bar_len / group_total;
            int close_bars = bar_len - open_bars;
            for (int i = 0; i < open_bars; i++)  putchar('O');
            for (int i = 0; i < close_bars; i++) putchar('C');
        } else {
            for (int i = 0; i < 20; i++) putchar('.');
        }

        printf(" (开=%d 关=%d 总=%d)\n", group_open, group_close, group_total);
    }
    printf("  ╚══════════════════════════════════════════════╝\n");

    /* ── 选出 Top-2 ── */
    int plan_idx = 0;

    for (int pick = 0; pick < 2 && plan_idx < SCHEDULE_MAX; pick++) {
        int best_h = -1, best_cnt = 0;
        for (int h = 0; h < 24; h++) {
            if (open_buckets[h] > best_cnt) { best_cnt = open_buckets[h]; best_h = h; }
        }
        if (best_h >= 0 && best_cnt > 0 && count_buckets[best_h] > 0) {
            auto &s = g_schedule.entries[plan_idx];
            s.confidence = open_buckets[best_h] * 100 / count_buckets[best_h];
            snprintf(s.time_str, 8, "%02d:%02d", best_h % 24, 30);
            snprintf(s.action, 8, "open");
            plan_idx++;
            open_buckets[best_h] = 0;
        }
    }
    for (int pick = 0; pick < 2 && plan_idx < SCHEDULE_MAX; pick++) {
        int best_h = -1, best_cnt = 0;
        for (int h = 0; h < 24; h++) {
            if (close_buckets[h] > best_cnt) { best_cnt = close_buckets[h]; best_h = h; }
        }
        if (best_h >= 0 && best_cnt > 0 && count_buckets[best_h] > 0) {
            auto &s = g_schedule.entries[plan_idx];
            s.confidence = close_buckets[best_h] * 100 / count_buckets[best_h];
            snprintf(s.time_str, 8, "%02d:%02d", best_h % 24, 30);
            snprintf(s.action, 8, "close");
            plan_idx++;
            close_buckets[best_h] = 0;
        }
    }
    g_schedule.count = plan_idx;
}

/* ── 模式 B: 时间+传感器加权预测 ── */

static float weighted_score(const pattern_entry_t &e, int target_hour,
                             float temp, float humi, float light,
                             float w_temp, float w_humi, float w_light)
{
    /*
     * 综合得分 = 时间匹配度 × (1 + 传感器相似度)
     *
     * 时间匹配度: 同小时=1.0, 相邻小时=0.5, 其他=0.1
     * 传感器相似度: 欧氏距离归一化后的补数 (越相似越接近 1.0)
     */
    int h_diff = abs(e.hour - target_hour);
    if (h_diff > 12) h_diff = 24 - h_diff;
    float time_score = (h_diff == 0) ? 1.0f :
                       (h_diff == 1) ? 0.5f : 0.1f;

    /* 传感器距离归一化 */
    float d_temp  = fabsf(e.temp_in - temp) / 20.0f;
    float d_humi  = fabsf(e.humi_in - humi) / 40.0f;
    float d_light = fabsf(e.light - light) / 50000.0f;
    if (d_temp  > 1.0f) d_temp  = 1.0f;
    if (d_humi  > 1.0f) d_humi  = 1.0f;
    if (d_light > 1.0f) d_light = 1.0f;

    float sensor_sim = 1.0f - (w_temp * d_temp + w_humi * d_humi
                                + w_light * d_light)
                              / (w_temp + w_humi + w_light);

    return time_score * (1.0f + sensor_sim);
}

static void predict_sensor_aware(void)
{
    printf("\n  ╔══════════════════════════════════════════════╗\n");
    printf(  "  ║    传感器感知预测 (时间+温/湿/光 加权)       ║\n");
    printf(  "  ╠══════════════════════════════════════════════╝\n");

    if ((int)g_patterns.size() < 5) {
        printf("  ║ 样本不足 (%d < 5)\n\n", (int)g_patterns.size());
        return;
    }

    /* 用最近 5 条记录的平均传感器值作为"当前环境" */
    float avg_temp = 0, avg_humi = 0, avg_light = 0;
    int n = std::min(5, (int)g_patterns.size());
    for (int i = (int)g_patterns.size() - n; i < (int)g_patterns.size(); i++) {
        avg_temp  += g_patterns[i].temp_in;
        avg_humi  += g_patterns[i].humi_in;
        avg_light += g_patterns[i].light;
    }
    avg_temp  /= n;
    avg_humi  /= n;
    avg_light /= n;

    printf("  ║ 当前环境: T=%.1f°C  H=%.0f%%  Light=%.0f lux\n",
           avg_temp, avg_humi, avg_light);
    printf("  ╠══════════════════════════════════════════════╣\n");

    /* 对每个小时计算加权得分 */
    struct hour_score_t { int h; float open_s; float close_s; int n; };
    hour_score_t scores[24] = {};

    for (int h = 0; h < 24; h++) {
        scores[h].h = h;
        int n_open = 0, n_close = 0;
        for (auto &e : g_patterns) {
            float s = weighted_score(e, h, avg_temp, avg_humi, avg_light,
                                     0.4f, 0.3f, 0.3f);
            if (e.action == 1) { scores[h].open_s += s; n_open++; }
            else               { scores[h].close_s += s; n_close++; }
        }
        scores[h].n = n_open + n_close;
    }

    /* 打印得分表 */
    for (int group = 0; group < 4; group++) {
        int gs = group * 6, ge = gs + 5;
        printf("  ║ %2d-%2d: ", gs, ge);
        float max_s = 0;
        for (int h = gs; h <= ge; h++) {
            float s = fmaxf(scores[h].open_s, scores[h].close_s);
            if (s > max_s) max_s = s;
        }
        int bar_total = 20;
        if (max_s > 0) {
            for (int h = gs; h <= ge; h++) {
                int open_bars = (int)(scores[h].open_s / max_s * bar_total / 6);
                int close_bars = (int)(scores[h].close_s / max_s * bar_total / 6);
                if (open_bars + close_bars == 0 && scores[h].n > 0) {
                    putchar('.');  /* 有数据但得分极低 */
                } else {
                    for (int i = 0; i < open_bars; i++)  putchar('o');
                    for (int i = 0; i < close_bars; i++) putchar('c');
                }
            }
        } else {
            for (int i = 0; i < 20; i++) putchar('.');
        }
        printf(" (o=开 c=关)\n");
    }
    printf("  ╚══════════════════════════════════════════════╝\n");
}

/* ── 打印预测计划表 ── */

static void print_schedule(void)
{
    printf("\n  ┌─────────────────────────────────────────┐\n");
    printf(  "  │         预 测 计 划 表                   │\n");
    printf(  "  ├──────┬────────┬────────┬────────────────┤\n");
    printf(  "  │  序号 │  时间   │  动作   │  置信度        │\n");
    printf(  "  ├──────┼────────┼────────┼────────────────┤\n");

    if (g_schedule.count == 0) {
        printf("  │  —   │   —    │   —    │  样本不足      │\n");
    } else {
        for (int i = 0; i < g_schedule.count; i++) {
            auto &s = g_schedule.entries[i];
            const char *act_cn = (strcmp(s.action, "open") == 0) ? "开窗" : "关窗";
            printf("  │  %2d   │ %s  │  %s   │     %3d%%       │\n",
                   i + 1, s.time_str, act_cn, s.confidence);
        }
    }
    printf("  └──────┴────────┴────────┴────────────────┘\n");
}

/* ── 打印最近操作 ── */

static void print_recent(void)
{
    printf("\n  最近 %d 条操作:\n", g_recent_count);
    int show = g_recent_count > 5 ? 5 : g_recent_count;
    for (int i = 0; i < show; i++) {
        const char *act = (strcmp(g_recent[i].action, "open") == 0) ? "开窗" : "关窗";
        printf("    %s %s\n", g_recent[i].time_str, act);
    }
}

int main()
{
    srand((unsigned)time(NULL));

    printf("┌──────────────────────────────────────────────────┐\n");
    printf("│       智能窗帘 — 自适应算法模拟器                  │\n");
    printf("│                                                  │\n");
    printf("│  输入格式: HH:MM open  或  HH:MM close            │\n");
    printf("│  示例:     08:30 open                            │\n");
    printf("│            15:00 close                           │\n");
    printf("│  输入 run   → 运行预测                            │\n");
    printf("│  输入 quit  → 退出                                │\n");
    printf("│  输入 demo  → 载入 30 条模拟数据                   │\n");
    printf("└──────────────────────────────────────────────────┘\n\n");

    char line[256];
    int input_count = 0;

    while (1) {
        printf("[%d] > ", input_count + 1);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* trim */
        char *p = line + strlen(line) - 1;
        while (p >= line && (*p == '\n' || *p == '\r')) { *p = '\0'; p--; }

        if (strlen(line) == 0) continue;
        if (strcmp(line, "quit") == 0) break;

        if (strcmp(line, "run") == 0) {
            printf("\n═══════════════════════════════════════\n");
            printf("  数据总量: %d 条\n", (int)g_patterns.size());

            /* 模式 A: 时间桶 */
            predict_time_only();
            print_schedule();

            /* 模式 B: 传感器感知 */
            predict_sensor_aware();

            print_recent();

            /* 打印传感器摘要 */
            if ((int)g_patterns.size() > 0) {
                printf("\n  ┌─────────────────────────────────────┐\n");
                printf(  "  │  传感器范围 (全部记录)               │\n");
                float tmin = 99, tmax = -99, hmin = 99, hmax = -99;
                float lmin = 1e9, lmax = -1;
                int smax = 0, rmax = 0;
                for (auto &e : g_patterns) {
                    if (e.temp_in < tmin) tmin = e.temp_in;
                    if (e.temp_in > tmax) tmax = e.temp_in;
                    if (e.humi_in < hmin) hmin = e.humi_in;
                    if (e.humi_in > hmax) hmax = e.humi_in;
                    if (e.light < lmin) lmin = e.light;
                    if (e.light > lmax) lmax = e.light;
                    if (e.smoke > smax) smax = e.smoke;
                    if (e.rain > rmax) rmax = e.rain;
                }
                printf("  │  室内温度: %.1f ~ %.1f °C\n", tmin, tmax);
                printf("  │  室内湿度: %.0f ~ %.0f %%\n", hmin, hmax);
                printf("  │  光照:     %.0f ~ %.0f lux\n", lmin, lmax);
                printf("  │  烟雾max:  %d%%     雨水max:  %d%%\n", smax, rmax);
                printf("  └─────────────────────────────────────┘\n");
            }

            printf("\n═══════════════════════════════════════\n\n");
            continue;
        }

        if (strcmp(line, "demo") == 0) {
            /* 生成 30 条模拟数据: 模拟 3-4 天, 每天早晚各操作一次 */
            const char *demo_ops[] = {
                "open", "open", "open", "open", "close", "close", "close", "close",
                "open", "open", "open", "open", "close", "close", "close", "close",
                "open", "open", "close", "close",
                "open", "open", "close", "close",
                "open", "open", "close", "close",
                "open", "close",
            };
            const int demo_hours[] = {
                 7,  7,  8,  8, 15, 15, 16, 16,
                 7,  7,  8,  8, 15, 15, 16, 16,
                 7,  8, 15, 16,
                 7,  8, 15, 16,
                 7,  8, 15, 16,
                 8, 16,
            };
            const int demo_mins[] = {
                20, 35, 10, 25, 30, 45,  0, 15,
                15, 30,  5, 20, 25, 40, 10, 20,
                30, 40, 50,  0,
                25, 15, 20, 45,
                10,  5, 30, 15,
                 0, 30,
            };
            int n = sizeof(demo_hours) / sizeof(demo_hours[0]);
            for (int i = 0; i < n; i++) {
                record_action(demo_hours[i], demo_mins[i], demo_ops[i]);
            }
            printf("  [demo] 已载入 %d 条模拟操作 (约4天早晚开/关窗)\n", n);
            input_count += n;
            continue;
        }

        /* 解析 HH:MM open/close */
        int hh, mm;
        char act[16];
        if (sscanf(line, "%d:%d %15s", &hh, &mm, act) == 3) {
            if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
                printf("  错误: 时间范围为 00:00-23:59\n");
                continue;
            }
            if (strcmp(act, "open") != 0 && strcmp(act, "close") != 0) {
                printf("  错误: 动作只能是 open 或 close\n");
                continue;
            }
            record_action(hh, mm, act);

            /* 打印传感器快照 */
            auto &last = g_patterns.back();
            printf("  → 记录 %s | T内=%.1f°C H内=%.0f%% 光照=%.0flux"
                   " 烟雾=%d%% 雨=%d%% 风=%s\n",
                   act, last.temp_in, last.humi_in, last.light,
                   last.smoke, last.rain, last.airflow ? "有" : "无");

            input_count++;
        } else {
            printf("  格式: HH:MM open/close   (如: 08:30 open)\n");
        }
    }

    printf("\n退出。共记录 %d 条操作。\n", input_count);
    return 0;
}
