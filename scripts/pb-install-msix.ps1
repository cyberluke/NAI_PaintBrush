# Install the signed NAI PaintBrush MSIX, provisioning the NAI trust chain.
#
# The MSIX is signed with the product leaf (CN=NAI Paint Brush, ...), whose chain
# must be trusted MACHINE-WIDE for AppX deployment. This script does what the
# NAI OS installer does: it re-launches itself elevated through UAC
# (Start-Process -Verb RunAs), imports the public root/intermediate/leaf into the
# machine stores, and installs the package.
#
#   pwsh scripts/pb-install-msix.ps1                 # newest MSIX in win64-meson\msix\msix
#   pwsh scripts/pb-install-msix.ps1 -Msix <path>
#   pwsh scripts/pb-install-msix.ps1 -TrustOnly      # just provision the chain
#   pwsh scripts/pb-install-msix.ps1 -Uninstall
#
# The build script (pb-build-msix.ps1) never elevates; provisioning lives here,
# per the CA contract (docs/TRUST.md: "Installer (machine-wide, elevated)").
[CmdletBinding()]
param(
    [string]$Msix = "",
    [switch]$TrustOnly,
    [switch]$Uninstall,
    [string]$CaDir = "D:\_SATIN_AI\_NAI_CA_AUTHORITY",
    # internal: set when the elevated child re-enters the script
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$certs = Join-Path $root 'certs'
$msixDir = Join-Path $root 'win64-meson\msix\msix'

# --- elevation -------------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin -and -not $Elevated) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
    if ($Msix) { $argList += @('-Msix', "`"$Msix`"") }
    if ($TrustOnly) { $argList += '-TrustOnly' }
    if ($Uninstall) { $argList += '-Uninstall' }
    $argList += @('-CaDir', "`"$CaDir`"")
    Write-Host "Requesting elevation (UAC) for machine-wide trust + MSIX install..."
    $p = Start-Process -FilePath 'pwsh.exe' -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

# --- uninstall -------------------------------------------------------------
if ($Uninstall) {
    $pkg = Get-AppxPackage -Name 'NAI.NAIPaintBrush' -ErrorAction SilentlyContinue
    if ($pkg) {
        Remove-AppxPackage -Package $pkg.PackageFullName
        Write-Host "Removed $($pkg.PackageFullName)"
    } else {
        Write-Host "NAI.NAIPaintBrush is not installed"
    }
    exit 0
}

# --- trust chain -----------------------------------------------------------
$rootCer = Join-Path $CaDir 'certs\NAI-Sovereign-Root-CA.cer'
$interCer = Join-Path $CaDir 'certs\NAI-Code-Signing-CA.cer'
$leafCer = Join-Path $certs 'NAI-PaintBrush.cer'
foreach ($c in @($rootCer, $interCer, $leafCer)) {
    if (-not (Test-Path -LiteralPath $c)) { throw "Certificate not found: $c" }
}

Write-Host "Provisioning NAI trust chain (machine-wide)"
Import-Certificate -FilePath $rootCer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $interCer -CertStoreLocation 'Cert:\LocalMachine\CA' | Out-Null
Import-Certificate -FilePath $leafCer -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
Write-Host "  root -> LocalMachine\Root"
Write-Host "  intermediate -> LocalMachine\CA"
Write-Host "  product leaf -> LocalMachine\TrustedPeople"

if ($TrustOnly) {
    Write-Host "Trust provisioned (-TrustOnly)."
    exit 0
}

# --- package ---------------------------------------------------------------
if (-not $Msix) {
    $candidate = Get-ChildItem -LiteralPath $msixDir -Filter '*.msix' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) {
        throw "No MSIX found in $msixDir. Build it first: pwsh scripts/pb-build-msix.ps1"
    }
    $Msix = $candidate.FullName
}
$Msix = (Resolve-Path -LiteralPath $Msix).Path

# Same identity from a previous install (or a loose dev registration) must go.
$existing = Get-AppxPackage -Name 'NAI.NAIPaintBrush' -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Removing prior package: $($existing.PackageFullName)"
    Remove-AppxPackage -Package $existing.PackageFullName
    Start-Sleep -Seconds 1
}

Write-Host "Installing $Msix"
Add-AppxPackage -Path $Msix -ErrorAction Stop
$pkg = Get-AppxPackage -Name 'NAI.NAIPaintBrush'
if (-not $pkg) { throw "Add-AppxPackage returned without a registered package" }

Write-Host ""
Write-Host ("Installed: {0} {1}" -f $pkg.Name, $pkg.Version)
Write-Host ("Family:    {0}" -f $pkg.PackageFamilyName)
Write-Host ("Location:  {0}" -f $pkg.InstallLocation)
Write-Host ("Launch:    explorer.exe shell:appsFolder\{0}!App" -f $pkg.PackageFamilyName)
