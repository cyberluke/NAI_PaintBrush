/*****************************************************************************
 * pb_lama.h : MinGW-facing native LaMa inpaint runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The service layer speaks this API. It forwards every call to
 * pb_lama_cuda.dll (MSVC + ONNX Runtime + CUDA) through the narrow C ABI in
 * lama/lama_runtime.h. When the optional DLL, its dependency closure or the
 * pinned ONNX graph is missing, the service reports exactly what is missing
 * and the existing provider / local-heal fallback keeps working.
 *
 * Fixed 512 export: this is context-ROI inpainting, not native-resolution
 * synthesis. Unmasked pixels are preserved by the DLL.
 *****************************************************************************/

#ifndef PB_LAMA_H
#define PB_LAMA_H

#include <QImage>
#include <QString>

namespace paintbrush {
namespace lama {

struct Capabilities
{
    bool runtimePresent = false;   /* pb_lama_cuda.dll could be loaded       */
    bool cudaAvailable = false;
    bool cpuAvailable = false;
    int cudaDeviceCount = 0;
    QString ortVersion;
    QString deviceName;
    QString backend;               /* human readable backend line            */
    QString detail;                /* why it is unavailable, if it is        */
};

struct SessionConfig
{
    QString modelPath;
    QString profileDir;
    QString expectedSha256;        /* optional pin, lowercase hex            */
    int device = 0;                /* 0 auto, 1 gpu required, 2 cpu          */
    int inputSize = 512;           /* fixed graph size                       */
    int intraThreads = 0;
    bool requireGpuOperators = true;
    int maskGrowPx = -1;           /* -1 auto, 0 none (parity diagnostics)   */
};

/* Phase timing in milliseconds. All values are measured with steady_clock
 * inside the native runtime except publishMs, which is the MinGW-side result
 * handoff (RGBA buffer -> QImage copy/format conversion). totalMs is the DLL
 * side of the call and does not include publishMs. loadMs is the persistent
 * session load and is the same for every run of that session.
 *
 * compositeMs and postprocessMs are the same measurement; compositeMs is the
 * explicit phase name and postprocessMs is kept for API compatibility. */
struct RunInfo
{
    double loadMs = -1.0;
    double maskAnalysisMs = -1.0;
    double preprocessMs = -1.0;
    double inferenceMs = -1.0;
    double compositeMs = -1.0;
    double postprocessMs = -1.0;   /* alias of compositeMs */
    double publishMs = -1.0;
    double totalMs = -1.0;
    int inputSize = 0;
    int roiX = 0;
    int roiY = 0;
    int roiW = 0;
    int roiH = 0;
    int components = 0;
    bool usedGpu = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    QString backend;
    QString modelSha256;
};

/* Process-wide runtime. One persistent session is shared by every consumer;
 * ORT sessions are internally thread-safe for Run but the DLL serializes runs
 * so VRAM stays bounded. */
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
    QString modelPath() const { return m_modelPath; }

    /* `rgb` and `mask` must have the same size. The mask is Grayscale8 (or any
     * format convertible to it) and masked pixels are >= 128. The returned
     * image is ARGB32, full canvas; unmasked pixels are preserved exactly. */
    bool inpaint(const QImage &rgb, const QImage &mask, QImage *out,
                 RunInfo *info, QString *error);

    /* Thread-safe: asks a Run() in flight to terminate. */
    void cancel();

private:
    void *m_session = nullptr;
    QString m_backend;
    QString m_loadError;
    QString m_modelPath;
    QString m_modelSha256;
    int m_inputSize = 0;
    int m_device = 0;
};

/* A process-wide instance so the generation service never allocates a second
 * copy of the 208 MB weights. */
Runtime &sharedRuntime();

} // namespace lama
} // namespace paintbrush

#endif // PB_LAMA_H
