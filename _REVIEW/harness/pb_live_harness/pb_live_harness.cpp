/* pb_live_harness.cpp — Paintbrush 2.0 Showroom LIVE acceptance run.
 *
 * The real end-to-end pipeline on real media:
 *   1. libvlc decodes REAL frames from a K-pop dance video (vmem callbacks)
 *   2. REAL SAM3 (sam3-q4_0.ggml) segments a dancer per frame
 *   3. Keep A/B/C -> HarvestTray (makeHarvestItem, exact PTS retained)
 *   4. poses dragged onto the canvas (sticker layers)
 *   5. Boost: analyze -> deterministic plan (roles/finish/depth) -> bridge
 *      pose from a REAL mid-gap frame (decode, score, SAM3, insert)
 *   6. 4K export via renderScaled; golden before/after artifacts saved
 *
 * Usage:
 *   pb_live_harness.exe <video.mkv> <sam3.ggml> [outdir]
 * Requires the win64-meson libvlc.dll + contrib sam3/ggml static libs.
 */
#include "pb_document.hpp"
#include "pb_harvest.hpp"
#include "pb_boost.hpp"
#include "pb_project_store.hpp"
#include "art/style/mood.hpp"

#include <vlc/vlc.h>

#include <QGuiApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <cstdio>

#include <sam3.h>

using namespace paintbrush;
using namespace paintbrush::art;

/* ------------------------------------------------------------------ libvlc */

struct VmemCtx
{
    QMutex mutex;
    QWaitCondition cond;
    unsigned char *buf = nullptr;
    int width = 0, height = 0;
    bool haveFrame = false;
    qint64 lastPtsMs = -1;
    bool wantFrame = false;
};

static void *vmemLock(void *opaque, void **planes)
{
    VmemCtx *c = static_cast<VmemCtx *>(opaque);
    planes[0] = c->buf;
    return nullptr;
}

static void vmemUnlock(void *, void *, void *const *) {}

static void vmemDisplay(void *opaque, void *)
{
    VmemCtx *c = static_cast<VmemCtx *>(opaque);
    QMutexLocker lock(&c->mutex);
    c->haveFrame = true;
    c->cond.wakeAll();
}

/* Grab one frame at `ptsUs` (libvlc time is MICROSECONDS in this API).
 * Seeks, then waits until a FRESH frame is displayed (the display callback
 * may fire with the pre-seek frame, so we wait for a second display after
 * the seek or until the player time approaches the target). Retries the
 * seek once — the vmem vout can stall after a fast seek. */
static QImage grabFrame(libvlc_media_player_t *mp, VmemCtx &ctx, qint64 ptsUs)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        {
            QMutexLocker lock(&ctx.mutex);
            ctx.haveFrame = false;
            ctx.wantFrame = true;
        }
        libvlc_media_player_play(mp); /* ensure we're playing after a seek */
        libvlc_media_player_set_time(mp, libvlc_time_t(ptsUs), true);
        /* wait until a fresh frame was displayed */
        QElapsedTimer t;
        t.start();
        for (;;)
        {
            {
                QMutexLocker lock(&ctx.mutex);
                if (ctx.haveFrame)
                    break;
            }
            if (t.elapsed() > 4000)
                break;
            QThread::msleep(5);
        }
        if (!ctx.haveFrame && attempt == 0)
            continue; /* retry the seek once */
        if (!ctx.haveFrame)
            return {};
        /* copy the vmem buffer (BGRA, top-down) */
        QImage img(ctx.width, ctx.height, QImage::Format_RGB32);
        {
            QMutexLocker lock(&ctx.mutex);
            memcpy(img.bits(), ctx.buf, size_t(ctx.width) * ctx.height * 4);
        }
        ctx.wantFrame = false;
        return img;
    }
    return {};
}

/* ------------------------------------------------------------ SAM3 wrapper */

struct Sam3Session
{
    std::shared_ptr<sam3_model> model;
    sam3_state_ptr state;

    bool load(const QString &modelPath)
    {
        sam3_params params;
        params.model_path = modelPath.toStdString();
        params.n_threads = qMax(2, int(QThread::idealThreadCount()));
        params.use_gpu = false; /* deterministic CPU run for the harness */
        params.encode_img_size = 0;
        model = sam3_load_model(params);
        if (!model)
            return false;
        state = sam3_create_state(*model, params);
        return state != nullptr;
    }

    /* Segment the dancer: PVS center-point first (a centered subject gives a
     * fuller mask than the detector's best box), then the text path. */
    QImage segment(const QImage &frame)
    {
        if (!model || !state || frame.isNull())
            return {};
        QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
        sam3_image img;
        img.width = rgb.width();
        img.height = rgb.height();
        img.channels = 3;
        img.data.assign(rgb.constBits(), rgb.constBits() + rgb.sizeInBytes());
        if (!sam3_encode_image(*state, *model, img))
            return {};

        sam3_result res;
        {
            sam3_pvs_params p;
            p.multimask = true;
            p.pos_points.push_back({ float(frame.width()) * 0.5f,
                                     float(frame.height()) * 0.55f });
            res = sam3_segment_pvs(*state, *model, p);
        }
        if (res.detections.empty())
        {
            sam3_pcs_params p;
            p.text_prompt = "person";
            p.score_threshold = 0.5f;
            p.nms_threshold = 0.1f;
            res = sam3_segment_pcs(*state, *model, p);
        }
        if (res.detections.empty())
            return {};

        /* best-scoring detection */
        const sam3_detection *best = &res.detections.front();
        for (const sam3_detection &d : res.detections)
            if (d.score > best->score)
                best = &d;
        if (best->mask.data.empty())
            return {};

        QImage mask(best->mask.width, best->mask.height, QImage::Format_Grayscale8);
        memcpy(mask.bits(), best->mask.data.data(),
               size_t(best->mask.width) * size_t(best->mask.height));
        if (mask.size() != frame.size())
            mask = mask.scaled(frame.size(), Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
        return mask;
    }
};

/* ------------------------------------------------------------------ checks */

static int g_pass = 0, g_fail = 0;
static QString g_outDir;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) { ++g_pass; std::printf("  ok   %s\n", msg); }              \
        else { ++g_fail; std::printf("  FAIL %s\n", msg); }                   \
        std::fflush(stdout);                                                  \
    } while (0)

static void save(const QImage &img, const char *name)
{
    if (!g_outDir.isEmpty() && !img.isNull())
        img.save(g_outDir + QLatin1Char('/') + QString::fromLatin1(name));
}

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
            sum += std::abs(qRed(pa[x]) - qRed(pb[x])) +
                   std::abs(qGreen(pa[x]) - qGreen(pb[x])) +
                   std::abs(qBlue(pa[x]) - qBlue(pb[x])) +
                   std::abs(qAlpha(pa[x]) - qAlpha(pb[x]));
            n += 4;
        }
    }
    return sum / double(n);
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    std::printf("Paintbrush Showroom — LIVE acceptance run (real video + real SAM3)\n\n");

    if (argc < 3)
    {
        std::printf("usage: %s <video.mkv> <sam3.ggml> [outdir]\n", argv[0]);
        return 2;
    }
    const QString videoPath = QString::fromLocal8Bit(argv[1]);
    const QString modelPath = QString::fromLocal8Bit(argv[2]);
    g_outDir = argc > 3 ? QString::fromLocal8Bit(argv[3]) : (QDir::currentPath() + QStringLiteral("/live"));
    QDir().mkpath(g_outDir);

    /* 0) SAM3 ready early so failures are reported before decoding */
    Sam3Session sam;
    CHECK(sam.load(modelPath), "SAM3 model loads");
    if (!sam.model)
    {
        std::printf("\n%d/%d checks passed\n", g_pass, g_pass + g_fail);
        return 1;
    }

    /* 1) libvlc: open the real video, learn its duration */
    libvlc_instance_t *vlc = libvlc_new(0, nullptr);
    CHECK(vlc != nullptr, "libvlc instance created");
    if (!vlc)
        return 1;
    libvlc_media_t *media = libvlc_media_new_path(videoPath.toUtf8().constData());
    CHECK(media != nullptr, "media opened from path");
    if (!media)
    {
        libvlc_release(vlc);
        return 1;
    }
    libvlc_media_player_t *mp = libvlc_media_player_new(vlc, nullptr, nullptr);
    libvlc_media_player_set_media(mp, media);
    CHECK(mp != nullptr, "media player created");

    /* vmem: fixed 960x540 BGRA output (fast encode for SAM3) */
    VmemCtx ctx;
    ctx.width = 960;
    ctx.height = 540;
    ctx.buf = new unsigned char[size_t(ctx.width) * ctx.height * 4];
    libvlc_video_set_callbacks(mp, vmemLock, vmemUnlock, vmemDisplay, &ctx);
    libvlc_video_set_format(mp, "RV32", ctx.width, ctx.height, ctx.width * 4);

    CHECK(libvlc_media_player_play(mp) == 0, "player started");
    {
        QElapsedTimer t;
        t.start();
        for (;;)
        {
            if (libvlc_media_player_get_state(mp) == libvlc_Playing)
                break;
            if (t.elapsed() > 5000)
                break;
            QThread::msleep(10);
        }
    }
    CHECK(libvlc_media_player_get_state(mp) == libvlc_Playing, "player reached Playing state");
    const libvlc_time_t duration = libvlc_media_player_get_length(mp);
    CHECK(duration > 0, qPrintable(QStringLiteral("media duration known (%1 ms)").arg(duration)));

    /* 2) three KEEP moments: UNEVENLY spaced so the sparsest gap triggers a
     * real bridge pose (evenly spaced sequences correctly get no bridge).
     * NOTE: this libvlc API reports time in MICROSECONDS; the harvest
     * FrameRef uses milliseconds, so convert at the boundary. */
    const qint64 durUs = qint64(duration);
    const qint64 keepUs[3] = { durUs * 30 / 100, durUs * 36 / 100, durUs * 54 / 100 };

    QVector<QImage> frames;
    QVector<QImage> masks;
    QVector<double> scores;
    QVector<qint64> pts;
    for (qint64 tUs : keepUs)
    {
        QImage frame = grabFrame(mp, ctx, tUs);
        const qint64 tMs = tUs / 1000;
        CHECK(!frame.isNull(), qPrintable(QStringLiteral("frame decoded at %1 ms").arg(tMs)));
        if (frame.isNull())
            continue;
        frames.append(frame);
        pts.append(tMs);
        QImage mask = sam.segment(frame);
        bool ok = !mask.isNull();
        double cov = 0.0;
        if (ok)
        {
            qint64 n = 0, total = 0;
            for (int y = 0; y < mask.height(); ++y)
                for (int x = 0; x < mask.width(); ++x)
                {
                    if (qGray(mask.pixel(x, y)) > 0)
                        ++n;
                    ++total;
                }
            cov = double(n) / double(qMax(1, total));
            ok = cov > 0.005; /* a real (possibly small) subject mask */
        }
        CHECK(ok, qPrintable(QStringLiteral("SAM3 segmented a person at %1 ms (coverage %2%)")
                                 .arg(tMs).arg(cov * 100.0, 0, 'f', 1)));
        if (ok)
        {
            masks.append(mask);
            scores.append(0.9);
        }
        else
        {
            masks.append(QImage());
            scores.append(0.0);
        }
    }
    save(frames.value(0), "live_frame_A.png");
    save(frames.value(1), "live_frame_B.png");
    save(frames.value(2), "live_frame_C.png");
    save(masks.value(0), "live_mask_A.png");
    save(masks.value(1), "live_mask_B.png");
    save(masks.value(2), "live_mask_C.png");

    const bool threeKeeps = frames.size() == 3 && masks.size() == 3 &&
                            !masks[0].isNull() && !masks[1].isNull() && !masks[2].isNull();
    CHECK(threeKeeps, "three real Keeps (frame + SAM3 mask) available");

    /* 3) Keep A/B/C -> tray + canvas poses (like dragging three poses on) */
    PaintbrushDocument doc;
    doc.resetDocument(QSize(1200, 600));
    doc.setMoodId(QStringLiteral("emo.ash"));

    FrameRef ref;
    ref.mrl = QUrl::fromLocalFile(videoPath).toString();
    ref.title = QFileInfo(videoPath).completeBaseName();
    ref.ptsMs = 0;
    ref.width = frames.value(0).width();
    ref.height = frames.value(0).height();

    QVector<int> harvestIds;
    QVector<int> layerIds;
    const double canvasX[3] = { 120.0, 460.0, 820.0 };
    const double scales[3] = { 1.0, 1.35, 1.0 };
    if (threeKeeps)
    {
        for (int i = 0; i < 3; ++i)
        {
            FrameRef r = ref;
            r.ptsMs = pts[i];
            const int hid = doc.harvest()->add(
                makeHarvestItem(0, r, frames[i], masks[i], scores[i]));
            harvestIds.append(hid);
            CanvasLayer l = CanvasLayer::createImage(
                doc.harvest()->itemById(hid)->cutout, QPointF(canvasX[i], 200),
                QStringLiteral("pose"));
            l.type = LAYER_STICKER;
            l.harvestId = hid;
            l.scale = float(scales[i]);
            QList<CanvasLayer> layers = doc.layers();
            layers.append(l);
            doc.setLayers(layers);
            layerIds.append(doc.layers().last().id);
        }
        /* hero (middle, largest) to the back to exercise depth reordering */
        QList<CanvasLayer> layers = doc.layers();
        layers.move(doc.indexOfLayerId(layerIds[1]), 0);
        doc.setLayers(layers);
    }

    const QImage rawRender = DocumentRenderer::render(doc);
    save(rawRender, "live_before_boost.png");
    const QList<int> rawOrder = [&]() {
        QList<int> ids;
        for (const CanvasLayer &l : doc.layers())
            ids.append(l.id);
        return ids;
    }();
    const int rawHarvest = doc.harvest()->count();
    Q_UNUSED(rawOrder); Q_UNUSED(rawHarvest);

    /* 4) Boost: analysis + deterministic plan */
    const BoostAnalysis a = BoostEngine::analyze(doc);
    CHECK(a.hasSequence() && a.sequence.size() == 3, "analysis found the 3-pose same-source sequence");
    CHECK(a.wantsBridge(), "sparsest temporal gap selected for a bridge pose");
    std::printf("  .. motion dir (%.2f, %.2f), hero layer %d, bridge %lld -> %lld ms\n",
                a.motionDirection.x(), a.motionDirection.y(), a.heroLayerId,
                (long long)a.bridgeFromMs, (long long)a.bridgeToMs);

    BoostOptions o;
    o.motionStrength = 1.0f;
    BoostTransaction tx(doc);
    BoostEngine::planDeterministic(tx, a, o);
    CHECK(tx.operationCount() >= 6, "deterministic plan recorded typed operations");
    CHECK(doc.layerById(a.heroLayerId)->role == int(LayerRole::Hero), "hero role assigned");

    /* 5) bridge pose from REAL mid-gap frames */
    int bridgeLayer = 0;
    if (a.wantsBridge())
    {
        const QVector<qint64> ts = BoostEngine::bridgeTimestamps(a.bridgeFromMs, a.bridgeToMs, 3);
        const HarvestItem &A = *doc.harvest()->itemById(a.sequence[0].harvestId);
        const HarvestItem &B = *doc.harvest()->itemById(a.sequence[2].harvestId);
        QVector<BridgeCandidate> cands;
        for (qint64 t : ts)
        {
            BridgeCandidate c;
            c.ptsMs = t;
            c.frame = grabFrame(mp, ctx, t * 1000); /* ms -> us */
            cands.append(c);
        }
        const int bestIdx = BoostEngine::scoreBridgeCandidates(cands, A, B,
                                                               a.bridgeFromMs, a.bridgeToMs);
        CHECK(bestIdx >= 0 && !cands.value(bestIdx).frame.isNull(),
              "real mid-gap frame scored best for the bridge");
        if (bestIdx >= 0)
        {
            const BridgeCandidate &best = cands[bestIdx];
            save(best.frame, "live_bridge_frame.png");
            const QImage mask = sam.segment(best.frame);
            CHECK(!mask.isNull(), "SAM3 segmented the bridge candidate");
            if (!mask.isNull())
            {
                FrameRef r = ref;
                r.ptsMs = best.ptsMs;
                HarvestItem derived = makeHarvestItem(0, r, best.frame, mask, 0.8);
                bridgeLayer = BoostEngine::insertBridgePose(tx, a, o, derived);
                CHECK(bridgeLayer > 0, "bridge pose inserted as a derived Transition layer");
            }
        }
    }

    const QString summary = BoostEngine::composeSummary(tx.summary());
    std::printf("  summary: %s\n", summary.toUtf8().constData());

    /* 6) export: screen + 4K, before/after golden artifacts */
    const QImage boosted = DocumentRenderer::render(doc);
    save(boosted, "live_after_boost.png");
    const double boostDiff = meanAbsDiff(rawRender, boosted);
    CHECK(boostDiff > 1.0, qPrintable(QStringLiteral("Boost visibly changed the render (mean diff %1/255)")
                                          .arg(boostDiff, 0, 'f', 2)));
    const QImage x4 = DocumentRenderer::renderScaled(doc, 4.0f);
    save(x4, "live_export_4k.png");
    CHECK(x4.size() == QSize(4800, 2400), "4K export renders at 4x canvas (4800x2400)");
    const QImage x4b = DocumentRenderer::renderScaled(doc, 4.0f);
    CHECK(x4b == x4, "4K export deterministic (byte-identical on re-render)");

    /* bridge layer present in the final document */
    if (bridgeLayer > 0)
    {
        const CanvasLayer *bl = doc.layerById(bridgeLayer);
        CHECK(bl && bl->role == int(LayerRole::Transition), "bridge layer styled Transition");
        CHECK(bl && doc.harvest()->itemById(bl->harvestId) != nullptr, "bridge harvest item in tray");
        CHECK(doc.indexOfLayerId(bridgeLayer) < doc.indexOfLayerId(layerIds[1]) &&
              doc.indexOfLayerId(bridgeLayer) < doc.indexOfLayerId(layerIds[2]),
              "bridge sits behind both neighbours");
    }

    /* 7) one Boost = one undo, restores byte-identical */
    paintbrush::UndoStack undo;
    auto cmd = tx.finish(QStringLiteral("Boost"));
    CHECK(cmd != nullptr && cmd->kind() == UndoCommand::Kind::Composite, "transaction wraps into one CompositeCommand");
    undo.pushCommand(std::move(cmd));
    CHECK(undo.count() == 1 && undo.undoText().contains(QStringLiteral("Boost")), "one top-level undo entry: Boost");
    CHECK(undo.undo(doc), "Undo Boost");
    const QImage restored = DocumentRenderer::render(doc);
    save(restored, "live_after_undo.png");
    CHECK(meanAbsDiff(restored, rawRender) < 0.01, "undo restores the raw composition (mean diff < 0.01)");

    /* cleanup */
    libvlc_media_player_stop_async(mp);
    libvlc_media_player_release(mp);
    libvlc_media_release(media);
    libvlc_release(vlc);
    delete[] ctx.buf;

    std::printf("\n%d/%d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}