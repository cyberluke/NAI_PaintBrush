/* PB_POWDER_SURFACE.CPP — see pb_powder_surface.hpp. */
#include "pb_powder_surface.hpp"

#include "shaders_qsb.h"
#include "../materials/glitter/glitter_powder.hpp"

#include <QMatrix4x4>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace paintbrush {
namespace art {

namespace {
QShader fromSerialized(const QByteArray &b)
{
    return QShader::fromSerialized(b);
}
QShader powderVs()
{
    return fromSerialized(QByteArray(reinterpret_cast<const char *>(kQSB_POWDER_VERT),
                                     int(sizeof(kQSB_POWDER_VERT))));
}
QShader powderFs()
{
    return fromSerialized(QByteArray(reinterpret_cast<const char *>(kQSB_POWDER_FRAG),
                                     int(sizeof(kQSB_POWDER_FRAG))));
}

/* full-screen quad, clip space (-1..1); 6 verts as a triangle list */
const float kQuadClip[12] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
     1.0f,  1.0f,
    -1.0f, -1.0f,
     1.0f,  1.0f,
    -1.0f,  1.0f,
};

/* manual IEEE-754 binary16 (Qt 6.11 has no qfloat16::rawBits) */
quint16 floatToHalf(float f)
{
    union { float f; quint32 u; } in;
    in.f = f;
    const quint32 u = in.u;
    const quint32 sign = (u >> 16) & 0x8000u;
    quint32 exp = (u >> 23) & 0xFFu;
    quint32 mant = u & 0x7FFFFFu;
    if (exp == 0xFF) {                       /* inf / nan */
        return quint16(sign | 0x7C00u | (mant ? 0x0200u : 0u));
    }
    const int e = int(exp) - 127 + 15;
    if (e >= 0x1F)
        return quint16(sign | 0x7C00u);      /* overflow -> inf */
    if (e <= 0) {                            /* subnormal / zero */
        if (e < -10)
            return quint16(sign);
        mant |= 0x800000u;
        const int shift = 14 - e;
        return quint16(sign | (mant >> shift));
    }
    return quint16(sign | quint32(e) << 10 | (mant >> 13));
}

QRhiGraphicsPipeline *buildPipeline(QRhi *rhi, const QShader &vs, const QShader &fs,
                                    QRhiShaderResourceBindings *srb, bool blendPremultiplied,
                                    QRhiRenderPassDescriptor *rp)
{
    QRhiGraphicsPipeline *ps = rhi->newGraphicsPipeline();
    ps->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs },
    });
    QRhiVertexInputLayout il;
    il.setBindings({ QRhiVertexInputBinding(2 * sizeof(float)) });
    il.setAttributes({ QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0) });
    ps->setVertexInputLayout(il);
    ps->setShaderResourceBindings(srb);
    ps->setRenderPassDescriptor(rp);
    ps->setDepthTest(false);
    ps->setDepthWrite(false);
    ps->setCullMode(QRhiGraphicsPipeline::None);
    if (blendPremultiplied)
        ps->setTargetBlends({ QRhiGraphicsPipeline::TargetBlend{
            QRhiGraphicsPipeline::ColorMask(0xF), true,
            QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha,
            QRhiGraphicsPipeline::Add,
            QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha,
            QRhiGraphicsPipeline::Add } });
    if (!ps->create()) {
        delete ps;
        return nullptr;
    }
    return ps;
}

} /* namespace */

PowderSurface::~PowderSurface()
{
    release();
}

bool PowderSurface::init(QRhi *rhi, QRhiRenderPassDescriptor *rp, bool blendPremultiplied)
{
    release();
    if (!rhi || !rp) {
        m_lastError = QStringLiteral("PowderSurface::init: null rhi or render-pass descriptor.");
        return false;
    }
    m_rhi = rhi;
    m_rp = rp;
    m_blendPremultiplied = blendPremultiplied;

    const QShader vs = powderVs();
    const QShader fs = powderFs();
    if (!vs.isValid() || !fs.isValid()) {
        m_lastError = QStringLiteral("PowderSurface: powder qsb pack invalid.");
        return false;
    }

    m_ub = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 512);
    if (!m_ub || !m_ub->create()) { m_lastError = QStringLiteral("powder UBO create failed"); return false; }

    m_vb = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadClip));
    if (!m_vb || !m_vb->create()) { m_lastError = QStringLiteral("powder VB create failed"); return false; }

    m_simSampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge);
    m_colSampler = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge);
    if (!m_simSampler->create() || !m_colSampler->create()) { m_lastError = QStringLiteral("powder samplers create failed"); return false; }

    if (m_grid.isValid() && !m_grid.isEmpty())
        ensurePipeline();

    m_lastError.clear();
    return true;
}

/* Build bindings + pipeline once the persistent textures and the RPD exist. */
void PowderSurface::ensurePipeline()
{
    if (!m_rhi || !m_rp || !m_simTex || !m_colTex)
        return;
    if (m_powder && m_bindings)
        return;

    delete m_bindings; m_bindings = nullptr;
    delete m_powder;   m_powder = nullptr;

    m_bindings = m_rhi->newShaderResourceBindings();
    m_bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(2,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_ub),
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, m_simTex, m_simSampler),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_colTex, m_colSampler),
    });
    if (!m_bindings->create()) { m_lastError = QStringLiteral("powder SRB create failed"); return; }

    m_powder = buildPipeline(m_rhi, powderVs(), powderFs(), m_bindings,
                             m_blendPremultiplied, m_rp);
    if (!m_powder) m_lastError = QStringLiteral("powder pipeline create failed");
}

bool PowderSurface::uploadCell(const glitter::GlitterPowder &powder, int idx,
                               quint8 *simOut, quint8 *colOut)
{
    const glitter::PowderCell &c = powder.powderCellAt(idx);
    const quint16 h = floatToHalf(c.height);
    const quint16 s = floatToHalf(c.seed);
    const quint16 b = floatToHalf(c.stuck);
    const quint16 g = floatToHalf(c.glue);
    simOut[0] = quint8(h & 0xFF);  simOut[1] = quint8(h >> 8);
    simOut[2] = quint8(s & 0xFF);  simOut[3] = quint8(s >> 8);
    simOut[4] = quint8(b & 0xFF);  simOut[5] = quint8(b >> 8);
    simOut[6] = quint8(g & 0xFF);  simOut[7] = quint8(g >> 8);
    const int maxSlot = 15;
    colOut[0] = quint8((qBound(0, c.lA, maxSlot) << 4) | (qBound(0, c.lB, maxSlot) & 0x0F));
    colOut[1] = quint8(qBound(0.0f, c.lF, 1.0f) * 255.0f);
    colOut[2] = quint8((qBound(0, c.sA, maxSlot) << 4) | (qBound(0, c.sB, maxSlot) & 0x0F));
    colOut[3] = quint8(qBound(0.0f, c.sF, 1.0f) * 255.0f);
    return true;
}

void PowderSurface::resize(const QSize &grid, float worldCell,
                           const glitter::PowderPalette &pal)
{
    m_grid = grid;
    m_worldCell = worldCell;
    m_holographic = pal.holographic;
    m_paper = pal.paper;
    m_palette = pal.colors;
    m_simCache = QByteArray(grid.width() * grid.height() * 8, '\0');
    m_colCache = QByteArray(grid.width() * grid.height() * 4, '\0');

    if (m_rhi) {
        /* rebuild the persistent textures to the new grid */
        if (m_simTex) { m_simTex->destroy(); delete m_simTex; m_simTex = nullptr; }
        if (m_colTex) { m_colTex->destroy(); delete m_colTex; m_colTex = nullptr; }
        if (grid.isValid() && !grid.isEmpty()) {
            m_simTex = m_rhi->newTexture(QRhiTexture::RGBA16F, grid, 1, {});
            m_colTex = m_rhi->newTexture(QRhiTexture::RGBA8, grid, 1, {});
            if (m_simTex) m_simTex->create();
            if (m_colTex) m_colTex->create();
            ensurePipeline();
        }
    }
}

QRhiResourceUpdateBatch *PowderSurface::uploadSimulation(const glitter::GlitterPowder &powder,
                                                         const QRectF *dirtyWorldRect)
{
    if (!m_rhi || !m_simTex || !m_colTex || m_grid.isEmpty())
        return nullptr;

    const int W = m_grid.width();
    const int H = m_grid.height();
    const int total = W * H;
    if (m_simCache.size() != total * 8)
        m_simCache = QByteArray(total * 8, '\0');
    if (m_colCache.size() != total * 4)
        m_colCache = QByteArray(total * 4, '\0');

    QVector<int> toUpload;
    if (!dirtyWorldRect || !dirtyWorldRect->isValid()) {
        toUpload.reserve(total);
        for (int i = 0; i < total; ++i)
            toUpload.append(i);
    } else {
        const float cell = m_worldCell;
        const int x0 = std::max(0, int(std::floor(dirtyWorldRect->left() / cell)));
        const int y0 = std::max(0, int(std::floor(dirtyWorldRect->top() / cell)));
        const int x1 = std::min(W - 1, int(std::floor(dirtyWorldRect->right() / cell)));
        const int y1 = std::min(H - 1, int(std::floor(dirtyWorldRect->bottom() / cell)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                toUpload.append(y * W + x);
    }
    for (const int i : toUpload) {
        uploadCell(powder, i, reinterpret_cast<quint8 *>(m_simCache.data()) + i * 8,
                             reinterpret_cast<quint8 *>(m_colCache.data()) + i * 4);
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->uploadTexture(m_simTex,
        { QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(m_simCache.constData(),
                                    quint32(m_simCache.size()))) });
    u->uploadTexture(m_colTex,
        { QRhiTextureUploadEntry(0, 0, QRhiTextureSubresourceUploadDescription(m_colCache.constData(),
                                    quint32(m_colCache.size()))) });
    return u;
}

QRhiResourceUpdateBatch *PowderSurface::uploadStaticQuad()
{
    if (!m_rhi || !m_vb)
        return nullptr;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->uploadStaticBuffer(m_vb, kQuadClip);
    return u;
}

QRhiResourceUpdateBatch *PowderSurface::uploadUniforms(float originX, float originY,
                                                       float worldPerPx,
                                                       const QSize &viewDeviceSize,
                                                       qint64 timeMs, int uiDpr)
{
    if (!m_rhi || !m_ub)
        return nullptr;

    const QVector3D L = m_lightDir.normalized();

    QByteArray ub(512, '\0');
    auto wf  = [&ub](int off, float v) { std::memcpy(ub.data() + off, &v, 4); };
    auto wv2 = [&ub](int off, float a, float b) { float t[2] = { a, b }; std::memcpy(ub.data() + off, t, 8); };
    auto wv3 = [&ub](int off, float a, float b, float c) { float t[4] = { a, b, c, 0.0f }; std::memcpy(ub.data() + off, t, 16); };

    QMatrix4x4 ident;
    std::memcpy(ub.data() + 0, ident.constData(), 64);
    wv2(64, float(viewDeviceSize.width()), float(viewDeviceSize.height()));
    wv2(72, originX, originY);
    wf(80, worldPerPx);
    wf(84, m_worldCell);
    wf(88, float(qMax(1, uiDpr)));
    wf(92, float(timeMs) / 1000.0f);
    wv2(96, float(m_grid.width()), float(m_grid.height()));
    wf(104, m_holographic);
    wf(108, m_flakePx);
    wf(112, m_tooth);
    wv3(128, L.x(), L.y(), L.z());
    wv3(144, m_paper.redF(), m_paper.greenF(), m_paper.blueF());
    for (int i = 0; i < qMin(m_palette.size(), 16); ++i)
        wv3(160 + i * 16, m_palette.at(i).redF(), m_palette.at(i).greenF(), m_palette.at(i).blueF());

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_ub, 0, 512, ub.constData());
    return u;
}

void PowderSurface::render(QRhiCommandBuffer *cb, const QRhiRenderTarget *rt)
{
    if (!m_rhi || !m_powder || !cb || !rt)
        return;
    cb->setGraphicsPipeline(m_powder);
    cb->setShaderResources(m_bindings);
    const QSize s = rt->pixelSize();
    cb->setViewport({ 0, 0, float(s.width()), float(s.height()) });
    if (m_vb) {
        QRhiCommandBuffer::VertexInput vi = { m_vb, 0 };
        cb->setVertexInput(0, 1, &vi);
    }
    cb->draw(6);
}

void PowderSurface::release()
{
    delete m_powder;       m_powder = nullptr;
    delete m_bindings;     m_bindings = nullptr;
    delete m_ub;           m_ub = nullptr;
    delete m_vb;           m_vb = nullptr;
    delete m_simSampler;   m_simSampler = nullptr;
    delete m_colSampler;   m_colSampler = nullptr;
    delete m_simTex;       m_simTex = nullptr;
    delete m_colTex;       m_colTex = nullptr;
    m_rhi = nullptr;
    m_rp = nullptr;
}

} // namespace art
} // namespace paintbrush