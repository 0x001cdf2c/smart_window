#!/usr/bin/env python3
"""Real-time airflow (motor/generator) voltage monitor.

Reads the ESP32-P4 serial log, extracts "AIN3 raw=<n> → <pct>%" lines, and
converts the raw ADS1115 value to real voltage.

ADS1115 config (adc_ads1115.c): PGA ±4.096V, 16-bit signed.
  -> 1 LSB = 4.096 / 32768 = 0.125 mV

Usage:
  python airflow_voltage_monitor.py [PORT] [--seconds N]
"""
import re
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial not found. Use the ESP-IDF venv python:", file=sys.stderr)
    print("  C:\\Espressif\\tools\\python\\v5.5.4\\venv\\Scripts\\python.exe airflow_voltage_monitor.py COM11", file=sys.stderr)
    sys.exit(1)

PORT = 'COM11'
SECONDS = 0
args = sys.argv[1:]
if args and not args[0].startswith('--'):
    PORT = args[0]
    args = args[1:]
if '--seconds' in args:
    i = args.index('--seconds')
    SECONDS = int(args[i + 1])

LSB_MV = 0.125  # 1 raw = 0.125 mV

line_re = re.compile(r'AIN3 raw=(-?\d+)')
pct_re = re.compile(r'(?:→|pct=)\s*(-?\d+)\s*%')


def fmt_voltage(raw):
    mv = raw * LSB_MV
    if abs(mv) >= 1000.0:
        return f"{mv/1000.0:.3f} V"
    return f"{mv:.2f} mV"


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.5)
    print(f"Connected to {PORT} @ 115200 baud", flush=True)
    print("Waiting for 'AIN3 raw=' lines... blow wind at the sensor.\n", flush=True)
    print(f"{'raw':>6}  {'voltage':>10}  {'pct':>5}", flush=True)
    print("-" * 26, flush=True)

    raws = []
    t0 = time.time()
    try:
        while True:
            line = ser.readline()
            if not line:
                if SECONDS and time.time() - t0 > SECONDS:
                    break
                continue
            txt = line.decode('utf-8', errors='replace')
            m = line_re.search(txt)
            if not m:
                continue
            raw = int(m.group(1))
            pct = int(pct_re.search(txt).group(1)) if pct_re.search(txt) else None
            raws.append(raw)
            ts = time.strftime('%H:%M:%S')
            pct_s = f"{pct}%" if pct is not None else "?"
            print(f"{raw:>6}  {fmt_voltage(raw):>10}  {pct_s:>5}   [{ts}]", flush=True)

            if SECONDS and time.time() - t0 > SECONDS:
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    if raws:
        print("\n" + "=" * 40, flush=True)
        print(f"samples: {len(raws)}", flush=True)
        print(f"raw      min={min(raws)}  max={max(raws)}  avg={sum(raws)/len(raws):.1f}", flush=True)
        print(f"voltage  min={fmt_voltage(min(raws))}  max={fmt_voltage(max(raws))}  "
              f"avg={fmt_voltage(sum(raws)/len(raws))}", flush=True)
        print("=" * 40, flush=True)
    else:
        print("\nNo 'AIN3 raw=' lines captured. Check: is the device connected/flashed? "
              "Is the sensor task logging?", flush=True)


if __name__ == '__main__':
    main()
