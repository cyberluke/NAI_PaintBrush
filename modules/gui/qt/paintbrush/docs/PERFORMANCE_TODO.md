# NAI Paintbrush — PERFORMANCE_TODO

> Powders / canvas live-paint performance ledger.
> Baseline measured **2026-11-22**, build `394b509c49`, RTX 4090,
> canvas grid `1001x564` cells (`cell 1.28` world px) at **124 %** zoom,
> brush 12 px, Glitter powder.
>
> Rule for this file: a number without the command that produced it is not a
> number. Keep every entry measurable.

---

## 1. How to measure (do this before touching anything)

| What | Command / knob | Output |
|---|---|---|
| Build the Qt plugin | `pwsh scripts/pb-build-plugin.ps1` | `NINJA_EXIT=0`, `plugins.dat` timestamp |
| Run the app | `pwsh scripts/launch-pb-noskip.ps1` | `pb_noskip.log` (8 KB **block-buffered** — not live evidence) |
| Powder cost split | `PB_POWDER_PERF=1` | stderr: `POWDER BAKE …` every 30 bakes, `POWDER PERF …` every ~60 ticks |
| Stroke trace | **on by default**; `PB_POWDER_TRACE=0` disables | `pb_stroke_trace.log` (16 MB rotate to `.1`) |
| One driven stroke + capture | `pwsh scripts/pb-powder-verify.ps1 -NoBlow -StepMs 60` | window capture + last log evidence lines |
| Zero-readback QSG preview (experimental) | `PB_POWDER_GPU_LIVE=1` | only for node work, see §4.3 |

Log formats to look for:

```
POWDER BAKE pack=0.8 upload=1.0 draw=1.3 read=0.5 unpremult=0.0 total=3.7 px=13899
POWDER PERF bake=gpu qsg=0 tick=2.6ms(160) physics=9.11ms(30) render=15.89ms(30) …
[pbtrace] pseg gpu=0 aff=92,356 178x178 uni=92,356 178x178
```

`physics` is per **segment**, not per tick, and a synthetic drag with
`-StepMs 60` produces ~120 px segments (≈57 dabs). A real mouse sends 2–8 px
segments. Compare like with like.

---

## 2. Baseline numbers (2026-11-22, after the fix)

| Stage | Before | Now |
|---|---|---|
| Full-grid pack (565 k cells, sim + colour) | 21.4 ms | **0.8 ms** (window only) |
| Glue field (64 B/cell) copy + texture + upload | ~16 ms, 36 MB | **skipped** when `hasAnyGlue()` is false |
| Texture upload / draw / readback | 0.9 / 1.1 / 0.5 ms | 1.0 / 1.3 / 0.5 ms |
| `unPremultiply` (3 double divisions / px) | ~14 ms | **0.0 ms** (integer reciprocal table) |
| **One bake (13.9 k px window)** | **57.8 → 41.4 → 23.9 ms** | **3.7 ms** |
| **Whole 8 ms input tick** | starved (~12 ticks/s) | **2.6 ms** (≈60 Hz preview) |

Result: live powder is **60 fps** with the premium material and no look change
on release.

---

## 3. What was fixed — do not regress these

1. **One renderer for preview and commit.**
   `PaintbrushCanvasItem::bakePowderRegion()` is the only bake entry point; it
   prefers `GpuRuntime::renderPowder(..., PowderOutput::MaterialLayer)` and
   uses the CPU Voronoi only as a fallback. The live path used to call
   `m_powder.render()` directly (the old white-bead look) while the commit used
   the GPU surface, so the material popped on release.
2. **One dirty window per segment.**
   `artStrokeSegment()` renders only `segment + radius*2 + 12 px` (min 6 cells),
   intersected with the material bounds and the layer raster. The scratch tile
   accumulates the stroke; the full union is baked once on release.
3. **Windowed cell packing.**
   `GpuRuntime::renderPowder()` takes `cellWindow` (from
   `powderCellWindow()`, +4 cell margin) and packs only those cells. The
   texture stays full-grid so the shader's world→cell mapping is unchanged;
   cells outside the window are the zero fill.
4. **No glue upload without glue.**
   `powder.hasAnyGlue()` gates the `kGlueSubRes² = 64 B/cell` field. Without
   it, a 1x1 zero stub is bound. `glueRevision()` makes future windowed glue
   uploads possible.
5. **Integer un-premultiply.** `unPremultiply()` uses
   `kInv[a] = round(255*65536/a)`; `v*kInv[a] < 2^32` by construction.
6. **Clear before drawing the live material.** A powder layer's raster *is* a
   bake of the material state; drawing it under the fresh render
   double-composited previously committed grains.
7. **Display-rate preview cadence.** Physics runs on the 8 ms input tick; the
   bake runs at 16 / 33 / 50 ms by window area and the shimmer clock
   accumulates the skipped time.
8. **Trace is on by default** and flushes incrementally (stroke begin/end and
   every 128 markers), so a stroke that dies before commit still leaves a tail.

---

## 4. Remaining performance TODO (ordered by value)

### 4.1 Release/commit bake is still one synchronous shot
- **Symptom:** on mouse-up the whole union is baked once
  (`renderPowderUnion()` → `renderPowderPreview(true)`), including
  `m_rhi->finish()` readback. A full-canvas stroke can hitch one frame.
- **Where:** `pb_canvas_item.cpp::renderPowderPreview` /
  `renderPowderUnion`, `pb_gpu_runtime.cpp::readback`.
- **Options:** (a) bake the union in horizontal strips of ~256 px, one strip per
  tick, and commit when done; (b) reuse the live windowed bake and only bake
  the strips not yet committed; (c) move the commit to the zero-readback node
  when §4.3 is fixed.
- **Gate:** release hitch < 16 ms for a 2000x800 union; identical pixels to
  today's one-shot bake.

### 4.2 Glue window uploads (only when glue is used)
- **Symptom:** the first bake after any glue edit uploads the full
  `W*8 x H*8` field (36 MB on a 1001x564 grid).
- **Where:** `pb_gpu_runtime.cpp` glue texture creation/upload.
- **Fix:** keep the glue texture persistent, upload
  `powderCellWindow(region) * kGlueSubRes` on `glueRevision()` change, full only
  on (re)creation. The packer (`pb_powder_pack.cpp`) already does this for the
  QSG node — reuse that logic.
- **Gate:** glue stroke tick ≤ 8 ms at 60 Hz; glue visuals unchanged.

### 4.3 Zero-readback QSG node is parked, not done
- **Status:** compiled and wired behind `PB_POWDER_GPU_LIVE=1`, **off by
  default**. Its rect/packing mapping did not match the raster path on the real
  QML view (one dab, offset from the cursor), so it is not trusted.
- **Where:** `art/gpu/pb_powder_scene_node.{hpp,cpp}`,
  `art/gpu/pb_powder_pack.{hpp,cpp}`, `pb_canvas_item.cpp::updatePaintNode`.
- **Next step:** prove it in isolation before re-enabling — a small
  `QQuickView` harness that draws one `PowderSceneNode` over a known rect and
  asserts the pixel position/size against the GPU bake. Only then wire it back.
- **Payoff:** removes `m_rhi->finish()` and the QImage round-trip entirely
  (~2.5 ms of the 3.7 ms bake).
- **Gate:** harness pixel parity, then a live stroke with
  `PB_POWDER_GPU_LIVE=1` and `tick ≤ 3 ms`.

### 4.4 Physics is linear in pointer travel
- **Symptom:** `steps = |segment| / (radius*0.35)`; a fast drag or a synthetic
  60 ms tick produces 50+ pours per segment (9 ms observed with the test drag).
- **Where:** `pb_canvas_item.cpp::artStrokeSegment` powder branch.
- **Fix:** cap `steps` per tick (e.g. 24) and distribute the remainder into the
  next tick, or pour along the segment analytically (one deposit per cell the
  segment crosses) instead of N circular dabs.
- **Gate:** `physics` per segment ≤ 2 ms with a 120 px synthetic segment;
  visual density unchanged at normal speeds.

### 4.5 Tile compositing re-composites the whole layer stack
- **Symptom:** `compositeDirtyTiles()` → `compositeTile()` runs
  `DocumentRenderer::composite()` per dirty tile, over every layer.
- **Where:** `pb_canvas_item.cpp::compositeDirtyTiles` / `compositeTile`.
- **Fix:** cache the composite of the layers *below* the active powder layer
  (it does not change during a powder stroke) and composite only the top layer
  during the stroke.
- **Gate:** ≤ 1 ms per dirty tile at 256 px with 8 layers.

### 4.6 Long-stroke bookkeeping
- **Symptom:** `captureBeforeRegion()` and `ensureScratch()` grow with the
  union; `m_strokeUnion`/`m_strokeBefore` are per-rect lists with a linear
  containment scan.
- **Where:** `pb_canvas_item.cpp`.
- **Fix:** merge captured rects into a coarse grid, or capture one union-sized
  snapshot lazily on the first tick that touches it.
- **Gate:** no per-tick growth in `tick=` over a 10 s continuous stroke.

### 4.7 Verify the smear tool on the unified path
- `TOOL_SMEAR` enters the same powder branch and now the same bake. Re-run the
  §2 measurements with Smear and a large brush; record them here.

### 4.8 (Optional) Buffer the stroke trace to disk less often
- Tracing is on by default; every 128 markers writes ~4 KB. Fine today, but if
  a stroke ever emits thousands of markers, batch to 256.

---

## 5. Acceptance gates for any change in this area

1. `tick=` ≤ 8 ms at 60 Hz on the §2 canvas with brush 12–64.
2. `total=` (bake) ≤ 4 ms for a ~14 k px window.
3. Live and released powder look identical (no pop) — compare with
   `scripts/pb-powder-verify.ps1 -NoBlow` plus a mid-stroke
   `scripts/pb-capture-attached.ps1` capture.
4. No tile holes under the stroke and no double-composited grains (check the
   `pb_stroke_trace.log` `pseg` union growth and the released raster).
5. `pb_noskip.log` / stderr has no `QSG powder node failed`, no QML warnings.
6. Numbers appended to §2 with the commit hash.
