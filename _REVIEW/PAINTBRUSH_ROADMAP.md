# Paintbrush 2.0 — Build roadmap (from 42 atoms)

Ordered so each stream is end-to-end runnable before the next begins. The
guiding sentence: *do not repair the current Paintbrush; salvage the working
low-level parts and build documentary/render/media/AI core on top of them.*

## Stream 1 — Foundational core (vertical slice must be beautiful & fast)
Nothing else is attempted until this flow is clean:
`video → exact frame → SAM click → mask → transparent cutout → drag onto canvas
→ real brush → VLM question → save/export`.

### Document & rendering
- **A.27** canonical compositor — one `DocumentRenderer::render(target, region)`
  used by screen, export, eyedropper, AI preview, doc thumbnail. Highest
  priority: today screen/export/AI can show *different* images.
- **A.21** split monolithic `PaintbrushCanvasItem` into Renderer + ToolEngine +
  SelectionSurface + Document.
- **A.26** local-sized layer content + transform matrix; `setCanvasSize()` no
  longer resizes image/text layers.
- **A.22** real dab brush pipeline (pointer samples → smoothing → pressure
  curve → spacing → tip → hardness/opacity → compositor). Replace
  `QPainter::drawLine`.
- **A.23** scanline/span flood fill, tile-aware, worker for large areas.
- **A.24** QSG/RHI layer compositor with persistent textures (CPU tile
  compositor gone).
- **A.25** adaptive texture filtering (nearest on pixel-art zoom, linear
  otherwise).

### Undo & persistence
- **A.28** command/delta undo (typed commands; structural ops no longer
  serialize the whole document).
- **A.29** typed undo (rotate/crop must store restore-able typed commands, not
  concatenated blobs).
- **A.30** `.npb` becomes a container/dir: manifest + binary assets/chunks.
- **A.31** real `QSaveFile::commit()` atomic save (remove-then-rename bug).

### Document API this stream lands
```
Document → Layer[] (id, type, asset, QTransform, opacity, blend, mask, metadata)
DocumentRenderer::render(target, region)  // single canonical path
```

## Stream 2 — VLM actually talks to the user
- **A.1** inject `question` into the payload (the single worst wiring bug).
- **A.2** real `VisionSession` per object: message history + stable object ID
  so "follow-up" is true multi-turn.
- **A.7** TLS: run with normal validation; dev self-signed is explicit opt-in.
- In this stream the *real* vertical slice is reachable end-to-end.

## Stream 3 — Media architecture split
- **A.8** split main VLC PlaybackSession (GPU smooth playback) from an exact
  EditFrameSource (deterministic PTS decode).
- **A.9** direct decoded-frame/picture bridge with PTS; drop the
  vout_GetSnapshot → PNG round-trip for editing/AI.
- **A.18** HW playback for the viewer; SW/exact decode only for the edit
  decoder split (keep the `avcodec-hwdec` tri-state as a generic capability).
- **A.19** AI source resolved by FrameRef ID/PTS, never first-layer-by-type.

## Stream 4 — SAM + tracking correctness
- **A.13** two propagation passes from the same seed state (reverse pass must
  re-seed and reset temporal state).
- **A.14** tracker consumes exact-PTS frames from EditFrameSource; delete the
  `singleShot(40)` race.

## Stream 5 — Thumbnails
- **A.10** dedicated decoder + actual priority queue; honor the computed
  `ordered`.
- **A.11** decode exact requested timestamp with PTS verification (drop the
  80 ms "hope it's there").
- **A.12** LRU byte-budget cache.

## Stream 6 — Extraction & stickers (collage workflow)
- **A.20** `extractObject()` produces a draggable asset/sticker into the
  existing document — never resets the document.
- **A.15** SelectedTrack state (no hardcoded `trackId = 0`).
- **A.16** sticker preview uses real cached source frames/cutouts, not a gray
  silhouette.
- **A.17** offline render/export pipeline; no main-player seek + `msleep(60)`
  polling, verify requested PTS.

## Stream 7 — Generation & lookup (honest capabilities)
- **A.3** capability truth: `local-heal` / `remote-generation` / `unavailable`.
  Remove `isConfigured() { return true; }`.
- **A.4** rename Generative Fill / Replace Background to Heal / Blur (or back
  them with a real model).
- **A.5** CPU fallback generation runs in a worker/async job; never blocks UI.
- **A.6** `url` responses: async GET → bytes → `QImage::fromData`.
- **A.42** Product lookup becomes a pipeline:
  `SAM crop → OCR/logo → VLM candidates → visual/text search → retrieval →
  rerank vs crop → source-backed result`. Disable the fake prompt-based
  "lookup" entirely until this lands.

## Stream 8 — Companion & sessions
- **A.33** assistant history per document (session object owned by the
  document).
- **A.34** Kids mode: local companion or explicit offline state; never silent.
- **A.41** Companion is a small avatar/pill (collapsed by default), not a dock.
- **A.32** track eviction operates on the real mutable track store, not a copy.

## Stream 9 — Workspace & Inspector (Paintbrush look)
- **A.35** canvas ~80% priority; one contextual drawer, closed by default.
- **A.36** iconic 2-column Paintbrush toolbar, large targets.
- **A.37** contextual inspector keyed to active tool/object.
- **A.38** real LayerModel with id/name/visibility/transform properties.
- **A.39** contextual tool options (kill the phantom "Size Size").
- **A.40** real SV plane + hue rail (or system picker); fix the `* 0.0`.

## Roadmap guardrails
- Stream 1 must finish *before* Stream 2—9 begin; it is the vertical slice.
- All streams must ship with the contested assertions fixed at the source.
- Merge approval requires A.1, A.2, A.27, A.28, A.8, A.20, A.3 verified by a
  runnable acceptance harness (not just "compiles").