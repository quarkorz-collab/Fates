param(
    [string]$Python = 'python',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$frontendDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDirectory = Split-Path -Parent $frontendDirectory
if (-not $OutputDirectory) {
    $OutputDirectory = $projectDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$buildDirectory = Join-Path $temporaryRoot ("fates-web-build-" + [guid]::NewGuid().ToString('N'))
$packageDirectory = Join-Path $buildDirectory 'packages'
$distributionDirectory = Join-Path $buildDirectory 'dist'
$workDirectory = Join-Path $buildDirectory 'work'
$requirementsPath = Join-Path $frontendDirectory 'requirements-build.txt'

try {
    New-Item -ItemType Directory -Path $packageDirectory, $distributionDirectory, $workDirectory -Force | Out-Null
    & $Python -m pip install --disable-pip-version-check --no-warn-script-location `
        --target $packageDirectory --requirement $requirementsPath
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'The configured pip index did not provide PyInstaller; retrying official PyPI.'
        & $Python -m pip install --disable-pip-version-check --no-warn-script-location `
            --index-url 'https://pypi.org/simple' `
            --target $packageDirectory --requirement $requirementsPath
    }
    if ($LASTEXITCODE -ne 0) { throw 'Failed to install the isolated PyInstaller build dependency.' }

    $previousPythonPath = $env:PYTHONPATH
    $env:PYTHONPATH = if ($previousPythonPath) { "$packageDirectory;$previousPythonPath" } else { $packageDirectory }
    try {
        & $Python -m PyInstaller --noconfirm --clean --onefile --console `
            --name fates-web `
            --distpath $distributionDirectory `
            --workpath $workDirectory `
            --specpath $buildDirectory `
            --add-data "$frontendDirectory\static;frontend_static" `
            "$frontendDirectory\fates_web.py"
        if ($LASTEXITCODE -ne 0) { throw 'PyInstaller build failed.' }
    }
    finally {
        $env:PYTHONPATH = $previousPythonPath
    }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $sourceExecutable = Join-Path $distributionDirectory 'fates-web.exe'
    $targetExecutable = Join-Path $OutputDirectory 'fates-web.exe'
    Copy-Item -LiteralPath $sourceExecutable -Destination $targetExecutable -Force
    Get-Item -LiteralPath $targetExecutable | Select-Object FullName, Length, LastWriteTime
    Get-FileHash -Algorithm SHA256 -LiteralPath $targetExecutable | Select-Object Algorithm, Hash
}
finally {
    if (Test-Path -LiteralPath $buildDirectory) {
        $resolvedBuild = [IO.Path]::GetFullPath($buildDirectory)
        if ($resolvedBuild.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            $resolvedBuild -ne $temporaryRoot) {
            Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
        }
    }
}
