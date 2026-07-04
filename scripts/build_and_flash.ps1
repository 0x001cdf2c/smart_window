$env:IDF_PATH = 'C:\Espressif\v5.5.4\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.4\venv'

$python = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
$idfpy = "$env:IDF_PATH\tools\idf.py"

Set-Location 'C:\Users\28145\Desktop\IOT\code\Net'

Write-Host "=== Setting target to esp32p4 ==="
& $python $idfpy set-target esp32p4

Write-Host "=== Building ==="
& $python $idfpy build
