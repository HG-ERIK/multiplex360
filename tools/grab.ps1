# Grabs one frame from the HDMI capture card ("USB Video") into screenshots\cam.png.
# Works when xbcapture can't: boot, dashboard, crash screens, a hung console.
#   .\tools\grab.ps1                 -> screenshots\cam.png
#   .\tools\grab.ps1 -Name crash1    -> screenshots\crash1.png
# Needs FFmpeg (winget install Gyan.FFmpeg).
param([string]$Name = 'cam', [string]$Device = 'USB Video')

$root = Split-Path $PSScriptRoot -Parent
$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ff) {
    $ff = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $ff) { throw 'ffmpeg not found (winget install Gyan.FFmpeg)' }

New-Item -ItemType Directory -Force "$root\screenshots" | Out-Null
$out = "$root\screenshots\$Name.png"
# Skip the first frames: the card needs a moment to settle exposure/sync.
$ErrorActionPreference = 'Continue'
& $ff -hide_banner -loglevel fatal -y -f dshow -rtbufsize 64M -i "video=$Device" `
    -vf "select=gte(n\,15),scale=1280:-2" -frames:v 1 $out 2>&1 | ForEach-Object { "$_" }
if (-not (Test-Path $out)) { throw "no frame captured from '$Device'" }
Write-Host "Captured $out"
