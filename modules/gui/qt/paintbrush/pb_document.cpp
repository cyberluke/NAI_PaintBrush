/*****************************************************************************
 * pb_document.cpp : NAI Paintbrush — document model implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_document.hpp"
#include "pb_history.hpp"
#include "art/style/mood.hpp"
#include "art/style/typography_style.hpp"
#include "art/surface/paper_renderer.hpp"
#include "art/materials/glitter/glitter_material.hpp"

#include <QPainter>
#include <QtMath>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>

namespace paintbrush {

PaintbrushDocument::PaintbrushDocument(QObject *parent)
    : QObject(parent)
{
    /* A fresh document is crafted paper, not a white rectangle (spec §8): the
     * default mood supplies paper, light and seed before any layer exists. */
    const art::Mood &m = art::mood::defaultMood();
    m_paperStockId = m.paperId;
    m_lightRig = m.light;
    m_moodId = m.id;
    m_artSeed = QRandomGenerator::global()->generate() | 1u;
    /* Living Harvest: rigs rebuild deterministically from the tray */
    m_puppets.setRigBuilder([this](int harvestId) {
        const HarvestItem *h = m_harvest.itemById(harvestId);
        return h ? motion::PuppetRuntime::build(h->cutout, h->mask) : motion::PuppetRig();
    });
}

bool PaintbrushDocument::hasVisibleLivingLayer() const
{
    for (const CanvasLayer &l : m_layers)
        if (l.visible && l.hasPuppet() && m_puppets.byId(l.puppetAssetId))
            return true;
    return false;
}

int PaintbrushDocument::prunePuppetRefs()
{
    int cleared = 0;
    for (CanvasLayer &l : m_layers) {
        if (l.puppetAssetId > 0 && !m_puppets.byId(l.puppetAssetId)) {
            l.puppetAssetId = -1;
            l.puppetEffect = motion::PuppetEffect();
            ++cleared;
        }
    }
    if (cleared)
        emit layersChanged();
    return cleared;
}

/* -------------------------------------------------------- art direction */

void PaintbrushDocument::setPaperStockId(const QString &id)
{
    if (m_paperStockId == id)
        return;
    m_paperStockId = id;
    m_dirty = true;
    emit artStateChanged();
    emit layersChanged();
    emit dirtyChanged(true);
}

const art::PaperStock *PaintbrushDocument::paperStock() const
{
    if (m_paperStockId.isEmpty())
        return nullptr;
    return art::paper::findById(m_paperStockId);
}

void PaintbrushDocument::setLightRig(const art::ArtLightRig &rig)
{
    m_lightRig = rig;
    m_dirty = true;
    emit artStateChanged();
    emit layersChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setMoodId(const QString &id)
{
    if (m_moodId == id)
        return;
    m_moodId = id;
    m_dirty = true;
    emit artStateChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setArtSeed(quint32 seed)
{
    m_artSeed = seed == 0 ? 1u : seed;
    emit artStateChanged();
}

art::ArtScale PaintbrushDocument::artScale(float exportScale) const
{
    art::ArtScale s;
    s.referenceDpi = 96.0f;
    s.worldUnitsPerInch = 96.0f;   /* canvas pixel == world unit at 1x */
    s.devicePixelRatio = 1.0f;
    s.exportScale = exportScale > 0.0f ? exportScale : 1.0f;
    return s;
}

void PaintbrushDocument::resetDocument(const QSize &size)
{
    m_canvasSize = size.isValid() && !size.isEmpty() ? size : QSize(1280, 720);
    m_layers.clear();
    m_nextLayerId = 1;
    m_currentLayerIndex = -1;
    m_selection = QPainterPath();
    m_hasSelection = false;
    m_sourceMRL.clear();
    m_sourceTitle.clear();
    m_sourceTimeMs = 0;
    m_tracks.clear();
    m_projectPath.clear();
    m_dirty = false;
    m_harvest.clear();
    {
        const art::Mood &m = art::mood::defaultMood();
        m_paperStockId = m.paperId;
        m_lightRig = m.light;
        m_moodId = m.id;
        m_artSeed = QRandomGenerator::global()->generate() | 1u;
    }

    /* Start with one empty raster layer, like Paintbrush does. */
    m_layers.append(CanvasLayer::createRaster(m_canvasSize, QStringLiteral("Layer 1")));
    m_layers[0].id = m_nextLayerId++;
    m_currentLayerIndex = 0;

    emit artStateChanged();
    emit canvasSizeChanged();
    emit layersChanged();
    emit currentLayerChanged(m_currentLayerIndex);
    emit selectionChanged();
    emit mediaChanged();
    emit tracksChanged();
    emit dirtyChanged(false);
    emit documentReset();
}

void PaintbrushDocument::setCanvasSize(const QSize &size)
{
    if (size == m_canvasSize || !size.isValid() || size.isEmpty())
        return;

    /* renderTarget only: layer rasters stay LOCAL-sized. A layer's placement
     * transform maps local content onto the (possibly resized) canvas. */
    m_canvasSize = size;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::assignLayerIds()
{
    /* ids loaded from a project are kept (stable across reopen); the
     * allocator always moves past the largest id in use so fresh ids never
     * collide with restored ones */
    for (const CanvasLayer &layer : m_layers)
        if (layer.id >= m_nextLayerId)
            m_nextLayerId = layer.id + 1;
    for (CanvasLayer &layer : m_layers)
        if (layer.id == 0)
            layer.id = m_nextLayerId++;
}

void PaintbrushDocument::setLayers(const QList<CanvasLayer> &layers)
{
    m_layers = layers;
    assignLayerIds();
    if (m_currentLayerIndex >= m_layers.size())
        m_currentLayerIndex = m_layers.isEmpty() ? -1 : m_layers.size() - 1;
    m_dirty = true;
    emit layersChanged();
    emit currentLayerChanged(m_currentLayerIndex);
    emit dirtyChanged(true);
}

void PaintbrushDocument::setCurrentLayerIndex(int index)
{
    if (index < -1 || index >= m_layers.size() || index == m_currentLayerIndex)
        return;
    m_currentLayerIndex = index;
    emit currentLayerChanged(index);
}

CanvasLayer *PaintbrushDocument::currentLayer()
{
    return layerAt(m_currentLayerIndex);
}

const CanvasLayer *PaintbrushDocument::currentLayer() const
{
    return layerAt(m_currentLayerIndex);
}

CanvasLayer *PaintbrushDocument::layerAt(int index)
{
    if (index < 0 || index >= m_layers.size())
        return nullptr;
    return &m_layers[index];
}

const CanvasLayer *PaintbrushDocument::layerAt(int index) const
{
    if (index < 0 || index >= m_layers.size())
        return nullptr;
    return &m_layers[index];
}

int PaintbrushDocument::indexOfLayerId(int id) const
{
    for (int i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].id == id)
            return i;
    return -1;
}

CanvasLayer *PaintbrushDocument::layerById(int id)
{
    const int index = indexOfLayerId(id);
    return index < 0 ? nullptr : &m_layers[index];
}

const CanvasLayer *PaintbrushDocument::layerById(int id) const
{
    const int index = indexOfLayerId(id);
    return index < 0 ? nullptr : &m_layers[index];
}

void PaintbrushDocument::setSelectionPath(const QPainterPath &path)
{
    m_selection = path;
    m_hasSelection = !path.isEmpty();
    emit selectionChanged();
}

void PaintbrushDocument::clearSelection()
{
    if (!m_hasSelection && m_selection.isEmpty())
        return;
    m_selection = QPainterPath();
    m_hasSelection = false;
    emit selectionChanged();
}

void PaintbrushDocument::setSourceMedia(const QString &mrl, const QString &title,
                                        qint64 timeMs)
{
    if (m_sourceMRL == mrl && m_sourceTitle == title && m_sourceTimeMs == timeMs)
        return;
    m_sourceMRL = mrl;
    m_sourceTitle = title;
    m_sourceTimeMs = timeMs;
    /* keep the exact-frame identity in sync (A19) */
    m_sourceFrame.mrl = mrl;
    m_sourceFrame.title = title;
    m_sourceFrame.ptsMs = qMax<qint64>(0, timeMs);
    m_dirty = true;
    emit mediaChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setSourceFrame(const FrameRef &ref)
{
    m_sourceFrame = ref;
    m_sourceMRL = ref.mrl;
    m_sourceTitle = ref.title;
    m_sourceTimeMs = qMax<qint64>(0, ref.ptsMs);
    m_dirty = true;
    emit mediaChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::clearSourceMedia()
{
    setSourceMedia({}, {}, 0);
}

void PaintbrushDocument::setTracks(const QVector<ObjectTrack> &tracks)
{
    m_tracks = tracks;
    m_dirty = true;
    emit tracksChanged();
    emit dirtyChanged(true);
}

void PaintbrushDocument::setProjectPath(const QString &path)
{
    if (m_projectPath == path)
        return;
    m_projectPath = path;
}

void PaintbrushDocument::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(dirty);
}

/* ------------------------------------------------------ canvas transforms */

QTransform pbCanvasRotateTransform(const QSize &oldCanvasSize, bool clockwise)
{
    const QPointF cOld(oldCanvasSize.width() / 2.0, oldCanvasSize.height() / 2.0);
    const QPointF cNew(oldCanvasSize.height() / 2.0, oldCanvasSize.width() / 2.0);
    QTransform t;
    t.translate(cNew.x(), cNew.y());
    t.rotate(clockwise ? 90 : -90);
    t.translate(-cOld.x(), -cOld.y());
    return t;
}

QTransform pbCanvasCropShift(const QRect &cropRect)
{
    QTransform t;
    t.translate(-cropRect.left(), -cropRect.top());
    return t;
}

std::unique_ptr<UndoCommand> PaintbrushDocument::makeRotate90Command(bool clockwise)
{
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Rotate");

    const QSize oldSize = m_canvasSize;
    const QSize newSize(oldSize.height(), oldSize.width());
    composite->children.push_back(std::make_unique<CanvasResizeCommand>(
        CanvasResizeCommand{oldSize, newSize, composite->desc}));

    const QTransform rot = pbCanvasRotateTransform(oldSize, clockwise);
    for (int i = 0; i < m_layers.size(); ++i)
    {
        CanvasLayer &layer = m_layers[i];
        if (layer.raster.isNull())
            continue;
        const QTransform before = layer.transform();
        layer.setTransform(rot * before);
        const QTransform after = layer.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{layer.id, i, before, after, composite->desc}));
    }

    m_canvasSize = newSize;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
    return composite;
}

std::unique_ptr<UndoCommand> PaintbrushDocument::makeCropCommand(const QRect &rect)
{
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Crop");

    const QSize oldSize = m_canvasSize;
    const QSize newSize = rect.size();
    composite->children.push_back(std::make_unique<CanvasResizeCommand>(
        CanvasResizeCommand{oldSize, newSize, composite->desc}));

    const QTransform shift = pbCanvasCropShift(rect);
    for (int i = 0; i < m_layers.size(); ++i)
    {
        CanvasLayer &layer = m_layers[i];
        if (layer.raster.isNull())
            continue;
        const QTransform before = layer.transform();
        layer.setTransform(shift * before);
        const QTransform after = layer.transform();
        composite->children.push_back(std::make_unique<SetTransformCommand>(
            SetTransformCommand{layer.id, i, before, after, composite->desc}));
    }

    m_canvasSize = newSize;
    m_dirty = true;
    emit canvasSizeChanged();
    emit layersChanged();
    emit dirtyChanged(true);
    return composite;
}

/* ------------------------------------------------------- canonical render */

/* One layer through the canonical transform. `region` is the canvas region
 * whose top-left corner sits at target pixel (0,0). This is the raw pixel
 * path: `layer.raster` is drawn as-is (effects already resolved by the
 * context-aware compositeLayer below). */
static void compositeLayerRaw(QImage &target, const QRect &region,
                              const CanvasLayer &layer)
{
    if (layer.raster.isNull() || !layer.visible)
        return;

    const QTransform tf = layer.transform();

    /* exact path: pure integer translation (the default for raster layers
     * and untransformed image/text layers) — blit the source sub-rect, no
     * resampling, pixel-identical to the old fast path */
    const bool exact = tf.type() <= QTransform::TxTranslate &&
                       tf.dx() == qreal(int(tf.dx())) &&
                       tf.dy() == qreal(int(tf.dy()));

    if (exact)
    {
        const QPoint off(int(tf.dx()), int(tf.dy()));
        /* the region expressed in LAYER-LOCAL coordinates */
        const QRect srcRect = region.translated(-off);
        const QRect layerRect = layer.raster.rect();
        /* the visible part of the layer, still in local coordinates */
        const QRect validSrc = srcRect.intersected(layerRect);
        if (validSrc.isEmpty())
            return;

        /* back to canvas space (+off), then into target space (-region) */
        const QRect dstRect = validSrc.translated(off - region.topLeft());
        const QRect srcLocal = validSrc;

        QImage layerPart(dstRect.size(), QImage::Format_ARGB32);
        layerPart.fill(Qt::transparent);
        {
            QPainter p(&layerPart);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(QPoint(0, 0), layer.raster, srcLocal);
            p.end();
        }

        if (!layer.mask.isNull())
        {
            QImage mask = layer.mask.scaled(layer.raster.size(),
                                            Qt::IgnoreAspectRatio,
                                            Qt::FastTransformation);
            QPainter p(&layerPart);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(QPoint(0, 0), maskToImage(mask), srcLocal);
            p.end();
        }

        QPainter p(&target);
        p.setOpacity(layer.opacity);
        p.setCompositionMode(toCompositionMode(layer.blendMode));
        p.drawImage(dstRect.topLeft(), layerPart);
        p.end();
        return;
    }

    /* transformed path: render the whole local content once with the
     * canonical center-pivot transform, then composite the part that lands
     * in this region */
    const QRect layerRect = layer.raster.rect();
    const QPointF contentCenter(layerRect.center());
    QImage layerPart(layerRect.size(), QImage::Format_ARGB32);
    layerPart.fill(Qt::transparent);
    {
        QPainter p(&layerPart);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), layer.raster);
        if (!layer.mask.isNull())
        {
            QImage mask = layer.mask.scaled(layerRect.size(),
                                            Qt::IgnoreAspectRatio,
                                            Qt::FastTransformation);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(QPoint(0, 0), maskToImage(mask));
        }
        p.end();
    }

    /* transform around the content center — identical semantics to the
     * tile compositor, so screen and export cannot diverge */
    QTransform tf2;
    tf2.translate(contentCenter.x(), contentCenter.y());
    tf2.rotate(layer.rotation);
    tf2.scale(layer.scale, layer.scale);
    tf2.translate(-contentCenter.x(), -contentCenter.y());

    QImage drawn = layerPart.transformed(tf2, Qt::SmoothTransformation);

    /* The transformed image's (0,0) is the BOUNDING-BOX top-left of the
     * transformed content, not the transformed origin. Compute that offset
     * so the content lands exactly where layer.transform() maps it —
     * otherwise the placement disagrees with transform() by the bbox
     * shift (screen, export and the transform math would diverge). */
    QPointF bboxTL = tf2.map(QPointF(0, 0));
    const QPointF c1 = tf2.map(QPointF(layerRect.width(), 0));
    const QPointF c2 = tf2.map(QPointF(0, layerRect.height()));
    const QPointF c3 = tf2.map(QPointF(layerRect.width(), layerRect.height()));
    bboxTL.setX(qMin(qMin(bboxTL.x(), c1.x()), qMin(c2.x(), c3.x())));
    bboxTL.setY(qMin(qMin(bboxTL.y(), c1.y()), qMin(c2.y(), c3.y())));

    const QPointF drawAt = layer.position + bboxTL;

    const QRect drawRect(drawAt.toPoint(), drawn.size());
    const QRect visible = drawRect.intersected(region);
    if (visible.isEmpty())
        return;

    QImage tilePart(visible.size(), QImage::Format_ARGB32);
    tilePart.fill(Qt::transparent);
    {
        QPainter p(&tilePart);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), drawn, visible.translated(-drawRect.topLeft()));
        p.end();
    }

    QPainter p(&target);
    p.setOpacity(layer.opacity);
    p.setCompositionMode(toCompositionMode(layer.blendMode));
    p.drawImage(visible.topLeft() - region.topLeft(), tilePart);
    p.end();
}

/* ------------------------------------------------- art-aware compositing */

DocumentRenderer::Context DocumentRenderer::contextFor(const PaintbrushDocument &doc,
                                                       float exportScale)
{
    Context ctx;
    ctx.scale = doc.artScale(exportScale);
    ctx.rig = doc.lightRig();
    ctx.paper = doc.paperStock();
    ctx.paintPaper = ctx.paper != nullptr;
    return ctx;
}

QImage DocumentRenderer::effectiveSource(const CanvasLayer &layer, const Context &ctx,
                                         QPoint *originOffset)
{
    return effectiveSourceAt(layer, ctx, originOffset, nullptr, 0);
}

QImage DocumentRenderer::effectiveSourceAt(const CanvasLayer &layer, const Context &ctx,
                                           QPoint *originOffset,
                                           const PaintbrushDocument *doc, qint64 timeMs)
{
    if (originOffset)
        *originOffset = QPoint(0, 0);
    if (layer.raster.isNull())
        return QImage();

    const float s = ctx.scale.exportScale;
    const bool scaled = !qFuzzyCompare(s, 1.0f);
    QImage src;

    /* Living Harvest (stage 3): the layer's motion replaces the static
     * raster at render time — the raster itself is never modified. The
     * puppet frame is cutout-sized and flows through the same scale/mask/
     * finish path as any other source. */
    if (layer.hasPuppet() && doc) {
        const motion::PuppetAsset *asset = doc->puppets()->byId(layer.puppetAssetId);
        if (asset)
            src = motion::PuppetRuntime::renderAtTime(*asset, layer.puppetEffect, timeMs);
    }

    /* 1) source pixels at the context scale */
    if (scaled && !layer.materialState.isEmpty() &&
        layer.materialPreset.startsWith(QLatin1String("material.glitter")))
    {
        /* re-render the material from its state in world units so flakes keep
         * their apparent size at export (spec §11, §55) */
        art::glitter::GlitterState st;
        if (st.deserialize(layer.materialState))
        {
            const QSize out(qMax(1, int(std::lround(layer.raster.width() * s))),
                            qMax(1, int(std::lround(layer.raster.height() * s))));
            src = st.render(out, 0.0f, 0.0f, 1.0f / s, ctx.rig, ctx.paper, ctx.scale);
        }
    }
    if (src.isNull() && scaled && layer.type == LAYER_TEXT)
    {
        /* editable typography (spec §57): text layers re-render their glyphs
         * from the stored treatment at export scale — a crisp vector-style
         * render, never a bitmap upscale of the 1x raster. Fall back to the
         * raster path when no treatment is stored (legacy documents). */
        const QVariant v = layer.metadata.value(QStringLiteral("textTreatment"));
        if (v.isValid())
        {
            const art::TextTreatment t = art::TextTreatment::fromVariant(v.toMap());
            src = art::typography::render(t, s);
        }
    }
    if (src.isNull())
    {
        src = layer.raster.convertToFormat(QImage::Format_ARGB32);
        if (scaled)
            src = src.scaled(qMax(1, int(std::lround(src.width() * s))),
                             qMax(1, int(std::lround(src.height() * s))),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    /* 2) local-space mask */
    if (!layer.mask.isNull())
    {
        QImage mask = layer.mask.scaled(src.size(), Qt::IgnoreAspectRatio,
                                        Qt::FastTransformation);
        QPainter p(&src);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(QPoint(0, 0), maskToImage(mask));
        p.end();
    }

    /* 3) non-destructive finish chain */
    if (layer.hasEnabledEffects())
    {
        const art::FinishResult fr = art::FinishRuntime::apply(src, layer.effects,
                                                               ctx.rig, ctx.scale);
        src = fr.image.convertToFormat(QImage::Format_ARGB32);
        if (originOffset)
            *originOffset = fr.originOffset;
    }
    return src;
}

void DocumentRenderer::compositeLayer(QImage &target, const QRect &region,
                                      const CanvasLayer &layer, const Context &ctx)
{
    if (layer.raster.isNull() || !layer.visible)
        return;
    if (!layer.hasEnabledEffects())
    {
        compositeLayerRaw(target, region, layer);
        return;
    }
    /* Resolve the finish chain into an expanded raster and composite it with
     * the SAME placement transform. The expanded image's local origin sits
     * at `off` (<= 0) relative to the raster origin and its pivot is shifted
     * by the same padding, so shifting `position` by `off` reproduces the
     * exact canvas placement for any rotation/scale (see transform()). */
    QPoint off;
    QImage src = effectiveSource(layer, ctx, &off);
    CanvasLayer tmp = layer;
    tmp.raster = src;
    tmp.mask = QImage();
    tmp.effects.clear();
    tmp.position = layer.position + QPointF(off);
    compositeLayerRaw(target, region, tmp);
}

void DocumentRenderer::compositeLayer(QImage &target, const QRect &region,
                                      const CanvasLayer &layer)
{
    Context ctx;
    ctx.paintPaper = false;
    compositeLayer(target, region, layer, ctx);
}

void DocumentRenderer::compositeLayerAt(QImage &target, const QRect &region,
                                        const CanvasLayer &layer,
                                        const Context &ctx,
                                        const PaintbrushDocument &doc, qint64 timeMs)
{
    if (layer.raster.isNull() || !layer.visible)
        return;
    if (!layer.hasEnabledEffects() && !layer.hasPuppet())
    {
        compositeLayerRaw(target, region, layer);
        return;
    }
    QPoint off;
    QImage src = effectiveSourceAt(layer, ctx, &off, &doc, timeMs);
    CanvasLayer tmp = layer;
    tmp.raster = src;
    tmp.mask = QImage();
    tmp.effects.clear();
    tmp.position = layer.position + QPointF(off);
    compositeLayerRaw(target, region, tmp);
}

namespace {

/* Single-entry paper cache: the full-canvas paper at one (stock, seed, size,
 * scale) is computed once and region draws copy from it. Procedural paper is
 * deterministic, so the cache can never go stale for the same key. */
struct PaperCache
{
    QMutex mutex;
    QString stockId;
    quint32 seed = 0;
    QSize size;
    float totalScale = 0.0f;
    float ambient = -1.0f;
    QImage image;
};

PaperCache &paperCache()
{
    static PaperCache c;
    return c;
}

} // namespace

void DocumentRenderer::paintPaper(QImage &target, const QRect &region, const Context &ctx)
{
    if (!ctx.paintPaper || !ctx.paper || region.isEmpty())
        return;
    const QSize full(region.right() + 1, region.bottom() + 1);
    PaperCache &cache = paperCache();
    QMutexLocker lock(&cache.mutex);
    const float total = ctx.scale.totalScale();
    if (cache.stockId != ctx.paper->id || cache.seed != ctx.paper->seed ||
        cache.totalScale != total || cache.image.isNull() ||
        cache.size.width() < full.width() || cache.size.height() < full.height() ||
        !qFuzzyCompare(cache.ambient, ctx.rig.ambient))
    {
        const QSize want(qMax(full.width(), cache.size.width()),
                         qMax(full.height(), cache.size.height()));
        art::PaperRenderer renderer;
        art::PaperRenderer::State st;
        st.stock = ctx.paper;
        st.rig = &ctx.rig;
        st.scale = ctx.scale;
        st.worldViewX = 0.0f;
        st.worldViewY = 0.0f;
        cache.image = renderer.render(want, st, QRect(QPoint(0, 0), want));
        cache.stockId = ctx.paper->id;
        cache.seed = ctx.paper->seed;
        cache.size = want;
        cache.totalScale = total;
        cache.ambient = ctx.rig.ambient;
    }
    QPainter p(&target);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(QPoint(0, 0), cache.image, region);
    p.end();
}

void DocumentRenderer::composite(QImage &target, const QRect &region,
                                 const PaintbrushDocument &doc)
{
    composite(target, region, doc, 0);
}

void DocumentRenderer::composite(QImage &target, const QRect &region,
                                 const PaintbrushDocument &doc, qint64 timeMs)
{
    const Context ctx = contextFor(doc, 1.0f);
    paintPaper(target, region, ctx);
    const auto &layers = doc.layers();
    for (const CanvasLayer &layer : layers)
        compositeLayerAt(target, region, layer, ctx, doc, timeMs);
}

QImage DocumentRenderer::render(const PaintbrushDocument &doc)
{
    return render(doc, 0);
}

QImage DocumentRenderer::render(const PaintbrushDocument &doc, qint64 timeMs)
{
    const QSize size = doc.canvasSize();
    if (size.isEmpty())
        return {};
    return renderRegion(doc, QRect(QPoint(0, 0), size), timeMs);
}

QImage DocumentRenderer::renderRegion(const PaintbrushDocument &doc,
                                      const QRect &region)
{
    return renderRegion(doc, region, 0);
}

QImage DocumentRenderer::renderRegion(const PaintbrushDocument &doc,
                                      const QRect &region, qint64 timeMs)
{
    if (region.isEmpty())
        return {};
    QImage result(region.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    composite(result, region, doc, timeMs);
    return result;
}

QImage DocumentRenderer::renderScaled(const PaintbrushDocument &doc, float scale)
{
    return renderScaled(doc, scale, 0);
}

namespace {

/* Shared export-scale render: paper on/off (living export needs transparent
 * frames) and the canonical time for Living Harvest layers. */
QImage renderScaledFrame(const PaintbrushDocument &doc, float scale,
                         qint64 timeMs, bool paintPaper)
{
    if (scale <= 0.0f || qFuzzyCompare(scale, 1.0f))
        return DocumentRenderer::render(doc, timeMs);
    const QSize canvas = doc.canvasSize();
    if (canvas.isEmpty())
        return {};
    const QSize out(qMax(1, int(std::lround(canvas.width() * scale))),
                    qMax(1, int(std::lround(canvas.height() * scale))));
    QImage result(out, QImage::Format_ARGB32);
    result.fill(Qt::transparent);

    const DocumentRenderer::Context ctx = DocumentRenderer::contextFor(doc, scale);
    if (paintPaper)
        DocumentRenderer::paintPaper(result, QRect(QPoint(0, 0), out), ctx);

    for (const CanvasLayer &layer : doc.layers())
    {
        if (!layer.visible || layer.raster.isNull())
            continue;
        QPoint off;
        const QImage src = DocumentRenderer::effectiveSourceAt(layer, ctx, &off, &doc, timeMs);
        if (src.isNull())
            continue;
        /* src pixel -> (translate off) -> local/scale -> layer transform ->
         * canvas * scale. Qt composes left-to-right. */
        const QTransform full =
            QTransform::fromTranslate(off.x(), off.y()) *
            QTransform::fromScale(1.0 / scale, 1.0 / scale) *
            layer.transform() *
            QTransform::fromScale(scale, scale);
        QPainter p(&result);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(layer.opacity);
        p.setCompositionMode(toCompositionMode(layer.blendMode));
        p.setTransform(full);
        p.drawImage(QPoint(0, 0), src);
        p.end();
    }
    return result;
}

} // namespace

QImage DocumentRenderer::renderScaled(const PaintbrushDocument &doc, float scale,
                                      qint64 timeMs)
{
    return renderScaledFrame(doc, scale, timeMs, true);
}

QImage DocumentRenderer::renderTransparentAt(const PaintbrushDocument &doc, float scale,
                                             qint64 timeMs)
{
    return renderScaledFrame(doc, scale, timeMs, false);
}

QImage PaintbrushDocument::flattenedImage() const
{
    /* thin wrapper over the canonical renderer — identical transform
     * semantics as the screen/export/eyedropper paths */
    return DocumentRenderer::render(*this);
}

QImage PaintbrushDocument::aiSourceImage() const
{
    /* Prefer the captured media frame as the AI source so object selection
     * and ask-about-this operate on the true video pixels. */
    return flattenedImage();
}

} // namespace paintbrush