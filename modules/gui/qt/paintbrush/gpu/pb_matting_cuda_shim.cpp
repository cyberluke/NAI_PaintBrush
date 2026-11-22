/*****************************************************************************
 * pb_matting_cuda_shim.cpp : MinGW implementation of the matting runtime API
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 *   MinGW plugin -> paintbrush::matting::Runtime (this file, MinGW STL)
 *                -> C ABI (matting_cuda_api.h)
 *                -> pb_matting_cuda.dll (MSVC + ONNX Runtime + CUDA)
 *
 * DLL loading is explicit and restricted: the matting closure lives in
 * <exe>/matting, every dependency is preloaded from that directory with an
 * absolute path, and no global search path is modified. Preloading is what
 * makes ONNX Runtime's later LoadLibrary("cudnn64_9.dll") resolve to the
 * packaged copy instead of whatever happens to be on PATH.
 *****************************************************************************/

#include "pb_matting.h"
#include "matting_cuda_api.h"

#include <windows.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace paintbrush {
namespace matting {

namespace {

std::mutex g_apiMutex;
HMODULE g_apiModule = nullptr;
const PbmcApi *g_api = nullptr;
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

/* Dependency closure of pb_matting_cuda.dll / onnxruntime_providers_cuda.dll.
 * Names are pinned by scripts/pb-package-matting-cuda.ps1. Missing files are
 * not fatal here: the session creation reports the precise ORT error. */
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
 *   1. PB_MATTING_DIR (explicit test/override)
 *   2. <exe>/matting
 *   3. <plugin>/matting
 *   4. <exe> (flat deployment)
 */
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

    QString dir;
    {
        const QByteArray env = qgetenv("PB_MATTING_DIR");
        if (!env.isEmpty())
            dir = QString::fromUtf8(env);
    }
    if (dir.isEmpty())
    {
        const QString exe = exeDir();
        if (!exe.isEmpty() && QFileInfo::exists(QDir(exe).filePath(QStringLiteral("matting/pb_matting_cuda.dll"))))
            dir = QDir(exe).filePath(QStringLiteral("matting"));
        else
        {
            const QString plugin = moduleDir();
            if (!plugin.isEmpty() && QFileInfo::exists(QDir(plugin).filePath(QStringLiteral("matting/pb_matting_cuda.dll"))))
                dir = QDir(plugin).filePath(QStringLiteral("matting"));
            else
                dir = exe;
        }
    }

    const QString dll = QDir(dir).filePath(QStringLiteral("pb_matting_cuda.dll"));
    if (!QFileInfo::exists(dll))
    {
        g_apiError = "pb_matting_cuda.dll is not installed";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    preloadClosure(dir);
    g_apiModule = loadAbsolute(dll);
    if (!g_apiModule)
    {
        g_apiError = "pb_matting_cuda.dll could not be loaded (missing runtime dependency?)";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    auto getApi = reinterpret_cast<PbmcGetApiFn>(
        reinterpret_cast<void *>(GetProcAddress(g_apiModule, PBMC_GET_API_SYMBOL)));
    if (!getApi)
    {
        g_apiError = "pb_matting_cuda.dll has no " PBMC_GET_API_SYMBOL " export";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    const PbmcApi *api = getApi();
    if (!api || api->abi_version != PBMC_ABI_VERSION
        || api->struct_size < sizeof(PbmcApi))
    {
        g_apiError = "matting ABI version mismatch";
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
    QString dir;
    const QByteArray env = qgetenv("PB_MATTING_DIR");
    if (!env.isEmpty())
        dir = QString::fromUtf8(env);
    if (dir.isEmpty())
    {
        const QString exe = exeDir();
        if (!exe.isEmpty())
            dir = QDir(exe).filePath(QStringLiteral("matting"));
    }
    if (path && !dir.isEmpty())
        *path = QDir(dir).filePath(QStringLiteral("pb_matting_cuda.dll"));
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

    PbmcCapabilities raw{};
    raw.struct_size = sizeof(raw);
    if (g_api->probe_capabilities(&raw) != PBMC_OK)
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
        m_loadError = QStringLiteral("matting model not found: %1").arg(config.modelPath);
        if (error)
            *error = m_loadError;
        return false;
    }

    PbmcSessionConfig raw{};
    raw.struct_size = sizeof(raw);
    const QByteArray model = config.modelPath.toUtf8();
    const QByteArray manifest = config.manifestPath.toUtf8();
    const QByteArray profile = config.profileDir.toUtf8();
    raw.model_path = model.constData();
    raw.manifest_path = manifest.isEmpty() ? nullptr : manifest.constData();
    raw.profile_dir = profile.isEmpty() ? nullptr : profile.constData();
    raw.device = config.device;
    raw.preset_size = config.presetSize;
    raw.intra_threads = config.intraThreads;
    raw.require_gpu_operators = config.requireGpuOperators ? 1 : 0;

    PbmcSession *session = nullptr;
    char err[512] = {};
    const int32_t status = g_api->create_session(&raw, &session, err, sizeof(err));
    if (status != PBMC_OK || !session)
    {
        m_loadError = QStringLiteral("matting session failed: %1").arg(QString::fromUtf8(err));
        if (error)
            *error = m_loadError;
        return false;
    }

    m_session = session;
    m_presetSize = config.presetSize;
    m_device = config.device;
    m_modelPath = config.modelPath;

    PbmcRunInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(session, &info) == PBMC_OK)
        m_backend = QString::fromUtf8(info.backend);
    else
        m_backend = QStringLiteral("unknown backend");
    return true;
}

void Runtime::unload()
{
    if (m_session && g_api)
        g_api->destroy_session(static_cast<PbmcSession *>(m_session));
    m_session = nullptr;
    m_backend.clear();
    m_presetSize = 0;
    m_device = 0;
}

RunInfo Runtime::backendInfo() const
{
    RunInfo out;
    if (!m_session || !g_api)
        return out;
    PbmcRunInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(static_cast<PbmcSession *>(m_session), &info) != PBMC_OK)
        return out;
    out.inputSize = info.input_width;
    out.usedGpu = info.used_gpu != 0;
    out.cudaNodes = info.cuda_node_count;
    out.cpuNodes = info.cpu_node_count;
    out.backend = QString::fromUtf8(info.backend);
    return out;
}

bool Runtime::refine(const QImage &rgb, const QImage &trimap, const QRect &roi,
                     matte::AlphaPlane *alpha, RunInfo *info, QString *error)
{
    if (!m_session || !g_api)
    {
        if (error)
            *error = QStringLiteral("matting runtime is not loaded");
        return false;
    }
    if (rgb.isNull() || trimap.isNull() || rgb.size() != trimap.size())
    {
        if (error)
            *error = QStringLiteral("matting input size mismatch");
        return false;
    }

    const QImage rgb888 = rgb.convertToFormat(QImage::Format_RGB888);
    const QImage tri = trimap.convertToFormat(QImage::Format_Grayscale8);
    if (rgb888.isNull() || tri.isNull())
    {
        if (error)
            *error = QStringLiteral("matting input conversion failed");
        return false;
    }

    QRect region = roi.isNull() ? rgb.rect() : roi.intersected(rgb.rect());
    if (region.isEmpty())
    {
        if (error)
            *error = QStringLiteral("matting ROI is empty");
        return false;
    }

    PbmcImage rgbImage{};
    rgbImage.data = rgb888.constBits();
    rgbImage.width = rgb888.width();
    rgbImage.height = rgb888.height();
    rgbImage.stride = int(rgb888.bytesPerLine());

    PbmcImage triImage{};
    triImage.data = tri.constBits();
    triImage.width = tri.width();
    triImage.height = tri.height();
    triImage.stride = int(tri.bytesPerLine());

    PbmcRoi rawRoi{region.x(), region.y(), region.width(), region.height()};
    std::vector<float> buffer(size_t(region.width()) * size_t(region.height()));

    PbmcRunInfo rawInfo{};
    rawInfo.struct_size = sizeof(rawInfo);
    char err[512] = {};
    const int32_t status = g_api->refine_frame(
        static_cast<PbmcSession *>(m_session), &rgbImage, &triImage, &rawRoi,
        buffer.data(), int64_t(buffer.size()), &rawInfo, err, sizeof(err));
    if (status != PBMC_OK)
    {
        if (error)
            *error = QString::fromUtf8(err[0] ? err : "matting inference failed");
        return false;
    }

    *alpha = matte::AlphaPlane::fromFloat(buffer.data(), region.width(),
                                          region.height());
    if (info)
    {
        info->preprocessMs = rawInfo.preprocess_ms;
        info->inferenceMs = rawInfo.inference_ms;
        info->postprocessMs = rawInfo.postprocess_ms;
        info->totalMs = rawInfo.total_ms;
        info->inputSize = rawInfo.input_width;
        info->roiX = rawInfo.roi_x;
        info->roiY = rawInfo.roi_y;
        info->roiW = rawInfo.roi_w;
        info->roiH = rawInfo.roi_h;
        info->usedGpu = rawInfo.used_gpu != 0;
        info->cudaNodes = rawInfo.cuda_node_count;
        info->cpuNodes = rawInfo.cpu_node_count;
        info->backend = QString::fromUtf8(rawInfo.backend);
    }
    return true;
}

void Runtime::cancel()
{
    if (m_session && g_api)
        g_api->cancel_current(static_cast<PbmcSession *>(m_session));
}

Runtime &sharedRuntime()
{
    static Runtime runtime;
    return runtime;
}

} // namespace matting
} // namespace paintbrush
