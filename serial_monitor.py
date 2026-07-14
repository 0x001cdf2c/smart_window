#!/usr/bin/env python3
"""持续串口监控 - Ctrl+C 退出"""
import serial
import signal
import sys

PORT = 'COM11'
BAUD = 115200

s = serial.Serial(PORT, BAUD, timeout=1)

def handler(sig, frame):
    print("\n\n[已退出监控]")
    s.close()
    sys.exit(0)

signal.signal(signal.SIGINT, handler)

print(f"=== 串口监控 {PORT} @ {BAUD} === 按 Ctrl+C 退出 ===\n")

try:
    while True:
        line = s.readline()
        if line:
            decoded = line.decode('utf-8', errors='replace').rstrip()
            print(decoded, flush=True)
except KeyboardInterrupt:
    pass
finally:
    s.close()
    print("\n[串口已关闭]")
