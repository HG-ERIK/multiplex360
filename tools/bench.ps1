# Plays fixed stretches of test videos directly (no conversion) and prints the console's
# BENCHMARK lines: decoded/shown/dropped frames and catch-ups over 90 s.
# Needs tools\loglisten.ps1 running and logto= in plex.ini.
param([string[]]$Runs = @('2560:1500', '680:600', '2407:600'), [int]$Seconds = 90)
$root = Split-Path $PSScriptRoot -Parent
foreach ($run in $Runs) {
    $key, $start = $run.Split(':')
    $ini = (Get-Content "$root\plex.ini") | Where-Object { $_ -notmatch '^(testnav|autoplay|autoplaystart|benchmark|directmax|kbps)=' }
    $ini + "autoplay=$key" + "autoplaystart=$start" + "benchmark=$Seconds" + 'directmax=1080' + 'kbps=12000' |
        Set-Content "$root\plex.ini" -Encoding ASCII
    $before = @(Get-Content "$root\screenshots\udplog.txt" | Select-String 'BENCHMARK').Count
    & "$root\tools\run.ps1" -Wait 0 *>&1 | Out-Null
    $deadline = (Get-Date).AddSeconds(180)
    $result = "$run => no result"
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 10
        $all = @(Get-Content "$root\screenshots\udplog.txt" | Select-String 'BENCHMARK')
        if ($all.Count -gt $before) { $result = "$run => $($all[-1].Line)"; break }
    }
    $result
}
$ini = (Get-Content "$root\plex.ini") | Where-Object { $_ -notmatch '^(testnav|autoplay|autoplaystart|benchmark|directmax|kbps)=' }
$ini | Set-Content "$root\plex.ini" -Encoding ASCII
