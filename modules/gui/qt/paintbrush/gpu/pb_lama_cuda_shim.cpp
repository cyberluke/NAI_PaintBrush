/*****************************************************************************
 * pb_lama_cuda_shim.cpp : MinGW implementation of the LaMa runtime API
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 *   MinGW plugin -> paintbrush::lama::Runtime (this file, MinGW STL)
 *                -> C ABI (lama/lama_runtime.h)
 *                -> pb_lama_cuda.dll (MSVC + ONNX Runtime + CUDA)
 *
 * DLL loading is explicit and restricted: the LaMa closure lives in
 * <exe>/lama, every dependency is preloaded from that directory with an
 * absolute path, and no global search path is modified. Preloading is what
 * makes ONNX Runtime's later LoadLibrary("cudnn64_9.dll") resolve to the
 * packaged copy instead of whatever happens to be on PATH.
 *
 * PB_LAMA_DLL overrides the runtime directory (test/portable install).
 *****************************************************************************/

#include "pb_lama.h"
#include "lama/lama_runtime.h"

#include <windows.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace paintbrush {
namespace lama {

namespace {

std::mutex g_apiMutex;
HMODULE g_apiModule = nullptr;
const PblmApi *g_api = nullptr;
bool g_apiTried = false;
std::string g_apiError;

std::wstring widen(const QString &text)
{
    return text.toStdWString();
}

QString exeDir()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, DWORD(sizeof(path)));
    if (len == 0 || len >= sizeof(path))
        return {};
    return QFileInfo(QString::fromWCharArray(path, int(len))).absolutePath();
}

QString moduleDir()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_apiModule), &self))
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(self, path, DWORD(sizeof(path)));
    if (len == 0 || len >= sizeof(path))
        return {};
    return QFileInfo(QString::fromWCharArray(path, int(len))).absolutePath();
}

HMODULE loadAbsolute(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return nullptr;
    return LoadLibraryExW(widen(path).c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                              | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

/* Dependency closure of pb_lama_cuda.dll / onnxruntime_providers_cuda.dll.
 * Names are pinned by scripts/pb-package-lama-cuda.ps1. Missing files are not
 * fatal here: session creation reports the precise ORT error. */
const char *kPreload[] = {
    "cudnn64_9.dll",
    "cudnn_graph64_9.dll",
    "cudnn_ops64_9.dll",
    "cudnn_cnn64_9.dll",
    "cudnn_adv64_9.dll",
    "cudnn_ext64_9.dll",
    "cudnn_heuristic64_9.dll",
    "cudnn_engines_precompiled64_9.dll",
    "cudnn_engines_runtime_compiled64_9.dll",
    "cudnn_engines_tensor_ir64_9.dll",
    "cublas64_13.dll",
    "cublasLt64_13.dll",
    "cudart64_13.dll",
    "onnxruntime_providers_shared.dll",
    "onnxruntime_providers_cuda.dll",
    "onnxruntime.dll",
};

void preloadClosure(const QString &dir)
{
    for (const char *name : kPreload)
        loadAbsolute(QDir(dir).filePath(QString::fromLatin1(name)));
}

QStringList missingClosure(const QString &dir)
{
    QStringList missing;
    for (const char *name : kPreload)
    {
        const QString path = QDir(dir).filePath(QString::fromLatin1(name));
        if (!QFileInfo::exists(path))
            missing << QString::fromLatin1(name);
    }
    return missing;
}

/* Resolve the runtime directory:
 *   1. PB_LAMA_DLL (explicit test/override)
 *   2. <exe>/lama
 *   3. <plugin>/lama
 *   4. <exe> (flat deployment)
 */
QString resolveRuntimeDir()
{
    const QByteArray env = qgetenv("PB_LAMA_DLL");
    if (!env.isEmpty())
        return QString::fromUtf8(env);

    const QString exe = exeDir();
    if (!exe.isEmpty()
        && QFileInfo::exists(QDir(exe).filePath(QStringLiteral("lama/pb_lama_cuda.dll"))))
        return QDir(exe).filePath(QStringLiteral("lama"));

    const QString plugin = moduleDir();
    if (!plugin.isEmpty()
        && QFileInfo::exists(QDir(plugin).filePath(QStringLiteral("lama/pb_lama_cuda.dll"))))
        return QDir(plugin).filePath(QStringLiteral("lama"));

    return exe;
}

bool ensureApi(QString *detail)
{
    std::lock_guard<std::mutex> lock(g_apiMutex);
    if (g_api)
        return true;
    if (g_apiTried)
    {
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    g_apiTried = true;

    const QString dir = resolveRuntimeDir();
    const QString dll = QDir(dir).filePath(QStringLiteral("pb_lama_cuda.dll"));
    if (!QFileInfo::exists(dll))
    {
        g_apiError = "pb_lama_cuda.dll is not installed";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    preloadClosure(dir);
    g_apiModule = loadAbsolute(dll);
    if (!g_apiModule)
    {
        g_apiError = "pb_lama_cuda.dll could not be loaded (missing runtime dependency?)";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    auto getApi = reinterpret_cast<PblmGetApiFn>(
        reinterpret_cast<void *>(GetProcAddress(g_apiModule, PBLM_GET_API_SYMBOL)));
    if (!getApi)
    {
        g_apiError = "pb_lama_cuda.dll has no " PBLM_GET_API_SYMBOL " export";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    const PblmApi *api = getApi();
    if (!api || api->abi_version != PBLM_ABI_VERSION
        || api->struct_size < sizeof(PblmApi))
    {
        g_apiError = "LaMa ABI version mismatch";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    g_api = api;
    return true;
}

} // namespace

bool Runtime::dllPath(QString *path, QString *detail)
{
    const QString dir = resolveRuntimeDir();
    if (path && !dir.isEmpty())
        *path = QDir(dir).filePath(QStringLiteral("pb_lama_cuda.dll"));
    if (detail && !dir.isEmpty())
    {
        const QStringList missing = missingClosure(dir);
        if (!missing.isEmpty())
            *detail = QStringLiteral("missing from %1: %2").arg(dir, missing.join(", "));
    }
    QString ignored;
    return ensureApi(detail ? detail : &ignored);
}

Capabilities Runtime::probe()
{
    Capabilities caps;
    QString detail;
    QString path;
    caps.runtimePresent = dllPath(&path, &detail);
    caps.detail = detail;
    if (!caps.runtimePresent)
        return caps;

    PblmCapabilities raw{};
    raw.struct_size = sizeof(raw);
    if (g_api->probe_capabilities(&raw) != PBLM_OK)
    {
        caps.detail = QString::fromUtf8(raw.error);
        return caps;
    }
    caps.cudaAvailable = raw.cuda_provider_available != 0 && raw.cuda_device_count > 0;
    caps.cpuAvailable = raw.cpu_fallback_available != 0;
    caps.cudaDeviceCount = raw.cuda_device_count;
    caps.ortVersion = QString::fromUtf8(raw.ort_version);
    caps.deviceName = QString::fromUtf8(raw.device_name);
    caps.backend = caps.cudaAvailable
        ? QStringLiteral("CUDAExecutionProvider (%1)").arg(caps.deviceName)
        : QStringLiteral("CPUExecutionProvider");
    if (raw.error[0])
        caps.detail = QString::fromUtf8(raw.error);
    /* A registered CUDA provider is not proof that its closure is complete.
     * The directory scan above reports missing cuDNN/CUDA/ORT files; treat
     * that as "GPU unavailable" instead of promising a backend that cannot
     * create a session. CPU stays available when only the GPU closure is
     * missing. */
    if (!detail.isEmpty())
    {
        caps.cudaAvailable = false;
        caps.detail = detail;
    }
    return caps;
}

bool Runtime::load(const SessionConfig &config, QString *error)
{
    unload();
    m_loadError.clear();

    QString detail;
    if (!ensureApi(&detail))
    {
        m_loadError = detail;
        if (error)
            *error = detail;
        return false;
    }
    if (config.modelPath.isEmpty() || !QFileInfo::exists(config.modelPath))
    {
        m_loadError = QStringLiteral("LaMa model not found: %1").arg(config.modelPath);
        if (error)
            *error = m_loadError;
        return false;
    }

    PblmSessionConfig raw{};
    raw.struct_size = sizeof(raw);
    const QByteArray model = config.modelPath.toUtf8();
    const QByteArray profile = config.profileDir.toUtf8();
    const QByteArray sha = config.expectedSha256.toUtf8();
    raw.model_path = model.constData();
    raw.profile_dir = profile.isEmpty() ? nullptr : profile.constData();
    raw.expected_sha256 = sha.isEmpty() ? nullptr : sha.constData();
    raw.device = config.device;
    raw.input_size = config.inputSize;
    raw.intra_threads = config.intraThreads;
    raw.require_gpu_operators = config.requireGpuOperators ? 1 : 0;
    raw.mask_grow_px = config.maskGrowPx;

    PblmSession *session = nullptr;
    char err[512] = {};
    const int32_t status = g_api->create_session(&raw, &session, err, sizeof(err));
    if (status != PBLM_OK || !session)
    {
        m_loadError = QStringLiteral("LaMa session failed: %1").arg(QString::fromUtf8(err));
        if (error)
            *error = m_loadError;
        return false;
    }

    m_session = session;
    m_inputSize = config.inputSize;
    m_device = config.device;
    m_modelPath = config.modelPath;

    PblmRunInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(session, &info) == PBLM_OK)
    {
        m_backend = QString::fromUtf8(info.backend);
        m_modelSha256 = QString::fromUtf8(info.model_sha256);
    }
    else
        m_backend = QStringLiteral("unknown backend");
    return true;
}

void Runtime::unload()
{
    if (m_session && g_api)
        g_api->destroy_session(static_cast<PblmSession *>(m_session));
    m_session = nullptr;
    m_backend.clear();
    m_modelPath.clear();
    m_modelSha256.clear();
    m_inputSize = 0;
    m_device = 0;
}

RunInfo Runtime::backendInfo() const
{
    RunInfo out;
    if (!m_session || !g_api)
        return out;
    PblmRunInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(static_cast<PblmSession *>(m_session), &info) != PBLM_OK)
        return out;
    out.inputSize = info.input_size;
    /* An older DLL leaves the appended fields zeroed; 0 ms is not a plausible
     * 208 MB load, so report it as unmeasured instead of a fake speed. */
    out.loadMs = info.load_ms > 0.0 ? info.load_ms : -1.0;
    out.usedGpu = info.used_gpu != 0;
    out.cudaNodes = info.cuda_node_count;
    out.cpuNodes = info.cpu_node_count;
    out.backend = QString::fromUtf8(info.backend);
    out.modelSha256 = QString::fromUtf8(info.model_sha256);
    return out;
}

bool Runtime::inpaint(const QImage &rgb, const QImage &mask, QImage *out,
                      RunInfo *info, QString *error)
{
    if (!m_session || !g_api)
    {
        if (error)
            *error = QStringLiteral("LaMa runtime is not loaded");
        return false;
    }
    if (!out || rgb.isNull() || mask.isNull() || rgb.size() != mask.size())
    {
        if (error)
            *error = QStringLiteral("LaMa input size mismatch");
        return false;
    }

    const QImage rgba = rgb.convertToFormat(QImage::Format_RGBA8888);
    const QImage mono = mask.convertToFormat(QImage::Format_Grayscale8);
    if (rgba.isNull() || mono.isNull())
    {
        if (error)
            *error = QStringLiteral("LaMa input conversion failed");
        return false;
    }

    PblmImage rawImage{};
    rawImage.data = rgba.constBits();
    rawImage.width = rgba.width();
    rawImage.height = rgba.height();
    rawImage.stride = int(rgba.bytesPerLine());
    rawImage.channels = 4;

    PblmMask rawMask{};
    rawMask.data = mono.constBits();
    rawMask.width = mono.width();
    rawMask.height = mono.height();
    rawMask.stride = int(mono.bytesPerLine());
    rawMask.threshold = 128;

    std::vector<uint8_t> buffer(size_t(rgba.width()) * size_t(rgba.height()) * 4);
    PblmRunInfo rawInfo{};
    rawInfo.struct_size = sizeof(rawInfo);
    char err[512] = {};
    const int32_t status = g_api->inpaint(
        static_cast<PblmSession *>(m_session), &rawImage, &rawMask, buffer.data(),
        int64_t(buffer.size()), &rawInfo, err, sizeof(err));
    if (status != PBLM_OK)
    {
        if (error)
            *error = QString::fromUtf8(err[0] ? err : "LaMa inference failed");
        return false;
    }

    /* Publication at the MinGW boundary: materialise the DLL's RGBA buffer as
     * an owned ARGB32 image. Measured separately from the DLL phases. */
    QElapsedTimer publishTimer;
    publishTimer.start();
    const QImage view(buffer.data(), rgba.width(), rgba.height(),
                      int(rgba.width()) * 4, QImage::Format_RGBA8888);
    *out = view.copy().convertToFormat(QImage::Format_ARGB32);
    const double publishMs = double(publishTimer.nsecsElapsed()) / 1e6;
    if (info)
    {
        /* Same old-DLL guard as backendInfo(): zero means the appended field
         * was never written, not a zero-cost load. */
        info->loadMs = rawInfo.load_ms > 0.0 ? rawInfo.load_ms : -1.0;
        info->maskAnalysisMs = rawInfo.mask_analysis_ms;
        info->preprocessMs = rawInfo.preprocess_ms;
        info->inferenceMs = rawInfo.inference_ms;
        info->compositeMs = rawInfo.postprocess_ms;
        info->postprocessMs = rawInfo.postprocess_ms;
        info->publishMs = publishMs;
        info->totalMs = rawInfo.total_ms;
        info->inputSize = rawInfo.input_size;
        info->roiX = rawInfo.roi_x;
        info->roiY = rawInfo.roi_y;
        info->roiW = rawInfo.roi_w;
        info->roiH = rawInfo.roi_h;
        info->components = rawInfo.components;
        info->usedGpu = rawInfo.used_gpu != 0;
        info->cudaNodes = rawInfo.cuda_node_count;
        info->cpuNodes = rawInfo.cpu_node_count;
        info->backend = QString::fromUtf8(rawInfo.backend);
        info->modelSha256 = QString::fromUtf8(rawInfo.model_sha256);
    }
    return true;
}

void Runtime::cancel()
{
    if (m_session && g_api)
        g_api->cancel_current(static_cast<PblmSession *>(m_session));
}

Runtime &sharedRuntime()
{
    static Runtime runtime;
    return runtime;
}

} // namespace lama
} // namespace paintbrush
