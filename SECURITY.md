# Security — code signing and trust

NAI PaintBrush releases are signed with a product leaf issued by the
**NAI Sovereign Certificate Authority** (intermediate: `NAI Code Signing CA`).
One leaf per product; the root and intermediate keys are never used to sign
product binaries.

## Product signing identity

| Field | Value |
|---|---|
| Subject | `CN=NAI PaintBrush, O=NANOTRIK.AI, C=US` |
| Issuer | `CN=NAI Code Signing CA, O=NANOTRIK.AI, C=US` |
| SHA-1 thumbprint | `49F660D897442D825416676196B71B39E5249F91` |
| Validity | 2026-11-22 → 2029-11-22 |
| Public certificate | [`certs/NAI-PaintBrush.cer`](certs/NAI-PaintBrush.cer) |
| Private key | `.cert/NAIPaintBrush.pfx` — **gitignored, never committed** |
| Password | `.cert/NAIPaintBrush.pfx.pwd` — **gitignored, never committed** |
| Authority | `D:\_SATIN_AI\_NAI_CA_AUTHORITY` |

The subject is fixed. Rotating the leaf keeps the same subject and replaces the
PFX; see the CA authority's `docs/ROTATION.md`.

## What gets signed

| Artifact | Path |
|---|---|
| Application | `win64-meson\bin\nai-paintbrush.exe` |
| Product UI plugin | `win64-meson\modules\libqt_plugin.dll` |
| Engine DLLs (optional, `-IncludeEngine`) | `win64-meson\lib\libvlc.dll`, `win64-meson\src\libvlccore.dll` |

## Sign

Operator-run, local only. There is no CI signing and no GitHub release
pipeline (CTO policy).

```powershell
# build first
pwsh scripts/pb-build-plugin.ps1
ninja -C win64-meson

# sign + verify
pwsh scripts/pb-sign.ps1
pwsh scripts/pb-sign.ps1 -IncludeEngine
pwsh scripts/pb-sign.ps1 -TrustChain   # import root/intermediate into CurrentUser for local verification
```

`pb-sign.ps1` reads the leaf PFX and its password from `.cert/`, refuses to run
if the PFX is trackable by git, pins the leaf by certificate object (no
`/sha1` guessing), stamps every signature with an RFC3161 timestamp over
SHA-256, and verifies the result. The password is never placed on a command
line.

**Sign before packaging.** Any manifest, bundle or release index must describe
the signed bytes; re-signing after packaging invalidates it.

Signing rewrites `libqt_plugin.dll`, which invalidates `modules\plugins.dat`
(VLC then starts without the Qt interface, i.e. with no window). `pb-sign.ps1`
regenerates the cache after signing, and `launch-pb-noskip.ps1` refreshes it
whenever the plugin is newer than the cache. Ship the post-sign `plugins.dat`,
never a pre-sign one.

## Verify

```powershell
Get-AuthenticodeSignature win64-meson\bin\nai-paintbrush.exe |
  Format-List Status, StatusMessage, SignerCertificate, TimeStamperCertificate
```

`Status` must be `Valid`, the signer subject must be
`CN=NAI PaintBrush, O=NANOTRIK.AI, C=US`, and the thumbprint must equal
`49F660D897442D825416676196B71B39E5249F91`. A missing
`TimeStamperCertificate` means the signature will die with the leaf — reject it.

For the full chain without touching the machine-wide store:

```powershell
$ca = 'D:\_SATIN_AI\_NAI_CA_AUTHORITY'
$root  = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new("$ca\certs\NAI-Sovereign-Root-CA.cer")
$inter = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new("$ca\certs\NAI-Code-Signing-CA.cer")
$leaf  = Get-AuthenticodeSignature win64-meson\bin\nai-paintbrush.exe
$chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
$chain.ChainPolicy.RevocationMode = 'NoCheck'
$chain.ChainPolicy.TrustMode = 'CustomRootTrust'
$chain.ChainPolicy.CustomTrustStore.Add($root) | Out-Null
$chain.ChainPolicy.ExtraStore.Add($inter) | Out-Null
$chain.Build($leaf.SignerCertificate)   # must be True
```

## Trust provisioning

- **Developers:** `pwsh scripts/pb-sign.ps1 -TrustChain` adds the public root
  and intermediate to the **current user's** `Root` / `CA` stores for local
  verification.
- **Installers:** machine-wide trust (`LocalMachine\Root`) is an installer or
  provisioning step only, never a build script.
- **Consumers:** verify SHA-256 **and** the signer thumbprint; never trust the
  hash alone.

## Rules

1. `.cert/` is radioactive: no PFX or password in git, chat, logs, build
   artifacts or uploads.
2. Never sign a product with the intermediate or the root.
3. Never share a leaf between products.
4. Timestamp every release signature.
5. Record every issuance/rotation in the CA authority's `docs/INVENTORY.md`.
6. No machine-wide trust from a build script.
7. No CI/CD or GitHub release workflows for signing.

If this file and the CA authority disagree, the CA authority is authoritative
for the chain; this file is authoritative for the PaintBrush leaf and its
subject.
