/* PB_POWDER_SURFACE.HPP — NAI Paintbrush GPU glitter-powder surface.
 *
 * A single reusable object that owns every GPU resource behind the reference
 * `powder.frag` chain and can render that shader's semantics into ANY QRhi
 * render target (the Qt Quick scene graph's live target for interactive
 * preview, OR an offscreen target for the deterministic commit bake / export).
 *
 * Keeping one shared "powder.frag surface" here is what guarantees the live
 * canvas preview and the committed document raster are the SAME glitter
 * product (the review's "interactive preview and export must not become two
 * different glitter products").
 *
 * Coordinate contract (matches powder.frag):
 *   - vPin = device px, top-left origin, y-down;
 *   - world = uWorldOrigin + vPin * uWorldPerPx;
 *   - sim/cell texture indexed by world / worldCell (the GlitterPowder grid).
 *
 * Resource-upload model: this object builds a QRhiResourceUpdateBatch for the
 * sim + packed-colour textures at any moment (setSimulation). The CALLER
 * submits that batch in the correct context for its rhi:
 *   - live scene-graph node: inside QSGRenderNode::render(), via the live
 *     command buffer's resourceUpdate() (bounded, dirty-region only);
 *   - offscreen commit/harness: inside its own beginOffscreenFrame.
 * Frame uniforms (uTime, view, origin, light) update the dynamic UBO the same
 * way (setUniforms builds a batch, caller submits).
 */
#ifndef PB_POWDER_SURFACE_HPP
#define PB_POWDER_SURFACE_HPP

#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <QVector3D>
#include <QSize>
#include <QRectF>
#include <QColor>

namespace paintbrush {
namespace art {

namespace glitter { struct GlitterPowder; struct PowderPalette; }

class PowderSurface
{
public:
    PowderSurface() = default;
    ~PowderSurface();

    PowderSurface(const PowderSurface &) = delete;
    PowderSurface &operator=(const PowderSurface &) = delete;

    /* (Re)create GPU resources against `rhi`. `rp` is the render-pass
     * descriptor of the target this surface renders into (interactive node:
     * the SG target's RPD; offscreen: its own target's RPD). `blendPremultiplied`
     * selects the compositing blend (true over the document tiles, false over an
     * empty/opaque offscreen target). */
    bool init(QRhi *rhi, QRhiRenderPassDescriptor *rp, bool blendPremultiplied);

    /* Rebuild the persistent sim + colour textures to `grid` and remember the
     * palette's colour/light parameters. */
    void resize(const QSize &grid, float worldCell,
                const glitter::PowderPalette &pal);

    /* Build a resource-update batch that (re)uploads only the cells inside
     * `dirtyWorldRect` (null = whole grid) of the persistent sim+colour
     * textures from `powder`. Caller submits the returned batch. */
    QRhiResourceUpdateBatch *uploadSimulation(const glitter::GlitterPowder &powder,
                                              const QRectF *dirtyWorldRect = nullptr);

    /* Build a resource-update batch that writes the frame uniforms into the
     * dynamic UBO. Normally called every frame so uTime (presentation-only)
     * and the view are current. Caller submits the returned batch. */
    QRhiResourceUpdateBatch *uploadUniforms(float originX, float originY,
                                            float worldPerPx,
                                            const QSize &viewDeviceSize,
                                            qint64 timeMs, int uiDpr = 1);

    /* Build a resource-update batch that uploads the immutable full-screen quad
     * once (for rhi backends where a prior offscreen pass was not possible).
     * Call exactly once after init, before the first render. */
    QRhiResourceUpdateBatch *uploadStaticQuad();

    /* Render the full-screen powder quad into the currently-started pass of
     * `cb`. The caller is responsible for having submitted the sim/col and
     * uniform batches via cb->resourceUpdate(...) beforehand. */
    void render(QRhiCommandBuffer *cb, const QRhiRenderTarget *rt);

    /* Treat current sim/col content as up-to-date (call after uploadSimulation
     * was submitted) — used to skip re-upload when nothing changed. */
    bool isReady() const { return m_rhi && m_powder && m_simTex && m_colTex; }
    QString lastError() const { return m_lastError; }

    QRhi *rhi() const { return m_rhi; }
    QRhiRenderPassDescriptor *renderPassDescriptor() const { return m_rp; }
    QSize grid() const { return m_grid; }
    float worldCell() const { return m_worldCell; }

    void release();

private:
    bool uploadCell(const glitter::GlitterPowder &powder, int idx,
                    quint8 *simOut, quint8 *colOut);
    void ensurePipeline();

    QRhi *m_rhi = nullptr;
    QRhiRenderPassDescriptor *m_rp = nullptr;
    bool m_blendPremultiplied = false;

    QRhiGraphicsPipeline *m_powder = nullptr;
    QRhiShaderResourceBindings *m_bindings = nullptr;
    QRhiBuffer *m_ub = nullptr;          /* dynamic, 512 B */
    QRhiBuffer *m_vb = nullptr;          /* full-screen quad */
    QRhiSampler *m_simSampler = nullptr; /* linear (float) */
    QRhiSampler *m_colSampler = nullptr; /* nearest (packed colour) */
    QRhiTexture *m_simTex = nullptr;     /* RGBA16F  W x H */
    QRhiTexture *m_colTex = nullptr;     /* RGBA8   W x H */

    QSize m_grid;
    float m_worldCell = 4.0f;
    QByteArray m_simCache;               /* W*H*8 float16 */
    QByteArray m_colCache;               /* W*H*4 packed */

    /* palette / light / material params */
    float m_holographic = 0.55f;
    float m_flakePx = 5.0f;
    float m_tooth = 0.4f;
    QColor m_paper{ 255, 255, 255 };
    QVector<QColor> m_palette;
    QVector3D m_lightDir{ -0.3f, 0.0f, 0.9f };

    QString m_lastError;
};

} // namespace art
} // namespace paintbrush

#endif // PB_POWDER_SURFACE_HPP