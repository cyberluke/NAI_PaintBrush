/*****************************************************************************
 * pb_creative_art.hpp : NAI Paintbrush — procedural creative artwork
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_ART_HPP
#define PB_CREATIVE_ART_HPP

#include <QColor>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace paintbrush {

class CanvasLayer;

/* Deterministic, dependency-free visual-effect primitives shared by the
 * Creative Play Pad patches and the subject-aware studio recipes.
 *
 * Everything here is a pure function of its arguments: the same inputs and
 * seed produce byte-identical pixels on every run. That is what keeps a
 * committed look reproducible after undo/redo, project reopen and video
 * export. Nothing here is generative: every result is derived from the
 * subject's own alpha, luminance and colour, the recipe palette, or a seeded
 * procedural field.
 *
 * Masks are 8-bit grayscale (Format_Grayscale8), images are ARGB32, and all
 * "px" parameters are pixels of the image being produced. */
namespace creative {

/* --- masks and cutouts -------------------------------------------------- */

/* Alpha channel of `raster` as a grayscale mask (the cutout silhouette). */
QImage maskFromAlpha(const QImage &raster);

/* The layer's editable mask when it has one, otherwise its alpha. The result
 * is always the size of the layer raster. Implemented in pb_creative_art.cpp
 * when the canvas model is available (not in the standalone harness). */
QImage layerMask(const CanvasLayer &layer);

/* Apply `mask` to the alpha of `raster` (layer mask or object cutout). */
QImage maskToCutout(const QImage &raster, const QImage &mask);

/* Trim fully transparent borders. `offset` receives the top-left offset of
 * the retained content in the source image. */
QImage trimTransparent(const QImage &image, QPoint *offset = nullptr);

/* Disc dilation / erosion. `radius` <= 0 returns the input. */
QImage growMask(const QImage &mask, int radius);
QImage shrinkMask(const QImage &mask, int radius);

/* Decorative bands around the silhouette:
 *   bandFromMask / outerBand = grow(mask, radius) - mask
 *   innerBand                = mask - shrink(mask, radius)            */
QImage bandFromMask(const QImage &mask, int radius);
QImage outerBand(const QImage &mask, int radius);
QImage innerBand(const QImage &mask, int radius);

/* Tint a grayscale band. `glitter` sprinkles deterministic pale flecks. */
QImage colorizeBand(const QImage &band, const QColor &color, bool glitter,
                    quint32 seed);

/* Cheap detail-preserving blur (downscale -> smooth upscale). `downscale`
 * <= 0 picks a factor from the image size. */
QImage softBlur(const QImage &image, int downscale = 0);

/* --- colour ------------------------------------------------------------- */

/* Perceptual lightness of a colour in Oklab (0 black .. 1 white). */
float oklabLightness(const QColor &color);

/* A simple, deterministic grade. `brightness` multiplies the channels,
 * `contrast` pushes them away from mid-grey, `saturation` blends towards
 * luminance. `tint` colours the shadows, `lightTint` the highlights. */
struct Grade
{
    float brightness = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    QColor tint;
    float tintAmount = 0.0f;
    QColor lightTint;
    float lightAmount = 0.0f;
};

QImage gradeImage(const QImage &image, const Grade &grade);

/* Rotate hue by `degrees` while keeping saturation/value and alpha. */
QImage hueShift(const QImage &image, int degrees);

/* Recolour only the masked pixels in Oklab while KEEPING the original
 * lightness. The palette is sampled by lightness (a gradient map), so fabric
 * folds and shading survive: only hue/chroma move. `textureKeep` 0..1 keeps
 * part of the original chroma variation (0 = flat map, 1 = original chroma).
 * `skinGuard` 0..1 reduces the effect on skin-like hue/chroma/lightness
 * clusters so faces are not painted over (an approximation, not a face mask).
 * `hueDrift` (radians) adds an iridescent hue ripple across lightness. */
struct RecolorParams
{
    QVector<QColor> palette;
    float strength = 0.9f;
    float textureKeep = 0.30f;
    float hueDrift = 0.0f;
    float skinGuard = 0.0f;
    quint32 seed = 0;
};

QImage recolorMasked(const QImage &source, const QImage &mask,
                     const RecolorParams &params);

/* Holographic specular sheen: bright pixels (folds, highlights) receive a
 * spectral colour that moves with position, so the material reads as light
 * travelling across the surface rather than a gradient painted on top. */
QImage spectralSheen(const QImage &source, const QImage &mask,
                     const QVector<QColor> &palette, float strength,
                     float angleDeg, float phase);

/* --- holographic materials --------------------------------------------- */

/* One material engine, four recipes. The recipes differ only in their optical
 * parameters (film thickness, diffraction scale, metallic response, sheen,
 * sparkle); the shader and the layer contract are identical. */
enum class HoloMaterialId
{
    LiquidHologram = 0,
    OpalDream = 1,
    ChromaticChrome = 2,
    PrismaticFantasy = 3,
};

struct HoloMaterial
{
    QString name;
    float metallic = 0.35f;       /* 0 dielectric .. 1 metal               */
    float roughness = 0.15f;      /* sharpness of the specular lobe        */
    float filmThickness = 320.0f; /* thin-film thickness in nm, base       */
    float filmVariation = 160.0f; /* nm variation with shading/orientation */
    float grating = 1.0f;         /* diffraction cycles across the surface */
    float dispersion = 0.8f;      /* spectral separation 0..1              */
    float sheen = 0.6f;           /* broad clearcoat response 0..1         */
    float sparkle = 0.15f;        /* discrete glints 0..1                  */
    float saturation = 0.9f;      /* palette chroma 0..1                   */
    float normalStrength = 0.6f;  /* how much the source normals matter    */
    QVector<QColor> palette;
};

HoloMaterial holoMaterialPreset(HoloMaterialId id);
QString holoMaterialName(HoloMaterialId id);

/* Screen-space orientation from the source's own shading. The result is an
 * ARGB32 image whose RGB channels encode a unit normal
 * (x right, y down, z toward the viewer). This is an approximation, not 3D
 * reconstruction; it gives the material directional highlights that follow
 * folds instead of a flat rainbow overlay. */
QImage estimateNormals(const QImage &source, const QImage &mask, float strength);

/* Albedo (replaces the subject inside the mask) and a transparent sheen layer
 * (screen-blended above it). Both are pure functions of the inputs and seed. */
struct HoloMaterialResult
{
    QImage albedo;
    QImage sheen;
};

HoloMaterialResult holographicMaterial(const QImage &source,
                                       const QImage &mask,
                                       const QImage &normals,
                                       const HoloMaterial &material,
                                       float strength, quint32 seed);

/* --- light -------------------------------------------------------------- */

/* Soft coloured halo grown from the mask (backdrops and auras). */
QImage glowField(const QImage &mask, int radius, const QColor &color,
                 float intensity);

/* Two-scale edge light: a sharp inner rim plus a wider outer falloff. */
QImage rimLight(const QImage &mask, int innerPx, int outerPx,
                const QColor &inner, const QColor &outer, float intensity);

/* Radial light burst. `intensity` 0..1 scales the alpha. */
QImage radialGlow(const QSize &size, const QPointF &center, qreal radius,
                  const QColor &color, float intensity);

/* A soft specular sheet clipped to the subject's silhouette. */
QImage glossOverlay(const QSize &stage, const QImage &subject,
                    const QRectF &subjectRect, const QColor &color);

/* Mirror the subject below itself and fade it out. `fadeStart` is the
 * fraction of the reflection that stays at full opacity. */
QImage reflection(const QImage &subject, qreal opacity, qreal fadeStart);

/* --- electricity -------------------------------------------------------- */

struct ArcParams
{
    float displacement = 0.16f;
    int branches = 2;
    float coreWidth = 2.2f;
    float glowWidth = 13.0f;
    QColor core{ 244, 252, 255 };
    QColor halo{ 0, 234, 255 };
    int sparkCount = 48;
};

/* One time-coherent bolt from `from` to `to`, with branching and sparks. */
QImage electricArc(const QSize &size, const QPointF &from, const QPointF &to,
                   quint32 seed, const ArcParams &params);

/* Boundary-emitted particles: sparks that hug the silhouette and streak
 * outward along the local normal. */
QImage sparkField(const QImage &mask, const QSize &size, quint32 seed,
                  const QVector<QColor> &palette, int count, float energy);

/* --- scene dressing ----------------------------------------------------- */

/* Full-frame neon backdrop: light-stripe texture, diagonal speed rays, a soft
 * focal bloom, stars and a vignette. */
QImage backdrop(const QSize &size, quint32 seed, const QVector<QColor> &palette,
                float strength);

/* Vertical gradient with an extra radial accent. `accentAmount` 0..1. */
QImage gradientField(const QSize &size, const QColor &top, const QColor &bottom,
                     const QColor &accent, qreal accentAmount);

/* Rotated halftone dot field. `dotRadius` is the maximum dot radius. */
QImage halftoneField(const QSize &size, const QColor &color, qreal cell,
                     qreal dotRadius, qreal angleDeg, qreal phase);

/* Defocused light discs. */
QImage bokehField(const QSize &size, quint32 seed, const QList<QColor> &palette,
                  int count, qreal minRadius, qreal maxRadius);

/* Film grain / paper tooth. `amount` is 0..255 peak alpha. */
QImage grainField(const QSize &size, quint32 seed, int amount);

/* Horizontal scan lines. `alpha` is 0..1. */
QImage scanlineField(const QSize &size, const QColor &color, int spacing,
                     qreal alpha);

/* Ink blobs and droplets inside `area`. */
QImage inkSplatter(const QSize &size, quint32 seed, const QRectF &area,
                   const QColor &color, int count);

/* Particle spiral around `center`. */
QImage spiralField(const QSize &size, const QPointF &center, qreal radius,
                   quint32 seed, const QList<QColor> &palette, int count);

/* Confetti ribbon bits. */
QImage confettiField(const QSize &size, quint32 seed,
                     const QList<QColor> &palette, int count, qreal minSize,
                     qreal maxSize);

/* Four-point stars in one colour. */
QImage starField(const QSize &size, quint32 seed, const QColor &color,
                 int count);

/* Four-point stars in a palette. */
QImage starField(const QSize &size, quint32 seed, const QList<QColor> &palette,
                 int count);

/* Small luminous butterflies clustered around `anchors`. */
QImage butterflyField(const QSize &size, quint32 seed,
                      const QList<QColor> &palette,
                      const QList<QPointF> &anchors, int count);

/* Kaleidoscopic repetition of a stage-sized subject canvas. */
QImage wedgeKaleidoscope(const QImage &subjectCanvas, const QSize &stage,
                         int wedges, qreal zoom, qreal rotation);

} // namespace creative
} // namespace paintbrush

#endif // PB_CREATIVE_ART_HPP
