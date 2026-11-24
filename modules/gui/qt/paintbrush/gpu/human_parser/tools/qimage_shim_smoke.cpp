/*****************************************************************************
 * qimage_shim_smoke.cpp : MinGW QImage -> C ABI integration smoke
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * The MSVC smoke proves the native runtime. This small Qt console program
 * proves the MinGW half of the contract that the application will use:
 *
 *   QImage (Qt) -> paintbrush::human_parser::Runtime (shim)
 *              -> C ABI -> pb_human_parser_cuda.dll (MSVC)
 *              -> Masks (QImage) back in MinGW
 *
 * It uses CPUExecutionProvider so the grid counts must match the exporter's
 * manifest exactly. The script scripts/pb-test-human-parser-shim.ps1 builds
 * it and stages the parser DLL plus the small ORT core closure.
 *****************************************************************************/

#include "pb_human_parser.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QStringList>

#include <cstdio>
#include <cstring>

namespace {

const quint64 kExpectedGrid[18] = {
    11530, 61, 236, 619, 2, 0, 568, 0, 0, 0, 0, 12, 206, 35, 279, 158,
    118, 0
};

void print_counts(const char *label, const QVector<quint64> &values)
{
    std::printf("%s", label);
    for (quint64 value : values)
        std::printf(" %llu", static_cast<unsigned long long>(value));
    std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4)
    {
        std::printf("usage: qimage_shim_smoke <model.onnx> <manifest.json> <image>\n");
        return 2;
    }

    using namespace paintbrush::human_parser;

    const Capabilities caps = Runtime::probe();
    std::printf("probe present=%d cuda=%d cpu=%d backend=%s\n",
                caps.runtimePresent ? 1 : 0, caps.cudaAvailable ? 1 : 0,
                caps.cpuAvailable ? 1 : 0, caps.backend.toUtf8().constData());
    if (!caps.runtimePresent)
    {
        std::printf("FAIL %s\n", caps.detail.toUtf8().constData());
        return 1;
    }

    SessionConfig config;
    config.modelPath = QString::fromLocal8Bit(argv[1]);
    config.manifestPath = QString::fromLocal8Bit(argv[2]);
    config.device = 2; /* CPU: exact exporter parity */
    config.verifyModelHash = true;

    Runtime runtime;
    QString error;
    QElapsedTimer timer;
    timer.start();
    if (!runtime.load(config, &error))
    {
        std::printf("FAIL load: %s\n", error.toUtf8().constData());
        return 1;
    }
    std::printf("loadMs=%lld backend=%s\n",
                static_cast<long long>(timer.elapsed()),
                runtime.backend().toUtf8().constData());

    QImage image(QString::fromLocal8Bit(argv[3]));
    if (image.isNull())
    {
        std::printf("FAIL image: %s\n", argv[3]);
        return 1;
    }
    image = image.convertToFormat(QImage::Format_RGB888).copy();

    Masks masks;
    RunInfo info;
    timer.restart();
    if (!runtime.parse(image, &masks, &info, &error))
    {
        std::printf("FAIL parse: %s\n", error.toUtf8().constData());
        return 1;
    }

    std::printf("parseMs=%lld inferMs=%.1f postMs=%.1f source=%dx%d grid=%dx%d\n",
                static_cast<long long>(timer.elapsed()), info.inferenceMs,
                info.postprocessMs, info.sourceWidth, info.sourceHeight,
                info.gridWidth, info.gridHeight);
    std::printf("classMap=%dx%d format=%d garment=%dx%d skin=%dx%d\n",
                masks.classMap.width(), masks.classMap.height(),
                int(masks.classMap.format()), masks.garment.width(),
                masks.garment.height(), masks.skin.width(), masks.skin.height());

    print_counts("GRID", masks.classPixelsGrid);
    print_counts("GROUPS", masks.groupPixels);

    if (masks.classPixelsGrid.size() != 18)
    {
        std::printf("FAIL grid count vector size\n");
        return 1;
    }
    int differences = 0;
    for (int i = 0; i < 18; ++i)
    {
        if (masks.classPixelsGrid[i] != kExpectedGrid[i])
        {
            std::printf("diff class %d got=%llu expected=%llu\n", i,
                        static_cast<unsigned long long>(masks.classPixelsGrid[i]),
                        static_cast<unsigned long long>(kExpectedGrid[i]));
            ++differences;
        }
    }
    if (differences != 0)
    {
        std::printf("FAIL exporter grid counts differ in %d classes\n", differences);
        return 1;
    }
    if (masks.classMap.width() != image.width()
        || masks.classMap.height() != image.height())
    {
        std::printf("FAIL class map size\n");
        return 1;
    }
    if (masks.groupPixels.size() != 6)
    {
        std::printf("FAIL group count vector size\n");
        return 1;
    }

    std::printf("SHIM SMOKE PASS\n");
    return 0;
}
