/*****************************************************************************
 * pb_gpu_runtime.hpp : NAI Paintbrush — GPU Art Runtime (QRhi)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_GPU_RUNTIME_HPP
#define PB_ART_GPU_RUNTIME_HPP

/* GPU Art Runtime — the Showroom first port (spec §70: GPU polish).
 *
 * The interactive canvas renders paper, glitter and the finish chain through
 * QRhi shaders (the same qsb-baked packs the QSG canvas nodes use) instead of
 * the CPU pixel loops, so 4K canvases stay interactive. The CPU
 * DocumentRenderer remains the GOLDEN reference: export, thumbnails, the AI
 * paths and the acceptance harness all keep using it, and the GPU harness
 * asserts GPU == CPU within tolerance so the shader port can never silently
 * drift from the golden picture.
 *
 * Design:
 *  - one QRhi instance per runtime (D3D11 first, then OpenGL/other backends)
 *  - pipelines built once from the baked QShader packs (shaders_qsb.h)
 *  - renderPaper / renderGlitter / renderFinish render into a QRhiTexture;
 *    readback gives a QImage for the harness / export previews
 *  - deterministic: the shaders are exact ports of the CPU math
 *    (art_noise.hpp, paper_renderer.cpp, glitter_material.cpp, effects.cpp)
 */

#include <QImage>
#include <QSize>
#include <QRect>
#include <QVector>
#include <QColor>
#include <QPointF>

#include <rhi/qshader.h>

#include "../runtime/art_scale.hpp"
#include "../runtime/light_rig.hpp"
#include "../surface/paper_stock.hpp"
#include "../materials/glitter/glitter_material.hpp"
#include "../finish/effects.hpp"

class QRhi;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiRenderPassDescriptor;
class QRhiGraphicsPipeline;
class QRhiBuffer;
class QRhiShaderResourceBindings;
class QRhiResourceUpdateBatch;

namespace paintbrush {
namespace art {

/* GPU runtime: owns the QRhi device, pipelines and scratch resources. One
 * instance per canvas (the Qt Quick window's scene graph runs on the same
 * RHI backend when created via createForWindow()). */
class GpuRuntime
{
public:
    GpuRuntime();
    ~GpuRuntime();

    /* Initialize the backend. `preferD3D11` tries D3D11 first (Windows);
     * returns false when no backend could be created (the caller keeps the
     * CPU path — the golden reference stays authoritative). */
    bool initialize(bool preferD3D11 = true);
    bool isReady() const { return m_rhi != nullptr; }
    QString backendName() const { return m_backendName; }
    QString lastError() const { return m_lastError; }

    /* Render the document paper region into an RGBA image (GPU readback).
     * `region` is the canvas rect; `worldPerPx` converts px -> world units. */
    QImage renderPaper(const QSize &outSize, const PaperStock &stock,
                       const ArtLightRig &rig, const ArtScale &scale,
                       float worldViewX, float worldViewY, float worldPerPx,
                       const QRect &region);

    /* Render a glitter layer from its state into an RGBA image. All grain
     * quads are built on the CPU (one vertex quad per grain, same layout as
     * the CPU golden) and shaded on the GPU with the exact CPU math. */
    QImage renderGlitter(const QSize &outSize, const glitter::GlitterState &state,
                         float outOriginWorldX, float outOriginWorldY,
                         float worldPerPx, const ArtLightRig &rig,
                         const PaperStock *paper);

    /* One finish effect applied to `source` (premultiplied ARGB32).
     * `mode`: 0 DirectedBlur, 1 Shadow, 2 Glow — the same three the canvas
     * nodes render. `scale` converts world units to source pixels. */
    QImage renderFinish(const QImage &source, int mode,
                        const EffectNode &node, const ArtLightRig &rig,
                        const ArtScale &scale);

    /* Release all GPU resources (called before the owning window dies). */
    void shutdown();

private:
    struct PipelineSet;
    PipelineSet *m_pipelines = nullptr;

    QRhi *m_rhi = nullptr;
    QString m_backendName;
    QString m_lastError;

    /* shader packs loaded once (qsb-baked at build time) */
    QShader m_paperVs, m_paperFs;
    QShader m_glitterVs, m_glitterFs;
    QShader m_finishVs, m_finishFs;

    /* scratch resources for readback */
    QRhiTexture *m_target = nullptr;
    QRhiTextureRenderTarget *m_rt = nullptr;
    QRhiRenderPassDescriptor *m_rtRp = nullptr;
    /* the render pass descriptor the current pipelines were built against;
     * pipelines are rebuilt when the target grows (new descriptor) */
    QRhiRenderPassDescriptor *m_pipelinesRp = nullptr;
    /* the finish SRB needs a valid texture at create time; the 1x1 dummy
     * stays alive for the runtime's lifetime */
    QRhiTexture *m_finishDummy = nullptr;

    QImage readback(QRhiTexture *tex, const QSize &size);
    bool ensureTarget(const QSize &size);
    void releaseTarget();
    bool ensurePipelines();   /* once the render target's RP desc exists */
};

} // namespace art
} // namespace paintbrush

#endif // PB_ART_GPU_RUNTIME_HPP