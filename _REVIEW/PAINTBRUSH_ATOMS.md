# Paintbrush — Analýza na atomy

Decomposed into individual atoms (root causes / unit findings) from the
`master` review at `b4298d8883`. Each atom is one falsifiable claim with
code evidence. No fixes applied.

## A.1 — The user question never reaches the VLM

- `PaintbrushController::askAboutObject(...)` calls
  `m_vision.ask(objectCrop(), currentAiSource(), question, conversationId)`.
- `VisionService::buildPayload(...)` receives `question` but never writes it
  into the JSON payload. The endpoint only receives the generic system prompt
  + the image(s). The UI promise ("ask about this object") is not delivered.

## A.2 — "Follow-up" is not a real conversation

- `VisionService` has no message history. `conversationId` is only a request
  property. `askAboutObjectFollowUp()` reuses the id but the service sends a
  brand-new request; there is no multi-turn state. The retained-context claim
  is cosmetic.

## A.3 — Image generation reports ready when nothing is configured

- `GenerationService::isConfigured() const { return true; }` unconditionally.
  The "Image generation ready" green status is shown even with no image
  provider. Capability truthfulness is required: local-heal / remote-generation
  / unavailable.

## A.4 — Generative Fill is not generative

- Without a provider, "generative fill" is a local onion-peel pixel
  propagation; "replace background" is a box blur. They must be renamed
  (Heal/Blur) or backed by a real image model.

## A.5 — CPU fallback generation blocks the UI

- The heavy CPU fallback runs synchronously *before* the queued callback,
  freezing the interface. Needs a worker/async render job (the queued callback
  is a separate mechanism that also exists).

## A.6 — Image-provider URL responses are not downloaded

- `resultFromJson()` for `url` does `QImage::load(url)`, which is not an HTTP
  download. A URL-response provider therefore cannot work. Needs async GET →
  bytes → `QImage::fromData`.

## A.7 — TLS validation is disabled globally

- Vision, generation, lookup and assistant all call `ignoreSslErrors()`
  unconditionally. Dev self-signed should be an explicit opt-in, not the
  default.

## A.8 — Playback, editing and AI share one VLC player

- Playback, thumbnails, scrubbing, tracking and sticker export all manipulate
  the same main VLC player. Random seeks from one consumer break the others.
  A PlaybackSession must be separated from an exact EditFrameSource.

## A.9 — "AI frame" capture is an expensive PNG middleware

- Frame capture goes video → vout → PNG encode → PNG decode → QImage. A direct
  decoded-frame/picture bridge with PTS is required.

## A.10 — Thumbnail priority is computed but then ignored

- The comment says anchor-first; the code does compute `ordered` but never
  uses it, and the queue is filled chronologically from `times`. Needs a
  dedicated decoder + an actual priority queue.

## A.11 — Thumbnail correctness is a race

- After a seek it waits one 80 ms tick and snapshots the current vout without
  verifying PTS. Must decode the exact requested timestamp.

## A.12 — Thumbnail cache has no eviction/budget

- `m_cache` has no eviction or byte budget. Needs an LRU byte-budget cache.

## A.13 — Tracking reverse pass never reseeds

- After the forward pass it only sets `m_stepSign=-1`; it never returns the
  player/tracker to the seed frame or resets temporal state. Two propagation
  passes from the same seed state are required.

## A.14 — Tracking frame source is a 40 ms guess

- After the frame-step callback there is a hardcoded `singleShot(40)` and a
  vout snapshot. A race, not a deterministic frame provider. Tracking must get
  a frame with a concrete PTS from the edit decoder.

## A.15 — Sticker track is hardcoded

- Media drawer opens an animated sticker with `trackId = 0`; there is no real
  track selection state.

## A.16 — Sticker preview uses a gray silhouette, not real pixels

- Preview uses a gray mask silhouette instead of the actual source video
  pixels. Requires cached real source frames/cutouts.

## A.17 — Sticker export drives the main player over the UI thread

- Export seeks the main player, polls on the UI path and does
  `QThread::msleep(60)`; the first non-zero snapshot is not verified against
  the requested PTS. Requires an offline render/export pipeline.

## A.18 — HW-decoder fix downgrades all main playback

- The new `avcodec-hwdec` tri-state is a reasonable generic VLC capability, but
  Paintbrush now forces its entire main playback to software decode. Should be
  HW playback + a separate SW/exact edit decoder.

## A.19 — AI source frame may be stale

- `pristineSourceFrame()` returns the *first* `LAYER_REFERENCE` it finds. With
  multiple captured frames the AI can work over an old frame. Needs FrameRef
  ID/PTS rather than first-layer-by-type lookup.

## A.20 — Extract-object resets the document

- `extractObject()` resets the entire document and makes a new canvas — the
  opposite of the original collage workflow. An object must be a draggable
  asset/sticker into the existing document.

## A.21 — Canvas is one monolithic QQuickItem

- One giant `QQuickItem` handles renderer, tiles, brush, shapes, selection,
  SAM mask, grid, pressure, input, spray, clipboard. Must be split into
  Renderer + ToolEngine + SelectionSurface + Document.

## A.22 — Brush is a line, not a dab pipeline

- Brush is essentially `QPainter::drawLine`/ellipse. No dab engine, spacing,
  hardness, stabilization, tilt, velocity, pressure interpolation. A real
  brush pipeline is required.

## A.23 — Flood fill blocks the UI

- Flood fill allocates a full-canvas visited buffer + a copy of the whole
  QImage and runs BFS synchronously. Needs scanline/span fill, tile-aware,
  worker for large areas.

## A.24 — CPU tile compositor, QSG as mere upload

- CPU tile composition of QImage plus QSG upload/display; transformed image
  layers are re-transformed during tile compositing. A QSG/RHI GPU compositor
  with persistent textures is required.

## A.25 — Zoom textures use nearest filtering

- Non-integer zoom can look rough/pixelated. Needs adaptive nearest for
  pixel-art zoom and linear otherwise.

## A.26 — Layer resizing corrupts local-sized layers

- `setCanvasSize()` resizes every raster including image/text layers to canvas
  size and keeps position. Each layer needs local content + a transform matrix.

## A.27 — Screen and export disagree (canonical compositor missing)

- Canvas render respects scale/rotation, but
  `PaintbrushDocument::flattenedImage()` ignores them. Export, eyedropper,
  companion and part of AI can see a different image than the user. One
  canonical compositor for screen/export/AI is required. (Single most
  consequential rendering defect.)

## A.28 — Undo is project serialization dressed as undo

- Structural operations compress *all* layers → PNG → base64 → JSON before and
  after each action. Needs command/delta undo.

## A.29 — Rotate/Crop undo stores a blob the undo cannot restore

- `rotate90()`/crop compose `canvasSnapshot + layersSnapshot`, but Structure
  undo can only restore `restoreLayersSnapshot()`. The formats do not match.
  Needs typed undo commands, not concatenated blobs.

## A.30 — Project persistence is JSON + base64 PNG everywhere

- `.npb` is JSON with base64 PNG for every layer and every track mask frame.
  Explodes size/save time on larger projects. Needs a container/dir with a
  manifest + binary assets/chunks.

## A.31 — "Atomic save" deletes before renaming

- The comment claims atomic save, but the code does `QFile::remove(path)` and
  only *then* `rename(tmp,path)`. `QSaveFile` is included but unused. Must use
  a real `QSaveFile::commit()`.

## A.32 — Track eviction lives on a copy

- The controller copies `m_document.tracks()` and takes pointers from the
  copy; eviction therefore never removes masks in the document. Eviction must
  operate on the real mutable track store.

## A.33 — Companion history leaks between documents

- `newBlankCanvas()` and open-image do not clear assistant history;
  AssistantService has one global `m_history`. Context can bleed across
  documents. Needs a session per document.

## A.34 — Kids mode silently disables

- If `paintbrush-kids-network=false`, companion analysis is just disabled; no
  local companion and no clear offline state. Needs a local model or an
  explicit offline state, not silence.

## A.35 — Workspace: canvas competes for a tiny share

- Rail + up-to-28% Inspector + up-to-22% Assistant + palette + media drawer
  leave the canvas without ~80% visual priority. Canvas must dominate; one
  contextual drawer closed by default.

## A.36 — Tool rail is a debug toolbar

- Text-only `Sel`, `Rnd`, `Oval`, `Pan` in `fontSize_xsmall` on a 56 px rail.
  Needs an iconic 2-column Paintbrush toolbar with large targets; tooltips
  secondary.

## A.37 — Inspector mixes unrelated concerns

- One panel mixes layers, transforms, selection morphology, SAM, generation,
  VLM, lookup, collage and diagnostics. Needs a contextual inspector keyed to
  the active tool/object.

## A.38 — Layers UI shows placeholders, not state

- Always shows "Layer 1", "Layer 2"; visibility checkbox is hardcoded
  `checked: true`; transform spinboxes default 100/0 instead of binding to
  layer state. Needs a real LayerModel with id/name/visibility/transform
  properties.

## A.39 — Palette has a visible phantom label

- In Spray mode the first `Size` label stays visible while the Spray slider is
  hidden, producing the screenshot "Size Size". Needs contextual tool options
  instead of one permanent bar.

## A.40 — HSV saturation pin is broken

- In the HSV picker the saturation stays effectively `1.0` while dragging
  because of an expression multiplying by `* 0.0`. Needs a real SV plane + hue
  rail or a system picker.

## A.41 — Companion is a greedy default dock

- Default `Buddy = 2` opens the companion; with Inspector it consumes ~50% of
  the width. Companion should be a small avatar/pill, collapsed by default,
  not a dock.

## A.42 — Product lookup is not a lookup

- `LookupService` reuses the same chat gateway prompt; no search, no catalog
  retrieval, no source URLs, and the controller passes empty OCR/description/
  query. The model even fabricates `confidence`. Needs VLM candidates + a
  real evidence-backed retrieval layer, source-backing every result.