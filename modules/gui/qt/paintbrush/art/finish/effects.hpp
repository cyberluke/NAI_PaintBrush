/*****************************************************************************
 * effects.hpp : NAI Paintbrush — Finish runtime (non-destructive layer effects)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_FINISH_EFFECTS_HPP
#define PB_ART_FINISH_EFFECTS_HPP

#include <QImage>
#include <QColor>
#include <QPointF>
#include <QPoint>
#include <QVector>
#include <QVariantMap>
#include <QString>

#include "../runtime/art_scale.hpp"
#include "../runtime/light_rig.hpp"

namespace paintbrush {
namespace art {

/* Semantic composition role of a layer (spec §20). A small vocabulary that
 * lets Teen Boost reason about hierarchy and motion without exposing dozens
 * of knobs. Roles are metadata; the Finish runtime offers defaults per role,
 * Boost decides when to apply them. */
enum class LayerRole
{
    None = 0,
    Hero,       /* highest clarity, normal/high opacity, strongest priority */
    Support,    /* reduced priority, softer shadow/contrast */
    Ghost,      /* lower opacity, optional desaturation/tint, motion echo */
    Transition  /* may receive motion blur, sits between key poses */
};

QString layerRoleName(LayerRole role);
LayerRole layerRoleFromName(const QString &name);

/* One non-destructive effect node (spec §39: a small internal effect graph
 * per layer — Cutout Layer ├─ Outline ├─ Shadow └─ MotionBlur). Fields are
 * explicit (typed, deterministic) and round-trip through QVariantMap for the
 * project manifest. The raw layer pixels are never modified. */
struct EffectNode
{
    enum Type
    {
        None = 0,
        Shadow,       /* §17: offset, blur, spread, color, opacity; light-rig aware */
        Glow,         /* §18: outer glow — color, radius, intensity, alpha threshold */
        MotionBlur,   /* §19: directed blur — direction, distance, falloff, samples */
        Outline,      /* §16: contour around the alpha */
        Ghost         /* §20: opacity reduction, desaturation, tint */
    };

    int type = None;
    bool enabled = true;

    /* Shadow / Glow / Outline / Ghost */
    QColor color{ 0, 0, 0 };
    float opacity = 0.55f;      /* shadow / glow / ghost strength */
    float blur = 8.0f;          /* shadow blur radius (world units) */
    float spread = 0.0f;        /* shadow spread 0..1 */
    QPointF offset{ 6.0f, 8.0f }; /* shadow offset (world units); overridden by
                                     the light rig when useLightRig */
    bool useLightRig = true;    /* derive shadow direction from the shared rig */
    float lightDistance = 10.0f; /* shadow length (world units) when rig-driven */

    /* Glow */
    float radius = 20.0f;       /* glow radius (world units) */
    float intensity = 1.0f;     /* glow multiplier */
    float threshold = 0.2f;     /* alpha threshold that seeds the glow */

    /* MotionBlur */
    QPointF direction{ 1.0f, 0.0f };
    float distance = 24.0f;     /* blur length (world units) */
    float falloff = 0.6f;       /* 0 uniform .. 1 strongly tapered */
    int samples = 24;

    /* Outline */
    float width = 3.0f;         /* world units */
    float softness = 0.2f;

    /* Ghost */
    float desaturate = 0.4f;    /* 0..1 */
    QColor tint{ 255, 255, 255, 0 }; /* alpha = tint strength */

    QVariantMap toVariant() const;
    static EffectNode fromVariant(const QVariantMap &m);

    static EffectNode makeShadow(float blurWorld = 8.0f, float opacity = 0.5f,
                                 const QColor &color = QColor(0, 0, 0));
    static EffectNode makeGlow(const QColor &color, float radiusWorld = 20.0f,
                               float intensity = 1.0f);
    static EffectNode makeMotionBlur(const QPointF &direction,
                                     float distanceWorld, float falloff = 0.6f,
                                     int samples = 24);
    static EffectNode makeOutline(const QColor &color, float widthWorld = 3.0f,
                                  float softness = 0.2f);
    static EffectNode makeGhost(float opacity = 0.35f, float desaturate = 0.4f,
                                const QColor &tint = QColor(255, 255, 255, 0));
};

/* Result of applying an effect chain: an expanded premultiplied image and the
 * offset (in the source image's pixel space) of its top-left corner, so the
 * compositor can place it with the same layer transform. */
struct FinishResult
{
    QImage image;           /* Format_ARGB32_Premultiplied */
    QPoint originOffset;    /* result(0,0) == source(originOffset) ; negative */
};

/* The Finish runtime. All math is on PREMULTIPLIED alpha (spec §19 "no dark
 * halo", §56 "correct alpha, correct blur/glow math"). Every routine is pure
 * and deterministic. World-unit parameters are converted through ArtScale so
 * the finish reads identically at 1x/2x/4x export (spec §41.3, §55). */
class FinishRuntime
{
public:
    /* Apply `effects` to `source` (any QImage format; converted internally).
     * `scale` converts world units to source pixels; `rig` supplies shadow
     * direction and light temperature. With no enabled effect, the source is
     * returned untouched (zero offset). */
    static FinishResult apply(const QImage &source,
                              const QVector<EffectNode> &effects,
                              const ArtLightRig &rig, const ArtScale &scale);

    /* The padding (in source px) a chain needs around the source bounds. */
    static int paddingFor(const QVector<EffectNode> &effects,
                          const ArtScale &scale);

    /* -- primitives (exposed for the golden-scene harness) --------------- */
    /* Separable 3-pass box blur on premultiplied ARGB32 (≈ Gaussian). */
    static void blurPremultiplied(QImage &premul, int radiusPx);
    /* Directed blur along `direction` over `distancePx` with `samples`
     * taps; falloff 0..1 tapers the trailing samples. Premultiplied in/out. */
    static QImage directedBlur(const QImage &premul, const QPointF &direction,
                               float distancePx, float falloff, int samples);
    /* Alpha-only mask (Format_Alpha8 as Grayscale8) of a premultiplied image. */
    static QImage alphaMask(const QImage &premul);
    /* Shadow direction for a rig: unit xy vector pointing AWAY from the light. */
    static QPointF shadowDirection(const ArtLightRig &rig);

    /* Defaults the Boost art director uses for a role (spec §20). Restrained:
     * Hero gets nothing, Support a soft shadow, Ghost the ghost node, and
     * Transition a ghost + directed blur along `motion`. */
    static QVector<EffectNode> defaultsForRole(LayerRole role,
                                               const QPointF &motion,
                                               float motionDistanceWorld);
};

} // namespace art
} // namespace paintbrush

#endif // PB_ART_FINISH_EFFECTS_HPP
