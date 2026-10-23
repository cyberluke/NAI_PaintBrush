/*****************************************************************************
 * pb_history.cpp : NAI Paintbrush — typed undo/redo implementation
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_history.hpp"
#include "pb_document.hpp"

#include <QPainter>

namespace paintbrush {

/* Restore `image` (which covers `rect`) into the layer's local raster. */
static void restoreRegion(CanvasLayer &layer, const QRect &rect,
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

/* -------------------------------------------------------------- commands */

qint64 StrokeCommand::bytes() const
{
    qint64 b = 0;
    if (!data.after.isNull())
        b += qint64(data.after.sizeInBytes());
    for (const auto &pair : data.beforeRects)
        b += qint64(pair.second.sizeInBytes());
    return b;
}

void StrokeCommand::apply(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerById(data.layerId);
    if (!layer)
        layer = doc.layerAt(data.layerIndex);
    if (layer)
    {
        restoreRegion(*layer, data.unionRect, data.after);
        doc.setDirty(true);
    }
}

void StrokeCommand::revert(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerById(data.layerId);
    if (!layer)
        layer = doc.layerAt(data.layerIndex);
    if (layer)
    {
        for (const auto &pair : data.beforeRects)
            restoreRegion(*layer, pair.first, pair.second);
        doc.setDirty(true);
    }
}

static CanvasLayer *resolveLayer(PaintbrushDocument &doc, int layerId,
                                 int layerIndex)
{
    CanvasLayer *layer = doc.layerById(layerId);
    if (!layer)
        layer = doc.layerAt(layerIndex);
    return layer;
}

void SetTransformCommand::apply(PaintbrushDocument &doc)
{
    CanvasLayer *layer = resolveLayer(doc, layerId, layerIndex);
    if (layer)
    {
        layer->setTransform(after);
        doc.setDirty(true);
    }
}

void SetTransformCommand::revert(PaintbrushDocument &doc)
{
    CanvasLayer *layer = resolveLayer(doc, layerId, layerIndex);
    if (layer)
    {
        layer->setTransform(before);
        doc.setDirty(true);
    }
}

void ReorderCommand::apply(PaintbrushDocument &doc)
{
    QList<CanvasLayer> layers = doc.layers();
    if (index < 0 || index >= layers.size() || target < 0 ||
        target >= layers.size())
        return;
    layers.move(index, target);
    doc.setLayers(layers);
    doc.setCurrentLayerIndex(target);
}

void ReorderCommand::revert(PaintbrushDocument &doc)
{
    QList<CanvasLayer> layers = doc.layers();
    if (target < 0 || target >= layers.size() || index < 0 ||
        index >= layers.size())
        return;
    layers.move(target, index);
    doc.setLayers(layers);
    doc.setCurrentLayerIndex(prevCurrentIndex);
}

void SetVisibilityCommand::apply(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerAt(layerIndex);
    if (layer)
    {
        layer->visible = after;
        doc.setDirty(true);
    }
}

void SetVisibilityCommand::revert(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerAt(layerIndex);
    if (layer)
    {
        layer->visible = before;
        doc.setDirty(true);
    }
}

void SetOpacityCommand::apply(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerAt(layerIndex);
    if (layer)
    {
        layer->opacity = after;
        doc.setDirty(true);
    }
}

void SetOpacityCommand::revert(PaintbrushDocument &doc)
{
    CanvasLayer *layer = doc.layerAt(layerIndex);
    if (layer)
    {
        layer->opacity = before;
        doc.setDirty(true);
    }
}

void InsertLayerCommand::apply(PaintbrushDocument &doc)
{
    QList<CanvasLayer> layers = doc.layers();
    const int idx = qBound(0, index, layers.size());
    CanvasLayer ins = layer;
    ins.id = 0; /* let the document assign a fresh id */
    layers.insert(idx, std::move(ins));
    doc.setLayers(layers);
    /* keep the recorded id stable across undo/redo so later commands and
     * revert-by-id stay valid */
    if (idx < doc.layersRef().size())
        doc.layersRef()[idx].id = layer.id;
    doc.setCurrentLayerIndex(idx);
}

void InsertLayerCommand::revert(PaintbrushDocument &doc)
{
    int idx = doc.indexOfLayerId(layer.id);
    if (idx < 0)
        idx = index;
    QList<CanvasLayer> layers = doc.layers();
    if (idx < 0 || idx >= layers.size())
        return;
    layers.removeAt(idx);
    doc.setLayers(layers);
    if (prevCurrentIndex >= 0 && prevCurrentIndex < layers.size())
        doc.setCurrentLayerIndex(prevCurrentIndex);
}

void DeleteLayerCommand::apply(PaintbrushDocument &doc)
{
    int idx = doc.indexOfLayerId(layer.id);
    if (idx < 0)
        idx = index;
    QList<CanvasLayer> layers = doc.layers();
    if (idx < 0 || idx >= layers.size())
        return;
    layers.removeAt(idx);
    doc.setLayers(layers);
    if (doc.currentLayerIndex() >= layers.size())
        doc.setCurrentLayerIndex(layers.isEmpty() ? -1 : layers.size() - 1);
}

void DeleteLayerCommand::revert(PaintbrushDocument &doc)
{
    QList<CanvasLayer> layers = doc.layers();
    const int idx = qBound(0, index, layers.size());
    CanvasLayer ins = layer;
    ins.id = 0;
    layers.insert(idx, std::move(ins));
    doc.setLayers(layers);
    if (idx < doc.layersRef().size())
        doc.layersRef()[idx].id = layer.id;
    doc.setCurrentLayerIndex(prevCurrentIndex);
}

qint64 MaskCommand::bytes() const
{
    qint64 b = 0;
    if (!data.before.isNull())
        b += qint64(data.before.sizeInBytes());
    if (!data.after.isNull())
        b += qint64(data.after.sizeInBytes());
    return b;
}

void MaskCommand::apply(PaintbrushDocument &doc)
{
    /* mask state lives in the controller; UndoStack emits maskRestored */
    Q_UNUSED(doc);
}

void MaskCommand::revert(PaintbrushDocument &doc)
{
    Q_UNUSED(doc);
}

void CanvasResizeCommand::apply(PaintbrushDocument &doc)
{
    doc.setCanvasSize(after);
}

void CanvasResizeCommand::revert(PaintbrushDocument &doc)
{
    doc.setCanvasSize(before);
}

void CompositeCommand::apply(PaintbrushDocument &doc)
{
    for (const auto &child : children)
        child->apply(doc);
}

void CompositeCommand::revert(PaintbrushDocument &doc)
{
    for (int i = children.size() - 1; i >= 0; --i)
        children[i]->revert(doc);
}

/* -------------------------------------------------------------- undo log */

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

void UndoStack::pushCommand(std::unique_ptr<UndoCommand> command)
{
    if (!command)
        return;

    /* drop any redo entries past the current position */
    while (m_entries.size() > m_undoIndex)
    {
        m_totalBytes -= m_entries.last().bytes;
        m_entries.removeLast();
    }

    Entry e;
    e.command = std::move(command); /* unique_ptr -> shared_ptr (no copy) */
    e.bytes = e.command->bytes();
    m_entries.append(std::move(e));
    m_undoIndex++;
    m_totalBytes += m_entries.last().bytes;
    enforceBounds();
    emit canUndoChanged();
    emit canRedoChanged();
}

void UndoStack::pushStroke(const StrokeCommand &cmd)
{
    pushCommand(std::make_unique<StrokeCommand>(cmd));
}

void UndoStack::pushSetTransform(const SetTransformCommand &cmd)
{
    pushCommand(std::make_unique<SetTransformCommand>(cmd));
}

void UndoStack::pushReorder(const ReorderCommand &cmd)
{
    pushCommand(std::make_unique<ReorderCommand>(cmd));
}

void UndoStack::pushSetVisibility(const SetVisibilityCommand &cmd)
{
    pushCommand(std::make_unique<SetVisibilityCommand>(cmd));
}

void UndoStack::pushSetOpacity(const SetOpacityCommand &cmd)
{
    pushCommand(std::make_unique<SetOpacityCommand>(cmd));
}

void UndoStack::pushInsertLayer(const InsertLayerCommand &cmd)
{
    pushCommand(std::make_unique<InsertLayerCommand>(cmd));
}

void UndoStack::pushDeleteLayer(const DeleteLayerCommand &cmd)
{
    pushCommand(std::make_unique<DeleteLayerCommand>(cmd));
}

void UndoStack::pushMask(const MaskCommand &cmd)
{
    if (cmd.data.before.isNull() && cmd.data.after.isNull())
        return;
    pushCommand(std::make_unique<MaskCommand>(cmd));
}

void UndoStack::pushCanvasResize(const CanvasResizeCommand &cmd)
{
    pushCommand(std::make_unique<CanvasResizeCommand>(cmd));
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
    return m_entries[m_undoIndex - 1].command->description();
}

QString UndoStack::redoText() const
{
    if (!canRedo())
        return {};
    return m_entries[m_undoIndex].command->description();
}

bool UndoStack::undo(PaintbrushDocument &doc)
{
    if (!canUndo())
        return false;

    const Entry &entry = m_entries[m_undoIndex - 1];
    if (entry.command->kind() == UndoCommand::Kind::Mask)
        emit maskRestored(static_cast<const MaskCommand *>(
                              entry.command.get())->data.before);
    entry.command->revert(doc);
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
    if (entry.command->kind() == UndoCommand::Kind::Mask)
        emit maskRestored(static_cast<const MaskCommand *>(
                              entry.command.get())->data.after);
    entry.command->apply(doc);
    m_undoIndex++;
    emit canUndoChanged();
    emit canRedoChanged();
    return true;
}

} // namespace paintbrush