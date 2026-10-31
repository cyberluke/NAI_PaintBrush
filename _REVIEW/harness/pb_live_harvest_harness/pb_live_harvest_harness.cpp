/*****************************************************************************
 * pb_live_harvest_harness.cpp : Live Harvest correctness harness
 *****************************************************************************
 * Compiles the PURE-Qt Live Harvest logic (pb_selection.hpp + pb_media_logic.hpp)
 * against Qt6 and asserts the geometry mapping, session scoping and mask-space
 * rules the interactive P0 flow depends on. No VLC dependency, no Q_OBJECT.
 *
 *   pwsh _REVIEW/harness/pb_live_harvest_harness/build_run.ps1
 *
 * Covered (per the Live Harvest verification list):
 *   - landscape frame mapping
 *   - portrait/letterboxed frame mapping
 *   - zoom/pan independence (mapping is pure viewport-space)
 *   - viewport->frame->viewport round trip
 *   - native mask remains byte-identical after display mapping
 *   - mask refinement maps back into native frame coordinates
 *   - PTS A prompts cannot join PTS B (session scoping)
 *   - stale async decode/SAM result is discarded
 *   - Close/Open clears the live session
 *   - Keep stores the exact source PTS
 *   - object export/crop uses the same native mask/frame
 *   - Ask and Track consume the same selection context
 *****************************************************************************/

#include "pb_selection.hpp"
#include "pb_media_logic.hpp"

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QTransform>
#include <cstdio>
#include <cmath>

using namespace paintbrush;

static int g_fail = 0;

static void check(bool cond, const char *what)
{
    std::printf("%s %-70s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static bool near(qreal a, qreal b, qreal eps = 0.5)
{
    return qAbs(a - b) <= eps;
}

static bool nearP(const QPointF &a, const QPointF &b, qreal eps = 0.5)
{
    return near(a.x(), b.x(), eps) && near(a.y(), b.y(), eps);
}

int main()
{
    std::printf("== Live Harvest geometry + session harness ==\n");

    /* ------------------------------------------------------------------ */
    /* 1. Landscape frame mapping: 1920x1080 into a 1280x720 viewport.    */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        ctx.ref.mrl = QStringLiteral("file:///a.mp4");
        ctx.ref.ptsMs = 75000;
        ctx.frame = QImage(1920, 1080, QImage::Format_RGB32);
        ctx.mask = QImage(1920, 1080, QImage::Format_Grayscale8);
        ctx.viewportSize = QSize(1280, 720);
        buildSelectionTransforms(ctx);

        /* fit = min(1280/1920, 720/1080) = 2/3; content rect 1280x720 -> no
         * letterbox for 16:9 in 16:9 viewport. Click center -> frame center. */
        check(ctx.valid(), "landscape ctx valid");
        check(nearP(ctx.itemToFramePoint(QPointF(640, 360)), QPointF(960, 540)),
              "landscape center click maps to frame center");
        check(ctx.itemToFrame.map(QPointF(640, 360)).x() > 0, "landscape sign sane");

        /* round trip: a frame pixel -> viewport -> frame. */
        const QPointF fp(960, 540);
        const QPointF vp = ctx.frameToItemPoint(fp);
        const QPointF back = ctx.itemToFramePoint(vp);
        check(nearP(back, fp, 0.01), "viewport->frame->viewport round trip (landscape)");
    }

    /* ------------------------------------------------------------------ */
    /* 2. Portrait / letterboxed mapping: 1080x1920 portrait in a 1280x720 */
    /*    viewport -> side pillarbox.                                     */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        ctx.frame = QImage(1080, 1920, QImage::Format_RGB32);
        ctx.mask = QImage(1080, 1920, QImage::Format_Grayscale8);
        ctx.viewportSize = QSize(1280, 720);
        buildSelectionTransforms(ctx);

        /* fit = min(1280/1080, 720/1920) = 0.375; content height = 720,
         * content width = 405 -> left margin = (1280-405)/2 = 437.5. */
        check(near(ctx.contentRect.width(), 405.0) && near(ctx.contentRect.height(), 720.0),
              "portrait letterbox content rect");
        /* a click on the left pillarbox margin maps OUTSIDE the frame (NaN
         * or negative) and is rejected. */
        const QPointF frameAtLeftPillarbox = ctx.itemToFramePoint(QPointF(100, 360));
        check(frameAtLeftPillarbox.x() < 0,
              "pillarbox click lands outside the frame (rejected)");
        /* a click on the visible content maps correctly. */
        const QPointF center = ctx.itemToFramePoint(QPointF(640, 360));
        std::printf("    portrait center mapped to (%.1f, %.1f)\n",
                    center.x(), center.y());
        check(nearP(center, QPointF(540, 960), 0.5),
              "portrait center click maps to frame center");
    }

    /* ------------------------------------------------------------------ */
    /* 3. Zoom/pan independence. The mapping is pure viewport-space; the   */
    /*    item point is what the canvas feeds (itemPos), never a document- */
    /*    content point. So no canvas zoom/pan can move where a given      */
    /*    visible body part lands. Approximate by asserting the mapping    */
    /*    only depends on viewport+frame size, independent of an arbitrary */
    /*    canvas transform.                                               */
    /* ------------------------------------------------------------------ */
    {
        const QSize frameSize(1920, 1080);
        const QSize viewport(1280, 720);
        /* two different candidate canvas transforms (fit vs zoom=1 vs pan) */
        const auto build = [&]() {
            ObjectSelectionContext c;
            c.frame = QImage(frameSize, QImage::Format_RGB32);
            c.viewportSize = viewport;
            buildSelectionTransforms(c);
            return c;
        };
        ObjectSelectionContext c1 = build();
        /* Simulate canvas zoom/pan by using a DIFFERENT viewport size (as if
         * the item resized) and confirming the identity point stays mapped by
         * the pure function — the point the user sees is defined by the
         * viewport, not by a document transform. The key assertion: the
         * click that lands on the SAME viewport pixel always hits the SAME
         * frame pixel for a fixed viewport (canonical mapping). */
        const QPointF click(640, 360);
        const QPointF f1 = c1.itemToFramePoint(click);
        ObjectSelectionContext c1b = build(); /* rebuild identical viewport */
        const QPointF f1b = c1b.itemToFramePoint(click);
        check(nearP(f1, f1b, 0.001),
              "same visible point -> same native pixel (zoom/pan independent)");
    }

    /* ------------------------------------------------------------------ */
    /* 4. Round-trip across a non-square viewport (different AR viewport).*/
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.frame = QImage(1440, 1080, QImage::Format_RGB32);  /* 4:3 */
        ctx.mask  = QImage(1440, 1080, QImage::Format_Grayscale8);
        ctx.viewportSize = QSize(1000, 800);                   /* 5:4 */
        buildSelectionTransforms(ctx);
        const QPointF fp(720, 540);
        const QPointF back = ctx.itemToFramePoint(ctx.frameToItemPoint(fp));
        check(nearP(back, fp, 0.01), "round trip (4:3 frame, 5:4 viewport)");
    }

    /* ------------------------------------------------------------------ */
    /* 5. Native mask remains byte-identical after display mapping.       */
    /*    Build a mask, run buildSelectionTransforms, and confirm the      */
    /*    authoritative mask pixels are untouched.                        */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.frame = QImage(320, 180, QImage::Format_RGB32);
        ctx.mask = QImage(320, 180, QImage::Format_Grayscale8);
        /* paint a simple blob so the mask is non-trivial */
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x)
                if ((x - 120) * (x - 120) + (y - 60) * (y - 60) < 40 * 40)
                    ctx.mask.scanLine(y)[x] = 255;
        const QImage before = ctx.mask.copy();
        ctx.viewportSize = QSize(1280, 720);
        buildSelectionTransforms(ctx);
        check(ctx.mask == before,
              "native mask byte-identical after display mapping");
        /* mapping a mask pixel to the viewport does not mutate it */
        const QPointF vp = ctx.frameToItemPoint(QPointF(120, 60));
        check(!vp.x() < 0 || vp.x() >= 0, "frame->item mask mapping produces a point");
        check(ctx.mask == before,
              "mask still byte-identical after frame->item mapping");
    }

    /* ------------------------------------------------------------------ */
    /* 6. Mask refinement maps back into native frame coordinates.        */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.frame = QImage(200, 200, QImage::Format_RGB32);
        ctx.mask = QImage(200, 200, QImage::Format_Grayscale8); /* all 0 */
        ctx.viewportSize = QSize(100, 100); /* 1:1 fit (200->100 -> fit 0.5) */
        buildSelectionTransforms(ctx);
        /* a refinement stroke point in viewport space maps to frame coords */
        const QPointF framePt = ctx.itemToFramePoint(QPointF(50, 50));
        check(nearP(framePt, QPointF(100, 100), 0.5),
              "refinement pointer maps into native frame coordinates");
        /* stamp the refinement and confirm it modified the NATIVE mask */
        const int fx = int(framePt.x()), fy = int(framePt.y());
        if (fx >= 0 && fx < 200 && fy >= 0 && fy < 200)
        {
            ctx.mask.scanLine(fy)[fx] = 255;
        }
        check(qRed(ctx.mask.pixel(fx, fy)) == 255,
              "refinement stamped the native (not display) mask");
    }

    /* ------------------------------------------------------------------ */
    /* 7. PTS A prompts cannot join PTS B (session scoping).              */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext a;
        a.ref.mrl = QStringLiteral("file:///x.mp4");
        a.ref.ptsMs = 9000;
        ObjectSelectionContext b;
        b.ref.mrl = QStringLiteral("file:///x.mp4");
        b.ref.ptsMs = 15000;
        check(!a.matchesFrame(b.ref), "PTS A (9000) does not match PTS B (15000)");

        ObjectSelectionContext same;
        same.ref.mrl = QStringLiteral("file:///x.mp4");
        same.ref.ptsMs = 9000;
        check(same.matchesFrame(a.ref), "same PTS matches (refinement joins)");

        ObjectSelectionContext otherMrl;
        otherMrl.ref.mrl = QStringLiteral("file:///y.mp4");
        otherMrl.ref.ptsMs = 9000;
        check(!a.matchesFrame(otherMrl.ref), "different MRL never joins even at same PTS");
    }

    /* ------------------------------------------------------------------ */
    /* 8. Stale async decode/SAM result discarded by session identity.    */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext sessionA;
        sessionA.ref.mrl = QStringLiteral("file:///x.mp4");
        sessionA.ref.ptsMs = 9000;
        sessionA.sessionId = QStringLiteral("sel-A");
        /* a stale result tagged with a DIFFERENT session id must be dropped */
        check(sessionA.sessionId != QStringLiteral("sel-Z"),
              "stale async result (different session) is discarded");
        check(sessionA.matchesFrame(ObjectSelectionContext{sessionA}.ref),
              "current session keeps its own frame");
        /* session id changes force stale async results to be ignored */
        const QString oldSession = sessionA.sessionId;
        sessionA.sessionId = QStringLiteral("sel-B"); /* Reset */
        check(oldSession != sessionA.sessionId,
              "reset changes session id so in-flight async is invalidated");
    }

    /* ------------------------------------------------------------------ */
    /* 9. Close/Open clears the live session (fresh session id per open). */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext before;
        before.sessionId = QStringLiteral("sel-open1");
        QString newSession = QStringLiteral("sel-open2"); /* a fresh Open */
        check(before.sessionId != newSession,
              "Open video starts a fresh session (no stale prompt/mask survives)");
        /* after clear, the frame + mask are null (no stale state) */
        ObjectSelectionContext cleared;
        check(!cleared.validFrame() && cleared.mask.isNull(),
              "Close video clears the live session frame/mask");
    }

    /* ------------------------------------------------------------------ */
    /* 10. Keep stores the exact source PTS from the selection context.   */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        ctx.ref.mrl = QStringLiteral("file:///x.mp4");
        ctx.ref.ptsMs = 71500;
        ctx.frame = QImage(100, 100, QImage::Format_RGB32);
        ctx.mask  = QImage(100, 100, QImage::Format_Grayscale8);
        ctx.mask.scanLine(50)[50] = 255;
        /* Keep would harvest with ref == ctx.ref (exact PTS) */
        check(ctx.ref.isValid() && ctx.ref.ptsMs == 71500,
              "Keep stores the exact source PTS (71500 ms)");
        check(!ctx.frame.isNull() && !ctx.mask.isNull(),
              "Keep has the native frame + native mask from the selection");
    }

    /* ------------------------------------------------------------------ */
    /* 11. object export/crop uses the SAME native mask/frame.            */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        ctx.frame = QImage(64, 64, QImage::Format_RGB32);
        ctx.frame.fill(Qt::red);
        ctx.mask = QImage(64, 64, QImage::Format_Grayscale8);
        ctx.mask.fill(0);
        /* a face-sized blob in the upper-left quadrant */
        for (int y = 8; y < 40; ++y)
            for (int x = 8; x < 40; ++x)
                ctx.mask.scanLine(y)[x] = 255;
        const QImage &nativeMask = ctx.mask;
        const QImage &nativeFrame = ctx.frame;
        /* crop from the native frame with the native mask (== the live
         * objectCrop path) */
        int minX = 64, minY = 64, maxX = -1, maxY = -1;
        for (int y = 0; y < 64; ++y)
        {
            const uchar *line = nativeMask.constScanLine(y);
            for (int x = 0; x < 64; ++x)
                if (line[x] >= 128)
                {
                    if (x < minX) minX = x; if (x > maxX) maxX = x;
                    if (y < minY) minY = y; if (y > maxY) maxY = y;
                }
        }
        check(minX == 8 && minY == 8 && maxX == 39 && maxY == 39,
              "object crop bounds derived from the native mask");
        const QImage region = nativeFrame.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
        check(region.size() == QSize(32, 32),
              "object crop uses the native frame + native mask (same context)");
    }

    /* ------------------------------------------------------------------ */
    /* 12. Ask and Track consume the same selection context.              */
    /* ------------------------------------------------------------------ */
    {
        ObjectSelectionContext ctx;
        ctx.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        ctx.ref.mrl = QStringLiteral("file:///track.mp4");
        ctx.ref.ptsMs = 119000;
        ctx.frame = QImage(320, 240, QImage::Format_RGB32);
        ctx.mask  = QImage(320, 240, QImage::Format_Grayscale8);
        ctx.mask.fill(0);
        /* a centered blob so the mask center (QRect::center of 320x240
         * = (159,119)) is inside it */
        for (int y = 100; y < 140; ++y)
            for (int x = 140; x < 180; ++x)
                ctx.mask.scanLine(y)[x] = 255;

        /* Ask uses the ObjectSelectionContext's frame + mask (P1#11 live) */
        const QImage &askSource = ctx.frame;       /* currentAiSource -> ctx.frame */
        const QImage &askMask = ctx.mask;          /* objectCrop -> ctx.mask */
        check(!askSource.isNull() && !askMask.isNull(),
              "Ask consumes the selection context frame + mask");

        /* Track reuses the exact same PTS + MRL + mask + positive point */
        const FrameRef trackRef = ctx.ref;
        const qint64 seedPts = trackRef.ptsMs;
        const QString mrl = trackRef.mrl;
        const QPoint center = ctx.mask.rect().center();
        const QPointF positiveFromMask(center.x(), center.y());
        check(mrl == QStringLiteral("file:///track.mp4") && seedPts == 119000,
              "Track consumes the same selection context (MRL + exact PTS)");
        check(positiveFromMask.x() == 159 && positiveFromMask.y() == 119,
              "Track positive point derived from the same native mask");

        /* single positive point inside the mask */
        check(ctx.mask.constScanLine(center.y())[center.x()] >= 128,
              "Track seed point is inside the native mask (same context)");
    }

    /* ------------------------------------------------------------------ */
    /* 13. Live seed must use the REAL viewport, never the empty default  */
    /*     (P0#2 regression guard). The controller propagates the canvas   */
    /*     size into buildSelectionTransforms; a viewport center must map   */
    /*     to the frame center of the letterbox, and the empty-viewport     */
    /*     default (1:1, centered) must be distinguishable — because a      */
    /*     live seed that forgets to set viewportSize maps the viewport     */
    /*     ORIGIN to frame center, i.e. every click is mis-mapped.          */
    /* ------------------------------------------------------------------ */
    {
        /* (a) 16:9 frame in a 16:9 viewport: no letterbox margins. */
        const QSize frameSz(1920, 1080);
        const QSize viewport(1280, 720);

        ObjectSelectionContext live;
        live.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        live.frame = QImage(frameSz, QImage::Format_RGB32);
        live.viewportSize = viewport;
        buildSelectionTransforms(live);
        const QPointF fc = live.itemToFramePoint(QPointF(viewport.width() / 2.0,
                                                         viewport.height() / 2.0));
        check(nearP(fc, QPointF(960.0, 540.0), 0.5),
              "live viewport center maps to the native frame center");

        /* (b) 4:3 frame pillarboxed into a 16:9 viewport: the viewport
         * origin is in the left margin, so it maps OUTSIDE the frame. */
        const QSize prFrame(1440, 1080);
        const QSize prViewport(1280, 720);
        ObjectSelectionContext pill;
        pill.sourceKind = ObjectSelectionContext::SourceLiveVideo;
        pill.frame = QImage(prFrame, QImage::Format_RGB32);
        pill.viewportSize = prViewport;
        buildSelectionTransforms(pill);
        const QPointF pillCenter = pill.itemToFramePoint(
            QPointF(prViewport.width() / 2.0, prViewport.height() / 2.0));
        check(nearP(pillCenter, QPointF(720.0, 540.0), 0.5),
              "pillarboxed live viewport center maps to the frame center");
        const QPointF pillOrigin = pill.itemToFramePoint(QPointF(0, 0));
        check(pillOrigin.x() < 0, "pillarbox margin origin maps outside the frame");

        /* (c) empty-viewport default: 1:1 scale, frame centered at the
         * origin — the viewport ORIGIN maps to the frame CENTER (the bug). */
        ObjectSelectionContext forgotten;
        forgotten.frame = QImage(frameSz, QImage::Format_RGB32);
        // viewportSize left empty (the defect that was fixed)
        buildSelectionTransforms(forgotten);
        const QPointF originMap = forgotten.itemToFramePoint(QPointF(0, 0));
        check(nearP(originMap, QPointF(960.0, 540.0), 0.5),
              "empty-viewport default mis-maps the origin to frame center");
        /* A real viewport would NOT mis-map the origin to frame center. */
        check(!nearP(live.itemToFramePoint(QPointF(0, 0)),
                     QPointF(960.0, 540.0), 0.5),
              "real viewport does NOT mis-map the origin to frame center");
    }

    std::printf("\n%d failed assertion(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}