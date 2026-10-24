/*****************************************************************************
 * pb_gpu_harness.cpp : NAI Paintbrush — GPU Art Runtime acceptance harness
 *****************************************************************************
 * Compiles with Qt6 ONLY (no VLC build required):
 *
 *   MOC=C:/msys64/mingw64/share/qt6/bin/moc.exe
 *   QT_INC=C:/msys64/mingw64/include/qt6
 *   QT_LIB=C:/msys64/mingw64/lib
 *   PB=../modules/gui/qt/paintbrush
 *
 *   g++ -std=c++17 -O2 -I$QT_INC -I$QT_LIB/Qt6Core/mkspecs/win32-g++ \
 *       -I$PB pb_gpu_harness.cpp stub_vlc.cpp \
 *       $PB/pb_common.cpp $PB/pb_layer.cpp $PB/pb_document.cpp \
 *       $PB/pb_history.cpp $PB/pb_harvest.cpp $PB/pb_boost.cpp \
 *       $PB/pb_project_store.cpp \
 *       $PB/art/runtime/material_registry.cpp \
 *       $PB/art/surface/paper_stock.cpp $PB/art/surface/paper_renderer.cpp \
 *       $PB/art/stroke/bristle_brush.cpp \
 *       $PB/art/materials/glitter/glitter_material.cpp \
 *       $PB/art/finish/effects.cpp \
 *       $PB/art/style/mood.cpp $PB/art/style/typography_style.cpp \
 *       $PB/art/style/badges.cpp \
 *       $PB/art/gpu/pb_gpu_runtime.cpp \
 *       -L$QT_LIB -lQt6Core -lQt6Gui -o pb_gpu_harness.exe
 *
 * It renders the SAME scenes through the GPU Art Runtime (QRhi) and the CPU
 * golden path and asserts GPU == CPU within tolerance. This is the Showroom
 * regression gate: every future GPU shader change must keep the GPU picture
 * equal to the golden CPU picture (spec §70: GPU polish, CPU stays the
 * reference).
 *
 * Exit code 0 = all assertions passed.
 *****************************************************************************/

#include "pb_document.hpp"
#include "art/gpu/pb_gpu_runtime.hpp"
#include "art/surface/paper_renderer.hpp"
#include "art/materials/glitter/glitter_material.hpp"
#include "art/finish/effects.hpp"
#include "art/style/mood.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QColor>
#include <QRgb>
#include <cstdio>
#include <cmath>

using namespace paintbrush;
using namespace paintbrush::art;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                     \
    do                                                                       \
    {                                                                        \
        ++g_checks;                                                          \
        if (cond)                                                            \
            std::printf("  ok: %s\n", msg);                                  \
        else                                                                 \
        {                                                                    \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

/* Mean absolute RGBA difference between two same-size images. */
static double meanAbsDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 1e9;
    const QImage ca = a.convertToFormat(QImage::Format_ARGB32);
    const QImage cb = b.convertToFormat(QImage::Format_ARGB32);
    double sum = 0.0;
    qint64 n = 0;
    for (int y = 0; y < ca.height(); ++y)
    {
        const QRgb *pa = reinterpret_cast<const QRgb *>(ca.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(cb.constScanLine(y));
        for (int x = 0; x < ca.width(); ++x)
        {
            sum += std::abs(qRed(pa[x]) - qRed(pb[x]));
            sum += std::abs(qGreen(pa[x]) - qGreen(pb[x]));
            sum += std::abs(qBlue(pa[x]) - qBlue(pb[x]));
            sum += std::abs(qAlpha(pa[x]) - qAlpha(pb[x]));
            n += 4;
        }
    }
    return n > 0 ? sum / double(n) : 0.0;
}

static void testPaper(GpuRuntime &gpu)
{
    std::printf("GPU paper == CPU paper (all six stocks, 1x + 2x)\n");
    const QVector<const PaperStock *> stocks = paper::presets();
    ArtLightRig rig = mood::defaultMood().light;
    ArtScale scale;
    scale.referenceDpi = 96.0f;
    scale.worldUnitsPerInch = 96.0f;
    scale.exportScale = 1.0f;

    for (const PaperStock *stock : stocks)
    {
        for (float exp : { 1.0f, 2.0f })
        {
            const QSize size(160, 120);
            ArtScale s = scale;
            s.exportScale = exp;
            const float worldPerPx = 1.0f / exp;

            /* CPU golden */
            PaperRenderer cpu;
            PaperRenderer::State st;
            st.stock = stock;
            st.rig = &rig;
            st.scale = s;
            const QImage golden = cpu.render(size, st, QRect(0, 0, 160, 120));

            /* GPU */
            QImage gpuImg = gpu.renderPaper(size, *stock, rig, s, 0.0f, 0.0f,
                                            worldPerPx, QRect(0, 0, 160, 120));
            if (gpuImg.isNull())
            {
                CHECK(false, qPrintable(QStringLiteral("paper %1 %2x GPU render").arg(stock->id).arg(exp)));
                continue;
            }
            const double d = meanAbsDiff(golden, gpuImg);
            /* deterministic integer hashing + identical float math: expect
             * near-bit-exact; allow a small epsilon for backend rounding */
            CHECK(d < 2.0,
                  qPrintable(QStringLiteral("paper %1 %2x mean|diff|=%3 < 2").arg(stock->id).arg(exp).arg(QString::number(d, 'f', 3))));
        }
    }
}

static void testGlitter(GpuRuntime &gpu)
{
    std::printf("GPU glitter == CPU glitter (deterministic state)\n");
    ArtLightRig rig = mood::defaultMood().light;
    const PaperStock *paper = paper::findById(QStringLiteral("luxury.greige"));

    /* the same dense stroke construction the art harness uses: many grains,
     * so the GPU-vs-CPU comparison is meaningful (not a near-empty pile) */
    GlitterParams p;
    p.seed = 0xBEEFu;
    p.amount = 1.2f;
    p.adhesion = 0.7f;
    p.looseMix = 0.3f;
    p.flakeSize = 0.8f;
    p.shimmer = 0.55f;
    p.palette = { QColor(0xE8, 0xC6, 0x2A), QColor(0xF2, 0xE0, 0x8A),
                  QColor(0xB8, 0x96, 0x1E), QColor(0xFF, 0x9E, 0xC7),
                  QColor(0x9C, 0xE8, 0xFF), QColor(0xFF, 0xFF, 0xFF) };
    const float cell = glitter::glitterWorldCellSize(p, 1.6f);

    glitter::GlitterState state;
    state.reset(QSize(int(200 / cell) + 2, int(120 / cell) + 2), cell);
    for (int x = 30; x <= 170; x += 6)
        state.applyGlitter(float(x), 60.0f, 18.0f, p, QPointF(0, 0), 1.0f);
    CHECK(state.grainCount() > 2000, qPrintable(QStringLiteral("state has grains (%1)").arg(state.grainCount())));

    const QSize size(200, 120);
    ArtScale scale;
    scale.referenceDpi = 96.0f;
    scale.worldUnitsPerInch = 96.0f;
    scale.exportScale = 1.0f;

    const QImage golden = state.render(size, 0.0f, 0.0f, 1.0f, rig, paper, scale);
    qint64 goldenPx = 0;
    for (int y = 0; y < golden.height(); ++y)
        for (int x = 0; x < golden.width(); ++x)
            if (qAlpha(golden.pixel(x, y)) > 0)
                ++goldenPx;
    CHECK(goldenPx > 3000, qPrintable(QStringLiteral("dense pile (%1 opaque px)").arg(goldenPx)));
    QImage gpuImg = gpu.renderGlitter(size, state, 0.0f, 0.0f, 1.0f, rig, paper);
    if (gpuImg.isNull())
    {
        CHECK(false, "glitter GPU render");
        return;
    }
    const double d = meanAbsDiff(golden, gpuImg);
    /* GPU rasterization differs at sub-pixel edges; the pile must match
     * closely (identical grain layout, lighting and palette) */
    CHECK(d < 12.0,
          qPrintable(QStringLiteral("glitter mean|diff|=%1 < 12").arg(QString::number(d, 'f', 3))));
    CHECK(gpuImg.width() == golden.width() && gpuImg.height() == golden.height(),
          "glitter output size matches");
}

static void testFinish(GpuRuntime &gpu)
{
    std::printf("GPU finish == CPU finish (blur / shadow / glow)\n");
    ArtLightRig rig = mood::defaultMood().light;
    ArtScale scale;
    scale.referenceDpi = 96.0f;
    scale.worldUnitsPerInch = 96.0f;
    scale.exportScale = 1.0f;

    /* a small "subject": a rounded heart-like blob on transparent */
    QImage subject(120, 120, QImage::Format_ARGB32);
    subject.fill(Qt::transparent);
    {
        QPainter p(&subject);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(0xE8, 0x6A, 0x8A));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(45, 45), 22, 22);
        p.drawEllipse(QPointF(75, 45), 22, 22);
        QPainterPath path;
        path.moveTo(22, 50);
        path.lineTo(98, 50);
        path.lineTo(60, 105);
        path.closeSubpath();
        p.drawPath(path);
        p.end();
    }

    /* 1) DirectedBlur */
    {
        EffectNode e = EffectNode::makeMotionBlur(QPointF(1.0, 0.0), 24.0f, 0.6f, 12);
        FinishResult cpu = FinishRuntime::apply(subject, { e }, rig, scale);
        QImage gpuImg = gpu.renderFinish(subject, 0, e, rig, scale);
        if (gpuImg.isNull())
        {
            CHECK(false, "motion blur GPU render");
        }
        else
        {
            const double d = meanAbsDiff(cpu.image, gpuImg);
            CHECK(d < 16.0,
                  qPrintable(QStringLiteral("directed blur mean|diff|=%1 < 16").arg(QString::number(d, 'f', 3))));
        }
    }

    /* 2) Shadow */
    {
        EffectNode e = EffectNode::makeShadow(8.0f, 0.5f);
        e.useLightRig = true;
        e.lightDistance = 8.0f;
        FinishResult cpu = FinishRuntime::apply(subject, { e }, rig, scale);
        QImage gpuImg = gpu.renderFinish(subject, 1, e, rig, scale);
        if (gpuImg.isNull())
        {
            CHECK(false, "shadow GPU render");
        }
        else
        {
            const double d = meanAbsDiff(cpu.image, gpuImg);
            CHECK(d < 16.0,
                  qPrintable(QStringLiteral("shadow mean|diff|=%1 < 16").arg(QString::number(d, 'f', 3))));
        }
    }

    /* 3) Glow */
    {
        EffectNode e = EffectNode::makeGlow(QColor(0xFF, 0xD7, 0x6E), 12.0f, 1.0f);
        FinishResult cpu = FinishRuntime::apply(subject, { e }, rig, scale);
        QImage gpuImg = gpu.renderFinish(subject, 2, e, rig, scale);
        if (gpuImg.isNull())
        {
            CHECK(false, "glow GPU render");
        }
        else
        {
            const double d = meanAbsDiff(cpu.image, gpuImg);
            CHECK(d < 16.0,
                  qPrintable(QStringLiteral("glow mean|diff|=%1 < 16").arg(QString::number(d, 'f', 3))));
        }
    }
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    std::printf("Paintbrush Showroom — GPU Art Runtime acceptance harness\n");
    std::printf("(QRhi backend renders paper/glitter/finish; CPU golden is the reference)\n\n");

    GpuRuntime gpu;
    if (!gpu.initialize(true))
    {
        std::printf("GPU backend unavailable: %s\n",
                    qPrintable(gpu.lastError()));
        std::printf("\nSKIPPED (environment without a GPU — CPU golden path remains authoritative)\n");
        return 0;
    }
    std::printf("backend: %s\n\n", qPrintable(gpu.backendName()));

    testPaper(gpu);
    testGlitter(gpu);
    testFinish(gpu);

    if (g_failures == 0)
    {
        std::printf("\nALL GPU ASSERTIONS PASSED (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("\n%d / %d GPU ASSERTION(S) FAILED\n", g_failures, g_checks);
    return 1;
}