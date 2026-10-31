// pb_gpu_powder_harness.cpp — NAI Paintbrush GPU glitter-POWDER acceptance gate.
//
// P0: PROVE the GPU powder shader (QRhi D3D11 offscreen) runs standalone and
// produces real images. This is NOT about shader compilation or the plugin
// building: it drives the actual `art::GpuRuntime::renderPowder(...)` backend,
// writes 8 PNGs into the harness build dir and asserts real, time-varied,
// light-varied, non-lattice powder rendering.
//
// Pattern (single translation unit, same as the CPU harness at
// _REVIEW/harness/pb_powder_harness/): we `#include` the engine .cpp files so
// all symbols resolve without a VLC/moc build. `#define private public` is
// used (CPU harness pattern) to reach cell internals for scenario setup
// (two-slot colour injection + seeded grain shuffle).
//
//   pwsh _REVIEW/harness/pb_gpu_powder_harness/build_run.ps1
//
// The harness renders 512x512 at worldPerPx 0.5 centred on the world heap at
// (512,512) (grid 256 x 4.0 world units): origin = 512 - 256/2 = 384.

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

// ---- engine internals access (test-only), BEFORE engine headers -----------

#define private public
#define protected public

// Compile the engine in ONE translation unit (CPU-harness pattern).
#include "modules/gui/qt/paintbrush/art/materials/glitter/glitter_powder.cpp"
#include "modules/gui/qt/paintbrush/art/finish/effects.cpp"   // FinishRuntime::paddingFor (renderFinish needs it)

// ---- restore normal access before pulling the GPU runtime (Qt/rhi headers) --
#undef private
#undef protected

#include "modules/gui/qt/paintbrush/art/materials/glitter/glitter_material.cpp"
#include "modules/gui/qt/paintbrush/art/gpu/pb_gpu_runtime.cpp"

#include <QGuiApplication>
#include <QImage>
#include <QColor>
#include <QRgb>
#include <QVector3D>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QVector>

using namespace paintbrush::art;
using namespace paintbrush::art::glitter;

/* ------------------------------------------------------------------ checks */

static int s_checks = 0;
static int s_pass = 0;
#define CHECK(name, cond) \
    do { ++s_checks; \
         const bool ok = (cond); \
         if (ok) ++s_pass; \
         std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); } while (0)

static double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 1e9;
    const QImage ca = a.convertToFormat(QImage::Format_ARGB32);
    const QImage cb = b.convertToFormat(QImage::Format_ARGB32);
    double sum = 0.0; qint64 n = 0;
    for (int y = 0; y < ca.height(); ++y)
    {
        const QRgb *pa = reinterpret_cast<const QRgb *>(ca.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(cb.constScanLine(y));
        for (int x = 0; x < ca.width(); ++x)
        {
            sum += std::abs(qRed(pa[x])   - qRed(pb[x]));
            sum += std::abs(qGreen(pa[x]) - qGreen(pb[x]));
            sum += std::abs(qBlue(pa[x])  - qBlue(pb[x]));
            n += 3;
        }
    }
    return n > 0 ? sum / double(n) : 0.0;
}

/* Count the number of *distinct* RGB colours and the non-background pixel
 * count. The powder shader outputs fully opaque pixels with a near-white
 * `uPaper` background (pal.paper = white); "non-background" = pixels that
 * differ from pure paper by more than a small tolerance (the heap, its dusty
 * rim and shadowed cast). */
struct ImgStats
{
    int distinct = 0;
    qint64 nonBg = 0;       /* pixels differing from paper colour by > tol */
    qint64 total = 0;
    bool hasContent = false;
};
static ImgStats analyzeImage(const QImage &src, const QColor &paper,
                             int tol = 14)
{
    ImgStats st;
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    st.total = qint64(img.width()) * img.height();
    std::vector<bool> seen(1 << 24, false);
    int distinct = 0;
    qint64 nonBg = 0;
    for (int y = 0; y < img.height(); ++y)
    {
        const QRgb *px = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            const QRgb p = px[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            const unsigned idx = (unsigned(r) << 16) | (unsigned(g) << 8) | unsigned(b);
            if (!seen[idx]) { seen[idx] = true; ++distinct; }
            const int dr = r - paper.red(), dg = g - paper.green(), db = b - paper.blue();
            if (dr * dr + dg * dg + db * db > tol * tol)
                ++nonBg;
        }
    }
    st.distinct = distinct;
    st.nonBg = nonBg;
    st.hasContent = distinct >= 20 && nonBg > 0;
    return st;
}

/* ----------------------------------------------------------------- scenes */

struct Scene
{
    GlitterPowder g;
    PowderPalette pal = defaultPowderPalette();
};

/* Mutable per-cell access for scenario setup (private made public above). */
static PowderCell *cellPtr(GlitterPowder &g, int cx, int cy)
{
    return const_cast<PowderCell *>(&g.powderCellAt(cy * g.cellW() + cx));
}

static void seedCells(GlitterPowder &g, float wx, float wy, float radiusWorld,
                      float seedBase)
{
    const float cell = g.worldCellSize();
    const int W = g.cellW(), H = g.cellH();
    const int cx0 = std::max(0, int(std::floor((wx - radiusWorld) / cell)));
    const int cy0 = std::max(0, int(std::floor((wy - radiusWorld) / cell)));
    const int cx1 = std::min(W - 1, int(std::floor((wx + radiusWorld) / cell)));
    const int cy1 = std::min(H - 1, int(std::floor((wy + radiusWorld) / cell)));
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            PowderCell *c = cellPtr(g, cx, cy);
            if (c->height > 0.0f)
            {
                const float hx = seedBase + (cx * 0.13f) + (cy * 0.37f);
                c->seed = std::fmod(hx * 1024.0f, 1024.0f);
            }
        }
}

/* Force the two-slot loose colour across the heap region: slot 0 (pink) +
 * slot 12 (cyan), lF 0.5. The renderPowder palette must carry 13 entries with
 * a distinct colour at index 12. */
static void forceTwoSlot(GlitterPowder &g, float wx, float wy, float radiusWorld,
                         int slotA = 0, int slotB = 12, float lF = 0.5f)
{
    const float cell = g.worldCellSize();
    const int W = g.cellW(), H = g.cellH();
    const int cx0 = std::max(0, int(std::floor((wx - radiusWorld) / cell)));
    const int cy0 = std::max(0, int(std::floor((wy - radiusWorld) / cell)));
    const int cx1 = std::min(W - 1, int(std::floor((wx + radiusWorld) / cell)));
    const int cy1 = std::min(H - 1, int(std::floor((wy + radiusWorld) / cell)));
    for (int cy = cy0; cy <= cy1; ++cy)
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            PowderCell *c = cellPtr(g, cx, cy);
            if (c->height > 0.05f)
            {
                c->lA = slotA; c->lB = slotB; c->lF = lF;
            }
        }
}

/* ------------------------------------------------------------- scenarios */

/* thin: one light dab. dense: several overlapping heavy dabs. */
static Scene makeThin(const PowderPalette &pal, std::uint32_t seed = 7u)
{
    Scene s; s.g.reset(QSize(256, 256), 4.0f, seed);
    GlitterParams p; p.amount = 0.12f; p.flakeSize = 0.8f;
    p.color = pal.colors.isEmpty() ? QColor("#f2a7c8") : pal.colors[0];
    s.g.pourGlitter(512, 512, 22.0f, p);
    s.g.relax(); s.g.relax();
    seedCells(s.g, 512, 512, 40.0f, 11.0f);
    return s;
}

static Scene makeDense(const PowderPalette &pal, std::uint32_t seed = 5u)
{
    Scene s; s.g.reset(QSize(256, 256), 4.0f, seed);
    GlitterParams p; p.amount = 1.5f; p.flakeSize = 0.8f;
    p.color = pal.colors[0];
    const float wc = (256 / 2.0f) * 4.0f;
    for (int i = 0; i < 6; ++i)
        s.g.pourGlitter(wc + (i - 2) * 8.0f, wc + (i % 3 - 1) * 6.0f,
                        22.0f + 2.0f * i, p);
    s.g.relax(); s.g.relax(); s.g.decayFlow();
    seedCells(s.g, wc, wc, 60.0f, 99.0f);
    return s;
}

/* mixed: two colours (slot0 pink + slot12 cyan). */
static Scene makeMixed(PowderPalette &pal)
{
    Scene s; s.g.reset(QSize(256, 256), 4.0f, 13u);
    const float wc = (256 / 2.0f) * 4.0f;
    GlitterParams pinkP; pinkP.amount = 1.3f; pinkP.flakeSize = 0.8f;
    pinkP.color = pal.colors.value(0, QColor("#f2a7c8"));
    GlitterParams cyanP; cyanP.amount = 1.3f; cyanP.flakeSize = 0.8f;
    cyanP.color = QColor("#00a8c8");
    for (int i = 0; i < 4; ++i)
    {
        s.g.pourGlitter(wc + (i - 1) * 6.0f, wc + ((i % 2) ? 6.0f : -6.0f), 20.0f + i, pinkP);
        s.g.pourGlitter(wc + (i - 1) * 6.0f, wc - ((i % 2) ? 6.0f : -6.0f), 18.0f + i, cyanP);
    }
    s.g.relax(); s.g.relax(); s.g.decayFlow();
    /* inject the two-slot loose colour over the heap (slot0 + slot12). */
    forceTwoSlot(s.g, wc, wc, 42.0f, 0, 12, 0.5f);
    seedCells(s.g, wc, wc, 50.0f, 777.0f);
    /* give the palette 13 entries so slot 12 resolves to cyan (index 12). */
    while (pal.colors.size() < 12)
        pal.colors << QColor("#e0e0e0");     /* neutral fills */
    if (pal.colors.size() == 12)
        pal.colors << QColor("#00a8c8");      /* slot 12 = distinct cyan */
    s.pal = pal;                               /* carry the extended palette */
    return s;
}

/* glue: paint wet glue, pour, absorb + relax so bonds form. */
static Scene makeGlue(const PowderPalette &pal, std::uint32_t seed = 21u)
{
    Scene s; s.g.reset(QSize(256, 256), 4.0f, seed);
    const float wc = (256 / 2.0f) * 4.0f;
    s.g.paintGlue(wc, wc, 120.0f, 0.9f);
    GlitterParams p; p.amount = 1.4f; p.flakeSize = 0.8f;
    p.color = pal.colors[0];
    for (int i = 0; i < 5; ++i)
        s.g.pourGlitter(wc + (i - 2) * 7.0f, wc + (i - 2) * 5.0f, 20.0f + i, p);
    s.g.relax(); s.g.relax();
    s.g.absorb();          /* glue bonds loose powder -> stuck */
    s.g.relax(); s.g.decayFlow();
    seedCells(s.g, wc, wc, 60.0f, 321.0f);
    return s;
}

/* --------------------------------------------------------------- renderers */

static QImage renderScene(GpuRuntime &rt, const Scene &s,
                          float originX, float originY, float worldPerPx,
                          const ArtLightRig &rig, const PaperStock *paper,
                          qint64 timeMs, int uiDpr = 1)
{
    return rt.renderPowder(QSize(512, 512), s.g, s.pal,
                           originX, originY, worldPerPx, rig, paper,
                           timeMs, uiDpr);
}

static bool savePng(const QImage &img, const QString &path)
{
    return img.save(path, "PNG");
}

/* -------------------------------------------------- periodicity / lattice - */

/* Cheap lattice probe on the loaded heap PNG. Downsample the CENTRAL region
 * (where the heap lives) to grayscale and, for offsets 1..16, compute the mean
 * absolute pixel difference along rows (horizontal) and along columns
 * (vertical). A real powder heap is aperiodic: no dominant small period on both
 * axes simultaneously. Prints the profile; returns a loose hard FAIL verdict.
 *
 * We fail only if BOTH axes show a distinct peak at the SAME offset within
 * [5,9] (a 6-8px lattice) that is markedly above the profile's own average. */
struct LatticeProbe
{
    std::vector<double> h, v;      /* mean |diff| per offset 1..16 */
    bool hardFail = false;
};
static LatticeProbe probeLattice(const QImage &src)
{
    LatticeProbe out;
    const QImage img = src.convertToFormat(QImage::Format_Grayscale8);
    const int W = img.width(), H = img.height();
    /* central window: 256x256 px centred on the 512 buffer -> the heap. */
    const int x0 = W / 2 - 128, y0 = H / 2 - 128, w = 256, h = 256;

    const int X0 = std::max(0, x0), Y0 = std::max(0, y0);
    const int X1 = std::min(W, x0 + w), Y1 = std::min(H, y0 + h);
    const QRect region(X0, Y0, X1 - X0, Y1 - Y0);

    std::vector<unsigned char> g8(img.sizeInBytes());
    std::memcpy(g8.data(), img.constBits(), img.sizeInBytes());

    out.h.assign(17, 0.0);
    out.v.assign(17, 0.0);
    for (int off = 1; off <= 16; ++off)
    {
        double hsum = 0, vsum = 0; qint64 hn = 0, vn = 0;
        for (int y = region.top(); y < region.bottom(); ++y)
        {
            const unsigned char *row = g8.data() + y * W;
            for (int x = region.left(); x + off < region.right(); ++x)
            {
                hsum += std::abs(int(row[x]) - int(row[x + off]));
                ++hn;
            }
        }
        for (int x = region.left(); x < region.right(); ++x)
        {
            for (int y = region.top(); y + off < region.bottom(); ++y)
            {
                const unsigned char *row = g8.data() + y * W;
                const unsigned char *row2 = g8.data() + (y + off) * W;
                vsum += std::abs(int(row[x]) - int(row2[x]));
                ++vn;
            }
        }
        out.h[off] = hn ? hsum / hn : 0.0;
        out.v[off] = vn ? vsum / vn : 0.0;
    }

    auto peakOffset = [](const std::vector<double> &a) -> int {
        int best = 1; double bestv = -1.0;
        for (int o = 1; o <= 16; ++o)
            if (a[o] > bestv) { bestv = a[o]; best = o; }
        return best;
    };
    auto meanOf = [](const std::vector<double> &a) -> double {
        double s = 0; int n = 0;
        for (int o = 1; o <= 16; ++o) { s += a[o]; ++n; }
        return n ? s / n : 0.0;
    };

    const int ph = peakOffset(out.h), pv = peakOffset(out.v);
    const double mh = meanOf(out.h), mv = meanOf(out.v);
    const double phVal = out.h[ph], pvVal = out.v[pv];
    /* a real lattice shows a sharp, narrow peak: value well above the profile
     * average, at a small even period, on BOTH axes at the SAME offset. */
    const bool bothSameSmallPeriod = (ph == pv) && ph >= 5 && ph <= 9;
    const bool sharpH = phVal > mh * 1.4 && (mh > 0.0);
    const bool sharpV = pvVal > mv * 1.4 && (mv > 0.0);
    out.hardFail = bothSameSmallPeriod && sharpH && sharpV;

    std::printf("    periodicity h: ");
    for (int o = 1; o <= 16; ++o) std::printf("%.1f ", out.h[o]);
    std::printf("\n    periodicity v: ");
    for (int o = 1; o <= 16; ++o) std::printf("%.1f ", out.v[o]);
    std::printf("\n    -> h peak @%d (%.1f, mean %.1f), v peak @%d (%.1f, mean %.1f)\n",
                ph, phVal, mh, pv, pvVal, mv);
    return out;
}

/* ---------------------------------------------------------- main ---------- */

int main(int argc, char **argv)
{
    std::printf("pb_gpu_powder_harness\n");
    QGuiApplication app(argc, argv);

    /* ---- output dir = harness build dir (where the exe lives) ---- */
    QString buildDir = QDir::currentPath();
    if (argc > 1)
        buildDir = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(buildDir);

    PowderPalette pal = defaultPowderPalette();

    /* ---- 1. backend -------------------------------------------------- */
    GpuRuntime rt;
    const bool initOk = rt.initialize(true);
    std::printf("backend: %s  isReady=%d\n",
                rt.isReady() ? qPrintable(rt.backendName()) : "(none)",
                rt.isReady() ? 1 : 0);
    if (!rt.isReady())
    {
        std::printf("  [%s] backend initialised (lastError: %s)\n",
                    "FAIL", qPrintable(rt.lastError()));
        std::printf("GPU_HARNESS 0/18 -- no backend, nothing rendered\n");
        return 1;
    }
    CHECK("backend created and isReady", initOk && rt.isReady());
    const PaperStock *paper = nullptr;   /* shader uses uPaper + default tooth */

    /* ---- view transform: 512px @ 0.5 world/px spans 256 world, centred on
     *      the world heap at (512,512) -> origin 384. */
    const float originX = 384.0f, originY = 384.0f, worldPerPx = 0.5f;
    const qint64 T0 = 0, T1 = 1234;

    /* ---- thin & dense ------------------------------------------------- */
    Scene thin  = makeThin(pal);
    Scene dense = makeDense(pal);
    Scene mixed = makeMixed(pal);
    Scene glue  = makeGlue(pal);

    /* light rig variations (mirror a scene-space light). */
    ArtLightRig rigLeft;  rigLeft.direction  = QVector3D(-0.4f, 0.8f, 0.6f);
    ArtLightRig rigRight; rigRight.direction = QVector3D( 0.4f, 0.8f, 0.6f);
    ArtLightRig rig0;

    /* ---- render the 8 PNGs ------------------------------------------- */
    QImage imgThin  = renderScene(rt, thin,  originX, originY, worldPerPx, rig0, paper, T0);
    QImage imgDense = renderScene(rt, dense, originX, originY, worldPerPx, rig0, paper, T0);
    QImage imgMixed = renderScene(rt, mixed, originX, originY, worldPerPx, rig0, paper, T0);
    QImage imgGlue  = renderScene(rt, glue,  originX, originY, worldPerPx, rig0, paper, T0);
    QImage imgT0    = renderScene(rt, dense, originX, originY, worldPerPx, rig0, paper, T0);
    QImage imgT1    = renderScene(rt, dense, originX, originY, worldPerPx, rig0, paper, T1);
    QImage imgLtL   = renderScene(rt, dense, originX, originY, worldPerPx, rigLeft,  paper, T0);
    QImage imgLtR   = renderScene(rt, dense, originX, originY, worldPerPx, rigRight, paper, T0);

    struct Named {
        const char *name; const QImage &img;
    };
    const Named imgs[8] = {
        { "gpu_powder_thin.png",       imgThin  },
        { "gpu_powder_dense.png",      imgDense },
        { "gpu_powder_mixed.png",      imgMixed },
        { "gpu_powder_glue.png",       imgGlue  },
        { "gpu_powder_t0.png",         imgT0    },
        { "gpu_powder_t1.png",         imgT1    },
        { "gpu_powder_light_left.png", imgLtL   },
        { "gpu_powder_light_right.png",imgLtR   },
    };

    /* ---- write the PNGs + validate reload ----------------------------- */
    qint64 sizes[8];
    std::printf("-- writing PNGs into %s\n", qPrintable(buildDir));
    for (int i = 0; i < 8; ++i)
    {
        const QString path = QDir(buildDir).filePath(QString::fromUtf8(imgs[i].name));
        const bool wr = savePng(imgs[i].img, path);
        QImage reloaded;
        qint64 bytes = 0;
        if (wr && QFile::exists(path))
        {
            bytes = QFileInfo(path).size();
            reloaded.load(path);
        }
        sizes[i] = bytes;
        std::printf("  %-28s wrote=%s size=%lldB reloaded=%s %dx%d\n",
                    imgs[i].name, wr ? "yes" : "NO", (long long)bytes,
                    reloaded.isNull() ? "no" : "yes",
                    reloaded.width(), reloaded.height());
    }

    /* ---- 2. each PNG is real data (valid non-null, non-empty, has some
     *         non-background pixels, recall reload round-trip). -------- */
    std::printf("-- per-image content checks\n");
    for (int i = 0; i < 8; ++i)
    {
        char label[96];
        std::snprintf(label, sizeof(label), "png fine: %s", imgs[i].name);
        const QImage reloaded = [&]() {
            QImage r;
            const QString path = QDir(buildDir).filePath(QString::fromUtf8(imgs[i].name));
            r.load(path);
            return r;
        }();
        const bool valid = !imgs[i].img.isNull() && !reloaded.isNull()
                         && reloaded.width() == 512 && reloaded.height() == 512
                         && sizes[i] > 0;
        const ImgStats st = analyzeImage(reloaded, QColor(255, 255, 255));
        const bool content = st.distinct >= 20 && st.nonBg > 0;
        std::printf("  %-30s distinct=%d nonBg=%lld\n",
                    imgs[i].name, st.distinct, (long long)st.nonBg);
        CHECK(label, valid);
        std::snprintf(label, sizeof(label), "content: %s (>=20 colors, non-bg) %d colors",
                      imgs[i].name, st.distinct);
        CHECK(label, content);
    }

    /* ---- 3. dense differs from thin ----------------------------------- */
    CHECK("dense differs from thin",
          meanAbsDiff(imgDense, imgThin) > 1.0);

    /* ---- 4. t0 vs t1 differ (time drives shimmer) ---------------------- */
    {
        const double d = meanAbsDiff(imgT0, imgT1);
        std::printf("    shimmer t0 vs t1 mean|diff| = %.4f\n", d);
        CHECK("t0 vs t1 differ (time drives shimmer)", d > 0.0);
    }

    /* ---- 5. light_left vs light_right differ (ableren rig shades) ----- */
    {
        const double d = meanAbsDiff(imgLtL, imgLtR);
        std::printf("    light_left vs light_right mean|diff| = %.4f\n", d);
        CHECK("light_left differs from light_right (rig drives shading)", d > 0.0);
    }

    /* ---- 6. the heap is NOT a regular grid (lattice probe) ------------ */
    {
        std::printf("-- lattice probe on gpu_powder_dense.png (central 256px window) --\n");
        QImage rel;
        rel.load(QDir(buildDir).filePath(QStringLiteral("gpu_powder_dense.png")));
        if (rel.isNull())
            CHECK("heap is not a regular grid", false);
        else
        {
            LatticeProbe lp = probeLattice(rel);
            CHECK("heap is not a regular grid (no dominant same-period lattice on both axes)",
                  !lp.hardFail);
        }
    }

    /* ---- mixed two-colour proof (slot0 pink & slot12 cyan present) ---- */
    {
        QImage rel;
        rel.load(QDir(buildDir).filePath(QStringLiteral("gpu_powder_mixed.png")));
        int pinkPx = 0, cyanPx = 0;
        if (!rel.isNull())
        {
            const QImage img = rel.convertToFormat(QImage::Format_ARGB32);
            /* Shaded grains: test by *hue family* rather than exact colour.
             * pink family: red dominates blue/green (r high, b notably lower).
             * cyan/blue family: blue dominates red (b >= g and b > r + 25). */
            for (int y = 0; y < img.height(); ++y)
            {
                const QRgb *px = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x)
                {
                    const QColor c(px[x]);
                    const int r = c.red(), g = c.green(), b = c.blue();
                    const bool pinkish = r > 140 && b < r - 30 && g < r - 10;
                    const bool cyanish  = b > 90 && b >= g && b > r + 25 && g > r;
                    if (pinkish) ++pinkPx;
                    else if (cyanish) ++cyanPx;
                }
            }
        }
        std::printf("    mixed two-slot: pink-family px=%d cyan/blue-family px=%d\n",
                    pinkPx, cyanPx);
        CHECK("mixed contains BOTH slot0 pink and slot12 cyan grains",
              pinkPx > 0 && cyanPx > 0);
    }

    /* ---- glue state actually bonds ------------------------------------ */
    CHECK("glue heap has bonded cells", glue.g.bondedCount() > 0);

    /* ---- final summary ------------------------------------------------ */
    std::printf("\nGPU_HARNESS %d/%d\n", s_pass, s_checks);
    return s_pass == s_checks ? 0 : 1;
}