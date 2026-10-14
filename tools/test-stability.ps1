[CmdletBinding()]
param([Parameter(Mandatory)][string]$EndpointId,
      [ValidateRange(30,3600)][int]$DurationSeconds=600,
      [ValidateRange(0,3600)][int]$LoadSeconds=180,
      [ValidateRange(0,32)][int]$Workers=12,
      [ValidateRange(13,100)][int]$QueueMs=13)
$ErrorActionPreference='Stop'
if($LoadSeconds -gt $DurationSeconds) {throw 'LoadSeconds must not exceed DurationSeconds.'}
$projectRoot=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $projectRoot 'build\Release\axe-usb78.exe'
if(-not (Test-Path -LiteralPath $exe)) {throw 'Build the bridge first.'}
$reportRoot=Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
$report=Join-Path $reportRoot 'stability-test.txt'
$workerScript=Join-Path $PSScriptRoot 'cpu-load-worker.ps1'
$powershell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$loadProcesses=@()
try {
    if($LoadSeconds -gt 0) {
        for($index=0; $index -lt $Workers; $index++) {
            $loadProcesses+=Start-Process -FilePath $powershell -ArgumentList @('-NoProfile','-File',"`"$workerScript`"",'-DurationSeconds',"$LoadSeconds") -WindowStyle Hidden -PassThru
        }
    }
    & $exe --bridge-test --endpoint-id $EndpointId --seconds $DurationSeconds --buffer 128 --queue-ms $QueueMs | Tee-Object -FilePath $report
    if($LASTEXITCODE) {throw 'Bridge stability test failed; inspect artifacts/stability-test.txt.'}
} finally {
    foreach($process in $loadProcesses) {if(-not $process.HasExited) {Stop-Process -Id $process.Id -ErrorAction SilentlyContinue}}
}
Write-Output "Stability test passed: $DurationSeconds seconds, $Workers workers for $LoadSeconds seconds, queue $QueueMs ms. Report: $report"
