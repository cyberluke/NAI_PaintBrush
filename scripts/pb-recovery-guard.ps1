# pb-recovery-guard.ps1 — isolate and restore NAI Paintbrush recovery files
#
#   pwsh scripts/pb-recovery-guard.ps1 -Action backup
#   pwsh scripts/pb-recovery-guard.ps1 -Action restore
#   pwsh scripts/pb-recovery-guard.ps1 -Action status
#
# `backup` copies the two recovery.npb files into the sandbox backup directory,
# renames the originals to recovery.npb.bak-test so the test launch cannot load
# a stale document, and records the original SHA-256 values in manifest.json.
# `restore` puts them back exactly and fails loudly if a hash does not match.
# It never leaves the original recovery files missing.

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('backup', 'restore', 'status')]
    [string]$Action
)

$ErrorActionPreference = 'Stop'
$backupDir = Join-Path $env:LOCALAPPDATA 'Temp\kilo\pb-recovery-backup'
$manifestPath = Join-Path $backupDir 'manifest.json'

$targets = @(
    [pscustomobject]@{
        Tag  = 'nai-paintbrush'
        Path = Join-Path $env:APPDATA 'nai-paintbrush\paintbrush\recovery.npb'
    },
    [pscustomobject]@{
        Tag  = 'paintbrush'
        Path = Join-Path $env:APPDATA 'paintbrush\paintbrush\recovery.npb'
    }
)

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Write-Manifest($Entries) {
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    $Entries | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
}

function Read-Manifest {
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "No recovery manifest at $manifestPath"
    }
    return @(Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json)
}

if ($Action -eq 'backup') {
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    $existing = @()
    foreach ($target in $targets) {
        $path = $target.Path
        $inPlace = "$path.bak-test"
        if ((Test-Path -LiteralPath $inPlace) -and (Test-Path -LiteralPath $path)) {
            $liveHash = Get-Sha256 $path
            $staleHash = Get-Sha256 $inPlace
            if ($liveHash -ne $staleHash) {
                throw "Both $path and $inPlace exist with different hashes; refusing to overwrite."
            }
            # A previous interrupted run left an identical copy. It carries no
            # information beyond the live file, so remove it and proceed.
            Remove-Item -LiteralPath $inPlace -Force
        }
        if (Test-Path -LiteralPath $path) {
            $hash = Get-Sha256 $path
            $copy = Join-Path $backupDir ("{0}-recovery.npb.bak-test" -f $target.Tag)
            Copy-Item -LiteralPath $path -Destination $copy -Force
            $copyHash = Get-Sha256 $copy
            if ($copyHash -ne $hash) {
                throw "Backup copy hash mismatch for $path"
            }
            Rename-Item -LiteralPath $path -NewName 'recovery.npb.bak-test'
            $movedHash = Get-Sha256 $inPlace
            if ($movedHash -ne $hash) {
                throw "Renamed recovery hash mismatch for $path"
            }
            $existing += [pscustomobject]@{
                Tag      = $target.Tag
                Path     = $path
                InPlace  = $inPlace
                Backup   = $copy
                Length   = (Get-Item -LiteralPath $inPlace).Length
                Sha256   = $hash
                Restored = $false
            }
        } elseif (Test-Path -LiteralPath $inPlace) {
            $existing += [pscustomobject]@{
                Tag      = $target.Tag
                Path     = $path
                InPlace  = $inPlace
                Backup   = $null
                Length   = (Get-Item -LiteralPath $inPlace).Length
                Sha256   = (Get-Sha256 $inPlace)
                Restored = $false
            }
        } else {
            $existing += [pscustomobject]@{
                Tag      = $target.Tag
                Path     = $path
                InPlace  = $inPlace
                Backup   = $null
                Length   = 0
                Sha256   = $null
                Restored = $true
            }
        }
    }
    Write-Manifest $existing
    Write-Output "Backup complete: $backupDir"
    $existing | Format-Table Tag, Length, Sha256 -AutoSize | Out-String | Write-Output
    exit 0
}

if ($Action -eq 'restore') {
    $manifest = Read-Manifest
    foreach ($entry in $manifest) {
        if ($entry.Restored) {
            continue
        }
        $path = [string]$entry.Path
        $inPlace = [string]$entry.InPlace
        $source = if (Test-Path -LiteralPath $inPlace) { $inPlace } else { $null }
        if (-not $source) {
            if ($entry.Backup -and (Test-Path -LiteralPath $entry.Backup)) {
                $source = [string]$entry.Backup
                Copy-Item -LiteralPath $source -Destination $inPlace -Force
                $source = $inPlace
            } else {
                throw "Recovery source missing for $path (checked $inPlace and backup)"
            }
        }
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
        Rename-Item -LiteralPath $source -NewName 'recovery.npb'
        $hash = Get-Sha256 $path
        if ($hash -ne [string]$entry.Sha256) {
            throw "Restored recovery hash mismatch for $path`: $hash != $($entry.Sha256)"
        }
        $entry.Restored = $true
    }
    Write-Manifest $manifest
    Write-Output "Recovery restore complete."
    foreach ($entry in $manifest) {
        if ([string]$entry.Sha256) {
            Write-Output ("{0}: {1} bytes SHA256 {2}" -f $entry.Tag, $entry.Length, $entry.Sha256)
        }
    }
    exit 0
}

# status
foreach ($target in $targets) {
    $path = $target.Path
    $inPlace = "$path.bak-test"
    $state = if (Test-Path -LiteralPath $path) {
        "present {0} bytes SHA256 {1}" -f (Get-Item -LiteralPath $path).Length, (Get-Sha256 $path)
    } else {
        'MISSING'
    }
    $testState = if (Test-Path -LiteralPath $inPlace) { 'isolated .bak-test present' } else { 'no .bak-test' }
    Write-Output ("{0}: {1}; {2}" -f $target.Tag, $state, $testState)
}
if (Test-Path -LiteralPath $manifestPath) {
    Write-Output "manifest: $manifestPath"
}
exit 0
