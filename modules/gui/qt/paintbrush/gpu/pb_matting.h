/*****************************************************************************
 * pb_matting.h : MinGW-facing neural matting runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The service layer speaks this API. It forwards every call to
 * pb_matting_cuda.dll (MSVC + ONNX Runtime + CUDA) through the narrow C ABI in
 * matting_cuda_api.h. When the optional DLL or its dependency closure is
 * missing, the matting service reports exactly what is missing and the
 * deterministic Raw/Refined modes keep working.
 *****************************************************************************/

#ifndef PB_MATTING_H
#define PB_MATTING_H

#include "../pb_matte.hpp"

#include <QImage>
#include <QRect>
#include <QString>

namespace paintbrush {
namespace matting {

struct Capabilities
{
    bool runtimePresent = false;   /* pb_matting_cuda.dll could be loaded     */
    bool cudaAvailable = false;
    bool cpuAvailable = false;
    int cudaDeviceCount = 0;
    QString ortVersion;
    QString deviceName;
    QString backend;               /* human readable backend line             */
    QString detail;                /* why it is unavailable, if it is         */
};

struct SessionConfig
{
    QString modelPath;
    QString manifestPath;
    QString profileDir;
    int device = 0;                /* 0 auto, 1 gpu required, 2 cpu           */
    int presetSize = 512;          /* square graph size                       */
    int intraThreads = 0;
    bool requireGpuOperators = true;
};

struct RunInfo
{
    double preprocessMs = -1.0;
    double inferenceMs = -1.0;
    double postprocessMs = -1.0;
    double totalMs = -1.0;
    int inputSize = 0;
    int roiX = 0;
    int roiY = 0;
    int roiW = 0;
    int roiH = 0;
    bool usedGpu = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    QString backend;
};

/* Process-wide runtime. One persistent session is shared by the interactive
 * service and the tracking worker; ORT sessions are internally
 * thread-safe for Run but we serialize runs in the DLL. */
class Runtime
{
public:
    static Capabilities probe();
    static bool dllPath(QString *path, QString *detail);

    bool load(const SessionConfig &config, QString *error);
    void unload();
    bool isLoaded() const { return m_session != nullptr; }
    RunInfo backendInfo() const;
    QString backend() const { return m_backend; }
    QString loadError() const { return m_loadError; }

    /* `trimap` must be Grayscale8 with values 0/128/255 and the same size as
     * `rgb`. `roi` is in source pixels; an empty rect means the whole frame.
     * The returned alpha is ROI-sized. */
    bool refine(const QImage &rgb, const QImage &trimap, const QRect &roi,
                matte::AlphaPlane *alpha, RunInfo *info, QString *error);

    /* Thread-safe: asks a Run() in flight to terminate. */
    void cancel();

private:
    void *m_session = nullptr;
    QString m_backend;
    QString m_loadError;
    int m_presetSize = 0;
    int m_device = 0;
    QString m_modelPath;
};

/* A process-wide instance so both the interactive refinement and the video
 * tracking worker use the same loaded model instead of allocating a second
 * copy of the weights. */
Runtime &sharedRuntime();

} // namespace matting
} // namespace paintbrush

#endif // PB_MATTING_H
