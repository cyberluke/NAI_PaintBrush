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
#include <QRegion>

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
    TOOL_BRISTLE,          /* Paintbrush 2.0: premium bristle paint  */
    TOOL_GLITTER,          /* Paintbrush 2.0: glitter material pour  */
    TOOL_GLITTER_POWDER,   /* glitter POWDER: loose heap deposit (reference opacity) */
    TOOL_SMEAR,            /* smear/plow: push loose glitter, bonded stays */
    TOOL_ASK,              /* NAI: Ask-anything (VLM over frame/region) */
    TOOL_COUNT
};

enum LayerType {
    LAYER_RASTER = 0,   /* drawn/painted content               */
    LAYER_IMAGE,        /* imported / cutout image layer       */
    LAYER_TEXT,         /* editable text layer                 */
    LAYER_REFERENCE,    /* media frame reference layer         */
    LAYER_GENERATED,    /* AI generated result layer           */
    LAYER_STICKER       /* extracted/collage sticker asset      */
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

/* ------------------------------------------------------------------ dabs */

/* Parameters of one brush dab (the atomic stamp of the dab pipeline).
 * Shared by the brush, eraser and spray generators so every tool that
 * deposits paint goes through the same stamping + commit path. */
struct DabSpec
{
    qreal radius = 1.0;     /* dab radius in the target's coordinate space  */
    qreal hardness = 1.0;   /* 1.0 = hard edge; < 1.0 = soft radial falloff */
    qreal opacity = 1.0;    /* scales the color alpha (pressure mapping)    */
    QColor color = Qt::black;
    QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
};

/* Stamp one dab into `target` (drawn at `center`, clipped to the image).
 * With CompositionMode_Clear the color is ignored (eraser). */
void stampDab(QImage &target, const QPointF &center, const DabSpec &spec);

/* Convenience overload: opacity/color/mode without building a DabSpec. */
void stampDab(QImage &target, const QPointF &center, qreal radius,
              qreal hardness, qreal opacity, const QColor &color,
              QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver);

/* ------------------------------------------------------------------ fill */

/* Bounded scanline (span) flood fill. Returns the filled region as a
 * QRegion. No full-image copy and no w*h visited buffer: visited pixels are
 * tracked per row as merged [start,end] intervals, so memory is O(spans),
 * never O(canvas). Pixels outside `selection` (when non-null) act as
 * barriers. At most `maxPixels` are filled (negative = unbounded). */
QRegion scanlineFill(const QImage &image, const QPoint &seed, QRgb target,
                     QRgb fill, int tolerance,
                     const QPainterPath *selection = nullptr,
                     qint64 maxPixels = -1);

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

/* Log a completed AI request with its id, duration and outcome. Used by the
 * network services so diagnostics/logs carry request IDs and timings
 * (never keys or headers). */
void pbLogRequest(vlc_object_t *obj, const char *kind,
                  const QString &requestId, qint64 elapsedMs,
                  bool ok, const QString &detail = {});

/* paintbrush.* log-category hierarchy. The paintbrush sources compile inside
 * the qt module (MODULE_STRING=qt), so the msg_* macros would tag every line
 * as [qt]. These helpers call vlc_object_Log() with an explicit category
 * module string ("paintbrush.<category>") so the log lines carry a real
 * hierarchy: paintbrush, paintbrush.segmentation, paintbrush.tracking,
 * paintbrush.vision, paintbrush.generation, paintbrush.assistant,
 * paintbrush.companion, paintbrush.lookup, paintbrush.thumbnails,
 * paintbrush.export. The category is a dotted path, matching how VLC log
 * filters (--verbose=2, logcat) match on the module name. */
void pbLogMsg(vlc_object_t *obj, int prio, const char *category,
              const char *file, unsigned line, const char *func,
              const char *format, ...) VLC_FORMAT(7, 8);

#define pbLogDebug(obj, category, ...) \
    pbLogMsg(VLC_OBJECT(obj), VLC_MSG_DBG, "paintbrush." category, \
             __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pbLogInfo(obj, category, ...) \
    pbLogMsg(VLC_OBJECT(obj), VLC_MSG_INFO, "paintbrush." category, \
             __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pbLogWarn(obj, category, ...) \
    pbLogMsg(VLC_OBJECT(obj), VLC_MSG_WARN, "paintbrush." category, \
             __FILE__, __LINE__, __func__, __VA_ARGS__)
#define pbLogError(obj, category, ...) \
    pbLogMsg(VLC_OBJECT(obj), VLC_MSG_ERR, "paintbrush." category, \
             __FILE__, __LINE__, __func__, __VA_ARGS__)

} // namespace paintbrush

#endif // PB_COMMON_HPP