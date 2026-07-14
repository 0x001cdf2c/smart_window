$env:MSYSTEM = ""
. C:\Espressif\v5.5.4\esp-idf\export.ps1
Set-Location C:\Users\28145\Desktop\IOT\code\Net

# Step 1: Restore SR models first
Write-Host "=== Flashing SR models ==="
python C:\Espressif\v5.5.4\esp-idf\components\esptool_py\esptool\esptool.py --chip esp32p4 --port COM3 --baud 921600 write_flash 0x610000 build2\srmodels\srmodels.bin

# Step 2: Build
Write-Host "=== Building ==="
idf.py build

# Step 3: Flash app only (preserve model partition)
Write-Host "=== Flashing app ==="
idf.py app-flash
