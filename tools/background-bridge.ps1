# Compatibility entry point for callers that still invoke this script directly.
# The scheduled task launches the panel executable itself to avoid a PowerShell wrapper.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$panel = Join-Path $projectRoot 'build\Release\axe-usb78-panel.exe'

if (-not (Test-Path -LiteralPath $panel -PathType Leaf)) {
    throw 'Build the tray panel first: .\tools\build.ps1 -Test'
}

& $panel --minimized
exit $LASTEXITCODE
