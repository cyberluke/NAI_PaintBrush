# NAI Paintbrush — Motion Toys: PuppetEffect spike report

**Date:** 2026-10-24
**Scope:** Motion Toys (NOT Phase 11, NOT a Showroom change). Isolated Puppet
runtime over HarvestItem-shaped assets (RGBA + binary mask), exactly the
AnimatedDrawings (MIT, archived 2025-09) algorithm-quarry port the roadmap
called for — no pose AI, no BVH import, no TorchServe, no GPU.
**Runtime:** `modules/gui/qt/paintbrush/motion/pb_puppet.{hpp,cpp}` (pure
C++17 + QtCore/QtGui value types, no QObject/moc, no VLC headers).
**Harness:** `_REVIEW/harness/pb_puppet_harness/build_run.ps1` → **92/92 pass,
0 fail, 0 skip** (compiles the REAL runtime sources).

## Stage 1 — the spike question — answered

> Can a harvested photographic person be deformed ±5–10° (ARAP over a
> mask-derived 2D mesh with a synthetic skeleton) and still look human?

**Yes — with margin.** On a REAL SAM3 dancer cutout (NewJeans "Hype Boy"
clip, `pb_live_harness` artifacts): the photo regime (±5° torso, ±7° arms,
counter-rotated head) moves the mesh **2.07 px (0.8% of bbox diag)**, flips
**0 triangles**, preserves silhouette **IoU 0.9993** and area ratio 1.000.
The identity pose reconstructs the cutout to **mean diff 2.76/255, 98.0%
within 3/255**. It does not look like a rubber demon.

The rubber-demon boundary is *measured*, not assumed: at ±35° elbows/head
the thin transition bands (neck, elbows) start inverting — **16/736 tris
(2.2%)**, silhouette IoU still 0.97; at ±60° → 18 flips, IoU 0.95. The
renderer skips flipped triangles (small holes, never a smear). So:
**photo regime = safe; illustration/doodle full-dance = needs the next
step** (blended bone weights / handle-ring rigid patches), as planned.

## Stage 2 — motion retargeting (the gate after acceptance)

The photo regime was accepted → retargeting landed, exactly per the plan:

```
PuppetMotionClip      keyframed pose track: linear angle interpolation,
                      Loop/Once/PingPong folding, byte-exact serialization
PuppetRetargeter      inverse FK (joint positions -> angles),
                      poseTrackFromMasks (source video as motion source),
                      name/alias joint mapping, PHOTO-REGIME ADAPTATION:
                      per-joint peak scaling into ±12° + root-displacement
                      bound (2% of target height — a sticker bobs, never
                      slides across the frame)
```

**The killer variant, proven on real data:** the three REAL SAM3 dancer
masks (A/B/C at different PTS) → per-mask skeleton fits → pose track →
retargeted onto a DIFFERENT figure (the synthetic humanoid): source peak
**177.2°** (dance poses 64 s apart) → retargeted peak **12.00°** (photo
regime), worst vertex move **2.3% diag**, **7 flips (0.95%, ≤ 3%)**,
silhouette **IoU 0.9974**, loop byte-seamless. *Best visual frame + motion
from the surrounding video → animated harvested sticker.* Proof:
`retarget_live_sheet.png` (4 phases of the synthetic figure dancing the
real dancer's motion), `retarget_live_source.png` (the three real fits).

Synthetic cross-proportions proof (chunky figure → slim figure): inverse-FK
roundtrip ≤ 6.66 px, keyframe sampling exact, interpolation linear, closed
loop pose(0)==pose(T) bitwise, mapping/clamp/root-scale semantics, clip
serialization byte-exact, end-to-end render stable (IoU 0.98, no material
flips). Small source motions pass through untouched (6° stays 6°).

## Stage 2 bugs found & fixed (harness catches real things)

1. **Unspecified evaluation order**: `QPointF(f32at(), f32at())` — C++17
   evaluates function arguments in unspecified order; the two reads with
   side effects swapped, corrupting the deserialized root offset. The
   byte-exact round-trip check caught it (first differing byte at the
   offset). Fixed with locals.
2. **Empty-row skeleton fit**: the chunky figure has a head–torso gap; the
   fit's shoulder row was empty, and the inside-mask clamp then dragged
   the shoulder joint ACROSS the figure to the far edge. Fixed:
   `rowAt()` — the fit now uses the nearest OCCUPIED row.

## What was implemented (both stages)

```
HarvestItem RGBA + mask
  -> PuppetBuilder      corner-cut contour (exact union boundary) -> RDP
                        -> ear-clip triangulation -> 1-4 subdivision
                        -> synthetic 10-joint humanoid fit (row profile,
                           nearest-occupied-row, every joint inside mask,
                           speckle-cleaned + hole-filled masks)
  -> PuppetDeformer     FK (per-joint degrees + root offset) -> ARAP
                        local-global solve (2D closed-form rotation,
                        matrix-free CG, damped handle ramp, λ=1e4)
  -> PuppetRenderer     canonical barycentric raster, bilinear sampling in
                        rest UV, binary-mask alpha with texture gating
  -> PuppetMotion       sway / bounce / breath / headTilt / composite,
                        seedable per-joint phases, Loop/Once/PingPong,
                        pose(0) == pose(duration) byte-exact
  -> PuppetMotionClip   keyframed pose tracks (source-motion carrier)
  -> PuppetRetargeter   inverse FK + mask-trajectory tracks + name mapping
                        + photo-regime adaptation (joints AND root)
PuppetEffect { kind, amplitude, speed, loop, seed, durationMs }
  -> byte-exact versioned serialization (19 B), rigId = FNV-1a 64 content hash
```

Design rules honored: deterministic everywhere (fixed iteration counts,
seedable hashes, no RNG), loopable by construction, no dependency on the
Showroom core — the document/layer/render pipeline is untouched.

## Verification (harness, real sources)

| Area | Result |
|---|---|
| Mesh | 465 verts / 736 tris; coverage 0.9931; build deterministic (byte-identical rebuild, stable rigId); rigId differs across inputs |
| Skeleton | 10 joints, valid hierarchy, all inside mask, spine order sane, every joint mapped to a handle vertex |
| Identity | deform(rest) == rest (≤1e-3); render byte-deterministic; vs masked cutout mean diff 1.30/255, 99.24% within 2/255 |
| Photo regime (amp 2 sway) | max move 3.94 px (0.8%); 0 flips; area 1.000; handles track FK ≤ 2 px; silhouette IoU 0.9983 |
| Illustration regime (±35°, head 15°) | 16 flips (2.17%, bounded ≤ 3%); IoU 0.9708; area 1.000; renderer skips flips |
| ±60° probe (report only) | 18 flips, IoU 0.9478, area 1.000 — the measured degradation zone |
| Motion semantics | bounce peaks up mid-cycle & rests at 0; breath NEVER moves the head (exact 0); headTilt isolates head+neck at 4°; amplitude scales angles exactly 2×; speed scales time; seed changes render; deterministic renderFrame |
| Loops | Sway/Bounce/Breath/HeadTilt/Composite: pose(0)==pose(T) and render byte-identical; Once holds rest; PingPong retraces |
| Serialization | round-trip byte-exact; garbage/truncated → default |
| Edge cases | empty mask → invalid rig; 12×12 blob IoU 1.0000; wide flat IoU ≥ 0.9; ring (hole) IoU ≥ 0.9 |
| REAL SAM3 photo | cutout 184×176 (dancer); coverage 0.9838; identity 2.76/255, 98.0% ≤3/255; photo regime 2.07 px, 0 flips, IoU 0.9993; loop seamless |
| Retargeting (synthetic) | inverse-FK roundtrip ≤ 6.66 px; key sampling exact; linear interp; closed loop bitwise; mapping/clamp/root-scale exact; clip serialization byte-exact; retargeted wave stable (IoU 0.98, ≤ 3% flips); small motions pass through (6° stays 6°) |
| Retargeting (REAL video) | 3 live SAM masks → track → retargeted onto a different figure: source 177.2° → 12.00° photo-regime cap; worst move 2.3% diag; flips 0.95%; IoU 0.9974; loop byte-seamless |
| Perf | renderFrame ~12–14 ms CPU (736-tris mesh, 30 frames) |

Proof artifacts in `_REVIEW/harness/pb_puppet_harness/build/`:
`synth_overlay.png` (mesh+skeleton on the figure), `synth_rest/sway/bounce/
breath/headtilt/composite_sheet/large_wave/extreme_60`, `photo_overlay.png`
(the real dancer rig!), `photo_rest/sway/bounce/composite_sheet`,
`retarget_synth_sheet/source`, `retarget_live_sheet/source` (the real
dancer's motion retargeted onto the synthetic figure),
`edge_12/wide/ring`.

## Bugs found & fixed along the way (worth knowing)

1. **Identity-pose ARAP distortion** — handles pinned vertices to the
   *joint* rest position, not the vertex rest; identity moved the mesh.
   Fixed: handles express the joint's FK target as a *relative* delta
   applied to the handle vertex (joint rides rigidly with it).
2. **Marching-squares diagonal pinches** — the canonical case-5/10 table
   emits self-intersecting figure-eights at diagonal contacts (AA jaggies,
   SAM noise); the winding interior silently dropped ~3.5% of mask pixels.
   Fixed with the **corner-cut model**: every grid corner connects its
   incident boundary-edge midpoints, with the explicit diagonal pairing
   rule (NW+SE → (N,E)+(S,W)). The polygon is now exactly the union
   boundary; every mask pixel center is inside.
3. **Bilinear texel centers** — sampling at `u = x+0.5` blended every
   pixel 50/50 with its neighbor; identity render smeared. Fixed by
   mapping into texel space (`u − 0.5`).
4. **Alpha semantics** — output alpha is the BINARY mask (the HarvestItem
   asset contract), with texture alpha gating interior holes; AA fringe
   pixels inside the mask now render at full alpha with their RGB.
5. **Speckle islands** — SAM masks carry disconnected noise pixels; the
   rig mask is now the speckle-cleaned original (≥ 0.5% of the largest
   component kept), the contour domain is hole-filled + cleaned.

## Honest limitations

- **Illustration regime is not full-dance yet**: ±35° starts inverting
  thin bands (2.2% flips). The planned fix is blended LBS weights /
  rigid handle-ring patches around joints, plus BVH-style retargeting of
  authored keyframes (the retargeter already maps arbitrary named
  skeletons — the harness proves the math on mask-derived tracks).
- **Mask-trajectory fit is row-profile, not pose estimation**: the source
  skeleton is fit per mask with the synthetic humanoid, so limbs that
  never reach a row extreme produce weak signals (the synthetic wave
  transferred 2.19° of its ~30° source swing; the real dance poses,
  which do move the silhouette extremes, transferred 177° → 12° fully).
  Strong limb tracking needs real pose estimation (a later, separate
  capability — explicitly out of scope for Motion Toys).
- The mesh is one connected component: separated body parts (SAM splits a
  hand, hair) are dropped by the speckle filter rather than rigged.
- No UI, no layer/effect wiring, no export loop yet — by design (spike +
  retargeting stages).
- Skeleton fit assumes a roughly upright standing figure; unusual poses
  need manual joints (the API takes an explicit joint list).

## Next step (when this stage is accepted)

`rigHarvest()` on the controller: HarvestItem → PuppetRuntime rig → subtle
source-motion loop (`PuppetMotionClip` from `poseTrackFromMasks` over the
surrounding video, or an authored clip) as a non-destructive effect — the
Motion Collage "living poster" (hero sway from real motion, badge pulse,
ghost poses static). Then illustration-regime hardening (blended weights)
and authored/BVH keyframe retargeting.