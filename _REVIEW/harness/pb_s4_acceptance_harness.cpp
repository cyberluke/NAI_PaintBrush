/*****************************************************************************
 * s4_acceptance_harness.cpp : Stream 4 acceptance harness (standalone)
 *****************************************************************************
 * Verifies the extraction/sticker/collage invariants of Stream 4:
 *
 *  (a) a sticker is inserted into the EXISTING document via
 *      InsertLayerCommand — layer count grows by 1, existing layers are
 *      preserved, undo removes the sticker, redo re-inserts it;
 *  (b) the sticker layer is transformable and SetTransformCommand
 *      undo/redo round-trips the transform exactly;
 *  (c) the media drawer's sticker editor binds to a real selected track id
 *      (static assertion: no hardcoded `stickerEditor.trackId = 0`, and the
 *      controller exposes the selectedTrackId property);
 *  (d) the sticker preview data URL renders REAL source pixels — the
 *      decoded PNG is non-uniform (color variance from the actual frame),
 *      while the old flat-silhouette path is provably flat;
 *  (e) no UI-thread QThread::msleep remains in the export paths (static
 *      assertion over pb_controller.cpp);
 *  (f) autoCollage composes stickers through typed SetTransformCommands on
 *      immutable assets — layer rasters are byte-identical before/after,
 *      undo restores every transform;
 *  (g) the sticker layer renders through the canonical DocumentRenderer
 *      (the same renderer screen/export use).
 *
 * BUILD (needs a Qt6 environment and the VLC tree; run moc on the two
 * Q_OBJECT headers first):
 *
 *   moc pb_document.hpp -o moc_pb_document.cpp
 *   moc pb_history.hpp  -o moc_pb_history.cpp
 *   <c++> -std=c++17 -fPIC -DHAVE_CONFIG_H \
 *       -I<vlc-root> -I<vlc-build-root> \
 *       -I<qt6>/include -I<qt6>/include/QtCore -I<qt6>/include/QtGui \
 *       s4_acceptance_harness.cpp moc_pb_document.cpp moc_pb_history.cpp \
 *       modules/gui/qt/paintbrush/pb_layer.cpp \
 *       modules/gui/qt/paintbrush/pb_document.cpp \
 *       modules/gui/qt/paintbrush/pb_history.cpp \
 *       modules/gui/qt/paintbrush/pb_sticker.cpp \
 *       modules/gui/qt/paintbrush/pb_common.cpp \
 *       -L<qt6>/lib -lQt6Core -lQt6Gui
 *
 * RUN: s4_acceptance_harness [<vlc-root>]
 * (root defaults to the current directory; the static assertions read the
 * live module sources from <root>/modules/gui/qt/paintbrush/.)
 *
 * Exit code 0 = all assertions passed.
 *****************************************************************************/

#include "pb_document.hpp"
#include "pb_history.hpp"
#include "pb_sticker.hpp"

#include <QImage>
#include <QBuffer>
#include <QSet>
#include <QRgb>
#include <QPainter>
#include <QFile>
#include <QTextStream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

/* ---------------------------------------------------------------- VLC stubs
 * The harness links pb_common.cpp, which references a handful of VLC runtime
 * functions. These stubs keep the harness standalone; the module code under
 * test never calls them with meaningful arguments. */
extern "C" {
char *var_InheritString(vlc_object_t *, const char *)
{
    return nullptr;
}
bool var_InheritBool(vlc_object_t *, const char *)
{
    return false;
}
int64_t var_InheritInteger(vlc_object_t *, const char *)
{
    return 0;
}
void vlc_object_vaLog(vlc_object_t *, int, const char *, const char *,
                      unsigned, const char *, const char *, va_list)
{
}
}

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(cond))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

/* ---------------------------------------------------------------- helpers */

/* Count distinct ARGB colors among pixels with alpha > 0. */
int distinctOpaqueColors(const QImage &image)
{
    QSet<QRgb> colors;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            if (qAlpha(line[x]) != 0)
                colors.insert(line[x] & 0x00ffffff);
    }
    return colors.size();
}

QImage decodeDataUrl(const QString &url)
{
    const int comma = url.indexOf(QLatin1Char(','));
    if (comma < 0)
        return {};
    return QImage::fromData(QByteArray::fromBase64(
        url.mid(comma + 1).toLatin1()));
}

/* A "real" source frame: left half red, right half blue — unmistakably
 * non-uniform. */
QImage makeRealFrame(const QSize &size)
{
    QImage frame(size, QImage::Format_ARGB32);
    frame.fill(Qt::transparent);
    QPainter p(&frame);
    p.fillRect(QRect(0, 0, size.width() / 2, size.height()), Qt::red);
    p.fillRect(QRect(size.width() / 2, 0, size.width() / 2, size.height()),
               Qt::blue);
    p.end();
    return frame;
}

QImage makeMask(const QSize &size)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter p(&mask);
    p.setBrush(Qt::white);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(size.width() / 2.0, size.height() / 2.0),
                  size.width() / 3.0, size.height() / 3.0);
    p.end();
    return mask;
}

/* A sticker layer exactly like PaintbrushController::insertStickerLayer
 * builds it: cutout raster + LAYER_STICKER + FrameRef/seed-mask metadata. */
CanvasLayer makeStickerLayer(const QImage &cutout, const QPointF &position,
                             const QString &name)
{
    CanvasLayer layer = CanvasLayer::createImage(cutout, position, name);
    layer.type = LAYER_STICKER;
    layer.metadata[QStringLiteral("sourceMRL")] = QStringLiteral("file:///clip.mp4");
    layer.metadata[QStringLiteral("sourceTitle")] = QStringLiteral("clip");
    layer.metadata[QStringLiteral("sourceTimeMs")] = qint64(12345);
    layer.metadata[QStringLiteral("seedMask")] = QStringLiteral("data:image/png;base64,AAAA");
    return layer;
}

/* -------------------------------------------------------------- test 3+4
 * (a) extract inserts into the existing document, undo removes it;
 * (b) SetTransformCommand round-trips on the sticker layer. */
void testExtractInsertsIntoExistingDocument()
{
    PaintbrushDocument doc;
    doc.resetDocument(QSize(640, 480)); /* base layer 1 */
    CanvasLayer existing = CanvasLayer::createImage(
        makeRealFrame(QSize(200, 150)), QPointF(10, 10),
        QStringLiteral("Existing layer"));
    QList<CanvasLayer> layers = doc.layers();
    layers.append(existing);
    doc.setLayers(layers); /* 2 layers */
    doc.setCurrentLayerIndex(0);
    const int before = doc.layerCount();

    UndoStack undo;

    /* the exact command shape PaintbrushController::insertStickerLayer uses */
    QImage cutout = makeRealFrame(QSize(90, 60));
    CanvasLayer sticker = makeStickerLayer(cutout, QPointF(300, 200),
                                           QStringLiteral("Extracted object"));
    InsertLayerCommand ins;
    ins.desc = QStringLiteral("Add Extracted object");
    QList<CanvasLayer> docLayers = doc.layers();
    docLayers.append(sticker);
    ins.index = docLayers.size() - 1;
    ins.prevCurrentIndex = doc.currentLayerIndex();
    doc.setLayers(docLayers);
    doc.setCurrentLayerIndex(ins.index);
    ins.layer = *doc.layerAt(ins.index);
    undo.pushInsertLayer(ins);

    /* layer count grows by exactly 1; existing layers preserved */
    CHECK(doc.layerCount() == before + 1);
    CHECK(doc.layerAt(0)->name == QStringLiteral("Layer 1"));
    CHECK(doc.layerAt(1)->name == QStringLiteral("Existing layer"));
    CHECK(doc.layerAt(2)->type == LAYER_STICKER);
    CHECK(doc.layerAt(2)->name == QStringLiteral("Extracted object"));
    CHECK(doc.layerAt(2)->metadata.value(QStringLiteral("sourceTimeMs")).toLongLong() == 12345);
    CHECK(doc.layerAt(2)->raster.size() == QSize(90, 60));
    CHECK(undo.canUndo());

    /* (b) transform the sticker and round-trip through undo/redo */
    CanvasLayer *st = doc.layerAt(2);
    const QTransform beforeT = st->transform();
    SetTransformCommand tf;
    tf.layerId = st->id;
    tf.layerIndex = 2;
    tf.before = beforeT;
    QTransform afterT;
    afterT.translate(100, 50);
    afterT.rotate(30);
    afterT.scale(1.5, 1.5);
    st->setTransform(afterT);
    tf.after = st->transform();
    CHECK(tf.after != beforeT);
    undo.pushSetTransform(tf);

    CHECK(undo.undo(doc)); /* removes the transform */
    CHECK(doc.layerCount() == before + 1); /* sticker still there */
    CHECK(doc.layerAt(2)->transform() == beforeT);
    CHECK(undo.redo(doc)); /* re-applies the transform */
    CHECK(doc.layerAt(2)->transform() == tf.after);

    /* undo the insert: the sticker disappears, the other layers survive */
    CHECK(undo.undo(doc)); /* transform off */
    CHECK(undo.undo(doc)); /* insert off */
    CHECK(doc.layerCount() == before);
    CHECK(doc.layerAt(0)->name == QStringLiteral("Layer 1"));
    CHECK(doc.layerAt(1)->name == QStringLiteral("Existing layer"));
    /* raster of the surviving layer untouched by all of the above */
    CHECK(!doc.layerAt(1)->raster.isNull());

    CHECK(undo.redo(doc)); /* insert back on */
    CHECK(doc.layerCount() == before + 1);
    CHECK(doc.layerAt(2)->type == LAYER_STICKER);
    CHECK(doc.layerAt(2)->metadata.value(QStringLiteral("sourceMRL")).toString()
          == QStringLiteral("file:///clip.mp4"));
}

/* ---------------------------------------------------------------- test 6
 * (f) autoCollage = composite of SetTransformCommands, no raster baking. */
void testCollageIsCommandsOnly()
{
    PaintbrushDocument doc;
    doc.resetDocument(QSize(800, 600));
    CanvasLayer a = makeStickerLayer(makeRealFrame(QSize(120, 80)),
                                     QPointF(0, 0), QStringLiteral("A"));
    CanvasLayer b = makeStickerLayer(makeRealFrame(QSize(140, 90)),
                                     QPointF(400, 100), QStringLiteral("B"));
    QList<CanvasLayer> layers = doc.layers();
    layers.append(a);
    layers.append(b);
    doc.setLayers(layers);
    const int idxA = 1, idxB = 2;

    /* raster fingerprints BEFORE the collage */
    const QByteArray beforeA = QByteArray::fromRawData(
        reinterpret_cast<const char *>(doc.layerAt(idxA)->raster.constBits()),
        int(doc.layerAt(idxA)->raster.sizeInBytes()));
    const QByteArray beforeB = QByteArray::fromRawData(
        reinterpret_cast<const char *>(doc.layerAt(idxB)->raster.constBits()),
        int(doc.layerAt(idxB)->raster.sizeInBytes()));

    /* the shape of PaintbrushController::autoCollage: one CompositeCommand
     * of per-layer SetTransformCommands, then a single setLayers */
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Auto collage");
    QList<CanvasLayer> work = doc.layers();
    const QPointF slots[2] = { QPointF(50, 50), QPointF(500, 300) };
    for (int i = 0; i < 2; ++i)
    {
        const int idx = i == 0 ? idxA : idxB;
        CanvasLayer &l = work[idx];
        const QTransform beforeT = l.transform();
        QTransform t;
        t.translate(slots[i].x(), slots[i].y());
        t.rotate(i == 1 ? 12.0 : 0.0);
        t.scale(0.75, 0.75);
        l.setTransform(t);
        const QTransform afterT = l.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{l.id, idx, beforeT, afterT, composite->desc}));
    }
    doc.setLayers(work);
    UndoStack undo;
    undo.pushCommand(std::move(composite));

    /* transforms applied, rasters byte-identical (immutable assets) */
    CHECK(doc.layerAt(idxA)->transform().dx() > 0);
    CHECK(qAbs(doc.layerAt(idxB)->rotation - 12.0) < 0.01);
    const QByteArray afterA = QByteArray::fromRawData(
        reinterpret_cast<const char *>(doc.layerAt(idxA)->raster.constBits()),
        int(doc.layerAt(idxA)->raster.sizeInBytes()));
    const QByteArray afterB = QByteArray::fromRawData(
        reinterpret_cast<const char *>(doc.layerAt(idxB)->raster.constBits()),
        int(doc.layerAt(idxB)->raster.sizeInBytes()));
    CHECK(afterA == beforeA);
    CHECK(afterB == beforeB);

    CHECK(undo.undo(doc)); /* one step restores both transforms */
    CHECK(doc.layerAt(idxA)->transform().dx() == 0);
    CHECK(qAbs(doc.layerAt(idxB)->rotation) < 0.01);
    CHECK(undo.redo(doc));
    CHECK(qAbs(doc.layerAt(idxB)->rotation - 12.0) < 0.01);
}

/* ---------------------------------------------------------------- test 7
 * (g) a sticker layer renders through the canonical DocumentRenderer. */
void testStickerRendersCanonically()
{
    PaintbrushDocument doc;
    doc.resetDocument(QSize(320, 240));
    QImage cutout = makeRealFrame(QSize(100, 100)); /* red|blue halves */
    CanvasLayer sticker = makeStickerLayer(cutout, QPointF(110, 70),
                                           QStringLiteral("Sticker"));
    QList<CanvasLayer> layers = doc.layers();
    layers.append(sticker);
    doc.setLayers(layers);

    const QImage flat = DocumentRenderer::render(doc);
    CHECK(!flat.isNull());
    CHECK(flat.size() == QSize(320, 240));
    /* the sticker's red half must land at its transformed position */
    CHECK(qRed(flat.pixel(115, 75)) > 200);   /* red half of the cutout */
    CHECK(qRed(flat.pixel(180, 75)) < 60 &&   /* blue half of the cutout */
          qBlue(flat.pixel(180, 75)) > 200);
    CHECK(qAlpha(flat.pixel(20, 20)) == 0);   /* transparent elsewhere */
}

/* ---------------------------------------------------------------- test 2
 * (d) preview renders REAL pixels — non-uniform from a real frame, flat
 * from the old gray silhouette. */
void testPreviewRealPixels()
{
    const QSize size(120, 90);
    const QImage frame = makeRealFrame(size); /* red | blue */
    const QImage mask = makeMask(size);
    const TrackEffect noEffect;

    /* the controller preview path: stickerPreviewDataUrl(frame, mask, ...) */
    const QString url = stickerPreviewDataUrl(frame, mask, &noEffect, 0,
                                              QColor(), false, 1.0);
    CHECK(!url.isEmpty());
    CHECK(url.startsWith(QStringLiteral("data:image/png;base64,")));
    const QImage decoded = decodeDataUrl(url);
    CHECK(!decoded.isNull());
    /* renderStickerFrame trims to the mask content, so the sticker is
     * never larger than the source frame */
    CHECK(decoded.size().width() <= size.width());
    CHECK(decoded.size().height() <= size.height());

    /* real pixels in: the output carries BOTH frame colors -> variance > 1 */
    CHECK(distinctOpaqueColors(decoded) >= 2);

    /* the OLD behavior (flat gray placeholder frame) is provably flat: this
     * is exactly the A16 defect — the silhouette has zero color variance,
     * so a variance check on the real-pixel path is meaningful. */
    QImage placeholder(size, QImage::Format_ARGB32);
    placeholder.fill(QColor(200, 200, 210));
    const QString flatUrl = stickerPreviewDataUrl(placeholder, mask, &noEffect,
                                                  0, QColor(), false, 1.0);
    const QImage flatDecoded = decodeDataUrl(flatUrl);
    CHECK(!flatDecoded.isNull());
    CHECK(distinctOpaqueColors(flatDecoded) == 1);
}

/* ---------------------------------------------------------------- test 1
 * trimToContent + the sticker frame cache (LRU, byte budget). */
void testTrimAndFrameCache()
{
    QImage big(80, 60, QImage::Format_ARGB32);
    big.fill(Qt::transparent);
    QPainter p(&big);
    p.fillRect(QRect(10, 12, 40, 25), Qt::green);
    p.end();
    QPoint offset;
    const QImage trimmed = trimToContent(big, &offset);
    CHECK(trimmed.size() == QSize(40, 25));
    CHECK(offset == QPoint(10, 12));

    StickerFrameCache cache;
    cache.setByteBudget(1 * 1024 * 1024); /* tiny budget */
    const QImage f1 = makeRealFrame(QSize(64, 64));   /* 16 KiB each */
    const QImage f2 = makeRealFrame(QSize(64, 64));
    const QImage f3 = makeRealFrame(QSize(64, 64));
    cache.put(1, 0, 1000, f1);
    cache.put(1, 1, 2000, f2);
    cache.put(1, 2, 3000, f3);
    StickerFrameCache::Entry e;
    CHECK(cache.get(1, 2, &e) && e.timeMs == 3000);
    CHECK(cache.get(1, 0, &e) && e.timeMs == 1000);
    CHECK(cache.get(1, 1, &e) && e.timeMs == 2000);
    CHECK(cache.bytes() == qint64(3) * qint64(f1.sizeInBytes()));

    cache.put(2, 0, 4000, makeRealFrame(QSize(64, 64))); /* 4th entry */
    cache.setByteBudget(2 * qint64(f1.sizeInBytes()));   /* keep 2 */
    /* LRU eviction drops from the back: order after the touches above is
     * 2/0, 1/1, 1/0, 1/2 — so 1/2 and 1/0 go first, 1/1 and 2/0 stay */
    CHECK(!cache.get(1, 0, nullptr));
    CHECK(!cache.get(1, 2, nullptr));
    CHECK(cache.get(1, 1, &e) && e.timeMs == 2000);
    CHECK(cache.bytes() <= 2 * qint64(f1.sizeInBytes()));
    CHECK(cache.get(2, 0, &e) && e.timeMs == 4000);

    cache.remove(2, 0);
    CHECK(!cache.get(2, 0, nullptr));
}

/* ---------------------------------------------------------- static checks
 * (c)+(e): read the LIVE module sources and assert the A15/A17 fixes. */
void testStaticSourceAssertions(const QString &root)
{
    const QString ctrlPath = root +
        QStringLiteral("/modules/gui/qt/paintbrush/pb_controller.cpp");
    const QString qmlPath = root +
        QStringLiteral("/modules/gui/qt/paintbrush/qml/PaintbrushMediaDrawer.qml");

    QFile ctrl(ctrlPath);
    QFile qml(qmlPath);
    CHECK(ctrl.open(QIODevice::ReadOnly));
    CHECK(qml.open(QIODevice::ReadOnly));
    if (!ctrl.isOpen() || !qml.isOpen())
        return;
    const QString ctrlText = QString::fromUtf8(ctrl.readAll());
    const QString qmlText = QString::fromUtf8(qml.readAll());

    /* (e) no UI-thread sleep remains in the controller at all */
    CHECK(!ctrlText.contains(QStringLiteral("QThread::msleep")));
    CHECK(!ctrlText.contains(QStringLiteral("QElapsedTimer")));

    /* (c) no hardcoded track 0 in the drawer; it binds to the controller's
     * SelectedTrack state instead */
    CHECK(!qmlText.contains(QStringLiteral("stickerEditor.trackId = 0")));
    CHECK(qmlText.contains(QStringLiteral("selectedTrackId")));
    CHECK(ctrlText.contains(QStringLiteral("selectedTrackId")));
    CHECK(ctrlText.contains(QStringLiteral("Q_PROPERTY(int selectedTrackId")));

    /* (a) extractObject must not reset the document */
    const QString marker = QStringLiteral("void PaintbrushController::extractObject");
    const int start = ctrlText.indexOf(marker);
    CHECK(start >= 0);
    if (start >= 0)
    {
        const int next = ctrlText.indexOf(
            QStringLiteral("void PaintbrushController::"), start + 1);
        const QString body = ctrlText.mid(
            start, next < 0 ? -1 : next - start);
        CHECK(!body.contains(QStringLiteral("resetDocument")));
        CHECK(body.contains(QStringLiteral("insertStickerLayer")));
    }

    /* the A20 insert path is typed: InsertLayerCommand inside
     * insertStickerLayer */
    const int insStart = ctrlText.indexOf(
        QStringLiteral("void PaintbrushController::insertStickerLayer"));
    CHECK(insStart >= 0);
    if (insStart >= 0)
    {
        const int insEnd = ctrlText.indexOf(
            QStringLiteral("void PaintbrushController::"), insStart + 1);
        const QString body = ctrlText.mid(
            insStart, insEnd < 0 ? -1 : insEnd - insStart);
        CHECK(body.contains(QStringLiteral("InsertLayerCommand")));
        CHECK(body.contains(QStringLiteral("LAYER_STICKER")));
    }
}

} // namespace

int main(int argc, char **argv)
{
    Q_UNUSED(argc);
    QString root = QStringLiteral(".");
    if (argc > 1)
        root = QString::fromLocal8Bit(argv[1]);

    testTrimAndFrameCache();
    testPreviewRealPixels();
    testExtractInsertsIntoExistingDocument();
    testCollageIsCommandsOnly();
    testStickerRendersCanonically();
    testStaticSourceAssertions(root);

    std::printf("s4 harness: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}