/*****************************************************************************
 * pb_document.hpp : NAI Paintbrush — document model
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_DOCUMENT_HPP
#define PB_DOCUMENT_HPP

#include "pb_layer.hpp"
#include "pb_media_logic.hpp"
#include "pb_harvest.hpp"
#include "motion/pb_living.hpp"
#include "art/runtime/art_scale.hpp"
#include "art/runtime/light_rig.hpp"
#include "art/surface/paper_stock.hpp"

#include <QObject>
#include <QSize>
#include <QPainterPath>
#include <QImage>
#include <QList>
#include <QVector>
#include <QStringList>
#include <QTransform>
#include <memory>

#include <vlc_common.h>

class UndoStack;

namespace paintbrush {

/* A tracked object: keyframe masks over a media timeline. Masks are
 * evictable (the PNG-encoded copy can be dropped while a source ref is
 * kept) but for the integrated build we keep them in memory, capped by
 * TrackingService. */
struct TrackKeyframe
{
    qint64 timeMs = 0;      /* media timestamp (ms) of the frame */
    int frameIndex = 0;     /* sequential index within the track  */
    QImage mask;            /* binary mask for this frame         */
    float confidence = 0.f; /* propagation confidence if known     */
    bool corrected = false; /* user corrected this keyframe        */
};

/* Visual effect applied to a tracked object when rendered (sticker preview,
 * export and canvas placement). Honest subset of common compositing effects;
 * "attach" composites the cutout back over the source frame at the mask
 * position. */
struct TrackEffect
{
    enum Type
    {
        None = 0,
        Blur,       /* soften the cutout edges/pixels            */
        Dim,        /* darken the cutout                         */
        Spotlight,  /* brighten the cutout, dim nothing else     */
        Outline,    /* colored contour around the cutout         */
        Attach,     /* composite cutout over the source frame    */
    };
    int type = None;
    int intensity = 50;   /* 0..100: blur radius / dim factor / spotlight boost */
    QString color;        /* outline color (QColor name), when applicable       */
};

struct ObjectTrack
{
    int id = 0;
    QString sourceMRL;
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString label;                        /* initial text prompt, if any  */
    QVector<TrackKeyframe> keyframes;
    TrackEffect effect;
    bool complete = false;
    bool cancelled = false;
    QString error;
};

/* A single assistant conversation message. */
struct AssistantMessage
{
    QString role;      /* "user" or "assistant" */
    QString text;
    qint64 timeMs = 0;
};

class PaintbrushDocument;
class UndoCommand;

/* The single canonical compositor. Screen tiles, export, eyedropper, AI
 * preview and thumbnails all render through here, so every consumer sees
 * the same picture with the same layer transforms. */
class DocumentRenderer
{
public:
    /* Art-runtime context shared by every layer of one render: the world
     * scale (1x screen, Nx export), the document light rig and paper. One
     * context per render keeps shadows, glitter and paper lit identically
     * (spec §12, §38). */
    struct Context
    {
        art::ArtScale scale;
        art::ArtLightRig rig;
        const art::PaperStock *paper = nullptr;
        bool paintPaper = true;
    };
    static Context contextFor(const PaintbrushDocument &doc, float exportScale = 1.0f);

    /* Composite one layer into `target` for the canvas region whose
     * top-left corner is `region.topLeft()` (target is region-sized).
     * Honors the full layer transform (position + center-pivot scale +
     * rotation), the local-space mask, opacity and blend mode, and the
     * layer's non-destructive effect chain. */
    static void compositeLayer(QImage &target, const QRect &region,
                               const CanvasLayer &layer);
    static void compositeLayer(QImage &target, const QRect &region,
                               const CanvasLayer &layer, const Context &ctx);
    /* Time-aware composite: a Living Harvest layer renders its puppet frame
     * at `timeMs` instead of the static raster (the canonical time path). */
    static void compositeLayerAt(QImage &target, const QRect &region,
                                 const CanvasLayer &layer, const Context &ctx,
                                 const PaintbrushDocument &doc, qint64 timeMs);

    /* The layer's effective source pixels: the raster (masked) with the
     * finish chain applied, plus the local-space offset of that image's
     * origin relative to the raster origin (<= 0 when effects pad). A
     * material layer with state is re-rendered at ctx.scale. */
    static QImage effectiveSource(const CanvasLayer &layer, const Context &ctx,
                                  QPoint *originOffset);
    /* Time-aware variant (Living Harvest layers); `doc` may be null for the
     * plain (static) path. */
    static QImage effectiveSourceAt(const CanvasLayer &layer, const Context &ctx,
                                    QPoint *originOffset,
                                    const PaintbrushDocument *doc, qint64 timeMs);

    /* Paint the document paper (PaperStock) into `target` for `region`. */
    static void paintPaper(QImage &target, const QRect &region, const Context &ctx);

    /* Composite paper + all visible layers of `doc` bottom-to-top. */
    static void composite(QImage &target, const QRect &region,
                          const PaintbrushDocument &doc);
    static void composite(QImage &target, const QRect &region,
                          const PaintbrushDocument &doc, qint64 timeMs);

    /* Full-canvas render (the canonical flatten). */
    static QImage render(const PaintbrushDocument &doc);
    static QImage render(const PaintbrushDocument &doc, qint64 timeMs);

    /* Render `region` (canvas coordinates) into a region-sized image. */
    static QImage renderRegion(const PaintbrushDocument &doc,
                               const QRect &region);
    static QImage renderRegion(const PaintbrushDocument &doc,
                               const QRect &region, qint64 timeMs);

    /* Export-resolution render (spec §55): the canvas at `scale` with paper,
     * material state and finish chains re-evaluated in world units (never a
     * bitmap upscale of the 1x flatten). scale == 1 is identical to render(). */
    static QImage renderScaled(const PaintbrushDocument &doc, float scale);
    static QImage renderScaled(const PaintbrushDocument &doc, float scale,
                               qint64 timeMs);
    /* Transparent-frame variant (living export without paper). */
    static QImage renderTransparentAt(const PaintbrushDocument &doc, float scale,
                                      qint64 timeMs);
};

/* Canvas-space transform helpers shared by the document-level rotate/crop
 * operations and their typed undo commands. */

/* Rotation that maps the old canvas rect exactly onto a canvas of swapped
 * dimensions (pivot = old canvas center, target center = new canvas
 * center). clockwise = true rotates 90° visually. */
QTransform pbCanvasRotateTransform(const QSize &oldCanvasSize, bool clockwise);

/* Translation that maps old-canvas point `cropRect.topLeft()` onto the new
 * canvas origin (crop). */
QTransform pbCanvasCropShift(const QRect &cropRect);

class PaintbrushDocument : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY canvasSizeChanged FINAL)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY canvasSizeChanged FINAL)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged FINAL)
    Q_PROPERTY(int currentLayerIndex READ currentLayerIndex WRITE setCurrentLayerIndex NOTIFY currentLayerChanged FINAL)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY tracksChanged FINAL)
    /* Paintbrush 2.0 art direction state (spec §8, §12, §23, §52). */
    Q_PROPERTY(QString paperStockId READ paperStockId WRITE setPaperStockId NOTIFY artStateChanged FINAL)
    Q_PROPERTY(QString moodId READ moodId WRITE setMoodId NOTIFY artStateChanged FINAL)
    Q_PROPERTY(paintbrush::HarvestTrayModel *harvest READ harvest CONSTANT FINAL)

public:
    explicit PaintbrushDocument(QObject *parent = nullptr);

    /* -- document state ------------------------------------------------ */
    QSize canvasSize() const { return m_canvasSize; }
    /* The render target. Layer rasters are LOCAL-sized and are never
     * resized here — only the canvas (render target) changes. */
    void setCanvasSize(const QSize &size);
    int canvasWidth() const { return m_canvasSize.width(); }
    int canvasHeight() const { return m_canvasSize.height(); }
    int trackCount() const { return m_tracks.size(); }

    const QList<CanvasLayer> &layers() const { return m_layers; }
    QList<CanvasLayer> &layersRef() { return m_layers; }
    void setLayers(const QList<CanvasLayer> &layers);
    int layerCount() const { return m_layers.size(); }

    int currentLayerIndex() const { return m_currentLayerIndex; }
    void setCurrentLayerIndex(int index);
    CanvasLayer *currentLayer();
    const CanvasLayer *currentLayer() const;
    CanvasLayer *layerAt(int index);
    const CanvasLayer *layerAt(int index) const;
    /* stable-id lookups (typed undo commands) */
    int indexOfLayerId(int id) const;
    CanvasLayer *layerById(int id);
    const CanvasLayer *layerById(int id) const;

    /* -- canvas-space operations (pure transforms, never raster resizing) - */
    /* Rotate the whole canvas 90° (clockwise = true). Canvas size swaps and
     * every layer transform is recomposed; rasters are never resampled.
     * The mutation is applied immediately and the returned typed command
     * replays (redo) or reverts (undo) it exactly. */
    std::unique_ptr<UndoCommand> makeRotate90Command(bool clockwise);
    /* Crop the canvas to `rect` (canvas coordinates). Same contract. */
    std::unique_ptr<UndoCommand> makeCropCommand(const QRect &rect);

    /* -- selection ------------------------------------------------------ */
    const QPainterPath &selectionPath() const { return m_selection; }
    void setSelectionPath(const QPainterPath &path);
    void clearSelection();

    /* -- media reference ------------------------------------------------ */
    QString sourceMediaMRL() const { return m_sourceMRL; }
    QString sourceMediaTitle() const { return m_sourceTitle; }
    qint64 sourceTimeMs() const { return m_sourceTimeMs; }
    void setSourceMedia(const QString &mrl, const QString &title, qint64 timeMs);
    void clearSourceMedia();

    /* The canonical FrameRef of the captured/edit frame (A19): the AI and
     * crop paths decode exactly THIS frame through FrameProvider instead
     * of hunting for the first LAYER_REFERENCE layer. Kept in sync with
     * setSourceMedia() (mrl/title/timeMs) and set wholesale when a frame
     * is captured with verified dimensions/PTS. */
    FrameRef sourceFrame() const { return m_sourceFrame; }
    void setSourceFrame(const FrameRef &ref);

    /* -- tracked objects ------------------------------------------------- */
    const QVector<ObjectTrack> &tracks() const { return m_tracks; }
    void setTracks(const QVector<ObjectTrack> &tracks);
    /* Mutable access to the REAL track store the document owns, so
     * cache-style mutations (mask eviction under a memory budget) reach the
     * document instead of a controller copy (A32). Callers must re-emit
     * trackingStateChanged/notify as needed; this does NOT mark the project
     * dirty — eviction is a cache operation, not a content edit. */
    QVector<ObjectTrack> &tracksRef() { return m_tracks; }

    /* -- art direction (Paintbrush 2.0) ----------------------------------- */
    /* The paper the canvas is made of (empty = legacy transparent canvas). */
    QString paperStockId() const { return m_paperStockId; }
    void setPaperStockId(const QString &id);
    const art::PaperStock *paperStock() const;
    /* The one document light rig every material/finisher samples. */
    const art::ArtLightRig &lightRig() const { return m_lightRig; }
    void setLightRig(const art::ArtLightRig &rig);
    /* Current mood id (art direction preset); empty = none applied. */
    QString moodId() const { return m_moodId; }
    void setMoodId(const QString &id);
    /* Document seed: every material/stroke derives its seed from this so
     * reopen/export reproduce identical results (spec §37). */
    quint32 artSeed() const { return m_artSeed; }
    void setArtSeed(quint32 seed);
    /* Harvest tray — document-scoped (spec §5.4, §52). */
    HarvestTrayModel *harvest() { return &m_harvest; }
    const HarvestTrayModel *harvest() const { return &m_harvest; }
    /* Living Harvest asset store — document-scoped. */
    PuppetAssetStore *puppets() { return &m_puppets; }
    const PuppetAssetStore *puppets() const { return &m_puppets; }
    /* After a project load: drop puppet refs whose asset could not be
     * rebuilt (missing harvest). Returns the count of cleared layers. */
    int prunePuppetRefs();
    /* Showroom gate: any VISIBLE layer carries a living asset. The
     * AnimationClock runs only while this is true. */
    bool hasVisibleLivingLayer() const;
    /* World scale for this document at `exportScale`. Canvas pixels are the
     * world unit at 1x (96 units/inch). */
    art::ArtScale artScale(float exportScale = 1.0f) const;

    /* -- project ---------------------------------------------------------- */
    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString &path);
    bool dirty() const { return m_dirty; }
    void setDirty(bool dirty);

    /* Flatten all visible layers through the canonical DocumentRenderer
     * (thin wrapper; identical transform semantics as the screen path). */
    QImage flattenedImage() const;

    /* Composited source for AI operations: the captured media frame when
     * present, otherwise the flattened canvas. */
    QImage aiSourceImage() const;

signals:
    void canvasSizeChanged();
    void layersChanged();
    void currentLayerChanged(int index);
    void selectionChanged();
    void mediaChanged();
    void tracksChanged();
    void dirtyChanged(bool dirty);
    void documentReset();
    void artStateChanged();

public slots:
    void resetDocument(const QSize &size);

private:
    void assignLayerIds();

    QString m_paperStockId;
    art::ArtLightRig m_lightRig;
    QString m_moodId;
    quint32 m_artSeed = 1;
    HarvestTrayModel m_harvest;
    PuppetAssetStore m_puppets;

    QSize m_canvasSize;
    QList<CanvasLayer> m_layers;
    int m_nextLayerId = 1;
    int m_currentLayerIndex = -1;
    QPainterPath m_selection;
    bool m_hasSelection = false;

    QString m_sourceMRL;
    QString m_sourceTitle;
    qint64 m_sourceTimeMs = 0;
    FrameRef m_sourceFrame; /* exact frame identity for the AI/crop paths */

    QVector<ObjectTrack> m_tracks;

    QString m_projectPath;
    bool m_dirty = false;
};

} // namespace paintbrush

#endif // PB_DOCUMENT_HPP