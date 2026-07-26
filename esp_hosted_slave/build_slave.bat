@echo off
cd /d C:\Espressif\v5.5.4\esp-idf
call export.bat
cd /d C:\Users\28145\Desktop\IOT\code\Net\esp_hosted_slave\slave
idf.py set-target esp32c6
idf.py build
