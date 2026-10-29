// NAI Paintbrush — Motion Toys: PuppetEffect spike harness
//
// Answers the spike question: can a harvested photographic person be
// deformed ±5–10° (ARAP over a mask-derived 2D mesh + synthetic skeleton)
// and still look like a person? Verifies the deterministic Puppet runtime
// end to end: mesh build, skeleton fit, identity reconstruction, photo
// regime, illustration regime, loop seamlessness, seed/amplitude/speed
// semantics, serialization, edge cases, motion retargeting (source-mask
// trajectories onto a different figure), and a REAL SAM3 harvest cutout
// (pb_live_harness artifacts) when present.
//
// Build/run:  pwsh _REVIEW/harness/pb_puppet_harness/build_run.ps1
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QtMath>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "motion/pb_puppet.hpp"

using namespace paintbrush::motion;

static constexpr double kPi = 3.14159265358979323846;

static int g_pass = 0, g_fail = 0, g_skip = 0;
static QString g_buildDir;

#define CHECK(name, cond)                                                     \
    do {                                                                      \
        const bool ok = (cond);                                               \
        if (ok) { ++g_pass; std::printf("PASS  %s\n", name); }                \
        else    { ++g_fail; std::printf("FAIL  %s\n", name); }                \
    } while (0)

#define SKIP(name, why)                                                       \
    do { ++g_skip; std::printf("SKIP  %s (%s)\n", name, why); } while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static bool imagesEqual(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int y = 0; y < a.height(); ++y)
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        size_t(a.bytesPerLine())) != 0)
            return false;
    return true;
}

static int diffPixels(const QImage &a, const QImage &b)
{
    int n = 0;
    for (int y = 0; y < qMin(a.height(), b.height()); ++y) {
        const quint8 *pa = a.constScanLine(y);
        const quint8 *pb = b.constScanLine(y);
        for (int x = 0; x < qMin(a.width(), b.width()); ++x) {
            const int o = x * 4;
            if (pa[o] != pb[o] || pa[o + 1] != pb[o + 1]
                || pa[o + 2] != pb[o + 2] || pa[o + 3] != pb[o + 3])
                ++n;
        }
    }
    return n;
}

/* mean absolute RGBA diff over pixels where either image is opaque */
static double meanAbsDiff(const QImage &a, const QImage &b)
{
    double sum = 0.0;
    qint64 n = 0;
    for (int y = 0; y < qMin(a.height(), b.height()); ++y) {
        const quint8 *pa = a.constScanLine(y);
        const quint8 *pb = b.constScanLine(y);
        for (int x = 0; x < qMin(a.width(), b.width()); ++x) {
            const int o = x * 4;
            if (pa[o + 3] > 0 || pb[o + 3] > 0) {
                for (int c = 0; c < 4; ++c)
                    sum += std::abs(int(pa[o + c]) - int(pb[o + c]));
                ++n;
            }
        }
    }
    return n ? sum / (4.0 * n) : 0.0;
}

/* fraction of silhouette pixels equal within tol (per channel) */
static double fracWithin(const QImage &a, const QImage &b, int tol)
{
    qint64 same = 0, total = 0;
    for (int y = 0; y < qMin(a.height(), b.height()); ++y) {
        const quint8 *pa = a.constScanLine(y);
        const quint8 *pb = b.constScanLine(y);
        for (int x = 0; x < qMin(a.width(), b.width()); ++x) {
            const int o = x * 4;
            if (pa[o + 3] > 0 || pb[o + 3] > 0) {
                ++total;
                bool ok = true;
                for (int c = 0; c < 4; ++c)
                    if (std::abs(int(pa[o + c]) - int(pb[o + c])) > tol)
                        ok = false;
                if (ok)
                    ++same;
            }
        }
    }
    return total ? double(same) / total : 1.0;
}

static double silhouetteIoU(const QImage &a, const QImage &b)
{
    qint64 inter = 0, uni = 0;
    for (int y = 0; y < qMin(a.height(), b.height()); ++y) {
        const quint8 *pa = a.constScanLine(y);
        const quint8 *pb = b.constScanLine(y);
        for (int x = 0; x < qMin(a.width(), b.width()); ++x) {
            const bool va = pa[x * 4 + 3] > 0;
            const bool vb = pb[x * 4 + 3] > 0;
            if (va && vb) ++inter;
            if (va || vb) ++uni;
        }
    }
    return uni ? double(inter) / uni : 1.0;
}

static QImage binaryMaskOf(const QImage &rgba)
{
    QImage m(rgba.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < rgba.height(); ++y) {
        const quint8 *row = rgba.constScanLine(y);
        quint8 *mr = m.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
            mr[x] = row[x * 4 + 3] > 127 ? 255 : 0;
    }
    return m;
}

static bool pointInAnyTriangle(const PuppetRig &rig, QPointF p)
{
    for (const auto &t : rig.mesh.tris) {
        const QPointF a = rig.mesh.verts[t[0]], b = rig.mesh.verts[t[1]],
                      c = rig.mesh.verts[t[2]];
        const float d1 = (b.x() - a.x()) * (p.y() - a.y())
                       - (b.y() - a.y()) * (p.x() - a.x());
        const float d2 = (c.x() - b.x()) * (p.y() - b.y())
                       - (c.y() - b.y()) * (p.x() - b.x());
        const float d3 = (a.x() - c.x()) * (p.y() - c.y())
                       - (a.y() - c.y()) * (p.x() - c.x());
        const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        if (!(hasNeg && hasPos))
            return true;
    }
    return false;
}

static double maskCoverage(const PuppetRig &rig)
{
    const QImage &m = rig.mask;
    qint64 inside = 0, total = 0;
    for (int y = 0; y < m.height(); y += 2) {
        const quint8 *row = m.constScanLine(y);
        for (int x = 0; x < m.width(); x += 2) {
            if (row[x] > 127) {
                ++total;
                if (pointInAnyTriangle(rig, QPointF(x + 0.5, y + 0.5)))
                    ++inside;
            }
        }
    }
    return total ? double(inside) / total : 1.0;
}

static double maxVertexMove(const PuppetRig &rig, const QVector<QPointF> &p)
{
    double dmax = 0.0;
    for (int i = 0; i < rig.mesh.verts.size(); ++i) {
        const QPointF d = p[i] - rig.mesh.verts[i];
        dmax = qMax(dmax, hypot(d.x(), d.y()));
    }
    return dmax;
}

static double flippedTris(const PuppetRig &rig, const QVector<QPointF> &p)
{
    /* MATERIAL inversions only: a triangle counts when its deformed signed
     * area is negative AND larger than 1e-3 px^2 — sub-pixel slivers that
     * flip under float noise are invisible and excluded. */
    double flips = 0;
    for (const auto &t : rig.mesh.tris) {
        const double restArea = (rig.mesh.verts[t[1]].x() - rig.mesh.verts[t[0]].x())
                              * (rig.mesh.verts[t[2]].y() - rig.mesh.verts[t[0]].y())
                              - (rig.mesh.verts[t[1]].y() - rig.mesh.verts[t[0]].y())
                              * (rig.mesh.verts[t[2]].x() - rig.mesh.verts[t[0]].x());
        const double defArea = (p[t[1]].x() - p[t[0]].x()) * (p[t[2]].y() - p[t[0]].y())
                             - (p[t[1]].y() - p[t[0]].y()) * (p[t[2]].x() - p[t[0]].x());
        if (defArea < 0 && -defArea > 1e-3 && restArea > 1e-3)
            ++flips;
    }
    return flips;
}

static double areaRatio(const PuppetRig &rig, const QVector<QPointF> &p)
{
    double rest = 0.0, def = 0.0;
    for (const auto &t : rig.mesh.tris) {
        const auto area = [](QPointF a, QPointF b, QPointF c) {
            return (b.x() - a.x()) * (c.y() - a.y())
                 - (b.y() - a.y()) * (c.x() - a.x());
        };
        rest += area(rig.mesh.verts[t[0]], rig.mesh.verts[t[1]], rig.mesh.verts[t[2]]);
        def += area(p[t[0]], p[t[1]], p[t[2]]);
    }
    return rest > 0 ? def / rest : 1.0;
}

static double jointFkError(const PuppetRig &rig, const PuppetPose &pose,
                           const QVector<QPointF> &deformed)
{
    /* The joint rides rigidly with its handle vertex: its estimated
     * position is deformed[v] + (jointRest - vertexRest). */
    const QVector<QPointF> fk = PuppetDeformer::fk(rig, pose);
    double worst = 0.0;
    for (int j = 0; j < rig.handleVerts.size(); ++j) {
        const int v = rig.handleVerts[j];
        if (v < 0)
            continue;
        const QPointF jointPos = deformed[v]
            + (rig.skeleton.joints[j].rest - rig.mesh.verts[v]);
        const QPointF d = jointPos - fk[j];
        worst = qMax(worst, hypot(d.x(), d.y()));
    }
    return worst;
}

static QString saveImg(const QString &name, const QImage &img)
{
    const QString path = QDir(g_buildDir).filePath(name);
    img.save(path);
    return path;
}

/* mesh + skeleton overlay for visual review */
static QImage overlay(const PuppetRig &rig, const QImage &base)
{
    QImage out = base.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPen triPen(QColor(120, 200, 120, 160));
    triPen.setWidthF(0.7);
    p.setPen(triPen);
    for (const auto &t : rig.mesh.tris) {
        p.drawLine(rig.mesh.verts[t[0]], rig.mesh.verts[t[1]]);
        p.drawLine(rig.mesh.verts[t[1]], rig.mesh.verts[t[2]]);
        p.drawLine(rig.mesh.verts[t[2]], rig.mesh.verts[t[0]]);
    }
    QPen bonePen(QColor(255, 60, 60, 230));
    bonePen.setWidthF(1.6);
    p.setPen(bonePen);
    for (int j = 1; j < rig.skeleton.joints.size(); ++j) {
        const int par = rig.skeleton.joints[j].parent;
        if (par >= 0)
            p.drawLine(rig.skeleton.joints[par].rest, rig.skeleton.joints[j].rest);
    }
    p.setBrush(QColor(255, 60, 60, 230));
    p.setPen(Qt::NoPen);
    for (const auto &j : rig.skeleton.joints)
        p.drawEllipse(j.rest, 3.0, 3.0);
    p.end();
    return out;
}

/* contact sheet of N frames side by side */
static QImage sheet(const QVector<QImage> &frames, int pad = 4)
{
    const int w = frames.isEmpty() ? 1 : frames.first().width();
    const int h = frames.isEmpty() ? 1 : frames.first().height();
    QImage out(w * frames.size() + pad * (frames.size() + 1), h + pad * 2,
               QImage::Format_ARGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    for (int i = 0; i < frames.size(); ++i)
        p.drawImage(QPoint(pad + i * (w + pad), pad), frames[i]);
    p.end();
    return out;
}

/* trim frame+mask to the mask bbox, alpha = mask (exactly the harvest path) */
static void trimToMask(const QImage &frame, const QImage &mask,
                       QImage &outCutout, QImage &outMask)
{
    int minX = mask.width(), minY = mask.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const quint8 *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] > 127) {
                minX = qMin(minX, x); maxX = qMax(maxX, x);
                minY = qMin(minY, y); maxY = qMax(maxY, y);
            }
        }
    }
    if (maxX < 0) { outCutout = QImage(); outMask = QImage(); return; }
    const QRect bbox(minX, minY, maxX - minX + 1, maxY - minY + 1);
    const QImage f = frame.copy(bbox).convertToFormat(QImage::Format_ARGB32);
    const QImage m = mask.copy(bbox).convertToFormat(QImage::Format_Grayscale8);
    outCutout = QImage(f.size(), QImage::Format_ARGB32);
    outCutout.fill(Qt::transparent);
    for (int y = 0; y < f.height(); ++y) {
        const quint8 *fr = f.constScanLine(y);
        const quint8 *mr = m.constScanLine(y);
        quint8 *orow = outCutout.scanLine(y);
        for (int x = 0; x < f.width(); ++x) {
            const int o = x * 4;
            orow[o] = fr[o]; orow[o + 1] = fr[o + 1]; orow[o + 2] = fr[o + 2];
            orow[o + 3] = mr[x] > 127 ? 255 : 0;
        }
    }
    outMask = m;
}

/* ------------------------------------------------------------------ */
/* synthetic humanoid figure (deterministic)                           */
/* ------------------------------------------------------------------ */

static QImage makeHumanoid()
{
    QImage img(220, 420, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    /* outfit + skin + hair colors (fixed, deterministic) */
    const QColor skin(240, 196, 160), outfit(40, 90, 170), hair(40, 30, 26);
    /* legs */
    p.setBrush(outfit);
    p.setPen(Qt::NoPen);
    QPolygonF legL, legR;
    legL << QPointF(78, 196) << QPointF(100, 196) << QPointF(97, 392) << QPointF(72, 388);
    legR << QPointF(120, 196) << QPointF(142, 196) << QPointF(148, 388) << QPointF(123, 392);
    p.drawPolygon(legL);
    p.drawPolygon(legR);
    /* arms */
    QPolygonF armL, armR;
    armL << QPointF(60, 74) << QPointF(80, 80) << QPointF(50, 246) << QPointF(34, 238);
    armR << QPointF(140, 80) << QPointF(160, 74) << QPointF(186, 238) << QPointF(170, 246);
    p.setBrush(skin);
    p.drawPolygon(armL);
    p.drawPolygon(armR);
    /* torso with a subtle deterministic texture (tests bilinear sampling) */
    p.setBrush(outfit);
    QPolygonF torso;
    torso << QPointF(64, 74) << QPointF(156, 74) << QPointF(142, 200) << QPointF(78, 200);
    p.drawPolygon(torso);
    for (int y = 78; y < 198; y += 6)
        for (int x = 68; x < 152; x += 6)
            if (((x / 6) + (y / 6)) % 2 == 0)
                p.fillRect(QRect(x, y, 4, 4), QColor(30, 70, 140));
    /* neck + head + hair */
    p.setBrush(skin);
    p.drawRect(QRect(102, 56, 16, 18));
    p.drawEllipse(QPointF(110, 34), 21, 24);
    p.setBrush(hair);
    p.drawEllipse(QPointF(110, 30), 21, 16);
    p.drawRect(QRect(96, 18, 28, 8));
    p.end();
    return img;
}

/* ------------------------------------------------------------------ */
/* sections                                                            */
/* ------------------------------------------------------------------ */

static void runMeshAndSkeleton(const PuppetRig &rig, const QImage &rgba)
{
    std::printf("== mesh + skeleton ==\n");
    CHECK("rig valid", rig.isValid());
    CHECK("vertex budget (8..900)", rig.mesh.verts.size() >= 8
          && rig.mesh.verts.size() <= 900);
    CHECK("triangles present", rig.mesh.tris.size() > 10);
    const double cov = maskCoverage(rig);
    std::printf("      mask coverage: %.4f\n", cov);
    CHECK("mesh covers mask (>= 0.99 sampled)", cov >= 0.99);
    CHECK("rigId stable prefix", rig.rigId.startsWith("rig-"));

    PuppetRig again = PuppetBuilder::build(rgba, rig.mask);
    bool sameMesh = again.isValid()
        && again.mesh.verts.size() == rig.mesh.verts.size()
        && again.mesh.tris.size() == rig.mesh.tris.size()
        && again.rigId == rig.rigId;
    if (sameMesh) {
        for (int i = 0; i < rig.mesh.verts.size() && sameMesh; ++i)
            if (rig.mesh.verts[i] != again.mesh.verts[i])
                sameMesh = false;
        for (int i = 0; i < rig.mesh.tris.size() && sameMesh; ++i)
            if (rig.mesh.tris[i] != again.mesh.tris[i])
                sameMesh = false;
    }
    CHECK("build deterministic (verts/tris/id)", sameMesh);

    CHECK("10 joints", rig.skeleton.joints.size() == 10);
    CHECK("skeleton hierarchy valid", rig.skeleton.isValid());
    bool jointsInside = true;
    for (const auto &j : rig.skeleton.joints) {
        const int x = qRound(j.rest.x()), y = qRound(j.rest.y());
        if (x < 0 || y < 0 || x >= rig.mask.width() || y >= rig.mask.height()
            || rig.mask.constScanLine(y)[x] <= 127)
            jointsInside = false;
    }
    CHECK("all joints inside mask", jointsInside);
    const PuppetJoint &root = rig.skeleton.joints[0];
    const PuppetJoint &chest = rig.skeleton.joints[1];
    const PuppetJoint &neck = rig.skeleton.joints[2];
    const PuppetJoint &head = rig.skeleton.joints[3];
    CHECK("spine order (head < neck < chest < root y)",
          head.rest.y() < neck.rest.y() && neck.rest.y() < chest.rest.y()
          && chest.rest.y() < root.rest.y());
    CHECK("every joint has a handle vertex", !rig.handleVerts.contains(-1));
}

static void runIdentity(const PuppetRig &rig, const QImage &cutout)
{
    std::printf("== identity (rest) ==\n");
    /* The rig's input asset is the MASK-applied cutout (HarvestItem.cutout
     * carries alpha = mask); semi-transparent AA fringe pixels of the raw
     * frame are not part of the asset, so compare against the masked
     * cutout — that is the exact reconstruction target. */
    QImage maskedCutout(cutout.size(), QImage::Format_ARGB32);
    maskedCutout.fill(Qt::transparent);
    for (int y = 0; y < cutout.height(); ++y) {
        const quint8 *sr = cutout.constScanLine(y);
        const quint8 *mr = rig.mask.constScanLine(y);
        quint8 *dr = maskedCutout.scanLine(y);
        for (int x = 0; x < cutout.width(); ++x) {
            const int o = x * 4;
            dr[o] = sr[o]; dr[o + 1] = sr[o + 1]; dr[o + 2] = sr[o + 2];
            dr[o + 3] = mr[x] > 127 ? 255 : 0;
        }
    }
    const PuppetPose rest = PuppetPose::rest(rig.skeleton);
    const QVector<QPointF> p = PuppetDeformer::deform(rig, rest);
    double dmax = 0.0;
    for (int i = 0; i < rig.mesh.verts.size(); ++i) {
        const QPointF d = p[i] - rig.mesh.verts[i];
        dmax = qMax(dmax, hypot(d.x(), d.y()));
    }
    CHECK("deform(rest) == rest verts", dmax <= 1e-3);

    const QImage r0 = PuppetRenderer::render(rig.texture, rig.mask, rig.mesh.verts,
                                             p, rig.mesh.tris);
    const QImage r1 = PuppetRenderer::render(rig.texture, rig.mask, rig.mesh.verts,
                                             p, rig.mesh.tris);
    CHECK("render deterministic (byte-identical)", imagesEqual(r0, r1));
    const double md = meanAbsDiff(r0, maskedCutout);
    const double fw = fracWithin(r0, maskedCutout, 2);
    std::printf("      identity render: mean diff %.3f/255, %.4f within 2/255\n", md, fw);
    CHECK("identity render ~= cutout (mean diff <= 4/255)", md <= 4.0);
    CHECK("identity render ~= cutout (>= 0.985 within 2/255)", fw >= 0.985);
}

static void runPhotoRegime(const PuppetRig &rig)
{
    std::printf("== ARAP photo regime (subtle, ±5-10 deg) ==\n");
    PuppetEffect sway;
    sway.kind = MotionKind::Sway;
    sway.amplitude = 2.0f;          /* chest ±5°, arms ±3.5°, counter head */
    sway.speed = 1.0f;
    sway.loop = LoopMode::Loop;
    sway.seed = 7;
    sway.durationMs = 3200;
    const qint64 t = sway.durationMs / 4;
    const PuppetPose pose = PuppetMotion::poseAt(rig, sway, t);
    const QVector<QPointF> p = PuppetDeformer::deform(rig, pose);
    const double diag = hypot(double(rig.bounds.width()), double(rig.bounds.height()));

    const double move = maxVertexMove(rig, p);
    std::printf("      max vertex move: %.2f px (%.1f%% of bbox diag)\n",
                move, 100.0 * move / diag);
    CHECK("bounded displacement (<= 12% of bbox diag)", move <= 0.12 * diag);
    CHECK("no triangle flips", flippedTris(rig, p) == 0);
    CHECK("area preserved (0.90..1.10)", areaRatio(rig, p) >= 0.90
          && areaRatio(rig, p) <= 1.10);
    CHECK("handles track FK targets (<= 2 px)", jointFkError(rig, pose, p) <= 2.0);

    const QImage restImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                  rig.mesh.verts, rig.mesh.verts,
                                                  rig.mesh.tris);
    const QImage swayImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                  rig.mesh.verts, p, rig.mesh.tris);
    const double iou = silhouetteIoU(restImg, swayImg);
    std::printf("      photo-regime silhouette IoU: %.4f\n", iou);
    CHECK("photo regime stays human (IoU >= 0.90)", iou >= 0.90);
}

static void runLargeRegime(const PuppetRig &rig)
{
    std::printf("== ARAP illustration regime (±20..35 deg) ==\n");
    PuppetPose pose = PuppetPose::rest(rig.skeleton);
    pose.anglesDeg[3] = 15.0f;    /* head */
    pose.anglesDeg[1] = 6.0f;     /* chest */
    pose.anglesDeg[4] = -20.0f;   /* lShoulder */
    pose.anglesDeg[6] = 20.0f;    /* rShoulder */
    pose.anglesDeg[5] = -35.0f;   /* lElbow */
    pose.anglesDeg[7] = 35.0f;    /* rElbow */
    const QVector<QPointF> p = PuppetDeformer::deform(rig, pose);
    const double flips = flippedTris(rig, p);
    const double flipFrac = flips / double(rig.mesh.tris.size());
    std::printf("      flips at ±35 deg: %.0f (%.2f%% of %d tris)\n",
                flips, 100.0 * flipFrac, int(rig.mesh.tris.size()));
    /* The thin transition bands (neck/elbows) invert at large rotations —
     * the measured ARAP degradation boundary. Renderer skips flipped
     * triangles (small holes, never a smear); the product question is the
     * photo regime (0 flips), this is the illustration-regime ceiling. */
    CHECK("flips bounded at ±35 deg (<= 3%)", flipFrac <= 0.03);
    CHECK("area preserved (0.85..1.15)", areaRatio(rig, p) >= 0.85
          && areaRatio(rig, p) <= 1.15);
    const QImage restImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                  rig.mesh.verts, rig.mesh.verts,
                                                  rig.mesh.tris);
    const QImage waveImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                  rig.mesh.verts, p, rig.mesh.tris);
    const double iou = silhouetteIoU(restImg, waveImg);
    std::printf("      large-regime silhouette IoU: %.4f\n", iou);
    CHECK("large regime stays coherent (IoU >= 0.50)", iou >= 0.50);
    saveImg("synth_large_wave.png", waveImg);

    /* degradation probe: push to ±60° and REPORT (no assert) — the rubber
     * demon measurement the spike exists for */
    PuppetPose extreme = PuppetPose::rest(rig.skeleton);
    extreme.anglesDeg[5] = -60.0f;
    extreme.anglesDeg[7] = 60.0f;
    extreme.anglesDeg[1] = 15.0f;
    const QVector<QPointF> pe = PuppetDeformer::deform(rig, extreme);
    const QImage extImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                 rig.mesh.verts, pe, rig.mesh.tris);
    std::printf("      [probe] ±60 deg: flips=%.0f IoU=%.4f area=%.3f\n",
                flippedTris(rig, pe), silhouetteIoU(restImg, extImg), areaRatio(rig, pe));
    saveImg("synth_extreme_60.png", extImg);
}

static void runMotionSemantics(const PuppetRig &rig)
{
    std::printf("== motion semantics ==\n");
    PuppetEffect fx;
    fx.loop = LoopMode::Loop;
    fx.seed = 0;

    /* determinism + seed + amplitude */
    fx.kind = MotionKind::Sway;
    fx.amplitude = 1.0f; fx.durationMs = 3200;
    const QImage a1 = PuppetRuntime::renderFrame(rig, fx, 900);
    const QImage a2 = PuppetRuntime::renderFrame(rig, fx, 900);
    CHECK("renderFrame deterministic", imagesEqual(a1, a2));
    fx.seed = 1;
    const QImage b1 = PuppetRuntime::renderFrame(rig, fx, 900);
    CHECK("seed changes the render", diffPixels(a1, b1) > 32);
    fx.seed = 0; fx.amplitude = 2.0f;
    const QImage c1 = PuppetRuntime::renderFrame(rig, fx, 900);
    CHECK("amplitude changes the render", diffPixels(a1, c1) > 32);

    /* bounce root offset */
    fx.kind = MotionKind::Bounce; fx.amplitude = 1.0f; fx.durationMs = 2600;
    const PuppetPose b0 = PuppetMotion::poseAt(rig, fx, 0);
    const PuppetPose b50 = PuppetMotion::poseAt(rig, fx, fx.durationMs / 2);
    CHECK("bounce starts at rest", std::abs(b0.rootOffset.y()) <= 1e-6);
    CHECK("bounce peaks upward at mid-cycle",
          b50.rootOffset.y() <= -0.005 * rig.bounds.height());

    /* breath never moves the head */
    fx.kind = MotionKind::Breath; fx.durationMs = 3800;
    bool headStill = true;
    double chestPeak = 0.0;
    for (int i = 0; i <= 16; ++i) {
        const PuppetPose bp = PuppetMotion::poseAt(rig, fx, i * fx.durationMs / 16);
        if (std::abs(bp.anglesDeg.value(3, 0.0f)) > 1e-6f)
            headStill = false;
        chestPeak = qMax(chestPeak, double(std::abs(bp.anglesDeg.value(1, 0.0f))));
    }
    CHECK("breath: head stays exactly still", headStill);
    CHECK("breath: chest moves", chestPeak > 0.3);

    /* head tilt isolates head + neck counter-roll (phase-independent) */
    fx.kind = MotionKind::HeadTilt; fx.durationMs = 6000;
    double headPeak = 0.0;
    bool othersStill = true;
    for (int i = 0; i <= 16; ++i) {
        const PuppetPose ht = PuppetMotion::poseAt(rig, fx, i * fx.durationMs / 16);
        headPeak = qMax(headPeak, double(std::abs(ht.anglesDeg.value(3, 0.0f))));
        for (int j = 0; j < rig.skeleton.joints.size(); ++j) {
            if (j == 3 || j == 2)      /* head + its counter neck */
                continue;
            if (std::abs(ht.anglesDeg.value(j, 0.0f)) > 1e-4f)
                othersStill = false;
        }
    }
    CHECK("headTilt: head peaks at ~4 deg (>= 3.9)", headPeak >= 3.9);
    CHECK("headTilt: only head+neck move", othersStill);

    /* amplitude semantics: the generator scales angles and root offset
     * EXACTLY (deterministic linearity of the motion, not of FK cos/sin) */
    fx.kind = MotionKind::Sway; fx.durationMs = 3200; fx.amplitude = 1.0f;
    const PuppetPose s1 = PuppetMotion::poseAt(rig, fx, 731);
    fx.amplitude = 2.0f;
    const PuppetPose s2 = PuppetMotion::poseAt(rig, fx, 731);
    bool scaled = s1.anglesDeg.size() == s2.anglesDeg.size();
    for (int j = 0; j < s1.anglesDeg.size() && scaled; ++j)
        if (std::abs(s2.anglesDeg[j] - 2.0f * s1.anglesDeg[j]) > 1e-5f)
            scaled = false;
    scaled = scaled
        && std::abs(s2.rootOffset.x() - 2.0 * s1.rootOffset.x()) <= 1e-5
        && std::abs(s2.rootOffset.y() - 2.0 * s1.rootOffset.y()) <= 1e-5;
    CHECK("amplitude scales exactly (angles + root offset x2)", scaled);
    /* and the FK head displacement grows with amplitude (monotone) */
    const QVector<QPointF> fk1 = PuppetDeformer::fk(rig, s1);
    const QVector<QPointF> fk2 = PuppetDeformer::fk(rig, s2);
    const double hd1 = std::abs(fk1[3].x() - fk1[0].x());
    const double hd2 = std::abs(fk2[3].x() - fk2[0].x());
    CHECK("bigger amplitude moves the head more", hd2 > hd1);

    /* speed scaling */
    fx.amplitude = 1.0f; fx.speed = 2.0f;
    const PuppetPose fast = PuppetMotion::poseAt(rig, fx, 400);
    fx.speed = 1.0f;
    const PuppetPose slow = PuppetMotion::poseAt(rig, fx, 800);
    bool same = fast.anglesDeg.size() == slow.anglesDeg.size()
        && std::abs(fast.rootOffset.x() - slow.rootOffset.x()) <= 1e-5
        && std::abs(fast.rootOffset.y() - slow.rootOffset.y()) <= 1e-5;
    for (int j = 0; j < fast.anglesDeg.size() && same; ++j)
        if (std::abs(fast.anglesDeg[j] - slow.anglesDeg[j]) > 1e-4f)
            same = false;
    CHECK("speed scales time (pose(t,2x) == pose(2t,1x))", same);
}

static void runLooping(const PuppetRig &rig)
{
    std::printf("== loop seamlessness ==\n");
    const MotionKind kinds[] = { MotionKind::Sway, MotionKind::Bounce,
                                 MotionKind::Breath, MotionKind::HeadTilt,
                                 MotionKind::Composite };
    const quint32 durs[] = { 3200, 2600, 3800, 6000, 4200 };
    for (int k = 0; k < 5; ++k) {
        PuppetEffect fx;
        fx.kind = kinds[k];
        fx.loop = LoopMode::Loop;
        fx.seed = 3;
        fx.durationMs = durs[k];
        const PuppetPose p0 = PuppetMotion::poseAt(rig, fx, 0);
        const PuppetPose pT = PuppetMotion::poseAt(rig, fx, qint64(durs[k]));
        bool same = p0.anglesDeg.size() == pT.anglesDeg.size()
            && std::abs(p0.rootOffset.x() - pT.rootOffset.x()) <= 1e-9
            && std::abs(p0.rootOffset.y() - pT.rootOffset.y()) <= 1e-9;
        for (int j = 0; j < p0.anglesDeg.size() && same; ++j)
            if (p0.anglesDeg[j] != pT.anglesDeg[j])
                same = false;
        const QImage r0 = PuppetRuntime::renderFrame(rig, fx, 0);
        const QImage rT = PuppetRuntime::renderFrame(rig, fx, qint64(durs[k]));
        std::printf("      %s: pose equal=%d render byte-identical=%d\n",
                    k == 0 ? "Sway" : k == 1 ? "Bounce" : k == 2 ? "Breath"
                        : k == 3 ? "HeadTilt" : "Composite",
                    int(same), int(imagesEqual(r0, rT)));
        CHECK(QString("loop seamless (%1)").arg(k == 0 ? "Sway" : k == 1 ? "Bounce"
                  : k == 2 ? "Breath" : k == 3 ? "HeadTilt" : "Composite")
                  .toUtf8().constData(),
              same && imagesEqual(r0, rT));
    }

    /* Once: holds rest after one cycle */
    PuppetEffect once;
    once.kind = MotionKind::Sway;
    once.loop = LoopMode::Once;
    once.seed = 3;
    once.durationMs = 3200;
    const PuppetPose hold = PuppetMotion::poseAt(rig, once, 2 * once.durationMs);
    CHECK("Once holds rest after the cycle", hold.isRest());

    /* PingPong: symmetric retrace */
    PuppetEffect pp;
    pp.kind = MotionKind::Sway;
    pp.loop = LoopMode::PingPong;
    pp.seed = 3;
    pp.durationMs = 3200;
    const PuppetPose pa = PuppetMotion::poseAt(rig, pp, pp.durationMs / 4);
    const PuppetPose pb = PuppetMotion::poseAt(rig, pp, 7 * pp.durationMs / 4);
    bool ppSame = true;
    for (int j = 0; j < pa.anglesDeg.size() && ppSame; ++j)
        if (std::abs(pa.anglesDeg[j] - pb.anglesDeg[j]) > 1e-4f)
            ppSame = false;
    CHECK("PingPong retraces (t == T+t' mirror)", ppSame);
}

static void runSerialization()
{
    std::printf("== PuppetEffect serialization ==\n");
    PuppetEffect fx;
    fx.kind = MotionKind::Composite;
    fx.amplitude = 1.25f;
    fx.speed = 1.5f;
    fx.loop = LoopMode::PingPong;
    fx.seed = 0xDEADBEEFu;
    fx.durationMs = 4200;
    const QByteArray bytes = fx.serialize();
    const QByteArray bytes2 = fx.serialize();
    CHECK("serialize deterministic", bytes == bytes2);
    const PuppetEffect back = PuppetEffect::deserialize(bytes);
    CHECK("round-trip byte-exact", back == fx && back.serialize() == bytes);
    CHECK("garbage -> default", PuppetEffect::deserialize(QByteArray("x", 1))
          == PuppetEffect());
    CHECK("truncated -> default", PuppetEffect::deserialize(bytes.left(10))
          == PuppetEffect());
}

static void runRigId(const PuppetRig &rigA, const PuppetRig &rigB)
{
    std::printf("== rig identity ==\n");
    CHECK("rigId unique across inputs", rigA.rigId != rigB.rigId);
    const PuppetRig rebuild = PuppetRuntime::build(rigA.texture, rigA.mask);
    CHECK("rigId stable across rebuilds", rebuild.rigId == rigA.rigId);
}

static void runEdgeCases()
{
    std::printf("== edge cases ==\n");
    /* empty mask */
    QImage empty(80, 120, QImage::Format_Grayscale8);
    empty.fill(0);
    QImage blank(80, 120, QImage::Format_ARGB32);
    blank.fill(Qt::transparent);
    const PuppetRig bad = PuppetBuilder::build(blank, empty);
    CHECK("empty mask -> invalid rig", !bad.isValid());

    /* 12x12 blob */
    QImage blob(12, 12, QImage::Format_ARGB32);
    blob.fill(Qt::transparent);
    {
        QPainter p(&blob);
        p.setBrush(QColor(200, 60, 60));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(6, 6), 5.0, 5.0);
        p.end();
    }
    const PuppetRig tiny = PuppetBuilder::build(blob, QImage());
    CHECK("12x12 blob -> valid rig", tiny.isValid());
    if (tiny.isValid()) {
        /* compare against the mask-derived cutout, not the AA source */
        QImage blobMasked(blob.size(), QImage::Format_ARGB32);
        blobMasked.fill(Qt::transparent);
        const QImage tinyMask = binaryMaskOf(blob);
        for (int y = 0; y < blob.height(); ++y) {
            const quint8 *sr = blob.constScanLine(y);
            const quint8 *mr = tinyMask.constScanLine(y);
            quint8 *dr = blobMasked.scanLine(y);
            for (int x = 0; x < blob.width(); ++x) {
                const int o = x * 4;
                dr[o] = sr[o]; dr[o + 1] = sr[o + 1]; dr[o + 2] = sr[o + 2];
                dr[o + 3] = mr[x] > 127 ? 255 : 0;
            }
        }
        const QImage tinyImg = PuppetRenderer::render(tiny.texture, tiny.mask,
                                                      tiny.mesh.verts, tiny.mesh.verts,
                                                      tiny.mesh.tris);
        const double tinyIou = silhouetteIoU(tinyImg, blobMasked);
        std::printf("      12x12 blob IoU (vs masked cutout): %.4f\n", tinyIou);
        CHECK("12x12 blob renders (IoU >= 0.9)", tinyIou >= 0.9);
        saveImg("edge_12.png", tinyImg);
    }

    /* wide flat mask */
    QImage wide(200, 24, QImage::Format_ARGB32);
    wide.fill(Qt::transparent);
    {
        QPainter p(&wide);
        p.setBrush(QColor(90, 160, 90));
        p.setPen(Qt::NoPen);
        p.drawRect(QRect(8, 4, 184, 16));
        p.end();
    }
    const PuppetRig wideRig = PuppetBuilder::build(wide, QImage());
    CHECK("wide flat mask -> valid rig", wideRig.isValid());
    if (wideRig.isValid()) {
        const QImage wideImg = PuppetRenderer::render(wideRig.texture, wideRig.mask,
                                                      wideRig.mesh.verts, wideRig.mesh.verts,
                                                      wideRig.mesh.tris);
        CHECK("wide flat renders (IoU >= 0.9)", silhouetteIoU(wideImg, wide) >= 0.9);
        saveImg("edge_wide.png", wideImg);
    }

    /* mask with a hole: mesh covers the FILLED shape, hole stays transparent */
    QImage ring(120, 120, QImage::Format_ARGB32);
    ring.fill(Qt::transparent);
    {
        QPainter p(&ring);
        p.setBrush(QColor(230, 180, 60));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(60, 60), 48, 48);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.drawEllipse(QPointF(60, 60), 22, 22);
        p.end();
    }
    const PuppetRig ringRig = PuppetBuilder::build(ring, QImage());
    CHECK("ring mask -> valid rig", ringRig.isValid());
    if (ringRig.isValid()) {
        const QImage ringImg = PuppetRenderer::render(ringRig.texture, ringRig.mask,
                                                      ringRig.mesh.verts, ringRig.mesh.verts,
                                                      ringRig.mesh.tris);
        CHECK("ring renders (IoU >= 0.9)", silhouetteIoU(ringImg, ring) >= 0.9);
        saveImg("edge_ring.png", ringImg);
    }
}

static void runLivePhoto(const QDir &liveDir)
{
    std::printf("== real harvested photo (SAM3 cutout) ==\n");
    const QString framePath = liveDir.filePath("live_frame_A.png");
    const QString maskPath = liveDir.filePath("live_mask_A.png");
    if (!QFile::exists(framePath) || !QFile::exists(maskPath)) {
        SKIP("real photo checks", "live harness artifacts not found");
        return;
    }
    QImage frame, mask;
    CHECK("load live frame", frame.load(framePath));
    CHECK("load live mask", mask.load(maskPath));
    QImage cutout, cutMask;
    trimToMask(frame, mask, cutout, cutMask);
    CHECK("live cutout non-empty", !cutout.isNull());

    PuppetRig rig = PuppetRuntime::build(cutout, cutMask);
    CHECK("live rig valid", rig.isValid());
    if (!rig.isValid())
        return;
    std::printf("      rig: %d verts, %d tris, %d joints, bounds %dx%d\n",
                int(rig.mesh.verts.size()), int(rig.mesh.tris.size()),
                int(rig.skeleton.joints.size()), rig.bounds.width(), rig.bounds.height());
    const double liveCov = maskCoverage(rig);
    std::printf("      live mesh coverage: %.4f\n", liveCov);
    CHECK("live mesh covers mask (>= 0.97)", liveCov >= 0.97);
    int inside = 0;
    for (const auto &j : rig.skeleton.joints) {
        const int x = qRound(j.rest.x()), y = qRound(j.rest.y());
        if (x >= 0 && y >= 0 && x < rig.mask.width() && y < rig.mask.height()
            && rig.mask.constScanLine(y)[x] > 127)
            ++inside;
    }
    CHECK("live joints inside mask (>= 9/10)", inside >= 9);
    CHECK("live handles mapped", !rig.handleVerts.contains(-1));

    /* identity (against the rig's cleaned mask — the mesh's alpha domain) */
    const QImage restImg = PuppetRuntime::renderFrame(rig, PuppetEffect(), 0);
    QImage liveTarget(cutout.size(), QImage::Format_ARGB32);
    liveTarget.fill(Qt::transparent);
    for (int y = 0; y < cutout.height(); ++y) {
        const quint8 *sr = cutout.constScanLine(y);
        const quint8 *mr = rig.mask.constScanLine(y);
        quint8 *dr = liveTarget.scanLine(y);
        for (int x = 0; x < cutout.width(); ++x) {
            const int o = x * 4;
            dr[o] = sr[o]; dr[o + 1] = sr[o + 1]; dr[o + 2] = sr[o + 2];
            dr[o + 3] = mr[x] > 127 ? 255 : 0;
        }
    }
    const double liveMd = meanAbsDiff(restImg, liveTarget);
    const double liveFw = fracWithin(restImg, liveTarget, 3);
    std::printf("      live identity: mean diff %.3f/255, %.4f within 3/255\n",
                liveMd, liveFw);
    CHECK("live identity ~= cutout (mean diff <= 5/255)", liveMd <= 5.0);
    CHECK("live identity (>= 0.97 within 3/255)", liveFw >= 0.97);

    /* photo regime on the real photo */
    PuppetEffect sway;
    sway.kind = MotionKind::Sway;
    sway.amplitude = 2.0f;
    sway.durationMs = 3200;
    sway.seed = 7;
    const PuppetPose pose = PuppetMotion::poseAt(rig, sway, sway.durationMs / 4);
    const QVector<QPointF> p = PuppetDeformer::deform(rig, pose);
    const double diag = hypot(double(rig.bounds.width()), double(rig.bounds.height()));
    const double move = maxVertexMove(rig, p);
    std::printf("      live max vertex move: %.2f px (%.1f%%)\n",
                move, 100.0 * move / diag);
    CHECK("live bounded displacement (<= 12%)", move <= 0.12 * diag);
    CHECK("live no flips", flippedTris(rig, p) == 0);
    CHECK("live handles track FK (<= 2 px)", jointFkError(rig, pose, p) <= 2.0);
    const QImage swayImg = PuppetRenderer::render(rig.texture, rig.mask,
                                                  rig.mesh.verts, p, rig.mesh.tris);
    const double iou = silhouetteIoU(restImg, swayImg);
    std::printf("      live photo-regime IoU: %.4f\n", iou);
    CHECK("live photo regime stays human (IoU >= 0.88)", iou >= 0.88);

    /* proof artifacts */
    PuppetEffect bounce;
    bounce.kind = MotionKind::Bounce;
    bounce.amplitude = 1.5f;
    bounce.durationMs = 2600;
    bounce.seed = 3;
    PuppetEffect comp;
    comp.kind = MotionKind::Composite;
    comp.amplitude = 1.0f;
    comp.durationMs = 4200;
    comp.seed = 5;
    saveImg("photo_rest.png", restImg);
    saveImg("photo_sway.png", swayImg);
    saveImg("photo_bounce.png", PuppetRuntime::renderFrame(rig, bounce, 650));
    saveImg("photo_overlay.png", overlay(rig, cutout));
    QVector<QImage> compFrames;
    for (int i = 0; i < 4; ++i)
        compFrames.append(PuppetRuntime::renderFrame(rig, comp, i * comp.durationMs / 4));
    saveImg("photo_composite_sheet.png", sheet(compFrames));

    /* loop on the real rig too */
    const QImage l0 = PuppetRuntime::renderFrame(rig, sway, 0);
    const QImage lT = PuppetRuntime::renderFrame(rig, sway, qint64(sway.durationMs));
    CHECK("live loop seamless (byte-identical)", imagesEqual(l0, lT));
}


/* ------------------------------------------------------------------ */
/* motion retargeting                                                  */
/* ------------------------------------------------------------------ */

/* A DIFFERENT-proportioned figure (chunky: wide torso, short legs, big
 * head) with a waving left arm and a bounce — the "source video". The
 * torso is narrow enough that the waved arm separates from the silhouette,
 * so the mask fit actually sees the limb. */
static QImage makeChunkyFigure(int waveDeg, int bouncePx)
{
    QImage img(140, 260, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const int dy = bouncePx;
    const QColor skin(196, 150, 120), shirt(200, 90, 60), pants(60, 60, 90);
    p.setBrush(pants);
    p.setPen(Qt::NoPen);
    p.drawRect(QRect(52, 168 + dy, 18, 80));
    p.drawRect(QRect(72, 168 + dy, 18, 80));
    p.setBrush(shirt);
    QPolygonF torso;
    torso << QPointF(46, 70 + dy) << QPointF(94, 70 + dy)
          << QPointF(88, 172 + dy) << QPointF(52, 172 + dy);
    p.drawPolygon(torso);
    p.setBrush(skin);
    p.drawEllipse(QPointF(70, 34 + dy), 20, 22);
    const QPointF shoulder(48, 78 + dy);
    const double rad = qDegreesToRadians(double(waveDeg));
    /* waveDeg < 0 swings LEFT (sin of a negative angle is negative) */
    const QPointF hand = shoulder + QPointF(std::sin(rad) * 70, std::cos(rad) * 70);
    QPen armPen(skin, 10, Qt::SolidLine, Qt::RoundCap);
    p.setPen(armPen);
    p.drawLine(shoulder, hand);
    const QPointF shR(96, 78 + dy);
    p.drawLine(shR, shR + QPointF(0, 70));
    p.end();
    return img;
}

static void runRetargeting(const PuppetRig &targetRig)
{
    std::printf("== motion retargeting (synthetic source video) ==\n");

    /* 8-frame "video" of the chunky figure: arm wave (LEFT, out of the torso
 * silhouette so the mask fit sees it) + bounce */
    const int N = 8;
    QVector<QImage> frames;
    QVector<QImage> masks;
    for (int i = 0; i < N; ++i) {
        const int wave = -(30 + i * 4);                  /* -30..-58 deg */
        const int bounce = int(3.0 * std::sin(2.0 * kPi * i / N));
        QImage f = makeChunkyFigure(wave, bounce);
        frames.append(f);
        masks.append(binaryMaskOf(f));
    }
    PuppetRetargeter::PoseTrackResult track =
        PuppetRetargeter::poseTrackFromMasks(masks, 120, true);
    CHECK("pose track from masks (source skeleton + closed clip)",
          track.isValid() && track.clip.keys.size() == N + 1
          && track.clip.keys.first().timeMs == 0
          && track.clip.keys.last().timeMs == 120 * N);

    /* inverse-FK roundtrip on the source skeleton */
    double roundtripWorst = 0.0;
    bool roundtripOk = true;
    for (int i = 0; i < N && roundtripOk; ++i) {
        PuppetSkeleton fit = PuppetBuilder::fitDefaultHumanoid(masks[i]);
        const QVector<QPointF> fkPos =
            PuppetDeformer::fk(track.sourceSkeleton, track.clip.keys[i].pose);
        for (int j = 0; j < fit.joints.size(); ++j) {
            const QPointF d = fkPos[j] - fit.joints[j].rest;
            roundtripWorst = qMax(roundtripWorst, hypot(d.x(), d.y()));
        }
    }
    std::printf("      inverse-FK roundtrip worst: %.2f px\n", roundtripWorst);
    CHECK("inverse FK roundtrips the source fits (<= 8 px)", roundtripWorst <= 8.0);

    /* keyframe sampling + interpolation */
    bool keysExact = true;
    for (const auto &k : track.clip.keys) {
        const PuppetPose p = track.clip.poseAt(track.sourceSkeleton, k.timeMs);
        if (p.anglesDeg != k.pose.anglesDeg
            || p.rootOffset != k.pose.rootOffset)
            keysExact = false;
    }
    CHECK("poseAt(key time) == key pose exactly", keysExact);
    const PuppetPose mid = track.clip.poseAt(track.sourceSkeleton, 60);
    const PuppetPose &a = track.clip.keys[0].pose;
    const PuppetPose &b = track.clip.keys[1].pose;
    bool midAvg = true;
    for (int j = 0; j < a.anglesDeg.size(); ++j)
        if (std::abs(mid.anglesDeg[j] - (a.anglesDeg[j] + b.anglesDeg[j]) * 0.5f) > 1e-4f)
            midAvg = false;
    CHECK("interpolation is linear (mid == average)", midAvg);
    const PuppetPose l0 = track.clip.poseAt(track.sourceSkeleton, 0);
    const PuppetPose lT = track.clip.poseAt(track.sourceSkeleton, track.clip.durationMs());
    CHECK("closed loop seamless (pose(0) == pose(T) exactly)",
          l0.anglesDeg == lT.anglesDeg && l0.rootOffset == lT.rootOffset);

    /* retarget mapping + clamp + root scale (single poses) */
    const PuppetSkeleton &target = targetRig.skeleton;
    PuppetPose single = PuppetPose::rest(track.sourceSkeleton);
    single.anglesDeg[5] = 10.0f;                 /* lElbow */
    single.anglesDeg[6] = 8.0f;                  /* rShoulder */
    single.rootOffset = QPointF(0, -5);
    const PuppetPose mapped = PuppetRetargeter::retarget(
        track.sourceSkeleton, target, single, PuppetRetargeter::Options());
    CHECK("retarget maps named joints (lElbow 10, rShoulder 8)",
          std::abs(mapped.anglesDeg[5] - 10.0f) <= 1e-3f
          && std::abs(mapped.anglesDeg[6] - 8.0f) <= 1e-3f);
    CHECK("retarget leaves unmapped joints at rest",
          mapped.anglesDeg[3] == 0.0f && mapped.anglesDeg[8] == 0.0f);
    /* root offset scales by the target/source height ratio */
    const QPointF r0 = PuppetRetargeter::retarget(
        track.sourceSkeleton, target, single, PuppetRetargeter::Options()).rootOffset;
    double srcHeight = 0, tgtHeight = 0;
    for (const auto &j : track.sourceSkeleton.joints)
        if (j.parent >= 0)
            srcHeight += hypot((track.sourceSkeleton.joints[j.parent].rest - j.rest).x(),
                               (track.sourceSkeleton.joints[j.parent].rest - j.rest).y());
    for (const auto &j : target.joints)
        if (j.parent >= 0)
            tgtHeight += hypot((target.joints[j.parent].rest - j.rest).x(),
                               (target.joints[j.parent].rest - j.rest).y());
    const double expectY = -5.0 * (tgtHeight / srcHeight);
    CHECK("retarget scales root offset by height ratio",
          std::abs(r0.y() - expectY) <= 1e-3 && std::abs(r0.x()) <= 1e-3);

    PuppetPose big = PuppetPose::rest(track.sourceSkeleton);
    big.anglesDeg[5] = 40.0f;
    const PuppetPose clamped = PuppetRetargeter::retarget(
        track.sourceSkeleton, target, big, PuppetRetargeter::Options());
    CHECK("per-pose clamp (40 deg -> 12 deg)", std::abs(clamped.anglesDeg[5]) <= 12.0f);

    /* clip-level photo-regime adaptation: peak 40 -> 12, small stays */
    PuppetMotionClip smallClip;
    smallClip.keys.append({ 0, PuppetPose::rest(track.sourceSkeleton) });
    PuppetPose smallPose = PuppetPose::rest(track.sourceSkeleton);
    smallPose.anglesDeg[5] = 6.0f;
    smallClip.keys.append({ 120, smallPose });
    smallClip.loop = LoopMode::Loop;
    const PuppetMotionClip smallRet = PuppetRetargeter::retargetClip(
        track.sourceSkeleton, target, smallClip, PuppetRetargeter::Options());
    CHECK("small source motion passes through (6 deg stays)",
          std::abs(smallRet.keys[1].pose.anglesDeg[5] - 6.0f) <= 1e-3f);
    const PuppetMotionClip waveRet = PuppetRetargeter::retargetClip(
        track.sourceSkeleton, target, track.clip, PuppetRetargeter::Options());
    float wavePeak = 0.0f;
    for (const auto &k : waveRet.keys)
        wavePeak = qMax(wavePeak, std::abs(k.pose.anglesDeg[5]));
    std::printf("      retargeted wave peak: %.2f deg\n", wavePeak);
    CHECK("photo-regime adaptation caps the wave (<= 12 deg)", wavePeak <= 12.001f);

    /* clip serialization */
    const QByteArray clipBytes = track.clip.serialize();
    const PuppetMotionClip clipBack = PuppetMotionClip::deserialize(clipBytes);
    CHECK("clip serialization round-trip byte-exact",
          clipBack.serialize() == clipBytes && clipBack.keys.size() == track.clip.keys.size());
    CHECK("clip deserialize garbage -> invalid", PuppetMotionClip::deserialize(QByteArray("x", 1)).keys.isEmpty());

    /* end-to-end: retargeted wave drives the TARGET rig */
    PuppetEffect fx;
    fx.kind = MotionKind::Custom;
    fx.amplitude = 1.0f;
    fx.speed = 1.0f;
    const qint64 dur = waveRet.durationMs();
    const QImage restImg = PuppetRuntime::renderFrame(targetRig, PuppetEffect(), 0);
    double worstMove = 0.0;
    double worstFlip = 0.0;
    double worstIou = 1.0;
    QVector<QImage> sheetFrames;
    for (int i = 0; i < 5; ++i) {
        const qint64 t = i * dur / 4;
        const PuppetPose pose = waveRet.poseAt(target, t).scaled(1.0f);
        const QVector<QPointF> p = PuppetDeformer::deform(targetRig, pose);
        const double diag = hypot(double(targetRig.bounds.width()),
                                  double(targetRig.bounds.height()));
        worstMove = qMax(worstMove, maxVertexMove(targetRig, p) / diag);
        worstFlip = qMax(worstFlip, flippedTris(targetRig, p));
        const QImage img = PuppetRenderer::render(targetRig.texture, targetRig.mask,
                                                  targetRig.mesh.verts, p,
                                                  targetRig.mesh.tris);
        worstIou = qMin(worstIou, silhouetteIoU(restImg, img));
        sheetFrames.append(img);
    }
    std::printf("      retargeted: worst move %.3f%% diag, flips %.0f, IoU %.4f\n",
                100.0 * worstMove, worstFlip, worstIou);
    CHECK("retargeted wave bounded (<= 12% diag)", worstMove <= 0.12);
    CHECK("retargeted wave no material flips (<= 3%)",
          worstFlip <= 0.03 * targetRig.mesh.tris.size());
    CHECK("retargeted wave stays human (IoU >= 0.90)", worstIou >= 0.90);
    const QImage rt0 = PuppetRuntime::renderClipFrame(targetRig, waveRet, fx, 0);
    const QImage rtT = PuppetRuntime::renderClipFrame(targetRig, waveRet, fx, dur);
    CHECK("retargeted clip loops byte-identical", imagesEqual(rt0, rtT));
    const QImage rtA = PuppetRuntime::renderClipFrame(targetRig, waveRet, fx, 240);
    const QImage rtB = PuppetRuntime::renderClipFrame(targetRig, waveRet, fx, 240);
    CHECK("retargeted clip deterministic", imagesEqual(rtA, rtB));
    saveImg("retarget_synth_sheet.png", sheet(sheetFrames));

    /* overlay: source fits + the waving source frame */
    QImage srcOverlay = frames[4].convertToFormat(QImage::Format_ARGB32);
    {
        PuppetSkeleton fit = PuppetBuilder::fitDefaultHumanoid(masks[4]);
        QPainter p(&srcOverlay);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(255, 60, 60, 230));
        pen.setWidthF(1.6);
        p.setPen(pen);
        for (int j = 1; j < fit.joints.size(); ++j) {
            const int par = fit.joints[j].parent;
            if (par >= 0)
                p.drawLine(fit.joints[par].rest, fit.joints[j].rest);
        }
        p.setBrush(QColor(255, 60, 60, 230));
        p.setPen(Qt::NoPen);
        for (const auto &j : fit.joints)
            p.drawEllipse(j.rest, 2.5, 2.5);
        p.end();
    }
    saveImg("retarget_synth_source.png", srcOverlay);
}

static void runLiveRetarget(const PuppetRig &targetRig, const QDir &liveDir)
{
    std::printf("== motion retargeting (REAL video mask trajectory) ==\n");
    QImage mA, mB, mC;
    if (!mA.load(liveDir.filePath("live_mask_A.png"))
        || !mB.load(liveDir.filePath("live_mask_B.png"))
        || !mC.load(liveDir.filePath("live_mask_C.png"))) {
        SKIP("live retarget", "live masks not found");
        return;
    }
    QVector<QImage> masks = { mA, mB, mC };
    PuppetRetargeter::PoseTrackResult track =
        PuppetRetargeter::poseTrackFromMasks(masks, 1000, true);
    CHECK("live pose track built", track.isValid() && track.clip.keys.size() == 4);
    if (!track.isValid())
        return;

    /* report the raw source motion magnitude before adaptation */
    float rawPeak = 0.0f;
    for (const auto &k : track.clip.keys)
        for (float a : k.pose.anglesDeg)
            rawPeak = qMax(rawPeak, std::abs(a));
    const PuppetMotionClip liveRet = PuppetRetargeter::retargetClip(
        track.sourceSkeleton, targetRig.skeleton, track.clip,
        PuppetRetargeter::Options());
    float retPeak = 0.0f;
    for (const auto &k : liveRet.keys)
        for (float a : k.pose.anglesDeg)
            retPeak = qMax(retPeak, std::abs(a));
    std::printf("      live source peak: %.1f deg -> retargeted peak: %.2f deg\n",
                rawPeak, retPeak);
    CHECK("live retarget within photo regime (<= 12 deg)", retPeak <= 12.001f);

    /* end-to-end on the synthetic target rig */
    const qint64 dur = liveRet.durationMs();
    const QImage restImg = PuppetRuntime::renderFrame(targetRig, PuppetEffect(), 0);
    double worstMove = 0.0, worstFlip = 0.0, worstIou = 1.0;
    QVector<QImage> sheetFrames;
    for (int i = 0; i < 4; ++i) {
        const qint64 t = i * dur / 4;
        const PuppetPose pose = liveRet.poseAt(targetRig.skeleton, t);
        const QVector<QPointF> p = PuppetDeformer::deform(targetRig, pose);
        const double diag = hypot(double(targetRig.bounds.width()),
                                  double(targetRig.bounds.height()));
        worstMove = qMax(worstMove, maxVertexMove(targetRig, p) / diag);
        worstFlip = qMax(worstFlip, flippedTris(targetRig, p));
        const QImage img = PuppetRenderer::render(targetRig.texture, targetRig.mask,
                                                  targetRig.mesh.verts, p,
                                                  targetRig.mesh.tris);
        worstIou = qMin(worstIou, silhouetteIoU(restImg, img));
        sheetFrames.append(img);
    }
    std::printf("      live retargeted: worst move %.3f%% diag, flips %.0f, IoU %.4f\n",
                100.0 * worstMove, worstFlip, worstIou);
    CHECK("live retarget bounded (<= 12% diag)", worstMove <= 0.12);
    CHECK("live retarget no material flips (<= 3%)",
          worstFlip <= 0.03 * targetRig.mesh.tris.size());
    CHECK("live retarget stays human (IoU >= 0.85)", worstIou >= 0.85);
    saveImg("retarget_live_sheet.png", sheet(sheetFrames));

    /* source overlay: the three live fits */
    int w = mA.width(), h = mA.height();
    QImage bigOverlay(w * 3, h, QImage::Format_ARGB32);
    bigOverlay.fill(Qt::black);
    for (int i = 0; i < 3; ++i) {
        QImage f;
        if (i == 0) f = mA; else if (i == 1) f = mB; else f = mC;
        QPainter p(&bigOverlay);
        p.drawImage(QPoint(i * w, 0), f);
        PuppetSkeleton fit = PuppetBuilder::fitDefaultHumanoid(f);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(255, 80, 80, 255));
        pen.setWidthF(1.2);
        p.setPen(pen);
        for (int j = 1; j < fit.joints.size(); ++j) {
            const int par = fit.joints[j].parent;
            if (par >= 0)
                p.drawLine(QPointF(fit.joints[par].rest.x() + i * w, fit.joints[par].rest.y()),
                           QPointF(fit.joints[j].rest.x() + i * w, fit.joints[j].rest.y()));
        }
        p.end();
    }
    saveImg("retarget_live_source.png", bigOverlay);
}
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    g_buildDir = QDir::current().absolutePath();
    std::printf("== NAI Paintbrush — Motion Toys: PuppetEffect spike harness ==\n");

    /* ---------- synthetic figure ---------- */
    const QImage rgba = makeHumanoid();
    const QImage mask = binaryMaskOf(rgba);
    PuppetRig rig = PuppetRuntime::build(rgba, mask);
    std::printf("      synthetic rig: %d verts, %d tris, %d joints\n",
                int(rig.mesh.verts.size()), int(rig.mesh.tris.size()),
                int(rig.skeleton.joints.size()));

    runMeshAndSkeleton(rig, rgba);
    runIdentity(rig, rgba);
    runPhotoRegime(rig);
    runLargeRegime(rig);
    runMotionSemantics(rig);
    runLooping(rig);
    runSerialization();
    runRigId(rig, PuppetRuntime::build(makeHumanoid(),
                                       binaryMaskOf(makeHumanoid())
                                           .flipped(Qt::Horizontal)));
    runEdgeCases();

    /* proof artifacts (synthetic) */
    saveImg("synth_rest.png", PuppetRuntime::renderFrame(rig, PuppetEffect(), 0));
    PuppetEffect sway;
    sway.kind = MotionKind::Sway; sway.amplitude = 2.0f; sway.seed = 7;
    sway.durationMs = 3200;
    saveImg("synth_sway_t25.png", PuppetRuntime::renderFrame(rig, sway, 800));
    PuppetEffect bounce;
    bounce.kind = MotionKind::Bounce; bounce.amplitude = 1.5f; bounce.seed = 3;
    bounce.durationMs = 2600;
    saveImg("synth_bounce_t50.png", PuppetRuntime::renderFrame(rig, bounce, 1300));
    PuppetEffect breath;
    breath.kind = MotionKind::Breath; breath.durationMs = 3800; breath.seed = 2;
    saveImg("synth_breath_t25.png", PuppetRuntime::renderFrame(rig, breath, 950));
    PuppetEffect tilt;
    tilt.kind = MotionKind::HeadTilt; tilt.durationMs = 6000; tilt.seed = 4;
    saveImg("synth_headtilt_t25.png", PuppetRuntime::renderFrame(rig, tilt, 1500));
    PuppetEffect comp;
    comp.kind = MotionKind::Composite; comp.amplitude = 1.0f; comp.seed = 5;
    comp.durationMs = 4200;
    QVector<QImage> compFrames;
    for (int i = 0; i < 4; ++i)
        compFrames.append(PuppetRuntime::renderFrame(rig, comp, i * comp.durationMs / 4));
    saveImg("synth_composite_sheet.png", sheet(compFrames));
    saveImg("synth_overlay.png", overlay(rig, rgba));

    /* ---------- real harvested photo (live harness artifacts) ---------- */
    QDir liveDir(g_buildDir);
    if (!liveDir.cd("../../pb_live_harness/build/live"))
        liveDir = QDir("../../../pb_live_harness/build/live");
    runLivePhoto(liveDir);

    /* ---------- motion retargeting ---------- */
    runRetargeting(rig);
    runLiveRetarget(rig, liveDir);

    /* ---------- perf ---------- */
    QElapsedTimer timer;
    timer.start();
    const int N = 30;
    for (int i = 0; i < N; ++i)
        PuppetRuntime::renderFrame(rig, sway, i * 97);
    const double msPerFrame = double(timer.nsecsElapsed()) / N / 1e6;
    std::printf("      renderFrame avg: %.2f ms (%d frames)\n", msPerFrame, N);

    std::printf("== RESULT: %d pass, %d fail, %d skip ==\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
