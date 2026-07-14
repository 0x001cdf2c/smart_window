@echo off
set MSYSTEM=
set IDF_PATH=C:\Espressif\v5.5.4\esp-idf
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python_env\idf5.5_py3.13_env
set PATH=C:\Espressif\tools\python_env\idf5.5_py3.13_env\Scripts;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Windows\System32;C:\Windows
cd /d C:\Users\28145\Desktop\IOT\code\Net
echo Flashing C6 directly via COM8... > flash_log.txt 2>&1
python -m esptool --chip esp32c6 -p COM8 -b 460800 --before usb_reset --after hard_reset write_flash --flash_mode dio --flash_size detect --flash_freq 80m 0x0 esp_hosted_slave\slave\build\network_adapter.bin >> flash_log.txt 2>&1
echo Exit code: %ERRORLEVEL% >> flash_log.txt 2>&1
