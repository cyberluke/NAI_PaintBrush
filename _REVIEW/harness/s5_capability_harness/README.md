# Stream 5 (first half) — acceptance harness

Standalone acceptance tests for the Paintbrush honest-AI changes (A3/A4/A5/
A6/A7/A42). The harness compiles the **real** paintbrush service sources
(`pb_capability.cpp`, `pb_generation_service.cpp`, `pb_assistant_service.cpp`,
`pb_lookup_service.cpp`) against Qt 6 (msys2 mingw64) with a stub
`vlc_common.h` and stub config/log helpers. No VLC build, no external network
(the HTTP provider binds `127.0.0.1` on an ephemeral port).

## Layout

- `stub/vlc_common.h` — minimal stand-in for VLC's header (services only
  pass `vlc_object_t *` around opaquely).
- `s5_acceptance.cpp` — the test program; also provides the stub
  `pbConfigString/Bool/Int/pbApiKey/pbLogRequest` definitions.
- `build_run.ps1` — moc + compile + link + run (requires
  `C:\msys64\mingw64` with qt6-base and qt6-network; moc lives at
  `share/qt6/bin/moc.exe`).

## Run

```powershell
& .\build_run.ps1
```

Expected result: `67 checks, 0 failures` (exit code 0).

## What is asserted

| Item | Assertion |
|---|---|
| (a) A3/A4 | `GenerationService::capability()` is `local-heal` / `local-blur` with a reason and `isGenerative()==false` when no provider; `remote-generation` with model name only when a provider is configured; the unconditional `isConfigured()==true` lie is gone. |
| (b) A5 | A multi-second CPU inpaint: `generate()` returns in <500 ms, no result synchronously, `resultReady` arrives asynchronously afterwards, busy state correct, both heal and blur paths. |
| (c) A6 | End-to-end against a loopback HTTP provider: POST → JSON `{url}` → async `QNetworkReply` GET → bytes → `QImage::fromData`; pixel-compare vs the served PNG; exactly two exchanges. |
| (d) A7 | `paintbrush-*-insecure` / `paintbrush-dev-insecure` default false for assistant, lookup and generation; posture flips to `DevSelfSigned` only when opted in. The `ignoreSslErrors()` call site is gated (`if (insecure)`) in all three services. |
| (e) A42 | `evidenceBacked()==false` at module level; every parsed match map and the plain-text fallback carry `evidenceBacked=false`, `confidenceIsEstimate=true`, empty `sourceUrl`/`imageUrl`; `Match::hasSource()` is false for empty URLs. |
| regression | Assistant chat round-trip against the loopback provider still works (conversation id + content preserved). |