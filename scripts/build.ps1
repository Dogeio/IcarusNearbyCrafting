[CmdletBinding()]
param(
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Configuration = 'Game__Shipping__Win64',

    [switch]$DebugLogging,

    [ValidateRange(1, 64)]
    [int]$CompilerProcesses = 2
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$UE4SSRoot = if ($env:UE4SS_RUNTIME_ROOT) { $env:UE4SS_RUNTIME_ROOT } else { Join-Path (Split-Path -Parent $ProjectRoot) '.ue4ss-runtime' }
$UE4SSVersionFile = Join-Path $ProjectRoot 'third_party\UE4SS_COMMIT'
$BuildRoot = Join-Path $ProjectRoot 'build'
if ($DebugLogging) {
    $PackageRoot = Join-Path $ProjectRoot 'dist\NearbyCrafting-Debug'
    $DebugLoggingValue = 'ON'
} else {
    $PackageRoot = Join-Path $ProjectRoot 'dist\NearbyCrafting-Release'
    $DebugLoggingValue = 'OFF'
}
$BuildPath = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $BuildPath

$GitEnvironmentNames = @(
    'GIT_CONFIG_GLOBAL',
    'GIT_CONFIG_COUNT',
    'GIT_CONFIG_KEY_0',
    'GIT_CONFIG_VALUE_0',
    'GIT_CONFIG_KEY_1',
    'GIT_CONFIG_VALUE_1'
)
$OriginalGitEnvironment = @{}
foreach ($name in $GitEnvironmentNames) {
    $OriginalGitEnvironment[$name] = [PSCustomObject]@{
        Exists = Test-Path -LiteralPath "Env:$name"
        Value = [System.Environment]::GetEnvironmentVariable($name, 'Process')
    }
}
$OriginalCompilerProcessLimit = [PSCustomObject]@{
    Exists = Test-Path -LiteralPath 'Env:CL_MPCount'
    Value = [System.Environment]::GetEnvironmentVariable('CL_MPCount', 'Process')
}

try {
    if (-not (Test-Path -LiteralPath (Join-Path $UE4SSRoot 'build\Game__Shipping__Win64\lib\UE4SS.lib'))) {
        throw "UE4SS is missing. Run scripts\\setup-ue4ss.ps1 first: $UE4SSRoot"
    }
    [System.Environment]::SetEnvironmentVariable(
        'CL_MPCount',
        $CompilerProcesses.ToString([System.Globalization.CultureInfo]::InvariantCulture),
        'Process')
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_GLOBAL', 'NUL', 'Process')
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_COUNT', '2', 'Process')
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_KEY_0', 'safe.directory', 'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_VALUE_0',
        (Join-Path $UE4SSRoot 'source').Replace('\', '/'),
        'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_KEY_1', 'url.https://github.com/.insteadOf', 'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_VALUE_1', 'git@github.com:', 'Process')

    cmake -S $ProjectRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 `
        "-DUE4SS_RUNTIME_ROOT=$UE4SSRoot" `
        "-DNEARBYCRAFTING_DEBUG_LOGGING=$DebugLoggingValue" `
        "-DNEARBYCRAFTING_PACKAGE_ROOT=$PackageRoot"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    $ModConfiguration = if ($Configuration -eq 'Game__Shipping__Win64') { 'Release' } else { 'Debug' }
    cmake --build $BuildRoot --config $ModConfiguration --target NearbyCraftingPackage
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE."
    }

    $DllPath = Join-Path $PackageRoot 'dlls\main.dll'
    if (-not (Test-Path -LiteralPath $DllPath)) {
        throw "Build completed without producing $DllPath."
    }

    $ProxyDllPath = Join-Path $UE4SSRoot 'build\Game__Shipping__Win64\bin\dwmapi.dll'
    if (-not (Test-Path -LiteralPath $ProxyDllPath)) {
        throw "Build completed without producing $ProxyDllPath."
    }
    $UE4SSDllPath = Join-Path $UE4SSRoot 'build\Game__Shipping__Win64\bin\UE4SS.dll'
    if (-not (Test-Path -LiteralPath $UE4SSDllPath)) {
        throw "Build completed without producing $UE4SSDllPath."
    }

    if (-not (Test-Path -LiteralPath $UE4SSVersionFile)) {
        throw "UE4SS version pin is missing: $UE4SSVersionFile"
    }
    $ExpectedUE4SSCommit = [System.IO.File]::ReadAllText($UE4SSVersionFile).Trim()
    $ActualUE4SSCommit = (git -C (Join-Path $UE4SSRoot 'source') `
        -c "safe.directory=$((Join-Path $UE4SSRoot 'source').Replace('\', '/'))" rev-parse HEAD |
        Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($ActualUE4SSCommit)) {
        throw "Could not read the pinned UE4SS commit from $UE4SSRoot."
    }
    $ActualUE4SSCommit = $ActualUE4SSCommit.Trim()
    if ($ActualUE4SSCommit -ne $ExpectedUE4SSCommit) {
        throw "UE4SS checkout mismatch. Expected $ExpectedUE4SSCommit, got $ActualUE4SSCommit."
    }
    Write-Host "NearbyCrafting package ready at $PackageRoot"
    Write-Host "Shared UE4SS runtime ready at $ProxyDllPath"
}
finally {
    $compilerProcessLimit = if ($OriginalCompilerProcessLimit.Exists) {
        $OriginalCompilerProcessLimit.Value
    } else {
        $null
    }
    [System.Environment]::SetEnvironmentVariable(
        'CL_MPCount', $compilerProcessLimit, 'Process')
    foreach ($name in $GitEnvironmentNames) {
        $original = $OriginalGitEnvironment[$name]
        $value = if ($original.Exists) { $original.Value } else { $null }
        [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
