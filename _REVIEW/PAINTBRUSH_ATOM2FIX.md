# Atom → Fix mapping (verified against source, master b4298d8883)

Each row: atom id, the exact current symbol/file:line that is wrong, and the 2.0 fix target
(the new symbol/design section where the fix lands). "Fix target" = replacement class/stream.

## Core / render / undo
| Atom | Current defect (file:line) | 2.0 fix target |
|---|---|---|
| A27 | flattenedImage() ignores scale/rotation (document.cpp:300-305) vs tile compositor applies them (canvas_item.cpp:462-518) | §1.4 DocumentRenderer single compositor; delete flattenedImage(); all callers route through renderer |
| A21 | One monolithic PaintbrushCanvasItem (canvas_item.cpp/html ~2357 lines) | §1 Renderer + ToolEngine + SelectionSurface + Document split |
| A26 | setCanvasSize() bakes local image/text layers to full-canvas origin (document.cpp:50-80) | §1.2 local-sized assets; renderTarget only |
| A22 | brush = QPainter::drawLine/ellipse (canvas_item.cpp:795-850) | §1.5 dab pipeline; Spray = StochasticGlitter dab generator |
| A23 | flood fill full-canvas copy + w*h visited + BFS (canvas_item.cpp:1455-1556) | §1.6 scanline/span fill, tile-aware, worker |
| A24 | CPU tile compositor; QSG mere upload, normally disposed textures | §1.4 QSG/RHI persistent textures; transforms as node matrixes |
| A25 | Nearest filtering hardcoded (canvas_item.cpp:651) | §1.4 adaptive Linear/Nearest |
| A28 | Structure undo = PNG-all-layers->base64->JSON (document.cpp:182-221) | §1.7 typed commands, O(changed) |
| A29 | rotate90/crop push canvasSnapshot()+layersSnapshot() blob; restoreLayersSnapshot no-ops, restoreCanvasSnapshot never called (canvas_item.cpp:2090-2156) | §1.7 SetTransformCommand; remove restoreCanvasSnapshot |

## Media / frames
| Atom | Current defect | 2.0 fix target |
|---|---|---|
| A8 | one player = viewer+scrub+thumbs+tracking+export (ctrl.cpp getPlayer() call sites: 401,1571,1686,2056,2142,3026 + tracking/thumbnail caches) | §2.1 PlaybackSession vs EditDecodeSource |
| A9 | AI frame = vout_GetSnapshot PNG round-trip (frame_capture.cpp:57-82) | §2.2 FrameProvider direct decode |
| A18 | avcodec-hwdec=1 forced on main playback item (ctrl.cpp:343-347) | §2.1 tri-state on EditDecodeSource only |
| A19 | pristineSourceFrame() first LAYER_REFERENCE (ctrl.cpp:1217-1230) | §2.2 FrameProvider.last() + stored FrameRef |
| A10 | 'ordered' computed but discarded; queue filled chronologically from 'times' (thumbnail.cpp:72-112 vs 125-loop) | §2.3 QPriorityQueue from ordered |
| A11 | seek -> 80ms tick -> snapshot no PTS verify (thumbnail.cpp:237-241) | §2.3 decode exact ts, verify PTS |
| A12 | m_cache no eviction/budget (thumbnail.hpp:109) | §2.3 LRU byte-budget |
| A13 | reverse pass: m_stepSign=-1 no reseed (tracking.cpp:195-201) | §2.4 SeedState re-AddInstance + reseek |
| A14 | singleShot(40) vout snapshot race (tracking.cpp:264-288) | §2.4 FrameProvider request by PTS |
| A15 | stickerEditor.trackId=0 hardcoded (MediaDrawer.qml:244) | §4 SelectedTrack state |
| A16 | preview = gray mask silhouette | §4 real pixels via FrameProvider |
| A17 | export seeks main player + msleep(60) poll, no PTS verify (ctrl.cpp:2056-2112) | §2.5 offline render/export |

## AI
| Atom | Current defect | 2.0 fix target |
|---|---|---|
| A1 | buildPayload drops 'question' (vision.cpp:105-169) | §3.2 question verbatim into payload |
| A2 | conversationId only reply-correlation; no history (vision.cpp:244) | §3.3 VisionSession real history |
| A3 | isConfigured()==true always (generation.hpp:55) | §3.1 CapabilityReport (DELETE isConfigured) |
| A4 | generative fill = local onion-peel; replace bg = box blur (generation.cpp:123-284) | §3.1 Heal/Blur labels or real model |
| A5 | sync CPU fallback on GUI thread (generation.cpp:299-333) | §3.4 worker pool |
| A6 | QImage::load(url) sync network (generation.cpp:86-112) | §3.4 async GET bytes |
| A7 | unconditional ignoreSslErrors() everywhere (vision.cpp:251-253, assistant.cpp:300-302, lookup.cpp:184-186) | §3.1 TLS opt-in flag |
| A33 | m_history never cleared on new/open (assistant.hpp:123, companion.hpp:109) | §3.3 session per document |
| A34 | kids+network-off silently disables (companion.cpp:69-82) | §3.6 local model or explicit offline |
| A42 | lookup = prompt to chat gateway; no search/sources/URLs; fabricated confidence (lookup.cpp:98-191,244-245) | §3.5 evidence-backed pipeline; UI Match only when sourceUrl non-empty |

## Document ops / persistence / workspace
| Atom | Current defect | 2.0 fix target |
|---|---|---|
| A20 | extractObject() resets whole document (ctrl.cpp) | §4 InsertLayerCommand Sticker asset |
| A30 | .npb = JSON + base64 PNG everything (project_store.cpp:105-172) | §5 container manifest + binary assets |
| A31 | save: QFile::remove then rename (project_store.cpp:197-202); QSaveFile unused | §5 QSaveFile::commit() |
| A32 | eviction works on controller copy of tracks() | §5 evict on document-owned TrackStore |
| A35 | canvas 1-56dp/W-min(0.28W,360)-min(0.22W,280) (Workspace.qml:265,283,290) | §6 canvas-first IA |
| A36 | 15 text-only 34dp buttons (ToolRail.qml:42-138) | §6 iconic 2-col, ~40dp |
| A37 | inspector mixes layers+selection+AI+diags permanently | §6 contextual drawer |
| A38 | ListView model:document.layerCount int (InspectorDock.qml:82) + checked:true(103) + Layer %1(109) + value:100/0(170,185) | §6 QAbstractListModel LayerModel with role bindings |
| A39 | both Size labels always visible (PaletteBar.qml:156-160,179-183) | §6 context bar |
| A40 | sat = 1.0-(y/h)*0.0+0.0 pins sat (PaletteBar.qml:264) | §6 real SV plane + hue rail |
| A41 | companionMode default 2 (Buddy) + !==0 dock visible (companion.hpp:98, Workspace.qml:292) | §6 companion pill collapsed by default |
