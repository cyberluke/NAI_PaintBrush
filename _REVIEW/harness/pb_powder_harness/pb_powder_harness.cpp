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
    const int looseBefore = g.looseCount();
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
        bg.beginBlow();
        int total = 0;
        for (int i = 0; i < 360; ++i)          /* a continuous ~6 s breath at 60 Hz */
            total += bg.blowStep(1.0f / 60.0f);
        std::printf("    [dbg] blow total moves=%d loose %.2f -> %.2f\n",
                    total, bgLoose, bg.looseMass());
        CHECK("blow moves loose powder", total > 0);
        CHECK("blow clears loose", bg.looseMass() < bgLoose * 0.35f);
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
        CHECK("light rig direction moves shading", !imgEqual(a, b, 2));
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
        for (int y = 17; y <= 20; ++y)
            for (int x = 17; x <= 20; ++x)
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