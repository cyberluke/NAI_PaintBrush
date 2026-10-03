/*****************************************************************************
 * pb_history.cpp : NAI Paintbrush — undo/redo implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_history.hpp"
#include "pb_document.hpp"

#include <QPainter>

namespace paintbrush {

UndoStack::UndoStack(QObject *parent)
    : QObject(parent)
{
}

void UndoStack::setMaxEntries(int maxEntries)
{
    m_maxEntries = qMax(1, maxEntries);
    enforceBounds();
}

void UndoStack::setByteBudget(qint64 bytes)
{
    m_byteBudget = qMax<qint64>(1, bytes);
    enforceBounds();
}

qint64 UndoStack::paintBytes(const PaintUndoEntry &entry) const
{
    qint64 bytes = 0;
    if (!entry.after.isNull())
        bytes += qint64(entry.after.sizeInBytes());
    for (const auto &pair : entry.beforeRects)
        bytes += qint64(pair.second.sizeInBytes());
    return bytes;
}

qint64 UndoStack::maskBytes(const MaskUndoEntry &entry) const
{
    qint64 bytes = 0;
    if (!entry.before.isNull())
        bytes += qint64(entry.before.sizeInBytes());
    if (!entry.after.isNull())
        bytes += qint64(entry.after.sizeInBytes());
    return bytes;
}

void UndoStack::pushPaint(const PaintUndoEntry &entry)
{
    if (entry.layerIndex < 0)
        return;

    /* Drop any redo entries past the current position. */
    while (m_entries.size() > m_undoIndex)
    {
        m_totalBytes -= m_entries.last().bytes;
        m_entries.removeLast();
    }

    Entry e;
    e.kind = Kind::Paint;
    e.paint = entry;
    e.bytes = paintBytes(entry);
    m_entries.append(e);
    m_undoIndex++;
    m_totalBytes += e.bytes;
    enforceBounds();
    emit canUndoChanged();
    emit canRedoChanged();
}

void UndoStack::pushStructure(const StructureUndoEntry &entry)
{
    while (m_entries.size() > m_undoIndex)
    {
        m_totalBytes -= m_entries.last().bytes;
        m_entries.removeLast();
    }

    Entry e;
    e.kind = Kind::Structure;
    e.structure = entry;
    e.bytes = qint64(entry.before.size() + entry.after.size());
    m_entries.append(e);
    m_undoIndex++;
    m_totalBytes += e.bytes;
    enforceBounds();
    emit canUndoChanged();
    emit canRedoChanged();
}

void UndoStack::pushMask(const MaskUndoEntry &entry)
{
    if (entry.before.isNull() && entry.after.isNull())
        return;

    while (m_entries.size() > m_undoIndex)
    {
        m_totalBytes -= m_entries.last().bytes;
        m_entries.removeLast();
    }

    Entry e;
    e.kind = Kind::Mask;
    e.mask = entry;
    e.bytes = maskBytes(entry);
    m_entries.append(e);
    m_undoIndex++;
    m_totalBytes += e.bytes;
    enforceBounds();
    emit canUndoChanged();
    emit canRedoChanged();
}

void UndoStack::dropOldest()
{
    while (m_entries.size() > m_maxEntries || m_totalBytes > m_byteBudget)
    {
        if (m_entries.isEmpty())
            break;
        m_totalBytes -= m_entries.first().bytes;
        m_entries.removeFirst();
        m_undoIndex--;
    }
    if (m_undoIndex < 0)
        m_undoIndex = 0;
}

void UndoStack::enforceBounds()
{
    if (m_entries.isEmpty())
        return;
    dropOldest();
}

void UndoStack::clear()
{
    m_entries.clear();
    m_undoIndex = 0;
    m_totalBytes = 0;
    emit canUndoChanged();
    emit canRedoChanged();
}

QString UndoStack::undoText() const
{
    if (!canUndo())
        return {};
    const Entry &e = m_entries[m_undoIndex - 1];
    if (e.kind == Kind::Paint)
        return e.paint.description;
    if (e.kind == Kind::Mask)
        return e.mask.description;
    return e.structure.description;
}

QString UndoStack::redoText() const
{
    if (!canRedo())
        return {};
    const Entry &e = m_entries[m_undoIndex];
    if (e.kind == Kind::Paint)
        return e.paint.description;
    if (e.kind == Kind::Mask)
        return e.mask.description;
    return e.structure.description;
}

void UndoStack::restoreRegion(CanvasLayer &layer, const QRect &rect,
                              const QImage &image)
{
    if (layer.raster.isNull() || image.isNull())
        return;

    const QRect clipped = rect.intersected(layer.raster.rect());
    if (clipped.isEmpty())
        return;

    QPainter p(&layer.raster);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    /* image covers `rect`; translate so it lands at the right spot */
    p.drawImage(clipped.topLeft(), image.copy(clipped.translated(-rect.topLeft())));
    p.end();
}

bool UndoStack::undo(PaintbrushDocument &doc)
{
    if (!canUndo())
        return false;

    const Entry &entry = m_entries[m_undoIndex - 1];
    if (entry.kind == Kind::Paint)
    {
        CanvasLayer *layer = doc.layerAt(entry.paint.layerIndex);
        if (layer)
        {
            for (const auto &pair : entry.paint.beforeRects)
                restoreRegion(*layer, pair.first, pair.second);
            doc.setDirty(true);
        }
    }
    else if (entry.kind == Kind::Mask)
    {
        emit maskRestored(entry.mask.before);
    }
    else
    {
        doc.restoreLayersSnapshot(entry.structure.before);
    }
    m_undoIndex--;
    emit canUndoChanged();
    emit canRedoChanged();
    return true;
}

bool UndoStack::redo(PaintbrushDocument &doc)
{
    if (!canRedo())
        return false;

    const Entry &entry = m_entries[m_undoIndex];
    if (entry.kind == Kind::Paint)
    {
        CanvasLayer *layer = doc.layerAt(entry.paint.layerIndex);
        if (layer)
        {
            restoreRegion(*layer, entry.paint.unionRect, entry.paint.after);
            doc.setDirty(true);
        }
    }
    else if (entry.kind == Kind::Mask)
    {
        emit maskRestored(entry.mask.after);
    }
    else
    {
        doc.restoreLayersSnapshot(entry.structure.after);
    }
    m_undoIndex++;
    emit canUndoChanged();
    emit canRedoChanged();
    return true;
}

} // namespace paintbrush