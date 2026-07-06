import serial, time, sys

s = serial.Serial('COM8', 115200, timeout=0.5)
t0 = time.time()
print("=== Serial capture started ===")
try:
    while True:
        n = s.in_waiting
        if n > 0:
            data = s.read(n)
            try:
                print(data.decode('utf-8', errors='replace'), end='', flush=True)
            except:
                print(str(data), end='', flush=True)
        else:
            time.sleep(0.1)
except KeyboardInterrupt:
    pass
finally:
    s.close()
    print("\n=== Serial capture ended ===")
