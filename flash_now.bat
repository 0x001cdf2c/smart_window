@echo off
set IDF_PATH=C:\Espressif\v5.5.4\esp-idf
set PATH=C:\Users\28145\.espressif\python_env\idf5.5_py3.13_env\Scripts;C:\Users\28145\.espressif\tools\xtensa-esp-elf\esp-14.2_20241126\xtensa-esp-elf\bin;C:\Users\28145\.espressif\tools\riscv32-esp-elf\esp-14.2_20241126\riscv32-esp-elf\bin;C:\Users\28145\.espressif\tools\cmake\3.30.7\bin;C:\Users\28145\.espressif\tools\ninja\1.12.0;C:\Windows\System32;C:\Windows
cd /d C:\Users\28145\Desktop\IOT\code\Net
echo Starting flash...
python C:\Espressif\v5.5.4\esp-idf\tools\idf.py -p COM8 flash
echo Flash completed with exit code: %ERRORLEVEL%
