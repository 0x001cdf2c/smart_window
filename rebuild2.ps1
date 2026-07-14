$env:MSYSTEM = ""
. C:\Espressif\v5.5.4\esp-idf\export.ps1
Set-Location C:\Users\28145\Desktop\IOT\code\Net
idf.py build
idf.py app-flash -p COM11
