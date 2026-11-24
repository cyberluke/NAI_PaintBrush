/*****************************************************************************
 * pb_blend.hpp : NAI Paintbrush — blend modes shared by the canvas and recipes
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_BLEND_HPP
#define PB_BLEND_HPP

#include <QPainter>

namespace paintbrush {

/* Paintbrush blend identifiers. Kept as plain ints so they map trivially to
 * QML and to the persisted layer format. This header is deliberately free of
 * VLC dependencies so the pure creative recipe builder (and its standalone
 * harness) can use the exact same values as the canvas renderer. */
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
    switch (blendMode)
    {
    case BLEND_MULTIPLY: return QPainter::CompositionMode_Multiply;
    case BLEND_SCREEN:   return QPainter::CompositionMode_Screen;
    case BLEND_OVERLAY:  return QPainter::CompositionMode_Overlay;
    case BLEND_DARKEN:   return QPainter::CompositionMode_Darken;
    case BLEND_LIGHTEN:  return QPainter::CompositionMode_Lighten;
    case BLEND_NORMAL:
    default:             return QPainter::CompositionMode_SourceOver;
    }
}

} // namespace paintbrush

#endif // PB_BLEND_HPP
