# Build, sign and verify the NAI PaintBrush MSIX.
#
# Mirrors the NAI OS packaging flow (Seelen-UI/scripts/bundle.msix.ts):
# stage a runtime tree, MakeAppx pack, signtool sign, verify. The MSIX is
# signed with the product leaf issued by the NAI Code Signing CA, and the
# manifest Publisher equals that leaf subject exactly:
#     CN=NAI PaintBrush, O=NANOTRIK.AI, C=US
#
#   pwsh scripts/pb-build-msix.ps1
#   pwsh scripts/pb-build-msix.ps1 -Version 1.0.0.0
#   pwsh scripts/pb-build-msix.ps1 -IncludeAI      # + CUDA + matting models
#   pwsh scripts/pb-build-msix.ps1 -SkipSign       # unsigned artifact
#   pwsh scripts/pb-build-msix.ps1 -Install        # sideload after verify
#
# Packaging rules (NAI CA, docs/SIGNING.md):
#   - sign before publishing; the release index must describe the signed bytes;
#   - never sign with the intermediate or the root;
#   - no CI: operator-run only;
#   - the PFX password never appears in output.
[CmdletBinding()]
param(
    [string]$Version = "1.0.0.0",
    [string]$Arch = "x64",
    [switch]$IncludeAI,
    [string]$SamModel = "D:\_SATIN_AI\models\sam3-q4_0.ggml",
    [switch]$SkipSign,
    [switch]$Install,
    [string]$OutDir = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'win64-meson'
$bin = Join-Path $build 'bin'
$modules = Join-Path $build 'modules'
if (-not $OutDir) { $OutDir = Join-Path $build 'msix' }
$stage = Join-Path $OutDir 'stage'
$stamp = Join-Path $OutDir 'msix'
$msixName = "NAI.PaintBrush_${Version}_${Arch}.msix"
$msixPath = Join-Path $stamp $msixName

$exe = Join-Path $bin 'nai-paintbrush.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Build output not found: $exe`nBuild it first: ninja -C win64-meson bin/nai-paintbrush.exe"
}

if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "MSIX Version must have four numeric parts (e.g. 1.0.0.0): $Version"
}

# --- SDK tools -------------------------------------------------------------
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10\bin'
$sdkVersion = Get-ChildItem $sdkRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $sdkVersion) { throw "Windows SDK not found under $sdkRoot" }
$sdkBin = Join-Path $sdkVersion.FullName $Arch
$makeappx = Join-Path $sdkBin 'makeappx.exe'
$signtool = Join-Path $sdkBin 'signtool.exe'
if (-not (Test-Path -LiteralPath $makeappx)) { throw "makeappx.exe not found: $makeappx" }

# --- stage -----------------------------------------------------------------
Write-Host "==> Staging runtime"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage, $stamp -Force | Out-Null

# CUDA/SAM3 runtime is optional: the editor runs without it (CPU paths).
$cudaExclude = @('cublas*', 'cudart*', 'cudnn*', 'cufft*', 'curand*',
                 'cusparse*', 'cusolver*', 'nvrtc*', 'ggml-cuda*')

$copied = 0
Get-ChildItem -LiteralPath $bin -File -Filter '*.dll' | ForEach-Object {
    if (-not $IncludeAI) {
        foreach ($pattern in $cudaExclude) {
            if ($_.Name -like $pattern) { return }
        }
    }
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $stage $_.Name) -Force
    $copied++
}
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage 'nai-paintbrush.exe') -Force

# The engine spawns its preparser as a child process from the executable
# directory; without it the process pool fails and the interface cannot start.
$preparser = Join-Path $bin 'vlc-preparser.exe'
if (Test-Path -LiteralPath $preparser) {
    Copy-Item -LiteralPath $preparser -Destination (Join-Path $stage 'vlc-preparser.exe') -Force
} else {
    throw "Preparser not found: $preparser"
}

# libvlc/libvlccore are built outside bin; a packaged app has no PATH setup, so
# the runtime tree is FLAT: everything beside the executable, plugins/ next to
# libvlc.dll (VLC discovers <libdir>\plugins).
foreach ($dll in @((Join-Path $build 'lib\libvlc.dll'),
                   (Join-Path $build 'src\libvlccore.dll'))) {
    if (-not (Test-Path -LiteralPath $dll)) { throw "Engine DLL not found: $dll" }
    Copy-Item -LiteralPath $dll -Destination (Join-Path $stage (Split-Path -Leaf $dll)) -Force
    $copied++
}

# plugins: VLC discovers <libvlc dir>\plugins (config_GetSysPath) and the cache
$pluginDir = Join-Path $stage 'plugins'
New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
Get-ChildItem -LiteralPath $modules -File -Filter '*.dll' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $pluginDir $_.Name) -Force
    $copied++
}
$pluginsDat = Join-Path $modules 'plugins.dat'
if (-not (Test-Path -LiteralPath $pluginsDat)) {
    throw "Plugin cache missing: $pluginsDat (run vlc-cache-gen after signing the plugin)"
}
Copy-Item -LiteralPath $pluginsDat -Destination (Join-Path $pluginDir 'plugins.dat') -Force

if ($IncludeAI) {
    $matting = Join-Path $bin 'matting'
    if (Test-Path -LiteralPath $matting) {
        Copy-Item -LiteralPath $matting -Destination (Join-Path $stage 'matting') -Recurse -Force
    } else {
        Write-Warning "matting runtime not found: $matting"
    }

    # SAM3 weights: redistribution is granted by the SAM License (bundled with
    # them); the app finds them at <exe>\models\sam3-q4_0.ggml with no config.
    if (-not (Test-Path -LiteralPath $SamModel)) {
        throw "SAM3 model not found: $SamModel`nPass -SamModel <path to sam3-q4_0.ggml>"
    }
    $modelDir = Join-Path $stage 'models'
    New-Item -ItemType Directory -Path $modelDir -Force | Out-Null
    Copy-Item -LiteralPath $SamModel -Destination (Join-Path $modelDir 'sam3-q4_0.ggml') -Force
}

# The Qt interface needs its QML modules and Qt plugins; the packaged app has no
# launcher to export QML2_IMPORT_PATH/QT_PLUGIN_PATH, so ship them next to the
# executable and let winvlc.c point Qt at them.
$qtShare = 'C:\msys64\mingw64\share\qt6'
foreach ($pair in @(@('qml', 'qml'), @('plugins', 'qtplugins'))) {
    $srcQt = Join-Path $qtShare $pair[0]
    if (-not (Test-Path -LiteralPath $srcQt)) { throw "Qt directory not found: $srcQt" }
    Copy-Item -LiteralPath $srcQt -Destination (Join-Path $stage $pair[1]) -Recurse -Force
}

# Ship the license/notice files inside the package: the MSIX distributes Qt,
# FFmpeg, MinGW and (optionally) CUDA binaries, so the notices must travel with
# the bytes.
foreach ($doc in @('COPYING', 'COPYING.LIB', 'LICENSE-THIRD-PARTY.md', 'AUTHORS')) {
    $srcDoc = Join-Path $root $doc
    if (Test-Path -LiteralPath $srcDoc) {
        Copy-Item -LiteralPath $srcDoc -Destination (Join-Path $stage $doc) -Force
    } elseif ($doc -in @('COPYING', 'COPYING.LIB', 'LICENSE-THIRD-PARTY.md')) {
        throw "Required notice file missing: $srcDoc"
    }
}
$licenseDir = Join-Path $root 'packaging\msix\licenses'
if (Test-Path -LiteralPath $licenseDir) {
    Copy-Item -LiteralPath $licenseDir -Destination (Join-Path $stage 'licenses') -Recurse -Force
}

# --- strip our MinGW-built binaries ---------------------------------------
# MinGW links unstripped by default: one plugin was 73 MB -> 3.9 MB stripped.
# Only strip artifacts this build produced; Qt and vendor DLLs are left alone.
$strip = 'C:\msys64\mingw64\bin\strip.exe'
if (Test-Path -LiteralPath $strip) {
    Write-Host "==> Stripping MinGW binaries"
    # binutils strip on a SIGNED image corrupts the PE (removing the certificate
    # table in place), and signing then fails with 0x800700C1. Strip the
    # unsigned image and sign the result.
    & $signtool remove /s (Join-Path $stage 'nai-paintbrush.exe') 2>$null | Out-Null
    $targets = @()
    $targets += Get-Item -LiteralPath (Join-Path $stage 'nai-paintbrush.exe')
    $targets += Get-ChildItem -LiteralPath $stage -File -Filter 'libvlc*.dll'
    $targets += Get-ChildItem -LiteralPath $stage -File -Filter 'libvlccore*.dll'
    $targets += Get-ChildItem -LiteralPath $pluginDir -File -Filter '*.dll' |
        # libqt_plugin.dll is the one artifact binutils stripping turns into a
        # PE that Authenticode/APPX signing rejects (0x800700C1); keep it as
        # linked (and signed by scripts/pb-sign.ps1).
        Where-Object { $_.Name -ne 'libqt_plugin.dll' }
    foreach ($f in $targets) {
        & $strip '--strip-unneeded' $f.FullName 2>$null
    }
    Write-Host ("    stripped {0} files" -f $targets.Count)
}

# Stripping changes every plugin's size/mtime, which invalidates the cache VLC
# ships; regenerate plugins.dat for the stripped set or VLC rejects the cache
# and never loads the Qt interface.
Write-Host "==> Regenerating plugin cache"
$cacheGen = Join-Path $bin 'vlc-cache-gen.exe'
if (-not (Test-Path -LiteralPath $cacheGen)) { throw "vlc-cache-gen not found: $cacheGen" }
$env:VLC_PLUGIN_PATH = $pluginDir
# cache-gen is a build-tree executable: give it the staged runtime (or the build
# dirs) on PATH so libvlc/libvlccore and Qt resolve.
$env:PATH = "$stage;C:\msys64\mingw64\bin;$(Join-Path $build 'lib');$(Join-Path $build 'src');" + $env:PATH
& $cacheGen $pluginDir | Out-Null
if ($LASTEXITCODE -ne 0) { throw "vlc-cache-gen failed on the staged plugins" }

# --- manifest + assets -----------------------------------------------------
Write-Host "==> Manifest"
$manifestTemplate = Join-Path $root 'packaging\msix\AppxManifest.xml.in'
$manifest = Get-Content -LiteralPath $manifestTemplate -Raw
$manifest = $manifest.Replace('{{version}}', $Version).Replace('{{architecture}}', $Arch)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, $utf8)

$assets = Join-Path $root 'packaging\msix\Assets'
if (-not (Test-Path -LiteralPath (Join-Path $assets 'Square44x44Logo.png'))) {
    throw "MSIX assets missing under $assets; run: python scripts/pb-make-icon.py"
}
Copy-Item -LiteralPath $assets -Destination (Join-Path $stage 'Assets') -Recurse -Force

# --- sign the staged product binaries --------------------------------------
# strip removed their Authenticode block; sign the product layer again BEFORE
# packing (NAI rule: sign before packaging), so the installed binaries are
# signed too. The MSIX has its own signature on top.
if (-not $SkipSign) {
    $pfx = Join-Path $root '.cert\NAIPaintBrush.pfx'
    $pwdFile = Join-Path $root '.cert\NAIPaintBrush.pfx.pwd'
    if (-not (Test-Path -LiteralPath $pfx)) { throw "Product leaf PFX not found: $pfx" }
    if (-not (Test-Path -LiteralPath $pwdFile)) { throw "Leaf password file not found: $pwdFile" }
    $leafPwd = (Get-Content -LiteralPath $pwdFile -Raw).Trim()
    $stagedCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, $leafPwd)
    Write-Host ("==> Signing staged binaries with {0}" -f $stagedCert.Thumbprint)
    # signtool rejects MinGW-stripped DLLs (0x800700C1 / bad PE format), so the
    # product executable carries the file-level signature here and the MSIX
    # package signature covers the stripped plugins.
    foreach ($rel in @('nai-paintbrush.exe')) {
        $target = Join-Path $stage $rel
        if (-not (Test-Path -LiteralPath $target)) { continue }
        & $signtool sign /fd SHA256 /f $pfx /sha1 $stagedCert.Thumbprint /tr $TimestampUrl /td sha256 /p $leafPwd $target 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Signing failed: $rel" }
    }
    $leafPwd = $null
}

# --- mapping ---------------------------------------------------------------
Write-Host "==> Mapping"
$mappingPath = Join-Path $OutDir 'package-mapping.txt'
$mapping = New-Object System.Collections.Generic.List[string]
$mapping.Add('[Files]')
Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    $mapping.Add(('"{0}" "{1}"' -f $_.FullName, $rel))
}
[System.IO.File]::WriteAllText($mappingPath, ($mapping -join "`r`n") + "`r`n",
                               (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("    {0} files staged" -f ($mapping.Count - 1))

# --- pack ------------------------------------------------------------------
Write-Host "==> MakeAppx"
& $makeappx pack /f $mappingPath /p $msixPath /nv /o | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $msixPath)) {
    throw "MakeAppx failed (exit $LASTEXITCODE)"
}

# --- sign ------------------------------------------------------------------
if (-not $SkipSign) {
    $pfx = Join-Path $root '.cert\NAIPaintBrush.pfx'
    $pwdFile = Join-Path $root '.cert\NAIPaintBrush.pfx.pwd'
    if (-not (Test-Path -LiteralPath $pfx)) { throw "Product leaf PFX not found: $pfx" }
    if (-not (Test-Path -LiteralPath $pwdFile)) { throw "Leaf password file not found: $pwdFile" }

    & git -C $root check-ignore -q -- $pfx
    if ($LASTEXITCODE -ne 0) { throw "Refusing to sign: $pfx is not gitignored." }

    $pwdPlain = (Get-Content -LiteralPath $pwdFile -Raw).Trim()
    $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, $pwdPlain)
    $pwdPlain = $null
    if (-not $cert.HasPrivateKey) { throw "Leaf has no private key: $pfx" }

    Write-Host ("==> Signing with {0}" -f $cert.Thumbprint)
    $signArgs = @('sign', '/fd', 'SHA256', '/f', $pfx, '/sha1', $cert.Thumbprint,
                  '/tr', $TimestampUrl, '/td', 'sha256', '/p',
                  (Get-Content -LiteralPath $pwdFile -Raw).Trim(), $msixPath)
    # signtool cannot read the password from a file: keep argv out of any log.
    $signOut = & $signtool @signArgs 2>&1
    $signArgs = $null
    if ($LASTEXITCODE -ne 0) {
        throw "MSIX signing failed; check the leaf, the password file and the timestamp service."
    }
    Write-Host "    signed"

    & $signtool verify /pa /all $msixPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "MSIX signature verification failed (signtool verify /pa /all)" }
    Write-Host "    signature verified (policy: /pa /all)"
} else {
    Write-Host "==> Skipping signing (-SkipSign)"
}

# --- report ----------------------------------------------------------------
$item = Get-Item -LiteralPath $msixPath
$hash = (Get-FileHash -LiteralPath $msixPath -Algorithm SHA256).Hash
Write-Host ""
Write-Host ("MSIX:   {0}" -f $msixPath)
Write-Host ("Size:   {0:N1} MB" -f ($item.Length / 1MB))
Write-Host ("SHA256: {0}" -f $hash)

# --- optional sideload -----------------------------------------------------
if ($Install) {
    $cer = Join-Path $root 'certs\NAI-PaintBrush.cer'
    if (-not (Test-Path -LiteralPath $cer)) { throw "Public leaf cert not found: $cer" }
    Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
    Add-AppxPackage -Path $msixPath
    $pkg = Get-AppxPackage -Name 'NAI.NAIPaintBrush'
    Write-Host ("Installed: {0} {1}" -f $pkg.Name, $pkg.Version)
    Write-Host ("Launch:    explorer.exe shell:appsFolder\{0}!App" -f $pkg.PackageFamilyName)
}
