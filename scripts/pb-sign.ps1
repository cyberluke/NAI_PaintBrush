# Copyright (c) NAI / NANOTRIK.AI
#
# Sign and verify the NAI PaintBrush release binaries with the product leaf
# issued by the NAI Code Signing CA.
#
#   pwsh scripts/pb-sign.ps1                 # app exe + Qt plugin
#   pwsh scripts/pb-sign.ps1 -IncludeEngine  # + libvlc.dll / libvlccore.dll
#   pwsh scripts/pb-sign.ps1 -TrustChain     # import the public root/intermediate
#                                            # into the CURRENT USER stores, then
#                                            # verify as Valid
#
# Rules (from D:\_SATIN_AI\_NAI_CA_AUTHORITY\AGENTS.md + docs/SIGNING.md):
#   - one leaf per product; never sign with the intermediate or the root;
#   - the PFX and its password live in the gitignored .cert/ and are never
#     committed, printed, or put on a command line;
#   - every release signature is timestamped (RFC3161, SHA256);
#   - sign BEFORE packaging: any manifest/index must describe the signed bytes;
#   - no CI: this is an operator-run script.
#
# This script uses Set-AuthenticodeSignature with an in-memory certificate
# handle rather than `signtool /p <password>`: the signature is the same
# Authenticode signature, but the password never appears in the process
# command line. The leaf is pinned by handing over that exact certificate
# object, which is equivalent to signtool's /sha1 <thumbprint>.
[CmdletBinding()]
param(
    # Extra artifacts to sign. Defaults are the application binary and the
    # product Qt plugin.
    [string[]]$Files = @(),

    # Also sign the rebuilt VLC engine DLLs (they ship with the product).
    [switch]$IncludeEngine,

    # Import the public root + intermediate into CurrentUser\Root / CurrentUser\CA
    # so Get-AuthenticodeSignature reports Valid on this machine. Local
    # verification only; installers own machine-wide trust.
    [switch]$TrustChain,

    [string]$Pfx = "",
    [string]$PasswordFile = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [string]$CaDir = "D:\_SATIN_AI\_NAI_CA_AUTHORITY"
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'win64-meson'
$bin = Join-Path $build 'bin'
$modules = Join-Path $build 'modules'

if (-not $Pfx) { $Pfx = Join-Path $root '.cert\NAIPaintBrush.pfx' }
if (-not $PasswordFile) { $PasswordFile = Join-Path $root '.cert\NAIPaintBrush.pfx.pwd' }

# --- guards ---------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Pfx -PathType Leaf)) {
    throw "Product leaf PFX not found: $Pfx`nIssue it with the CA: pwsh `"$CaDir\scripts\Issue-ProductLeaf.ps1`" -Product NAI-PaintBrush -Name NAIPaintBrush -Subject 'CN=NAI PaintBrush, O=NANOTRIK.AI, C=US' -OutDir `"$root\.cert`" -Force"
}
if (-not (Test-Path -LiteralPath $PasswordFile -PathType Leaf)) {
    throw "Leaf password file not found: $PasswordFile (keep it next to the PFX in the gitignored .cert/)."
}

# The PFX must never be trackable by git.
& git -C $root check-ignore -q -- $Pfx
if ($LASTEXITCODE -ne 0) {
    throw "Refusing to continue: $Pfx is not ignored by git. Add '.cert/' to .gitignore first."
}

# --- artifacts ------------------------------------------------------------
if ($Files.Count -eq 0) {
    $Files = @(
        (Join-Path $bin 'nai-paintbrush.exe'),
        (Join-Path $modules 'libqt_plugin.dll')
    )
    if ($IncludeEngine) {
        $Files += (Join-Path $build 'lib\libvlc.dll')
        $Files += (Join-Path $build 'src\libvlccore.dll')
    }
}
foreach ($f in $Files) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        throw "Artifact not found: $f`nBuild it first: ninja -C win64-meson"
    }
}

# --- leaf -----------------------------------------------------------------
$pwdPlain = (Get-Content -LiteralPath $PasswordFile -Raw).Trim()
if (-not $pwdPlain) { throw "Leaf password file is empty: $PasswordFile" }

Add-Type -AssemblyName System.Security | Out-Null
$flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
$cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($Pfx, $pwdPlain, $flags)
$pwdPlain = $null

if (-not $cert.HasPrivateKey) {
    throw "The leaf has no private key: $Pfx"
}
$expected = 'CN=NAI PaintBrush, O=NANOTRIK.AI, C=US'
if ($cert.Subject -ne $expected) {
    throw "Unexpected leaf subject: '$($cert.Subject)' (expected '$expected'). Refusing to sign."
}

Write-Host ("Leaf : {0}" -f $cert.Subject)
Write-Host ("  by : {0}" -f $cert.Issuer)
Write-Host ("  sha1: {0}" -f $cert.Thumbprint)
Write-Host ("  exp : {0:yyyy-MM-dd}" -f $cert.NotAfter)

# --- local trust (opt-in) -------------------------------------------------
if ($TrustChain) {
    $rootCer = Join-Path $CaDir 'certs\NAI-Sovereign-Root-CA.cer'
    $interCer = Join-Path $CaDir 'certs\NAI-Code-Signing-CA.cer'
    if (-not (Test-Path -LiteralPath $rootCer) -or -not (Test-Path -LiteralPath $interCer)) {
        throw "Public root/intermediate certs not found under $CaDir\certs."
    }
    Import-Certificate -FilePath $rootCer -CertStoreLocation 'Cert:\CurrentUser\Root' | Out-Null
    Import-Certificate -FilePath $interCer -CertStoreLocation 'Cert:\CurrentUser\CA' | Out-Null
    Write-Host "Imported the NAI root + intermediate into the CURRENT USER stores (local verification only)."
}

# --- sign -----------------------------------------------------------------
$failed = 0
foreach ($f in $Files) {
    Write-Host ("`nSigning {0}" -f (Resolve-Path -LiteralPath $f))
    $sig = Set-AuthenticodeSignature -FilePath $f -Certificate $cert `
              -TimestampServer $TimestampUrl -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') {
        Write-Warning ("  signature status after signing: {0} - {1}" -f $sig.Status, $sig.StatusMessage)
    }
}

# --- verify ---------------------------------------------------------------
Write-Host "`nVerification"
$rows = @()
foreach ($f in $Files) {
    $v = Get-AuthenticodeSignature -FilePath $f
    $rows += [pscustomobject]@{
        File       = Split-Path -Leaf $f
        Status     = [string]$v.Status
        Signer     = if ($v.SignerCertificate) { $v.SignerCertificate.Subject } else { '' }
        Thumbprint = if ($v.SignerCertificate) { $v.SignerCertificate.Thumbprint } else { '' }
        Timestamp  = if ($v.TimeStamperCertificate) { $v.TimeStamperCertificate.NotAfter.ToString('yyyy-MM-dd') } else { 'none' }
    }
    if ($v.Status -ne 'Valid') { $failed++ }
}
$rows | Format-Table -AutoSize

if (-not $TrustChain) {
    Write-Host "Status 'NotTrusted' is expected unless the NAI root is trusted for this user; re-run with -TrustChain or use the installer trust step."
}
if ($failed -gt 0) {
    Write-Error ("{0} artifact(s) did not verify as Valid." -f $failed)
    exit 1
}
Write-Host "All artifacts verified: Valid."
