/*****************************************************************************
 * pb_puppet.hpp : NAI Paintbrush — Motion Toys: 2D puppet runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_PUPPET_HPP
#define PB_PUPPET_HPP

/* Motion Toys — the AnimatedDrawings-style experiment, isolated.
 *
 * This is an ALGORITHM QUARRY (same standing as glitter-powder and
 * react-artboard): the ideas behind Meta's AnimatedDrawings (MIT) —
 * textured 2D mesh + 2D skeleton + ARAP deformation — ported into a
 * deterministic, dependency-free runtime. No pose AI, no BVH import, no
 * TorchServe, no GPU: the first spike only answers one question:
 *
 *   "Can a harvested photographic person be deformed ±5–10° and still
 *    look like a person and not a rubber demon?"
 *
 * Pipeline (deterministic, seedable, byte-exact serializable):
 *
 *   RGBA texture + binary mask
 *     -> PuppetBuilder: contour -> simplification -> ear-clip triangulation
 *        -> 1-4 subdivision (interior verts) -> synthetic humanoid skeleton
 *     -> PuppetDeformer: FK pose -> ARAP local-global solve (2D closed form)
 *     -> PuppetRenderer: canonical barycentric textured-mesh raster
 *
 * Design rules:
 *  - No VLC headers, no QObject, no moc. Pure C++17 + QtCore/QtGui value
 *    types so the acceptance harness compiles the REAL sources directly.
 *  - Everything is deterministic: fixed iteration counts, no RNG beyond
 *    the seedable hash, integer-quantized hashing for rig identity.
 *  - Loopable: every built-in motion is a pure function of phase with
 *    pose(0) == pose(duration) (Loop), pose(0) == rest and hold (Once),
 *    and a folded phase for PingPong.
 *  - Photo regime (HarvestItem of a real person): keep amplitudes small —
 *    the PuppetEffect amplitude defaults are tuned for ±2–6°; the harness
 *    verifies the ±10–15° boundary and the "rubber demon" failure mode.
 */

#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>

#include <array>

namespace paintbrush {
namespace motion {

/* ------------------------------------------------------------------ */
/* Core value types                                                    */
/* ------------------------------------------------------------------ */

/* A joint in the 2D skeleton. `rest` is in cutout pixel space. The FK
 * chain rotates the child segment around the parent's current position. */
struct PuppetJoint
{
    int parent = -1;      /* index into PuppetSkeleton::joints, -1 = root */
    QPointF rest;         /* rest position in cutout pixels */
    QString name;         /* diagnostics only */
};

struct PuppetSkeleton
{
    QVector<PuppetJoint> joints;

    int depth(int j) const;      /* chain length from the root */
    bool isValid() const;        /* parents valid, root exists */
};

/* Triangle mesh in cutout pixel space. `verts` are ordered: boundary
 * vertices first (contour), then interior vertices (subdivision). */
struct PuppetMesh
{
    QVector<QPointF> verts;
    QVector<std::array<int, 3>> tris;
    bool isValid() const;
};

/* The complete rig: texture + mask + mesh + skeleton + handle mapping.
 * `handleVerts[j]` = mesh vertex that follows skeleton joint j in the
 * ARAP solve (-1 when unmapped). */
struct PuppetRig
{
    QImage texture;              /* ARGB32 straight, cutout-sized */
    QImage mask;                 /* Grayscale8 binary, cutout-sized */
    PuppetMesh mesh;
    PuppetSkeleton skeleton;
    QVector<int> handleVerts;    /* size == skeleton.joints.size() */
    QString rigId;               /* content hash, stable per input */
    QRect bounds;                /* texture bounds (cutout size) */

    bool isValid() const
    {
        return !texture.isNull() && mesh.isValid() && skeleton.isValid()
            && handleVerts.size() == skeleton.joints.size();
    }
};

/* A pose: one rotation (degrees) per joint applied on top of the rest
 * chain, plus a root translation (bounce). rest() is all zeros. */
struct PuppetPose
{
    QVector<float> anglesDeg;    /* size == joints count */
    QPointF rootOffset;          /* cutout pixels, applied to the root */

    static PuppetPose rest(const PuppetSkeleton &skeleton);
    bool isRest() const;
    /* Uniform amplitude: angles and root offset scaled by `a`. */
    PuppetPose scaled(float a) const;
};

/* ------------------------------------------------------------------ */
/* PuppetEffect — the user-facing "living asset" knob                 */
/* ------------------------------------------------------------------ */

enum class MotionKind : quint8
{
    Sway = 0,      /* torso roll + counter head + arm follow-through */
    Bounce,        /* root hop + limb compression, seamless sin² cycle */
    Breath,        /* ribcage/shoulder micro motion, no head movement */
    HeadTilt,      /* head roll only */
    Composite,     /* all four summed at reduced weights */
    Custom         /* harness/app supplies poses directly */
};

enum class LoopMode : quint8
{
    Once = 0,      /* one full cycle, then hold rest */
    Loop,          /* seamless: pose(0) == pose(duration) */
    PingPong       /* fold the phase back and forth */
};

struct PuppetEffect
{
    MotionKind kind = MotionKind::Sway;
    float amplitude = 1.0f;      /* 0..2; 1.0 == authored subtle default */
    float speed = 1.0f;          /* time scale (2.0 == twice as fast) */
    LoopMode loop = LoopMode::Loop;
    quint32 seed = 0;            /* per-joint phase offsets */
    quint32 durationMs = 3200;   /* loop period for this effect */
    /* initial phase 0..1 of the loop (for clips: shifts poseAt sampling) */
    float phase = 0.0f;

    /* Byte-exact deterministic serialization (versioned; v1 = 19 B without
     * phase, v2 = 23 B with phase). */
    QByteArray serialize() const;
    static PuppetEffect deserialize(const QByteArray &data);
    bool operator==(const PuppetEffect &o) const;
};

/* ------------------------------------------------------------------ */
/* PuppetMotionClip — keyframed pose track (source-motion carrier)     */
/* ------------------------------------------------------------------ */

struct PuppetClipKey
{
    qint64 timeMs = 0;             /* absolute, sorted; first key at 0 */
    PuppetPose pose;
};

/* A deterministic keyframe pose track: the carrier for source motion
 * (video-derived mask trajectories or authored keyframes). Linear angle
 * interpolation, loop folding identical to PuppetMotion. A clip is
 * rig-independent (poses carry their own sizes); poseAt validates against
 * the skeleton it is sampled with. */
struct PuppetMotionClip
{
    QVector<PuppetClipKey> keys;   /* sorted by timeMs; first at 0 */
    LoopMode loop = LoopMode::Loop;

    qint64 durationMs() const;     /* last key time (>= 1) */
    bool isValid(const PuppetSkeleton &skel) const;
    /* Sampled pose at timeMs (loop folding; once = hold last key). */
    PuppetPose poseAt(const PuppetSkeleton &skel, qint64 timeMs) const;

    /* Byte-exact deterministic serialization (versioned). */
    QByteArray serialize() const;
    static PuppetMotionClip deserialize(const QByteArray &data);
};

/* ------------------------------------------------------------------ */
/* PuppetAsset — the Living Harvest product object                     */
/* ------------------------------------------------------------------ */

/* Photo regime = the ±12°-adapted, photo-safe motion (the product default).
 * Illustration regime = unclamped / full retargeted dance (later stage). */
enum class PuppetRegime : quint8
{
    Photo = 0,
    Illustration = 1
};

/* One rigged harvest: the immutable HarvestItem's RGBA + mask are rebuilt
 * into a rig ONCE, a photo-safe motion clip is attached, and the layer only
 * references the asset id + a PuppetEffect. The original HarvestItem bitmap
 * is never modified (non-destructive enrichment). */
struct PuppetAsset
{
    int id = 0;                  /* stable per-document asset id */
    int sourceHarvestId = 0;     /* the HarvestItem this was rigged from */
    PuppetRig rig;               /* mesh + skeleton + handles (derived) */
    PuppetMotionClip motion;     /* photo-adapted clip (never raw source) */
    PuppetRegime regime = PuppetRegime::Photo;
    float defaultAmplitude = 0.65f; /* product default (Normal) */
    QString name;

    bool isValid() const
    {
        return id > 0 && sourceHarvestId > 0 && rig.isValid()
            && motion.keys.size() >= 2;
    }
    /* Rig-less manifest (persistence; the rig is rebuilt from the harvest
     * on load, deterministically). */
    QByteArray serializeManifest() const;
    static PuppetAsset fromManifest(const QByteArray &data, const PuppetRig &rig);
};

/* ------------------------------------------------------------------ */
/* PuppetMotionScanner — automatic source-motion window                */
/* ------------------------------------------------------------------ */

struct PuppetMotionScanner
{
    /* One mask sample on the source track (frame-space or trimmed — the
     * scoring uses angles, which are translation/scale invariant). */
    struct FrameSample
    {
        qint64 ptsMs = 0;
        QImage mask;             /* grayscale binary mask */
        double quality = 1.0;    /* mask quality 0..1 (SAM score) */
    };

    struct ScanOptions
    {
        int minSpanFrames = 3;   /* clip length in samples (loop closes) */
        int maxSpanFrames = 6;
        qint64 stepMs = 200;
        double minVariation = 0.5;   /* deg: mean per-joint std required */
        double maxEnergy = 20.0;     /* deg: mean |angle| ceiling */
        double variationWeight = 1.0;
        double energyPenalty = 1.0;
        double characterWeight = 1.0;/* match to the harvested pose */
        double closureWeight = 2.0;  /* loop closure quality */
        double occlusionWeight = 1.0;/* mask-area variance (occlusion) */
        double anchorBonus = 1.5;    /* window starting at the harvest */
    };

    struct ScanResult
    {
        bool ok = false;
        int startIndex = -1;     /* sample index of the window start */
        int spanFrames = 0;
        double score = 0.0;
        PuppetSkeleton sourceSkeleton;  /* fit of the window's first mask */
        PuppetMotionClip clip;          /* closed loop, window-relative */
    };

    /* Deterministic window search over the same-track samples: windows of
     * minSpan..maxSpan frames scored by pose variation, loop closure cost,
     * harvested-pose character match, motion energy, mask quality/occlusion
     * and a bonus for windows anchored at the harvest PTS. Windows starting
     * at the harvest frame close the loop exactly (t=0 == the static
     * figure). Returns ok=false when no window has enough variation. */
    static ScanResult scan(const QVector<FrameSample> &samples,
                           qint64 harvestPtsMs,
                           const PuppetSkeleton &harvestSkeleton,
                           const ScanOptions &opts);
    /* Default options. */
    static ScanResult scan(const QVector<FrameSample> &samples,
                           qint64 harvestPtsMs,
                           const PuppetSkeleton &harvestSkeleton);

    /* Procedural fallback clip when the track has no usable source window:
     * a subtle seeded sway on the TARGET rig (photo-safe by construction). */
    static PuppetMotionClip proceduralFallback(const PuppetRig &rig,
                                               quint32 durationMs = 3200,
                                               quint32 seed = 11);
};

/* ------------------------------------------------------------------ */
/* PuppetBuilder — mask -> mesh + skeleton                             */
/* ------------------------------------------------------------------ */

struct PuppetBuilderConfig
{
    int maxBoundaryVerts = 128;  /* contour simplification target */
    int maxSubdivisionRounds = 2;/* 1-4 interior subdivision rounds */
    int maxVerts = 900;
    bool fitHumanoid = true;     /* synthetic 10-joint humanoid fit */
};

struct PuppetBuilder
{
    /* Build a rig from a cutout RGBA texture and a binary mask. When the
     * mask is null the texture alpha is used. Returns an invalid rig for
     * an empty mask. Deterministic for the same input. */
    static PuppetRig build(const QImage &rgba, const QImage &mask,
                           const PuppetBuilderConfig &config = PuppetBuilderConfig());

    /* The synthetic default: 10 joints (root/chest/neck/head + shoulders/
     * elbows + hips), fitted from the mask's row profile. Every joint is
     * guaranteed inside the mask. */
    static PuppetSkeleton fitDefaultHumanoid(const QImage &mask);

    /* Contour -> simplified polygon (for diagnostics/tests). */
    static QVector<QPointF> extractContour(const QImage &mask, float epsilonPx);
};

/* ------------------------------------------------------------------ */
/* PuppetDeformer — FK + ARAP                                          */
/* ------------------------------------------------------------------ */

struct PuppetDeformer
{
    struct Options
    {
        int iterations = 10;         /* local-global ARAP sweeps (damped) */
        int cgIterations = 120;      /* global step (SPD, matrix-free) */
        float handleWeight = 1.0e4f; /* soft pin strength */
        float cgTolerance = 1.0e-9f;
    };

    /* Forward kinematics: joint target positions for a pose. */
    static QVector<QPointF> fk(const PuppetRig &rig, const PuppetPose &pose);
    /* Skeleton-only FK (no rig needed — e.g. source-skeleton roundtrips). */
    static QVector<QPointF> fk(const PuppetSkeleton &skel, const PuppetPose &pose);

    /* ARAP: original joints -> target joints -> deformed vertices.
     * Identity pose returns the rest vertices (within 1e-3). */
    static QVector<QPointF> deform(const PuppetRig &rig, const PuppetPose &pose);
    static QVector<QPointF> deform(const PuppetRig &rig, const PuppetPose &pose,
                                   const Options &opts);
};

/* ------------------------------------------------------------------ */
/* PuppetRenderer — canonical textured-mesh raster                     */
/* ------------------------------------------------------------------ */

struct PuppetRenderer
{
    /* Rasterize the deformed mesh with bilinear texture sampling in REST
     * UV space. Alpha comes from the mask (fallback: texture alpha).
     * Transparent outside the mesh. Deterministic. */
    static QImage render(const QImage &texture, const QImage &mask,
                         const QVector<QPointF> &restVerts,
                         const QVector<QPointF> &deformedVerts,
                         const QVector<std::array<int, 3>> &tris);
};

/* ------------------------------------------------------------------ */
/* PuppetMotion — deterministic, loopable procedural motions           */
/* ------------------------------------------------------------------ */

struct PuppetMotion
{
    /* The pose at timeMs for an effect (loop folding applied). */
    static PuppetPose poseAt(const PuppetRig &rig, const PuppetEffect &effect,
                             qint64 timeMs);

    /* Raw generator for one kind at phase t in [0,1) of the period.
     * amplitude multiplies the authored base degrees. */
    static PuppetPose sample(const PuppetRig &rig, MotionKind kind, float t01,
                             float amplitude, quint32 seed);
};

/* ------------------------------------------------------------------ */
/* PuppetRetargeter — source skeleton -> Paintbrush skeleton           */
/* ------------------------------------------------------------------ */

struct PuppetRetargeter
{
    struct Options
    {
        float amplitudeScale = 1.0f; /* global gain on transferred angles */
        bool clamp = true;           /* photo-regime clamp (see below) */
        float maxJointDeg = 12.0f;   /* per-joint ceiling (±10-15° rule) */
        /* root-displacement ceiling: fraction of the TARGET skeleton height
         * the retargeted track may move the root (gross source-frame drift
         * must not drag the sticker around) */
        float maxRootFrac = 0.02f;
    };

    /* A pose track lifted from a mask sequence (the source video): the
     * skeleton is fit per mask (fitDefaultHumanoid), the first fit is the
     * source rest, and every frame's joint positions are decomposed into
     * per-joint angles relative to that rest (inverse FK). The clip is
     * loop-closed (last key == first key) by default. */
    struct PoseTrackResult
    {
        PuppetSkeleton sourceSkeleton;
        PuppetMotionClip clip;
        bool isValid() const
        {
            return sourceSkeleton.isValid() && clip.keys.size() >= 2;
        }
    };

    /* Inverse FK: joint world positions -> per-joint angles + root offset
     * relative to the skeleton's rest chain. */
    static PuppetPose poseFromPositions(const PuppetSkeleton &skel,
                                        const QVector<QPointF> &world);
    static PoseTrackResult poseTrackFromMasks(const QVector<QImage> &masks,
                                              qint64 stepMs,
                                              bool closeLoop = true);
    /* Map one source pose onto the target skeleton: joints are matched by
     * normalized name (with aliases), angles transfer with amplitudeScale,
     * optional per-joint clamp; the root offset scales by the target/source
     * height ratio. Unmapped target joints stay at rest. */
    static PuppetPose retarget(const PuppetSkeleton &source,
                               const PuppetSkeleton &target,
                               const PuppetPose &sourcePose);
    static PuppetPose retarget(const PuppetSkeleton &source,
                               const PuppetSkeleton &target,
                               const PuppetPose &sourcePose,
                               const Options &opts);
    /* Clip-level retarget with PHOTO-REGIME ADAPTATION: per mapped joint
     * the track is scaled down so its peak |angle| fits maxJointDeg (the
     * ±10-15° product rule) — the motion DIRECTION is preserved, its
     * magnitude is bounded. Small source motions pass through untouched. */
    static PuppetMotionClip retargetClip(const PuppetSkeleton &source,
                                         const PuppetSkeleton &target,
                                         const PuppetMotionClip &clip);
    static PuppetMotionClip retargetClip(const PuppetSkeleton &source,
                                         const PuppetSkeleton &target,
                                         const PuppetMotionClip &clip,
                                         const Options &opts);
};

/* ------------------------------------------------------------------ */
/* PuppetRuntime — facade                                              */
/* ------------------------------------------------------------------ */

struct PuppetRuntime
{
    static PuppetRig build(const QImage &rgba, const QImage &mask,
                           const PuppetBuilderConfig &config = PuppetBuilderConfig());
    /* Deformed + rendered frame for an effect at a time. */
    static QImage renderFrame(const PuppetRig &rig, const PuppetEffect &effect,
                              qint64 timeMs);
    /* Clip frame: sample the track at timeMs*speed, scale the pose by
     * effect.amplitude, deform + render. The clip's own loop mode applies. */
    static QImage renderClipFrame(const PuppetRig &rig, const PuppetMotionClip &clip,
                                  const PuppetEffect &effect, qint64 timeMs);
    /* Living Harvest frame: the asset's photo-adapted clip sampled at
     * (timeMs + phase*period) * speed, scaled by amplitude, deformed and
     * rendered on the asset's rig. timeMs 0 with phase 0 renders exactly
     * the static harvested figure (the clip's first key is the rest pose
     * for harvest-anchored windows). */
    static QImage renderAtTime(const PuppetAsset &asset,
                               const PuppetEffect &effect, qint64 timeMs);
    /* Deterministic content hash (FNV-1a 64, hex) of a rig. */
    static QString rigId(const PuppetRig &rig);
};

/* Deterministic [0,1) hash used for seed-based per-joint phases. */
float puppetHash01(quint32 seed, int jointIndex);

} // namespace motion
} // namespace paintbrush

#endif // PB_PUPPET_HPP