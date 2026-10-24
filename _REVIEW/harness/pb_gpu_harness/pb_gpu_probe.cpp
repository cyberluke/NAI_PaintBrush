#include "pb_document.hpp"
#include "art/gpu/pb_gpu_runtime.hpp"
#include "art/surface/paper_renderer.hpp"
#include "art/style/mood.hpp"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <cstdio>

using namespace paintbrush;
using namespace paintbrush::art;

static void msgHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    std::fprintf(stderr, "[log %d %s] %s\n", int(type), ctx.category ? ctx.category : "?", qPrintable(msg));
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    qInstallMessageHandler(msgHandler);
    QLoggingCategory::setFilterRules(QStringLiteral("qt.graphics.rhi.debug=true\nqt.graphics.rhi=true"));
    std::printf("probe: init\n");
    GpuRuntime gpu;
    if (!gpu.initialize(true))
    {
        std::printf("init failed: %s\n", qPrintable(gpu.lastError()));
        return 1;
    }
    std::printf("backend=%s\n", qPrintable(gpu.backendName()));

    const PaperStock *stock = paper::findById(QStringLiteral("luxury.greige"));
    ArtLightRig rig = mood::defaultMood().light;
    ArtScale scale;
    scale.referenceDpi = 96.0f;
    scale.worldUnitsPerInch = 96.0f;
    scale.exportScale = 1.0f;

    std::printf("probe: renderPaper 64x48\n");
    QImage img = gpu.renderPaper(QSize(64, 48), *stock, rig, scale, 0.0f, 0.0f, 1.0f, QRect(0, 0, 64, 48));
    std::printf("result null=%d size=%dx%d\n", img.isNull(), img.width(), img.height());
    if (!img.isNull())
    {
        std::printf("row0: %08x %08x %08x %08x\n", img.pixel(0, 0), img.pixel(16, 0), img.pixel(32, 0), img.pixel(63, 0));
        std::printf("row24: %08x %08x %08x %08x\n", img.pixel(0, 24), img.pixel(16, 24), img.pixel(32, 24), img.pixel(63, 24));
        std::printf("row47: %08x %08x %08x %08x\n", img.pixel(0, 47), img.pixel(16, 47), img.pixel(32, 47), img.pixel(63, 47));
        img.save("_REVIEW/harness/pb_gpu_harness/build/paper_probe.png");
    }

    /* finish probe */
    std::printf("probe: renderFinish blur\n");
    QImage subject(64, 48, QImage::Format_ARGB32);
    subject.fill(Qt::transparent);
    {
        QPainter p(&subject);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(QColor(0xE8, 0x6A, 0x8A));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(32, 24), 18, 16);
        p.end();
    }
    EffectNode e = EffectNode::makeMotionBlur(QPointF(1.0, 0.0), 24.0f, 0.6f, 12);
    QImage fimg = gpu.renderFinish(subject, 0, e, rig, scale);
    std::printf("finish null=%d size=%dx%d\n", fimg.isNull(), fimg.width(), fimg.height());
    if (!fimg.isNull())
        fimg.save("_REVIEW/harness/pb_gpu_harness/build/finish_probe.png");
    return 0;
}