/*****************************************************************************
 * human_parser_runtime.cpp : MSVC/ONNX Runtime FASHN human parser
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Production rules enforced here:
 *
 *   - one persistent Ort::Session per parser session (the 262 MB SegFormer-B4
 *     graph is never reloaded per frame);
 *   - the CUDA execution provider is requested explicitly for GPU modes and
 *     the CPU provider is a reported fallback, never a silent substitution;
 *   - the graph contract is validated at load time:
 *       pixel_values float32 [1,3,576,384] -> logits float32 [1,18,144,96];
 *   - preprocessing matches the exporter exactly: RGB, cv2.INTER_AREA when
 *     both axes downscale (OpenCV emulates bilinear when an axis zooms in),
 *     ImageNet mean/std, NCHW;
 *   - logits are bilinearly upsampled to the source size with
 *     align_corners=false, then softmaxed; the argmax class map and the
 *     garment/hair/skin/accessory probability planes are written directly;
 *   - every failure returns a specific status code and a readable message;
 *   - cancel_current() terminates a run in flight through ORT's own
 *     RunOptionsSetTerminate and also aborts postprocessing between row
 *     batches (no unsafe thread killing).
 *
 * The subject alpha is deliberately absent from this layer. The parser only
 * produces semantic probabilities; the service composes them with the SAM3
 * alpha through the canonical CoverageMode contract.
 *****************************************************************************/

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "human_parser_cuda_api.h"
#include "sha256.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/* --------------------------------------------------------------- constants */

constexpr int kInputWidth = 384;
constexpr int kInputHeight = 576;
constexpr int kInputChannels = 3;
constexpr int kGridWidth = 96;
constexpr int kGridHeight = 144;
constexpr int kClassCount = PBHP_CLASS_COUNT;
constexpr size_t kMaxSourcePixels = 268435456u; /* 16384 x 16384 */

const float kMean[kInputChannels] = {0.485f, 0.456f, 0.406f};
const float kStd[kInputChannels] = {0.229f, 0.224f, 0.225f};

/* Every class belongs to exactly one material-facing group. */
const int kClassGroup[kClassCount] = {
    PBHP_GROUP_BACKGROUND, /* background */
    PBHP_GROUP_SKIN,       /* face       */
    PBHP_GROUP_HAIR,       /* hair       */
    PBHP_GROUP_GARMENT,    /* top        */
    PBHP_GROUP_GARMENT,    /* dress      */
    PBHP_GROUP_GARMENT,    /* skirt      */
    PBHP_GROUP_GARMENT,    /* pants      */
    PBHP_GROUP_ACCESSORY,  /* belt       */
    PBHP_GROUP_ACCESSORY,  /* bag        */
    PBHP_GROUP_ACCESSORY,  /* hat        */
    PBHP_GROUP_ACCESSORY,  /* scarf      */
    PBHP_GROUP_ACCESSORY,  /* glasses    */
    PBHP_GROUP_SKIN,       /* arms       */
    PBHP_GROUP_SKIN,       /* hands      */
    PBHP_GROUP_SKIN,       /* legs       */
    PBHP_GROUP_SKIN,       /* feet       */
    PBHP_GROUP_SKIN,       /* torso      */
    PBHP_GROUP_ACCESSORY   /* jewelry    */
};

const char *const kClassName[kClassCount] = {
    "background", "face", "hair", "top", "dress", "skirt", "pants",
    "belt", "bag", "hat", "scarf", "glasses", "arms", "hands", "legs",
    "feet", "torso", "jewelry"
};

/* ------------------------------------------------------------------- util */

void set_error(char *err, int32_t err_len, const std::string &text)
{
    if (!err || err_len <= 0)
        return;
    const size_t n = std::min<size_t>(text.size(), size_t(err_len) - 1);
    std::memcpy(err, text.data(), n);
    err[n] = '\0';
}

std::wstring widen(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 1)
        return {};
    std::wstring out(size_t(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), needed);
    return out;
}

bool file_exists(const std::string &path)
{
    const DWORD attrs = GetFileAttributesW(widen(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string read_text(const std::string &path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double ms_since(std::chrono::steady_clock::time_point start)
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

uint8_t quantize_probability(float value)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    const int v = int(std::lround(value * 255.0f));
    return uint8_t(std::max(0, std::min(255, v)));
}

/* --------------------------------------------------- manifest (FASHN v1 JSON)
 * Only the safety-critical fields are parsed: the ONNX file name, byte size
 * and SHA-256. The manifest is written by scripts/pb-fashn-export.py. A
 * structural mismatch is a hard error when verification was requested. */

struct ModelManifest
{
    std::string file;
    std::string sha256;
    long long bytes = -1;
    bool found = false;
};

size_t find_key(const std::string &text, const std::string &key, size_t from)
{
    return text.find("\"" + key + "\"", from);
}

std::string read_string_value(const std::string &text, size_t keyPos)
{
    if (keyPos == std::string::npos)
        return {};
    size_t colon = text.find(':', keyPos);
    if (colon == std::string::npos)
        return {};
    size_t begin = text.find('"', colon);
    if (begin == std::string::npos)
        return {};
    size_t end = text.find('"', begin + 1);
    if (end == std::string::npos)
        return {};
    return text.substr(begin + 1, end - begin - 1);
}

long long read_int_value(const std::string &text, size_t keyPos)
{
    if (keyPos == std::string::npos)
        return -1;
    size_t colon = text.find(':', keyPos);
    if (colon == std::string::npos)
        return -1;
    size_t p = colon + 1;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t'
                               || text[p] == '\n' || text[p] == '\r'))
        ++p;
    long long value = 0;
    bool any = false;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9')
    {
        value = value * 10 + (text[p] - '0');
        any = true;
        ++p;
    }
    return any ? value : -1;
}

ModelManifest find_manifest_onnx(const std::string &manifest)
{
    ModelManifest entry;
    const size_t onnxKey = find_key(manifest, "onnx", 0);
    if (onnxKey == std::string::npos)
        return entry;
    const size_t brace = manifest.find('{', onnxKey);
    if (brace == std::string::npos)
        return entry;
    const size_t end = manifest.find('}', brace);
    if (end == std::string::npos)
        return entry;
    const std::string object = manifest.substr(brace, end - brace + 1);
    entry.file = read_string_value(object, find_key(object, "file", 0));
    entry.sha256 = read_string_value(object, find_key(object, "sha256", 0));
    entry.bytes = read_int_value(object, find_key(object, "bytes", 0));
    entry.found = !entry.file.empty() && entry.sha256.size() == 64;
    return entry;
}

/* --------------------------------------------------------------- CUDA probe */

struct CudaProbe
{
    int deviceCount = 0;
    std::string deviceName;
    std::string error;
};

CudaProbe probe_cuda_driver()
{
    CudaProbe probe;
    HMODULE driver = LoadLibraryW(L"nvcuda.dll");
    if (!driver)
    {
        probe.error = "nvcuda.dll is not available";
        return probe;
    }
    using CuInit = int(__stdcall *)(unsigned int);
    using CuDeviceGetCount = int(__stdcall *)(int *);
    using CuDeviceGet = int(__stdcall *)(int *, int);
    using CuDeviceGetName = int(__stdcall *)(char *, int, int);
    auto cuInit = reinterpret_cast<CuInit>(GetProcAddress(driver, "cuInit"));
    auto cuCount = reinterpret_cast<CuDeviceGetCount>(GetProcAddress(driver, "cuDeviceGetCount"));
    auto cuGet = reinterpret_cast<CuDeviceGet>(GetProcAddress(driver, "cuDeviceGet"));
    auto cuName = reinterpret_cast<CuDeviceGetName>(GetProcAddress(driver, "cuDeviceGetName"));
    if (!cuInit || !cuCount || !cuGet || !cuName)
    {
        probe.error = "CUDA driver entry points are missing";
        return probe;
    }
    if (cuInit(0) != 0)
    {
        probe.error = "cuInit failed";
        return probe;
    }
    if (cuCount(&probe.deviceCount) != 0 || probe.deviceCount <= 0)
    {
        probe.error = "no CUDA device";
        probe.deviceCount = 0;
        return probe;
    }
    int device = 0;
    if (cuGet(&device, 0) != 0)
    {
        probe.error = "cuDeviceGet failed";
        return probe;
    }
    char name[160] = {};
    if (cuName(name, int(sizeof(name)), device) == 0)
        probe.deviceName = name;
    return probe;
}

/* ------------------------------------------------------------- Ort env/session */

struct RuntimeEnv
{
    std::mutex mutex;
    std::unique_ptr<Ort::Env> env;

    Ort::Env &get()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!env)
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                             "nai-paintbrush-human-parser");
        return *env;
    }
};

RuntimeEnv g_env;

struct PbhpSessionImpl
{
    Ort::Session session{nullptr};
    Ort::MemoryInfo hostMemory{nullptr};

    std::string backend = "uninitialised";
    std::string error;
    std::string modelPath;
    std::string modelSha = "unverified";
    std::string inputName = "pixel_values";
    std::string outputName = "logits";
    std::string providerAssignment;

    int deviceIndex = 0;
    bool useGpu = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    double loadMs = 0.0;

    std::mutex runMutex; /* one inference at a time */
    std::mutex cancelMutex;
    Ort::RunOptions *activeRun = nullptr;
    std::atomic<bool> cancelRequested{false};

    std::vector<float> hostInput; /* 3*576*384, reused across runs */
};

const char *status_text(int32_t status)
{
    switch (status)
    {
    case PBHP_OK: return "ok";
    case PBHP_ERR_ARGUMENT: return "invalid argument";
    case PBHP_ERR_DLL: return "runtime dependency unavailable";
    case PBHP_ERR_MODEL: return "model or manifest invalid";
    case PBHP_ERR_DEVICE: return "device backend unavailable";
    case PBHP_ERR_INFERENCE: return "inference failed";
    case PBHP_ERR_CANCELLED: return "cancelled";
    case PBHP_ERR_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

bool shape_matches(const std::vector<int64_t> &shape,
                   std::initializer_list<int64_t> expected, bool allowDynamic)
{
    if (shape.size() != expected.size())
        return false;
    size_t i = 0;
    for (int64_t want : expected)
    {
        const int64_t got = shape[i++];
        if (got < 0)
        {
            if (!allowDynamic)
                return false;
            continue;
        }
        if (got != want)
            return false;
    }
    return true;
}

void count_provider_assignment(PbhpSessionImpl &session)
{
    session.cudaNodes = 0;
    session.cpuNodes = 0;
    session.providerAssignment.clear();
    try
    {
        const auto subgraphs = session.session.GetEpGraphAssignmentInfo();
        for (const auto &subgraph : subgraphs)
        {
            const std::string ep = subgraph.GetEpName();
            const size_t nodes = subgraph.GetNodes().size();
            if (ep.find("CUDA") != std::string::npos)
                session.cudaNodes += int(nodes);
            else
                session.cpuNodes += int(nodes);
            if (!session.providerAssignment.empty())
                session.providerAssignment += ", ";
            session.providerAssignment += ep + ":" + std::to_string(nodes);
        }
    }
    catch (const std::exception &e)
    {
        session.providerAssignment = std::string("assignment query failed: ") + e.what();
    }
}

void fill_backend_info(PbhBackendInfo *out, const PbhpSessionImpl &session)
{
    if (!out)
        return;
    out->used_gpu = session.useGpu ? 1 : 0;
    out->cuda_node_count = session.cudaNodes;
    out->cpu_node_count = session.cpuNodes;
    out->device_index = session.deviceIndex;
    out->input_width = kInputWidth;
    out->input_height = kInputHeight;
    out->grid_width = kGridWidth;
    out->grid_height = kGridHeight;
    std::snprintf(out->backend, sizeof(out->backend), "%s", session.backend.c_str());
    std::snprintf(out->model_sha256, sizeof(out->model_sha256), "%s",
                  session.modelSha.c_str());
}

/* --------------------------------------------------------------- resampling */

struct Tap
{
    int index;
    float weight;
};
using AxisTaps = std::vector<std::vector<Tap>>;

/* Exact port of OpenCV 4.x computeResizeAreaTab (true area path used when both
 * axes downscale). The source interval for output pixel d is
 * [d*scale, (d+1)*scale); the final boundary alpha is capped at one source
 * pixel, exactly as OpenCV does. Verified pixel-identical to
 * cv2.resize(..., INTER_AREA) on the FASHN example (2324x1152 -> 384x576). */
AxisTaps area_taps(int src, int dst)
{
    AxisTaps taps(static_cast<size_t>(dst));
    const double scale = double(src) / double(dst);
    for (int d = 0; d < dst; ++d)
    {
        const double fsx1 = double(d) * scale;
        const double fsx2 = fsx1 + scale;
        const double cellWidth = std::min(scale, double(src) - fsx1);
        int sx1 = int(std::ceil(fsx1));
        int sx2 = int(std::floor(fsx2));
        sx2 = std::min(sx2, src - 1);
        sx1 = std::min(sx1, sx2);

        std::vector<Tap> &list = taps[size_t(d)];
        if (sx1 - fsx1 > 1e-3)
            list.push_back({sx1 - 1, float((sx1 - fsx1) / cellWidth)});
        for (int sx = sx1; sx < sx2; ++sx)
            list.push_back({sx, float(1.0 / cellWidth)});
        if (fsx2 - sx2 > 1e-3)
            list.push_back({sx2,
                            float(std::min(std::min(fsx2 - sx2, 1.0), cellWidth)
                                  / cellWidth)});
    }
    return taps;
}

/* Standard half-pixel-centre bilinear taps (align_corners=false). Used for the
 * logits upsample, not for preprocessing. */
AxisTaps bilinear_taps(int src, int dst)
{
    AxisTaps taps(static_cast<size_t>(dst));
    const double scale = double(src) / double(dst);
    for (int d = 0; d < dst; ++d)
    {
        double s = (double(d) + 0.5) * scale - 0.5;
        s = std::max(0.0, std::min(double(src) - 1.0, s));
        const int i0 = int(std::floor(s));
        const int i1 = std::min(src - 1, i0 + 1);
        const double f = s - double(i0);
        if (i0 == i1 || f <= 0.0)
            taps[size_t(d)].push_back({i0, 1.0f});
        else
        {
            taps[size_t(d)].push_back({i0, float(1.0 - f)});
            taps[size_t(d)].push_back({i1, float(f)});
        }
    }
    return taps;
}

/* OpenCV's INTER_AREA fallback when either axis zooms in. It is NOT the same
 * as INTER_LINEAR: area_mode uses sx=floor(d*scale) and the fractional part of
 * the destination right edge, then quantizes both weights to 11-bit fixed
 * point for 8-bit images. Reproducing that precisely keeps the native service
 * aligned with the exporter's Python/OpenCV preprocessing even for a mixed
 * downscale/upscale source such as the 632x316 car cutout. */
struct FixedTap
{
    int index;
    int weight;
};
using FixedAxisTaps = std::vector<std::vector<FixedTap>>;

int fixed_weight(double weight)
{
    const double scaled = std::nearbyint(weight * 2048.0);
    return int(std::max(-32768.0, std::min(32767.0, scaled)));
}

FixedAxisTaps linear_area_fallback_taps(int src, int dst)
{
    FixedAxisTaps taps(static_cast<size_t>(dst));
    const double scale = double(src) / double(dst);
    const double invScale = double(dst) / double(src);
    for (int d = 0; d < dst; ++d)
    {
        int sx = int(std::floor(double(d) * scale));
        float fx = float((double(d + 1) - double(sx + 1) * invScale));
        fx = fx <= 0.0f ? 0.0f : fx - std::floor(fx);
        if (sx < 0)
        {
            sx = 0;
            fx = 0.0f;
        }
        if (sx >= src - 1)
        {
            sx = src - 1;
            fx = 0.0f;
        }
        const int w0 = fixed_weight(1.0 - double(fx));
        const int w1 = fixed_weight(double(fx));
        taps[size_t(d)].push_back({sx, w0});
        if (sx + 1 < src && w1 != 0)
            taps[size_t(d)].push_back({sx + 1, w1});
    }
    return taps;
}

void resize_area_exact(const PbhImage &src, std::vector<uint8_t> &dst)
{
    const AxisTaps xt = area_taps(src.width, kInputWidth);
    const AxisTaps yt = area_taps(src.height, kInputHeight);

    /* Separable float accumulation, mirroring OpenCV's ResizeArea_Invoker. */
    std::vector<float> horizontal(size_t(src.height) * size_t(kInputWidth) * 3u,
                                  0.0f);
    for (int y = 0; y < src.height; ++y)
    {
        const uint8_t *row = src.data + size_t(y) * size_t(src.stride);
        for (int dx = 0; dx < kInputWidth; ++dx)
        {
            float accum[3] = {0.0f, 0.0f, 0.0f};
            for (const Tap &tx : xt[size_t(dx)])
            {
                const uint8_t *p = row + size_t(tx.index) * 3u;
                accum[0] += float(p[0]) * tx.weight;
                accum[1] += float(p[1]) * tx.weight;
                accum[2] += float(p[2]) * tx.weight;
            }
            float *out = horizontal.data()
                         + (size_t(y) * size_t(kInputWidth) + size_t(dx)) * 3u;
            out[0] = accum[0];
            out[1] = accum[1];
            out[2] = accum[2];
        }
    }

    for (int dy = 0; dy < kInputHeight; ++dy)
    {
        for (int dx = 0; dx < kInputWidth; ++dx)
        {
            float accum[3] = {0.0f, 0.0f, 0.0f};
            for (const Tap &ty : yt[size_t(dy)])
            {
                const float *row = horizontal.data()
                                   + (size_t(ty.index) * size_t(kInputWidth)
                                      + size_t(dx)) * 3u;
                accum[0] += row[0] * ty.weight;
                accum[1] += row[1] * ty.weight;
                accum[2] += row[2] * ty.weight;
            }
            uint8_t *out = dst.data()
                           + (size_t(dy) * size_t(kInputWidth) + size_t(dx)) * 3u;
            for (int c = 0; c < 3; ++c)
            {
                const long v = std::lround(std::nearbyint(accum[c]));
                out[c] = uint8_t(std::max(0L, std::min(255L, v)));
            }
        }
    }
}

void resize_area_linear_fallback(const PbhImage &src, std::vector<uint8_t> &dst)
{
    const FixedAxisTaps xt = linear_area_fallback_taps(src.width, kInputWidth);
    const FixedAxisTaps yt = linear_area_fallback_taps(src.height, kInputHeight);

    std::vector<int> horizontal(size_t(src.height) * size_t(kInputWidth) * 3u, 0);
    for (int y = 0; y < src.height; ++y)
    {
        const uint8_t *row = src.data + size_t(y) * size_t(src.stride);
        for (int dx = 0; dx < kInputWidth; ++dx)
        {
            int accum[3] = {0, 0, 0};
            for (const FixedTap &tx : xt[size_t(dx)])
            {
                const uint8_t *p = row + size_t(tx.index) * 3u;
                accum[0] += int(p[0]) * tx.weight;
                accum[1] += int(p[1]) * tx.weight;
                accum[2] += int(p[2]) * tx.weight;
            }
            int *out = horizontal.data()
                       + (size_t(y) * size_t(kInputWidth) + size_t(dx)) * 3u;
            out[0] = accum[0];
            out[1] = accum[1];
            out[2] = accum[2];
        }
    }

    for (int dy = 0; dy < kInputHeight; ++dy)
    {
        for (int dx = 0; dx < kInputWidth; ++dx)
        {
            int accum[3] = {0, 0, 0};
            for (const FixedTap &ty : yt[size_t(dy)])
            {
                const int *row = horizontal.data()
                                 + (size_t(ty.index) * size_t(kInputWidth)
                                    + size_t(dx)) * 3u;
                accum[0] += (ty.weight * (row[0] >> 4)) >> 16;
                accum[1] += (ty.weight * (row[1] >> 4)) >> 16;
                accum[2] += (ty.weight * (row[2] >> 4)) >> 16;
            }
            uint8_t *out = dst.data()
                           + (size_t(dy) * size_t(kInputWidth) + size_t(dx)) * 3u;
            for (int c = 0; c < 3; ++c)
            {
                const int v = (accum[c] + 2) >> 2;
                out[c] = uint8_t(std::max(0, std::min(255, v)));
            }
        }
    }
}

void resize_rgb_to_model(const PbhImage &src, std::vector<uint8_t> &dst)
{
    dst.assign(size_t(kInputWidth) * size_t(kInputHeight) * 3u, 0);
    if (src.width >= kInputWidth && src.height >= kInputHeight)
        resize_area_exact(src, dst);
    else
        resize_area_linear_fallback(src, dst);
}

void fill_input_tensor(const std::vector<uint8_t> &rgb, std::vector<float> &tensor)
{
    const size_t plane = size_t(kInputWidth) * size_t(kInputHeight);
    tensor.resize(plane * 3u);
    for (size_t i = 0; i < plane; ++i)
    {
        for (int c = 0; c < 3; ++c)
            tensor[c * plane + i] = (float(rgb[i * 3u + size_t(c)]) / 255.0f - kMean[c])
                                    / kStd[c];
    }
}

/* ------------------------------------------------------------- postprocess */

struct RowStats
{
    uint64_t classPixels[kClassCount] = {};
    uint64_t groupPixels[PBHP_GROUP_COUNT] = {};
    uint64_t groupProbabilitySum[PBHP_GROUP_COUNT] = {};
};

int32_t postprocess(const std::vector<float> &logits,
                    int sourceWidth, int sourceHeight,
                    const PbhParseOutputs &outputs,
                    PbhParseInfo &info,
                    std::atomic<bool> &cancelRequested,
                    std::string *error)
{
    const auto start = std::chrono::steady_clock::now();
    const size_t sourceCount = size_t(sourceWidth) * size_t(sourceHeight);
    const size_t plane = size_t(kGridWidth) * size_t(kGridHeight);

    /* Grid argmax counts are exactly comparable to the exporter's validation
     * counts (they are computed before any upsample). */
    for (int gy = 0; gy < kGridHeight; ++gy)
    {
        for (int gx = 0; gx < kGridWidth; ++gx)
        {
            const size_t gi = size_t(gy) * size_t(kGridWidth) + size_t(gx);
            int best = 0;
            float bestLogit = logits[gi];
            for (int c = 1; c < kClassCount; ++c)
            {
                const float v = logits[size_t(c) * plane + gi];
                if (v > bestLogit)
                {
                    bestLogit = v;
                    best = c;
                }
            }
            info.class_pixels_grid[best]++;
        }
    }

    /* Bilinear source taps, align_corners=false. */
    AxisTaps xt = bilinear_taps(kGridWidth, sourceWidth);
    AxisTaps yt = bilinear_taps(kGridHeight, sourceHeight);

    uint8_t *groupOut[PBHP_GROUP_COUNT] = {
        nullptr, outputs.garment, outputs.hair, outputs.skin,
        outputs.accessory, nullptr
    };

    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0)
        hardware = 1;
    int threadCount = int(std::min<unsigned>(hardware, 16u));
    threadCount = std::max(1, std::min(threadCount, sourceHeight));

    std::vector<RowStats> stats(static_cast<size_t>(threadCount));
    std::atomic<bool> aborted{false};

    auto worker = [&](int threadIndex)
    {
        const int begin = sourceHeight * threadIndex / threadCount;
        const int end = sourceHeight * (threadIndex + 1) / threadCount;
        RowStats &local = stats[size_t(threadIndex)];
        for (int y = begin; y < end; ++y)
        {
            if ((y & 7) == 0 && cancelRequested.load())
            {
                aborted.store(true);
                return;
            }
            const std::vector<Tap> &rowTaps = yt[size_t(y)];
            for (int x = 0; x < sourceWidth; ++x)
            {
                const std::vector<Tap> &colTaps = xt[size_t(x)];
                const size_t dstIndex = size_t(y) * size_t(sourceWidth) + size_t(x);

                float probs[kClassCount];
                float maxLogit = -1.0e30f;
                for (int c = 0; c < kClassCount; ++c)
                {
                    const float *srcPlane = logits.data() + size_t(c) * plane;
                    float value = 0.0f;
                    for (const Tap &ty : rowTaps)
                    {
                        const float *row = srcPlane + size_t(ty.index) * size_t(kGridWidth);
                        for (const Tap &tx : colTaps)
                            value += ty.weight * tx.weight * row[size_t(tx.index)];
                    }
                    probs[c] = value;
                    if (value > maxLogit)
                        maxLogit = value;
                }

                float sum = 0.0f;
                for (int c = 0; c < kClassCount; ++c)
                {
                    probs[c] = std::exp(probs[c] - maxLogit);
                    sum += probs[c];
                }
                const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
                for (int c = 0; c < kClassCount; ++c)
                    probs[c] *= inv;

                int best = 0;
                for (int c = 1; c < kClassCount; ++c)
                    if (probs[c] > probs[best])
                        best = c;

                outputs.class_map[dstIndex] = uint8_t(best);
                local.classPixels[best]++;
                local.groupPixels[kClassGroup[best]]++;

                float groupProb[PBHP_GROUP_COUNT] = {};
                for (int c = 0; c < kClassCount; ++c)
                    groupProb[kClassGroup[c]] += probs[c];

                for (int g = 1; g < PBHP_GROUP_COUNT; ++g)
                {
                    const uint8_t q = quantize_probability(groupProb[g]);
                    if (groupOut[g] != nullptr)
                        groupOut[g][dstIndex] = q;
                    local.groupProbabilitySum[g] += q;
                }

                if (outputs.class_probabilities != nullptr)
                {
                    for (int c = 0; c < kClassCount; ++c)
                        outputs.class_probabilities[size_t(c) * sourceCount + dstIndex] =
                            quantize_probability(probs[c]);
                }
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(size_t(threadCount));
    try
    {
        for (int t = 1; t < threadCount; ++t)
            pool.emplace_back(worker, t);
    }
    catch (const std::exception &e)
    {
        for (std::thread &thread : pool)
            if (thread.joinable())
                thread.join();
        *error = std::string("postprocess thread creation failed: ") + e.what();
        return PBHP_ERR_INFERENCE;
    }
    worker(0);
    for (std::thread &thread : pool)
        thread.join();

    if (aborted.load() || cancelRequested.load())
    {
        *error = "cancelled";
        return PBHP_ERR_CANCELLED;
    }

    for (const RowStats &local : stats)
    {
        for (int c = 0; c < kClassCount; ++c)
            info.class_pixels[c] += local.classPixels[c];
        for (int g = 0; g < PBHP_GROUP_COUNT; ++g)
        {
            info.group_pixels[g] += local.groupPixels[g];
            info.group_probability_sum[g] += local.groupProbabilitySum[g];
        }
    }

    info.postprocess_ms = ms_since(start);
    return PBHP_OK;
}

/* ------------------------------------------------------------- API methods */

const char *pbhp_version(void)
{
    return "nai-paintbrush-human-parser/1";
}

int32_t pbhp_probe(PbhCapabilities *out)
{
    if (!out || out->struct_size != sizeof(PbhCapabilities))
        return PBHP_ERR_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(PbhCapabilities);

    try
    {
        const char *version = OrtGetApiBase()->GetVersionString();
        if (version)
            std::snprintf(out->ort_version, sizeof(out->ort_version), "%s", version);
    }
    catch (...)
    {
        std::snprintf(out->error, sizeof(out->error), "ONNX Runtime API unavailable");
        return PBHP_ERR_DLL;
    }
    out->ort_available = 1;

    char **providers = nullptr;
    int count = 0;
    if (Ort::GetApi().GetAvailableProviders(&providers, &count) == nullptr)
    {
        for (int i = 0; i < count; ++i)
        {
            const std::string name = providers[i] ? providers[i] : "";
            if (name.find("CUDA") != std::string::npos)
                out->cuda_provider_available = 1;
            if (name.find("CPU") != std::string::npos)
                out->cpu_fallback_available = 1;
        }
        Ort::GetApi().ReleaseAvailableProviders(providers, count);
    }

    const CudaProbe cuda = probe_cuda_driver();
    out->cuda_device_count = cuda.deviceCount;
    if (!cuda.deviceName.empty())
        std::snprintf(out->device_name, sizeof(out->device_name), "%s",
                      cuda.deviceName.c_str());
    if (!cuda.error.empty() && out->cuda_provider_available)
        std::snprintf(out->error, sizeof(out->error), "%s", cuda.error.c_str());
    return PBHP_OK;
}

int32_t pbhp_create_session(const PbhSessionConfig *config, PbhSession **out_session,
                            char *err, int32_t err_len)
{
    if (!config || !out_session || config->struct_size != sizeof(PbhSessionConfig))
    {
        set_error(err, err_len, "invalid session config");
        return PBHP_ERR_ARGUMENT;
    }
    *out_session = nullptr;
    if (!config->model_path || !*config->model_path)
    {
        set_error(err, err_len, "model path is empty");
        return PBHP_ERR_ARGUMENT;
    }
    if (!file_exists(config->model_path))
    {
        set_error(err, err_len, std::string("model not found: ") + config->model_path);
        return PBHP_ERR_MODEL;
    }

    const auto start = std::chrono::steady_clock::now();
    std::string modelFile = config->model_path;
    std::string modelSha = "unverified";

    if (config->manifest_path && *config->manifest_path)
    {
        const std::string manifest = read_text(config->manifest_path);
        if (manifest.empty())
        {
            set_error(err, err_len, std::string("manifest unreadable: ")
                                        + config->manifest_path);
            return PBHP_ERR_MODEL;
        }
        const ModelManifest entry = find_manifest_onnx(manifest);
        if (!entry.found)
        {
            set_error(err, err_len, "manifest has no usable onnx.file/onnx.sha256");
            return PBHP_ERR_MODEL;
        }

        /* Resolve the manifest-declared file next to the manifest; this keeps
         * a stale copy from masquerading as the pinned artifact. */
        std::string dir(config->manifest_path);
        const size_t slash = dir.find_last_of("\\/");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        const std::string candidate = dir + entry.file;
        if (file_exists(candidate))
            modelFile = candidate;
        else if (entry.file != std::string(config->model_path))
        {
            set_error(err, err_len, "manifest graph file missing: " + candidate);
            return PBHP_ERR_MODEL;
        }

        if (entry.bytes > 0)
        {
            std::ifstream sizeCheck(std::filesystem::u8path(modelFile),
                                    std::ios::binary | std::ios::ate);
            if (!sizeCheck)
            {
                set_error(err, err_len, "cannot open model for size check: " + modelFile);
                return PBHP_ERR_MODEL;
            }
            const long long actualBytes = static_cast<long long>(sizeCheck.tellg());
            if (actualBytes != entry.bytes)
            {
                set_error(err, err_len,
                          "model size mismatch (manifest " + std::to_string(entry.bytes)
                              + ", actual " + std::to_string(actualBytes) + ")");
                return PBHP_ERR_MODEL;
            }
        }

        if (config->verify_model_hash)
        {
            std::string hashError;
            const std::string actual = pbhp::sha256_file(modelFile, &hashError);
            if (actual.empty())
            {
                set_error(err, err_len, "cannot hash model: " + hashError);
                return PBHP_ERR_MODEL;
            }
            if (actual != entry.sha256)
            {
                set_error(err, err_len, "model hash mismatch (manifest " + entry.sha256
                                            + ", actual " + actual + ")");
                return PBHP_ERR_MODEL;
            }
            modelSha = actual;
        }
        else
            modelSha = "unverified";
    }

    auto session = std::make_unique<PbhpSessionImpl>();
    session->modelPath = modelFile;
    session->modelSha = modelSha;
    session->deviceIndex = 0;

    const bool wantGpu = config->device != PBHP_DEVICE_CPU;
    const bool requireGpu = config->device == PBHP_DEVICE_GPU;

    try
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.EnableMemPattern();
        options.EnableCpuMemArena();
        options.AddConfigEntry("session.record_ep_graph_assignment_info", "1");
        if (config->intra_threads > 0)
            options.SetIntraOpNumThreads(config->intra_threads);
        if (config->profile_dir && *config->profile_dir)
        {
            std::string prefix(config->profile_dir);
            prefix += "\\pb_human_parser";
            const std::wstring widePrefix = widen(prefix);
            options.EnableProfiling(widePrefix.c_str());
        }

        bool gpuAppended = false;
        if (wantGpu)
        {
            try
            {
                Ort::CUDAProviderOptions cudaOptions;
                cudaOptions.Update({{"device_id", "0"},
                                    {"arena_extend_strategy", "kSameAsRequested"},
                                    {"cudnn_conv_algo_search", "HEURISTIC"},
                                    {"do_copy_in_default_stream", "1"}});
                options.AppendExecutionProvider_CUDA_V2(*cudaOptions);
                gpuAppended = true;
            }
            catch (const std::exception &e)
            {
                if (requireGpu)
                {
                    set_error(err, err_len,
                              std::string("CUDA execution provider unavailable: ")
                                  + e.what());
                    return PBHP_ERR_DEVICE;
                }
                session->backend = std::string("CPUExecutionProvider (CUDA init failed: ")
                                   + e.what() + ")";
            }
        }

        const std::wstring widePath = widen(modelFile);
        session->session = Ort::Session(g_env.get(), widePath.c_str(), options);
        session->hostMemory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                         OrtMemTypeDefault);

        Ort::AllocatorWithDefaultOptions allocator;
        if (session->session.GetInputCount() != 1
            || session->session.GetOutputCount() != 1)
        {
            set_error(err, err_len, "graph must have exactly one input and one output");
            return PBHP_ERR_MODEL;
        }
        {
            auto name = session->session.GetInputNameAllocated(0, allocator);
            session->inputName = name.get();
            auto outName = session->session.GetOutputNameAllocated(0, allocator);
            session->outputName = outName.get();
        }

        const auto inputShape = session->session.GetInputTypeInfo(0)
                                    .GetTensorTypeAndShapeInfo().GetShape();
        if (!shape_matches(inputShape,
                           {1, kInputChannels, kInputHeight, kInputWidth}, true))
        {
            set_error(err, err_len, "unexpected input shape (expected [1,3,576,384])");
            return PBHP_ERR_MODEL;
        }
        const auto outputShape = session->session.GetOutputTypeInfo(0)
                                     .GetTensorTypeAndShapeInfo().GetShape();
        if (!shape_matches(outputShape,
                           {1, kClassCount, kGridHeight, kGridWidth}, true))
        {
            set_error(err, err_len, "unexpected output shape (expected [1,18,144,96])");
            return PBHP_ERR_MODEL;
        }

        session->useGpu = gpuAppended;
        count_provider_assignment(*session);

        if (gpuAppended)
        {
            const CudaProbe probe = probe_cuda_driver();
            session->backend = "CUDAExecutionProvider";
            if (!probe.deviceName.empty())
                session->backend += " (" + probe.deviceName + ")";
            if (session->cudaNodes <= 0)
            {
                if (requireGpu)
                {
                    set_error(err, err_len,
                              "CUDA provider was registered but no graph node was "
                              "assigned to it (" + session->providerAssignment + ")");
                    return PBHP_ERR_DEVICE;
                }
                session->backend += " [no CUDA nodes; CPU fallback]";
                session->useGpu = false;
            }
        }
        else
        {
            session->backend = "CPUExecutionProvider";
            session->useGpu = false;
        }

        session->hostInput.assign(size_t(kInputChannels) * size_t(kInputWidth)
                                      * size_t(kInputHeight), 0.0f);
        session->loadMs = ms_since(start);
    }
    catch (const Ort::Exception &e)
    {
        set_error(err, err_len, std::string("session creation failed: ") + e.what());
        return PBHP_ERR_DEVICE;
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("session creation failed: ") + e.what());
        return PBHP_ERR_MODEL;
    }

    *out_session = reinterpret_cast<PbhSession *>(session.release());
    return PBHP_OK;
}

void pbhp_destroy_session(PbhSession *session)
{
    delete reinterpret_cast<PbhpSessionImpl *>(session);
}

int32_t pbhp_get_backend_info(PbhSession *session, PbhBackendInfo *out)
{
    if (!session || !out || out->struct_size != sizeof(PbhBackendInfo))
        return PBHP_ERR_ARGUMENT;
    fill_backend_info(out, *reinterpret_cast<PbhpSessionImpl *>(session));
    return PBHP_OK;
}

int32_t pbhp_parse(PbhSession *session, const PbhImage *rgb,
                   const PbhParseOutputs *outputs, PbhParseInfo *info,
                   char *err, int32_t err_len)
{
    if (!session || !rgb || !outputs
        || outputs->struct_size != sizeof(PbhParseOutputs))
    {
        set_error(err, err_len, "invalid parse arguments");
        return PBHP_ERR_ARGUMENT;
    }
    if (info && info->struct_size != sizeof(PbhParseInfo))
    {
        set_error(err, err_len, "invalid parse info struct_size");
        return PBHP_ERR_ARGUMENT;
    }
    if (!rgb->data || rgb->width <= 0 || rgb->height <= 0
        || rgb->stride < rgb->width * 3)
    {
        set_error(err, err_len, "invalid RGB image");
        return PBHP_ERR_ARGUMENT;
    }
    const size_t sourceCount = size_t(rgb->width) * size_t(rgb->height);
    if (sourceCount == 0 || sourceCount > kMaxSourcePixels)
    {
        set_error(err, err_len, "unsupported source size");
        return PBHP_ERR_ARGUMENT;
    }
    if (!outputs->class_map || outputs->class_map_capacity < int64_t(sourceCount))
    {
        set_error(err, err_len, "class_map buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    if (outputs->garment
        && outputs->garment_capacity < int64_t(sourceCount))
    {
        set_error(err, err_len, "garment buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    if (outputs->hair && outputs->hair_capacity < int64_t(sourceCount))
    {
        set_error(err, err_len, "hair buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    if (outputs->skin && outputs->skin_capacity < int64_t(sourceCount))
    {
        set_error(err, err_len, "skin buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    if (outputs->accessory
        && outputs->accessory_capacity < int64_t(sourceCount))
    {
        set_error(err, err_len, "accessory buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    if (outputs->class_probabilities
        && outputs->class_probabilities_capacity < int64_t(sourceCount) * kClassCount)
    {
        set_error(err, err_len, "class_probabilities buffer too small");
        return PBHP_ERR_ARGUMENT;
    }
    const int64_t modelRgbCount = int64_t(kInputWidth) * kInputHeight * 3;
    if (outputs->model_rgb && outputs->model_rgb_capacity < modelRgbCount)
    {
        set_error(err, err_len, "model_rgb buffer too small");
        return PBHP_ERR_ARGUMENT;
    }

    auto *impl = reinterpret_cast<PbhpSessionImpl *>(session);
    std::lock_guard<std::mutex> lock(impl->runMutex);
    impl->cancelRequested.store(false);

    const auto totalStart = std::chrono::steady_clock::now();
    PbhParseInfo localInfo{};
    localInfo.struct_size = sizeof(PbhParseInfo);
    localInfo.input_width = kInputWidth;
    localInfo.input_height = kInputHeight;
    localInfo.grid_width = kGridWidth;
    localInfo.grid_height = kGridHeight;
    localInfo.source_width = rgb->width;
    localInfo.source_height = rgb->height;
    localInfo.used_gpu = impl->useGpu ? 1 : 0;
    localInfo.cuda_node_count = impl->cudaNodes;
    localInfo.cpu_node_count = impl->cpuNodes;
    localInfo.device_index = impl->deviceIndex;
    localInfo.wrote_class_probabilities = outputs->class_probabilities ? 1 : 0;
    std::snprintf(localInfo.backend, sizeof(localInfo.backend), "%s",
                  impl->backend.c_str());
    std::snprintf(localInfo.model_sha256, sizeof(localInfo.model_sha256), "%s",
                  impl->modelSha.c_str());

    const auto preStart = std::chrono::steady_clock::now();
    try
    {
        std::vector<uint8_t> modelRgb;
        resize_rgb_to_model(*rgb, modelRgb);
        if (outputs->model_rgb)
            std::memcpy(outputs->model_rgb, modelRgb.data(), size_t(modelRgbCount));
        fill_input_tensor(modelRgb, impl->hostInput);
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("preprocessing failed: ") + e.what());
        return PBHP_ERR_INFERENCE;
    }
    catch (...)
    {
        set_error(err, err_len, "preprocessing failed: unknown allocation error");
        return PBHP_ERR_INFERENCE;
    }
    localInfo.preprocess_ms = ms_since(preStart);

    std::vector<float> hostOutput;
    int32_t result = PBHP_OK;

    try
    {
        const int64_t inputElements = int64_t(kInputChannels) * kInputWidth * kInputHeight;
        const int64_t outputElements = int64_t(kClassCount) * kGridHeight * kGridWidth;
        const std::array<int64_t, 4> inputShape = {1, kInputChannels,
                                                   kInputHeight, kInputWidth};
        const std::array<int64_t, 4> outputShape = {1, kClassCount,
                                                    kGridHeight, kGridWidth};

        const auto inferStart = std::chrono::steady_clock::now();

        Ort::Value input = Ort::Value::CreateTensor<float>(
            impl->hostMemory, impl->hostInput.data(), size_t(inputElements),
            inputShape.data(), inputShape.size());
        hostOutput.resize(size_t(outputElements));
        Ort::Value output = Ort::Value::CreateTensor<float>(
            impl->hostMemory, hostOutput.data(), size_t(outputElements),
            outputShape.data(), outputShape.size());

        Ort::RunOptions runOptions;
        {
            std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
            impl->activeRun = &runOptions;
        }
        const char *inputNames[] = {impl->inputName.c_str()};
        const char *outputNames[] = {impl->outputName.c_str()};
        impl->session.Run(runOptions, inputNames, &input, 1, outputNames, &output, 1);
        {
            std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
            impl->activeRun = nullptr;
        }

        localInfo.inference_ms = ms_since(inferStart);
    }
    catch (const Ort::Exception &e)
    {
        set_error(err, err_len, std::string("inference failed: ") + e.what());
        result = (std::string(e.what()).find("terminated") != std::string::npos)
                     ? PBHP_ERR_CANCELLED
                     : PBHP_ERR_INFERENCE;
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("inference failed: ") + e.what());
        result = PBHP_ERR_INFERENCE;
    }

    {
        std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
        impl->activeRun = nullptr;
    }
    if (result != PBHP_OK)
        return result;
    if (impl->cancelRequested.load())
    {
        set_error(err, err_len, "cancelled");
        return PBHP_ERR_CANCELLED;
    }
    if (hostOutput.size()
        != size_t(kClassCount) * size_t(kGridHeight) * size_t(kGridWidth))
    {
        set_error(err, err_len, "unexpected output tensor shape");
        return PBHP_ERR_INFERENCE;
    }

    std::string postError;
    int32_t post = PBHP_OK;
    try
    {
        post = postprocess(hostOutput, rgb->width, rgb->height, *outputs,
                           localInfo, impl->cancelRequested, &postError);
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("postprocessing failed: ") + e.what());
        return PBHP_ERR_INFERENCE;
    }
    catch (...)
    {
        set_error(err, err_len, "postprocessing failed: unknown allocation error");
        return PBHP_ERR_INFERENCE;
    }
    if (post != PBHP_OK)
    {
        set_error(err, err_len, postError.empty() ? status_text(post) : postError);
        return post;
    }

    localInfo.total_ms = ms_since(totalStart);
    if (info)
    {
        const uint32_t requestedSize = info->struct_size;
        *info = localInfo;
        info->struct_size = requestedSize ? requestedSize : sizeof(PbhParseInfo);
    }
    return PBHP_OK;
}

void pbhp_cancel_current(PbhSession *session)
{
    if (!session)
        return;
    auto *impl = reinterpret_cast<PbhpSessionImpl *>(session);
    impl->cancelRequested.store(true);
    std::lock_guard<std::mutex> lock(impl->cancelMutex);
    if (impl->activeRun)
    {
        try
        {
            impl->activeRun->SetTerminate();
        }
        catch (...)
        {
        }
    }
}

const char *pbhp_last_error(PbhSession *session)
{
    if (!session)
        return "";
    return reinterpret_cast<PbhpSessionImpl *>(session)->error.c_str();
}

int32_t pbhp_class_group(int32_t classIndex)
{
    if (classIndex < 0 || classIndex >= kClassCount)
        return PBHP_GROUP_NONE;
    return kClassGroup[classIndex];
}

const char *pbhp_class_name(int32_t classIndex)
{
    if (classIndex < 0 || classIndex >= kClassCount)
        return "";
    return kClassName[classIndex];
}

const PbhApi g_api = {
    PBHP_ABI_VERSION,
    sizeof(PbhApi),
    pbhp_version,
    pbhp_probe,
    pbhp_create_session,
    pbhp_destroy_session,
    pbhp_get_backend_info,
    pbhp_parse,
    pbhp_cancel_current,
    pbhp_last_error,
    pbhp_class_group,
    pbhp_class_name,
};

} // namespace

extern "C" const PbhApi *pbhp_get_api(void)
{
    return &g_api;
}
