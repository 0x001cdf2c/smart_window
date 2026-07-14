$env:MSYSTEM = ""
. C:\Espressif\v5.5.4\esp-idf\export.ps1
Set-Location C:\Users\28145\Desktop\IOT\code\Net

# Step 1: Copy real SR model to build dir to prevent dummy overwrite
Copy-Item -Force "build2\srmodels\srmodels.bin" "build\srmodels\srmodels.bin"

# Step 2: Build only (no flash)
idf.py build

# Step 3: Flash only the factory app partition (preserve model partition)
idf.py app-flash
