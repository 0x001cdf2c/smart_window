@echo off
set MSYSTEM=
set IDF_PATH=C:\Espressif\v5.5.4\esp-idf
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf5.5_py3.13_env
set PATH=C:\Espressif\tools\python_env\idf5.5_py3.13_env\Scripts;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Windows\System32;C:\Windows
cd /d C:\Users\28145\Desktop\IOT\code\Net

echo ===== COM8 scan =====
python -m esptool --port COM8 --no-stub read_mac 2>&1
echo ===== COM10 scan =====
python -m esptool --port COM10 --no-stub read_mac 2>&1
