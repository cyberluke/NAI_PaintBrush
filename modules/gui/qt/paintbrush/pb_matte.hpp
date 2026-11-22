/*****************************************************************************
 * pb_matte.hpp : NAI Paintbrush — neural matting and rotoscoping core
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************
 * Three independently represented quantities (the architectural rule of this
 * subsystem):
 *
 *   semantics  — WHAT the subject is (SAM3 binary mask, never overwritten)
 *   coverage   — HOW MUCH of each pixel the subject covers (continuous alpha)
 *   colour     — WHAT COLOUR the subject has without the original background
 *
 * This header is pure Qt: no VLC, no CUDA, no ONNX. It can be compiled and
 * asserted by a standalone harness, and the same code runs inside the GUI.
 * The neural inference that turns a trimap into a fractional matte lives
 * behind the versioned C ABI in gpu/matting_cuda_api.h; it never replaces the
 * semantic mask produced here.
 *****************************************************************************/

#ifndef PB_MATTE_HPP
#define PB_MATTE_HPP

#include <QImage>
#include <QMetaType>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <vector>

namespace paintbrush {
namespace matte {

/* The representation a stored subject mask currently has. Refinement always
 * ADDS a representation; it never destroys the previous one. */
enum class Representation {
    SemanticBinary = 0,     /* SAM3 0/255 mask                             */
    SemanticProbability = 1,/* optional per-pixel probability when exposed  */
    RefinedAlpha = 2,       /* continuous fractional coverage               */
};

/* Quality preset. Controls the model input resolution, not subject identity. */
enum class Quality {
    Preview = 0,    /* 512 reference short edge                       */
    Balanced = 1,   /* 1024 reference short edge                      */
    Maximum = 2,    /* native subject region where feasible (<= 2048) */
};

/* User-facing refinement mode (Paintbrush tool options). */
enum class Mode {
    Raw = 0,        /* the untouched SAM3 semantic mask               */
    Refined = 1,    /* deterministic topology + edge refinement       */
    Neural = 2,     /* neural alpha + foreground reconstruction       */
};

/* All distances are expressed at a 1080-pixel reference short edge and are
 * scaled to the actual source resolution by scaleForShortEdge(). These are
 * initial calibration values, not validated optima. */
struct Parameters
{
    int referenceShortEdge = 1080;
    int innerBandPx = 4;         /* confidently opaque inside the boundary    */
    int outerBandPx = 8;         /* confidently background outside it         */
    int fineDetailBandPx = 16;   /* widened band around hair/fur/motion blur  */
    int maxBandPx = 32;          /* hard ceiling for adaptive expansion       */

    double detailRecovery = 0.65;      /* 0..1 widen unknown around detail    */
    double artifactSuppression = 0.55; /* 0..1 remove detached specks         */
    double holeRecovery = 0.55;        /* 0..1 restore enclosed false holes   */
    double decontamination = 0.75;     /* 0..1 foreground colour reconstruction*/
    double temporalStability = 0.5;    /* 0..1 motion-compensated alpha fusion */
    bool restoreEnclosedHoles = true;  /* master switch for hole recovery     */
    bool keepUserComponents = true;    /* never drop a positively seeded part */

    int innerBandFor(int shortEdge) const;
    int outerBandFor(int shortEdge) const;
    int detailBandFor(int shortEdge) const;
    int maxBandFor(int shortEdge) const;
    double scaleFor(int shortEdge) const;
};

/* One analysed foreground component. */
struct Component
{
    enum class Kind { Foreground, EnclosedHole };

    Kind kind = Kind::Foreground;
    int label = 0;
    int area = 0;                 /* pixels                                  */
    double relativeArea = 0.0;    /* area / largest foreground component     */
    double distanceToMain = 0.0;  /* px from the main component (0 = main)   */
    double meanColorDistance = 0.0; /* hole interior vs surrounding ring     */
    bool userSeeded = false;      /* a positive prompt lands inside          */
    bool userExcluded = false;    /* a negative prompt lands inside          */
    bool kept = true;             /* retained as real subject                */
    bool filled = false;          /* enclosed hole restored to foreground    */
    bool deferredToUnknown = false; /* left transparent but marked uncertain */
    QString reason;               /* human-readable decision                 */
    QRect bounds;
};

/* Result of the deterministic topology stage. `repairedMask` is binary; the
 * original mask is never modified. */
struct TopologyReport
{
    int detachedFound = 0;
    int detachedRemoved = 0;
    int detachedKept = 0;
    int holesFound = 0;
    int holesFilled = 0;
    int holesKept = 0;
    int holesDeferred = 0;
    QVector<Component> components;

    QImage repairedMask;   /* 0/255 after component decisions              */
    QImage removedMask;    /* 0/255 pixels dropped as artifacts            */
    QImage filledHoleMask; /* 0/255 hole pixels restored to foreground     */
    QImage deferredHoleMask; /* 0/255 holes that must go to the unknown    */

    bool isEmpty() const { return repairedMask.isNull(); }
};

/* Normalized 16-bit production alpha (FP16-equivalent storage precision).
 * FP32 is used during processing; the stored plane keeps at least 16 bits so
 * the value never round-trips through 8-bit by accident. */
class AlphaPlane
{
public:
    AlphaPlane() = default;
    AlphaPlane(int width, int height);

    bool isNull() const { return m_width <= 0 || m_height <= 0; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    QSize size() const { return QSize(m_width, m_height); }

    void fill(float value);
    float at(int x, int y) const;
    void setAt(int x, int y, float value);

    const quint16 *constData() const { return m_data.data(); }
    quint16 *data() { return m_data.data(); }
    const std::vector<quint16> &raw() const { return m_data; }

    /* Derived views (never the authoritative value). */
    QImage toGrayscale8() const;
    QImage toGrayscale16() const;
    QImage toPremultipliedRgba(const QImage &foregroundRgb) const;

    /* Replace the plane from an external FP32 buffer (already cropped). */
    static AlphaPlane fromFloat(const float *values, int width, int height);

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<quint16> m_data;
};

/* Diagnostic bundle for the §15 "diagnostic display mode". All images are
 * source-space and optional: only the requested views are materialized. */
struct Diagnostics
{
    bool enabled = false;
    QImage original;         /* the decoded source frame region            */
    QImage semanticMask;     /* SAM3 binary                                */
    QImage repairedMask;     /* topology-repaired binary                   */
    QImage trimap;           /* 0 = bg, 128 = unknown, 255 = fg            */
    QImage trimapColor;      /* colour-coded: black / green / white        */
    QImage alphaPreview;     /* grayscale alpha                            */
    QImage backgroundEstimate;
    QImage foregroundRgb;    /* decontaminated straight colour             */
    QImage premultipliedRgba;
};

/* Inputs for a refinement of one frame/subject. `mask` is authoritative and
 * is never modified. `probability` may be null (the SAM3 shim exposes only a
 * binary mask today); when it is null the trimap uncertainty is derived from
 * geometry and image structure and is NOT presented as model confidence.
 * Prompt points are in mask/frame pixel coordinates. */
struct Inputs
{
    QImage rgb;
    QImage mask;
    QImage probability;
    QVector<QPointF> positivePoints;
    QVector<QPointF> negativePoints;
    QRect roi;               /* optional subject ROI; empty = whole frame   */
};

struct Result
{
    AlphaPlane alpha;
    QImage foregroundRgb;
    QImage premultipliedRgba;
    QImage trimap;
    TopologyReport topology;
    Diagnostics diagnostics;
    QString backend;          /* "refined", "vitmatte-s/512", ...           */
    QString modelRevision;
    double inferenceMs = -1.0;
    double trimapUnknownFraction = 0.0;
    int width = 0;
    int height = 0;

    bool isValid() const { return !alpha.isNull(); }
};

/* ---- deterministic stages (always available, no model needed) ---------- */

/* Connected-component topology analysis + repair. */
TopologyReport analyseTopology(const Inputs &inputs, const Parameters &params);

/* Adaptive trimap from the repaired binary mask. `signedDistanceOut` and
 * `detailOut` are optional debug/quality signals. */
QImage buildTrimap(const QImage &repairedMask, const QImage &rgb,
                   const QImage &probability, const TopologyReport &topology,
                   const Parameters &params,
                   QImage *signedDistanceOut = nullptr,
                   QImage *detailOut = nullptr,
                   double *unknownFractionOut = nullptr);

/* Deterministic fractional alpha from the signed field: a sub-pixel edge over
 * the inner/outer band plus the detail-aware widening. This is the "Refined"
 * mode — honest edge refinement, not neural matting. */
AlphaPlane refineAlphaDeterministic(const QImage &repairedMask,
                                    const QImage &rgb,
                                    const Parameters &params,
                                    const QImage &trimap = QImage());

/* Full deterministic pipeline (topology -> trimap -> alpha -> foreground).
 * No model, no GPU. Used by Refined mode and as the baseline the neural stage
 * starts from. */
Result refineDeterministic(const Inputs &inputs, const Parameters &params);

/* Foreground colour reconstruction + premultiplied RGBA. `alpha` must be in
 * rgb coordinates. `trimap` supplies the confirmed foreground/background. */
QImage reconstructForeground(const QImage &rgb, const AlphaPlane &alpha,
                             const QImage &trimap,
                             const Parameters &params,
                             QImage *backgroundOut = nullptr,
                             QImage *foregroundOut = nullptr);

/* Straight ARGB32 for the document's layer storage (which is straight alpha).
 * Keeps the decontaminated foreground colour under fully transparent pixels so
 * texture filtering cannot darken or brighten the edge. This is the ONE
 * premultiplied -> straight conversion at the storage boundary; the
 * premultiplied texture remains the authoritative processing form. */
QImage straightRgba(const QImage &foregroundRgb, const AlphaPlane &alpha);

/* ---- neural stage ------------------------------------------------------ */

/* Attach a neural alpha (model space already mapped back to source pixels by
 * the runtime) to the deterministic result: replaces the alpha plane, then
 * reconstructs foreground colour with the same decontamination code. */
Result applyNeuralAlpha(const Inputs &inputs, Result result,
                        const AlphaPlane &neuralAlpha,
                        const Parameters &params,
                        const QString &backend,
                        const QString &modelRevision,
                        double inferenceMs);

/* ---- helpers shared with the runtime and the harness ------------------- */

/* Exact Euclidean distance transform (Felzenszwalb-Huttenlocher), squared
 * distances. `seeds` is 0/255; seed pixels have distance 0. */
QImage distanceTransformSquared(const QImage &seeds);

/* Signed distance in pixels: negative inside the mask, positive outside. */
QImage signedDistance(const QImage &binaryMask);

/* Separable binary morphology (disk approximated by a square; radius 0 = no
 * op). Used to build constraints and diagnostics, never as the final matte. */
QImage erodeBinary(const QImage &binaryMask, int radius);
QImage dilateBinary(const QImage &binaryMask, int radius);

/* Convert a mask of any format into a 0/255 edge-preserving grayscale image.
 * Unlike pb_common's maskToImage() this NEVER thresholds fractional values. */
QImage levelsOf(const QImage &mask);

/* True when the mask contains fractional values (any pixel not 0/255). */
bool hasFractionalLevels(const QImage &mask);

/* Geometry identity used by the cache/revision contract. */
struct FrameIdentity
{
    QString mediaId;
    qint64 ptsMs = 0;
    QString subjectId;
    quint64 selectionRevision = 0;
    quint64 refinementRevision = 0;
    quint64 mediaGeneration = 0;

    bool isValid() const { return !mediaId.isEmpty() && !subjectId.isEmpty(); }
    QString cacheKey(const Parameters &params, const QString &backend) const;
};

} // namespace matte
} // namespace paintbrush

/* Qt queued signals carry these across the worker/GUI boundary. */
Q_DECLARE_METATYPE(paintbrush::matte::Result)
Q_DECLARE_METATYPE(paintbrush::matte::FrameIdentity)

#endif // PB_MATTE_HPP
