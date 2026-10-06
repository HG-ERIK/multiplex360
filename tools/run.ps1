# Copies the build to the console (hdd:\PlexClient), launches it, and optionally
# takes a screenshot and pulls back the log.
#   .\tools\run.ps1                         -> deploy Release + launch
#   .\tools\run.ps1 -Screenshot -Wait 8     -> also capture screenshots\latest.png after 8 s
#   .\tools\run.ps1 -NoDeploy -Screenshot   -> just capture what's on screen now
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$NoDeploy,
    [switch]$Screenshot,
    [int]$Wait = 8
)

# xbdm.dll is 32-bit, so re-run this script under 32-bit PowerShell.
if ([Environment]::Is64BitProcess) {
    $ps32 = "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    $fwd = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Configuration', $Configuration, '-Wait', $Wait)
    if ($NoDeploy) { $fwd += '-NoDeploy' }
    if ($Screenshot) { $fwd += '-Screenshot' }
    & $ps32 @fwd
    exit $LASTEXITCODE
}

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$xedk = [Environment]::GetEnvironmentVariable('XEDK', 'Machine')
$bin = "$xedk\bin\win32"
$remote = 'hdd:\PlexClient'
$out = "$root\build\$Configuration"
$console = (Get-ItemProperty 'HKCU:\Software\Microsoft\XenonSDK').XboxName

# xbcp.exe can't upload to this RGH console (it exits 1 silently), but xbdm's
# DmSendFile works, so uploads go through it directly.
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Xbdm {
    [DllImport(@"$bin\xbdm.dll", CharSet = CharSet.Ansi)] public static extern int DmSetXboxName(string name);
    [DllImport(@"$bin\xbdm.dll", CharSet = CharSet.Ansi)] public static extern int DmSendFile(string local, string remote);
    [DllImport(@"$bin\xbdm.dll", CharSet = CharSet.Ansi)] public static extern int DmMkdir(string remote);
}
"@

function Test-Hr([int]$hr, [string]$what) {
    # Success HRESULTs have the top bit clear (XBDM_NOERR is 0x02DA0000).
    if ($hr -lt 0) { throw ("{0} failed: 0x{1:X8}" -f $what, $hr) }
}

function Send-File([string]$local, [string]$dest) {
    Test-Hr ([Xbdm]::DmSendFile($local, $dest)) "Upload $dest"
}

function Send-Dir([string]$local, [string]$dest) {
    [void][Xbdm]::DmMkdir($dest)   # fails harmlessly if it exists
    foreach ($item in Get-ChildItem -LiteralPath $local) {
        if ($item.PSIsContainer) { Send-Dir $item.FullName "$dest\$($item.Name)" }
        else { Send-File $item.FullName "$dest\$($item.Name)" }
    }
}

function Invoke-Xb([string]$tool, [string[]]$arguments) {
    # The XDK tools print banners to stderr; only the exit code means failure.
    $ErrorActionPreference = 'Continue'
    $output = & "$bin\$tool.exe" @arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$tool failed: $($output -join "`n")" }
    $output
}

Test-Hr ([Xbdm]::DmSetXboxName($console)) "Connect to $console"

if (-not $NoDeploy) {
    if (-not (Test-Path "$root\plex.ini")) { throw "plex.ini missing - copy plex.ini.example to plex.ini" }
    Send-Dir "$out\Media" "$remote\Media"
    Send-File "$root\plex.ini" "$remote\plex.ini"
    Send-File "$out\default.xex" "$remote\default.xex"
    Write-Host "Deployed to $console $remote"
    Invoke-Xb xbreboot @("$remote\default.xex") | Out-Null
    Write-Host "Launched $remote\default.xex"
}

if ($Screenshot) {
    Start-Sleep -Seconds $Wait
    New-Item -ItemType Directory -Force "$root\screenshots" | Out-Null
    $bmp = "$root\screenshots\latest.bmp"
    Invoke-Xb xbcapture @($bmp) | Out-Null

    Add-Type -AssemblyName System.Drawing
    $img = [System.Drawing.Image]::FromFile($bmp)
    $small = New-Object System.Drawing.Bitmap $img, 1280, ([int](1280 * $img.Height / $img.Width))
    $img.Dispose()
    $small.Save("$root\screenshots\latest.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $small.Dispose()
    Remove-Item $bmp
    Write-Host "Screenshot: screenshots\latest.png"

    try {
        Invoke-Xb xbcp @('/y', '/q', "$remote\plexlog.txt", "$root\screenshots\plexlog.txt") | Out-Null
        Write-Host "--- plexlog.txt"
        Get-Content "$root\screenshots\plexlog.txt"
    } catch { Write-Host "(no log on console yet)" }
}
