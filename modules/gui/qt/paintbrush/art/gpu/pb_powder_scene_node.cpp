/*****************************************************************************
 * pb_powder_scene_node.cpp : zero-readback powder pass (Qt Quick / QRhi)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_powder_scene_node.hpp"
#include "shaders_qsb.h"

#include "modules/gui/qt/paintbrush/art/materials/glitter/glitter_powder.hpp"

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <QMatrix4x4>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace paintbrush {
namespace art {

namespace {

QShader shaderFrom(const unsigned char *data, int size)
{
    return QShader::fromSerialized(QByteArray(reinterpret_cast<const char *>(data), size));
}

/* Two CCW triangles; 4 floats per vertex to match the powder pipeline's
 * stride (x, y, 0, 0). */
const float kQuad[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 0.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 0.0f, 0.0f,
};

} // namespace

PowderSceneNode::PowderSceneNode() = default;

PowderSceneNode::~PowderSceneNode()
{
    destroyResources();
}

void PowderSceneNode::setScene(const PowderSceneData &data)
{
    const bool gridChanged = data.gridW != m_data.gridW
                             || data.gridH != m_data.gridH
                             || data.glueW != m_data.glueW
                             || data.glueH != m_data.glueH
                             || data.glueAny != m_data.glueAny;
    m_data = data;
    m_dirty = true;
    /* Only a pack whose window covers the whole grid can be the first content
     * of a freshly created texture. Until then the texels outside the window
     * are undefined, so the pass must not draw (one frame). */
    if (data.gridW > 0 && data.gridH > 0
        && data.cellX == 0 && data.cellY == 0
        && data.cellW == data.gridW && data.cellH == data.gridH)
        m_awaitFullScene = false;
    if (gridChanged && m_resourcesReady)
        destroyResources();
}

QSGRenderNode::StateFlags PowderSceneNode::changedStates() const
{
    return ViewportState | ScissorState | BlendState;
}

void PowderSceneNode::prepare()
{
    QRhiRenderTarget *rt = renderTarget();
    if (!rt)
        return;
    const bool ok = ensureResources(rt->rhi(), rt->renderPassDescriptor());
    if (!ok)
    {
        m_resourcesReady = false;
        m_failed.store(true, std::memory_order_release);
    }
    else
    {
        m_failed.store(false, std::memory_order_release);
    }
}

bool PowderSceneNode::ensureResources(QRhi *rhi, QRhiRenderPassDescriptor *rp)
{
    if (!rhi || !rp || !m_data.valid())
        return false;
    if (m_resourcesReady && m_rhi == rhi && m_rp == rp
        && m_simTex && m_simTex->pixelSize() == QSize(m_data.gridW, m_data.gridH))
        return true;
    destroyResources();
    m_rhi = rhi;
    m_rp = rp;

    const QSize simSize(m_data.gridW, m_data.gridH);
    const QSize glueSize = (m_data.glueAny && m_data.glueW > 0 && m_data.glueH > 0)
                               ? QSize(m_data.glueW, m_data.glueH) : QSize(1, 1);

    m_simTex = rhi->newTexture(QRhiTexture::RGBA16F, simSize, 1, {});
    m_colTex = rhi->newTexture(QRhiTexture::RGBA8, simSize, 1, {});
    m_glueTex = rhi->newTexture(QRhiTexture::R8, glueSize, 1, {});
    if (!m_simTex || !m_colTex || !m_glueTex
        || !m_simTex->create() || !m_colTex->create() || !m_glueTex->create())
    {
        destroyResources();
        return false;
    }

    m_simSampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                   QRhiSampler::None, QRhiSampler::ClampToEdge,
                                   QRhiSampler::ClampToEdge);
    m_colSampler = rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                   QRhiSampler::None, QRhiSampler::ClampToEdge,
                                   QRhiSampler::ClampToEdge);
    m_glueSampler = rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                    QRhiSampler::None, QRhiSampler::ClampToEdge,
                                    QRhiSampler::ClampToEdge);
    if (!m_simSampler->create() || !m_colSampler->create()
        || !m_glueSampler->create())
    {
        destroyResources();
        return false;
    }

    m_ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 512);
    m_vbuf = rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                            sizeof(kQuad));
    if (!m_ubuf || !m_vbuf || !m_ubuf->create() || !m_vbuf->create())
    {
        destroyResources();
        return false;
    }

    m_srb = rhi->newShaderResourceBindings();
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            2, QRhiShaderResourceBinding::VertexStage
                   | QRhiShaderResourceBinding::FragmentStage, m_ubuf),
        QRhiShaderResourceBinding::sampledTexture(
            0, QRhiShaderResourceBinding::FragmentStage, m_simTex, m_simSampler),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_colTex, m_colSampler),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, m_glueTex, m_glueSampler),
    });
    if (!m_srb->create())
    {
        destroyResources();
        return false;
    }

    const QShader vs = shaderFrom(kQSB_POWDER_VERT, int(sizeof(kQSB_POWDER_VERT)));
    const QShader fs = shaderFrom(kQSB_POWDER_FRAG, int(sizeof(kQSB_POWDER_FRAG)));
    if (!vs.isValid() || !fs.isValid())
    {
        destroyResources();
        return false;
    }

    m_pipeline = rhi->newGraphicsPipeline();
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs },
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({ QRhiVertexInputBinding(4 * sizeof(float)) });
    layout.setAttributes({ QRhiVertexInputAttribute(
        0, 0, QRhiVertexInputAttribute::Float2, 0) });
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setShaderResourceBindings(m_srb);
    m_pipeline->setRenderPassDescriptor(rp);
    m_pipeline->setDepthTest(false);
    m_pipeline->setDepthWrite(false);
    m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    /* Premultiplied source-over: the shader emits premultiplied RGB and the
     * paper/tile under the union must remain visible through transparent
     * material (the tile is cleared in the union while this node is active). */
    m_pipeline->setTargetBlends({
        QRhiGraphicsPipeline::TargetBlend{
            QRhiGraphicsPipeline::ColorMask(0xF), true,
            QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha,
            QRhiGraphicsPipeline::Add,
            QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha,
            QRhiGraphicsPipeline::Add }
    });
    if (!m_pipeline->create())
    {
        destroyResources();
        return false;
    }

    /* static vertex data is uploaded once through the run-time batch */
    m_ub.assign(512, '\0');
    m_resourcesReady = true;
    m_glueZeroUploaded = false;
    /* A freshly created texture has no valid content outside the window we
     * upload per frame: if the pending scene already covers the whole grid the
     * very next render can draw it; otherwise wait for the full resync pack. */
    m_awaitFullScene = !(m_data.gridW > 0 && m_data.gridH > 0
                         && m_data.cellX == 0 && m_data.cellY == 0
                         && m_data.cellW == m_data.gridW
                         && m_data.cellH == m_data.gridH);
    m_resync.store(true, std::memory_order_release);
    return true;
}

void PowderSceneNode::destroyResources()
{
    if (m_pipeline) { m_pipeline->destroy(); delete m_pipeline; m_pipeline = nullptr; }
    if (m_srb) { m_srb->destroy(); delete m_srb; m_srb = nullptr; }
    if (m_vbuf) { m_vbuf->destroy(); delete m_vbuf; m_vbuf = nullptr; }
    if (m_ubuf) { m_ubuf->destroy(); delete m_ubuf; m_ubuf = nullptr; }
    if (m_glueSampler) { m_glueSampler->destroy(); delete m_glueSampler; m_glueSampler = nullptr; }
    if (m_colSampler) { m_colSampler->destroy(); delete m_colSampler; m_colSampler = nullptr; }
    if (m_simSampler) { m_simSampler->destroy(); delete m_simSampler; m_simSampler = nullptr; }
    if (m_glueTex) { m_glueTex->destroy(); delete m_glueTex; m_glueTex = nullptr; }
    if (m_colTex) { m_colTex->destroy(); delete m_colTex; m_colTex = nullptr; }
    if (m_simTex) { m_simTex->destroy(); delete m_simTex; m_simTex = nullptr; }
    m_resourcesReady = false;
    m_awaitFullScene = true;
    m_rhi = nullptr;
    m_rp = nullptr;
}

void PowderSceneNode::writeUniforms()
{
    auto wf = [this](int off, float v) {
        std::memcpy(m_ub.data() + off, &v, 4);
    };
    auto wv2 = [this](int off, float a, float b) {
        float t[2] = { a, b };
        std::memcpy(m_ub.data() + off, t, 8);
    };
    auto wv3 = [this](int off, float a, float b, float c) {
        float t[4] = { a, b, c, 0.0f };
        std::memcpy(m_ub.data() + off, t, 16);
    };

    m_ub.fill('\0');
    const QMatrix4x4 ident;
    std::memcpy(m_ub.data() + 0, ident.constData(), 64);
    /* uViewSize is written in render() once the viewport is known; set a
     * sensible value here for the first frame. */
    wv2(64, float(m_rect.width()), float(m_rect.height()));
    wv2(72, float(m_data.worldOrigin.x()), float(m_data.worldOrigin.y()));
    wf(80, m_data.worldPerPx);
    wf(84, m_data.worldCell);
    wf(88, 1.0f);                       /* uDpr (geometry is in device px) */
    wf(92, float(m_data.timeMs / 1000.0)); /* uTime */
    wv2(96, float(m_data.gridW), float(m_data.gridH));
    wf(104, m_data.holo);
    wf(108, m_data.flakePx);
    wf(112, m_data.flakeWorld);
    wf(116, m_data.tooth);
    wf(120, 1.0f);                      /* uMaterialMode: transparent layer */
    wv3(128, m_data.lightDir.x(), m_data.lightDir.y(), m_data.lightDir.z());
    wf(140, m_data.mirror);
    wv3(144, m_data.paper.redF(), m_data.paper.greenF(), m_data.paper.blueF());
    wf(156, m_data.exposure);
    for (int i = 0; i < 16; ++i)
    {
        QColor c = m_data.palette.isEmpty()
                       ? QColor(255, 255, 255)
                       : m_data.palette.at(std::min(i, int(m_data.palette.size()) - 1));
        wv3(160 + i * 16, c.redF(), c.greenF(), c.blueF());
    }
}

void PowderSceneNode::render(const RenderState *state)
{
    if (!state || !m_resourcesReady || !m_rhi || m_data.simHalf.isEmpty())
        return;
    if (m_awaitFullScene)
        return;   /* fresh textures have no valid texels outside the window */
    QRhiRenderTarget *rt = renderTarget();
    QRhiCommandBuffer *cb = commandBuffer();
    if (!rt || !cb)
        return;

    /* ---- viewport from the node rect through the scene matrix ---------- */
    const QMatrix4x4 mvp = *state->projectionMatrix() * *matrix();
    const QVector4D c0 = mvp * QVector4D(float(m_rect.left()), float(m_rect.top()), 0, 1);
    const QVector4D c1 = mvp * QVector4D(float(m_rect.right()), float(m_rect.bottom()), 0, 1);
    if (qFuzzyIsNull(c0.w()) || qFuzzyIsNull(c1.w()))
        return;
    const QSize rtSize = rt->pixelSize();
    const float x0 = (c0.x() / c0.w() * 0.5f + 0.5f) * float(rtSize.width());
    const float y0 = (0.5f - c0.y() / c0.w() * 0.5f) * float(rtSize.height());
    const float x1 = (c1.x() / c1.w() * 0.5f + 0.5f) * float(rtSize.width());
    const float y1 = (0.5f - c1.y() / c1.w() * 0.5f) * float(rtSize.height());
    const int vx = std::clamp(int(std::floor(std::min(x0, x1))), 0, rtSize.width());
    const int vy = std::clamp(int(std::floor(std::min(y0, y1))), 0, rtSize.height());
    const int vw = std::clamp(int(std::ceil(std::abs(x1 - x0))), 0, rtSize.width() - vx);
    const int vh = std::clamp(int(std::ceil(std::abs(y1 - y0))), 0, rtSize.height() - vy);
    if (vw <= 0 || vh <= 0)
        return;

    writeUniforms();
    float view[2] = { float(vw), float(vh) };
    std::memcpy(m_ub.data() + 64, view, 8);

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    if (m_dirty)
    {
        /* PARTIAL upload of the packed window. The texture stays FULL-grid so
         * the shader's world->cell mapping and neighbour reads keep working;
         * only the cells the dirty union touched are transferred. */
        const QSize winSize(m_data.cellW, m_data.cellH);
        const QPoint winPos(m_data.cellX, m_data.cellY);
        {
            QRhiTextureSubresourceUploadDescription d(m_data.simHalf);
            d.setSourceSize(winSize);
            d.setDataStride(quint32(m_data.cellW) * 8u);   /* RGBA16F */
            d.setDestinationTopLeft(winPos);
            u->uploadTexture(m_simTex, { QRhiTextureUploadEntry(0, 0, d) });
        }
        {
            QRhiTextureSubresourceUploadDescription d(m_data.col);
            d.setSourceSize(winSize);
            d.setDataStride(quint32(m_data.cellW) * 4u);   /* RGBA8 */
            d.setDestinationTopLeft(winPos);
            u->uploadTexture(m_colTex, { QRhiTextureUploadEntry(0, 0, d) });
        }
        if (!m_data.glueAny)
        {
            if (!m_glueZeroUploaded)
            {
                const uchar zero[1] = { 0 };
                u->uploadTexture(m_glueTex, { QRhiTextureUploadEntry(
                    0, 0, QRhiTextureSubresourceUploadDescription(
                              QByteArray(reinterpret_cast<const char *>(zero), 1))) });
                m_glueZeroUploaded = true;
            }
        }
        else if (!m_data.glue.isEmpty() && m_data.glueW > 0 && m_data.gridW > 0)
        {
            /* glue is only packed when its revision advanced; the window is in
             * sub-cell units (glueW / gridW samples per cell edge). */
            const int sub = m_data.glueW / m_data.gridW;
            QRhiTextureSubresourceUploadDescription d(m_data.glue);
            d.setSourceSize(QSize(m_data.cellW * sub, m_data.cellH * sub));
            d.setDataStride(quint32(m_data.cellW * sub));   /* R8 */
            d.setDestinationTopLeft(QPoint(m_data.cellX * sub, m_data.cellY * sub));
            u->uploadTexture(m_glueTex, { QRhiTextureUploadEntry(0, 0, d) });
        }
        /* static quad, uploaded once with the first dirty frame */
        u->uploadStaticBuffer(m_vbuf, kQuad);
        m_dirty = false;
    }
    u->updateDynamicBuffer(m_ubuf, 0, 512, m_ub.constData());
    cb->resourceUpdate(u);

    cb->setViewport(QRhiViewport(float(vx), float(vy), float(vw), float(vh)));
    cb->setScissor(QRhiScissor(vx, vy, vw, vh));
    cb->setGraphicsPipeline(m_pipeline);
    cb->setShaderResources(m_srb);
    QRhiCommandBuffer::VertexInput vi(m_vbuf, 0);
    cb->setVertexInput(0, 1, &vi);
    cb->draw(6);
}

void PowderSceneNode::releaseResources()
{
    destroyResources();
}

} // namespace art
} // namespace paintbrush
