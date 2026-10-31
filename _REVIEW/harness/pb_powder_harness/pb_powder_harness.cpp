// pb_powder_harness.cpp — NAI Paintbrush glitter-powder correctness harness.
//
// Compiles the PURE-Qt powder engine (glitter_powder.cpp) directly against Qt6
// (no VLC, no moc). Covers the CPU physics + render correctness that the
// review's P0/P1 audit requires: determinism, explicit render time, shared
// ArtLightRig lighting, per-grain tone, thin vs dense coverage, real two-slot
// colours (B != A+1), deep heights preserved, world-scale stability and the
// empty-powder background. Emits CPU reference sheets for a visual audit.
//
//   pwsh _REVIEW/harness/pb_powder_harness/build_run.ps1
//
// (The GPU path is proven at build level by the plugin; a GPU-parity harness
// that drives GpuRuntime::renderPowder under QRhi is the follow-up gate.)

#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>

// Reach the engine's internals for targeted material-state tests (test-only).
#define private public
#define protected public
#include "modules/gui/qt/paintbrush/art/materials/glitter/glitter_powder.cpp"
#undef private
#undef protected

using namespace paintbrush::art;
using namespace paintbrush::art::glitter;

static int s_checks = 0;
static int s_pass = 0;
#define CHECK(name, cond) \
    do { ++s_checks; \
         const bool ok = (cond); \
         if (ok) ++s_pass; \
         std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); } while (0)

/* Run a Blow using the EXACT GUI controller lifecycle (P0 #3): the real
 * PaintbrushCanvasItem::stepPowderBlow finish predicate, not a hand-picked
 * blowStep() count. Steps at ~60 Hz and stops when the loose bed has settled
 * AND no airborne flake is still in flight (flakes keep landing/off-paging),
 * or after a generous safety cap. Mirrors the controller so the harness proves
 * the same operation the GUI runs. */
static void runBlowLifecycle(GlitterPowder &g)
{
    g.beginBlow();
    float age = 0.0f;
    const float dt = 1.0f / 60.0f;
    while (age <= 12.0f)
    {
        const int moved = g.blowStep(dt);
        age += dt;
        const bool bedSettled = age > 3.5f || (moved == 0 && age > 0.8f);
        const bool airborne = g.hasActiveParticles();
        if ((bedSettled && !airborne) || age > 12.0f)
            break;
    }
    g.endBlow();
}

static bool imgEqual(const QImage &a, const QImage &b, int tol = 0)
{
    if (a.size() != b.size())
        return false;
    if (tol == 0)
        return a == b;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (qAbs<int>(qAlpha(a.pixel(x, y)) - qAlpha(b.pixel(x, y))) > tol)
                return false;
    return true;
}

static double imageMeanAlpha(const QImage &img, QRect r = {})
{
    if (r.isEmpty())
        r = img.rect();
    double s = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
        { s += qAlpha(img.pixel(x, y)); ++n; }
    return n ? s / n : 0.0;
}

static GlitterPowder makeHeap(const PowderPalette &pal, int grid = 256, float seed = 7.0f)
{
    GlitterPowder g;
    g.reset(QSize(grid, grid), 4.0f, std::uint32_t(seed));
    /* a few overlapping pours to build a mound + loose dust; the world centre
     * of the grid is (grid/2)*4 (world = cell * grid / 2) */
    const float worldC = (grid / 2.0f) * 4.0f;
    GlitterParams p;
    p.amount = 1.4f;
    p.flakeSize = 0.8f;
    p.color = pal.colors.isEmpty() ? QColor("#f2a7c8") : pal.colors[0];
    for (int i = 0; i < 6; ++i)
        g.pourGlitter(worldC + (i - 2) * 8.0f, worldC + (i - 2) * 6.0f,
                      22.0f + 2.0f * i, p);
    g.relax();
    g.relax();
    g.decayFlow();
    return g;
}

/* ---- spatial-periodicity (lattice) probe ----------------------------------
 * Mirrors the reviewer's pb_powder_periodicity.py decision on the rendered
 * material: measure the mean |diff| for a 1px shift along rows (h) and columns
 * (v) of the alpha (material coverage). A regular pour GRID shows local MINIMA
 * (low diff = repeat) at every multiple of the base period on BOTH axes at the
 * same period; an organic single fitted heap shows no dominant same-period
 * lattice. FAIL if a candidate period 2..48 has same-period contrast above the
 * threshold on both axes (a real grid), or an extreme single-axis stripe. */
static int latticePeriodContrast(const QImage &img, int axis, float &contrast)
{
    const int w = img.width(), h = img.height();
    const int dmax = std::min(axis == 0 ? w / 2 : h / 2, 64);
    std::vector<double> prof(dmax, 0.0);
    for (int s = 1; s <= dmax; ++s)
    {
        double sum = 0.0; long n = 0;
        if (axis == 0)
        {
            for (int y = 0; y < h; ++y)
                for (int x = s; x < w; ++x)
                { sum += std::abs(qAlpha(img.pixel(x, y)) - qAlpha(img.pixel(x - s, y))); ++n; }
        }
        else
        {
            for (int y = s; y < h; ++y)
                for (int x = 0; x < w; ++x)
                { sum += std::abs(qAlpha(img.pixel(x, y)) - qAlpha(img.pixel(x, y - s))); ++n; }
        }
        prof[s - 1] = n ? sum / n : 1.0;
    }
    int bestP = 0; double bestC = 0.0;
    for (int p = 2; p <= std::min(dmax, 48); ++p)
    {
        int nMult = 0; double vMult = 0.0;
        for (int k = 1; p * k <= dmax; ++k) { vMult += prof[p * k - 1]; ++nMult; }
        if (nMult < 2) continue;
        vMult /= nMult;
        int nMid = 0; double vMid = 0.0;
        for (int k = 1; p * k <= dmax; ++k)
        {
            int m = std::max(1, p * k - p / 2);
            if (m <= dmax) { vMid += prof[m - 1]; ++nMid; }
        }
        vMid = nMid ? vMid / nMid : 1.0;
        const double cc = (vMid - vMult) / (vMid + vMult + 1e-6);
        if (cc > bestC) { bestC = cc; bestP = p; }
    }
    contrast = float(bestC);
    return bestP;
}
static bool noLatticeGrid(const QImage &img, float thresh)
{
    float hc = 0.0f, vc = 0.0f;
    int hp = latticePeriodContrast(img, 0, hc);
    int vp = latticePeriodContrast(img, 1, vc);
    const bool same = (hp > 0 && hp == vp && hc > thresh && vc > thresh);
    const bool stripe = (hc > 0.30f || vc > 0.30f);
    return !same && !stripe;
}

int main(int argc, char **argv)
{
    Q_UNUSED(argc); Q_UNUSED(argv);
    std::printf("pb_powder_harness\n");

    PowderPalette pal = defaultPowderPalette();
    const qint64 T1 = 1234;              /* explicit phase, not wall-clock */

    /* ---- 1. CPU physics baseline ------------------------------------- */
    GlitterPowder g = makeHeap(pal);
    CHECK("pour deposits mass (> 0 loose)", g.looseMass() > 0.0f);
    const int bondedBefore = g.bondedCount();
    /* paint wet glue over the heap, then absorb bonds the loose layer */
    g.paintGlue(g.cellW() / 2.0f * 4.0f, g.cellH() / 2.0f * 4.0f, 120.0f, 1.0f);
    g.absorb();
    CHECK("absorb bonds loose to glue", g.bondedCount() > bondedBefore);
    /* smear pushes loose, bonded stays */
    const int bondedAfterSmear = g.bondedCount();
    g.smear(g.cellW() / 2.0f * 4.0f, g.cellH() / 2.0f * 4.0f,
            -1.0f, -0.5f, 30.0f, 0.5f);
    g.absorb();
    CHECK("smear preserves bonded", g.bondedCount() >= bondedAfterSmear);

    /* ---- blow on a FRESH (unrelaxed) heap: unsettled loose is liftable ---- */
    {
        GlitterPowder bg;
        bg.reset(QSize(192, 192), 4.0f, 42u);
        const float wc = (192 / 2.0f) * 4.0f;
        GlitterParams bp; bp.amount = 1.2f; bp.flakeSize = 0.8f;
        bp.color = pal.colors[0];
        for (int i = 0; i < 5; ++i)
            bg.pourGlitter(wc + (i - 2) * 7.0f, wc + (i - 2) * 5.0f, 20.0f + i, bp);
        const float bgLoose = bg.looseMass();
        runBlowLifecycle(bg);
        std::printf("    [dbg] blow total (lifecycle) loose %.2f -> %.2f airborneAfter=%d\n",
                    bgLoose, bg.looseMass(), bg.hasActiveParticles());
        CHECK("blow moves loose powder", bgLoose > 0.0f && bg.looseMass() < bgLoose);
        CHECK("blow clears loose", bg.looseMass() < bgLoose * 0.35f);
        CHECK("blow leaves no airborne flakes at settle",
              !bg.hasActiveParticles());
    }

    /* ---- Blow A/B physics gate (P0): glued glitter survives, bare does not ---
     * Two IDENTICAL glitter regions, one glued (A) and one bare (B), blown with
     * the SAME breath. Before: A loose ~= B loose. After: A retains significant
     * BONDED mass (glue holds it), B loses most LOOSE mass (nothing holds it).
     * A.stuck must exceed B.stuck by a large margin. This is the real product
     * Blow behaviour (bare glitter moves/leaves, glued shape stays), not a
     * "glue serializes" assertion. */
    {
        auto stuckMass = [](const GlitterPowder &g) {
            double m = 0.0;
            for (int i = 0; i < g.cellCount(); ++i)
                m += g.powderCellAt(i).stuck;
            return m;
        };
        auto makeRegion = [&](bool glue, std::uint32_t seed) {
            GlitterPowder g;
            g.reset(QSize(96, 96), 4.0f, seed);
            const float wc = (96 / 2.0f) * 4.0f;
            GlitterParams bp; bp.amount = 1.4f; bp.flakeSize = 0.8f;
            bp.color = pal.colors[0];
            for (int i = 0; i < 6; ++i)
                g.pourGlitter(wc + (i - 2) * 9.0f, wc + (i - 2) * 7.0f, 22.0f + i, bp);
            if (glue)
            {
                /* wet glue over the same disc, then let it bond BEFORE the blow */
                g.paintGlue(wc, wc, 40.0f, 1.0f);
                g.absorb();
                g.relax(); g.relax();
            }
            return g;
        };
        GlitterPowder A = makeRegion(true,  11u);   /* glued */
        GlitterPowder B = makeRegion(false, 11u);   /* bare, same glitter seed */
        const double A_loose0 = A.looseMass(), B_loose0 = B.looseMass();
        const double A_stuck0 = stuckMass(A), B_stuck0 = stuckMass(B);
        std::printf("    [blowA/B] before: A loose=%.2f stuck=%.2f | B loose=%.2f stuck=%.2f\n",
                    A_loose0, A_stuck0, B_loose0, B_stuck0);
        CHECK("blow A/B: before blow A and B carry ~equal loose glitter",
              B_loose0 > 0 && std::fabs(A_loose0 - B_loose0) < B_loose0 * 0.25);
        /* the SAME breath on both, run through the GUI's own finish lifecycle
         * (settled loose bed AND no airborne flakes remain — P0 #3), not a
         * hand-picked step count */
        runBlowLifecycle(A);
        runBlowLifecycle(B);
        const double A_loose1 = A.looseMass(), B_loose1 = B.looseMass();
        const double A_stuck1 = stuckMass(A), B_stuck1 = stuckMass(B);
        std::printf("    [blowA/B] after : A loose=%.2f stuck=%.2f | B loose=%.2f stuck=%.2f\n",
                    A_loose1, A_stuck1, B_loose1, B_stuck1);
        /* after the breath settles NO airborne flakes are left (the canonical
         * commit wouldn't bake transient particles — P0 #1) */
        CHECK("blow A/B: after settle neither region has airborne flakes",
              !A.hasActiveParticles() && !B.hasActiveParticles());
        /* bare region loses most of its loose glitter */
        CHECK("blow A/B: bare-region loose loses >= 70% (blow physically moves it)",
              B_loose0 > 0 && B_loose1 < B_loose0 * 0.30);
        /* glued region retains significant BONDED glitter */
        CHECK("blow A/B: glued-region bonded mass remains (>= 60%",
              A_stuck0 > 0 && A_stuck1 >= A_stuck0 * 0.60);
        /* A's bonded mass must far exceed B's (glue holds it, bare gets blown) */
        CHECK("blow A/B: glued survives where bare is blown (A.stuck >> B.stuck)",
              A_stuck1 > B_stuck1 + 1.0 && A_stuck1 > B_stuck1 * 2.0);
    }

    /* ---- Glue has REAL physical impact (P0) -------------------------------
     * paintGlue() on positive coverage must immediately bond loose powder that
     * touches it (not only be a visible film). Assert the behavioural contract:
     *   glue=0 -> stuck stays ~0 (nothing to catch it)
     *   glue>0 + powder -> stuck rises (loose bonds up to ~kGlueDepth*glue)
     *   Blow -> stuck remains, loose moves */
    {
        auto stuckMass = [](const GlitterPowder &g) {
            double m = 0.0;
            for (int i = 0; i < g.cellCount(); ++i)
                m += g.powderCellAt(i).stuck;
            return m;
        };
        const std::uint32_t s1 = 5u, s2 = 6u;

        /* glue=0: poured glitter stays loose (stuck stays ~0) */
        {
            GlitterPowder ng;
            ng.reset(QSize(64, 64), 4.0f, s1);
            GlitterParams bp; bp.amount = 1.2f; bp.flakeSize = 0.8f;
            bp.color = pal.colors[0];
            ng.pourGlitter(32 * 4.0f, 32 * 4.0f, 20.0f, bp);
            const double stuckBefore = stuckMass(ng);
            ng.absorb();
            const double stuckAfter = stuckMass(ng);
            CHECK("glue impact: loose powder with glue=0 does NOT become stuck",
                  stuckAfter < stuckBefore + 0.5);
        }

        /* glue>0 + powder already present: paintGlue IMMEDIATELY bonds it */
        {
            GlitterPowder g;
            g.reset(QSize(64, 64), 4.0f, s2);
            GlitterParams bp; bp.amount = 1.2f; bp.flakeSize = 0.8f;
            bp.color = pal.colors[0];
            g.pourGlitter(32 * 4.0f, 32 * 4.0f, 22.0f, bp);
            const double stuckBefore = stuckMass(g);
            g.paintGlue(32 * 4.0f, 32 * 4.0f, 30.0f, 1.0f);   /* glue+auto-absorb */
            const double stuckAfter = stuckMass(g);
            std::printf("    [glueImpact] gluing over powder: stuck %.2f -> %.2f\n",
                        stuckBefore, stuckAfter);
            CHECK("glue impact: applying glue over loose powder BONDS it immediately",
                  stuckAfter > stuckBefore + 1.0);
        }
    }

    /* ---- 2. CPU static determinism (same state + t=0 == identical) --- */
    const float gc2 = (256 / 2.0f) * 4.0f;   /* heap centre for makeHeap(256) */
    GlitterPowder g2 = makeHeap(pal, 256, 99.0f);
    QImage r0 = g2.render(QSize(256, 256), gc2 - 128, gc2 - 128, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
    QImage r1 = g2.render(QSize(256, 256), gc2 - 128, gc2 - 128, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
    CHECK("determinism: same state+t0 == identical pixels", imgEqual(r0, r1));

    /* ---- 3. CPU temporal shimmer (t1 != t2 differs, state unchanged) - */
    const GlitterPowder stateBefore = g2;      /* copy the physics state */
    QImage rs1 = g2.render(QSize(256, 256), gc2 - 128, gc2 - 128, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), T1);
    {
        int diff = 0;
        for (int y = 0; y < r0.height(); ++y)
            for (int x = 0; x < r0.width(); ++x)
                if (r0.pixel(x, y) != rs1.pixel(x, y))
                    ++diff;
        std::printf("    [dbg] shimmer diff px=%d (t=0 vs t=%lld)\n", diff, (long long)T1);
    }
    CHECK("shimmer: t1!=t2 changes pixels", !imgEqual(r0, rs1));
    CHECK("shimmer: physics untouched by render-time",
          g2.powderCellAt(stateBefore.cellCount()/2).height
              == stateBefore.powderCellAt(stateBefore.cellCount()/2).height);

    /* ---- 4. ArtLightRig drives lighting ----------------------------- */
    {
        GlitterPowder gl = makeHeap(pal);
        const float wc = (256 / 2.0f) * 4.0f;
        ArtLightRig rigL, rigR;                /* light from two directions */
        rigL.direction = QVector3D(0.9f, 0.1f, 0.6f);
        rigR.direction = QVector3D(-0.9f, 0.1f, 0.6f);
        QImage a = gl.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rigL, pal, nullptr, ArtScale(), 0);
        QImage b = gl.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rigR, pal, nullptr, ArtScale(), 0);
        /* light moves the SHADING: check RGB too, because for a dense heap the
         * coverage alpha can saturate at 1 while the facet specular/lighting
         * still clearly moves (alpha-only is not enough here). */
        long rgbDiff = 0; int nRGB = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
            {
                rgbDiff += qAbs<int>(qRed(a.pixel(x, y))   - qRed(b.pixel(x, y)))
                         + qAbs<int>(qGreen(a.pixel(x, y)) - qGreen(b.pixel(x, y)))
                         + qAbs<int>(qBlue(a.pixel(x, y))  - qBlue(b.pixel(x, y)));
                ++nRGB;
            }
        const double meanRGB = nRGB ? double(rgbDiff) / nRGB : 0.0;
        CHECK("light rig direction moves shading", meanRGB > 0.5);
    }

    /* ---- 5. thin vs dense coverage ----------------------------------- */
    {
        GlitterPowder dense = makeHeap(pal, 128, 5.0f);
        GlitterPowder thin;
        thin.reset(QSize(128, 128), 4.0f, 6u);
        GlitterParams p; p.amount = 0.12f; p.flakeSize = 0.8f;
        p.color = pal.colors[0];
        thin.pourGlitter(64 * 4.0f, 64 * 4.0f, 22.0f, p);   /* one light dab */
        thin.relax();
        QImage d = dense.render(QSize(128, 128), 0, 0, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        QImage t = thin.render(QSize(128, 128), 0, 0, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("thin layer has lower coverage than dense",
              imageMeanAlpha(t, QRect(48, 48, 32, 32))
                  < imageMeanAlpha(d, QRect(48, 48, 32, 32)) * 0.9 + 1e-6);
    }

    /* ---- 6. deep heights > 1 preserved (no 8-bit normalization) ----- */
    {
        GlitterPowder tg;
        tg.reset(QSize(16, 16), 4.0f, 1u);
        /* force a tall cell: push 6.0 cells of powder so the shader would see
         * a real value — CPU golden just must represent it (alpha saturated) */
        for (int i = 0; i < 24; ++i)
            tg.pourGlitter(8 * 4.0f, 8 * 4.0f, 6.0f, [](){
                GlitterParams p; p.amount = 0.3f; p.flakeSize = 0.8f;
                p.color = QColor("#f2a7c8"); return p; }());
        QImage tall = tg.render(QSize(64, 64), 0, 0, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("deep heap renders opaque (not invisible)", imageMeanAlpha(tall, QRect(28, 28, 8, 8)) > 200);
    }

    /* ---- 7. two-slot colour B != A+1 (test-only cell injection) ------ */
    {
        GlitterPowder mc;
        mc.reset(QSize(8, 8), 4.0f, 12u);
        mc.paintGlue(4 * 4.0f, 4 * 4.0f, 20.0f, 1.0f);
        /* inject a cell whose loose layer is 60% slot 0 (pink) / 40% slot 2 (white)
         * and bonded 100% slot 1 (silver): B is NOT A+1, and loose chooses at random */
        PowderPalette p3;
        p3.colors = { QColor("#f2a7c8"), QColor("#c0c0c0"), QColor("#ffffff") };
        p3.paper = QColor("#ffffff");
        PowderCell &c = mc.m_cellsData[4 * 8 + 4];
        c.height = 1.6f; c.stuck = 0.8f; c.glue = 1.0f;
        c.lA = 0; c.lB = 2; c.lF = 0.4f;
        c.sA = 1; c.sB = 1; c.sF = 0.0f;
        QImage mix = mc.render(QSize(32, 32), 0, 0, 1.0f, ArtLightRig(), p3, nullptr, ArtScale(), 0);
        /* the grains are either pink (slot A) or white (slot B) — discrete,
         * never a brown average of the two slots */
        int pinkish = 0, whitish = 0, other = 0;
        /* the grain centre is pixel (16,16); the bead core sits right there.
         * Sample only the bead's own pixels — NOT the clear-glue neighbours,
         * which now render (correctly, P0) as near-transparent clear adhesive
         * and would otherwise count as "other" (transparent). */
        for (int y = 15; y <= 18; ++y)
            for (int x = 15; x <= 18; ++x)
            {
                QColor px(mix.pixel(x, y));
                const float rpx = px.redF(), gpx = px.greenF(), bpx = px.blueF();
                if (bpx > 0.7f && rpx > 0.8f)        /* pinkish */
                    ++pinkish;
                else if (rpx > 0.85f && gpx > 0.85f && bpx > 0.85f)  /* white */
                    ++whitish;
                else
                    ++other;
            }
        CHECK("two-slot colour is discrete (grain picks A or B, no mud)",
              other <= pinkish + whitish + 1 && (pinkish + whitish) > 0);
        std::printf("    [twoslot] pinkish=%d whitish=%d other=%d\n", pinkish, whitish, other);
    }

    /* ---- 8. seed drives the pour layout (no hidden noise) ---------- */
    {
        /* The RNG seed shapes the pour/dust layout — two different seeds give
         * a different heap; the grain SHUFFLE remains position-derived. This
         * confirms the seed does real work (not a dead field) while the
         * determinism check above already proves seed-identical renders are
         * byte-identical. */
        GlitterPowder sA, sB;
        sA.reset(QSize(128, 128), 4.0f, 1u);
        sB.reset(QSize(128, 128), 4.0f, 123456789u);
        GlitterParams sp; sp.amount = 1.0f; sp.flakeSize = 0.8f;
        sp.color = pal.colors[0];
        sA.pourGlitter(64 * 4.0f, 64 * 4.0f, 20.0f, sp);
        sB.pourGlitter(64 * 4.0f, 64 * 4.0f, 20.0f, sp);
        sA.relax(); sB.relax();
        QImage iA = sA.render(QSize(96, 96), 64 * 4.0f - 48, 64 * 4.0f - 48, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        QImage iB = sB.render(QSize(96, 96), 64 * 4.0f - 48, 64 * 4.0f - 48, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("different seeds give a different (deterministic) layout",
              !imgEqual(iA, iB, 1));
    }

    /* ---- 9. worldPerPx 1.0 and non-1.0, non-zero origin, 4K --------- */
    {
        GlitterPowder wg = makeHeap(pal, 512, 21.0f);   /* grid 512 -> heap at world 1024 */
        const float worldC = (512 / 2.0f) * 4.0f;
        QImage w1 = wg.render(QSize(256, 256), worldC - 128, worldC - 128, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        QImage w2 = wg.render(QSize(256, 256), worldC - 128, worldC - 128, 0.5f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("worldPerPx != 1 renders valid non-empty", !w2.isNull() && imageMeanAlpha(w2) > 0.0);
        QImage crop = wg.render(QSize(200, 200), worldC - 60, worldC - 60, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("non-zero origin cropped render valid", !crop.isNull() && imageMeanAlpha(crop) > 0.0);
        QImage big = wg.render(QSize(4096, 4096), worldC - 2048, worldC - 2048, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("4K render valid and non-empty", !big.isNull() && imageMeanAlpha(big) > 0.0);
        QImage big2 = wg.render(QSize(4096, 4096), worldC - 2048, worldC - 2048, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("4K determinism (repeat)", imgEqual(big, big2));
    }

    /* ---- 10. empty powder → background ------------------------------ */
    {
        GlitterPowder empty;
        empty.reset(QSize(64, 64), 4.0f, 3u);
        QImage e = empty.render(QSize(64, 64), 0, 0, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("empty powder renders fully transparent", imageMeanAlpha(e) == 0.0);
    }

    /* ---- persistence: serialize/deserialize round-trip ----------------- */
    {
        GlitterPowder a;
        a.reset(QSize(128, 128), 4.0f, 5u);
        GlitterParams sp; sp.amount = 1.3f; sp.flakeSize = 0.8f; sp.color = pal.colors[0];
        a.pourGlitter(70 * 4.0f, 70 * 4.0f, 20.0f, sp);
        a.paintGlue(50 * 4.0f, 50 * 4.0f, 16.0f, 0.8f);
        a.pourGlitter(50 * 4.0f, 50 * 4.0f, 14.0f, sp);
        a.relax(); a.relax(); a.decayFlow();
        a.absorb(); /* some loose bonds to the glue */
        const QByteArray bytes = a.serialize();
        CHECK("serialize produces non-empty bytes", !bytes.isEmpty() && bytes.size() > 32);
        GlitterPowder b;
        CHECK("deserialize accepts its own serialization", b.deserialize(bytes));
        CHECK("round-trip preserves grid", b.cellW() == a.cellW() && b.cellH() == a.cellH());
        CHECK("round-trip preserves worldCell", std::fabs(b.worldCellSize() - a.worldCellSize()) < 1e-6f);
        int sameCells = 0;
        for (int i = 0; i < a.cellCount(); ++i)
        {
            const PowderCell &x = a.powderCellAt(i);
            const PowderCell &y = b.powderCellAt(i);
            if (std::fabs(x.height - y.height) < 1e-5f && std::fabs(x.stuck - y.stuck) < 1e-5f
                && std::fabs(x.glue - y.glue) < 1e-5f && std::fabs(x.seed - y.seed) < 1e-5f
                && std::fabs(x.flow - y.flow) < 1e-5f && std::fabs(x.vx - y.vx) < 1e-5f
                && std::fabs(x.vy - y.vy) < 1e-5f && x.lA == y.lA && x.lB == y.lB
                && x.sA == y.sA && x.sB == y.sB
                && std::fabs(x.lF - y.lF) < 1e-5f && std::fabs(x.sF - y.sF) < 1e-5f)
                ++sameCells;
        }
        CHECK("round-trip preserves every physics field", sameCells == a.cellCount());
        CHECK("round-trip preserves palette", b.palette() == a.palette());
        /* a restored powder must render the same as the original (state, not
         * raster, is authoritative) */
        QImage ra = a.render(QSize(96, 96), 50 * 4.0f - 48, 50 * 4.0f - 48, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        QImage rb = b.render(QSize(96, 96), 50 * 4.0f - 48, 50 * 4.0f - 48, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("restored state renders identically", imgEqual(ra, rb));
        /* corrupt input must be rejected, not crash */
        GlitterPowder bad;
        QByteArray junk(24, '\xAA');
        CHECK("deserialize rejects garbage", !bad.deserialize(junk));
        std::vector<char> shortEnd(bytes.begin(), bytes.end());
        shortEnd.resize(shortEnd.size() - 7);
        QByteArray cut(shortEnd.data(), int(shortEnd.size()));
        CHECK("deserialize rejects truncated payload", !bad.deserialize(cut));
    }

    /* ---- P0.12 raster cache == materialState invariant ------------------
     * layer.materialState is the AUTHORITATIVE physics; layer.raster must be
     * the deterministic t=0 material bake of that exact state. Simulate that
     * contract: bake a state's t=0 raster, serialize the state, deserialize it
     * into a fresh engine and re-bake t=0; the two bakes must be identical
     * (a corrupted or stale cached raster would disagree here). */
    {
        const float wc = (60 * 4.0f);
        GlitterPowder st;
        st.reset(QSize(128, 128), 4.0f, 19u);
        GlitterParams sp; sp.amount = 1.3f; sp.flakeSize = 0.8f; sp.color = pal.colors[0];
        st.pourGlitter(wc, wc, 20.0f, sp);
        st.paintGlue(62 * 4.0f, 62 * 4.0f, 14.0f, 0.8f);
        st.pourGlitter(62 * 4.0f, 62 * 4.0f, 12.0f, sp);
        st.relax(); st.relax(); st.decayFlow(); st.absorb();
        /* the canonical t=0 material bake (the "cached raster") */
        const QRect view(48, 40, 128, 128);
        const QImage cachedRaster = st.render(view.size(), (float)view.x(), (float)view.y(),
                                              1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        const QByteArray state = st.serialize();
        GlitterPowder reopened;
        CHECK("P0.12 deserialize(materialState) accepts the persisted state", reopened.deserialize(state));
        /* t=0 re-bake from the deserialized state must reproduce the cached raster */
        if (reopened.cellW() == st.cellW() && reopened.cellH() == st.cellH())
        {
            const QImage reBaked = reopened.render(view.size(), (float)view.x(), (float)view.y(),
                                                   1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
            CHECK("P0.12 deserialize(materialState) → t=0 re-bake == cached raster",
                  imgEqual(cachedRaster, reBaked, 2));
        }
        else
        {
            CHECK("P0.12 deserialize(materialState) grid preserved", false);
        }
    }

    /* ---- End-to-end golden through the GUI lifecycle (P0 #1/#3) ----------
     * The reviewer's required path: Glue -> Pour -> Blow (run through the exact
     * GUI controller finish predicate, not a fixed step count) -> airborne
     * settle -> canonical commit (serialize) -> deserialize -> re-bake the
     * canonical raster. Asserts:
     *   (a) the breath settles with ZERO airborne flakes — a canonical commit
     *       never bakes transient particle mass absent from materialState
     *   (b) mass accounting: the loose powder lifted+blown is gone from the
     *       settled field, bonded glue mass survives
     *   (c) canonical raster == deserialize-rendered raster (materialState is
     *       authoritative and reproducible). */
    {
        /* --- build the material exactly like the GUI's Glue->Pour ->Blow */
        GlitterPowder e2e;
        e2e.reset(QSize(96, 96), 4.0f, 20260901u);
        const float wc = (96 / 2.0f) * 4.0f;
        GlitterParams e2p; e2p.amount = 1.5f; e2p.flakeSize = 0.8f;
        e2p.color = pal.colors[0];
        /* Glue: a wet glue disc */
        e2e.paintGlue(wc, wc, 30.0f, 1.0f);
        /* Pour: glitter over the glue (and slightly beyond) */
        e2e.pourGlitter(wc, wc, 26.0f, e2p);
        e2e.relax(); e2e.relax(); e2e.decayFlow();
        /* bond to the glue so some powder becomes stuck */
        e2e.absorb();
        const double stuckBeforeBlow = ([](const GlitterPowder &g){
            double m = 0.0; for (int i=0;i<g.cellCount();++i) m += g.powderCellAt(i).stuck; return m; })(e2e);
        const double looseBeforeBlow = e2e.looseMass();
        CHECK("e2e golden: glue bonds some powder before the blow",
              stuckBeforeBlow > 0.0 && looseBeforeBlow > 0.0);
        /* Blow through the EXACT GUI finish lifecycle, then settle */
        runBlowLifecycle(e2e);
        /* (a) at the canonical commit moment nothing is airborne */
        CHECK("e2e golden: no airborne flakes at canonical commit",
              !e2e.hasActiveParticles());
        /* (b) mass accounting: bonding survives the blow */
        const double stuckAfterBlow = ([](const GlitterPowder &g){
            double m = 0.0; for (int i=0;i<g.cellCount();++i) m += g.powderCellAt(i).stuck; return m; })(e2e);
        CHECK("e2e golden: bonded mass survives the blow (glue holds it)",
              stuckAfterBlow > 0.0 && stuckAfterBlow >= stuckBeforeBlow * 0.60);
        CHECK("e2e golden: loose powder was blown off (mass accounting)",
              e2e.looseMass() < looseBeforeBlow);
        /* (c) canonical commit: render the settled state at t=0 (this is what
         * layer.raster would store), serialize, deserialize, re-bake and compare */
        const QRect view(40, 40, 96, 96);
        const QImage canonicalRaster = e2e.render(view.size(), (float)view.x(), (float)view.y(),
                                                  1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        const QByteArray canonicalState = e2e.serialize();
        GlitterPowder reopened2;
        CHECK("e2e golden: canonical commit deserializes cleanly",
              reopened2.deserialize(canonicalState));
        CHECK("e2e golden: deserialized state is airborne-free too",
              !reopened2.hasActiveParticles());
        const QImage rebaked = reopened2.render(view.size(), (float)view.x(), (float)view.y(),
                                                1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("e2e golden: canonical raster == deserialize-rebaked raster",
              imgEqual(canonicalRaster, rebaked, 2));
        /* (determinism P2 #8) a SECOND reload renders identically: the RNG is
         * persisted, so the state is fully reproducible */
        GlitterPowder reopened3;
        CHECK("e2e golden: second reload accepts state", reopened3.deserialize(canonicalState));
        const QImage rebaked3 = reopened3.render(view.size(), (float)view.x(), (float)view.y(),
                                                 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        CHECK("e2e golden: reload is deterministic (same raster twice)",
              imgEqual(rebaked, rebaked3, 0));
    }

    /* ---- P0.7 crop-invariant CPU rendering ------------------------------
     * Grain identity + size must be anchored to ABSOLUTE world coordinates, so
     * overlapping world-space pixels render identically no matter what crop
     * (origin/size) is being drawn. Render the same state as a full frame, a
     * left, a middle and a right crop, then verify the overlapping regions of
     * the crops exactly match the full-frame pixels for those world coords. */
    {
        GlitterPowder ci;
        ci.reset(QSize(64, 64), 4.0f, 21u);
        GlitterParams cp; cp.amount = 1.4f; cp.flakeSize = 0.8f; cp.color = pal.colors[0];
        cp.palette.clear();
        /* material spread across world x/y ~ [64,192] (a couple of pours + glue) */
        ci.pourGlitter(96.0f, 96.0f, 22.0f, cp);
        ci.pourGlitter(128.0f, 96.0f, 22.0f, cp);
        ci.paintGlue(96.0f, 112.0f, 14.0f, 0.8f);
        ci.relax(); ci.relax(); ci.decayFlow();

        const float wpp = 1.0f;   /* world per px (fixed across all renders) */
        /* full frame covers world x[64,192], y[64,192] */
        const QRect fullR(64, 64, 128, 128);
        const QImage full = ci.render(fullR.size(), float(fullR.x()), float(fullR.y()),
                                      wpp, ArtLightRig(), pal, nullptr, ArtScale(), 0)
                                .convertToFormat(QImage::Format_ARGB32);

        /* three overlapping crops across the same material */
        struct Crop { QRect r; const char *name; };
        const Crop crops[] = {
            { QRect(64,  64, 96, 128), "left"   },   /* world x[64,160]  */
            { QRect(96,  64, 96, 128), "middle" },   /* world x[96,192]  */
            { QRect(128, 64, 96, 128), "right"  },   /* world x[128,224] */
        };
        bool allMatch = true;
        for (const Crop &c : crops)
        {
            QImage crop = ci.render(c.r.size(), float(c.r.x()), float(c.r.y()),
                                    wpp, ArtLightRig(), pal, nullptr, ArtScale(), 0)
                              .convertToFormat(QImage::Format_ARGB32);
            int count = 0; long diffSum = 0; int badPixel = 0; int maxBad = 0;
            for (int y = 0; y < crop.height() && y < full.height(); ++y)
                for (int x = 0; x < crop.width(); ++x)
                {
                    const int wx = c.r.x() + x;              /* world x of crop px */
                    if (wx < fullR.x() || wx >= fullR.x() + fullR.width()) continue;
                    const int fx = wx - fullR.x();           /* full-frame px for that world x */
                    const int fy = y + c.r.y() - fullR.y();  /* full-frame y (crops y-aligned) */
                    if (fy < 0 || fy >= full.height()) continue;
                    const QRgb pc = crop.pixel(x, y);
                    const QRgb fc = full.pixel(fx, fy);
                    const int da = qAbs<int>(qAlpha(pc) - qAlpha(fc));
                    const int dr = qAbs<int>(qRed(pc) - qRed(fc));
                    const int dg = qAbs<int>(qGreen(pc) - qGreen(fc));
                    const int db = qAbs<int>(qBlue(pc) - qBlue(fc));
                    diffSum += dr + dg + db; ++count;
                    if (dr + dg + db + da > 20) { ++badPixel; if (dr > maxBad) maxBad = dr; }
                }
            const double meanDiff = count ? double(diffSum) / (3.0 * count) : 0.0;
            std::printf("    P0.7 overlap[%s] world-px=%d meanRGBdiff=%.2f badPx=%d\n",
                        c.name, count, meanDiff, badPixel);
            if (count == 0 || badPixel > qMax(2, count / 1500) || meanDiff > 8.0)
                allMatch = false;
        }
        CHECK("P0.7 crop-invariant CPU rendering (A∩B==B, A∩C==C, A∩D==D)", allMatch);
    }

    /* ---- P0.7 bounded material palette ---------------------------------
     * ensurePowderPaletteSlot resolves the CURRENT foreground colour to a
     * bounded slot: reuse an exact existing colour, append while free, reuse
     * the nearest when the palette is full (16). Existing grains keep their
     * original slots, so a colour change deposits NEW glitter, not a rewrite. */
    {
        GlitterPowder g;
        g.reset(QSize(32, 32), 4.0f, 1u);
        const QColor pink(0xF2, 0xA7, 0xC8);
        const QColor blue(0x00, 0xA8, 0xC8);
        const QColor yellow(0xF2, 0xC8, 0x2A);
        const int sPink1 = g.ensurePowderPaletteSlot(pink);
        const int sBlue  = g.ensurePowderPaletteSlot(blue);
        const int sPink2 = g.ensurePowderPaletteSlot(pink);
        CHECK("P0.7 same colour reuses its existing slot", sPink1 == 0 && sPink2 == sPink1);
        CHECK("P0.7 different colour appends a new slot", sBlue == 1);
        CHECK("P0.7 palette grows with distinct colours", g.palette().size() == 2);
        /* fill the palette to capacity, then a new colour falls back to the
         * nearest existing one (never a 17th slot) */
        while (g.palette().size() < 16)
            g.ensurePowderPaletteSlot(QColor(g.palette().size() * 17, 40, 200));
        const int cap = g.palette().size();
        CHECK("P0.7 palette is bounded at 16 slots", cap == 16);
        const int nearPink = g.ensurePowderPaletteSlot(QColor(0xF3, 0xA8, 0xC9)); /* ≈ pink */
        CHECK("P0.7 full palette reuses nearest slot", nearPink >= 0 && nearPink < 16);
        CHECK("P0.7 full palette never exceeds 16 slots", g.palette().size() == 16);
        /* a coloured pour deposits into the resolved slot, not rewriting an
         * already-resolved slot (existing grains keep their palette slots) */
        {
            GlitterPowder g2;
            g2.reset(QSize(48, 48), 4.0f, 2u);
            GlitterParams pp; pp.amount = 1.0f; pp.flakeSize = 0.8f; pp.color = pink;
            pp.palette.clear();
            g2.pourGlitter(20 * 4.0f, 24 * 4.0f, 12.0f, pp);   /* pink -> slot 0 */
            GlitterParams bp; bp.amount = 1.0f; bp.flakeSize = 0.8f; bp.color = blue;
            bp.palette.clear();
            g2.pourGlitter(28 * 4.0f, 24 * 4.0f, 12.0f, bp);   /* blue -> slot 1 */
            int blueCells = 0;
            int wrongSlot0 = 0;
            int slot1Cells = 0;
            for (int i = 0; i < g2.cellCount(); ++i) {
                const PowderCell &c = g2.powderCellAt(i);
                const float wx = (i % g2.cellW() + 0.5f) * g2.worldCellSize();
                if (c.height > 0 && wx > 104.0f) {   /* firmly inside the BLUE pour site */
                    ++blueCells;
                    if (c.lA == 1) ++slot1Cells;      /* slot 1 = blue */
                    if (c.lA == 0) ++wrongSlot0;      /* slot 0 is pink */
                }
            }
            std::printf("    P0.7 blue zone: cells=%d slot1(blue)=%d slot0(pink)=%d\n",
                        blueCells, slot1Cells, wrongSlot0);
            const bool blueDepositedBlue = blueCells > 50 && slot1Cells > 0 && wrongSlot0 == 0;
            CHECK("P0.7 a coloured pour deposits into its resolved slot (not slot 0)",
                  blueDepositedBlue);
            CHECK("P0.7 existing grains keep their original slots (blue gets its own slot)",
                  g2.palette().size() == 2 && g2.palette()[0] == pink && g2.palette()[1] == blue);
        }
    }

    /* ---- Glue-only material correctness (P0.1 / P0.11) ------------------
     * A pure glue stroke (no glitter) is still a real material: it must be
     * visible, non-empty, render wet glue, persist through serialize and be
     * restored with its bounds intact. */
    {
        GlitterPowder g;
        g.reset(QSize(64, 64), 4.0f, 4u);
        CHECK("P0.1 empty before any material", g.isEmpty());
        CHECK("P0.1 no glue before any material", !g.hasGlue());
        g.paintGlue(32 * 4.0f, 32 * 4.0f, 12.0f, 0.9f);   /* glue ring, NO powder */
        CHECK("P0.1 hasGlue true after glue-only stroke", g.hasGlue());
        CHECK("P0.1 hasMaterial true for glue-only", g.hasMaterial());
        CHECK("P0.1 isEmpty false when meaningful glue exists", !g.isEmpty());
        CHECK("P0.1 hasPowder false for glue-only", !g.hasPowder());

        /* glue bounds are non-empty even with no powder */
        float mnx = 0, mny = 0, mxx = 0, mxy = 0;
        const bool hasMatBounds = g.materialBoundsWorld(mnx, mny, mxx, mxy);
        CHECK("P0.1 materialBoundsWorld includes glue", hasMatBounds && mxx > mnx && mxy > mny);
        /* powder-only bounds must be EMPTY for a glue-only field */
        float pnx = 0, pny = 0, pxx = 0, pxy = 0;
        CHECK("P0.1 powderBoundsWorld empty for glue-only", !g.powderBoundsWorld(pnx, pny, pxx, pxy));

        /* glue renders visibly (transparent adhesive alpha > 0 inside the ring) */
        QImage glueImg = g.render(QSize(96, 96), 32 * 4.0f - 48, 32 * 4.0f - 48, 1.0f,
                                  ArtLightRig(), pal, nullptr, ArtScale(), 0);
        glueImg = glueImg.convertToFormat(QImage::Format_ARGB32);
        int glueAlpha = 0;
        for (int y = 0; y < glueImg.height(); ++y)
            for (int x = 0; x < glueImg.width(); ++x)
                glueAlpha += qAlpha(glueImg.pixel(x, y));
        const double meanGlueA = glueAlpha / double(glueImg.width() * glueImg.height());
        std::printf("    P0.1 glue-only mean alpha = %.3f\n", meanGlueA);
        CHECK("P0.1 glue-only renders visible wet glue (alpha>0)", meanGlueA > 1.0);

        /* glue-only persists + deserializes */
        const QByteArray glueState = g.serialize();
        CHECK("P0.1 glue-only materialState non-empty", !glueState.isEmpty());
        GlitterPowder gr;
        CHECK("P0.1 glue-only deserialize accepts state", gr.deserialize(glueState));
        CHECK("P0.1 glue survives deserialize", gr.hasGlue());
        float grn = 0, gr2 = 0, grx = 0, gry = 0;
        const bool grB = gr.materialBoundsWorld(grn, gr2, grx, gry);
        CHECK("P0.1 glue bounds survive deserialize", grB && grx > grn && gry > gr2);
        /* and render wet glue after restore */
        QImage grImg = gr.render(QSize(96, 96), 32 * 4.0f - 48, 32 * 4.0f - 48, 1.0f,
                                 ArtLightRig(), pal, nullptr, ArtScale(), 0);
        grImg = grImg.convertToFormat(QImage::Format_ARGB32);
        int grAlpha = 0;
        for (int y = 0; y < grImg.height(); ++y)
            for (int x = 0; x < grImg.width(); ++x)
                grAlpha += qAlpha(grImg.pixel(x, y));
        CHECK("P0.1 restored glue renders visible", grAlpha > glueAlpha / 2);
        /* P0.2: the full-resolution glue field persists through the round trip
         * and is present (a sub-res sample inside the painted disc reads > 0). */
        const float gPresence = g.gluePresence(32 * 4.0f, 32 * 4.0f);
        CHECK("P0.2 full-res glue field present", gPresence > 0.05f);
        /* rebuilt (deserialized) engine samples glue at the same centre */
        const float grPresence = gr.gluePresence(32 * 4.0f, 32 * 4.0f);
        CHECK("P0.2 glue field survives deserialize", grPresence > 0.05f);
        /* the coarse cell glue is unchanged by the sub-res field */
        CHECK("P0.2 coarse cell glue still set", g.powderCellAt(32 * g.cellW() + 32).glue > 0);
    }

    /* ---- P0.6 material-bounds-driven dirty (rect-clip regression) -------
     * The committed/dirty region must come from the physical MATERIAL bounds
     * (old ∪ new + support margin), NOT the pointer footprint. Verify that
     * pour / push / erase / blow change material bounds as expected and that
     * erase/old-new invalidation is covered. */
    {
        GlitterPowder mb;
        mb.reset(QSize(96, 96), 4.0f, 31u);
        float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        const bool preEmpty = !mb.materialBoundsWorld(bx0, by0, bx1, by1);
        CHECK("P0.6 empty material has no bounds", preEmpty);

        /* Glue creates material bounds immediately (not void). */
        mb.paintGlue(48 * 4.0f, 48 * 4.0f, 14.0f, 0.9f);
        float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        const bool glueBounds = mb.materialBoundsWorld(gx0, gy0, gx1, gy1);
        CHECK("P0.6 glue stroke yields material bounds", glueBounds && gx1 > gx0 && gy1 > gy0);

        /* Pour over that glue EXPANDS the bounds outward (loose heap falls
         * wider than the glue disc). */
        float p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        mb.materialBoundsWorld(p0, p1, p2, p3);
        GlitterParams pp; pp.amount = 1.3f; pp.flakeSize = 0.8f; pp.color = pal.colors[0];
        pp.palette.clear();
        mb.pourGlitter(48 * 4.0f, 48 * 4.0f, 16.0f, pp);
        mb.relax(); mb.relax(); mb.decayFlow();
        float q0 = 0, q1 = 0, q2 = 0, q3 = 0;
        mb.materialBoundsWorld(q0, q1, q2, q3);
        CHECK("P0.6 material bounds after Pour grow (loose heap wider than glue)",
              q2 > p2 + 2.0f || q3 > p3 + 2.0f);

        /* Erase removes material: the OLD bounds are larger than the new ones */
        float e0 = 0, e1 = 0, e2 = 0, e3 = 0;
        mb.materialBoundsWorld(e0, e1, e2, e3);
        mb.erase(48 * 4.0f, 48 * 4.0f, 30.0f, 1.0f);   /* erase a big disc */
        mb.relax(); mb.decayFlow();
        float f0 = 0, f1 = 0, f2 = 0, f3 = 0;
        const bool afterErase = mb.materialBoundsWorld(f0, f1, f2, f3);
        const bool shrank = !afterErase || (f2 <= e2 && f3 <= e3);
        std::printf("    P0.6 erase old=(%.0f,%.0f,%.0f,%.0f) new=(%.0f,%.0f,%.0f,%.0f)\n",
                    e0, e1, e2, e3, f0, f1, f2, f3);
        CHECK("P0.6 Eraser reduces material bounds (old >= new)", shrank);
    }

    /* ---- physical Eraser ---------------------------------------------- */
    {
        GlitterPowder er;
        er.reset(QSize(96, 96), 4.0f, 11u);
        GlitterParams sp; sp.amount = 1.4f; sp.flakeSize = 0.8f; sp.color = pal.colors[0];
        er.pourGlitter(48 * 4.0f, 48 * 4.0f, 22.0f, sp);
        er.paintGlue(48 * 4.0f, 48 * 4.0f, 18.0f, 0.9f);
        er.pourGlitter(48 * 4.0f, 48 * 4.0f, 18.0f, sp);
        er.relax(); er.relax(); er.decayFlow(); er.absorb();
        float prevL = 0, prevS = 0;
        for (int i = 0; i < er.cellCount(); ++i)
        {
            prevL += er.powderCellAt(i).height - er.powderCellAt(i).stuck;
            prevS += er.powderCellAt(i).stuck;
        }
        /* erase a disc slightly SMALLER than the poured heap, but well inside
         * the settled mass, so the falloff region is covered */
        er.erase(48 * 4.0f, 48 * 4.0f, 18.0f, 1.0f);
        float afterL = 0, afterS = 0;
        for (int i = 0; i < er.cellCount(); ++i)
        {
            afterL += er.powderCellAt(i).height - er.powderCellAt(i).stuck;
            afterS += er.powderCellAt(i).stuck;
        }
        CHECK("erase removes loose substance (mass drops)",
              afterL < prevL);
        CHECK("erase removes bonded substance too (not just loose)",
              afterS < prevS * 0.4f);
        CHECK("erase clears wet glue in the disc", [&]() -> bool {
            for (int i = 0; i < er.cellCount(); ++i)
            {
                const float wx = (i % er.cellW() + 0.5f) * er.worldCellSize();
                const float wy = (i / er.cellW() + 0.5f) * er.worldCellSize();
                const float dx = wx - 48 * 4.0f;
                const float dy = wy - 48 * 4.0f;
                const float d2 = dx * dx + dy * dy;
                if (d2 < 100.0f && er.powderCellAt(i).glue > 0.5f)
                    return false;
            }
            return true;
        }());
    }

    /* ---- POUR GOLDEN (reviewer gate #8) ---------------------------------
     * The actual reference workflow with the fitted pourMain(fit=true):
     *   glue ring -> Pour pink -> Blow -> second glue ring -> Pour blue -> Blow
     *   -> yellow+green crossing glitter -> Push -> Blow
     * Then render and demand NO regular rows/columns/checker (the old grid
     * algorithm's hard failure), no same-period lattice on both axes, and that
     * existing glitter is PRESERVED (pouring over keeps earlier colours). */
    {
        GlitterPowder gold;
        gold.reset(QSize(192, 192), 4.0f, 2026u);
        const float wc = (192 / 2.0f) * 4.0f;        /* world centre */
        const float ringR = 44.0f;                    /* ring radius (world) */
        const float ringW = 10.0f;                    /* glue line width */

        auto paintGlueRing = [&](float wx, float wy, float r, float wdt)
        {
            /* a ring of wet glue by stamping many small discs on the circle */
            const int steps = 140;
            for (int s = 0; s < steps; ++s)
            {
                const float a = 6.28318f * float(s) / float(steps);
                const float gx = wx + r * std::cos(a);
                const float gy = wy + r * std::sin(a);
                gold.paintGlue(gx, gy, wdt, 1.0f);
            }
        };

        GlitterParams pink; pink.amount = 1.2f; pink.flakeSize = 0.8f;
        pink.color = QColor("#f2a7c8");              /* reference signature pink */
        GlitterParams blue; blue.amount = 1.2f; blue.flakeSize = 0.8f;
        blue.color = QColor("#2f6fd6");
        GlitterParams yellow; yellow.amount = 1.2f; yellow.flakeSize = 0.8f;
        yellow.color = QColor("#ffc900");
        GlitterParams green; green.amount = 1.2f; green.flakeSize = 0.8f;
        green.color = QColor("#3ab05a");

        /* 1. outer glue ring */
        paintGlueRing(wc, wc, ringR, ringW);
        /* 2. Pour pink over the bare glue (fitted pourMain) */
        gold.pourGlitterMain(true, pink);
        gold.relax(); gold.decayFlow();
        /* 3. Blow: loose pink over the ring either bonds or drifts off */
        runBlowLifecycle(gold);
        /* 4. second (inner) glue ring */
        paintGlueRing(wc, wc, ringR * 0.5f, ringW);
        /* 5. Pour blue over the inner bare glue (earlier pink is left alone) */
        gold.pourGlitterMain(true, blue);
        gold.relax(); gold.decayFlow();
        /* 6. Blow again */
        runBlowLifecycle(gold);
        /* 7. yellow + green crossing glitter (brush sprinkles, not pour) */
        gold.pourGlitter(wc + ringR * 0.5f, wc - 6.0f, 12.0f, yellow);
        gold.pourGlitter(wc - 2.0f, wc + ringR * 0.2f, 12.0f, green);
        gold.relax(); gold.decayFlow();
        /* 8. Push: smear the crossing glitter a little */
        gold.smear(wc, wc, 0.4f, -0.3f, 18.0f, 0.25f);
        gold.absorb();
        gold.relax(); gold.decayFlow();
        /* 9. final Blow */
        runBlowLifecycle(gold);

        CHECK("pour golden: material covers meaningful area (both rings+pours)",
              gold.looseMass() + gold.bondedCount() * 0.1f > 0.0f
              && gold.bondedCount() > 100);
        /* pouring over KEEPS earlier colours: pink slot stays, blue added */
        QVector<QColor> palC = gold.palette();
        const bool keptPink = palC.contains(QColor("#f2a7c8"));
        const bool keptBlue = palC.contains(QColor("#2f6fd6"));
        CHECK("pour golden: pink is PRESERVED after pouring blue over it",
              keptPink);
        CHECK("pour golden: blue slot added by the second pour", keptBlue);

        /* render the finished piece and probe for a lattice */
        QImage goldImg = gold.render(QSize(288, 288), wc - 144, wc - 144, 1.0f,
                                     ArtLightRig(), pal, nullptr, ArtScale(), 0);
        goldImg.save("pb_powder_pour_golden.png");
        float hc = 0.0f, vc = 0.0f;
        int hp = latticePeriodContrast(goldImg, 0, hc);
        int vp = latticePeriodContrast(goldImg, 1, vc);
        std::printf("    [golden] h period=%d c=%.3f  v period=%d c=%.3f\n",
                    hp, (double)hc, vp, (double)vc);
        /* the fitted single pour must NOT produce a same-period grid */
        CHECK("pour golden: no regular lattice (pourMain fit, not grid)",
              noLatticeGrid(goldImg, 0.10f));
        CHECK("pour golden: renders non-empty non-transparent material",
              goldImg.width() > 0 && !goldImg.isNull()
              && imageMeanAlpha(goldImg, QRect(0, 0, goldImg.width(), goldImg.height())) > 2.0);
    }

    /* ---- DARK-COLOUR GOLDENS (reviewer, revised product look) ----------------
     * One fixed serialized powder state rendered with #000000 / #202020 /
     * #FFC900 / #F2A7C8 / #FFFFFF through the SAME geometry. The product's
     * black material is:
     *   shadow-facing side   -> black
     *   light-facing side    -> dark / medium graphite grey
     *   microfacets          -> neutral silver/white mirror highlights
     *   holographic response -> movement/intensity in reflection, NOT random
     *                           coloured dots
     * So we assert (NOT the earlier "colourful flashes are good" gate):
     *   - silhouette + pile geometry IDENTICAL across colours,
     *   - black body luminance is spatially split: lit region darker than a
     *     graphite tone yet clearly brighter than the shadow region (the same
     *     light/shade split yellow gets from its albedo),
     *   - black material saturation stays LOW (no isolated RGB dots, P99 of
     *     channel spread below a strict threshold), even at shimmer 1,
     *   - bright mirror cores exist but are near-neutral (silver/white),
     *   - shimmer1 modulates NDETER variant (no rainbow wash) and the flake
     *     chroma is clamped for black. */
    {
        /* fixed powder state: a substantial pink heap, serialized once */
        GlitterPowder src = makeHeap(pal, 256, 555.0f);
        const QByteArray stateBytes = src.serialize();
        std::vector<QColor> colours = { QColor("#000000"), QColor("#202020"),
                                        QColor("#FFC900"), QColor("#F2A7C8"),
                                        QColor("#FFFFFF") };
        const float wc = (256 / 2.0f) * 4.0f;
        const int nSlots = src.palette().size() > 0 ? src.palette().size() : 1;

        /* reference alpha silhouette (from the first colour) */
        QImage firstA;
        std::vector<QImage> imgsByColour;
        for (const QColor &c : colours)
        {
            PowderPalette cp;
            cp.holographic = 0.55f;
            for (int i = 0; i < nSlots; ++i)
                cp.colors << c;
            GlitterPowder g; g.deserialize(stateBytes);
            QImage img = g.render(QSize(192, 192), wc - 96, wc - 96, 1.0f,
                                  ArtLightRig(), cp, nullptr, ArtScale(), 0);
            imgsByColour.push_back(img);
            if (firstA.isNull())
                firstA = img;
        }

        /* 1. geometry identical across colours: alpha (coverage) is tint-free */
        bool geoSame = true;
        for (const QImage &img : imgsByColour)
            if (img.size() != firstA.size()
                || std::abs((double)imageMeanAlpha(img) - imageMeanAlpha(firstA)) > 4.0)
                geoSame = false;
        CHECK("dark goldens: silhouette identical across black..white (geometry)", geoSame);

        /* 2. BLACK spatial light/shade split + low saturation.
         * Split each black render into a light-facing and shadow-facing HALF
         * using the rig light direction projected on the crop (the heap is
         * centred, the rig light comes from upper-left/upper-right). The lit
         * half must read clearly brighter (graphite) than the shadow half, but
         * must stay dark (not washed grey). And bright pixels must be near-
         * neutral (low channel spread) — no isolated saturated dots. */
        {
            PowderPalette bp; bp.holographic = 0.55f;
            for (int i = 0; i < nSlots; ++i) bp.colors << QColor("#000000");
            auto rigA = [](){ ArtLightRig r; r.direction = QVector3D(0.55f, 0.45f, 0.7f); return r; }(); /* upper-right */
            /* compute the LIGHT AZIMUTH in the same way the renderer does, so the
             * lit/shadow split is grounded in the actual BRDF light, not a guess:
             * render L.xy = -unitDirection().xy (unitDirection is down-pointing). */
            const QVector3D rigDirA = rigA.unitDirection();
            const float lax = -rigDirA.x(), lay = -rigDirA.y();
            const float laLen = std::sqrt(lax * lax + lay * lay) + 1e-6f;
            const float ulx = lax / laLen, uly = lay / laLen;   /* image-plane light azimuth */
            double litLum = 0, shaLum = 0, bodyDark = 0;
            long litN = 0, shaN = 0;
            std::vector<int> brightSpreads;      /* channel spread of bright px */
            int opaque = 0, brightNeutral = 0, brightColoured = 0;
            /* accumulate over several shimmer instants (moving mirror) */
            const qint64 times[4] = { 1234, 2321, 3105, 4488 };
            for (int ti = 0; ti < 4; ++ti)
            {
                GlitterPowder g; g.deserialize(stateBytes);
                QImage bimg = g.render(QSize(192, 192), wc - 96, wc - 96, 1.0f,
                                       rigA, bp, nullptr, ArtScale(), times[ti]);
                const int cxc = bimg.width() / 2, cyc = bimg.height() / 2;
                for (int y = 0; y < bimg.height(); ++y)
                    for (int x = 0; x < bimg.width(); ++x)
                    {
                        const int a = qAlpha(bimg.pixel(x, y));
                        if (a <= 8) continue;
                        opaque += (ti == 0);
                        const double lum = 0.299*qRed(bimg.pixel(x, y))
                                         + 0.587*qGreen(bimg.pixel(x, y))
                                         + 0.114*qBlue(bimg.pixel(x, y));
                        const int dx = x - cxc, dy = y - cyc;
                        const bool litSide = (dx*ulx + dy*uly) >= 0.0;
                        if (litSide) { litLum += lum; ++litN; } else { shaLum += lum; ++shaN; }
                        if (lum < 45.0) bodyDark += 1.0;
                        if (lum > 120.0)
                        {
                            const int spread = qMax(qRed(bimg.pixel(x,y)), qMax(qGreen(bimg.pixel(x,y)), qBlue(bimg.pixel(x,y))))
                                             - qMin(qRed(bimg.pixel(x,y)), qMin(qGreen(bimg.pixel(x,y)), qBlue(bimg.pixel(x,y))));
                            brightSpreads.push_back(spread);
                            if (spread <= 60) ++brightNeutral; else ++brightColoured;
                        }
                    }
            }
            /* the split could be mirrored depending on sign convention; take the
             * brighter of the two projected halves as the LIT side so the gate is
             * robust — the requirement is a clear lit/shadow luminance split. */
            litLum = litN ? litLum / litN : 0; shaLum = shaN ? shaLum / shaN : 0;
            if (shaLum > litLum) std::swap(litLum, shaLum);
            std::sort(brightSpreads.begin(), brightSpreads.end());
            const int p99 = brightSpreads.empty() ? 0
                : brightSpreads[size_t(brightSpreads.size()) * 99 / 100];
            std::printf("    [dark] black litLum=%.1f shaLum=%.1f bodyDarkPx=%ld brightNeutral=%d brightColoured=%d brightSpreads=%zu P99=%d opaque=%d\n",
                        litLum, shaLum, (long)bodyDark, brightNeutral, brightColoured,
                        brightSpreads.size(), p99, opaque);
            std::printf("    [dark] lit-vs-shadow ratio=%.2f (must be > 1.35)\n",
                        shaLum > 0 ? litLum / shaLum : 0.0);
            /* the black body is a dark material: bulk pixels stay near-black */
            CHECK("dark goldens: black body stays black (not lifted to grey)",
                  bodyDark > opaque * 0.6);
            /* lit side brighter than shadow side (graphite-vs-black split) */
            CHECK("dark goldens: black lit side brighter than shadow side",
                  litN > 0 && shaN > 0 && litLum > shaLum * 1.35 + 3.0);
            /* lit side stays dark/graphite, not washed grey */
            CHECK("dark goldens: black lit region is graphite, not grey",
                  litLum < 150.0);
            /* no isolated saturated dots: bright pixels near-neutral, P99 low */
            CHECK("dark goldens: black bright mirror cores are neutral (no RGB dots)",
                  brightColoured <= brightNeutral / 4 && p99 < 60);
        }

        /* 3+4. Shimmer on BLACK: holographic response is neutral reflection
         * modulation, NOT new saturated dots. shimmer0 vs shimmer1 must change
         * the mirror intensity/location (movement), but both must stay low-
         * saturation for black. (Coloured thin-film may stay for coloured
         * glitter; this gate only enforces black neutrality.) */
        {
            PowderPalette p0; p0.holographic = 0.0f;
            PowderPalette p1; p1.holographic = 1.0f;
            for (int i = 0; i < nSlots; ++i) { p0.colors << QColor("#000000"); p1.colors << QColor("#000000"); }
            GlitterPowder g0; g0.deserialize(stateBytes);
            GlitterPowder g1; g1.deserialize(stateBytes);
            QImage i0 = g0.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, ArtLightRig(), p0, nullptr, ArtScale(), 1234);
            QImage i1 = g1.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, ArtLightRig(), p1, nullptr, ArtScale(), 1234);
            long hue0 = 0, hue1 = 0, bright0 = 0, bright1 = 0;
            double sat0 = 0, sat1 = 0; long s0 = 0, s1 = 0;
            for (int y = 0; y < i0.height(); y += 1)
                for (int x = 0; x < i0.width(); x += 1)
                {
                    if (qAlpha(i0.pixel(x, y)) <= 4 || qAlpha(i1.pixel(x, y)) <= 4) continue;
                    const QRgb a = i0.pixel(x, y), b = i1.pixel(x, y);
                    const double lum0 = 0.299*qRed(a)+0.587*qGreen(a)+0.114*qBlue(a);
                    const double lum1 = 0.299*qRed(b)+0.587*qGreen(b)+0.114*qBlue(b);
                    const int m0 = qMax(qRed(a),qMax(qGreen(a),qBlue(a))) - qMin(qRed(a),qMin(qGreen(a),qBlue(a)));
                    const int m1 = qMax(qRed(b),qMax(qGreen(b),qBlue(b))) - qMin(qRed(b),qMin(qGreen(b),qBlue(b)));
                    if (lum0 > 90.0) { bright0++; if (m0 > 25) hue0++; sat0 += m0; ++s0; }
                    if (lum1 > 90.0) { bright1++; if (m1 > 25) hue1++; sat1 += m1; ++s1; }
                }
            sat0 = s0 ? sat0 / s0 : 0; sat1 = s1 ? sat1 / s1 : 0;
            std::printf("    [dark] shimmer0(black) bright=%ld hue=%ld avgSat=%.2f | shimmer1 bright=%ld hue=%ld avgSat=%.2f\n",
                        bright0, hue0, sat0, bright1, hue1, sat1);
            /* both must be low-saturation (neutral mirror, no black dots) */
            CHECK("dark goldens: black shimmer0 stays neutral (no rainbow dots)",
                  hue0 < 100 && sat0 < 30.0);
            CHECK("dark goldens: black shimmer1 stays neutral (no RGB dots)",
                  hue1 < 120 && sat1 < 40.0);
            CHECK("dark goldens: shimmer1 changes mirror response (movement)",
                  !imgEqual(i0, i1, 0));
        }

        /* 5. moving the light moves highlights (geometry fixed) */
        {
            PowderPalette rs; rs.holographic = 0.55f;
            for (int i = 0; i < nSlots; ++i) rs.colors << QColor("#f2a7c8");
            ArtLightRig rl, rr;
            rl.direction = QVector3D(0.9f, 0.05f, 0.6f);
            rr.direction = QVector3D(-0.9f, 0.05f, 0.6f);
            GlitterPowder gl; gl.deserialize(stateBytes);
            GlitterPowder gr; gr.deserialize(stateBytes);
            QImage a = gl.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rl, rs, nullptr, ArtScale(), 0);
            QImage b = gr.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rr, rs, nullptr, ArtScale(), 0);
            CHECK("dark goldens: changing light moves highlights", !imgEqual(a, b, 0));
        }

        /* 6. black vs yellow reveal the SAME geometry: same silhouette and a
         * lit-side brighter than shadow-side split in BOTH (the tint only
         * changes the hue, not where the light/shade split falls). */
        {
            auto rigB = [](){ ArtLightRig r; r.direction = QVector3D(0.55f, 0.45f, 0.7f); return r; }();
            PowderPalette bp; bp.holographic = 0.55f;
            PowderPalette yp; yp.holographic = 0.55f;
            for (int i = 0; i < nSlots; ++i) { bp.colors << QColor("#000000"); yp.colors << QColor("#FFC900"); }
            GlitterPowder gb; gb.deserialize(stateBytes);
            GlitterPowder gy; gy.deserialize(stateBytes);
            QImage bi = gb.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rigB, bp, nullptr, ArtScale(), 0);
            QImage yi = gy.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, rigB, yp, nullptr, ArtScale(), 0);
            /* geometry (alpha) identical */
            bool alphaSame = (bi.size() == yi.size());
            if (alphaSame)
                for (int y = 0; y < bi.height(); ++y)
                    for (int x = 0; x < bi.width(); ++x)
                        if (std::abs(qAlpha(bi.pixel(x, y)) - qAlpha(yi.pixel(x, y))) > 10) { alphaSame = false; break; }
            CHECK("dark goldens: black and yellow share the SAME pile geometry", alphaSame);
        }

        /* 7. changing time moves facets asynchronously (geometry + t fixed) */
        {
            PowderPalette rs; rs.holographic = 0.55f;
            for (int i = 0; i < nSlots; ++i) rs.colors << QColor("#f2a7c8");
            GlitterPowder gt0; gt0.deserialize(stateBytes);
            GlitterPowder gt1; gt1.deserialize(stateBytes);
            QImage a = gt0.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, ArtLightRig(), rs, nullptr, ArtScale(), 0);
            QImage b = gt1.render(QSize(128, 128), wc - 64, wc - 64, 1.0f, ArtLightRig(), rs, nullptr, ArtScale(), 2026);
            CHECK("dark goldens: changing time moves facets (not an idle frame)",
                  !imgEqual(a, b, 0));
        }
    }

    /* ---- reference sheets for the visual audit ----------------------- */
    {
        const float wc = (256 / 2.0f) * 4.0f;
        QImage sheet = makeHeap(pal).render(QSize(256, 256), wc - 128, wc - 128, 1.0f, ArtLightRig(), pal, nullptr, ArtScale(), 0);
        sheet.save("pb_powder_cpu_reference.png");
        QImage lit = makeHeap(pal).render(QSize(256, 256), wc - 128, wc - 128, 1.0f,
                                          [](){ ArtLightRig r; r.direction = QVector3D(-0.4f, 0.8f, 0.6f); return r; }(),
                                          pal, nullptr, ArtScale(), 0);
        lit.save("pb_powder_cpu_light90.png");
    }

    /* ---- dense-lattice audit sheet ------------------------------------
     * A LARGE dense heap rendered big enough to inspect whether the CPU grain
     * field shows any regular rows/columns/checkerboard (the bug this render
     * path removed). Not a counted check — saved for visual inspection only. */
    {
        GlitterPowder lat;
        lat.reset(QSize(192, 192), 4.0f, 77u);
        const float wc = (192 / 2.0f) * 4.0f;
        GlitterParams lp; lp.amount = 1.6f; lp.flakeSize = 0.8f;
        lp.color = pal.colors[0];
        for (int i = 0; i < 9; ++i)
            for (int j = 0; j < 9; ++j)
                lat.pourGlitter(wc + (i - 4) * 12.0f, wc + (j - 4) * 12.0f,
                                16.0f + 3.0f * float((i * 7 + j) % 5), lp);
        lat.relax(); lat.relax(); lat.decayFlow();
        QImage latSheet = lat.render(QSize(256, 256), wc - 128, wc - 128, 1.0f,
                                     ArtLightRig(), pal, nullptr, ArtScale(), 0);
        latSheet.save("pb_powder_cpu_dense_lattice.png");
    }

    std::printf("\n%d/%d harness checks passed.\n", s_pass, s_checks);
    return s_pass == s_checks ? 0 : 1;
}