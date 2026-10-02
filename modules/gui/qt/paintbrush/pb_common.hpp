/*****************************************************************************
 * pb_common.hpp : NAI Paintbrush — shared types and helpers
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef PB_COMMON_HPP
#define PB_COMMON_HPP

#include <QImage>
#include <QString>
#include <QPointF>
#include <QVector>
#include <QPainter>
#include <QPainterPath>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

struct vlc_player_t;

namespace paintbrush {

/* RAII guard for the vlc_player lock. The player lock is reference counted:
 * each vlc_player_Lock() must be paired with one vlc_player_Unlock(). All
 * vlc_player_* accessors require the lock to be held; this guard releases it
 * automatically on scope exit. */
class PlayerLock
{
public:
    explicit PlayerLock(vlc_player_t *player_) noexcept;
    ~PlayerLock();
    PlayerLock(const PlayerLock &) = delete;
    PlayerLock &operator=(const PlayerLock &) = delete;

private:
    vlc_player_t *player;
};

/* Classic Paintbrush tool identifiers. Kept as plain ints so they map
 * trivially to QML without extra registration machinery. */
enum ToolId {
    TOOL_NONE = 0,
    TOOL_RECT_SELECT,      /* Rectangular selection            */
    TOOL_FREEFORM_SELECT,  /* Lasso / freeform selection       */
    TOOL_OBJECT_SELECT,    /* AI object selection (SAM3)       */
    TOOL_PENCIL,
    TOOL_BRUSH,
    TOOL_ERASER,
    TOOL_FILL,
    TOOL_EYEDROPPER,
    TOOL_SPRAY,
    TOOL_LINE,
    TOOL_RECTANGLE,
    TOOL_ROUNDED_RECT,
    TOOL_ELLIPSE,
    TOOL_TEXT,
    TOOL_PAN_ZOOM,
    TOOL_COUNT
};

enum LayerType {
    LAYER_RASTER = 0,   /* drawn/painted content               */
    LAYER_IMAGE,        /* imported / cutout image layer       */
    LAYER_TEXT,         /* editable text layer                 */
    LAYER_REFERENCE,    /* media frame reference layer         */
    LAYER_GENERATED     /* AI generated result layer           */
};

enum BlendMode {
    BLEND_NORMAL = 0,
    BLEND_MULTIPLY,
    BLEND_SCREEN,
    BLEND_OVERLAY,
    BLEND_DARKEN,
    BLEND_LIGHTEN
};

/* Convert a paintbrush blend mode to a QPainter composition mode. */
inline QPainter::CompositionMode toCompositionMode(int blendMode)
{
    switch (blendMode) {
    case BLEND_MULTIPLY: return QPainter::CompositionMode_Multiply;
    case BLEND_SCREEN:   return QPainter::CompositionMode_Screen;
    case BLEND_OVERLAY:  return QPainter::CompositionMode_Overlay;
    case BLEND_DARKEN:   return QPainter::CompositionMode_Darken;
    case BLEND_LIGHTEN:  return QPainter::CompositionMode_Lighten;
    case BLEND_NORMAL:
    default:             return QPainter::CompositionMode_SourceOver;
    }
}

/* Format an image for a specific file format using Qt's image writers. */
QStringList supportedImageFormats();

/* Convert a binary (0/255) mask into a gray QImage. */
QImage maskToImage(const QImage &mask);

/* Apply a binary mask as the alpha channel of a source image. The source
 * is expected to be RGB (the mask supplies transparency). */
QImage applyMaskAsAlpha(const QImage &source, const QImage &mask);

/* Convert a QImage to an RGB888 QImage (3 channels, tightly packed). */
QImage toRgb888(const QImage &image);

/* Trace the outer contour of a binary mask into a QPainterPath (Moore
 * neighbor tracing). Used to convert AI masks into editable selections. */
QPainterPath maskToPath(const QImage &mask);

/* Read a VLC config string option. */
QString pbConfigString(vlc_object_t *obj, const char *name);
/* Read a VLC config bool option. */
bool pbConfigBool(vlc_object_t *obj, const char *name);
/* Read a VLC config integer option. */
int pbConfigInt(vlc_object_t *obj, const char *name);
/* Read an API key from the environment or VLC config (never logged). */
QString pbApiKey(vlc_object_t *obj, const char *envVar, const char *configName);

} // namespace paintbrush

#endif // PB_COMMON_HPP