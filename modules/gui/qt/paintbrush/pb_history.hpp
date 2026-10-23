/*****************************************************************************
 * pb_history.hpp : NAI Paintbrush — typed document undo/redo commands
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
#include <QTransform>
#include <memory>
#include <vector>

namespace paintbrush {

class PaintbrushDocument;

/* Region-paint undo payload (StrokeCommand). `beforeRects` holds the
 * original pixel content of the painted sub-regions in LAYER-LOCAL
 * coordinates, captured before each was first modified, so strokes only pay
 * for the area they actually touched. */
struct StrokeUndoData
{
    int layerIndex = -1;
    int layerId = 0;
    QRect unionRect;                                   /* layer-local coords */
    QList<QPair<QRect, QImage>> beforeRects;           /* original pixels */
    QImage after;                                      /* unionRect-sized */
    QString description;
};

/* Object-mask refine payload: before/after binary grayscale masks so a
 * freehand mask stroke is a single undoable operation. */
struct MaskUndoData
{
    QImage before;
    QImage after;
    QString description;
};

/* ------------------------------------------------------------------ undo */

/* Typed undo command. Each command knows how to apply (redo / replay) and
 * revert (undo) itself on the document. Structural commands never serialize
 * layer pixels — the only pixel payloads are region deltas (StrokeCommand)
 * and mask pairs (MaskCommand). */
class UndoCommand
{
public:
    virtual ~UndoCommand() = default;
    virtual void apply(PaintbrushDocument &doc) = 0;   /* redo / initial */
    virtual void revert(PaintbrushDocument &doc) = 0;  /* undo */
    virtual QString description() const = 0;
    virtual qint64 bytes() const { return 0; }

    enum class Kind
    {
        Stroke,
        SetTransform,
        Reorder,
        SetVisibility,
        SetOpacity,
        InsertLayer,
        DeleteLayer,
        Mask,
        CanvasResize,
        Composite
    };
    virtual Kind kind() const = 0;
};

class StrokeCommand : public UndoCommand
{
public:
    StrokeUndoData data;
    Kind kind() const override { return Kind::Stroke; }
    QString description() const override { return data.description; }
    qint64 bytes() const override;
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* Layer transform change (scale/rotation/move, rotate90/crop children).
 * apply()/revert() restore the exact QTransform through the layer's
 * transform/setTransform round trip — rasters are never touched. */
class SetTransformCommand : public UndoCommand
{
public:
    SetTransformCommand() = default;
    SetTransformCommand(int layerId_, int layerIndex_,
                        const QTransform &before_, const QTransform &after_,
                        QString desc_)
        : layerId(layerId_), layerIndex(layerIndex_), before(before_),
          after(after_), desc(std::move(desc_))
    {
    }
    int layerId = 0;
    int layerIndex = -1;
    QTransform before;
    QTransform after;
    QString desc;
    Kind kind() const override { return Kind::SetTransform; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

class ReorderCommand : public UndoCommand
{
public:
    int index = -1;
    int target = -1;
    int prevCurrentIndex = -1;
    QString desc;
    Kind kind() const override { return Kind::Reorder; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

class SetVisibilityCommand : public UndoCommand
{
public:
    int layerIndex = -1;
    bool before = true;
    bool after = true;
    QString desc;
    Kind kind() const override { return Kind::SetVisibility; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* Layer opacity change (0..1). Mirrors SetVisibilityCommand: a scalar
 * in-place mutation with before/after values, O(1), never pixel data. */
class SetOpacityCommand : public UndoCommand
{
public:
    int layerIndex = -1;
    float before = 1.0f;
    float after = 1.0f;
    QString desc;
    Kind kind() const override { return Kind::SetOpacity; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* Insert/delete carry the layer content by value (QImage is copy-on-write):
 * the asset is immutable once the command is pushed, and no pixels are
 * re-serialized on undo/redo. */
class InsertLayerCommand : public UndoCommand
{
public:
    int index = -1;
    CanvasLayer layer;
    int prevCurrentIndex = -1;
    QString desc;
    Kind kind() const override { return Kind::InsertLayer; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

class DeleteLayerCommand : public UndoCommand
{
public:
    int index = -1;
    CanvasLayer layer;
    int prevCurrentIndex = -1;
    QString desc;
    Kind kind() const override { return Kind::DeleteLayer; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* apply() emits maskRestored(after), revert() maskRestored(before). */
class MaskCommand : public UndoCommand
{
public:
    MaskUndoData data;
    Kind kind() const override { return Kind::Mask; }
    QString description() const override { return data.description; }
    qint64 bytes() const override;
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* Canvas size change — metadata only: layer rasters are never resized. */
class CanvasResizeCommand : public UndoCommand
{
public:
    CanvasResizeCommand() = default;
    CanvasResizeCommand(const QSize &before_, const QSize &after_, QString desc_)
        : before(before_), after(after_), desc(std::move(desc_))
    {
    }
    QSize before;
    QSize after;
    QString desc;
    Kind kind() const override { return Kind::CanvasResize; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* One undo step spanning several typed sub-commands (rotate90/crop: canvas
 * resize + per-layer SetTransformCommand). Children apply in order and
 * revert in reverse order. std::vector (move-only elements; QList's COW
 * cannot hold unique_ptr). */
class CompositeCommand : public UndoCommand
{
public:
    QString desc;
    std::vector<std::unique_ptr<UndoCommand>> children;
    Kind kind() const override { return Kind::Composite; }
    QString description() const override { return desc; }
    void apply(PaintbrushDocument &doc) override;
    void revert(PaintbrushDocument &doc) override;
};

/* The undo/redo log. Commands are typed and O(changed) — never whole-document
 * PNG/base64 serialization. */
class UndoStack : public QObject
{
    Q_OBJECT

public:
    explicit UndoStack(QObject *parent = nullptr);

    /* Generic typed-command push (takes ownership). */
    void pushCommand(std::unique_ptr<UndoCommand> command);
    void pushStroke(const StrokeCommand &cmd);
    void pushSetTransform(const SetTransformCommand &cmd);
    void pushReorder(const ReorderCommand &cmd);
    void pushSetVisibility(const SetVisibilityCommand &cmd);
    void pushSetOpacity(const SetOpacityCommand &cmd);
    void pushInsertLayer(const InsertLayerCommand &cmd);
    void pushDeleteLayer(const DeleteLayerCommand &cmd);
    void pushMask(const MaskCommand &cmd);
    void pushCanvasResize(const CanvasResizeCommand &cmd);

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
    /* Fired on undo/redo of a Mask command; the controller re-syncs the
     * active object mask (document + canvas) to `mask`. */
    void maskRestored(const QImage &mask);

private:
    struct Entry
    {
        /* shared_ptr so the entry list (QList) stays copyable; commands are
         * immutable after push */
        std::shared_ptr<UndoCommand> command;
        qint64 bytes = 0;
    };

    void enforceBounds();
    void dropOldest();

    QList<Entry> m_entries;
    int m_undoIndex = 0;
    int m_maxEntries = 50;
    qint64 m_byteBudget = 384 * 1024 * 1024; /* 384 MiB */
    qint64 m_totalBytes = 0;
};

} // namespace paintbrush

#endif // PB_HISTORY_HPP