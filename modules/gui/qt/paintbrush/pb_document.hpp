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

#include <QObject>
#include <QSize>
#include <QPainterPath>
#include <QImage>
#include <QList>
#include <QVector>
#include <QStringList>

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

class PaintbrushDocument : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY canvasSizeChanged FINAL)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY canvasSizeChanged FINAL)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY layersChanged FINAL)
    Q_PROPERTY(int currentLayerIndex READ currentLayerIndex WRITE setCurrentLayerIndex NOTIFY currentLayerChanged FINAL)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY tracksChanged FINAL)

public:
    explicit PaintbrushDocument(QObject *parent = nullptr);

    /* -- document state ------------------------------------------------ */
    QSize canvasSize() const { return m_canvasSize; }
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

    /* -- tracked objects ------------------------------------------------- */
    const QVector<ObjectTrack> &tracks() const { return m_tracks; }
    void setTracks(const QVector<ObjectTrack> &tracks);

    /* -- project ---------------------------------------------------------- */
    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString &path);
    bool dirty() const { return m_dirty; }
    void setDirty(bool dirty);

    /* -- undo support ------------------------------------------------------ */
    /* Snapshot helpers used by UndoStack. Layer snapshots are compact JSON
     * blobs (see ProjectStore serialization) so structural changes can be
     * undone as one unit. */
    QByteArray layersSnapshot() const;
    void restoreLayersSnapshot(const QByteArray &data);
    QByteArray canvasSnapshot() const;
    void restoreCanvasSnapshot(const QByteArray &data);

    /* Flatten all visible layers into one image (used for export, AI input
     * and companion previews). */
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

public slots:
    void resetDocument(const QSize &size);

private:
    QSize m_canvasSize;
    QList<CanvasLayer> m_layers;
    int m_currentLayerIndex = -1;
    QPainterPath m_selection;
    bool m_hasSelection = false;

    QString m_sourceMRL;
    QString m_sourceTitle;
    qint64 m_sourceTimeMs = 0;

    QVector<ObjectTrack> m_tracks;

    QString m_projectPath;
    bool m_dirty = false;
};

} // namespace paintbrush

#endif // PB_DOCUMENT_HPP