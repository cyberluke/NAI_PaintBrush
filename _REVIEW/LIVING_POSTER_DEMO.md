# Living Poster — 30-second investor demo

> **Don't generate the moment. Keep it alive.**

One flow, minutes long, every step a shipped and harness-verified path.
No debugging session: if a step misbehaves, see the troubleshooting table.

## The one-liner

**Living Poster** — harvest a real moment from video and bring it subtly
back to life.

## The demo video

`_REVIEW/harness/pb_live_harness/build/video_input.mkv` (580 MB) — the
dancer clip that produced the real SAM3 masks used in the motion harness
(`live_frame_A/B/C`). Use it; the rig path is already proven on this exact
footage. (Any 10–30 s clip of a single person on a clean background works
the same way.)

## Launch

```powershell
pwsh scripts/launch-pb-noskip.ps1
```

## The flow (target: under 5 minutes)

| # | Step | Where | Expect |
|---|---|---|---|
| 1 | Open `video_input.mkv` | Media drawer → open file | video plays in the source panel |
| 2 | Pause on a full-body frame | space | sharp, upright pose |
| 3 | Select the person | object-select tool, drag over the figure | mask preview |
| 4 | **Keep** | Keep button | pose lands in the Harvest Tray (exact frame + mask) |
| 5 | Repeat 2× at nearby moments (a few hundred ms apart, slightly different poses) | — | tray holds 3 moments of the same person |
| 6 | Drag all 3 onto the canvas | tray → canvas | three poses composed |
| 7 | **Compose** | inspector / compose action | arranged composition |
| 8 | **Luxury Greige** paper | Paper picker | paper + light rig applied |
| 9 | Glitter title | text tool + glitter | title sparkles |
| 10 | **Boost** | Boost button | roles, motion blur, shadow, bridge pose (SAM3) — one undo step |
| 11 | **Make it living** | right-click the hero pose in the tray | asset rigged (auto motion window) |
| 12 | Select the hero layer → **Motion: Normal** | inspector Motion group | **the figure starts moving on the canvas** |
| 13 | **Compare** switch | inspector Motion group | static poster vs living loop — the before/after moment |
| 14 | **Export Living Poster video (MP4)** | top bar, filmstrip icon | `living_poster.mp4` + the canonical PNG frames beside it |

## The talking points (in order)

1. **VLC as media source** — exact frame provenance, no re-encoding.
2. **SAM3** — the selection became a mask in one Keep.
3. **Exact video provenance** — every pose carries its source time.
4. **Creative composition** — deterministic Art Materials (paper, light, glitter).
5. **AI orchestration** — Boost decided roles and placed a real bridge pose.
6. **Source-aware motion** — the hero moves with *its own* subtle motion from
   the video, photo-safe (never raw), one undo restores the static figure.
7. **Zero diffusion video generation** — nothing was generated.

Pointa: **Nevygenerovali jsme video. Oživili jsme skutečný moment.**

## Export cheat sheet

- **Primary**: `Living Poster video (MP4)` — H.264/yuv420p, share-ready,
  enabled only when the mux pipeline is verified present.
- **Advanced menu**: WebM alpha (transparent, for compositing) + PNG frames
  (canonical, deterministic — the test oracle).
- The MP4 muxes from the same frames the tests assert: 3 s / 24 fps,
  frame 0 == the static poster, loop-seamless.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| MP4 button disabled | ffmpeg (with libx264) missing — install ffmpeg or add to PATH; the button never lies |
| Motion group hidden | no rigged asset yet — step 11 |
| Preview doesn't move | clock gates: layer hidden, workspace minimized, or Compare switch on |
| Bridge pose skipped | Boost still works structurally; the note in the status bar says so |
| Live loop feels off | see the feel sheets: `living_live_feel_sheet.png` / `living_synth_feel_sheet.png`
  (Subtle/Normal/Strong × 12 frames) — tune amplitude default (0.65), speed
  or phase in `PuppetAsset::defaultAmplitude` / `PuppetEffect` after an
  eyeball audit, never blind |

## Audit checklist before a real showing (30 Hz visual feel)

Eyes-on verdict (2026-10-24, feel sheets): **checkpoint frozen** —
synthetic Strong is clean (no tearing/rubber/silhouette breakdown), the
solid-blue live mask is too subtle a perceptual proxy for a textured dancer
(no reason to lower anything), paper surface reads well. The fixture
`living_doc_t0/t1` confirms determinism/static semantics, not motion feel.

Product scale (fixed, not tunable per-session):

```text
Subtle  0.35   barely alive
Normal  0.65   product default
Strong  1.00   clearly alive, still photo-safe
```

The one GUI decision rule:

> At 100% zoom, within the first ~0.5 s you should clearly see the hero is
> alive, but not perceive the deformation as an effect → **0.65 is correct**.
> If you have to stare for two seconds to see any movement, go straight to
> **0.75** — no 0.67 → 0.69 → 0.71 iteration. Strong stays 1.00 as max.

- [ ] Motion looks alive, not robotic (sway arcs, counter-movements visible)
- [ ] Normal (0.65) reads as "subtle life" at 100% zoom within ~0.5 s
- [ ] Strong (1.0) still photo-safe
- [ ] Loop seam invisible (no pop at the wrap)
- [ ] Compare switch flips instantly between static poster and living loop
- [ ] Export MP4 plays back identically to the preview

If `Normal` reads well on the **textured dancer in the real composition**,
the freeze is definitive — no further engine work.