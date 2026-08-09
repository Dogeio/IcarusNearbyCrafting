[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $ProjectRoot 'CMakeLists.txt'
$UE4SSRoot = if ($env:UE4SS_RUNTIME_ROOT) { $env:UE4SS_RUNTIME_ROOT } else { Join-Path (Split-Path -Parent $ProjectRoot) '.ue4ss-runtime' }
$UE4SSVersionFile = Join-Path $ProjectRoot 'third_party\UE4SS_COMMIT'
$UE4SSSourceRoot = Join-Path $UE4SSRoot 'source'
$UE4SSAssetsRoot = Join-Path $UE4SSSourceRoot 'assets'
$UE4SSRuntimeBin = Join-Path $UE4SSRoot 'build\Game__Shipping__Win64\bin'
$ModName = 'NearbyCrafting'
$ModsListPath = Join-Path $ProjectRoot 'package\Mods\mods.txt'
$ModReleaseDocuments = @(
    [PSCustomObject]@{ Source = Join-Path $ProjectRoot 'README.md'; Name = 'README.md' },
    [PSCustomObject]@{ Source = Join-Path $ProjectRoot 'LICENSE'; Name = 'LICENSE' },
    [PSCustomObject]@{ Source = Join-Path $ProjectRoot 'THIRD_PARTY_NOTICES.md'; Name = 'THIRD_PARTY_NOTICES.md' }
)
$UE4SSReleaseDocuments = @(
    [PSCustomObject]@{ Source = Join-Path $UE4SSSourceRoot 'README.md'; Name = 'UE4SS-README.md' },
    [PSCustomObject]@{ Source = Join-Path $UE4SSSourceRoot 'LICENSE'; Name = 'UE4SS-LICENSE' },
    [PSCustomObject]@{ Source = Join-Path $UE4SSAssetsRoot 'Changelog.md'; Name = 'UE4SS-Changelog.md' }
)

function Assert-ArchiveContainsNoPrivatePaths(
    [string]$ArchivePath,
    [string[]]$PrivateRoots
) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.Length -eq 0) { continue }
            $entryStream = $entry.Open()
            try {
                $memory = [System.IO.MemoryStream]::new()
                $entryStream.CopyTo($memory)
                $bytes = $memory.ToArray()
            }
            finally {
                $entryStream.Dispose()
                if ($null -ne $memory) { $memory.Dispose() }
            }
            $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
            $unicode = [System.Text.Encoding]::Unicode.GetString($bytes)
            foreach ($privateRoot in $PrivateRoots) {
                if ([string]::IsNullOrWhiteSpace($privateRoot)) { continue }
                foreach ($candidate in @($privateRoot, $privateRoot.Replace('\', '/'))) {
                    if ($ascii.IndexOf($candidate, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                        $unicode.IndexOf($candidate, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                        throw "Release archive contains a private build path in '$($entry.FullName)': $ArchivePath"
                    }
                }
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Set-IniValue([string]$Text, [string]$Key, [string]$Value) {
    $pattern = "(?m)^(\s*$([regex]::Escape($Key))\s*=\s*).*$"
    $matches = [regex]::Matches($Text, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one '$Key' setting in the pinned UE4SS configuration; found $($matches.Count)."
    }
    [regex]::Replace($Text, $pattern, {
        param($match)
        $match.Groups[1].Value + $Value
    })
}

function New-ReleaseUE4SSSettings([string]$SourcePath, [string]$DestinationPath) {
    $settings = [System.IO.File]::ReadAllText($SourcePath)
    if ([regex]::IsMatch($settings, '(?m)^\+ModsFolderPaths\s*=')) {
        throw "Pinned UE4SS configuration already defines +ModsFolderPaths; update the release generator."
    }
    $modsFolderPattern = '(?m)^ModsFolderPath\s*=\s*$'
    if ([regex]::Matches($settings, $modsFolderPattern).Count -ne 1) {
        throw 'Could not locate the default ModsFolderPath setting in the pinned UE4SS configuration.'
    }
    $settings = [regex]::Replace(
        $settings,
        $modsFolderPattern,
        "ModsFolderPath =`r`n+ModsFolderPaths = ../Mods")

    $releaseValues = [ordered]@{
        EnableHotReloadSystem = '0'
        EnableAutoReloadingLuaMods = '0'
        UseCache = '1'
        InvalidateCacheIfDLLDiffers = '1'
        bUseUObjectArrayCache = 'false'
        DoEarlyScan = '0'
        LoadAllAssetsBeforeDumpingObjects = '0'
        LoadAllAssetsBeforeGeneratingCXXHeaders = '0'
        ConsoleEnabled = '0'
        GuiConsoleEnabled = '0'
        GuiConsoleVisible = '0'
        GraphicsAPI = 'dx11'
        EnableDumping = '1'
        FullMemoryDump = '0'
    }
    foreach ($entry in $releaseValues.GetEnumerator()) {
        $settings = Set-IniValue $settings $entry.Key $entry.Value
    }
    $settings = [regex]::Replace($settings, '\r?\n', "`r`n")
    [System.IO.File]::WriteAllText(
        $DestinationPath,
        $settings,
        [System.Text.UTF8Encoding]::new($false))
}

& (Join-Path $PSScriptRoot 'generate-mod-description.ps1')
$ModSourceRoot = Join-Path $ProjectRoot 'dist\NearbyCrafting-Release'

if (-not (Test-Path -LiteralPath $ModSourceRoot)) {
    throw "Built mod package is missing. Run scripts\build.ps1 first: $ModSourceRoot"
}

foreach ($document in $ModReleaseDocuments) {
    if (-not (Test-Path -LiteralPath $document.Source)) {
        throw "Mod release document is missing: $($document.Source)"
    }
    Copy-Item -LiteralPath $document.Source `
        -Destination (Join-Path $ModSourceRoot $document.Name) -Force
}

$projectDefinition = [System.IO.File]::ReadAllText($ProjectFile)
$projectVersionMatch = [System.Text.RegularExpressions.Regex]::Match(
    $projectDefinition,
    'project\s*\(\s*NearbyCrafting\s+VERSION\s+(\d+\.\d+\.\d+)',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
if (-not $projectVersionMatch.Success) {
    throw "Could not read the NearbyCrafting version from $ProjectFile."
}

$ProjectVersion = $projectVersionMatch.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $ProjectVersion
} elseif ($Version -ne $ProjectVersion) {
    throw "Requested release version $Version does not match project version $ProjectVersion."
}

$ExpectedRuntimeFiles = @(
    (Join-Path $UE4SSRuntimeBin 'dwmapi.dll'),
    (Join-Path $UE4SSRuntimeBin 'UE4SS.dll'),
    (Join-Path $UE4SSAssetsRoot 'UE4SS-settings.ini'),
    $ModsListPath
)
foreach ($path in $ExpectedRuntimeFiles) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Pinned UE4SS runtime is incomplete. Run scripts\setup-ue4ss.ps1 first: $path"
    }
}
foreach ($document in $UE4SSReleaseDocuments) {
    if (-not (Test-Path -LiteralPath $document.Source)) {
        throw "Pinned UE4SS release document is missing: $($document.Source)"
    }
}
if (-not (Test-Path -LiteralPath $UE4SSVersionFile)) {
    throw "UE4SS version pin is missing: $UE4SSVersionFile"
}
$ExpectedUE4SSCommit = [System.IO.File]::ReadAllText($UE4SSVersionFile).Trim()
$ActualUE4SSCommit = (git -C $UE4SSSourceRoot `
    -c "safe.directory=$($UE4SSSourceRoot.Replace('\', '/'))" rev-parse HEAD |
    Select-Object -Last 1)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($ActualUE4SSCommit)) {
    throw "Could not read the pinned UE4SS commit from $UE4SSSourceRoot."
}
$ActualUE4SSCommit = $ActualUE4SSCommit.Trim()
if ($ActualUE4SSCommit -ne $ExpectedUE4SSCommit) {
    throw "UE4SS checkout mismatch. Expected $ExpectedUE4SSCommit, got $ActualUE4SSCommit."
}
$ModsListEntries = @([System.IO.File]::ReadAllLines($ModsListPath) |
    ForEach-Object { $_.Trim() } |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$ExpectedModsListEntry = "$ModName : 1"
if ($ModsListEntries.Count -ne 1 -or $ModsListEntries[0] -cne $ExpectedModsListEntry) {
    throw "Release mods.txt must contain only '$ExpectedModsListEntry'."
}

$StandaloneArchivePath = Join-Path $ProjectRoot "dist\NearbyCrafting-v$Version-Standalone.zip"
$FullArchivePath = Join-Path $ProjectRoot "dist\NearbyCrafting-v$Version.zip"
$StandaloneStagingRoot = Join-Path $ProjectRoot "dist\.NearbyCrafting-standalone-$([guid]::NewGuid().ToString('N'))"
$ReleaseStagingRoot = Join-Path $ProjectRoot "dist\.NearbyCrafting-release-$([guid]::NewGuid().ToString('N'))"

try {
    $StagedStandaloneModRoot = Join-Path $StandaloneStagingRoot $ModName
    New-Item -ItemType Directory -Path $StandaloneStagingRoot -Force | Out-Null
    Copy-Item -LiteralPath $ModSourceRoot -Destination $StagedStandaloneModRoot -Recurse
    if (Test-Path -LiteralPath $StandaloneArchivePath) {
        Remove-Item -LiteralPath $StandaloneArchivePath -Force
    }
    Compress-Archive -Path $StagedStandaloneModRoot `
        -DestinationPath $StandaloneArchivePath -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $StandaloneStagingRoot) {
        Remove-Item -LiteralPath $StandaloneStagingRoot -Recurse -Force
    }
}

try {
    $StagedUE4SSRoot = Join-Path $ReleaseStagingRoot 'ue4ss'
    $StagedModsRoot = Join-Path $StagedUE4SSRoot 'Mods'
    New-Item -ItemType Directory -Path $StagedModsRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $UE4SSRuntimeBin 'dwmapi.dll') `
        -Destination (Join-Path $ReleaseStagingRoot 'dwmapi.dll')
    Copy-Item -LiteralPath (Join-Path $UE4SSRuntimeBin 'UE4SS.dll') `
        -Destination (Join-Path $StagedUE4SSRoot 'UE4SS.dll')
    foreach ($document in $UE4SSReleaseDocuments) {
        Copy-Item -LiteralPath $document.Source `
            -Destination (Join-Path $StagedUE4SSRoot $document.Name)
    }
    New-ReleaseUE4SSSettings `
        (Join-Path $UE4SSAssetsRoot 'UE4SS-settings.ini') `
        (Join-Path $StagedUE4SSRoot 'UE4SS-settings.ini')
    Copy-Item -LiteralPath $ModsListPath -Destination (Join-Path $StagedModsRoot 'mods.txt')
    Copy-Item -LiteralPath $ModSourceRoot `
        -Destination (Join-Path $StagedModsRoot $ModName) -Recurse

    if (Test-Path -LiteralPath $FullArchivePath) {
        Remove-Item -LiteralPath $FullArchivePath -Force
    }
    Compress-Archive -Path (Join-Path $ReleaseStagingRoot '*') `
        -DestinationPath $FullArchivePath -CompressionLevel Optimal
}
finally {
    if (Test-Path -LiteralPath $ReleaseStagingRoot) {
        Remove-Item -LiteralPath $ReleaseStagingRoot -Recurse -Force
    }
}

$PrivateBuildRoots = @(
    $env:USERPROFILE,
    [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::UserProfile),
    'C:\Users\',
    $ProjectRoot,
    $UE4SSRoot
)
Assert-ArchiveContainsNoPrivatePaths $StandaloneArchivePath $PrivateBuildRoots
Assert-ArchiveContainsNoPrivatePaths $FullArchivePath $PrivateBuildRoots

Write-Host "Full release ready: $FullArchivePath"
Write-Host "Standalone release ready: $StandaloneArchivePath"
