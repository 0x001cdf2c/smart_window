$port = New-Object System.IO.Ports.SerialPort COM11, 115200, None, 8, One
$port.ReadTimeout = 2000
try {
    $port.Open()
    $start = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt 12) {
        try {
            $line = $port.ReadLine()
            Write-Host $line
        } catch {}
    }
    $port.Close()
} catch {
    Write-Host "Error: $_"
}
