# Build the 1-click NAI PaintBrush installer (NAI-PaintBrush-Setup.exe).
#
# The setup downloads the signed MSIX from the branded URL (or uses a local file
# passed with -Msix for testing), verifies the SHA-256, provisions the NAI trust
# chain machine-wide (UAC once), installs the package and launches the app.
# csc.exe from .NET Framework is used so the build machine needs no extra SDK.
#
#   pwsh scripts/pb-build-setup.ps1
#   pwsh scripts/pb-build-setup.ps1 -Msix <path> -MsixUrl https://<domain>/<file>.msix
[CmdletBinding()]
param(
    [string]$Msix = "",
    [string]$MsixUrl = "https://s4.nanotrik.ai/nai-paintbrush/1.0.0/NAI.PaintBrush_1.0.0.0_x64.msix",
    [string]$OutDir = "",
    [string]$CaDir = "D:\_SATIN_AI\_NAI_CA_AUTHORITY"
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'win64-meson\msix\setup' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

if (-not $Msix) {
    $candidate = Get-ChildItem -LiteralPath (Join-Path $root 'win64-meson\msix\msix') -Filter '*.msix' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) { throw "No MSIX found; build one first (pb-build-msix.ps1)." }
    $Msix = $candidate.FullName
}
$Msix = (Resolve-Path -LiteralPath $Msix).Path
$hash = (Get-FileHash -LiteralPath $Msix -Algorithm SHA256).Hash
Write-Host ("MSIX:   {0} ({1:N0} MB)" -f $Msix, ((Get-Item $Msix).Length / 1MB))
Write-Host ("SHA256: {0}" -f $hash)
Write-Host ("URL:    {0}" -f $MsixUrl)

# --- certificates to embed ------------------------------------------------
$certs = @(
    @{ Path = Join-Path $CaDir 'certs\NAI-Sovereign-Root-CA.cer'; Name = 'PB.NAICA.Root.cer' },
    @{ Path = Join-Path $CaDir 'certs\NAI-Code-Signing-CA.cer';   Name = 'PB.NAICA.Intermediate.cer' },
    @{ Path = Join-Path $root 'certs\NAI-PaintBrush.cer';        Name = 'PB.NAIProduct.cer' }
)
foreach ($c in $certs) {
    if (-not (Test-Path -LiteralPath $c.Path)) { throw "Certificate not found: $($c.Path)" }
}

# --- compile --------------------------------------------------------------
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) {
    throw "csc.exe not found at $csc (needs .NET Framework 4.x, present on Windows 10/11)."
}

$template = Get-Content -LiteralPath (Join-Path $root 'packaging\bootstrapper\Setup.cs') -Raw
$source = $template.Replace('@MSIX_URL@', $MsixUrl).Replace('@MSIX_SHA256@', $hash)
$tempSource = Join-Path $env:TEMP 'PBSetup.g.cs'
[System.IO.File]::WriteAllText($tempSource, $source,
    (New-Object System.Text.UTF8Encoding($false)))

$manifest = Join-Path $root 'packaging\bootstrapper\setup.manifest'
$out = Join-Path $OutDir 'NAI-PaintBrush-Setup.exe'

$args = @(
    '/nologo', '/target:winexe', '/platform:x64', '/optimize+',
    "/win32manifest:$manifest",
    '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll', '/r:System.Security.dll',
    "/out:$out"
)
foreach ($c in $certs) {
    $args += "/resource:$($c.Path),$($c.Name)"
}
$args += $tempSource

Write-Host "==> Compiling setup"
& $csc @args
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) {
    throw "csc failed (exit $LASTEXITCODE)"
}

# --- sign the setup -------------------------------------------------------
$pfx = Join-Path $root '.cert\NAIPaintBrush.pfx'
$pwdFile = Join-Path $root '.cert\NAIPaintBrush.pfx.pwd'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
$sdk = Get-ChildItem $sdkRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$signtool = if ($sdk) { Join-Path $sdk.FullName 'x64\signtool.exe' } else { $null }

if ((Test-Path -LiteralPath $pfx) -and (Test-Path -LiteralPath $pwdFile) -and $signtool) {
    $pwd = (Get-Content -LiteralPath $pwdFile -Raw).Trim()
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, $pwd)
    Write-Host ("==> Signing setup with {0}" -f $cert.Thumbprint)
    & $signtool sign /fd SHA256 /f $pfx /sha1 $cert.Thumbprint `
        /tr http://timestamp.digicert.com /td sha256 /p $pwd $out 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Signing the setup failed" }
} else {
    Write-Warning "Setup left unsigned (missing PFX/password/signtool)."
}

$item = Get-Item -LiteralPath $out
$setupHash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash
Write-Host ""
Write-Host ("Setup:  {0}" -f $out)
Write-Host ("Size:   {0:N1} MB" -f ($item.Length / 1MB))
Write-Host ("SHA256: {0}" -f $setupHash)
