[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ReadmePath = Join-Path $ProjectRoot 'README.md'
$OutputPath = Join-Path $ProjectRoot 'dist\MOD_DESCRIPTION.md'
$RepositoryUrl = 'https://github.com/Dogeio/IcarusNearbyCrafting'

function Convert-InlineMarkdown([string]$Text) {
    if ($Text -match '^\*\*`([^`]+)`\*\*$') {
        return "[color=#c27ba0][b]$($Matches[1])[/b][/color]"
    }
    if ($Text -match '^\*\*([^*]+)\*\*$') {
        return "[color=#ff0000][size=4]$(Convert-InlineMarkdown $Matches[1])[/size][/color]"
    }
    $Text = [regex]::Replace($Text, '\[([^\]]+)\]\(([^)]+)\)', {
        param($match)
        $target = $match.Groups[2].Value
        if ($target -match '^https?://') {
            return "[url=$target]$($match.Groups[1].Value)[/url]"
        }
        if ($target -notmatch '^#' -and $target -notmatch '^[a-z][a-z0-9+.-]*:') {
            $repositoryTarget = $target.TrimStart('.', '/').Replace('\', '/')
            return "[url=$RepositoryUrl/blob/main/$repositoryTarget]$($match.Groups[1].Value)[/url]"
        }
        "[color=#c27ba0]$($match.Groups[1].Value)[/color]"
    })
    $Text = [regex]::Replace($Text, '\*\*`([^`]+)`\*\*([.,;:!?]?)', '[color=#c27ba0][b]$1[/b]$2[/color]')
    $Text = [regex]::Replace($Text, '`([^`]+)`', '[color=#c27ba0]$1[/color]')
    $Text = [regex]::Replace($Text, '\*\*([^*]+)\*\*', '[color=#ff0000]$1[/color]')
    [regex]::Replace($Text, '(?<!\*)\*([^*]+)\*(?!\*)', '[color=#c27ba0]$1[/color]')
}

if (-not (Test-Path -LiteralPath $ReadmePath)) { throw "README is missing: $ReadmePath" }
$lines = [System.IO.File]::ReadAllLines($ReadmePath)
$firstSection = [array]::FindIndex($lines, [Predicate[string]]{ param($line) $line -match '^##\s+\S' })
if ($firstSection -lt 0) { throw "README must contain a level-two section: $ReadmePath" }

$output = [System.Collections.Generic.List[string]]::new()
$inCode = $false
$listOpen = $false
for ($index = $firstSection; $index -lt $lines.Count; $index++) {
    $line = $lines[$index]
    if ($line -match '^```') {
        if ($listOpen) { $output[$output.Count - 1] += '[/list]'; $listOpen = $false }
        if ($inCode) { $output[$output.Count - 1] = "$($output[$output.Count - 1])[/code]" }
        else {
            $index++
            if ($index -ge $lines.Count) { throw "Empty code block in $ReadmePath" }
            $output.Add("[code]$($lines[$index])")
        }
        $inCode = -not $inCode
        continue
    }
    if ($inCode) { $output.Add($line); continue }
    if ($line -match '^#{2,3}\s+(.+)$') {
        if ($listOpen) { $output[$output.Count - 1] += '[/list]'; $output.Add(''); $listOpen = $false }
        if ($output.Count -gt 1) { $output.Add('[line]'); $output.Add('') }
        $output.Add("[size=5]$(Convert-InlineMarkdown $Matches[1])[/size]")
        continue
    }
    if ($line -match '^(\d+\.|-)\s+(.+)$') {
        $listPrefix = ''
        if (-not $listOpen) { $listPrefix = $(if ($Matches[1] -eq '-') { '[list]' } else { '[list=1]' }); $listOpen = $true }
        $itemText = $Matches[2]
        while ($index + 1 -lt $lines.Count -and $lines[$index + 1] -match '^\s{2,}\S') {
            $index++
            $itemText += "`n$($lines[$index].Trim())"
        }
        $output.Add("$listPrefix[*]$(Convert-InlineMarkdown $itemText)[/*]")
        continue
    }
    if ($listOpen) {
        $output[$output.Count - 1] += '[/list]'
        $listOpen = $false
        if ([string]::IsNullOrWhiteSpace($line)) {
            $output.Add('')
            continue
        }
    }
    $output.Add($(if ([string]::IsNullOrWhiteSpace($line)) { '' } else { Convert-InlineMarkdown $line.Trim() }))
}
if ($inCode) { throw "Unclosed code block in $ReadmePath" }
if ($listOpen) { $output[$output.Count - 1] += '[/list]' }
while ($output.Count -gt 0 -and [string]::IsNullOrWhiteSpace($output[$output.Count - 1])) { $output.RemoveAt($output.Count - 1) }
$output[0] = "[font=Verdana]$($output[0])"
$output[$output.Count - 1] += '[/font]'
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
[System.IO.File]::WriteAllText($OutputPath, (($output -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
Write-Host "Updated $OutputPath"
