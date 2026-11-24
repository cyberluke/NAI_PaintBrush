/*****************************************************************************
 * pb_lama_shim_smoke.cpp : integration smoke for the MinGW LaMa shim
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * This is NOT part of the MSVC DLL build (which cannot link MinGW Qt). It is a
 * small, manually built Qt console program that exercises the exact path the
 * paintbrush plugin uses:
 *
 *   pb_lama_cuda_shim.cpp (MinGW)
 *     -> %PB_LAMA_DLL%/pb_lama_cuda.dll (MSVC + ONNX Runtime CUDA)
 *     -> packaged closure + models/lama_fp32.onnx
 *
 * Build (MinGW Qt 6):
 *   g++ -std=c++17 -O1 -o lama_shim_smoke.exe pb_lama_shim_smoke.cpp ^
 *       pb_lama_cuda_shim.cpp -I<gpu dir> -I<QtCore> -I<QtGui> ... ^
 *       -lQt6Gui -lQt6Core
 *
 * Run:
 *   PB_LAMA_DLL=<package dir> lama_shim_smoke.exe <package dir>/models/lama_fp32.onnx
 *****************************************************************************/

#include "pb_lama.h"

#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2)
    {
        std::printf("usage: lama_shim_smoke <lama_fp32.onnx>\n");
        return 2;
    }
    const QString model = QString::fromLocal8Bit(argv[1]);

    const paintbrush::lama::Capabilities caps = paintbrush::lama::Runtime::probe();
    std::printf("probe: runtime=%d cuda=%d cpu=%d devices=%d ort=%s adapter=%s\n",
                caps.runtimePresent, caps.cudaAvailable, caps.cpuAvailable,
                caps.cudaDeviceCount, qPrintable(caps.ortVersion),
                qPrintable(caps.deviceName));
    if (!caps.runtimePresent)
    {
        std::printf("probe failed: %s\n", qPrintable(caps.detail));
        return 3;
    }

    paintbrush::lama::SessionConfig config;
    config.modelPath = model;
    config.device = 0; /* auto */
    config.expectedSha256 = QStringLiteral(
        "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6");
    config.requireGpuOperators = caps.cudaAvailable;

    paintbrush::lama::Runtime &runtime = paintbrush::lama::sharedRuntime();
    QString error;
    if (!runtime.load(config, &error))
    {
        std::printf("load failed: %s\n", qPrintable(error));
        return 4;
    }
    const paintbrush::lama::RunInfo backend = runtime.backendInfo();
    std::printf("loaded: backend=%s gpu=%d cudaNodes=%d cpuNodes=%d sha=%s\n",
                qPrintable(backend.backend), backend.usedGpu, backend.cudaNodes,
                backend.cpuNodes, qPrintable(backend.modelSha256));

    const int width = 640, height = 480;
    QImage source(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y)
    {
        uchar *line = source.scanLine(y);
        for (int x = 0; x < width; ++x)
        {
            line[x * 4 + 0] = uchar(std::min(255, 20 + x * 180 / width));
            line[x * 4 + 1] = uchar(std::min(255, 40 + y * 170 / height));
            line[x * 4 + 2] = uchar((x * 11 + y * 7) & 255);
            line[x * 4 + 3] = 255;
        }
    }
    QImage mask(width, height, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 180; y < 300; ++y)
        std::memset(mask.scanLine(y) + 240, 255, 160);

    paintbrush::lama::RunInfo info;
    QImage result;
    if (!runtime.inpaint(source, mask, &result, &info, &error))
    {
        std::printf("inpaint failed: %s\n", qPrintable(error));
        runtime.unload();
        return 5;
    }

    size_t unmaskedBad = 0, maskedChanged = 0, masked = 0;
    const QImage sourceCheck = source.convertToFormat(QImage::Format_ARGB32);
    const QImage resultCheck = result.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y)
    {
        const uchar *a = sourceCheck.constScanLine(y);
        const uchar *b = resultCheck.constScanLine(y);
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < width; ++x)
        {
            const uchar *pa = a + x * 4;
            const uchar *pb = b + x * 4;
            if (m[x] >= 128)
            {
                ++masked;
                if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2])
                    ++maskedChanged;
            }
            else
            {
                if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]
                    || pa[3] != pb[3])
                    ++unmaskedBad;
            }
        }
    }
    const bool ok = result.width() == width && result.height() == height
                    && unmaskedBad == 0 && maskedChanged > 0;
    std::printf("inpaint: %s size=%dx%d roi=%d,%d %dx%d components=%d "
                "infer=%.1fms total=%.1fms masked=%zu changed=%zu unmaskedBad=%zu\n",
                ok ? "ok" : "FAIL", result.width(), result.height(), info.roiX,
                info.roiY, info.roiW, info.roiH, info.components, info.inferenceMs,
                info.totalMs, masked, maskedChanged, unmaskedBad);
    runtime.unload();
    return ok ? 0 : 1;
}
