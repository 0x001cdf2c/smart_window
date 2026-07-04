Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYSCON -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue

$env:IDF_PATH = 'C:\Espressif\v5.5.4\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.4\venv'
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011/'
$env:IDF_CCACHE_ENABLE = '1'
$env:ESP_IDF_VERSION = '5.5'

$tp = 'C:\Espressif\tools'
$env:PATH = "$tp\ccache\4.12.1\ccache-4.12.1-windows-x86_64;$tp\cmake\3.30.2\bin;$tp\esp-clang\esp-19.1.2_20250312\esp-clang\bin;$tp\esp-rom-elfs\20241011\;$tp\idf-exe\1.0.3\;$tp\ninja\1.12.1\;$tp\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;$tp\riscv32-esp-elf-gdb\16.3_20250913\riscv32-esp-elf-gdb\bin;$tp\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$tp\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\riscv32-esp-elf\bin;$tp\python\v5.5.4\venv\Scripts;$env:PATH"

$python = "$tp\python\v5.5.4\venv\Scripts\python.exe"
$idfpy = "$env:IDF_PATH\tools\idf.py"

Set-Location 'C:\Users\28145\Desktop\IOT\code\Net'

Write-Host "=== Setting target esp32p4 ==="
& $python $idfpy set-target esp32p4
if ($LASTEXITCODE -ne 0) { Write-Host "set-target failed"; exit $LASTEXITCODE }

Write-Host "=== Building ==="
& $python $idfpy build
if ($LASTEXITCODE -ne 0) { Write-Host "Build failed"; exit $LASTEXITCODE }

Write-Host "=== Build successful ==="
