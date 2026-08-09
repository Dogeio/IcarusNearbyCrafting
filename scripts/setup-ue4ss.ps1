[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$RuntimeRoot = if ($env:UE4SS_RUNTIME_ROOT) { $env:UE4SS_RUNTIME_ROOT } else { Join-Path (Split-Path -Parent $ProjectRoot) '.ue4ss-runtime' }
$SourceRoot = Join-Path $RuntimeRoot 'source'
$BuildRoot = Join-Path $RuntimeRoot 'build'
$VersionFile = Join-Path $ProjectRoot 'third_party\UE4SS_COMMIT'
$UserProfileRoot = if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
    $env:USERPROFILE
} else {
    [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::UserProfile)
}
$CargoHomeRoot = if (-not [string]::IsNullOrWhiteSpace($env:CARGO_HOME)) {
    $env:CARGO_HOME
} else {
    Join-Path $UserProfileRoot '.cargo'
}
$RustupHomeRoot = if (-not [string]::IsNullOrWhiteSpace($env:RUSTUP_HOME)) {
    $env:RUSTUP_HOME
} else {
    Join-Path $UserProfileRoot '.rustup'
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
$ExpectedCommit = [System.IO.File]::ReadAllText($VersionFile).Trim()
if ($ExpectedCommit -notmatch '^[0-9a-f]{40}$') { throw "UE4SS version pin is invalid: $VersionFile" }
$OriginalRustFlags = $env:RUSTFLAGS
try {
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_GLOBAL', 'NUL', 'Process')
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_COUNT', '2', 'Process')
    [System.Environment]::SetEnvironmentVariable('GIT_CONFIG_KEY_0', 'safe.directory', 'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_VALUE_0', $SourceRoot.Replace('\', '/'), 'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_KEY_1', 'url.https://github.com/.insteadOf', 'Process')
    [System.Environment]::SetEnvironmentVariable(
        'GIT_CONFIG_VALUE_1', 'git@github.com:', 'Process')

    if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot '.git'))) {
        New-Item -ItemType Directory -Force -Path $RuntimeRoot | Out-Null
        git clone --filter=blob:none --no-checkout https://github.com/UE4SS-RE/RE-UE4SS.git $SourceRoot
        if ($LASTEXITCODE -ne 0) { throw "Could not clone UE4SS into $SourceRoot." }
    }
    git -C $SourceRoot -c "safe.directory=$($SourceRoot.Replace('\', '/'))" `
        cat-file -e "$ExpectedCommit`^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0) {
        git -C $SourceRoot -c "safe.directory=$($SourceRoot.Replace('\', '/'))" `
            fetch origin $ExpectedCommit
        if ($LASTEXITCODE -ne 0) { throw "Could not fetch pinned UE4SS commit $ExpectedCommit." }
    }
    git -C $SourceRoot -c "safe.directory=$($SourceRoot.Replace('\', '/'))" `
        checkout --detach $ExpectedCommit
    if ($LASTEXITCODE -ne 0) { throw "Could not check out pinned UE4SS commit $ExpectedCommit." }
    git -C $SourceRoot -c "safe.directory=$($SourceRoot.Replace('\', '/'))" `
        submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw 'Could not prepare UE4SS submodules.' }

    $RustRemapFlags = @(
        "--remap-path-prefix=$UserProfileRoot=user",
        "--remap-path-prefix=$CargoHomeRoot=cargo",
        "--remap-path-prefix=$RustupHomeRoot=rustup",
        "--remap-path-prefix=$RuntimeRoot=ue4ss"
    )
    $env:RUSTFLAGS = (@($OriginalRustFlags) + $RustRemapFlags |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join ' '

    cmake -S $SourceRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 `
        -DFETCHCONTENT_UPDATES_DISCONNECTED=ON `
        -DCMAKE_EXE_LINKER_FLAGS='/Brepro /PDBALTPATH:%_PDB%' `
        -DCMAKE_SHARED_LINKER_FLAGS='/Brepro /PDBALTPATH:%_PDB%'
    if ($LASTEXITCODE -ne 0) { throw "UE4SS CMake configuration failed with exit code $LASTEXITCODE." }
    cmake --build $BuildRoot --config Game__Shipping__Win64 --target UE4SS proxy
    if ($LASTEXITCODE -ne 0) { throw "UE4SS build failed with exit code $LASTEXITCODE." }
}
finally {
    $env:RUSTFLAGS = $OriginalRustFlags
    foreach ($name in $GitEnvironmentNames) {
        $original = $OriginalGitEnvironment[$name]
        $value = if ($original.Exists) { $original.Value } else { $null }
        [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
$RuntimeBin = Join-Path $BuildRoot 'Game__Shipping__Win64\bin'
if (-not (Test-Path -LiteralPath (Join-Path $RuntimeBin 'UE4SS.dll'))) { throw "Shared UE4SS build failed: $RuntimeBin" }

foreach ($binaryName in @('UE4SS.dll', 'dwmapi.dll')) {
    $binaryPath = Join-Path $RuntimeBin $binaryName
    if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        throw "Shared UE4SS build is incomplete: $binaryPath"
    }
    $bytes = [System.IO.File]::ReadAllBytes($binaryPath)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [System.Text.Encoding]::Unicode.GetString($bytes)
    $privatePathMarkers = @(
        $UserProfileRoot,
        $RuntimeRoot,
        'C:\Users\',
        [System.IO.Path]::GetFileName($RuntimeRoot),
        [System.IO.Path]::GetFileName((Split-Path -Parent $ProjectRoot))
    )
    foreach ($privateRoot in $privatePathMarkers) {
        foreach ($candidate in @($privateRoot, $privateRoot.Replace('\', '/'))) {
            if ($ascii.Contains($candidate) -or $unicode.Contains($candidate)) {
                throw "Shared UE4SS binary contains a private build path: $binaryPath"
            }
        }
    }
}
Write-Host "Shared UE4SS runtime ready at $RuntimeBin"
