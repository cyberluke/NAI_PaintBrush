/*****************************************************************************
 * pb_powder_scene_node.hpp : zero-readback powder pass in the Qt Quick scene
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The offline GpuRuntime renders the powder into an RGBA texture, reads it
 * back to a QImage, then the canvas re-uploads that image as scene-graph tiles.
 * That round trip is why native powder could not match the WebGL reference.
 *
 * This node runs the SAME powder shader directly in the Qt Quick render pass:
 *
 *   CPU simulation data -> RGBA16F/RGBA8/R8 textures on the window RHI
 *   -> powder.frag draws into the current render target
 *
 * No readback, no QPainter composite, no tile re-upload. It is used for the
 * live powder union; the authoritative layer raster is baked (with readback)
 * only once when the stroke commits.
 *
 * UPLOAD CONTRACT (perf): the textures are FULL-grid and stay resident; every
 * frame only the dirty window (cellX/cellY/cellW/cellH) is uploaded with a
 * partial texture upload. The glue field is uploaded only when its revision
 * advances (it is 64 bytes per powder cell). The scene data is packed on the
 * GUI thread and handed over through setScene() inside updatePaintNode (the
 * GUI thread is blocked during the QSG sync, so no locking is needed).
 *****************************************************************************/

#ifndef PB_POWDER_SCENE_NODE_HPP
#define PB_POWDER_SCENE_NODE_HPP

#include <QtQuick/QSGRenderNode>

#include <QByteArray>
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QVector3D>

#include <atomic>

class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace paintbrush {
namespace art {

/* Everything the pass needs, packed on the GUI thread from the CPU sim. */
struct PowderSceneData
{
    QByteArray simHalf;   /* RGBA16F window: r=height g=seed b=stuck a=glue  */
    QByteArray col;       /* RGBA8 window packed palette slots                */
    QByteArray glue;      /* R8 window of the glue field; empty = no upload   */
    int gridW = 0;        /* full sim grid (texture size)                     */
    int gridH = 0;
    int cellX = 0;        /* packed window origin (cells)                     */
    int cellY = 0;
    int cellW = 0;        /* packed window size (cells)                       */
    int cellH = 0;
    int glueW = 0;        /* full sub-res glue dims; 0 = no glue -> 1x1 zero  */
    int glueH = 0;
    bool glueAny = false;
    quint64 glueRev = 0;  /* glue revision this pack represents               */

    QPointF worldOrigin;  /* layer-local world at the node's top-left         */
    float worldPerPx = 0.5f; /* layer-local world per DEVICE pixel             */
    float worldCell = 4.0f;
    float holo = 0.55f;
    float flakePx = 5.0f;
    float flakeWorld = 2.5f;
    float tooth = 0.4f;
    float mirror = 0.9f;
    float exposure = 1.0f;
    double timeMs = 0.0;
    QVector3D lightDir = QVector3D(0.3f, 0.4f, 0.85f);
    QColor paper = QColor(255, 255, 255);
    QVector<QColor> palette;

    bool valid() const
    {
        return gridW > 0 && gridH > 0 && cellW > 0 && cellH > 0
               && !simHalf.isEmpty();
    }
};

class PowderSceneNode : public QSGRenderNode
{
public:
    PowderSceneNode();
    ~PowderSceneNode() override;

    void setScene(const PowderSceneData &data);
    void setPaintRect(const QRectF &itemRect) { m_rect = itemRect; }

    /* True exactly once after the GPU resources were (re)created. The caller
     * must then pack the FULL grid window (and the glue field) once: only the
     * dirty window is uploaded per frame, so a freshly created texture has no
     * valid content outside it. */
    bool takeResync() { return m_resync.exchange(false); }
    /* True when the RHI pipeline/textures could not be created on this
     * backend. The producer must then stop routing the live preview through
     * this node and fall back to the CPU material renderer (slow but never
     * invisible). */
    bool failed() const { return m_failed.load(std::memory_order_acquire); }

    StateFlags changedStates() const override;
    void prepare() override;
    void render(const RenderState *state) override;
    void releaseResources() override;

private:
    bool ensureResources(QRhi *rhi, QRhiRenderPassDescriptor *rp);
    void destroyResources();
    void writeUniforms();

    PowderSceneData m_data;
    QRectF m_rect;
    bool m_dirty = true;

    QRhi *m_rhi = nullptr;
    QRhiRenderPassDescriptor *m_rp = nullptr;
    QRhiTexture *m_simTex = nullptr;
    QRhiTexture *m_colTex = nullptr;
    QRhiTexture *m_glueTex = nullptr;
    QRhiSampler *m_simSampler = nullptr;
    QRhiSampler *m_colSampler = nullptr;
    QRhiSampler *m_glueSampler = nullptr;
    QRhiBuffer *m_ubuf = nullptr;
    QRhiBuffer *m_vbuf = nullptr;
    QRhiShaderResourceBindings *m_srb = nullptr;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QByteArray m_ub;
    bool m_resourcesReady = false;
    bool m_glueZeroUploaded = false;
    /* Textures start undefined: draw nothing until a full-grid pack landed. */
    bool m_awaitFullScene = true;
    std::atomic<bool> m_resync{false};
    std::atomic<bool> m_failed{false};
};

} // namespace art
} // namespace paintbrush

#endif // PB_POWDER_SCENE_NODE_HPP
