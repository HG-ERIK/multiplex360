# Checks that the console recovers on its own: launches the app with testcrash= or testhang=,
# puts the normal plex.ini back right away, then watches xbdm and the capture card.
#   .\tools\recovertest.ps1 -Mode hang -After 20 -Watch 90
param([ValidateSet('crash', 'hang')] [string]$Mode = 'hang', [int]$After = 20, [int]$Watch = 90)

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$bin = "$([Environment]::GetEnvironmentVariable('XEDK', 'Machine'))\bin\win32"

$orig = Get-Content plex.ini
($orig + "test$Mode=$After") | Set-Content plex.ini -Encoding ASCII
& "$root\tools\run.ps1" -Wait 0 *>&1 | Out-Null
$orig | Set-Content plex.ini -Encoding ASCII
Start-Sleep 4
& "$root\tools\push.ps1" plex.ini 'hdd:\PlexClient\plex.ini' | Out-Null   # never relaunch into the test

Add-Type -AssemblyName System.Drawing
$t0 = Get-Date
$i = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Watch) {
    $s = [int]((Get-Date) - $t0).TotalSeconds
    $j = Start-Job { & "$using:bin\xbdir.exe" 'hdd:\' 2>&1 | Out-Null; $LASTEXITCODE }
    $ok = if (Wait-Job $j -Timeout 6) { (Receive-Job $j) -eq 0 } else { Stop-Job $j; $false }
    $name = '{0}_{1:D2}' -f $Mode, $i
    & "$root\tools\grab.ps1" -Name $name *>&1 | Out-Null
    $light = 0
    if (Test-Path "$root\screenshots\$name.png") {
        $b = [System.Drawing.Bitmap]::FromFile("$root\screenshots\$name.png")
        foreach ($p in @(@(320, 180), @(640, 360), @(960, 540), @(200, 600), @(1100, 100))) {
            $c = $b.GetPixel($p[0], $p[1]); $light += $c.R + $c.G + $c.B
        }
        $b.Dispose()
    }
    '{0,4}s  xbdm {1,-7}  screen {2,4}  {3}' -f $s, $(if ($ok) { 'answers' } else { 'DOWN' }), $light, $name
    $i++
    Start-Sleep 3
}
$log = "$env:TEMP\plexlog.recover.txt"
& "$bin\xbcp.exe" /Y /Q 'hdd:\PlexClient\plexlog.txt' $log *>&1 | Out-Null
if (Test-Path $log) { Get-Content $log | Select-String 'Test (crash|hang)|GUARD' | ForEach-Object Line }
