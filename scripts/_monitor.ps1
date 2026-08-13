Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYSCON -ErrorAction SilentlyContinue
Remove-Item Env:MSYS2_PATH_TYPE -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue

$env:IDF_PATH = 'C:\Espressif\v5.5.4\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif\tools'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v5.5.4\venv'
$env:IDF_COMPONENT_LOCAL_STORAGE_URL = 'file://C:\Espressif\tools'
$env:ESP_ROM_ELF_DIR = 'C:\Espressif\tools\esp-rom-elfs\20241011/'
$env:OPENOCD_SCRIPTS = 'C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215/openocd-esp32/share/openocd/scripts'
$env:IDF_CCACHE_ENABLE = '1'
$env:PYTHONIOENCODING = 'utf-8'

$toolsPath = 'C:\Espressif\tools'
$env:PATH = "$toolsPath\ccache\4.12.1\ccache-4.12.1-windows-x86_64;$toolsPath\cmake\3.30.2\bin;$toolsPath\esp-clang\esp-19.1.2_20250312\esp-clang\bin;$toolsPath\esp-rom-elfs\20241011\;$toolsPath\idf-exe\1.0.3\;$toolsPath\ninja\1.12.1\;$toolsPath\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin;$toolsPath\riscv32-esp-elf-gdb\16.3_20250913\riscv32-esp-elf-gdb\bin;$toolsPath\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$toolsPath\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\riscv32-esp-elf\bin;$toolsPath\python\v5.5.4\venv\Scripts;$env:PATH"

Set-Location 'C:\Users\28145\Desktop\IOT\code\Net'

& "$toolsPath\python\v5.5.4\venv\Scripts\python.exe" -m esp_idf_monitor -p COM11 -b 115200 --target esp32p4 build\Net.elf
