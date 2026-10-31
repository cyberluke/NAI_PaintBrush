/*****************************************************************************
 * pb_gpu_runtime.cpp : NAI Paintbrush — GPU Art Runtime (QRhi)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_gpu_runtime.hpp"
#include "shaders_qsb.h"

#include "../runtime/art_noise.hpp"

#include <QVector3D>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QWindow>
#include <QImage>
#include <QRgb>
#include <QDebug>
#include <QPainter>
#include <QDateTime>
#include <cstring>
#include <cmath>

#include <rhi/qrhi.h>

namespace paintbrush {
namespace art {

namespace {

/* Qt 6.11: setVertexInput takes a VertexInput array. */
inline void pbSetVertexInput(QRhiCommandBuffer *cb, QRhiBuffer *vb)
{
    QRhiCommandBuffer::VertexInput vi = { vb, 0 };
    cb->setVertexInput(0, 1, &vi);
}

/* The framebuffer holds premultiplied bytes (blend One/OneMinusSrcAlpha);
 * readback labels them ARGB32 (straight), so un-premultiply to match the
 * CPU golden images the harness compares against. */
QImage unPremultiply(const QImage &premul)
{
    if (premul.isNull())
        return {};
    QImage out(premul.size(), QImage::Format_ARGB32);
    for (int y = 0; y < premul.height(); ++y)
    {
        const QRgb *src = reinterpret_cast<const QRgb *>(premul.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < premul.width(); ++x)
        {
            const int a = qAlpha(src[x]);
            if (a == 0)
            {
                dst[x] = 0;
                continue;
            }
            const int r = qBound(0, int(qRed(src[x]) * 255.0 / a + 0.5), 255);
            const int g = qBound(0, int(qGreen(src[x]) * 255.0 / a + 0.5), 255);
            const int b = qBound(0, int(qBlue(src[x]) * 255.0 / a + 0.5), 255);
            dst[x] = qRgba(r, g, b, a);
        }
    }
    return out;
}

} // namespace

/* -------------------------------------------------------------- pipelines */

struct GpuRuntime::PipelineSet
{
    QRhiGraphicsPipeline *paper = nullptr;
    QRhiShaderResourceBindings *paperBindings = nullptr;
    QRhiBuffer *paperUb = nullptr;

    QRhiGraphicsPipeline *glitter = nullptr;
    QRhiShaderResourceBindings *glitterBindings = nullptr;
    QRhiBuffer *glitterUb = nullptr;
    QRhiBuffer *glitterVb = nullptr;

    QRhiGraphicsPipeline *powder = nullptr;
    QRhiShaderResourceBindings *powderBindings = nullptr;
    QRhiBuffer *powderUb = nullptr;
    QRhiSampler *powderSim = nullptr;   /* bilinear for smooth height seams */
    QRhiSampler *powderCol = nullptr;   /* nearest for per-cell palette reads */

    QRhiGraphicsPipeline *finish = nullptr;
    QRhiShaderResourceBindings *finishBindings = nullptr;
    QRhiBuffer *finishUb = nullptr;
    QRhiSampler *finishSampler = nullptr;
};

namespace {

QShader loadQsb(const unsigned char *data, size_t size)
{
    return QShader::fromSerialized(QByteArray(reinterpret_cast<const char *>(data), int(size)));
}

/* one qsb pack per stage (the bake script emits _VERT/_FRAG arrays) */
QShader vertexFor(const char *name)
{
    if (qstrcmp(name, "paper") == 0)
        return loadQsb(kQSB_PAPER_VERT, sizeof(kQSB_PAPER_VERT));
    if (qstrcmp(name, "glitter") == 0)
        return loadQsb(kQSB_GLITTER_VERT, sizeof(kQSB_GLITTER_VERT));
    if (qstrcmp(name, "powder") == 0)
        return loadQsb(kQSB_POWDER_VERT, sizeof(kQSB_POWDER_VERT));
    return loadQsb(kQSB_FINISH_VERT, sizeof(kQSB_FINISH_VERT));
}

QShader fragmentFor(const char *name)
{
    if (qstrcmp(name, "paper") == 0)
        return loadQsb(kQSB_PAPER_FRAG, sizeof(kQSB_PAPER_FRAG));
    if (qstrcmp(name, "glitter") == 0)
        return loadQsb(kQSB_GLITTER_FRAG, sizeof(kQSB_GLITTER_FRAG));
    if (qstrcmp(name, "powder") == 0)
        return loadQsb(kQSB_POWDER_FRAG, sizeof(kQSB_POWDER_FRAG));
    return loadQsb(kQSB_FINISH_FRAG, sizeof(kQSB_FINISH_FRAG));
}

/* IEEE-754 binary16 encoding for the RGBA16F simulation texture upload.
 * Keeps the physics' real cell-unit values end-to-end (no 8-bit quantization). */
quint16 floatToHalf(float f)
{
    quint32 x = 0;
    std::memcpy(&x, &f, 4);                                /* float bits via memcpy (C++17-safe) */
    const quint32 sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xFFu) - 127 + 15;
    quint32 mant = x & 0x7FFFFFu;
    if (exp >= 0x1F)
        return quint16(sign | 0x7C00u);                     /* inf / NaN */
    if (exp <= 0)
    {
        if (exp < -10)
            return quint16(sign);                           /* underflow to 0 */
        mant = (mant | 0x800000u) >> (1 - exp);             /* subnormal */
        return quint16(sign | (mant + 0x1000u) >> 13);
    }
    quint32 m = mant >> 13;
    const quint32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u)))
        ++m;                                                /* round-to-nearest-even */
    if ((m & 0x7C00u) == 0x7C00u)
        return quint16(sign | 0x7C00u);                     /* rounded up to inf */
    return quint16(sign | (quint32(exp) << 10) | m);
}

/* The full-screen quad as a 6-vertex TRIANGLE LIST (two CCW triangles) —
 * triangle strips are avoided so winding/culling behaves identically on
 * every backend (D3D11 y-down vs GL y-up). */
const float kQuadClip[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
};

QRhiGraphicsPipeline *buildPipeline(QRhi *rhi, const QShader &vs, const QShader &fs,
                                    const QList<QRhiVertexInputAttribute> &attrs,
                                    int stride, QRhiShaderResourceBindings *srb,
                                    bool blendPremultiplied,
                                    QRhiRenderPassDescriptor *rp)
{
    QRhiGraphicsPipeline *ps = rhi->newGraphicsPipeline();
    ps->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs },
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ QRhiVertexInputBinding(stride) });
    inputLayout.setAttributes(attrs.begin(), attrs.end());
    ps->setVertexInputLayout(inputLayout);
    ps->setShaderResourceBindings(srb);
    ps->setRenderPassDescriptor(rp);
    ps->setDepthTest(false);
    ps->setDepthWrite(false);
    ps->setCullMode(QRhiGraphicsPipeline::None);
    if (blendPremultiplied)
        ps->setTargetBlends({ QRhiGraphicsPipeline::TargetBlend{ QRhiGraphicsPipeline::ColorMask(0xF), true, QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha, QRhiGraphicsPipeline::Add, QRhiGraphicsPipeline::One, QRhiGraphicsPipeline::OneMinusSrcAlpha, QRhiGraphicsPipeline::Add } });
    if (!ps->create())
        return nullptr;
    return ps;
}

} // namespace

/* ---------------------------------------------------------------- runtime */

GpuRuntime::GpuRuntime() = default;

GpuRuntime::~GpuRuntime()
{
    shutdown();
}

bool GpuRuntime::initialize(bool preferD3D11)
{
    if (m_rhi)
        return true;

#ifdef Q_OS_WIN
    if (preferD3D11)
    {
        QRhiD3D11InitParams params;
        params.enableDebugLayer = qEnvironmentVariableIsSet("PB_GPU_DEBUG");
        m_rhi = QRhi::create(QRhi::D3D11, &params);
        if (m_rhi)
            m_backendName = QStringLiteral("d3d11");
    }
#endif
    if (!m_rhi)
    {
        QRhiGles2InitParams params;
        params.fallbackSurface = nullptr;
        m_rhi = QRhi::create(QRhi::OpenGLES2, &params);
        if (m_rhi)
            m_backendName = QStringLiteral("opengles");
    }
    if (!m_rhi)
    {
        m_lastError = QStringLiteral("No QRhi backend could be created (D3D11/GLES2).");
        return false;
    }

    m_pipelines = new PipelineSet();

    /* ---- load shader packs + create buffers (pipelines need the render
     * pass descriptor, which exists only after the first target is
     * created — see ensurePipelines) ------------------------------------ */
    m_paperVs = vertexFor("paper");
    m_paperFs = fragmentFor("paper");
    m_glitterVs = vertexFor("glitter");
    m_glitterFs = fragmentFor("glitter");
    m_powderVs = vertexFor("powder");
    m_powderFs = fragmentFor("powder");
    m_finishVs = vertexFor("finish");
    m_finishFs = fragmentFor("finish");
    if (!m_paperVs.isValid() || !m_paperFs.isValid() ||
        !m_glitterVs.isValid() || !m_glitterFs.isValid() ||
        !m_powderVs.isValid() || !m_powderFs.isValid() ||
        !m_finishVs.isValid() || !m_finishFs.isValid())
    {
        m_lastError = QStringLiteral("Shader pack invalid (qsb).");
        return false;
    }

    m_pipelines->paperUb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    if (!m_pipelines->paperUb->create())
        return false;
    m_pipelines->paperBindings = m_rhi->newShaderResourceBindings();
    m_pipelines->paperBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->paperUb),
    });
    if (!m_pipelines->paperBindings->create())
        return false;

    m_pipelines->glitterUb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    if (!m_pipelines->glitterUb->create())
        return false;
    m_pipelines->glitterBindings = m_rhi->newShaderResourceBindings();
    m_pipelines->glitterBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->glitterUb),
    });
    if (!m_pipelines->glitterBindings->create())
        return false;
    m_pipelines->glitterVb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 21);
    if (!m_pipelines->glitterVb->create())
        return false;

    /* powder: fullscreen heightfield pass — ubuf (binding 2) + two samplers
     * (sim bind 0, col bind 1). Textures are uploaded per renderPowder call;
     * the SRB keeps a persistent 1x1 dummy so it can be created here once. */
    m_pipelines->powderUb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 512);
    if (!m_pipelines->powderUb->create())
        return false;
    m_pipelines->powderSim = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge);
    m_pipelines->powderCol = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                               QRhiSampler::None, QRhiSampler::ClampToEdge,
                                               QRhiSampler::ClampToEdge);
    if (!m_pipelines->powderSim->create() || !m_pipelines->powderCol->create())
        return false;
    m_pipelines->powderBindings = m_rhi->newShaderResourceBindings();
    {
        /* dummies keep the SRB valid at create; the sim sampler is a float
         * filter, so its dummy is RGBA16F (format-compatible) */
        m_powderSimDummy = m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1, {});
        m_powderSimDummy->create();
        m_powderColDummy = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1, {});
        m_powderColDummy->create();
    }
    m_pipelines->powderBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->powderUb),
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, m_powderSimDummy, m_pipelines->powderSim),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_powderColDummy, m_pipelines->powderCol),
    });
    if (!m_pipelines->powderBindings->create())
        return false;

    m_pipelines->finishUb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 256);
    if (!m_pipelines->finishUb->create())
        return false;
    m_pipelines->finishSampler = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                                   QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                   QRhiSampler::ClampToEdge);
    if (!m_pipelines->finishSampler->create())
        return false;
    m_pipelines->finishBindings = m_rhi->newShaderResourceBindings();
    m_pipelines->finishBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->finishUb),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, nullptr, m_pipelines->finishSampler),
    });
    /* QRhi requires a valid texture at srb create: a created (empty) 1x1 dummy
     * that stays alive for the runtime's lifetime. */
    {
        m_finishDummy = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1, {});
        m_finishDummy->create();
        m_pipelines->finishBindings->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->finishUb),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_finishDummy, m_pipelines->finishSampler),
        });
        if (!m_pipelines->finishBindings->create())
            return false;
    }

    return true;
}

bool GpuRuntime::ensurePipelines()
{
    if (!m_pipelines || !m_rtRp)
        return false;
    /* the pipelines embed the render pass descriptor they were created with;
     * when the target grows, m_rtRp is a NEW object and the old pipelines
     * reference a DELETED descriptor — rebuild them against the new one */
    if (m_pipelines->paper && m_pipelines->glitter && m_pipelines->powder &&
        m_pipelines->finish && m_pipelinesRp == m_rtRp)
        return true;

    /* drop pipelines bound to a stale render pass descriptor */
    if (m_pipelines->paper || m_pipelines->glitter || m_pipelines->powder || m_pipelines->finish)
    {
        delete m_pipelines->paper;
        delete m_pipelines->glitter;
        delete m_pipelines->powder;
        delete m_pipelines->finish;
        m_pipelines->paper = nullptr;
        m_pipelines->glitter = nullptr;
        m_pipelines->powder = nullptr;
        m_pipelines->finish = nullptr;
    }

    /* paper: kQuadClip is 4 floats/vertex (pos + texcoord); only position is
     * consumed but the stride must match the buffer layout */
    m_pipelines->paper = buildPipeline(m_rhi, m_paperVs, m_paperFs,
                                       { QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0) },
                                       4 * sizeof(float), m_pipelines->paperBindings, false, m_rtRp);
    if (!m_pipelines->paper)
    {
        m_lastError = QStringLiteral("Paper pipeline creation failed.");
        return false;
    }

    /* glitter: 10 attributes, stride 64B (triangle LIST: 6 verts per quad —
     * the default Topology::Triangles, unlike the strip used for the
     * full-screen paper/finish quads) */
    m_pipelines->glitter = buildPipeline(m_rhi, m_glitterVs, m_glitterFs,
                                         {
                                             QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                                             QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 8),
                                             QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float, 16),
                                             QRhiVertexInputAttribute(0, 3, QRhiVertexInputAttribute::Float, 20),
                                             QRhiVertexInputAttribute(0, 4, QRhiVertexInputAttribute::Float4, 24),
                                             QRhiVertexInputAttribute(0, 5, QRhiVertexInputAttribute::Float, 40),
                                             QRhiVertexInputAttribute(0, 6, QRhiVertexInputAttribute::Float, 44),
                                             QRhiVertexInputAttribute(0, 7, QRhiVertexInputAttribute::Float, 48),
                                             QRhiVertexInputAttribute(0, 8, QRhiVertexInputAttribute::Float2, 52),
                                             QRhiVertexInputAttribute(0, 9, QRhiVertexInputAttribute::Float, 60),
                                         },
                                         16 * sizeof(float), m_pipelines->glitterBindings, true, m_rtRp);
    if (!m_pipelines->glitter)
    {
        m_lastError = QStringLiteral("Glitter pipeline creation failed.");
        return false;
    }

    /* powder: fullscreen heightfield pass (6-triangle-list quad, blend off —
     * the shader outputs an opaque RGBA which is composited over the paper) */
    m_pipelines->powder = buildPipeline(m_rhi, m_powderVs, m_powderFs,
                                        { QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0) },
                                        4 * sizeof(float), m_pipelines->powderBindings, false, m_rtRp);
    if (!m_pipelines->powder)
    {
        m_lastError = QStringLiteral("Powder pipeline creation failed.");
        return false;
    }

    /* finish */
    m_pipelines->finish = buildPipeline(m_rhi, m_finishVs, m_finishFs,
                                        { QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
                                          QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 8) },
                                        4 * sizeof(float), m_pipelines->finishBindings, true, m_rtRp);
    if (!m_pipelines->finish)
    {
        m_lastError = QStringLiteral("Finish pipeline creation failed.");
        return false;
    }
    m_pipelinesRp = m_rtRp;
    return true;
}

void GpuRuntime::shutdown()
{
    releaseTarget();
    delete m_finishDummy;
    m_finishDummy = nullptr;
    delete m_powderSimDummy;
    m_powderSimDummy = nullptr;
    delete m_powderColDummy;
    m_powderColDummy = nullptr;
    delete m_pipelines;
    m_pipelines = nullptr;
    delete m_rhi;
    m_rhi = nullptr;
}

bool GpuRuntime::ensureTarget(const QSize &size)
{
    if (!m_rhi || size.isEmpty() || size.width() > 8192 || size.height() > 8192)
        return false;
    /* Grow-only target: the render pass descriptor (and the pipelines built
     * against it) must stay alive across renders of different sizes, so the
     * target only ever grows. Each render uses a viewport over the region
     * and crops the readback to the requested size. */
    if (m_target && m_target->pixelSize().width() >= size.width() &&
        m_target->pixelSize().height() >= size.height())
        return true;
    releaseTarget();
    m_target = m_rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                 QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!m_target->create())
    {
        std::fprintf(stderr, "pb_gpu: target texture create failed\n");
        return false;
    }
    m_rt = m_rhi->newTextureRenderTarget({ m_target });
    m_rtRp = m_rt->newCompatibleRenderPassDescriptor();
    m_rt->setRenderPassDescriptor(m_rtRp);
    if (!m_rt->create())
    {
        std::fprintf(stderr, "pb_gpu: render target create failed\n");
        return false;
    }
    /* the render pass descriptor changed with the new target: the pipelines
     * reference it, so they must be rebuilt against the new descriptor */
    if (!ensurePipelines())
    {
        std::fprintf(stderr, "pb_gpu: ensurePipelines failed: %s\n", qPrintable(m_lastError));
        return false;
    }
    return true;
}

void GpuRuntime::releaseTarget()
{
    delete m_rtRp;
    m_rtRp = nullptr;
    delete m_rt;
    m_rt = nullptr;
    delete m_target;
    m_target = nullptr;
}

QImage GpuRuntime::readback(QRhiTexture *tex, const QSize &size)
{
    if (!m_rhi || !tex)
        return {};
    QRhiReadbackResult result;
    QImage image;
    /* readBackTexture reads the ENTIRE texture; the target is grow-only so
     * the returned buffer can be larger than the requested region. Build the
     * image from the ACTUAL buffer size (result.pixelSize), then crop the
     * requested size from the top-left (each render draws at viewport origin). */
    result.completed = [&image, &result]() {
        if (result.data.isEmpty() || result.pixelSize.isEmpty())
            return;
        QImage img(reinterpret_cast<const uchar *>(result.data.constData()),
                   result.pixelSize.width(), result.pixelSize.height(),
                   result.pixelSize.width() * 4, QImage::Format_RGBA8888);
        image = img.copy();
    };
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->readBackTexture(tex, &result);
    QRhiCommandBuffer *cb = nullptr;
    const QRhi::FrameOpResult fr = m_rhi->beginOffscreenFrame(&cb);
    if (fr != QRhi::FrameOpSuccess)
    {
        std::fprintf(stderr, "pb_gpu: readback frame failed %d\n", int(fr));
        return {};
    }
    cb->beginPass(m_rt, Qt::transparent, { 1.0f, 0 }, u);
    cb->endPass();
    m_rhi->endOffscreenFrame();
    m_rhi->finish(); /* synchronous readback */
    QImage out = image.convertToFormat(QImage::Format_ARGB32);
    /* crop to the requested size (the target is grow-only). The backend with
     * a Y-down framebuffer (D3D11/Metal) flips the viewport Y for offscreen
     * renders: the drawn content sits at the BOTTOM of the target when the
     * target is taller than the viewport, so the crop is bottom-anchored
     * vertically there; Y-up backends (GL) keep it top-anchored. X stays
     * top-left-anchored on every backend (no horizontal flip). */
    if (!out.isNull() && out.size() != size)
    {
        const int y = m_rhi->isYUpInFramebuffer() ? 0 : out.height() - size.height();
        out = out.copy(0, y, size.width(), size.height());
    }
    return out;
}

/* ----------------------------------------------------------------- paper */

QImage GpuRuntime::renderPaper(const QSize &outSize, const PaperStock &stock,
                               const ArtLightRig &rig, const ArtScale &scale,
                               float worldViewX, float worldViewY,
                               float worldPerPx, const QRect &region)
{
    if (!m_rhi || !m_pipelines || !ensureTarget(outSize))
        return {};
    Q_UNUSED(region);

    const QColor base = stock.surfaceColor(rig);

    /* ubuf layout (std140): mat4(64) vec2(8) vec2(8) float(4) float(4)
     * pad(8) vec4(16) vec4(16) vec4(16) uint+pad(16) = 160B */
    struct PaperUb
    {
        float matrix[16];                          /* 0 */
        float worldOriginX, worldOriginY;          /* 64 */
        float viewW, viewH;                        /* 72 */
        float worldPerPx;                          /* 80 */
        float opaque;                              /* 84 */
        float pad0[2];                             /* 88 */
        float baseR, baseG, baseB, pad1;           /* 96 vec4 */
        float microHeight, tooth, fiberStrength, fiberScale; /* 112 */
        float specular, roughness, ambient, pad2;  /* 128 */
        quint32 seed;                              /* 144 */
        float pad3[3];                             /* 148 */
    };                                             /* 160 */
    PaperUb ub;
    memset(&ub, 0, sizeof(ub));
    QMatrix4x4 ident;
    memcpy(ub.matrix, ident.constData(), 64);
    ub.worldOriginX = worldViewX;
    ub.worldOriginY = worldViewY;
    ub.viewW = float(outSize.width());
    ub.viewH = float(outSize.height());
    ub.worldPerPx = worldPerPx;
    ub.opaque = 1.0f;
    ub.baseR = base.redF();
    ub.baseG = base.greenF();
    ub.baseB = base.blueF();
    ub.microHeight = stock.microHeight;
    ub.tooth = stock.tooth;
    ub.fiberStrength = stock.fiberStrength;
    ub.fiberScale = stock.fiberScale;
    ub.specular = stock.specular;
    ub.roughness = stock.roughness;
    ub.ambient = rig.ambient;
    ub.seed = stock.seed;

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_pipelines->paperUb, 0, sizeof(ub), &ub);
    QRhiBuffer *vb = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadClip));
    vb->create();
    u->uploadStaticBuffer(vb, kQuadClip);

    QRhiCommandBuffer *cb = nullptr;
    const QRhi::FrameOpResult fr = m_rhi->beginOffscreenFrame(&cb);
    if (fr != QRhi::FrameOpSuccess)
    {
        std::fprintf(stderr, "pb_gpu: beginOffscreenFrame failed %d\n", int(fr));
        delete vb;
        return {};
    }
    cb->beginPass(m_rt, Qt::transparent, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipelines->paper);
    cb->setShaderResources(m_pipelines->paperBindings);
    cb->setViewport({ 0, 0, float(outSize.width()), float(outSize.height()) });
    pbSetVertexInput(cb, vb);
    cb->draw(6);
    cb->endPass();
    m_rhi->endOffscreenFrame();
    delete vb;

    return readback(m_target, outSize);
}

/* -------------------------------------------------------------- glitter */

QImage GpuRuntime::renderGlitter(const QSize &outSize,
                                 const glitter::GlitterState &state,
                                 float outOriginWorldX, float outOriginWorldY,
                                 float worldPerPx, const ArtLightRig &rig,
                                 const PaperStock *paper)
{
    if (!m_rhi || !m_pipelines || !ensureTarget(outSize))
        return {};
    if (state.isEmpty() || outSize.isEmpty() || worldPerPx <= 0.0f)
        return QImage(outSize, QImage::Format_ARGB32);

    /* grain quad vertex: pos(2) center(2) radius rot color(4) height bonded
     * shadowLen shadowDir(2) seed — 16 floats */
    struct GrainVert
    {
        float pos[2];
        float center[2];
        float radius;
        float rot;
        float color[4];
        float height;
        float bonded;
        float shadowLen;
        float shadowDir[2];
        float seed;
    };
    QVector<GrainVert> verts;
    verts.reserve(8192);

    /* light context (CPU makeLight) */
    const QVector3D d = rig.unitDirection(); /* points down onto the paper */
    QVector3D L(-d.x(), -d.y(), -d.z());
    if (L.z() < 0.05f)
        L.setZ(0.05f);
    L.normalize();
    const QVector3D V(0.0f, 0.0f, 1.0f);
    const QVector3D H = (L + V).normalized();

    /* shadow direction: away from the light (CPU) */
    QPointF shadowDir(-d.x(), -d.y());
    const double slen = std::sqrt(shadowDir.x() * shadowDir.x() + shadowDir.y() * shadowDir.y());
    if (slen < 1e-6)
        shadowDir = QPointF(0.0, 1.0);
    else
        shadowDir /= slen;
    const float shadowDirX = float(shadowDir.x());
    const float shadowDirY = float(shadowDir.y());
    const float toothShade = paper ? paper->tooth : 0.4f;
    const float absorption = paper ? paper->absorption : 0.4f;
    const float cs = state.worldCellSize();
    const float pxPerWorld = 1.0f / worldPerPx;
    const float flakeRWorld = cs / 2.2f * 0.5f;

    const QVector<glitter::PaletteSlot> palette = state.paletteForRender();
    const int paletteSize = palette.isEmpty() ? 3 : palette.size();

    const float visW = outSize.width() * worldPerPx;
    const float visH = outSize.height() * worldPerPx;
    const int cx0 = std::max(0, int(std::floor(outOriginWorldX / cs)) - 1);
    const int cy0 = std::max(0, int(std::floor(outOriginWorldY / cs)) - 1);
    const int cx1 = std::min(state.cellW() - 1,
                             int(std::floor((outOriginWorldX + visW) / cs)) + 1);
    const int cy1 = std::min(state.cellH() - 1,
                             int(std::floor((outOriginWorldY + visH) / cs)) + 1);

    struct GrainData
    {
        float wx, wy, rWorld, rot, height;
        float color[4];
        float bonded;
        quint32 gi;
    };
    QVector<GrainData> grains;
    QVector<GrainData> shadows;
    grains.reserve(8192);
    shadows.reserve(8192);

    for (int cy = cy0; cy <= cy1; ++cy)
    {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const glitter::GlitterState::CellView c = state.cellView(cx, cy);
            for (int i = 0; i < c.count; ++i)
            {
                const glitter::Grain &g = c.grains[i];
                const quint32 gi = artHash2(
                    quint32(cx * 92821 + cy), quint32(i) ^ 0x9E37u);
                GrainData gd;
                gd.wx = (cx + g.x) * cs;
                gd.wy = (cy + g.y) * cs;
                gd.rWorld = flakeRWorld * g.size;
                gd.rot = g.rot;
                gd.height = g.height / 1023.0f;
                gd.bonded = g.bonded ? 1.0f : 0.0f;
                gd.gi = gi;
                const glitter::PaletteSlot &slot =
                    palette.at(std::min<int>(g.slot, paletteSize - 1));
                gd.color[0] = slot.color.redF();
                gd.color[1] = slot.color.greenF();
                gd.color[2] = slot.color.blueF();
                gd.color[3] = g.bonded ? 1.0f : 0.95f;
                grains.append(gd);

                /* contact shadow (pass 0): offset away from light, tighter */
                GrainData sh = gd;
                sh.rWorld = gd.rWorld * 0.9f;
                shadows.append(sh);
            }
        }
    }

    const auto emitQuad = [&](const GrainData &g, float shadowLenWorld) {
        const float rPx = std::max(0.6f, g.rWorld * pxPerWorld);
        const float cxPx = (g.wx - outOriginWorldX) * pxPerWorld;
        const float cyPx = (g.wy - outOriginWorldY) * pxPerWorld;
        const float sx = shadowLenWorld * pxPerWorld;
        const float ox = shadowDirX * sx;
        const float oy = shadowDirY * sx;
        /* two triangles (ccw): (bl, br, tl) (br, tr, tl) */
        const float corners[6][2] = {
            { -1, -1 }, { 1, -1 }, { -1, 1 },
            { 1, -1 }, { 1, 1 }, { -1, 1 },
        };
        for (const auto &corner : corners)
        {
            GrainVert v;
            v.pos[0] = corner[0];
            v.pos[1] = corner[1];
            v.center[0] = cxPx + ox;
            v.center[1] = cyPx + oy;
            v.radius = rPx;
            v.rot = g.rot;
            v.color[0] = g.color[0];
            v.color[1] = g.color[1];
            v.color[2] = g.color[2];
            v.color[3] = g.color[3];
            v.height = g.height;
            v.bonded = g.bonded;
            v.shadowLen = shadowLenWorld > 0.0f ? sx : 0.0f; /* px offset, 0 = grain */
            v.shadowDir[0] = shadowDirX;
            v.shadowDir[1] = shadowDirY;
            v.seed = float(g.gi);
            verts.append(v);
        }
    };

    /* CPU pass order: all shadows first, then bonded, then loose */
    for (const GrainData &sh : shadows)
        emitQuad(sh, sh.rWorld * (0.25f + 0.5f * sh.height));
    for (const GrainData &g : grains)
        emitQuad(g, 0.0f);

    if (verts.isEmpty())
        return QImage(outSize, QImage::Format_ARGB32);

    const int bytes = verts.size() * int(sizeof(GrainVert));
    if (bytes > m_pipelines->glitterVb->size())
    {
        delete m_pipelines->glitterVb;
        m_pipelines->glitterVb = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, bytes);
        m_pipelines->glitterVb->create();
    }
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_pipelines->glitterVb, 0, quint32(bytes), verts.constData());

    struct GlitterUb
    {
        float matrix[16];                          /* 0 */
        float lightL[3]; float pad0;               /* 64 vec4 */
        float lightH[3]; float pad1;               /* 80 vec4 */
        float specExp, keyIntensity, ambient, softness; /* 96 */
        float keyColor[4];                         /* 112 */
        float toothShade, absorption;              /* 128 */
        float viewW, viewH;                        /* 136 */
        float pad2[2];                             /* 144 */
    };                                             /* 160 */
    GlitterUb ub;
    memset(&ub, 0, sizeof(ub));
    QMatrix4x4 ident;
    memcpy(ub.matrix, ident.constData(), 64);
    ub.lightL[0] = L.x(); ub.lightL[1] = L.y(); ub.lightL[2] = L.z();
    ub.lightH[0] = H.x(); ub.lightH[1] = H.y(); ub.lightH[2] = H.z();
    ub.specExp = 60.0f - 40.0f * qBound(0.0f, rig.softness, 1.0f);
    ub.keyIntensity = rig.intensity;
    ub.ambient = rig.ambient;
    ub.softness = rig.softness;
    const QColor key = rig.keyColor();
    ub.keyColor[0] = key.redF();
    ub.keyColor[1] = key.greenF();
    ub.keyColor[2] = key.blueF();
    ub.keyColor[3] = 1.0f;
    ub.toothShade = toothShade;
    ub.absorption = absorption;
    ub.viewW = float(outSize.width());
    ub.viewH = float(outSize.height());
    u->updateDynamicBuffer(m_pipelines->glitterUb, 0, sizeof(ub), &ub);

    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return {};
    cb->beginPass(m_rt, Qt::transparent, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipelines->glitter);
    cb->setShaderResources(m_pipelines->glitterBindings);
    cb->setViewport({ 0, 0, float(outSize.width()), float(outSize.height()) });
    pbSetVertexInput(cb, m_pipelines->glitterVb);
    cb->draw(verts.size());
    cb->endPass();
    m_rhi->endOffscreenFrame();

    return unPremultiply(readback(m_target, outSize));
}

/* --------------------------------------------------------------- powder */

QImage GpuRuntime::renderPowder(const QSize &outSize,
                                const glitter::GlitterPowder &powder,
                                const glitter::PowderPalette &pal,
                                float outOriginWorldX, float outOriginWorldY,
                                float worldPerPx, const ArtLightRig &rig,
                                const PaperStock *paper, qint64 timeMs,
                                int uiDpr)
{
    if (!m_rhi || !m_pipelines || !ensureTarget(outSize))
        return {};
    if (powder.cellW() <= 0 || powder.cellH() <= 0 || outSize.isEmpty())
        return QImage(outSize, QImage::Format_ARGB32);
    if (!ensurePipelines() || !m_pipelines->powder)
        return {};

    const int W = powder.cellW();
    const int H = powder.cellH();
    const int dpr = qMax(1, uiDpr);
    const int palN = qMax(1, pal.colors.size());

    /* --- FLOAT simulation texture (RGBA16F) ----------------------------
     * Preserves the REAL cell-unit heights/stuck/glue as the physics has them
     * (no 0..15 -> 0..1 quantization), so the shader's reference thresholds
     * see the actual values. r=height g=seed b=stuck a=glue */
    QByteArray simHalf(W * H * 4 * 2, '\0');     /* 4 channels x float16 */
    quint8 *sim8 = reinterpret_cast<quint8 *>(simHalf.data());
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const glitter::PowderCell &c = powder.powderCellAt(y * W + x);
            const quint16 vh = floatToHalf(c.height);
            const quint16 vs = floatToHalf(c.seed);
            const quint16 vb = floatToHalf(c.stuck);
            const quint16 va = floatToHalf(c.glue);
            quint8 *p = sim8 + size_t(y * W + x) * 8;
            p[0] = quint8(vh & 0xFF);   p[1] = quint8(vh >> 8);
            p[2] = quint8(vs & 0xFF);   p[3] = quint8(vs >> 8);
            p[4] = quint8(vb & 0xFF);   p[5] = quint8(vb >> 8);
            p[6] = quint8(va & 0xFF);   p[7] = quint8(va >> 8);
        }
    }

    /* --- packed colour texture (RGBA8), reference uCol layout ----------
     *  R = loose  A<<4 | B   (two palette slots, four bits each)
     *  G = loose  B share    0..255
     *  B = bonded A<<4 | B
     *  A = bonded B share    0..255
     * The shader decodes with floor(texelFetch(uCol).* 255 + 0.5) so the
     * palette indices survive (no 0..15 -> 0..1 sampler normalization loss). */
    QByteArray colBuf(W * H * 4, '\0');
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const glitter::PowderCell &c = powder.powderCellAt(y * W + x);
            const size_t o = size_t(y * W + x) * 4;
            const int maxSlot = qMin(palN - 1, 15);
            const int loA = qBound(0, c.lA, maxSlot);
            const int loB = qBound(0, c.lB, maxSlot);
            const int bdA = qBound(0, c.sA, maxSlot);
            const int bdB = qBound(0, c.sB, maxSlot);
            colBuf[o + 0] = quint8((loA << 4) | (loB & 0x0F));
            colBuf[o + 1] = quint8(qBound(0.0f, c.lF, 1.0f) * 255.0f);
            colBuf[o + 2] = quint8((bdA << 4) | (bdB & 0x0F));
            colBuf[o + 3] = quint8(qBound(0.0f, c.sF, 1.0f) * 255.0f);
        }
    }

    QRhiTexture *simTex = m_rhi->newTexture(QRhiTexture::RGBA16F, QSize(W, H), 1, {});
    QRhiTexture *colTex = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(W, H), 1, {});
    if (!simTex->create() || !colTex->create())
        return {};
    /* a float sampler needs a linear float filter on most backends */
    QRhiSampler *simSampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                                QRhiSampler::None, QRhiSampler::ClampToEdge,
                                                QRhiSampler::ClampToEdge);

    /* light context from the shared ArtLightRig (same derivation as CPU) */
    const QVector3D d = rig.unitDirection();       /* points down onto paper */
    QVector3D L(-d.x(), -d.y(), -d.z());
    if (L.z() < 0.05f)
        L.setZ(0.05f);
    L.normalize();

    /* --- ubuf (std140), see powder.frag --------------------------------- */
    static const int kUbSize = 512;
    QByteArray ub(kUbSize, 0);
    auto wf = [&ub](int off, float v) { std::memcpy(ub.data() + off, &v, 4); };
    auto wv2 = [&ub](int off, float a, float b) { float t[2] = { a, b }; std::memcpy(ub.data() + off, t, 8); };
    auto wv3 = [&ub](int off, float a, float b, float c) { float t[4] = { a, b, c, 0.0f }; std::memcpy(ub.data() + off, t, 16); };

    QMatrix4x4 ident;
    std::memcpy(ub.data() + 0, ident.constData(), 64);
    wv2(64, float(outSize.width()), float(outSize.height()));          /* uViewSize */
    wv2(72, outOriginWorldX, outOriginWorldY);                          /* uWorldOrigin */
    wf(80, worldPerPx);                                                 /* uWorldPerPx */
    wf(84, powder.worldCellSize());                                     /* uWorldCell */
    wf(88, float(dpr));                                                 /* uDpr */
    wf(92, float(timeMs) / 1000.0f);                                    /* uTime */
    wv2(96, float(W), float(H));                                        /* uGrid */
    wf(104, pal.holographic);                                           /* uHolo */
    wf(108, float(dpr * 5));                                            /* uFlakePx */
    wf(112, paper ? paper->tooth : 0.4f);                               /* uTooth */
    wv3(128, L.x(), L.y(), L.z());                                      /* uLightDir */
    wv3(144, pal.paper.redF(), pal.paper.greenF(), pal.paper.blueF());  /* uPaper */
    for (int i = 0; i < qMin(palN, 16); ++i)                            /* uPal[16] */
    {
        const QColor &c = pal.colors.at(i);
        wv3(160 + i * 16, c.redF(), c.greenF(), c.blueF());
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    /* raw float16 upload for the sim texture (tightly packed 4xH=RGBA) */
    u->uploadTexture(simTex, { QRhiTextureUploadEntry(0, 0,
                            QRhiTextureSubresourceUploadDescription(simHalf.constData(),
                                                                    quint32(simHalf.size()))) });
    /* packed colour texture as an owned QImage (removes ARGB swap) */
    QImage colI((const uchar *)colBuf.constData(), W, H, W * 4, QImage::Format_RGBA8888);
    u->uploadTexture(colTex, colI.copy());
    u->updateDynamicBuffer(m_pipelines->powderUb, 0, kUbSize, ub.constData());

    /* per-call SRB (layout must match the powder pipeline's) */
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(2, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->powderUb),
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, simTex, simSampler),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, colTex, m_pipelines->powderCol),
    });
    if (!srb->create())
        return {};

    QRhiBuffer *vb = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadClip));
    vb->create();
    u->uploadStaticBuffer(vb, kQuadClip);

    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return {};
    cb->beginPass(m_rt, Qt::transparent, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipelines->powder);
    cb->setShaderResources(srb);
    cb->setViewport({ 0, 0, float(outSize.width()), float(outSize.height()) });
    pbSetVertexInput(cb, vb);
    cb->draw(6);
    cb->endPass();
    m_rhi->endOffscreenFrame();

    delete vb;
    delete srb;
    delete simSampler;
    QImage result = readback(m_target, outSize);
    delete simTex;
    delete colTex;
    return unPremultiply(result);
}

/* ---------------------------------------------------------------- finish */

QImage GpuRuntime::renderFinish(const QImage &source, int mode,
                                const EffectNode &node, const ArtLightRig &rig,
                                const ArtScale &scale)
{
    if (!m_rhi || !m_pipelines || source.isNull() || mode < 0 || mode > 2)
        return {};

    /* same padding as the CPU golden path (FinishRuntime::paddingFor) */
    const int pad = FinishRuntime::paddingFor({ node }, scale);
    const QSize outSize(source.width() + 2 * pad, source.height() + 2 * pad);
    if (!ensureTarget(outSize))
    {
        std::fprintf(stderr, "pb_gpu: finish ensureTarget failed\n");
        return {};
    }

    /* upload the PADDED canvas as the texture: the CPU blurs the padded image
     * (subject at uPad, transparent padding), so taps inside the padding are
     * transparent — exactly mirrored by sampling the padded texture */
    QImage padded(outSize, QImage::Format_ARGB32_Premultiplied);
    padded.fill(Qt::transparent);
    {
        QPainter p(&padded);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(pad, pad, source.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    QImage rgba = padded.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QRhiTexture *srcTex = m_rhi->newTexture(QRhiTexture::RGBA8, rgba.size(), 1, {});
    if (!srcTex->create())
    {
        std::fprintf(stderr, "pb_gpu: finish srcTex create failed\n");
        return {};
    }

    struct FinishUb
    {
        float matrix[16];                          /* 0-63 */
        float texW, texH;                          /* 64-71 */
        float outW, outH;                          /* 72-79 */
        float dirX, dirY, distancePx, falloff;     /* 80-95 */
        int mode, samples;                         /* 96-103 */
        float padX, padY;                          /* 104-111 */
        float colorR, colorG, colorB, colorA;      /* 112-127 */
        float offsetX, offsetY;                    /* 128-135 */
        float threshold, intensity;                /* 136-143 */
    };                                             /* 144 */
    FinishUb ub;
    memset(&ub, 0, sizeof(ub));
    QMatrix4x4 ident;
    memcpy(ub.matrix, ident.constData(), 64);
    ub.texW = float(source.width());
    ub.texH = float(source.height());
    ub.outW = float(outSize.width());
    ub.outH = float(outSize.height());
    ub.padX = float(pad);
    ub.padY = float(pad);
    ub.mode = mode;
    if (mode == 0) /* DirectedBlur */
    {
        QPointF dir = node.direction;
        const double dl = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
        if (dl < 1e-6)
            dir = QPointF(1.0, 0.0);
        else
            dir /= dl;
        ub.dirX = float(dir.x());
        ub.dirY = float(dir.y());
        ub.distancePx = scale.worldToDevice(node.distance);
        ub.falloff = node.falloff;
        ub.samples = std::max(2, node.samples);
        ub.colorR = 1.0f; ub.colorG = 1.0f; ub.colorB = 1.0f; ub.colorA = 1.0f;
    }
    else if (mode == 1) /* Shadow */
    {
        QPointF off = node.offset;
        if (node.useLightRig)
            off = FinishRuntime::shadowDirection(rig) * double(node.lightDistance);
        ub.offsetX = scale.worldToDevice(float(off.x()));
        ub.offsetY = scale.worldToDevice(float(off.y()));
        ub.distancePx = scale.worldToDevice(node.blur);
        ub.colorR = node.color.redF();
        ub.colorG = node.color.greenF();
        ub.colorB = node.color.blueF();
        ub.colorA = qBound(0.0f, node.opacity, 1.0f);
        ub.samples = 8;
        ub.threshold = 0.0f;
        ub.intensity = 1.0f;
    }
    else /* Glow */
    {
        ub.distancePx = scale.worldToDevice(node.radius);
        ub.colorR = node.color.redF();
        ub.colorG = node.color.greenF();
        ub.colorB = node.color.blueF();
        ub.colorA = 1.0f;
        ub.samples = 8;
        ub.threshold = qBound(0.0f, node.threshold, 1.0f);
        ub.intensity = qBound(0.0f, node.intensity, 4.0f);
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->uploadTexture(srcTex, rgba);
    u->updateDynamicBuffer(m_pipelines->finishUb, 0, sizeof(ub), &ub);

    /* rebind the sampled texture for this call */
    QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelines->finishUb),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, srcTex, m_pipelines->finishSampler),
    });
    if (!srb->create())
    {
        std::fprintf(stderr, "pb_gpu: finish srb create failed\n");
        delete srcTex;
        return {};
    }

    QRhiBuffer *vb = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadClip));
    vb->create();
    u->uploadStaticBuffer(vb, kQuadClip);

    QRhiCommandBuffer *cb = nullptr;
    if (m_rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
        delete vb;
        delete srcTex;
        return {};
    }
    cb->beginPass(m_rt, Qt::transparent, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipelines->finish);
    cb->setShaderResources(srb);
    cb->setViewport({ 0, 0, float(outSize.width()), float(outSize.height()) });
    pbSetVertexInput(cb, vb);
    cb->draw(6);
    cb->endPass();
    m_rhi->endOffscreenFrame();
    delete vb;
    delete srb;

    QImage result = readback(m_target, outSize);
    delete srcTex;
    return unPremultiply(result);
}

} // namespace art
} // namespace paintbrush