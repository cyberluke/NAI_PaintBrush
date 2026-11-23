/*****************************************************************************
 * matte_core_test.cpp : NAI Paintbrush — matting core acceptance harness
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This is a REAL behaviour harness for pb_matte.cpp: it builds synthetic
 * fixtures with known ground truth and asserts the documented decisions.
 * It needs Qt Core/Gui only (no VLC, no GPU, no model). It is the evidence
 * gate for the deterministic stages:
 *
 *   - detached specks are removed, nearby real fragments are kept
 *   - enclosed socks-like holes are restored
 *   - legitimate openings stay transparent (deferred to the unknown band)
 *   - user prompts always win over heuristics
 *   - the trimap has the three states and a bounded unknown band
 *   - refined alpha is genuinely fractional at the edge
 *   - foreground reconstruction reduces contamination against ground truth
 *   - every premultiplied pixel obeys rgb <= alpha
 *****************************************************************************/

#include "../pb_matte.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QtMath>

#include <cmath>
#include <cstdio>

using namespace paintbrush::matte;

namespace {

int g_failures = 0;
int g_checks = 0;
QStringList g_lines;

void check(bool condition, const QString &what, const QString &detail = QString())
{
    ++g_checks;
    const QString line = QStringLiteral("%1 %2%3")
                             .arg(condition ? "PASS" : "FAIL")
                             .arg(what)
                             .arg(detail.isEmpty() ? QString()
                                                   : QStringLiteral("  [%1]").arg(detail));
    g_lines << line;
    std::printf("%s\n", line.toUtf8().constData());
    std::fflush(stdout);
    if (!condition)
        ++g_failures;
}

QImage makeDisc(int w, int h, QPointF center, double radius, QRgb color)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::black);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromRgb(color));
    p.drawEllipse(center, radius, radius);
    p.end();
    return img;
}

QImage makeMaskDisc(int w, int h, QPointF center, double radius)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    img.fill(0);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(center, radius, radius);
    p.end();
    return img;
}

void punchHole(QImage &mask, QPointF center, double radius, bool fillWhite)
{
    QPainter p(&mask);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.setPen(Qt::NoPen);
    p.setBrush(fillWhite ? QColor(Qt::white) : QColor(Qt::black));
    p.drawEllipse(center, radius, radius);
    p.end();
}

void paintBlob(QImage &mask, QRect rect)
{
    QPainter p(&mask);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(rect);
    p.end();
}

QImage levelsToBinary(const QImage &levels)
{
    QImage bin(levels.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < levels.height(); ++y)
    {
        const uchar *src = levels.constScanLine(y);
        uchar *dst = bin.scanLine(y);
        for (int x = 0; x < levels.width(); ++x)
            dst[x] = src[x] >= 128 ? 255 : 0;
    }
    return bin;
}

bool anyPixel(const QImage &img, const QRect &rect, int threshold = 128)
{
    const QRect r = rect.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y)
    {
        const uchar *line = img.constScanLine(y);
        for (int x = r.left(); x <= r.right(); ++x)
            if (line[x] >= threshold)
                return true;
    }
    return false;
}

int countPixels(const QImage &img, int exact, int tolerance = 0)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
    {
        const uchar *line = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            if (std::abs(int(line[x]) - exact) <= tolerance)
                ++n;
    }
    return n;
}

QImage srgbComposite(const QImage &foreground, const QImage &background,
                     const QImage &alpha8)
{
    QImage fg = foreground.convertToFormat(QImage::Format_RGB32);
    QImage bg = background.convertToFormat(QImage::Format_RGB32);
    QImage out(fg.size(), QImage::Format_RGB32);
    for (int y = 0; y < fg.height(); ++y)
    {
        const QRgb *fl = reinterpret_cast<const QRgb *>(fg.constScanLine(y));
        const QRgb *bl = reinterpret_cast<const QRgb *>(bg.constScanLine(y));
        const uchar *al = alpha8.isNull() ? nullptr : alpha8.constScanLine(y);
        QRgb *ol = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < fg.width(); ++x)
        {
            const double a = al ? double(al[x]) / 255.0 : 1.0;
            /* linear-light composite, encoded back to sRGB */
            auto lin = [](double c) {
                return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
            };
            auto srgb = [](double c) {
                return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
            };
            const int r = int(std::lround(srgb(lin(qRed(fl[x]) / 255.0) * a
                                               + lin(qRed(bl[x]) / 255.0) * (1 - a)) * 255.0));
            const int g = int(std::lround(srgb(lin(qGreen(fl[x]) / 255.0) * a
                                               + lin(qGreen(bl[x]) / 255.0) * (1 - a)) * 255.0));
            const int b = int(std::lround(srgb(lin(qBlue(fl[x]) / 255.0) * a
                                               + lin(qBlue(bl[x]) / 255.0) * (1 - a)) * 255.0));
            ol[x] = qRgb(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
        }
    }
    return out;
}

void testDetachedComponents()
{
    const int W = 512, H = 512;
    QImage mask(W, H, QImage::Format_Grayscale8);
    mask.fill(0);
    paintBlob(mask, QRect(120, 120, 300, 300));
    /* isolated speck far from the subject */
    paintBlob(mask, QRect(462, 30, 28, 28));
    /* a real fragment 10 px away from the main subject */
    paintBlob(mask, QRect(95, 250, 22, 22));

    Inputs in;
    in.rgb = makeDisc(W, H, QPointF(256, 256), 150, qRgb(200, 180, 160));
    in.mask = mask;

    Parameters p;
    p.artifactSuppression = 0.9;
    const TopologyReport report = analyseTopology(in, p);

    check(report.detachedRemoved == 1 && report.detachedKept == 1,
          "detached specks: one removed, one real fragment kept",
          QStringLiteral("removed=%1 kept=%2").arg(report.detachedRemoved)
                                                    .arg(report.detachedKept));
    check(!anyPixel(report.repairedMask, QRect(460, 28, 32, 32)),
          "removed speck is absent from the repaired mask");
    check(anyPixel(report.repairedMask, QRect(93, 248, 26, 26)),
          "nearby real fragment survives repair");
    check(anyPixel(report.removedMask, QRect(460, 28, 32, 32)),
          "removed speck is reported for diagnostics");
}

void testEnclosedHoles()
{
    const int W = 512, H = 512;
    QImage mask = makeMaskDisc(W, H, QPointF(256, 260), 160);
    /* socks-like hole: small, same material as the surrounding subject */
    punchHole(mask, QPointF(230, 260), 11, false);
    /* legitimate opening: large and visually different (background shows) */
    punchHole(mask, QPointF(320, 260), 34, false);

    QImage rgb = makeDisc(W, H, QPointF(256, 260), 160, qRgb(222, 190, 168));
    QPainter p(&rgb);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 120, 220)); /* blue background inside the opening */
    p.drawEllipse(QPointF(320, 260), 34, 34);
    p.end();

    Inputs in;
    in.rgb = rgb;
    in.mask = mask;

    Parameters dflt;
    dflt.holeRecovery = 0.7;
    TopologyReport report = analyseTopology(in, dflt);
    check(report.holesFound == 2, "two enclosed holes detected",
          QStringLiteral("found=%1").arg(report.holesFound));
    check(report.holesFilled == 1, "socks-like hole restored",
          QStringLiteral("filled=%1").arg(report.holesFilled));
    check(report.holesDeferred == 1, "legitimate opening deferred, not filled",
          QStringLiteral("deferred=%1 kept=%2")
              .arg(report.holesDeferred).arg(report.holesKept));
    check(anyPixel(report.repairedMask, QRect(220, 250, 22, 22)),
          "restored hole is foreground in the repaired mask");
    check(!anyPixel(report.repairedMask, QRect(300, 240, 40, 40)),
          "legitimate opening stays transparent");

    /* Explicit user intent must win over the colour heuristic. */
    Inputs neg = in;
    neg.negativePoints << QPointF(230, 260);
    const TopologyReport reportNeg = analyseTopology(neg, dflt);
    check(reportNeg.holesFilled == 0,
          "negative prompt on a hole prevents automatic filling",
          QStringLiteral("filled=%1").arg(reportNeg.holesFilled));

    /* Disabling recovery must disable filling. */
    Parameters off = dflt;
    off.restoreEnclosedHoles = false;
    const TopologyReport reportOff = analyseTopology(in, off);
    check(reportOff.holesFilled == 0,
          "hole recovery switch is honoured",
          QStringLiteral("filled=%1").arg(reportOff.holesFilled));
}

void testTrimapAndAlpha()
{
    const int W = 480, H = 640; /* portrait */
    QImage mask = makeMaskDisc(W, H, QPointF(240, 320), 170);
    QImage rgb = makeDisc(W, H, QPointF(240, 320), 170, qRgb(90, 200, 190));

    Inputs in;
    in.rgb = rgb;
    in.mask = mask;
    Parameters p;
    const TopologyReport topo = analyseTopology(in, p);
    double unknownFraction = 0.0;
    const QImage trimap = buildTrimap(topo.repairedMask, rgb, QImage(), topo, p,
                                      nullptr, nullptr, &unknownFraction);

    check(!trimap.isNull() && trimap.size() == mask.size(),
          "trimap matches the source resolution");
    const int fg = countPixels(trimap, 255);
    const int bg = countPixels(trimap, 0);
    const int unknown = countPixels(trimap, 128);
    check(fg > 0 && bg > 0 && unknown > 0,
          "trimap has foreground, background and unknown states",
          QStringLiteral("fg=%1 bg=%2 unknown=%3").arg(fg).arg(bg).arg(unknown));
    check(unknownFraction > 0.0 && unknownFraction < 0.30,
          "unknown band is bounded (not a global feather)",
          QStringLiteral("fraction=%1").arg(unknownFraction, 0, 'f', 4));
    /* Portrait: the band scales with the short edge (480/1080). */
    const int expectedOuter = p.outerBandFor(std::min(W, H));
    check(expectedOuter >= 2 && expectedOuter <= 6,
          "outer band scales with the short edge",
          QStringLiteral("outer=%1 px at %2").arg(expectedOuter).arg(std::min(W, H)));
    check(trimap.pixelColor(W / 2, H / 2).red() == 255,
          "subject interior is confidently foreground");
    check(trimap.pixelColor(2, 2).red() == 0,
          "far background is confidently background");

    const AlphaPlane alpha = refineAlphaDeterministic(topo.repairedMask, rgb, p, trimap);
    int fractional = 0;
    for (int y = 0; y < alpha.height(); ++y)
        for (int x = 0; x < alpha.width(); ++x)
        {
            const float a = alpha.at(x, y);
            if (a > 0.02f && a < 0.98f)
                ++fractional;
        }
    check(fractional > 200, "refined alpha is genuinely fractional at the edge",
          QStringLiteral("fractional=%1 px").arg(fractional));
    check(alpha.at(W / 2, H / 2) > 0.99f, "subject core is opaque");
    check(alpha.at(2, 2) < 0.01f, "far background is transparent");
    check(alpha.toGrayscale16().format() == QImage::Format_Grayscale16,
          "production alpha is stored at 16-bit precision");
}

void testDecontamination()
{
    const int W = 384, H = 384;
    const QPointF center(192, 192);
    const double radius = 120.0;

    const QColor foregroundColor(24, 196, 172);   /* turquoise dress */
    const QColor backgroundColor(40, 130, 240);   /* blue/cyan studio */

    QImage alpha8(W, H, QImage::Format_Grayscale8);
    alpha8.fill(0);
    {
        QPainter p(&alpha8);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(center, radius, radius);
        p.end();
    }
    QImage fgImage(W, H, QImage::Format_RGB32);
    fgImage.fill(foregroundColor);
    QImage bgImage(W, H, QImage::Format_RGB32);
    bgImage.fill(backgroundColor);
    const QImage composite = srgbComposite(fgImage, bgImage, alpha8);

    QImage mask = levelsToBinary(alpha8);
    QImage trimap(W, H, QImage::Format_Grayscale8);
    AlphaPlane alpha(W, H);
    for (int y = 0; y < H; ++y)
    {
        const uchar *al = alpha8.constScanLine(y);
        uchar *tri = trimap.scanLine(y);
        for (int x = 0; x < W; ++x)
        {
            const float a = float(al[x]) / 255.0f;
            alpha.setAt(x, y, a);
            tri[x] = a >= 0.99f ? 255 : (a <= 0.01f ? 0 : 128);
        }
    }

    Inputs in;
    in.rgb = composite;
    in.mask = mask;
    Parameters p;
    p.decontamination = 1.0;
    Result result = refineDeterministic(in, p);
    result = applyNeuralAlpha(in, result, alpha, p,
                              QStringLiteral("test"), QStringLiteral("fixture"), 0.0);

    double baselineError = 0.0;
    double refinedError = 0.0;
    int bandPixels = 0;
    int turquoiseHueBetter = 0;
    int turquoiseHueWorse = 0;
    QImage refined = result.foregroundRgb.convertToFormat(QImage::Format_RGB32);
    QImage source = composite.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < H; ++y)
    {
        const QRgb *sl = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        const QRgb *rl = reinterpret_cast<const QRgb *>(refined.constScanLine(y));
        const uchar *al = alpha8.constScanLine(y);
        for (int x = 0; x < W; ++x)
        {
            const float a = float(al[x]) / 255.0f;
            if (a < 0.12f || a > 0.88f)
                continue;
            ++bandPixels;
            baselineError += std::abs(qRed(sl[x]) - foregroundColor.red())
                             + std::abs(qGreen(sl[x]) - foregroundColor.green())
                             + std::abs(qBlue(sl[x]) - foregroundColor.blue());
            refinedError += std::abs(qRed(rl[x]) - foregroundColor.red())
                            + std::abs(qGreen(rl[x]) - foregroundColor.green())
                            + std::abs(qBlue(rl[x]) - foregroundColor.blue());
            const bool sourceTurquoise = qGreen(sl[x]) > qBlue(sl[x]);
            const bool refinedTurquoise = qGreen(rl[x]) > qBlue(rl[x]);
            if (!sourceTurquoise && refinedTurquoise)
                ++turquoiseHueBetter;
            if (sourceTurquoise && !refinedTurquoise)
                ++turquoiseHueWorse;
        }
    }
    check(bandPixels > 500, "boundary band has enough pixels for measurement",
          QStringLiteral("band=%1").arg(bandPixels));
    const double baselineMean = baselineError / double(bandPixels) / 3.0;
    const double refinedMean = refinedError / double(bandPixels) / 3.0;
    check(refinedMean < baselineMean * 0.6,
          "foreground reconstruction reduces boundary colour error by >40%",
          QStringLiteral("baseline=%1 refined=%2")
              .arg(baselineMean, 0, 'f', 1).arg(refinedMean, 0, 'f', 1));
    check(turquoiseHueBetter > turquoiseHueWorse * 4,
          "blue/cyan contamination is reduced without destroying the turquoise",
          QStringLiteral("better=%1 worse=%2")
              .arg(turquoiseHueBetter).arg(turquoiseHueWorse));

    /* Premultiplication invariant over the whole texture. */
    const QImage premul = result.premultipliedRgba;
    bool premulOk = premul.format() == QImage::Format_ARGB32_Premultiplied;
    int maxViolation = 0;
    for (int y = 0; y < premul.height() && premulOk; ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(premul.constScanLine(y));
        for (int x = 0; x < premul.width(); ++x)
        {
            const int a = qAlpha(line[x]);
            const int violation = std::max({ qRed(line[x]) - a,
                                             qGreen(line[x]) - a,
                                             qBlue(line[x]) - a });
            if (violation > 0)
            {
                premulOk = false;
                maxViolation = violation;
                break;
            }
        }
    }
    check(premulOk, "every premultiplied pixel obeys rgb <= alpha",
          QStringLiteral("maxViolation=%1").arg(maxViolation));
    check(qAlpha(premul.pixel(W / 2, H / 2)) == 255,
          "opaque subject centre stays fully opaque");
    check(qAlpha(premul.pixel(2, 2)) == 0,
          "transparent background has zero premultiplied RGB");

    /* Opaque colours must remain byte-identical to the source. */
    const QRgb srcCenter = source.pixel(W / 2, H / 2);
    const QRgb refinedCenter = refined.pixel(W / 2, H / 2);
    check(qRed(refinedCenter) == qRed(srcCenter)
              && qGreen(refinedCenter) == qGreen(srcCenter)
              && qBlue(refinedCenter) == qBlue(srcCenter),
          "fully opaque foreground colour is preserved exactly");
}

void testThinStructures()
{
    const int W = 512, H = 512;
    QImage mask(W, H, QImage::Format_Grayscale8);
    mask.fill(0);
    {
        QPainter p(&mask);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(150, 256), 60, 60);
        p.drawEllipse(QPointF(362, 256), 60, 60);
        /* 2 px false bridge between the two legs */
        p.fillRect(QRect(150, 255, 212, 2), Qt::white);
        p.end();
    }
    QImage rgb(W, H, QImage::Format_RGB32);
    rgb.fill(QColor(30, 30, 30));

    Inputs in;
    in.rgb = rgb;
    in.mask = mask;
    Parameters p;
    const TopologyReport topo = analyseTopology(in, p);
    const QImage trimap = buildTrimap(topo.repairedMask, rgb, QImage(), topo, p);

    check(trimap.pixelColor(150, 256).red() == 255,
          "thin-structure analysis keeps the subject core foreground");
    check(trimap.pixelColor(256, 256).red() == 128,
          "a 2 px false bridge becomes unknown, not hard foreground");
    /* A 14 px bar must survive erosion+regrowth and stay confirmed. */
    QImage bar(W, H, QImage::Format_Grayscale8);
    bar.fill(0);
    {
        QPainter q(&bar);
        q.fillRect(QRect(150, 249, 212, 14), Qt::white);
        q.end();
    }
    Inputs barIn;
    barIn.rgb = rgb;
    barIn.mask = bar;
    const TopologyReport barTopo = analyseTopology(barIn, p);
    const QImage barTri = buildTrimap(barTopo.repairedMask, rgb, QImage(),
                                      barTopo, p);
    check(barTri.pixelColor(256, 256).red() == 255,
          "a 14 px bar is not misclassified as a thin structure");

    /* A realistic, multi-pixel ankle bridge: the old fixed ~4 px opening
     * radius missed anything wider than a 1-2 px leak. */
    QImage bridge10(W, H, QImage::Format_Grayscale8);
    bridge10.fill(0);
    {
        QPainter q(&bridge10);
        q.setRenderHint(QPainter::Antialiasing, false);
        q.setPen(Qt::NoPen);
        q.setBrush(Qt::white);
        q.drawEllipse(QPointF(150, 256), 60, 60);
        q.drawEllipse(QPointF(362, 256), 60, 60);
        q.fillRect(QRect(150, 251, 212, 10), Qt::white);
        q.end();
    }
    Inputs bridgeIn;
    bridgeIn.rgb = rgb;
    bridgeIn.mask = bridge10;
    const TopologyReport bridgeTopo = analyseTopology(bridgeIn, p);
    const QImage bridgeTri = buildTrimap(bridgeTopo.repairedMask, rgb, QImage(),
                                         bridgeTopo, p);
    check(bridgeTri.pixelColor(256, 256).red() == 128,
          "a 10 px false bridge becomes unknown, not hard foreground");
    check(bridgeTri.pixelColor(150, 256).red() == 255,
          "a 10 px bridge does not swallow the subject core");

    /* A thick bar is a real structure: never downgrade it to unknown. */
    QImage bar24(W, H, QImage::Format_Grayscale8);
    bar24.fill(0);
    {
        QPainter q(&bar24);
        q.fillRect(QRect(150, 244, 212, 24), Qt::white);
        q.end();
    }
    Inputs bar24In;
    bar24In.rgb = rgb;
    bar24In.mask = bar24;
    const TopologyReport bar24Topo = analyseTopology(bar24In, p);
    const QImage bar24Tri = buildTrimap(bar24Topo.repairedMask, rgb, QImage(),
                                        bar24Topo, p);
    check(bar24Tri.pixelColor(256, 256).red() == 255,
          "a 24 px bar stays confirmed foreground");

    /* Two legs with a clear gap must not manufacture unknown between them. */
    QImage gap(W, H, QImage::Format_Grayscale8);
    gap.fill(0);
    {
        QPainter q(&gap);
        q.setRenderHint(QPainter::Antialiasing, false);
        q.setPen(Qt::NoPen);
        q.setBrush(Qt::white);
        q.drawEllipse(QPointF(150, 256), 60, 60);
        q.drawEllipse(QPointF(362, 256), 60, 60);
        q.end();
    }
    Inputs gapIn;
    gapIn.rgb = rgb;
    gapIn.mask = gap;
    const TopologyReport gapTopo = analyseTopology(gapIn, p);
    const QImage gapTri = buildTrimap(gapTopo.repairedMask, rgb, QImage(),
                                      gapTopo, p);
    check(gapTri.pixelColor(256, 256).red() == 0,
          "a clear gap between legs stays background");
}

void testCacheIdentity()
{
    FrameIdentity id;
    id.mediaId = QStringLiteral("file:///dance.mp4");
    id.ptsMs = 12807;
    id.subjectId = QStringLiteral("subject-a");
    id.selectionRevision = 3;
    id.refinementRevision = 1;
    Parameters p;
    const QString a = id.cacheKey(p, QStringLiteral("vitmatte-s/512"));
    id.refinementRevision = 2;
    const QString b = id.cacheKey(p, QStringLiteral("vitmatte-s/512"));
    check(a != b, "cache key changes with the refinement revision");
    id.refinementRevision = 1;
    check(a == id.cacheKey(p, QStringLiteral("vitmatte-s/512")),
          "cache key is stable for identical identity + parameters");
    p.decontamination = 0.1;
    check(a != id.cacheKey(p, QStringLiteral("vitmatte-s/512")),
          "cache key changes with refinement parameters");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::printf("NAI Paintbrush matte core harness\n");
    testDetachedComponents();
    testEnclosedHoles();
    testTrimapAndAlpha();
    testDecontamination();
    testThinStructures();
    testCacheIdentity();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
