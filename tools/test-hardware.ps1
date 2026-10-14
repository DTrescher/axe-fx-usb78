[CmdletBinding()]
param([Parameter(Mandatory)][string]$EndpointId,[ValidateRange(30,3600)][int]$DurationSeconds=120)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$releaseRoot=Join-Path $projectRoot 'build\Release'
$reportRoot=Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Force -Path $reportRoot | Out-Null
$report=Join-Path $reportRoot 'hardware-test.txt'
& (Join-Path $releaseRoot 'axe-fas-volume-test.exe') --endpoint-id $EndpointId | Tee-Object -FilePath $report
if($LASTEXITCODE) {throw 'Native endpoint volume/mute verification failed.'}
& (Join-Path $releaseRoot 'axe-fas-shared-mix-test.exe') $EndpointId --verify-session-volume | Tee-Object -FilePath $report -Append
if($LASTEXITCODE) {throw 'Native shared-session mixing verification failed.'}
& (Join-Path $releaseRoot 'axe-fas-feasibility.exe') $EndpointId --seconds $DurationSeconds | Tee-Object -FilePath $report -Append
if($LASTEXITCODE) {throw 'Physical FAS-only relay verification failed.'}
& (Join-Path $releaseRoot 'axe-usb78.exe') --bridge-test --endpoint-id $EndpointId --seconds $DurationSeconds | Tee-Object -FilePath $report -Append
if($LASTEXITCODE) {throw 'Application bridge verification failed.'}
Write-Output "Hardware checks passed; results saved to $report"
