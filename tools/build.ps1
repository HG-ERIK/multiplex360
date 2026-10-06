# Builds PlexClient for Xbox 360 with VS2010's 32-bit MSBuild.
#   .\tools\build.ps1            -> Release
#   .\tools\build.ps1 Debug
param([ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$msbuild = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"

& $msbuild "$root\PlexClient.vcxproj" /nologo /v:minimal /m `
    "/p:Configuration=$Configuration" "/p:Platform=Xbox 360"
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

$xex = "$root\build\$Configuration\default.xex"
Write-Host "Built $xex ($([math]::Round((Get-Item $xex).Length / 1KB)) KB)"
