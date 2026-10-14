[CmdletBinding()]
param([switch]$Minimized)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$panel = Join-Path $projectRoot 'build\Release\axe-usb78-panel.exe'

if (-not (Test-Path -LiteralPath $panel -PathType Leaf)) {
    throw 'Build the tray panel first: .\tools\build.ps1 -Test'
}

$startOptions = @{
    FilePath = $panel
    WorkingDirectory = $projectRoot
    PassThru = $true
}
if ($Minimized) { $startOptions.ArgumentList = '--minimized' }
Start-Process @startOptions | Out-Null
Write-Output 'Started Axe-Fx III USB 7-8 control panel.'
