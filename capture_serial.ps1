$env:MSYSTEM = $null

$tools = "C:\Espressif\tools"
$env:PATH = "$tools\cmake\3.30.2\bin;$tools\ninja\1.12.1;$tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$tools\python\v5.5.4\venv\Scripts;$env:PATH"
$env:IDF_PATH = "C:\Espressif\v5.5.4\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "$tools\python\v5.5.4\venv"

Set-Location "C:\Users\28145\Desktop\IOT\code\Net"

$proc = Start-Process -FilePath python -ArgumentList "$env:IDF_PATH\tools\idf.py", "-p", "COM11", "monitor" -NoNewWindow -RedirectStandardOutput "C:\Users\28145\Desktop\IOT\code\Net\serial_log.txt" -RedirectStandardError "C:\Users\28145\Desktop\IOT\code\Net\serial_log_err.txt" -PassThru
Start-Sleep -Seconds 20
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Write-Output "Done - captured 20s"
