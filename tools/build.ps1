[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [string]$BuildDirectory = 'build'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    throw 'BuildDirectory cannot be empty.'
}
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
}

$cmake = (Get-Command 'cmake' -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Install Visual Studio 2022 with Desktop development with C++ and CMake.'
    }
    $vsRoot = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vsRoot) {
        throw 'Visual Studio 2022 C++ tools were not found.'
    }
    $cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'CMake was not found.'
}

& $cmake -S $projectRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) {
    throw 'CMake configure failed.'
}

& $cmake --build $buildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw 'Build failed.'
}

if ($Test) {
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
        throw 'CTest was not found next to CMake.'
    }
    & $ctest --test-dir $buildPath -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw 'Tests failed.'
    }
}
