/*****************************************************************************
 * pb_model_paths.hpp : NAI Paintbrush — model discovery inside the install
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_MODEL_PATHS_HPP
#define PB_MODEL_PATHS_HPP

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace paintbrush {

/* A packaged build ships the quantized SAM3 weights next to the executable
 * (models/sam3-q4_0.ggml). Services use them when no model is configured, so a
 * clean install has Object Select / tracking available with zero setup. A
 * configured --paintbrush-sam-model always wins. */
inline QString defaultSamModelPath()
{
    const QDir app(QCoreApplication::applicationDirPath());
    const char *candidates[] = {
        "models/sam3-q4_0.ggml",
        "models/sam3-f16.ggml",
    };
    for (const char *rel : candidates) {
        const QString path = app.filePath(QString::fromLatin1(rel));
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

} // namespace paintbrush

#endif // PB_MODEL_PATHS_HPP
