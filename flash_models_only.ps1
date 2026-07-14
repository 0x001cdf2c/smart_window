$env:MSYSTEM = ""
. C:\Espressif\v5.5.4\esp-idf\export.ps1
Set-Location C:\Users\28145\Desktop\IOT\code\Net
idf.py set-target esp32p4
python C:\Espressif\v5.5.4\esp-idf\components\esptool_py\esptool\esptool.py --chip esp32p4 --port COM3 --baud 921600 --before default_reset write_flash 0x610000 build2\srmodels\srmodels.bin
