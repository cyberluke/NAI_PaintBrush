/*****************************************************************************
 * pb_project_store.hpp : NAI Paintbrush — project persistence
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_PROJECT_STORE_HPP
#define PB_PROJECT_STORE_HPP

#include "pb_common.hpp"

#include <QObject>
#include <QString>
#include <QByteArray>

namespace paintbrush {

class PaintbrushDocument;

/* Versioned single-file Paintbrush project format (JSON). Atomic save via
 * temp file + rename; crash-safe; forward-compatible unknown fields are
 * preserved by the loader (extra object members are kept). External video
 * files are referenced by MRL, not duplicated. */
class ProjectStore : public QObject
{
    Q_OBJECT

public:
    static constexpr int FORMAT_VERSION = 1;

    explicit ProjectStore(QObject *parent = nullptr);

    /* Returns an error message, or empty on success. */
    QString save(PaintbrushDocument *doc, const QString &path);
    QString saveToBytes(PaintbrushDocument *doc, QByteArray *out) const;
    QString load(PaintbrushDocument *doc, const QString &path);
    QString loadFromBytes(PaintbrushDocument *doc, const QByteArray &data) const;

    QString lastError() const { return m_lastError; }

private:
    QString m_lastError;
};

} // namespace paintbrush

#endif // PB_PROJECT_STORE_HPP