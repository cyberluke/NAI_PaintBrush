/*****************************************************************************
 * pb_human_parser_shim.cpp : MinGW implementation of the human parser API
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 *   MinGW plugin -> paintbrush::human_parser::Runtime (this file, MinGW STL)
 *                -> C ABI (human_parser_cuda_api.h)
 *                -> pb_human_parser_cuda.dll (MSVC + ONNX Runtime + CUDA)
 *
 * DLL loading is explicit and restricted: the parser closure lives in
 * <exe>/human-parser, every dependency is preloaded from that directory with
 * an absolute path, and no global search path is modified. Preloading is what
 * makes ONNX Runtime's later LoadLibrary("cudnn64_9.dll") resolve to the
 * packaged copy instead of whatever happens to be on PATH.
 *****************************************************************************/

#include "pb_human_parser.h"
#include "human_parser_cuda_api.h"

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
namespace human_parser {

namespace {

std::mutex g_apiMutex;
HMODULE g_apiModule = nullptr;
const PbhApi *g_api = nullptr;
bool g_apiTried = false;
std::string g_apiError;

std::wstring widen(const QString &text)
{
    return text.toStdWString();
}

QString exeDir()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, DWORD(MAX_PATH));
    if (len == 0 || len >= MAX_PATH)
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
    const DWORD len = GetModuleFileNameW(self, path, DWORD(MAX_PATH));
    if (len == 0 || len >= MAX_PATH)
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

/* Dependency closure of pb_human_parser_cuda.dll / onnxruntime_providers_cuda.dll.
 * Names are pinned by the existing matting packaging recipe. Missing files are
 * not fatal here: session creation reports the precise ORT error. */
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
 *   1. PB_HUMAN_PARSER_DIR (explicit test/override)
 *   2. <exe>/human-parser
 *   3. <plugin>/human-parser
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
        const QByteArray env = qgetenv("PB_HUMAN_PARSER_DIR");
        if (!env.isEmpty())
            dir = QString::fromUtf8(env);
    }
    if (dir.isEmpty())
    {
        const QString exe = exeDir();
        if (!exe.isEmpty()
            && QFileInfo::exists(QDir(exe).filePath(
                   QStringLiteral("human-parser/pb_human_parser_cuda.dll"))))
            dir = QDir(exe).filePath(QStringLiteral("human-parser"));
        else
        {
            const QString plugin = moduleDir();
            if (!plugin.isEmpty()
                && QFileInfo::exists(QDir(plugin).filePath(
                       QStringLiteral("human-parser/pb_human_parser_cuda.dll"))))
                dir = QDir(plugin).filePath(QStringLiteral("human-parser"));
            else
                dir = exe;
        }
    }

    const QString dll = QDir(dir).filePath(
        QStringLiteral("pb_human_parser_cuda.dll"));
    if (!QFileInfo::exists(dll))
    {
        g_apiError = "pb_human_parser_cuda.dll is not installed";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    preloadClosure(dir);
    g_apiModule = loadAbsolute(dll);
    if (!g_apiModule)
    {
        g_apiError = "pb_human_parser_cuda.dll could not be loaded "
                     "(missing runtime dependency?)";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }

    auto getApi = reinterpret_cast<PbhGetApiFn>(
        reinterpret_cast<void *>(GetProcAddress(g_apiModule, PBHP_GET_API_SYMBOL)));
    if (!getApi)
    {
        g_apiError = "pb_human_parser_cuda.dll has no " PBHP_GET_API_SYMBOL
                     " export";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    const PbhApi *api = getApi();
    if (!api || api->abi_version != PBHP_ABI_VERSION
        || api->struct_size < sizeof(PbhApi))
    {
        g_apiError = "human parser ABI version mismatch";
        if (detail)
            *detail = QString::fromStdString(g_apiError);
        return false;
    }
    g_api = api;
    return true;
}

QImage grayFromBuffer(const std::vector<uint8_t> &buffer, int width, int height)
{
    if (width <= 0 || height <= 0
        || buffer.size() != size_t(width) * size_t(height))
        return {};
    return QImage(buffer.data(), width, height, width,
                  QImage::Format_Grayscale8)
        .copy();
}

} // namespace

bool Runtime::dllPath(QString *path, QString *detail)
{
    QString dir;
    const QByteArray env = qgetenv("PB_HUMAN_PARSER_DIR");
    if (!env.isEmpty())
        dir = QString::fromUtf8(env);
    if (dir.isEmpty())
    {
        const QString exe = exeDir();
        if (!exe.isEmpty())
            dir = QDir(exe).filePath(QStringLiteral("human-parser"));
    }
    if (path && !dir.isEmpty())
        *path = QDir(dir).filePath(QStringLiteral("pb_human_parser_cuda.dll"));
    if (detail && !dir.isEmpty())
    {
        const QStringList missing = missingClosure(dir);
        if (!missing.isEmpty())
            *detail = QStringLiteral("missing from %1: %2")
                          .arg(dir, missing.join(QStringLiteral(", ")));
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

    PbhCapabilities raw{};
    raw.struct_size = sizeof(raw);
    if (g_api->probe_capabilities(&raw) != PBHP_OK)
    {
        caps.detail = QString::fromUtf8(raw.error);
        return caps;
    }
    caps.cudaAvailable = raw.cuda_provider_available != 0
                         && raw.cuda_device_count > 0;
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
        m_loadError = QStringLiteral("human parser model not found: %1")
                          .arg(config.modelPath);
        if (error)
            *error = m_loadError;
        return false;
    }

    PbhSessionConfig raw{};
    raw.struct_size = sizeof(raw);
    const QByteArray model = config.modelPath.toUtf8();
    const QByteArray manifest = config.manifestPath.toUtf8();
    const QByteArray profile = config.profileDir.toUtf8();
    raw.model_path = model.constData();
    raw.manifest_path = manifest.isEmpty() ? nullptr : manifest.constData();
    raw.profile_dir = profile.isEmpty() ? nullptr : profile.constData();
    raw.device = config.device;
    raw.intra_threads = config.intraThreads;
    raw.require_gpu_operators = config.requireGpuOperators ? 1 : 0;
    raw.verify_model_hash = config.verifyModelHash ? 1 : 0;

    PbhSession *session = nullptr;
    char err[512] = {};
    const int32_t status = g_api->create_session(&raw, &session, err, sizeof(err));
    if (status != PBHP_OK || !session)
    {
        m_loadError = QStringLiteral("human parser session failed: %1")
                          .arg(QString::fromUtf8(err));
        if (error)
            *error = m_loadError;
        return false;
    }

    m_session = session;
    m_device = config.device;
    m_modelPath = config.modelPath;
    m_exportClassProbabilities = config.exportClassProbabilities;

    PbhBackendInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(session, &info) == PBHP_OK)
        m_backend = QString::fromUtf8(info.backend);
    else
        m_backend = QStringLiteral("unknown backend");
    return true;
}

void Runtime::unload()
{
    if (m_session && g_api)
        g_api->destroy_session(static_cast<PbhSession *>(m_session));
    m_session = nullptr;
    m_backend.clear();
    m_device = 0;
}

RunInfo Runtime::backendInfo() const
{
    RunInfo out;
    if (!m_session || !g_api)
        return out;
    PbhBackendInfo info{};
    info.struct_size = sizeof(info);
    if (g_api->get_backend_info(static_cast<PbhSession *>(m_session), &info)
        != PBHP_OK)
        return out;
    out.inputWidth = info.input_width;
    out.inputHeight = info.input_height;
    out.gridWidth = info.grid_width;
    out.gridHeight = info.grid_height;
    out.usedGpu = info.used_gpu != 0;
    out.cudaNodes = info.cuda_node_count;
    out.cpuNodes = info.cpu_node_count;
    out.backend = QString::fromUtf8(info.backend);
    out.modelSha256 = QString::fromUtf8(info.model_sha256);
    return out;
}

bool Runtime::parse(const QImage &rgb, Masks *masks, RunInfo *info,
                    QString *error)
{
    if (masks)
        masks->clear();
    if (!m_session || !g_api)
    {
        if (error)
            *error = QStringLiteral("human parser runtime is not loaded");
        return false;
    }
    if (rgb.isNull())
    {
        if (error)
            *error = QStringLiteral("human parser input is empty");
        return false;
    }
    if (!masks)
    {
        if (error)
            *error = QStringLiteral("human parser output is null");
        return false;
    }

    const QImage rgb888 = rgb.convertToFormat(QImage::Format_RGB888);
    if (rgb888.isNull())
    {
        if (error)
            *error = QStringLiteral("human parser RGB conversion failed");
        return false;
    }

    const int width = rgb888.width();
    const int height = rgb888.height();
    const size_t count = size_t(width) * size_t(height);
    std::vector<uint8_t> classMap(count, 0);
    std::vector<uint8_t> garment(count, 0);
    std::vector<uint8_t> hair(count, 0);
    std::vector<uint8_t> skin(count, 0);
    std::vector<uint8_t> accessory(count, 0);
    std::vector<uint8_t> classProbabilities;
    if (m_exportClassProbabilities)
        classProbabilities.assign(count * PBHP_CLASS_COUNT, 0);

    PbhParseOutputs outputs{};
    outputs.struct_size = sizeof(outputs);
    outputs.class_map = classMap.data();
    outputs.class_map_capacity = int64_t(count);
    outputs.garment = garment.data();
    outputs.garment_capacity = int64_t(count);
    outputs.hair = hair.data();
    outputs.hair_capacity = int64_t(count);
    outputs.skin = skin.data();
    outputs.skin_capacity = int64_t(count);
    outputs.accessory = accessory.data();
    outputs.accessory_capacity = int64_t(count);
    if (m_exportClassProbabilities)
    {
        outputs.class_probabilities = classProbabilities.data();
        outputs.class_probabilities_capacity = int64_t(count * PBHP_CLASS_COUNT);
    }

    PbhImage image{};
    image.data = rgb888.constBits();
    image.width = width;
    image.height = height;
    image.stride = int(rgb888.bytesPerLine());

    PbhParseInfo rawInfo{};
    rawInfo.struct_size = sizeof(rawInfo);
    char err[512] = {};
    const int32_t status = g_api->parse(
        static_cast<PbhSession *>(m_session), &image, &outputs, &rawInfo,
        err, sizeof(err));
    if (status != PBHP_OK)
    {
        if (error)
            *error = QString::fromUtf8(err[0] ? err : "human parser inference failed");
        return false;
    }

    masks->classMap = grayFromBuffer(classMap, width, height);
    masks->garment = grayFromBuffer(garment, width, height);
    masks->hair = grayFromBuffer(hair, width, height);
    masks->skin = grayFromBuffer(skin, width, height);
    masks->accessory = grayFromBuffer(accessory, width, height);
    if (masks->classMap.isNull())
    {
        if (error)
            *error = QStringLiteral("human parser produced an invalid class map");
        masks->clear();
        return false;
    }

    if (m_exportClassProbabilities)
    {
        masks->classProbabilities.reserve(PBHP_CLASS_COUNT);
        for (int c = 0; c < PBHP_CLASS_COUNT; ++c)
        {
            const uint8_t *plane = classProbabilities.data() + size_t(c) * count;
            masks->classProbabilities.push_back(
                QImage(plane, width, height, width, QImage::Format_Grayscale8)
                    .copy());
        }
    }

    masks->classPixels.resize(PBHP_CLASS_COUNT);
    masks->classPixelsGrid.resize(PBHP_CLASS_COUNT);
    for (int c = 0; c < PBHP_CLASS_COUNT; ++c)
    {
        masks->classPixels[c] = rawInfo.class_pixels[c];
        masks->classPixelsGrid[c] = rawInfo.class_pixels_grid[c];
    }
    masks->groupPixels.resize(PBHP_GROUP_COUNT);
    masks->groupProbabilitySum.resize(PBHP_GROUP_COUNT);
    for (int g = 0; g < PBHP_GROUP_COUNT; ++g)
    {
        masks->groupPixels[g] = rawInfo.group_pixels[g];
        masks->groupProbabilitySum[g] = rawInfo.group_probability_sum[g];
    }

    if (info)
    {
        info->preprocessMs = rawInfo.preprocess_ms;
        info->inferenceMs = rawInfo.inference_ms;
        info->postprocessMs = rawInfo.postprocess_ms;
        info->totalMs = rawInfo.total_ms;
        info->inputWidth = rawInfo.input_width;
        info->inputHeight = rawInfo.input_height;
        info->gridWidth = rawInfo.grid_width;
        info->gridHeight = rawInfo.grid_height;
        info->sourceWidth = rawInfo.source_width;
        info->sourceHeight = rawInfo.source_height;
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
        g_api->cancel_current(static_cast<PbhSession *>(m_session));
}

Runtime &sharedRuntime()
{
    static Runtime runtime;
    return runtime;
}

} // namespace human_parser
} // namespace paintbrush
