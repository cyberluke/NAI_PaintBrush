# Paintbrush 2.0 — Reference Architecture
> Grounded in a source-level audit at modules/gui/qt/paintbrush/, master b4298d8883.
> Guiding sentence: do not repair the current Paintbrush; salvage the working low-level
> parts and build the document/render/media/AI core on top of them.

## 0. Salvage (verified, not assumed)

| Salvage | Verified evidence | Reuse as |
|---|---|---|
| SAM3 C++ binding | SegmentationService owns worker thread + sam3 encode/PCS/PVS (segmentation.cpp:213-350), independent of player. | Segmenter backend unchanged; frame source changes. |
| avcodec-hwdec tri-state | Per-media :avcodec-hwdec=1 (ctrl.cpp:343-347). Generic capability. | VLC-level setting for the edit decoder only. |
| PlayerLock | RAII vlc_player_Lock/Unlock (frame_capture.cpp:21-32). | Reuse verbatim. |
| Build/QML/plugin wiring | qml_modulepaintbrush static + libqt_plugin; PaintbrushCtx singleton; --paintbrush entry + sidebar (MainDisplay.qml:609). | Keep surface; slot new types in. |
| Native video/shader path | modules/video_output/opengl/*, d3d11 va (video_output/win32/direct3d11.cpp). | Source for GPU render; don't reinvent. |
| Selection mask handoff + region-delta | setSelectionFromMask + cut/paste. | Region-delta undo is the only undo concept to keep. |
| Provider/config resolver | pbConfigString/Bool/Int, pbApiKey (env-over-config), paintbrush-* keys (qt.cpp:417-478). | Keep resolver; replace capability reporting. |

## 1. Canonical document + compositor (A27 A24 A26 A22 A23 A28 A29)

### 1.1 Core invariant
One DocumentRenderer for screen, export, eyedropper, AI preview, doc thumbnail.
Today the tile compositor applies scale/rotation (canvas_item.cpp:462-518) while
flattenedImage() ignores them (document.cpp:300-305); export/eyedropper/copy/
companion/AI can show a different picture than the user.

### 1.2 Layer model (A26)
Layer { id (stable), type (Raster|Image|Text|Reference|Mask|Sticker), asset
(AssetRef, immutable), transform (QTransform full affine), opacity, blend, mask
(MaskRef), metadata }.
Raster content is always LOCAL-sized. setCanvasSize() (document.cpp:50-80) changes
Document.renderTarget only; it never resizes layer assets. Ends the "image layer
baked to full-canvas at origin" corruption.

### 1.3 Document
renderTarget, layers (Order, stable id map), selection (QPainterPath), source
(MediaRef + FrameRef), tracks (mutable TrackStore, not a controller copy), session
(AiSession, 3.3).

### 1.4 DocumentRenderer::render(target, region)
GPU-first QSG/RHI: per layer resolve(asset->persistent texture), apply(transform)
as QSGTransformNode (never re-rasterize), push(blend, opacity), mask pass.
Adaptive filtering: Linear on continuous zoom, Nearest on integer pixel-art zoom (A25).
CPU fallback = refactored compositeLayer pure function sharing the same transform
semantics. flattenedImage() removed; all callers go through the renderer.

### 1.5 Brush (A22)
Real dab pipeline replacing paintDot/paintLineSegment (canvas_item.cpp:795-850):
pointer -> normalizer -> stabilizer -> velocity -> spacing -> dab(tip/hardness/
opacity) -> scratch tile -> commit region delta. Spray = StochasticGlitter dab
generator (reuse m_sprayNozzle/wobble/vel/flow); same commit path so undo is covered.

### 1.6 Flood fill (A23)
Bounded scanline/span fill replacing full-canvas copy + w*h visited + BFS
(canvas_item.cpp:1455-1556). Runs on layer asset at local dims, maps affected region
through transform for undo/dirty repaint. Large fills -> render worker.

### 1.7 Undo (A28 A29)
Delete whole-doc serialization (document.cpp:182-221). Typed commands:
SetTransformCommand {layerId,before,after}; ReorderCommand; SetVisibilityCommand;
InsertLayerCommand/DeleteLayerCommand (immutable asset ref); StrokeCommand
{region,beforeTiles,afterTiles}; MaskCommand; CanvasResizeCommand (metadata only).
Rotate/crop become SetTransformCommand, never concatenated blobs. Remove
restoreCanvasSnapshot (zero undo call sites; only existed for the broken blob).

## 2. Media architecture split (A8 A9 A18 A19 A10 A11 A12 A13 A14 A17)

### 2.1 Two sources. Never one shared player.
Today every consumer pulls raw vlc_player_t via PlayerController::getPlayer() and
seeks/steps/snapshots the SAME playback session - UI, thumbs, tracking, sticker
export, scrubber. avcodec-hwdec=1 is forced on the one player that is the viewer.

PlaybackSession (viewer, GPU, existing Qt PlayerController + native video surface)
vs EditDecodeSource (exact frames, dedicated decoder). FrameProvider { pts -> QImage }
sits under EditDecodeSource. Thumbs/SAM3/tracking/export/VLM all consume FrameProvider.
No consumer touches the playback player for exact-frame work.
avcodec-hwdec applies ONLY to EditDecodeSource; viewing keeps Auto (fixes A18).

### 2.2 FrameProvider (A9 A19)
request(FrameRef)->QImage decodes exact pts and returns frame+pts; last() returns
FrameRef{mrl,title,ptsMs,width,height}. Every consumer requests an explicit FrameRef.
No consumer snapshots live vout (vout_GetSnapshot PNG retired from edit path; kept
only as generic viewer screenshot). pristineSourceFrame (A19) replaced by FrameProvider
.last() + stored FrameRef on the layer, never "first LAYER_REFERENCE in list".

### 2.3 Thumbnails (A10 A11 A12)
Dedicated decoder, not the import the scrubber drives. Real priority: current
'ordered' (thumbnail.cpp:72-112) is computed but DISCARDED; queue refilled
chronologically from 'times' (thumbnail.cpp:125-loop). 2.0 feeds ordered into a
QPriorityQueue, pop highest priority first. Correctness: decode requested ts, verify
returned PTS within tolerance, then publish; delete seek->tick->snapshot race.
Cache: m_cache (thumbnail.hpp:109) grows unbounded. LRU byte-budget store, evict
least-recently-used by cumulative bytes.

### 2.4 Tracking (A13 A14)
Forward end sets m_stepSign=-1; m_done=0 (tracking.cpp:195-201) but never reseeds to
seed frame, never resets SAM instance/m_lastMask, never re-AddInstance; reverse runs
the WRONG mask from wherever playback stopped. Steps arrive via singleShot(40) vout
snapshot (tracking.cpp:264-288), a race.
2.0: keep SeedState {seedFrame,seedPoints,seedMask,pts}. On reverse switch: re-AddInstance
with same seed mask/points; EditDecodeSource seeks to seedPts as pass-2 frame 0; reset
m_lastMask to SEED mask, m_done=0, phase=1. Frames from FrameProvider.request; mask keyed
by PTS, not "whatever vout was at +40ms".

### 2.5 Stickers (A15 A16 A17)
Real track selection; delete hardcoded stickerEditor.trackId=0 (MediaDrawer.qml:244).
Real pixel preview via FrameProvider source frames/cutouts. Offline export: walk
SeedState.keyframes, decode each via EditDecodeSource, apply effect+outline+shadow+
scale, write sequentially. No player muts, no sleep-poll, no "first non-zero frame".

## 3. AI orchestration (A1 A2 A3 A4 A5 A6 A7 A33 A34 A42)

### 3.1 Capabilities must be truth (A3 A4 A7)
CapabilityReport drives every status dot, never a hardcoded true: sam Ready|MissingModel|BuildNoSam3|LoadError; vision Ready(model)|Unconfigured|Auth; generation local-heal|remote-generation(model)|unavailable (DELETE isConfigured()==true); lookup enabled|disabled; offline on|off; tls normal|dev-selfsigned (explicit flag).
"Generative fill" w/o provider = Heal; Replace Background w/o model = Blur; never
"Image generation ready". TLS: remove unconditional ignoreSslErrors() (vision.cpp:251-253,
assistant.cpp:300-302, lookup.cpp:184-186, generation.cpp, controller); add explicit
paintbrush-dev-insecure opt-in.

### 3.2 Vision: single most damaging fix (A1 A2)
Verified: buildPayload(crop,context,question) (vision.cpp:105-169) builds a user message
with only static system prompt + image URLs; 'question' is NEVER referenced. So "What
brand is this handbag?" never reaches the model.
2.0 userMsg.content = [ {type:text,text:question (VERBATIM)}, {type:text,text:systemPrompt},
{type:image_url,url:cropPng}, {type:image_url,url:contextPng} ].

### 3.3 Real multi-turn session (A2 A33)
conversationId today is only a reply-correlation property (vision.cpp:244); payload is a
single user message, no history.
VisionSession { id (obj-hash of crop+pts), history QList<{role,parts}>, document weak ref }.
Follow-up = new turn appended to the SAME session history.
AssistantService::m_history (assistant.hpp:123) and CompanionService::m_conversation
(companion.hpp:109) never cleared on new/open (verified). Move both onto Document.session
so new/open starts clean (A33).

### 3.4 Local generation must not block (A5 A6)
CPU inpaint/box-blur runs synchronously on the GUI thread during generate(); only signal
emit is queued (generation.cpp:299-333). Wrap fallback in worker pool; resultReady fires
from worker. resultFromJson url uses synchronous QImage::load(url) (generation.cpp:86-112);
use async QNetworkReply GET -> bytes -> QImage::fromData.

### 3.5 Find Product becomes lookup (A42)
Verified: LookupService posts one prompt + one image to the SAME assistant gateway
(lookup.cpp:98-191); controller passes empty vlmDesc/ocr/query (ctrl.cpp:1469);
sourceUrl/imageUrl never populated (lookup.cpp:193-277); confidence is model ad-lib
(lookup.cpp:244-245).
2.0 pipeline (disabled until it lands): SAM crop -> OCR/logo -> VLM candidates
{brand,product,hints} -> retrieval over real catalog -> rerank vs crop -> result
{product,brand,model,confidence,sourceUrl,imageUrl,evidence}. UI shows a Match only
when sourceUrl non-empty; confidence computed near sources.

### 3.6 Kids / offline honesty (A34)
analysisAllowed() silently false in MODE_KIDS w/o network (companion.cpp:69-82). Add
paintbrush-kids-local local classifier, or CapabilityReport.offline surfaced as
"offline companion: local model only", never silence.

## 4. Extract-object and collage (A20 A15)
extractObject() currently resets the ENTIRE document and builds a new canvas (reverse
of the collage workflow). 2.0: selection -> build Sticker asset (cutout + FrameRef +
seed mask) -> InsertLayerCommand {assetRef} -> place at cursor/drop point in the SAME
document -> normal draggable/transformable/shareable layer. No doc reset, no canvas
re-creation. Participates in normal undo (InsertLayerCommand), persistence, export.

## 5. Persistence (A30 A31 A32)
.npb is a single JSON with base64 PNG for every layer and track mask frame
(project_store.cpp:105-172). 2.0 container:
proj.npb/ { manifest.json (small: canvas, layers[], source, tracks[] metadata),
assets/<layerId>.png, masks/<layerId>.png, frames/<pts>.png (deduped by content hash),
extras/<id>.png }. Autosave writes manifest+dirty assets only; unchanged hard-linked/skipped.

Atomic save (A31): ProjectStore::save does QFile::remove(path) then rename(tmp,path)
(project_store.cpp:197-202) - dest deleted before rename, failure loses old file.
QSaveFile included but unused. Use QSaveFile::commit().

Track eviction (A32): controller copies m_document.tracks() and takes pointers from the
copy, so eviction never reaches the document. Evict through the mutable TrackStore the
document owns (document.tracks.prune(...)), with framesChanged notifications.

## 6. Workspace and inspector (A35 A36 A37 A38 A39 A40 A41)
Current budget: canvas = 1 - 56dp/W - min(0.28W,360)/W - min(0.22W,280)/W. Both docks
uncapped-capped at 640dp combined; canvas not the visual center.
2.0: canvas-first. Iconic 2-column Paintbrush rail, large (~40dp) targets, tooltip
secondary (replaces 15 text-only 34dp buttons, ToolRail.qml:42-138; keep tool->canvas.tool
mapping). Inspector = contextual drawer, default-closed, shows only what the active
selection needs (kills permanent 28% stack of layers+selection+AI+diagnostics). Diagnostics
move behind a diagnostics action (green dots fed by CapabilityReport, not in creative space).
Companion = small avatar pill, collapsed by default (not Buddy dock; fixes A41 and the
"Collapsed by default" header contradiction vs companionMode!==0 + default=2).
Palette (A39): tool-option context bar shows only active tool options; kills "Size Size"
(neither Size label, PaletteBar.qml:156-160/179-183, has a visible binding while the Spray
slider/value at 167/175 are gated to tool===9). Color picker (A40): SVG/S saturation bug
1.0-(mouse.y/height)*0.0+0.0 (PaletteBar.qml:264) pins sat to 1.0; replace with real SV
plane + hue rail or QColorDialog/native. Hue *0.85, value *0.8 stay.
LayerModel (A38): replace ListView model:document.layerCount int with QAbstractListModel
PainbrushLayerModel exposing roleId/name/visible/opacity/transform/thumb; bind checked to
real visible (delete hardcoded checked:true, InspectorDock.qml:103), show roleName(name)
(delete "Layer %1".arg(index+1), :109), init transforms from roleTransform (delete value:100/0,
:170/:185). Add opacity control (currently absent despite CanvasLayer::opacity).

## 7. Landing order (streams; each ends runnable)
S1 core document+renderer+undo (draw/rotate/crop/undo/redo/export identical).
S2 VLM truth (question verbatim; real VisionSession; TLS opt-in).
S3 media split (PlaybackSession + EditDecodeSource + FrameProvider; tracking two-pass
reseeds; thumbnails real-priority PTS-verified LRU; sticker export offline; avcodec-hwdec
on edit decoder).
S4 extraction+collage (extractObject inserts Sticker, no doc reset; real track selection;
real preview pixels; sessions per document).
S5 honest AI (CapabilityReport; Heal/Blur; async local gen + url bytes; product lookup
disabled until sources-backed).
S6 workspace+companion (canvas-first IA, iconic rail, contextual inspector, LayerModel,
palette fix, SV picker, companion pill).
S1 is the only blocker before S2-6. Every stream ships an acceptance harness asserting the
real runtime path, not "compiles".

## 8. Acceptance invariants
- Screen == Export == Eyedropper == AI preview == doc thumbnail (one renderer).
- Undo is typed and O(changed), never PNG-all-layers.
- No consumer touches the playback player for exact-frame work.
- VLM receives the user's question; follow-up is real history.
- Caps report truth; no green dot for a not-configured capability.
- extractObject never resets the document.
- Companion/assistant history never leaks across documents.
- Save is atomic via QSaveFile::commit(); track eviction reaches the real store.
