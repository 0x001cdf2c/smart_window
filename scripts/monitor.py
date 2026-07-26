"""Serial monitor for ESP32-P4 — continuous output until Ctrl+C."""
import serial
import sys
import time

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM11'
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

ser = serial.Serial(PORT, BAUD, timeout=0.5)
print(f'Connected to {PORT} @ {BAUD} baud', flush=True)
print('Press Ctrl+C to exit\n', flush=True)

try:
    while True:
        line = ser.readline()
        if line:
            print(line.decode('utf-8', errors='replace'), end='', flush=True)
except KeyboardInterrupt:
    print('\nDisconnected.', flush=True)
finally:
    ser.close()
