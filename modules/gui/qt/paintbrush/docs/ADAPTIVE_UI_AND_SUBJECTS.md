# NAI Paintbrush: Adaptive Creative Workspace & Subject Architecture

**Status:** Accepted product direction, revised architectural contract.
**Partially implemented (2026-11-21):** SubjectAsset / SubjectInstance /
DerivedArtwork stores and placements; layer-space Object Select; transactional
undo for layer edits plus subject place/delete; adaptive strip baseline;
settings persistence through the VLC config. **Pending:** video-into-canvas
preview (plan corrected in `docs/HANDOFF.md` §2, not implemented); full
invariant-8 capability migration; native `LAYER_VECTOR`; true stylus pressure;
native acceptance of settings, undo and strip.

**Target:** `modules/gui/qt/paintbrush/docs/ADAPTIVE_UI_AND_SUBJECTS.md`

**Scope:** Existing VLC4/SAML-based NAI Paintbrush, including native Qt UI, illustration, segmentation, compositing and persistent documents.

> Revision note: this document supersedes the first ADR. The four corrected gaps are
> (1) `SubjectAsset` vs `SubjectInstance` separation, (2) `DerivedArtwork` as the
> editable illustration output, (3) document-wide transactional undo with
> track-scoped changes, (4) Phase 0 (segmentation P0) before any new architecture.

## 0. Product definition

NAI Paintbrush is an AI-native creative environment combining:

- Procreate-class digital painting.
- Concepts-class editable vector illustration.
- Krita/Sketchbook-class brush ergonomics.
- Fusion-class non-destructive compositing.
- Video-subject extraction, tracking and stylization using the existing VLC4/SAML integration.

The product must support both professional illustration from an empty canvas and the
transformation of real-world subjects extracted from video into editable artwork.

**These are capabilities of one editor and one document system, not separate
applications or incompatible modes.**

The canvas remains the primary workspace. Complexity is progressively disclosed through
contextual controls, floating inspectors and advanced editing surfaces.

### Non-negotiable principles

1. **Canvas-first:** editing controls must not dominate the artwork.
2. **Non-destructive:** source media, original strokes, masks and transformation history remain recoverable.
3. **Editable:** vector artwork consists of real editable geometry and strokes, not disguised raster images.
4. **Temporal:** video-derived objects retain source-time relationships and animation capabilities.
5. **Reusable:** subjects and artwork can be instantiated across compositions.
6. **Responsive:** brush interactions and canvas navigation remain responsive independently of heavy AI processing.
7. **Local-first:** documents and editing operations must remain usable without a cloud service.
8. **Preserve existing behavior:** do not replace functioning VLC4/SAML workflows or unrelated editing features.

## 1. Unified document architecture

The central architectural decision is to separate three concepts:

- **SubjectAsset:** reusable extracted subject and source identity.
- **SubjectInstance:** placement and animation of that subject in a composition.
- **DerivedArtwork:** editable artistic representation generated from a subject.

These have independent lifecycles.

### 1.1 SubjectAsset

A SubjectAsset is a persistent, reusable library object.

Conceptual contract:

`SubjectAsset { id, name, sourceMediaId, sourcePtsMs, identityTrackId, segmentationTrackId, defaultEffectsTrackId?, derivedArtworkIds[] }`

Responsibilities:

- Reference source media by stable identity rather than filesystem path alone.
- Preserve the originating frame position.
- Maintain subject identity and segmentation information over time.
- Support non-destructive mask corrections.
- Own references to derived illustration revisions.
- Remain independent of any particular composition.

`sourcePtsMs` identifies the originating media moment. Frame-accurate operations must
additionally retain the source stream's native timestamp and time-base information. Do not
derive authoritative frame identity from rounded milliseconds or assumed constant frame rate.

### 1.2 SubjectInstance

A SubjectInstance places a reusable asset into a specific composition.

Conceptual contract:

`SubjectInstance { id, subjectAssetId, compositionId, sourceTimeMapId, transformTrackId, instanceEffectsTrackId?, zIndex, visibilityTrackId? }`

Responsibilities:

- Define position, scale, rotation, visibility and stacking order.
- Map composition time to source-media time.
- Support trimming, retiming and animation.
- Override appearance without modifying the shared source asset.
- Allow several independent instances of one performer.

**Critical invariant:** two instances of the same SubjectAsset must be independently
transformable and stylable.

Updating an instance must never silently modify other compositions.

Shared source-level style changes and instance-level overrides must have explicit semantics.

### 1.3 DerivedArtwork

DerivedArtwork is a persistent, editable artistic representation of source content.

Conceptual contract:

`DerivedArtwork { id, sourceSubjectId, sourceRevisionId, mode, styleRecipeId, contourLayerIds[], fillLayerIds[], shadingLayerIds[], textureLayerIds[], editableStrokeSetId?, regionMapId?, provenanceId }`

Supported modes:

- `raster`: stylized image layers.
- `vector`: editable paths, fills and strokes.
- `hybrid`: vector geometry combined with raster shading and textures.

Requirements:

- Generated contours and fills must remain independently editable.
- Artist-authored modifications must not be silently overwritten by regeneration.
- Store the source revision, processing configuration, renderer/model versions and generation provenance.
- Support multiple artistic interpretations of the same subject.
- Preserve links to original segmentation and media.

A rasterized illustration must never be presented as editable vector artwork merely because
it is displayed inside a vector container.

### 1.4 Unified canvas objects

The document scene must support video subjects, raster artwork, vector artwork, images,
text, groups and composition instances through a common object system.

Shared capabilities include:

- Spatial transforms.
- Layer ordering and grouping.
- Visibility and opacity.
- Masks and blending.
- Effect references.
- Selection and manipulation.
- Serialization and history.
- Optional temporal properties.

Not every object needs a video timeline. Static artwork must work without artificial
temporal complexity.

### 1.5 Tracks, revisions and undo

Identity, segmentation, transformation and effects must remain independently versioned and
invalidated.

**Independent tracks do not require independent document files.** Choose physical storage
based on existing persistence architecture.

Rules:

- Modifying a segmentation keyframe must not erase transforms.
- Changing an effect must not rewrite segmentation.
- Rebuilding a derived representation must not destroy artist edits.
- Cross-track changes must be atomic transactions.
- Ordinary Undo/Redo follows chronological document operations.
- Track-specific history may be exposed as an additional advanced capability.
- Every accepted operation is recoverable after interruption or restart.

Use stable IDs, schema versions, explicit migration paths and atomic persistence.

## 2. Adaptive UI

### 2.1 One contextual bottom strip

Replace the current tool-options bar and separate color palette with one constant-height
adaptive surface.

The strip's content is derived from the active tool and selection context.

| Context           | Primary controls                                   |
| ----------------- | -------------------------------------------------- |
| Select / Move     | Transform, alignment, distribution, layer order    |
| Brush / Pencil    | Brush preset, size, flow, opacity, smoothing       |
| Eraser / Smudge   | Brush preset, strength, size                       |
| Vector Edit       | Nodes, width, smoothing, Nudge, Slice              |
| Shape             | Fill, stroke, width, radius, snapping              |
| Text              | Font, size, weight, spacing, alignment             |
| Object Select     | Selection status, add/subtract, refine edge, track |
| Subject           | Extract, style, mask, source, instance operations  |
| Fill / Eyedropper | Tolerance, sampling, colors                        |
| Glitter / Powder  | Existing material and interaction parameters       |
| Export            | Output format, quality and export action           |

Rules:

- Constant height. Tool changes must not resize or reposition the canvas.
- Swatches appear only when useful, collapsing completely in unrelated contexts.
- Zero or one prominent primary action is permitted. Continuous painting must not require an unnecessary Apply button.
- Preserve Keep/Boost/Mood behavior where applicable, but integrate it contextually instead of retaining unrelated floating chips.
- No persistent instructional bubbles covering the canvas.
- Every control is keyboard-accessible with predictable focus handling.
- Advanced controls open contextual popovers rather than expanding the strip.

### 2.2 Cursor popup palette

Implement a customizable palette inspired by Krita.

Contextual access:

- Brush presets.
- Color selection and recent colors.
- Brush size and opacity.
- Canvas rotation.
- Frequently used tools.
- Context-specific actions for selected subjects.

Support mouse, pen and keyboard invocation.

Follow the existing input and shortcut architecture. Do not globally hijack gestures or
right-click interactions required by other tools.

### 2.3 Brush puck

Provide a compact, optional Sketchbook-style brush control.

- Horizontal adjustment: brush size.
- Vertical adjustment: opacity or flow.
- Fast access to brush presets.
- Configurable visibility and position.
- No permanent obstruction of the artwork.

### 2.4 Top command hierarchy

Organize commands by intent:

- Document
- Edit
- View
- AI
- App

Do not interpret these as five always-visible icon groups.

At desktop resolution, the title bar should contain a small global set of high-frequency
commands, with the rest available through menus, shortcuts and contextual controls.

Settings has one entry point. Destructive operations remain secondary and require
appropriate confirmation.

Preserve native window controls and existing accessibility behavior.

### 2.5 Advanced surfaces

Provide collapsible:

- Layer inspector.
- Brush Studio.
- Vector properties.
- Subject inspector.
- Timeline.
- Effects graph.
- Subject library.

Ordinary painting must not require any of these panels to remain visible.

A canvas-only mode must hide editing chrome while preserving navigation and recovery
shortcuts.

## 3. Professional illustration engine

### 3.1 Input processing

Use a unified pointer/stylus event pipeline supporting:

- Pressure.
- Tilt.
- Velocity.
- Timestamped samples.
- Configurable stabilization.
- Mouse fallback.
- High-DPI and transformed-canvas coordinate mapping.

Where supported by hardware, expose additional pen properties through capability detection.

Input samples must be retained independently of presentation frames so that temporary
rendering delays do not irreversibly destroy stroke quality.

### 3.2 Raster brush engine

Support professional brush behaviors:

- Spacing, scatter and rotation.
- Shape and texture sources.
- Pressure/tilt response curves.
- Opacity, flow and buildup.
- Hardness and edge softness.
- Smudge and wet-media behavior.
- Blend modes.
- Presets and preset versioning.

Pencil, ink, marker, watercolor, oil, airbrush and eraser presets must have genuinely
differentiated rendering behavior.

Do not implement them merely as different brush icons with the same stroke algorithm.

### 3.3 Editable vector strokes

Vector strokes must retain editable stroke geometry and attributes.

Each stroke stores sufficient information to reconstruct its appearance, including control
geometry, dynamics, brush identity and relevant source samples.

Required operations:

- Select and transform.
- Nudge and deform.
- Slice and split.
- Recolor and restyle.
- Adjust width.
- Smooth while preserving intentional corners.
- Edit control points.
- Snap and align.
- Convert to fill regions where geometrically valid.

Preserve stroke identity across edits.

**Raster brushes and editable vectors must coexist.** A highly textured stroke may use
raster rendering while retaining a vector centerline and editable parameters.

### 3.4 Infinite canvas and composition bounds

Support an unbounded editing coordinate space with:

- Zoom and rotation.
- Grid systems.
- Paper and background styles.
- Shape guides.
- Movable artboards.
- Export regions.
- Precision snapping.

A finite video frame is an artboard or composition output region inside the larger
workspace, not a limitation imposed on the entire document.

### 3.5 Rendering architecture

Inspect the existing Qt/VLC4 rendering pipeline before introducing new dependencies.

Requirements:

- Efficient dirty-region updates.
- Tile or region caching for expensive content.
- High-resolution artwork support.
- Predictable alpha blending and color handling.
- GPU acceleration where supported and advantageous.
- CPU-side authoritative document state.
- Safe recovery from GPU device/context loss.
- Independent preview and final-quality processing.
- Explicit cache invalidation based on track and asset revisions.

Preserve source data and editing operations even when GPU caches are discarded.

## 4. Video extraction and Subject creation

The existing VLC4/SAML implementation remains authoritative.

`ObjectSelectionContext` is transient interaction state. It must not be expanded into the
persistent Subject entity.

### 4.1 Lifecycle

The expected user-visible sequence is:

1. Import or open source video.
2. Select a subject on the canvas.
3. Start segmentation encoding.
4. Receive encode completion.
5. Materialize and verify mask data.
6. Preview and optionally refine the mask.
7. Commit a persistent SubjectAsset.
8. Instantiate the asset into the composition.

**Encode DONE is not equivalent to mask ready.**

These must be independently observable states.

Segmentation failures, cancellation and recovery must not leave partially committed
SubjectAssets masquerading as valid assets.

### 4.2 Temporal tracking

Segmentation must support:

- Multiple subjects.
- Stable identities where tracking evidence supports them.
- Per-frame or keyed mask corrections.
- Occlusions.
- Reappearance after temporary disappearance.
- Alpha-edge refinement.
- Temporal mask consistency.
- Explicit correction when tracking fails.

Do not assume identity can always be recovered across unrelated shots.

### 4.3 Library persistence

Save source references, tracks, revisions and derived artwork.

Media assets must be validated on reopening. Missing or relocated media must produce a
recoverable relink state, not silent data corruption.

## 5. Subject-to-artwork reconstruction

This is a core NAI Paintbrush capability, not a decorative AI filter.

### 5.1 Processing stages

A configurable pipeline operates on the segmented subject:

**Source → Mask → Structure → Contours → Regions → Style → Editable Artwork → Composite**

Potential processing components:

- Alpha refinement.
- Silhouette extraction.
- Pose/landmark estimation.
- Semantic region analysis.
- Internal contour extraction.
- Tone and detail separation.
- Curve fitting and simplification.
- Structured fill generation.
- Painterly shading and texture.
- Editable stroke reconstruction.

Stages must be individually replaceable and independently cacheable.

ML-driven stages may use the existing model infrastructure or additional evaluated
components. Do not assume the segmentation model itself performs stylization, semantic
reconstruction or high-quality vector generation.

### 5.2 Three explicit artistic output types

**Stylized Raster**

Produces layered painterly results using masks, shading, color and textures.

**Editable Vector**

Produces actual paths, contours, fills and strokes. Topology and geometry are available for
direct editing.

**Hybrid Illustration**

Produces editable primary contours and fill regions combined with raster textures, shading
and detail.

Hybrid illustration is the preferred default for Concepts-like artistic results.

### 5.3 Style recipes

Styles are reusable, versioned processing recipes, not destructive filters.

Initial target families:

- Ink illustration.
- Product concept sketch.
- Comic rendering.
- Watercolor.
- Chalk and pencil.
- Gloss and glitter.
- Fashion illustration.

Recipes may control contour simplification, line weight, palette, shading, texture, detail
retention and reconstruction behavior.

Preserve recipe versions, parameters, model dependencies and provenance.

### 5.4 Temporal style consistency

Applying a style to a moving subject must not mean independently generating unrelated images
for every frame.

Support temporal correspondence using available motion, tracking, region and geometry
information.

Requirements:

- Stable contour appearance.
- Consistent palette and materials.
- Controlled shading movement.
- Predictable transitions across segmentation corrections.
- Preservation of source motion.
- Reusable style parameters across a clip.
- Manual correction at selected frames or intervals.

Complex topology changes may require regenerated geometry or artist intervention. Do not
promise perfect automatic vector correspondence through severe occlusion, scene cuts or
identity ambiguity.

### 5.5 Non-destructive manual refinement

Artist edits must live in explicit editable layers or override records.

Regenerating source-derived artwork must not erase those edits.

Where correspondence can be established, preserve or reproject corrections. Otherwise
report conflicts and retain both revisions for recovery.

### 5.6 Effect graph

Represent the pipeline internally as a versioned dependency graph.

Nodes may handle:

- Source media.
- Segmentation.
- Alpha refinement.
- Structural analysis.
- Vector reconstruction.
- Color and material generation.
- Painting overlays.
- Temporal transforms.
- Composition output.

Expose advanced graph editing optionally, but keep ordinary interaction selection-first and
canvas-first.

A style operation may generate persistent artwork assets; it is not necessarily a real-time
pixel shader. The effects system must support both procedural rendering and generated
representations.

## 6. Interaction scenarios

### Scenario A: Professional illustration

Starting with an empty document:

1. Create an unbounded workspace.
2. Select a pressure-sensitive brush.
3. Draw smooth, responsive strokes.
4. Combine vector contours, raster shading and textures.
5. Nudge and reshape existing strokes.
6. Use layers, shape guides and snapping.
7. Save and reopen the editable document.
8. Export a selected artboard.

**Acceptance target:** create a detailed sneaker illustration comparable in structure and
editability to the Concepts reference, without importing video or using AI generation.

### Scenario B: Video-to-illustration

Starting with K-pop performance footage:

1. Import and preview the video.
2. Click a performer.
3. Execute the existing segmentation workflow.
4. Confirm the mask is materialized.
5. Commit a reusable SubjectAsset.
6. Place a SubjectInstance in the composition.
7. Apply a Hybrid Illustration recipe.
8. Edit generated outlines and fills.
9. Add hand-painted details.
10. Animate or retime the instance.
11. Save, reopen and export.

**Acceptance target:** the resulting illustration remains linked to its source media,
retains independent animation, and contains genuinely editable artwork.

### Scenario C: Reusable subject

Extract one performer and instantiate the SubjectAsset twice.

- Give each instance different positions.
- Apply different style overrides.
- Retain the shared source segmentation.
- Edit one instance without affecting the other.
- Refine the common segmentation and verify both instances update appropriately without
  losing their independent transformations.

### Scenario D: Temporal artistic consistency

Apply the same illustration recipe to a moving performer.

- Scrub across a clip.
- Verify subject identity and alpha alignment.
- Verify stable contour and palette behavior.
- Correct one problematic time interval.
- Reprocess the affected dependency range.
- Verify artist-authored corrections remain recoverable.

## 7. Implementation phases

### Phase 0: Restore and prove existing behavior

Before expanding architecture:

- Resolve the outstanding `encode DONE → mask ready` P0.
- Verify mask materialization and selection lifecycle.
- Inspect and correct the Settings magenta rendering issue if still present.
- Record current build, probe and runtime evidence.

Do not mistake an unverified status transition for a delivered feature.

### Phase 1: Document architecture

- Inspect current document and persistence implementation.
- Introduce SubjectAsset and SubjectInstance boundaries.
- Establish typed track references and independent revisions.
- Implement transactional history and persistent serialization.
- Preserve existing document compatibility.
- Add migrations and recovery behavior.

Deliver a real Subject creation, save, reopen and reuse path.

### Phase 2: Adaptive UI

- Merge the bottom bars.
- Implement contextual strip visibility.
- Preserve constant canvas geometry.
- Consolidate existing floating actions.
- Group top commands and add overflow.
- Introduce cursor popup palette and optional brush puck.

All controls must invoke real existing or newly implemented operations.

### Phase 3: Illustration foundation

- Unified stylus input.
- Brush presets and dynamics.
- Editable vector strokes.
- Raster/vector layer compositing.
- Infinite canvas and artboards.
- Shape guides and snapping.
- Layer inspector and persistence.

### Phase 4: Reconstruction pipeline

- Implement source-to-structure processing.
- Produce real DerivedArtwork objects.
- Support raster, vector and hybrid output.
- Add versioned style recipes.
- Enable manual editing of generated results.

### Phase 5: Temporal styling and compositing

- Apply recipes over video time.
- Introduce temporal correspondence and consistency handling.
- Preserve tracked animation and instance overrides.
- Support range-scoped regeneration.
- Expose advanced effects and timeline surfaces.

Each phase must deliver an independently usable vertical slice, not merely interfaces or
placeholders.

## 8. Verification and acceptance gates

### Functional invariants

- Original media is never destructively rewritten.
- Subject assets and instances remain independent.
- Segmentation edits do not destroy transformation tracks.
- Style edits do not mutate original masks.
- Undo/redo preserves document consistency across tracks.
- Artist edits survive regeneration or remain explicitly recoverable.
- Real vector artwork remains structurally editable after save/reopen.
- No tool switch causes canvas reflow.
- Existing Keep/Boost/Mood capabilities remain reachable.
- Segmentation failures do not produce false-ready subjects.

### Runtime evidence

Every phase must provide:

- Successful `libqt_plugin.dll` build.
- Relevant probe suite results.
- Native Qt runtime confirmation.
- Structured logs of the critical state transitions.
- Save/reopen verification where persistence changes.
- Rendering screenshots or recordings for UI/visual behavior.
- Measured input/render performance where graphics behavior changes.

For performance-sensitive work, record hardware, document dimensions, frame rate, frame-time
distributions, GPU/CPU utilization and memory usage.

Set quantitative performance budgets before implementation and report actual measurements.
Never claim 60 FPS, latency guarantees or GPU acceleration without runtime evidence.

### Reliability

- Recoverable document state.
- Atomic commits and rollback.
- Bounded resource use for long-running processing.
- Cancellation and resumable work where practical.
- Explicit error states.
- Versioned cache keys and deterministic invalidation.
- No silent processing-provider substitution.
- No hidden source or artist-edit destruction.

## 9. Explicit exclusions

Do not:

- Replace existing working functionality with mock controls.
- Build separate incompatible painting and video document formats.
- Conflate ObjectSelectionContext and SubjectAsset.
- Store composition-specific transforms on a shared reusable asset.
- Present rasterized images as editable vector geometry.
- Flatten AI output without preserving source lineage.
- Implement style conversion solely as independent per-frame filters.
- Introduce an always-visible node editor.
- Expand the toolbar into another permanent panel collection.
- Overwrite manual corrections during regeneration.
- Declare implementation complete from a successful build alone.

## 10. Delivery instructions

Treat this ADR as a product and architecture contract.

Before modifying code:

1. Inspect the current relevant implementation.
2. Identify existing reusable infrastructure and actual conflicts with this contract.
3. Preserve working interfaces and stored project compatibility.
4. Implement one real vertical slice at a time.
5. Validate through build, probes, runtime transitions and persisted output.
6. Report delivered behavior separately from incomplete targets.

Do not reopen settled UI/product decisions without a concrete incompatibility supported by
implementation evidence.

**Final product acceptance:**

A user can paint a professional product illustration from scratch, extract a performer from
video, convert that performer into editable illustration layers, refine the artwork
manually, reuse the subject across compositions, retain its animation, and reopen the
complete project without losing source media relationships, masks, transforms or artwork
history.

All of this occurs inside one cohesive, minimalist, canvas-first NAI Paintbrush workspace.

## Ownership boundaries

**SubjectAsset** — Original video · Identity · Segmentation

**SubjectInstance** — Timeline · Transform · Effects

**DerivedArtwork** — Contours · Fills · Brush strokes

**Unified Composition** — Painting + Illustration + Animation + Video

Derived artwork can also be instantiated and animated independently.

This prevents the most damaging design trap: coupling an idol's source identity, artistic
representation and composition placement into one mutable object.
