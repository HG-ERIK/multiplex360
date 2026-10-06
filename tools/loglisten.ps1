# Receives the console's UDP log lines (set logto=<this PC's IP>:5555 in plex.ini) and
# appends them to screenshots\udplog.txt. Lines arrive as they're written, so the log
# survives a console freeze. Stop with Ctrl+C (or stop the background job).
param([int]$Port = 5555)

$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force "$root\screenshots" | Out-Null
$out = "$root\screenshots\udplog.txt"
$udp = New-Object System.Net.Sockets.UdpClient $Port
$from = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any, 0)
Write-Host "Listening on UDP $Port -> $out"
try {
    while ($true) {
        $bytes = $udp.Receive([ref]$from)
        $line = [System.Text.Encoding]::UTF8.GetString($bytes).TrimEnd("`r", "`n")
        Add-Content -LiteralPath $out -Value $line -Encoding UTF8
    }
} finally {
    $udp.Close()
}
