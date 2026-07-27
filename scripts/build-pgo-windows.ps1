[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [switch]$EnableAVX2,
    [ValidateSet('quick', 'balanced')]
    [string]$Training = 'balanced',
    [ValidateSet('none', 'blend', 'INTEL64', 'AMD64')]
    [string]$Favor = 'blend',
    [switch]$KeepBuildDirectory
)

$ErrorActionPreference = 'Stop'
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDirectory = [IO.Path]::GetFullPath((Join-Path $scriptDirectory '..'))
if (-not $OutputDirectory) {
    $suffix = if ($EnableAVX2) { 'windows-x64-pgo-avx2' } else { 'windows-x64-pgo' }
    $OutputDirectory = Join-Path $projectDirectory "artifacts\$suffix"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$buildDirectory = Join-Path $temporaryRoot ("fates-pgo-" + [guid]::NewGuid().ToString('N'))
$workloadPath = Join-Path $scriptDirectory 'pgo-workloads.json'
$workloadDefinition = Get-Content -LiteralPath $workloadPath -Raw -Encoding UTF8 | ConvertFrom-Json

function Import-MsvcEnvironment {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($cl -and $env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
        return
    }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $vswhere) {
        throw 'Visual Studio Installer (vswhere.exe) was not found.'
    }

    $installationPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installationPath) {
        throw 'A Visual Studio installation with the x64 C++ toolchain was not found.'
    }

    $vcvars = Join-Path $installationPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvars64.bat was not found at $vcvars"
    }

    $environmentLines = & $env:ComSpec /d /s /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw 'vcvars64.bat failed.'
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $separator),
                $line.Substring($separator + 1),
                'Process')
        }
    }
}

function Invoke-NativeStep {
    param(
        [string]$Name,
        [string]$Command,
        [string[]]$ArgumentList
    )

    Write-Host "==> $Name"
    & $Command @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Invoke-TrainingWorkload {
    param(
        [string]$Name,
        [string]$Executable,
        [string[]]$ArgumentList
    )

    Write-Host "==> PGO training: $Name"
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @ArgumentList 1>$null 2>$null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    $stopwatch.Stop()
    if ($exitCode -ne 0) {
        throw "PGO training workload '$Name' failed with exit code $exitCode"
    }
    Write-Host ("    completed in {0:N2}s" -f $stopwatch.Elapsed.TotalSeconds)
}

Import-MsvcEnvironment
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'cl.exe is not available after loading the MSVC environment.'
}
if (-not (Get-Command link.exe -ErrorAction SilentlyContinue)) {
    throw 'link.exe is not available after loading the MSVC environment.'
}

try {
    New-Item -ItemType Directory -Path $buildDirectory, $OutputDirectory -Force | Out-Null

    $sourcePath = Join-Path $projectDirectory 'src\fates.cpp'
    $objectPath = Join-Path $buildDirectory 'fates.obj'
    $compilerPdb = Join-Path $buildDirectory 'fates-compile.pdb'
    $profileDatabase = Join-Path $buildDirectory 'fates-profile.pgd'
    $trainingExecutable = Join-Path $buildDirectory 'fates-train.exe'
    $optimizedExecutable = Join-Path $OutputDirectory 'fates.exe'

    $compileArguments = @(
        '/nologo', '/c', '/std:c++20', '/O2', '/Ob3', '/GL', '/Gw', '/Gy',
        '/fp:precise', '/W4', '/utf-8', '/EHsc', '/MT', '/DNDEBUG'
    )
    if ($EnableAVX2) {
        $compileArguments += '/arch:AVX2'
    }
    if ($Favor -ne 'none') {
        $compileArguments += "/favor:$Favor"
    }
    $compileArguments += @($sourcePath, "/Fo$objectPath", "/Fd$compilerPdb")

    Invoke-NativeStep -Name 'Compile instrumented object' -Command 'cl.exe' `
        -ArgumentList $compileArguments

    $instrumentArguments = @(
        '/nologo', $objectPath, "/OUT:$trainingExecutable",
        '/LTCG:PGINSTRUMENT', '/GENPROFILE', "/PGD:$profileDatabase",
        '/OPT:REF', '/OPT:ICF'
    )
    Invoke-NativeStep -Name 'Link instrumented executable' -Command 'link.exe' `
        -ArgumentList $instrumentArguments

    $pgoRuntime = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\pgort140.dll'
    if (-not (Test-Path -LiteralPath $pgoRuntime)) {
        $pgoRuntime = Get-ChildItem -LiteralPath $env:VCToolsInstallDir `
            -Filter pgort140.dll -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Hostx64\\x64\\pgort140\.dll$' } |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $pgoRuntime -or -not (Test-Path -LiteralPath $pgoRuntime)) {
        throw 'pgort140.dll was not found in the active MSVC toolchain.'
    }
    Copy-Item -LiteralPath $pgoRuntime -Destination $buildDirectory -Force

    $workloads = @($workloadDefinition.base)
    if ($Training -eq 'balanced') {
        $workloads += @($workloadDefinition.balanced_extra)
    }
    foreach ($workload in $workloads) {
        Invoke-TrainingWorkload -Name ([string]$workload.name) -Executable $trainingExecutable `
            -ArgumentList ([string[]]@($workload.arguments))
    }

    $profileFiles = @(Get-ChildItem -LiteralPath $buildDirectory -Filter '*.pgc')
    if ($profileFiles.Count -lt $workloads.Count) {
        throw "Expected at least $($workloads.Count) PGO profile files, found $($profileFiles.Count)."
    }

    $optimizeArguments = @(
        '/nologo', $objectPath, "/OUT:$optimizedExecutable",
        '/LTCG:PGOPTIMIZE', "/PGD:$profileDatabase", '/OPT:REF', '/OPT:ICF'
    )
    Invoke-NativeStep -Name 'Link optimized executable' -Command 'link.exe' `
        -ArgumentList $optimizeArguments

    Copy-Item -LiteralPath $optimizedExecutable `
        -Destination (Join-Path $OutputDirectory 'pries.exe') -Force
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'README.md') `
        -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'LICENSE') `
        -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'THIRD_PARTY_NOTICES.md') `
        -Destination $OutputDirectory -Force

    $licenseDirectory = Join-Path $OutputDirectory 'licenses'
    New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'third_party\unordered_dense\LICENSE') `
        -Destination (Join-Path $licenseDirectory 'unordered_dense-LICENSE') -Force
    Copy-Item -LiteralPath (Join-Path $projectDirectory 'frontend\static\vendor\katex\LICENSE') `
        -Destination (Join-Path $licenseDirectory 'KaTeX-LICENSE') -Force

    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $optimizedExecutable --self-test 1>$null 2>$null
        $finalTestExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($finalTestExitCode -ne 0) {
        throw "Final PGO executable failed self-test with exit code $finalTestExitCode"
    }

    $buildInfo = @(
        'Fates 1.0',
        'toolchain=MSVC',
        "pgo_training=$Training",
        "pgo_profile=$($workloadDefinition.profile)",
        "pgo_workloads_sha256=$((Get-FileHash -LiteralPath $workloadPath -Algorithm SHA256).Hash.ToLowerInvariant())",
        "avx2=$($EnableAVX2.IsPresent)",
        "favor=$Favor"
    )
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'build-info.txt') `
        -Value $buildInfo -Encoding ASCII

    $checksumPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'
    $outputPrefix = $OutputDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $checksumLines = Get-ChildItem -LiteralPath $OutputDirectory -File -Recurse |
        Where-Object { $_.FullName -ne $checksumPath } |
        Sort-Object FullName |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $relativePath = $_.FullName.Substring($outputPrefix.Length).Replace('\', '/')
            "$hash  $relativePath"
        }
    Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding ASCII

    Write-Host "PGO build completed: $OutputDirectory"
    Get-Item -LiteralPath $optimizedExecutable | Select-Object FullName, Length, LastWriteTime
}
finally {
    if (-not $KeepBuildDirectory -and (Test-Path -LiteralPath $buildDirectory)) {
        $resolvedBuild = [IO.Path]::GetFullPath($buildDirectory)
        if ($resolvedBuild.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            $resolvedBuild -ne $temporaryRoot) {
            Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
        }
    }
    elseif ($KeepBuildDirectory) {
        Write-Host "PGO build directory kept at $buildDirectory"
    }
}
