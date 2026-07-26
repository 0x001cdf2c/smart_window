#!/usr/bin/env python3
"""
adaptive_sim.py — 自适应算法模拟器

运行: python scripts/adaptive_sim.py

输入格式: HH:MM open/close
输入 demo → 载入 30 条模拟数据
输入 run  → 运行预测
"""

import time
import random
import math
import sys

random.seed()

# ── 全局数据 ──
patterns = []       # list of dict
schedule = []       # list of dict
recent = []         # list of dict (max 10)
RECENT_MAX = 10
SCHEDULE_MAX = 4
PATTERN_MAX = 300


def sim_sensors(hour):
    """根据时刻生成 6 类传感器的随机值 (正常范围)"""
    # 室外温度: 日夜正弦波
    day_phase = math.sin((hour - 6) * math.pi / 12.0)
    temp_out = 18.0 + 12.0 * max(day_phase, 0) + random.uniform(-3, 3)
    temp_out = max(5.0, min(40.0, temp_out))

    # 室内温度: 跟随室外但有滞后衰减
    temp_in = 20.0 + (temp_out - 15.0) * 0.6 + random.uniform(-1.5, 1.5)
    temp_in = max(10.0, min(35.0, temp_in))

    # 室外湿度: 与温度负相关
    humi_out = 50.0 - (temp_out - 20.0) * 2.0 + random.uniform(-10, 10)
    humi_out = max(25.0, min(95.0, humi_out))

    # 室内湿度
    humi_in = humi_out + random.uniform(-8, 8)
    humi_in = max(30.0, min(90.0, humi_in))

    # 光照: 6-18 白天
    if 6 <= hour <= 18:
        noon_phase = math.sin((hour - 6) * math.pi / 12.0)
        light = 500 + 45000 * max(noon_phase, 0) + random.uniform(-5000, 5000)
    else:
        light = random.uniform(0, 50)
    light = max(0, min(65535, light))

    # 烟雾: 90% 正常, 8% 轻度, 2% 报警
    r = random.uniform(0, 100)
    if r < 90:      smoke = random.randint(0, 5)
    elif r < 98:    smoke = random.randint(5, 20)
    else:           smoke = random.randint(15, 40)

    # 雨水: 85% 晴天, 10% 小雨, 5% 触发关窗
    r = random.uniform(0, 100)
    if r < 85:      rain = 0
    elif r < 95:    rain = random.randint(1, 40)
    else:           rain = random.randint(50, 100)

    # 气流: 70% 概率有风
    airflow = random.uniform(0, 100) < 70

    return {
        'temp_in': round(temp_in, 1),
        'humi_in': round(humi_in, 0),
        'temp_out': round(temp_out, 1),
        'humi_out': round(humi_out, 0),
        'light': round(light, 0),
        'smoke': smoke,
        'rain': rain,
        'airflow': airflow,
    }


def record_action(hour, minute, action):
    """记录一条用户操作"""
    sensors = sim_sensors(hour)
    sensors['hour'] = hour
    sensors['min'] = minute
    sensors['action'] = action  # "open" or "close"

    if len(patterns) >= PATTERN_MAX:
        patterns.pop(0)
    patterns.append(sensors)

    # 更新最近操作
    if len(recent) >= RECENT_MAX:
        recent.pop()
    recent.insert(0, {
        'time': f"{hour:02d}:{minute:02d}",
        'action': action,
    })


def predict_time_only():
    """模式 A: 纯时间桶预测 (与固件算法一致)"""
    global schedule
    schedule = []

    if len(patterns) < 5:
        print("  [时间桶] 样本不足 (%d < 5)\n" % len(patterns))
        return

    open_buckets = [0] * 24
    close_buckets = [0] * 24
    count_buckets = [0] * 24

    for p in patterns:
        h = p['hour']
        if p['action'] == 'open':
            open_buckets[h] += 1
        else:
            close_buckets[h] += 1
        count_buckets[h] += 1

    # ASCII 直方图
    max_cnt = max(count_buckets) if max(count_buckets) > 0 else 1

    print()
    print("  ╔══════════════════════════════════════════╗")
    print("  ║    24 小时操作频次分布 (纯时间桶)          ║")
    print("  ╠══════════════════════════════════════════╣")
    print("  ║ 时  开   关   总数  柱状图                ║")

    for h in range(24):
        o = open_buckets[h]
        c = close_buckets[h]
        t = count_buckets[h]
        bar_len = int(t * 18 / max_cnt) if t > 0 else 0
        bar = ''
        if t > 0:
            o_len = int(o * bar_len / t) if bar_len > 0 else 0
            c_len = bar_len - o_len
            bar = 'O' * o_len + 'C' * c_len
        else:
            bar = '.' * 18
        print(f"  ║ {h:02d}  {o:3d}  {c:3d}  {t:3d}  {bar}")

    print("  ╚══════════════════════════════════════════╝")

    # 选 Top-2 开窗 + Top-2 关窗
    open_bk = open_buckets[:]
    close_bk = close_buckets[:]

    for _ in range(2):
        if len(schedule) >= SCHEDULE_MAX: break
        best_h = max(range(24), key=lambda h: open_bk[h])
        if open_bk[best_h] > 0 and count_buckets[best_h] > 0:
            conf = open_bk[best_h] * 100 // count_buckets[best_h]
            schedule.append({
                'time': f"{best_h:02d}:30", 'action': 'open', 'confidence': conf
            })
            open_bk[best_h] = 0

    for _ in range(2):
        if len(schedule) >= SCHEDULE_MAX: break
        best_h = max(range(24), key=lambda h: close_bk[h])
        if close_bk[best_h] > 0 and count_buckets[best_h] > 0:
            conf = close_bk[best_h] * 100 // count_buckets[best_h]
            schedule.append({
                'time': f"{best_h:02d}:30", 'action': 'close', 'confidence': conf
            })
            close_bk[best_h] = 0


def predict_sensor_aware():
    """模式 B: 时间+传感器加权预测"""
    if len(patterns) < 5:
        print("  样本不足\n")
        return

    # 最近 5 条的平均传感器值作为"当前环境"
    n = min(5, len(patterns))
    recent_p = patterns[-n:]
    avg_temp = sum(p['temp_in'] for p in recent_p) / n
    avg_humi = sum(p['humi_in'] for p in recent_p) / n
    avg_light = sum(p['light'] for p in recent_p) / n

    print("  ╔══════════════════════════════════════════╗")
    print("  ║   传感器感知预测 (时间+温/湿/光 加权)       ║")
    print(f"  ║   当前环境: T={avg_temp:.1f}°C  H={avg_humi:.0f}%  L={avg_light:.0f}lux")
    print("  ╠══════════════════════════════════════════╣")

    def weighted_score(p, target_hour):
        h_diff = abs(p['hour'] - target_hour)
        if h_diff > 12: h_diff = 24 - h_diff
        time_score = 1.0 if h_diff == 0 else (0.5 if h_diff == 1 else 0.1)

        w_t, w_h, w_l = 0.4, 0.3, 0.3
        d_temp = min(abs(p['temp_in'] - avg_temp) / 20.0, 1.0)
        d_humi = min(abs(p['humi_in'] - avg_humi) / 40.0, 1.0)
        d_light = min(abs(p['light'] - avg_light) / 50000.0, 1.0)

        sensor_sim = 1.0 - (w_t * d_temp + w_h * d_humi + w_l * d_light) / (w_t + w_h + w_l)
        return time_score * (1.0 + sensor_sim)

    # 每小时得分
    hour_scores = []
    for h in range(24):
        open_s = sum(weighted_score(p, h) for p in patterns if p['action'] == 'open')
        close_s = sum(weighted_score(p, h) for p in patterns if p['action'] == 'close')
        n = sum(1 for p in patterns if p['hour'] == h)
        hour_scores.append((h, open_s, close_s, n))

    for h in range(24):
        _, o, c, n = hour_scores[h]
        bar_o = int(o * 10) if o > 0 else 0
        bar_c = int(c * 10) if c > 0 else 0
        bar = ('o' * min(bar_o, 20) + 'c' * min(bar_c, 20))[:20]
        if not bar: bar = '.'
        print(f"  ║ {h:02d}  {bar:<20s}  o={o:4.1f} c={c:4.1f}")

    print("  ╚══════════════════════════════════════════╝")


def print_schedule():
    print()
    print("  ┌─────────────────────────────────────┐")
    print("  │        预 测 计 划 表                │")
    print("  ├──────┬────────┬────────┬───────────┤")
    print("  │ 序号 │  时间   │  动作   │  置信度    │")
    print("  ├──────┼────────┼────────┼───────────┤")
    if not schedule:
        print("  │  —   │   —    │   —    │ 样本不足   │")
    else:
        for i, s in enumerate(schedule):
            act = "开窗" if s['action'] == 'open' else "关窗"
            print(f"  │  {i+1}   │ {s['time']}  │  {act}   │   {s['confidence']:3d}%     │")
    print("  └──────┴────────┴────────┴───────────┘")


def print_recent():
    if not recent:
        return
    print("\n  最近操作:")
    for i, r in enumerate(recent[:5]):
        act = "开窗" if r['action'] == 'open' else "关窗"
        print(f"    {r['time']}  {act}")


def print_sensor_summary():
    if not patterns:
        return
    t_in_min = min(p['temp_in'] for p in patterns)
    t_in_max = max(p['temp_in'] for p in patterns)
    h_in_min = min(p['humi_in'] for p in patterns)
    h_in_max = max(p['humi_in'] for p in patterns)
    l_min = min(p['light'] for p in patterns)
    l_max = max(p['light'] for p in patterns)
    s_max = max(p['smoke'] for p in patterns)
    r_max = max(p['rain'] for p in patterns)

    print()
    print("  ┌─────────────────────────────────────┐")
    print("  │  传感器范围 (全部记录)                │")
    print(f"  │  室内温度: {t_in_min:.1f} ~ {t_in_max:.1f} °C")
    print(f"  │  室内湿度: {h_in_min:.0f} ~ {h_in_max:.0f} %")
    print(f"  │  光照:     {l_min:.0f} ~ {l_max:.0f} lux")
    print(f"  │  烟雾max:  {s_max}%     雨水max:  {r_max}%")
    print("  └─────────────────────────────────────┘")


def main():
    print("┌──────────────────────────────────────────────────┐")
    print("│     智能窗帘 — 自适应算法模拟器 (Python)           │")
    print("│                                                  │")
    print("│  输入格式: HH:MM open  或  HH:MM close            │")
    print("│  示例:     08:30 open                            │")
    print("│            15:00 close                           │")
    print("│  输入 run   → 运行预测                            │")
    print("│  输入 demo  → 载入 30 条模拟数据                   │")
    print("│  输入 quit  → 退出                                │")
    print("└──────────────────────────────────────────────────┘\n")

    input_count = 0

    while True:
        try:
            line = input(f"[{input_count + 1}] > ").strip()
        except (EOFError, KeyboardInterrupt):
            break

        if not line:
            continue
        if line == 'quit':
            break

        if line == 'run':
            print("\n" + "=" * 44)
            print(f"  数据总量: {len(patterns)} 条\n")

            # 模式 A: 时间桶
            predict_time_only()
            print_schedule()

            # 模式 B: 传感器感知
            predict_sensor_aware()

            print_recent()
            print_sensor_summary()

            print("\n" + "=" * 44 + "\n")
            continue

        if line == 'demo':
            # 模拟 4 天早晚操作
            demo_data = [
                ( 7, 20, 'open'), ( 7, 35, 'open'), ( 8, 10, 'open'), ( 8, 25, 'open'),
                (15, 30, 'close'), (15, 45, 'close'), (16,  0, 'close'), (16, 15, 'close'),
                ( 7, 15, 'open'), ( 7, 30, 'open'), ( 8,  5, 'open'), ( 8, 20, 'open'),
                (15, 25, 'close'), (15, 40, 'close'), (16, 10, 'close'), (16, 20, 'close'),
                ( 7, 30, 'open'), ( 8, 40, 'open'), (15, 50, 'close'), (16,  0, 'close'),
                ( 7, 25, 'open'), ( 8, 15, 'open'), (15, 20, 'close'), (16, 45, 'close'),
                ( 7, 10, 'open'), ( 8,  5, 'open'), (15, 30, 'close'), (16, 15, 'close'),
                ( 8, 30, 'open'), (16, 30, 'close'),
            ]
            for hh, mm, act in demo_data:
                record_action(hh, mm, act)
            print(f"  [demo] 已载入 {len(demo_data)} 条模拟操作 (约4天早晚开/关窗)")
            input_count += len(demo_data)
            continue

        # 解析 HH:MM open/close
        parts = line.split()
        if len(parts) != 2:
            print("  格式: HH:MM open/close  (如: 08:30 open)")
            continue

        time_str, act = parts
        if ':' not in time_str:
            print("  格式: HH:MM open/close")
            continue

        hh_str, mm_str = time_str.split(':')
        try:
            hh, mm = int(hh_str), int(mm_str)
        except ValueError:
            print("  时间必须是数字")
            continue

        if not (0 <= hh <= 23 and 0 <= mm <= 59):
            print("  时间范围 00:00-23:59")
            continue
        if act not in ('open', 'close'):
            print("  动作只能是 open 或 close")
            continue

        record_action(hh, mm, act)
        last = patterns[-1]
        print(f"  → 记录 {act} | T内={last['temp_in']:.1f}°C H内={last['humi_in']:.0f}%"
              f" 光照={last['light']:.0f}lux 烟雾={last['smoke']}% 雨={last['rain']}%"
              f" 风={'有' if last['airflow'] else '无'}")

        input_count += 1

    print(f"\n退出。共记录 {input_count} 条操作。")


if __name__ == '__main__':
    main()
