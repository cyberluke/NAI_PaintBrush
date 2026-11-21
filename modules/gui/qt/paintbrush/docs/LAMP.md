# LAMP — NAI Paintbrush continuation state

> The lamp is durable state, not a report. Read this first after a context
> reset. The ADR (`docs/ADAPTIVE_UI_AND_SUBJECTS.md`) is the contract; this
> file is the position on the map.

## Last verified commit

`8e2a6eadc5` — "placeSubject — usable subject -> canvas placement"

Build: `libqt_plugin.dll` linked, `EXIT=0`.
Probes: `check-qml-contracts PASS`, `check-palettes PASS`, `2/2 probes passed`.

Build command (apps must be closed — they hold the DLLs):

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
ninja -C win64-meson modules/libqt_plugin.dll
```

Probe suite:

```powershell
modules\gui\qt\paintbrush\probes\run-probes.ps1
```

## Delivered (file -> what exists)

| Area | Location |
|---|---|
| SubjectAsset + SubjectInstance + DerivedArtwork models, JSON persistence, cut-out PNG | `pb_subject_store.{hpp,cpp}` |
| QML surface: subjectList/saveSubject/deleteSubject/placeSubject, instanceList/saveInstance/deleteInstance, artworkList | `pb_controller.hpp` (decls) + `pb_controller_palettes.cpp` (impl) |
| Object Select creates a persistent Subject once the mask is ready | `pb_controller.cpp` |
| placeSubject: cut-out PNG -> real canvas layer -> persisted instance, rollback on save failure | `pb_controller_palettes.cpp` |
| Palette store (27 built-ins incl. Art Studio), import/export | `pb_palette_store.{hpp,cpp}` |
| Eager SAM preload (`ensureModelLoaded`, worker prologue + per-job) | `pb_segmentation_service.cpp` |
| Model-load + encode phase markers | `pb_segmentation_service.cpp` |
| Taskbar progress (ITaskbarList3 pimpl, queued GUI-thread COM) | `pb_taskbar.{hpp,cpp}` |
| Segmentation busy/progress forwarded to QML + progress rail | `pb_controller.cpp`, `PaintbrushCanvasView.qml` |
| Layer-space Object Select (`SourceCanvasLayer`), mask baked to document space | `pb_selection.hpp`, `pb_controller.cpp` |
| Bounded 640 px encode + prompt-point mapping | `pb_segmentation_service.cpp` |
| Frame placement centred on the unscaled raster | `pb_controller.cpp` |
| Keyframe-accurate thumbnails (`modelData.*`, nearest keyframe) | `pb_thumbnail_service.cpp`, `PaintbrushMediaDrawer.qml` |
| Swatches collapse during Object Select; strip height constant | `PaintbrushPaletteBar.qml` |
| Settings rewritten (draft state, wired Cancel/Apply) | `qml/PaintbrushSettingsDialog.qml` |

## Architectural invariants (do not reopen)

1. `SubjectAsset` carries source identity + track ids only. **No transform
   fields** — placement lives exclusively on `SubjectInstance`.
2. `ObjectSelectionContext` is transient interaction state; never persist it.
3. Masks are authoritative in the frame/layer space they were produced in;
   display-space copies are derived, never the source of truth.
4. Segmentation failures must never produce a "ready" subject.
5. Palette/instance/subject mutators return an **empty string on success**,
   otherwise a human-readable reason.
6. Interface order: original media is never destructively rewritten.

## Unfinished requirements (in execution order)

1. **Instance transform write-back** — editing a placed instance on canvas must
   update the persisted `SubjectInstance` (x/y/scale/rotation), and instances
   must reload with their composition on project open.
2. **Multiple independent instances** — a UI affordance to place a second copy
   at an offset (proves independence; `placeSubject` called twice already
   creates two instances at the same centre).
3. **Cross-track transactional undo** — one document transaction spanning
   segmentation + transform + effects; undo restores a coherent state.
4. **DerivedArtwork generation** — real contours/fills from the mask; the model
   and persistence exist, generation does not.
5. **Single-surface adaptive strip** — the two bottom bars are still two
   components; contextual collapse and margins are done.

### Vector/hybrid DerivedArtwork — measured scope

A vector layer type is required; Paintbrush has raster/text/image/sticker only.
`LAYER_STICKER` alone is handled in 15 call sites across pb_boost, pb_controller,
pb_controller_art, pb_layer, plus project serialization and the QML tool rail.
Adding LAYER_VECTOR therefore means: enum + CanvasLayer path/stroke fields +
compositor render + project-store serialize/load + delete/duplicate/reorder
handling + contour extraction (Moore boundary trace over the alpha) + editing
UI. This is a subsystem; tracing contours into a raster and labelling it vector
is explicitly forbidden by the ADR.

Start here: `pb_layer.hpp` (enum + fields) -> `pb_document.cpp` compositeLayer ->
`pb_project_store.cpp` serialization -> `pb_controller::deriveArtwork`.
## Delivered after this lamp was first written

- deriveArtwork(subjectId, recipeId): raster tone-ramp derivation, mode=raster.
- deriveContour(subjectId): cut-out alpha -> document QPainterPath selection,
  path persisted as nai-paintbrush/contour/v1, mode=vector.
- applySubjectStyle(instanceId, recipeId): Outline/Glow/Ghost on the linked layer.
- deleteInstance removes the record and its layer atomically.
- One bottom strip surface (options + palette, constant height).

## Known issues / evidence

- Settings magenta was fixed (Dialog `background` must not resolve `theme.*`
  in its own component scope). Visual confirmation still pending from a human.
- SAM runtime acceptance (`encode DONE -> mask ready`) has never been observed
  because the model load happens on first use; eager preload now removes that
  from the click path, but the transition is still unverified at runtime.

## Exact next action

Wrap multi-mutation operations in beginTransaction/endTransaction, then implement
DerivedArtwork generation:

1. Adopt the transaction scope in placeSubject and any style operation, with an
   RAII guard so an early-return error cannot leave a composite open.
2. applySubjectStyle(instanceId, recipeId): resolve the layer linked by
   subjectInstanceId, append an art::EffectNode to layer->effects, push
   SetEffectsCommand, mark canvas dirty. Verify the effect renders and that the
   source segmentation/mask is untouched (invariant 3).
3. Generation: contours/fills from the segmentation mask into a DerivedArtwork
   with mode raster|vector|hybrid, storing sourceRevisionId and provenance.

Implement instance transform write-back:

1. Add `Q_INVOKABLE QString updateInstance(const QVariantMap &instance)` (or a
   focused `updateInstanceTransform(id, x, y, scale, rotation)`) in
   `pb_controller.hpp` + `pb_controller_palettes.cpp`.
2. Persist through `SubjectStore::saveInstance`.
3. On document open, load `instanceList(compositionId)` and re-insert layers
   from each asset's cut-out at the stored transform (skip missing assets and
   log the reason — never crash the open path).
4. Build, commit, continue to item 2 above.
