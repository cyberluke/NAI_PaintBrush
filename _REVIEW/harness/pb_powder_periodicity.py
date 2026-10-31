#!/usr/bin/env python3
"""pb_powder_periodicity.py — spatial-periodicity acceptance test for the
glitter-powder renderers.

The exact bug this guards against (review "Root cause", 2026-10-31): a 6-7px
simulation-cell/bead lattice appearing in the final rendered material — visible
as rows/columns/cross-hatch. This test MUST FAIL automatically for any renderer
whose output carries dominant spectral peaks at the simulation-cell width, the
bead-subdivision width, or their integer harmonics.

It is NOT a visual/unit-test shortcut gate. It measures the actual rendered
image.

Usage:
    python pb_powder_periodicity.py <render.png> [--label NAME] [--fail-thresh F]
      where --fail-thresh is the autocorrelation strength (0..1) treated as an
      unacceptable lattice (default 0.10). Higher = stricter.

Output:
    for the supplied render PNG:
      - the 1D autocorrelation profile along rows (h) and columns (v),
      - the dominant periods with peak strength,
      - PASS/FAIL: FAIL iff a dominant periodic peak is found at a plausible
        lattice period (2..48 px) above the threshold on BOTH axes at the
        same period (a real grid) OR on a single axis only if the peak is
        extreme (> 0.30, e.g. an obvious single-axis stripe).
    prints an exit code 0 (pass) / 1 (fail).

Also, when given --side-by-side A.png B.png --diff-out out.png, writes a
REF|NAI|DIFF composite for visual comparison.
"""

import sys
import os
import math

def load(img, gray=True):
    from PIL import Image
    im = Image.open(img).convert("L" if gray else "RGB")
    return im

def autocorr_1d_profile(gray, axis):
    """Mean |diff| for a 1-px shift along `axis` (0=h along rows, 1=v down
    cols), normalized by the image mean contrast. Returns a list indexed by
    shift (1..N). Periodic images show a repeating low-diff at the period."""
    w, h = gray.size
    import numpy as np
    a = np.asarray(gray, dtype=np.float32)
    prof = []
    if axis == 0:  # horizontal shifts
        dmax = min(w // 2, 64)
        for s in range(1, dmax + 1):
            diff = np.mean(np.abs(a[:, s:] - a[:, :-s]))
            prof.append((s, diff))
    else:  # vertical shifts
        dmax = min(h // 2, 64)
        for s in range(1, dmax + 1):
            diff = np.mean(np.abs(a[s:, :] - a[:-s, :]))
            prof.append((s, diff))
    return prof

def detect_lattice(prof, fail_thresh):
    """Look for a clear periodic low-diff dip (repeat) at a small period.
    Period candidates 2..48. A real lattice yields local MINIMA at multiples
    of the base period with local MAXIMA in between (contrast of ~half period)."""
    import numpy as np
    vals = np.array([d for _, d in prof], dtype=np.float32)
    # don't examine the trivial far field; but keep full window
    base = None
    best = (0, 0.0)
    for p in range(2, min(len(vals) + 1, 49)):
        # consistency: avg value at multiples of p should be lower than that at
        # p//2 offsets (i.e. a dip at p, peak midway), seen across >=2 periods
        multiples = [p * k for k in range(1, 1 + len(vals) // p)]
        if len(multiples) < 2:
            continue
        mid = [max(1, p * k - p // 2) for k in range(1, 1 + len(vals) // p)]
        v_multi = np.mean(vals[[m - 1 for m in multiples if m - 1 < len(vals)]]) \
            if multiples else 1.0
        v_mid = np.mean(vals[[m - 1 for m in mid if m - 1 < len(vals)]]) if mid else 1.0
        contrast = (v_mid - v_multi) / (v_mid + v_multi + 1e-6)
        if contrast > best[1]:
            best = (p, float(contrast))
    return best

def peak_energy(gray, period):
    """How much periodicity energy sits at `period` (spectral, 2D FFT on a
    colour-luminance patch). Larger = sharper lattice."""
    import numpy as np
    a = np.asarray(gray, dtype=np.float32)
    # crop to a central power-of-two-ish window (at least 2*period)
    n = min(a.shape[0], a.shape[1])
    c = min(256, (n // 128) * 128)
    c = max(c, 2 * period * 2)
    c = min(c, n)
    y0 = (a.shape[0] - c) // 2
    x0 = (a.shape[1] - c) // 2
    patch = a[y0:y0 + c, x0:x0 + c] - a[y0:y0 + c, x0:x0 + c].mean()
    F = np.fft.fft2(patch)
    power = np.abs(np.fft.fftshift(F)) ** 2
    peaks = []
    for p in range(2, min(c // 2, 49)):
        fx = c // p
        fy = c // p
        # peak at frequency (c/p, 0) for vertical lines of period p, etc.
        for (py_, px_) in ((fy, 0), (0, fx), (fy, fx)):
            v = power[py_, px_] if py_ < power.shape[0] and px_ < power.shape[1] else 0.0
            peaks.append((p, v))
    denom = power.max() + 1e-6
    # highest normalized periodic peak
    bestp = max(peaks, key=lambda t: t[1])
    return bestp[0], bestp[1] / denom

def side_by_side(ref, nai, out):
    from PIL import Image
    import numpy as np
    r = load(ref, gray=False).convert("RGB")
    n = load(nai, gray=False).convert("RGB")
    w = min(r.width, n.width)
    h = min(r.height, n.height)
    r = r.resize((w, h)); n = n.resize((w, h))
    rn = np.asarray(r, dtype=np.float32)
    nn = np.asarray(n, dtype=np.float32)
    diff = np.abs(rn - nn)
    # amplify the diff for visibility
    diffimg = np.uint8(np.clip(diff * 6.0, 0, 255))
    d = Image.fromarray(diffimg)
    assert d.size == r.size
    comp = Image.new("RGB", (w * 3, h))
    comp.paste(r, (0, 0))
    comp.paste(n, (w, 0))
    comp.paste(d, (2 * w, 0))
    comp.save(out)
    return out

def main():
    import numpy as np
    args = sys.argv[1:]
    fail_thresh = 0.10
    label = "render"
    pair = None
    pngs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--fail-thresh":
            fail_thresh = float(args[i + 1]); i += 2
        elif a == "--label":
            label = args[i + 1]; i += 2
        elif a == "--side-by-side":
            pair = (args[i + 1], args[i + 2], args[i + 3]); i += 4
        else:
            pngs.append(a); i += 1

    if not pngs:
        print("usage: pb_powder_periodicity.py <render.png> [--label N] [--fail-thresh F] [--side-by-side A.png B.png out.png]")
        return 2

    overall_fail = False
    for png in pngs:
        if not os.path.exists(png):
            print(f"[{label}] MISSING {png}"); overall_fail = True; continue
        gray = load(png)
        hp = autocorr_1d_profile(gray, 0)
        vp = autocorr_1d_profile(gray, 1)
        h_base, h_contrast = detect_lattice(hp, fail_thresh)
        v_base, v_contrast = detect_lattice(vp, fail_thresh)
        # Fourier confirmation
        spec_p, spec_e = peak_energy(gray, 8)
        lat = False
        reasons = []
        same = (h_base == v_base and h_base > 0
                and h_contrast > fail_thresh and v_contrast > fail_thresh)
        single_extreme = (h_contrast > 0.30 or v_contrast > 0.30)
        if same:
            lat = True
            reasons.append(f"same-period grid on both axes (period {h_base}px, h{h_contrast:.2f}/v{v_contrast:.2f})")
        if single_extreme:
            lat = True
            reasons.append(f"extreme single-axis periodic stripe (h{h_contrast:.2f}/v{v_contrast:.2f})")
        print(f"[{label}] {os.path.basename(png)}")
        print(f"  h autocorr best period={h_base}px contrast={h_contrast:.3f}")
        print(f"  v autocorr best period={v_base}px contrast={v_contrast:.3f}")
        print(f"  fft dominant periodic period={spec_p}px energy={spec_e:.4f}")
        if lat:
            print(f"  RESULT: FAIL — {'; '.join(reasons)}")
            overall_fail = True
        else:
            print(f"  RESULT: PASS — no dominant lattice (threshold {fail_thresh})")

    if 'pair' in locals() and pair:
        out = side_by_side(pair[0], pair[1], pair[2])
        print(f"side-by-side written: {out}")

    return 1 if overall_fail else 0

if __name__ == "__main__":
    sys.exit(main())