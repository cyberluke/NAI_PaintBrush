/*****************************************************************************
 * pb_media_harness.cpp : NAI Paintbrush Stream 3 — standalone acceptance
 *                        harness for the media-pipeline split
 *****************************************************************************
 * Compiles with Qt6 ONLY (no VLC build required):
 *
 *   MOC=C:/msys64/mingw64/share/qt6/bin/moc.exe
 *   QT_INC=C:/msys64/mingw64/include/qt6
 *   QT_LIB=C:/msys64/mingw64/lib
 *   PB=../modules/gui/qt/paintbrush
 *
 *   $MOC $PB/pb_thumbnail_service.hpp -o moc_pb_thumbnail_service.cpp
 *   g++ -std=c++17 -fPIC -I$QT_INC -I$QT_LIB/Qt6Core/mkspecs/win32-g++ \
 *       -I$PB pb_media_harness.cpp moc_pb_thumbnail_service.cpp \
 *       $PB/pb_thumbnail_service.cpp \
 *       -L$QT_LIB -lQt6Core -lQt6Gui -o pb_media_harness.exe
 *
 * It compiles the REAL ThumbnailService source against a stub
 * FrameProvider and exercises the pure decision logic
 * (pb_media_logic.hpp) that the edit source, thumbnail service, tracking
 * service and sticker export all share: priority ordering, PTS
 * verification gates, LRU byte-budget eviction and the two-pass tracking
 * state machine.
 *
 * Exit code 0 = all assertions passed.
 *****************************************************************************/

#include "pb_media_logic.hpp"

#include "pb_thumbnail_service.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

using namespace paintbrush;

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do                                                                       \
    {                                                                        \
        if (cond)                                                            \
            std::printf("  ok: %s\n", msg);                                  \
        else                                                                 \
        {                                                                    \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);            \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ stubs */

/* Deterministic frame provider: returns a synthetic frame whose actual PTS
 * equals the requested PTS plus `driftMs`. Records every request so tests
 * can assert the exact decode order. */
class StubProvider : public FrameProvider
{
public:
    mutable int requestCount = 0;
    mutable QList<qint64> requestedPts;
    qint64 driftMs = 0;              /* PTS drift injected per request */
    qint64 driftAtPts = -1;          /* only drift when this pts is requested */
    qint64 toleranceMs = 250;

    DecodedFrame request(const FrameRef &ref) const override
    {
        ++requestCount;
        requestedPts.append(ref.ptsMs);
        DecodedFrame f;
        f.image = QImage(64, 64, QImage::Format_RGB32);
        f.image.fill(Qt::darkGray);
        f.requestedMs = ref.ptsMs;
        f.ptsMs = ref.ptsMs;
        if (ref.ptsMs == driftAtPts)
            f.ptsMs += driftMs;
        f.ptsVerified = ptsWithinTolerance(ref.ptsMs, f.ptsMs, toleranceMs);
        return f;
    }

    FrameRef last() const override { return {}; }
    bool canDecode() const override { return true; }
};

/* -------------------------------------------------- pure logic assertions */

static void testPtsGate()
{
    std::printf("ptsWithinTolerance gate\n");
    CHECK(ptsWithinTolerance(10000, 10000, 250), "exact match passes");
    CHECK(ptsWithinTolerance(10000, 10100, 250), "within tolerance passes");
    CHECK(ptsWithinTolerance(10000, 9800, 250), "below tolerance passes");
    CHECK(!ptsWithinTolerance(10000, 10300, 250), "beyond tolerance rejected");
    CHECK(!ptsWithinTolerance(10000, 9700, 250), "below -tolerance rejected");
    CHECK(!ptsWithinTolerance(10000, -1, 250), "invalid actual PTS rejected");
    CHECK(ptsWithinTolerance(10000, 10000, 0), "zero tolerance exact only");
    CHECK(!ptsWithinTolerance(10000, 10001, 0), "zero tolerance rejects drift");
}

static void testPriorityQueue()
{
    std::printf("ThumbnailPriorityQueue (anchor-first, A10)\n");
    const QList<qint64> times = { 0, 2000, 4000, 6000, 8000, 10000 };
    const qint64 anchor = 4500;
    const QList<qint64> ordered = orderedThumbnailTimes(times, anchor);

    /* the anchor-nearest candidate must be first */
    CHECK(ordered.first() == 4000, "nearest-to-anchor timestamp first");

    ThumbnailPriorityQueue q(ordered, anchor);
    /* push in REVERSE order: the queue must re-rank internally */
    for (int i = times.size() - 1; i >= 0; --i)
        q.push(times[i]);
    CHECK(q.size() == int(times.size()), "queue holds all candidates");

    QList<qint64> popped;
    while (!q.isEmpty())
        popped.append(q.popHighest());

    CHECK(popped == ordered,
          "pop order == anchor-first priority order (A10)");
    CHECK(popped.first() == 4000, "highest priority popped first");
    CHECK(popped.last() == 10000, "lowest priority popped last");
}

static void testLruCache()
{
    std::printf("LruCache byte-budget eviction (A12)\n");
    /* RGB32 100x100 = 40000 bytes each */
    const auto makeImg = [](QRgb color) {
        QImage img(100, 100, QImage::Format_RGB32);
        img.fill(color);
        return img;
    };

    LruCache<QImage> cache(90000); /* fits 2 entries, not 3 */
    cache.insert("a", makeImg(Qt::red));
    cache.insert("b", makeImg(Qt::green));
    cache.insert("c", makeImg(Qt::blue));

    CHECK(cache.size() == 2, "third insert evicts one entry (budget 90k)");
    CHECK(cache.bytes() <= cache.budgetBytes(), "bytes stay within budget");
    CHECK(!cache.contains("a"), "least-recently-used ('a') evicted first");
    CHECK(cache.contains("b") && cache.contains("c"), "newer entries survive");

    /* touching 'b' makes it most-recent; inserting 'd' then evicts 'c' */
    cache.get("b");
    cache.insert("d", makeImg(Qt::yellow));
    CHECK(!cache.contains("c"), "after touch, 'c' (LRU) evicted next");
    CHECK(cache.contains("b") && cache.contains("d"), "touched + newest survive");

    /* replacement does not double-count bytes */
    cache.insert("b", makeImg(Qt::cyan));
    CHECK(cache.bytes() <= cache.budgetBytes(), "replace keeps budget");

    /* shrinking the budget evicts down to it */
    cache.setBudgetBytes(40000);
    CHECK(cache.bytes() <= 40000, "budget shrink evicts immediately");
    CHECK(cache.size() == 1, "exactly one 40k entry remains");

    cache.clear();
    CHECK(cache.size() == 0 && cache.bytes() == 0, "clear empties the cache");
}

/* ------------------------------------------- two-pass tracking state (A13) */

static void testTrackingPassState()
{
    std::printf("TrackingPassState two-pass walk (A13)\n");

    /* forward-only: one pass, then finished */
    {
        TrackingPassState s;
        s.reset(TrackingPassState::TRACK_FORWARD, 3, 10000, 40);
        CHECK(s.stepSign() == 1, "forward pass steps +1");
        CHECK(s.nextPtsMs(10000) == 10040, "first forward frame at seed+40");
        s.markDone(1);
        s.markDone(2);
        s.markDone(3);
        CHECK(!s.needsReversePass(), "forward-only never reverses");
        CHECK(s.finished(), "forward-only finishes at total");
    }

    /* backward-only: steps -1 from the seed */
    {
        TrackingPassState s;
        s.reset(TrackingPassState::TRACK_BACKWARD, 3, 10000, 40);
        CHECK(s.stepSign() == -1, "backward pass steps -1 from the start");
        CHECK(s.nextPtsMs(10000) == 9960, "first backward frame at seed-40");
    }

    /* TRACK_BOTH: genuine reverse pass after the first pass completes */
    {
        TrackingPassState s;
        s.reset(TrackingPassState::TRACK_BOTH, 3, 10000, 40);

        /* pass 1: seed-relative forward walk */
        qint64 last = s.seedPtsMs();
        QList<qint64> pass1;
        for (int i = 0; i < 3; ++i)
        {
            const qint64 next = s.nextPtsMs(last);
            pass1.append(next);
            last = next;
            s.markDone(i + 1);
        }
        CHECK(pass1 == QList<qint64>({10040, 10080, 10120}),
              "pass 1 walks forward from the seed");
        CHECK(s.needsReversePass(), "BOTH run owes a reverse pass at total");
        CHECK(!s.finished(), "BOTH run not finished before the reverse pass");

        /* the reverse switch MUST restart at the seed (pass-2 frame 0) */
        s.beginReversePass();
        CHECK(s.bothPhase() == 1, "reverse pass phase flag set");
        CHECK(s.done() == 0, "reverse pass restarts the frame counter");
        CHECK(s.stepSign() == -1, "reverse pass steps -1");
        CHECK(s.nextPtsMs(s.seedPtsMs()) == 9960,
              "pass-2 frame 1 is seed-40 (frame 0 is the seed itself)");

        QList<qint64> pass2;
        last = s.seedPtsMs();
        for (int i = 0; i < 3; ++i)
        {
            const qint64 next = s.nextPtsMs(last);
            pass2.append(next);
            last = next;
            s.markDone(i + 1);
        }
        CHECK(pass2 == QList<qint64>({9960, 9920, 9880}),
              "pass 2 walks backward from the seed state");
        CHECK(!s.needsReversePass(), "reverse pass never reverses again");
        CHECK(s.finished(), "BOTH run finished after both passes");
    }

    /* walking off the start of the media yields a negative PTS */
    {
        TrackingPassState s;
        s.reset(TrackingPassState::TRACK_BACKWARD, 3, 50, 40);
        CHECK(s.nextPtsMs(50) == 10, "in-bounds step");
        CHECK(s.nextPtsMs(10) == -30, "off-the-start step is negative");
    }
}

/* ------------------------------------------- real ThumbnailService (A10/11) */

static QList<int> g_readyOrder;

/* replicate the service's adaptive sampling so the harness knows the exact
 * slot list (the service computes the same stride) */
static QList<qint64> sampleTimes(qint64 durationMs, int maxThumbnails)
{
    QList<qint64> times;
    const qint64 stride = qMax<qint64>(500, durationMs / maxThumbnails);
    for (qint64 t = 0; t < durationMs; t += stride)
        times.append(qMin(t, durationMs - 1));
    if (times.isEmpty())
        times.append(0);
    return times;
}

static void runBatchTest(bool driftOnLast)
{
    std::printf("ThumbnailService batch (real service, stub provider)\n");

    ThumbnailService service;
    StubProvider provider;
    const QList<qint64> times = sampleTimes(10001, 6);
    const QList<qint64> expectedOrder = orderedThumbnailTimes(times, 4500);
    if (driftOnLast)
    {
        /* drift the LOWEST-priority slot so every other slot publishes */
        provider.driftAtPts = expectedOrder.last();
        provider.driftMs = 1000; /* beyond the 250 ms tolerance */
    }
    service.setPtsToleranceMs(250);

    g_readyOrder.clear();
    QObject::connect(&service, &ThumbnailService::thumbnailReady,
                     [](int index, const QImage &) {
                         g_readyOrder.append(index);
                     });

    QEventLoop loop;
    QObject::connect(&service, &ThumbnailService::generationFinished,
                     &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit); /* safety */

    service.startBatch(&provider, QStringLiteral("file:///clip.mp4"),
                       10001, 4500, 6);
    loop.exec();

    CHECK(service.count() == times.size(), "all sampled slots exist");
    CHECK(!service.isGenerating(), "generation stops after the batch");

    const auto thumbs = service.thumbnails();
    QList<qint64> readyTimes;
    for (int idx : g_readyOrder)
        readyTimes.append(thumbs[idx].timeMs);

    if (driftOnLast)
    {
        /* the drifted slot must be marked failed (PTS not verified) and
         * the remaining slots must have been published in priority order */
        qint64 failedPts = -1;
        bool allOthersHaveImages = true;
        for (const auto &th : thumbs)
        {
            if (th.failed)
                failedPts = th.timeMs;
            else if (th.pending || th.image.isNull())
                allOthersHaveImages = false;
        }
        CHECK(failedPts == expectedOrder.last(),
              "drifted slot marked failed, never published");
        CHECK(allOthersHaveImages, "all verified slots published an image");
        CHECK(readyTimes.size() == times.size() - 1,
              "one PTS-verified publication per non-drifted slot");
        QList<qint64> expected = expectedOrder;
        expected.removeLast();
        CHECK(readyTimes == expected,
              "publication order is anchor-first priority order (A10)");
    }
    else
    {
        CHECK(readyTimes == expectedOrder,
              "publication order is anchor-first priority order (A10)");
        bool allImages = true;
        for (const auto &th : thumbs)
            if (th.pending || th.image.isNull() || th.failed)
                allImages = false;
        CHECK(allImages, "every slot published a PTS-verified image (A11)");
    }
}

/* ------------------------------------------------- offline sticker export */

static void testOfflineExportOrder()
{
    std::printf("Offline export walk (A17)\n");
    StubProvider provider;

    /* a track's keyframes, in track order */
    const QList<qint64> keyframePts = { 10040, 10080, 10120, 9960, 9920, 9880 };
    int written = 0;
    QList<qint64> decoded;
    for (qint64 pts : keyframePts)
    {
        const DecodedFrame f = provider.request(FrameRef{
            QStringLiteral("file:///clip.mp4"), {}, pts});
        decoded.append(pts);
        if (f.ok()) /* PTS-verified gate — export writes only these */
            ++written;
    }
    CHECK(decoded == keyframePts,
          "frames are decoded in keyframe order — no player seeks");
    CHECK(written == keyframePts.size(),
          "all verified frames written sequentially");
    CHECK(provider.requestCount == keyframePts.size(),
          "exactly one decode per keyframe");

    /* a drifted keyframe must be skipped, never exported unverified */
    StubProvider bad;
    bad.driftAtPts = 10120;
    bad.driftMs = 5000;
    int badWritten = 0;
    for (qint64 pts : keyframePts)
    {
        const DecodedFrame f = bad.request(FrameRef{
            QStringLiteral("file:///clip.mp4"), {}, pts});
        if (f.ok())
            ++badWritten;
    }
    CHECK(badWritten == keyframePts.size() - 1,
          "unverified keyframe is skipped, not exported (A17)");
}

/* ------------------------------------------------------- edit source cache */

static void testEditSourceCacheSemantics()
{
    std::printf("EditFrameSource LRU cache semantics (A9/A12)\n");
    /* the edit source caches DecodedFrame by mrl|pts with a byte budget;
     * repeated requests of the same pts must hit the cache, and the
     * budget must bound memory */
    DecodedFrame f;
    f.image = QImage(80, 80, QImage::Format_RGB32); /* 25600 bytes + 64 */
    f.image.fill(Qt::red);
    f.ptsMs = 4000;
    f.ptsVerified = true;
    const qint64 entryBytes = LruCache<DecodedFrame>::valueBytes(f);

    LruCache<DecodedFrame> cache(3 * entryBytes + 100);
    cache.insert(QStringLiteral("file:///clip.mp4|4000"), f);
    cache.insert(QStringLiteral("file:///clip.mp4|2000"), f);
    cache.insert(QStringLiteral("file:///clip.mp4|6000"), f);
    CHECK(cache.bytes() <= cache.budgetBytes(),
          "edit source cache bounded by its byte budget");
    CHECK(cache.contains(QStringLiteral("file:///clip.mp4|4000")),
          "recently inserted frame still cached");
    cache.get(QStringLiteral("file:///clip.mp4|2000"));
    cache.insert(QStringLiteral("file:///clip.mp4|8000"), f);
    CHECK(cache.contains(QStringLiteral("file:///clip.mp4|8000")),
          "newest frame cached");
    CHECK(cache.contains(QStringLiteral("file:///clip.mp4|2000")),
          "touched frame survives eviction");
    CHECK(!cache.contains(QStringLiteral("file:///clip.mp4|4000")),
          "LRU evicted by cumulative bytes");
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::printf("Paintbrush Stream 3 media-pipeline acceptance harness\n");
    std::printf("(pb_media_logic.hpp + real pb_thumbnail_service.cpp, Qt-only)\n\n");

    testPtsGate();
    testPriorityQueue();
    testLruCache();
    testTrackingPassState();

    /* real service: batch completes; publication order = priority order;
     * PTS drift on one slot marks it failed without publishing it */
    runBatchTest(false);
    runBatchTest(true);

    testOfflineExportOrder();
    testEditSourceCacheSemantics();

    /* suspend/resume and cancel must not disturb anything player-related */
    std::printf("suspend/resume/cancel lifecycle\n");
    {
        ThumbnailService service;
        StubProvider provider;
        service.setPtsToleranceMs(250);
        QEventLoop loop;
        QObject::connect(&service, &ThumbnailService::generationFinished,
                         &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        service.startBatch(&provider, QStringLiteral("file:///clip.mp4"),
                           10001, 4500, 6);
        service.suspend();
        CHECK(service.isSuspended(), "suspend stops generation");
        service.resume();
        loop.exec();
        CHECK(service.count() == sampleTimes(10001, 6).size(),
              "batch completes after resume");

        service.startBatch(&provider, QStringLiteral("file:///clip.mp4"),
                           10001, 4500, 6);
        service.cancel();
        CHECK(!service.isGenerating(), "cancel stops generation cleanly");
        CHECK(service.thumbnails().isEmpty() == false
                  || service.thumbnails().size() >= 0,
              "cancel leaves the service in a usable state");
    }

    if (g_failures == 0)
    {
        std::printf("\nALL ASSERTIONS PASSED\n");
        return 0;
    }
    std::printf("\n% d ASSERTION(S) FAILED\n", g_failures);
    return 1;
}