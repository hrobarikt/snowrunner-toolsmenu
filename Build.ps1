[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build'),
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio 2022 Build Tools with the "Desktop development with C++" workload is required.'
}
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($installation)) {
    throw 'Visual Studio 2022 Build Tools with the "Desktop development with C++" workload is required.'
}
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'The Visual Studio CMake component is required.'
}

# Some managed shells provide both Path and PATH. MSBuild copies environment
# variables into a case-insensitive dictionary and rejects that duplicate, so
# normalize it inside this build process before invoking the compiler.
$environment = [Environment]::GetEnvironmentVariables()
$processPath = @($environment.GetEnumerator() | Where-Object { $_.Key -ceq 'Path' } | Select-Object -First 1).Value
if ([string]::IsNullOrWhiteSpace([string]$processPath)) {
    $processPath = @($environment.GetEnumerator() | Where-Object { $_.Key -ieq 'Path' } | Select-Object -First 1).Value
}
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $processPath, 'Process')

& $cmake -S $PSScriptRoot -B $BuildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
& $cmake --build $BuildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
