<#
.SYNOPSIS
  Local developer build helper: configures and builds Fates with the bundled
  Visual Studio MSVC toolchain + Ninja, without requiring a Developer Prompt.

.EXAMPLE
  pwsh -File scripts/dev-build.ps1 -BuildDir build-dev -Avx2
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-dev",
    [switch]$Avx2,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw "No Visual Studio installation with the C++ toolset was found" }

$cmake = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake).Source }
if (-not (Test-Path $ninja)) { $ninja = (Get-Command ninja).Source }

# Import the x64 native environment once per process.
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
$envDump = & "$env:ComSpec" /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

$buildPath = Join-Path $repo $BuildDir
if ($Clean -and (Test-Path $buildPath)) { Remove-Item -Recurse -Force $buildPath }

$configureArgs = @(
    "-S", $repo, "-B", $buildPath, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MAKE_PROGRAM=$ninja"
)
if ($Avx2) { $configureArgs += "-DFATES_ENABLE_AVX2=ON" }

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& $cmake --build $buildPath --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

Write-Host "built: $(Join-Path $buildPath 'fates.exe')"
