[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeFile = Join-Path $projectRoot 'CMakeLists.txt'
$cmakeContent = Get-Content -LiteralPath $cmakeFile -Raw -Encoding UTF8
$versionMatch = [regex]::Match(
    $cmakeContent,
    '(?m)^project\(\s*\S+\s+VERSION\s+(?<version>\d+(?:\.\d+){1,3})'
)
if (-not $versionMatch.Success) {
    throw 'Could not read the project version from CMakeLists.txt.'
}
$version = $versionMatch.Groups['version'].Value

$buildScript = Join-Path $PSScriptRoot 'build.ps1'
& $buildScript -Configuration Release -BuildDirectory 'build\installer'

$compiler = (Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue).Source
if (-not $compiler) {
    $compilerCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')
    )
    $compiler = $compilerCandidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}
if (-not $compiler) {
    throw 'Inno Setup 6 or 7 was not found. Install it or add ISCC.exe to PATH.'
}

$installerScript = Join-Path $projectRoot 'installer\axe-fx-usb78.iss'
$outputDirectory = Join-Path $projectRoot 'artifacts\installer'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

Push-Location (Split-Path -Parent $installerScript)
try {
    & $compiler "/DAppVersion=$version" (Split-Path -Leaf $installerScript)
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$installer = Join-Path $outputDirectory "axe-fx-usb78-setup-$version.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw 'Inno Setup completed without producing the expected installer.'
}

Write-Output "Created $installer"
