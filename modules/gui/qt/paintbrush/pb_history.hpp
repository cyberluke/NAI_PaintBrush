/*****************************************************************************
 * pb_history.hpp : NAI Paintbrush — document undo/redo command log
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_HISTORY_HPP
#define PB_HISTORY_HPP

#include "pb_common.hpp"
#include "pb_layer.hpp"

#include <QObject>
#include <QImage>
#include <QRect>
#include <QPair>
#include <QList>
#include <QByteArray>

namespace paintbrush {

class PaintbrushDocument;

/* Region-paint undo entry. `beforeRects` holds the original pixel content
 * of the painted sub-regions, captured before each was first modified, so
 * strokes only pay for the area they actually touched. */
struct PaintUndoEntry
{
    int layerIndex = -1;
    QRect unionRect;                                   /* canvas coords */
    QList<QPair<QRect, QImage>> beforeRects;           /* original pixels */
    QImage after;                                      /* unionRect-sized */
    QString description;
};

/* Structural undo entry: a JSON snapshot of the whole layer list. */
struct StructureUndoEntry
{
    QByteArray before;                                 /* layersSnapshot() */
    QByteArray after;
    QString description;
};

class UndoStack : public QObject
{
    Q_OBJECT

public:
    explicit UndoStack(QObject *parent = nullptr);

    enum class Kind { Paint, Structure };

    void pushPaint(const PaintUndoEntry &entry);
    void pushStructure(const StructureUndoEntry &entry);

    bool canUndo() const { return m_undoIndex > 0; }
    bool canRedo() const { return m_undoIndex < m_entries.size(); }
    QString undoText() const;
    QString redoText() const;
    int count() const { return m_entries.size(); }
    void clear();

    /* Returns true when something was restored. */
    bool undo(PaintbrushDocument &doc);
    bool redo(PaintbrushDocument &doc);

    /* Memory bounds: maximum entries and a total byte budget for the
     * before/after pixel data. Oldest entries are dropped first. */
    void setMaxEntries(int maxEntries);
    void setByteBudget(qint64 bytes);

signals:
    void canUndoChanged();
    void canRedoChanged();

private:
    struct Entry
    {
        Kind kind = Kind::Paint;
        PaintUndoEntry paint;
        StructureUndoEntry structure;
        qint64 bytes = 0;
    };

    void enforceBounds();
    void dropOldest();
    static void restoreRegion(CanvasLayer &layer, const QRect &rect,
                              const QImage &image);
    qint64 paintBytes(const PaintUndoEntry &entry) const;

    QList<Entry> m_entries;
    int m_undoIndex = 0;
    int m_maxEntries = 50;
    qint64 m_byteBudget = 384 * 1024 * 1024; /* 384 MiB */
    qint64 m_totalBytes = 0;
};

} // namespace paintbrush

#endif // PB_HISTORY_HPP