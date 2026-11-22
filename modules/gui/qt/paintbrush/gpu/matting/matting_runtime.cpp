/*****************************************************************************
 * matting_runtime.cpp : MSVC/ONNX Runtime implementation of pb_matting_cuda
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Production rules enforced here:
 *
 *   - one persistent Ort::Session per matting session (the model is never
 *     reloaded per frame);
 *   - the CUDA execution provider is requested explicitly, the CPU provider is
 *     never used as a silent substitute in GPU mode;
 *   - the graph is a FIXED square (the exported manifest pins the size), so
 *     dynamic-shape operator fallbacks cannot happen;
 *   - I/O binding keeps the input and output tensors in device memory when the
 *     session allocator supports it; the measured host copy is explicit;
 *   - every failure returns a specific status code and a readable message;
 *   - cancel_current() terminates a run in flight through ORT's own
 *     RunOptionsSetTerminate (no unsafe thread killing).
 *****************************************************************************/

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "matting_cuda_api.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* ------------------------------------------------------------------ util */

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
    std::ifstream in(path, std::ios::binary);
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

/* --------------------------------------------------------- SHA-256 (compact)
 * Used to verify the ONNX graph against the manifest before it is loaded.
 * This is not a security boundary (the file is local), it is an integrity and
 * provenance check so a stale or partial copy cannot masquerade as the pinned
 * model. */
struct Sha256
{
    uint32_t h[8];
    uint64_t total = 0;
    uint8_t block[64];
    size_t used = 0;

    Sha256()
    {
        static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                         0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                         0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h, init, sizeof(h));
    }

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void transform(const uint8_t *p)
    {
        static const uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16)
                   | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t *data, size_t len)
    {
        total += len;
        while (len > 0)
        {
            const size_t take = std::min<size_t>(64 - used, len);
            std::memcpy(block + used, data, take);
            used += take;
            data += take;
            len -= take;
            if (used == 64)
            {
                transform(block);
                used = 0;
            }
        }
    }

    std::string hex()
    {
        const uint64_t bits = total * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (used != 56)
            update(&zero, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = uint8_t((bits >> (56 - i * 8)) & 0xff);
        update(lenBytes, 8);
        char out[65];
        for (int i = 0; i < 8; ++i)
            std::snprintf(out + i * 8, 9, "%08x", h[i]);
        out[64] = '\0';
        return std::string(out, 64);
    }
};

std::string sha256_file(const std::string &path, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (error)
            *error = "cannot open " + path;
        return {};
    }
    Sha256 sha;
    std::vector<uint8_t> buffer(1u << 20);
    while (in)
    {
        in.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size()));
        const std::streamsize n = in.gcount();
        if (n > 0)
            sha.update(buffer.data(), size_t(n));
    }
    return sha.hex();
}

/* --------------------------------------------------- manifest (nai v1 JSON)
 * The manifest is written by scripts/pb-export-matting-model.py. We parse only
 * the fields that are safety-critical: the graph entry for this preset size,
 * its file name and its SHA-256. A structural mismatch is a hard error. */
struct GraphEntry
{
    int size = 0;
    std::string file;
    std::string sha256;
    bool found = false;
};

size_t find_key(const std::string &text, const std::string &key, size_t from)
{
    return text.find("\"" + key + "\"", from);
}

std::string read_string_value(const std::string &text, size_t keyPos)
{
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

int read_int_value(const std::string &text, size_t keyPos)
{
    size_t colon = text.find(':', keyPos);
    if (colon == std::string::npos)
        return -1;
    size_t p = colon + 1;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r'))
        ++p;
    int value = 0;
    bool any = false;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9')
    {
        value = value * 10 + (text[p] - '0');
        any = true;
        ++p;
    }
    return any ? value : -1;
}

GraphEntry find_graph(const std::string &manifest, int size)
{
    GraphEntry entry;
    entry.size = size;
    const size_t graphs = find_key(manifest, "graphs", 0);
    if (graphs == std::string::npos)
        return entry;
    size_t pos = graphs;
    while ((pos = manifest.find('{', pos)) != std::string::npos)
    {
        const size_t end = manifest.find('}', pos);
        if (end == std::string::npos)
            break;
        const std::string object = manifest.substr(pos, end - pos + 1);
        const size_t sizeKey = find_key(object, "size", 0);
        if (sizeKey != std::string::npos && read_int_value(object, sizeKey) == size)
        {
            entry.file = read_string_value(object, find_key(object, "file", 0));
            entry.sha256 = read_string_value(object, find_key(object, "sha256", 0));
            entry.found = !entry.file.empty() && entry.sha256.size() == 64;
            return entry;
        }
        pos = end + 1;
    }
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

/* Host <-> device copies through the CUDA DRIVER API (nvcuda.dll). Using the
 * driver keeps the runtime closure limited to what ORT itself already needs
 * (no cudart import library), and every copy is an explicit, measured call. */
struct CudaCopier
{
    using CuMemcpyHtoD = int(__stdcall *)(uint64_t, const void *, size_t);
    using CuMemcpyDtoH = int(__stdcall *)(void *, uint64_t, size_t);
    using CuCtxSynchronize = int(__stdcall *)(void);

    CuMemcpyHtoD htoD = nullptr;
    CuMemcpyDtoH dtoH = nullptr;
    CuCtxSynchronize sync = nullptr;

    CudaCopier()
    {
        HMODULE driver = GetModuleHandleW(L"nvcuda.dll");
        if (!driver)
            driver = LoadLibraryW(L"nvcuda.dll");
        if (!driver)
            return;
        htoD = reinterpret_cast<CuMemcpyHtoD>(GetProcAddress(driver, "cuMemcpyHtoD_v2"));
        dtoH = reinterpret_cast<CuMemcpyDtoH>(GetProcAddress(driver, "cuMemcpyDtoH_v2"));
        sync = reinterpret_cast<CuCtxSynchronize>(GetProcAddress(driver, "cuCtxSynchronize"));
    }

    bool usable() const { return htoD && dtoH; }
    bool upload(void *device, const void *host, size_t bytes) const
    {
        return htoD && htoD(reinterpret_cast<uint64_t>(device), host, bytes) == 0;
    }
    bool download(void *host, const void *device, size_t bytes) const
    {
        return dtoH && dtoH(host, reinterpret_cast<uint64_t>(device), bytes) == 0;
    }
};

/* ------------------------------------------------------------- Ort env/session */

struct RuntimeEnv
{
    std::mutex mutex;
    std::unique_ptr<Ort::Env> env;

    Ort::Env &get()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!env)
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "nai-paintbrush-matting");
        return *env;
    }
};

RuntimeEnv g_env;

struct PbmcSessionImpl
{
    Ort::Session session{nullptr};
    Ort::MemoryInfo cudaMemory{nullptr};
    Ort::MemoryInfo hostMemory{nullptr};

    std::string backend = "uninitialised";
    std::string error;
    std::string modelPath;
    std::string modelSha;
    std::string providerAssignment;

    int size = 0;
    int deviceIndex = 0;
    bool useGpu = false;
    bool deviceIo = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    double loadMs = 0.0;

    std::mutex runMutex;          /* one inference at a time                  */
    std::mutex cancelMutex;
    Ort::RunOptions *activeRun = nullptr;
    std::atomic<bool> cancelRequested{false};

    /* Scratch: model-space input tensor (host) and output fallback. */
    std::vector<float> hostInput;
};

const char *status_text(int32_t status)
{
    switch (status)
    {
    case PBMC_OK: return "ok";
    case PBMC_ERR_ARGUMENT: return "invalid argument";
    case PBMC_ERR_DLL: return "runtime dependency unavailable";
    case PBMC_ERR_MODEL: return "model or manifest invalid";
    case PBMC_ERR_DEVICE: return "device backend unavailable";
    case PBMC_ERR_INFERENCE: return "inference failed";
    case PBMC_ERR_CANCELLED: return "cancelled";
    case PBMC_ERR_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

void fill_run_info(PbmcRunInfo *info, const PbmcSessionImpl &session)
{
    if (!info)
        return;
    info->used_gpu = session.useGpu ? 1 : 0;
    info->cuda_node_count = session.cudaNodes;
    info->cpu_node_count = session.cpuNodes;
    info->device_index = session.deviceIndex;
    info->input_width = session.size;
    info->input_height = session.size;
    std::snprintf(info->backend, sizeof(info->backend), "%s", session.backend.c_str());
}

void count_provider_assignment(PbmcSessionImpl &session)
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

/* Bilinear RGB sample from the source ROI. */
void sample_rgb_bilinear(const uint8_t *rgb, int width, int height, int stride,
                         double sx, double sy, float out[3])
{
    sx = std::max(0.0, std::min(double(width) - 1.0, sx));
    sy = std::max(0.0, std::min(double(height) - 1.0, sy));
    const int x0 = int(std::floor(sx));
    const int y0 = int(std::floor(sy));
    const int x1 = std::min(width - 1, x0 + 1);
    const int y1 = std::min(height - 1, y0 + 1);
    const double fx = sx - x0;
    const double fy = sy - y0;
    const uint8_t *p00 = rgb + size_t(y0) * size_t(stride) + size_t(x0) * 3;
    const uint8_t *p10 = rgb + size_t(y0) * size_t(stride) + size_t(x1) * 3;
    const uint8_t *p01 = rgb + size_t(y1) * size_t(stride) + size_t(x0) * 3;
    const uint8_t *p11 = rgb + size_t(y1) * size_t(stride) + size_t(x1) * 3;
    for (int c = 0; c < 3; ++c)
    {
        const double a = double(p00[c]) * (1 - fx) + double(p10[c]) * fx;
        const double b = double(p01[c]) * (1 - fx) + double(p11[c]) * fx;
        out[c] = float((a * (1 - fy) + b * fy) / 255.0);
    }
}

/* Trimap classification over the destination pixel's source footprint:
 * unknown wins, then majority foreground/background. This preserves
 * three-state semantics across a large downscale. */
uint8_t sample_trimap_nearest(const uint8_t *trimap, int width, int height,
                              int stride, double sx, double sy, double footprint)
{
    const int radius = std::max(0, int(std::floor(footprint * 0.5)));
    const int cx = int(std::floor(sx));
    const int cy = int(std::floor(sy));
    int fg = 0, bg = 0, unknown = 0;
    for (int dy = -radius; dy <= radius; ++dy)
    {
        const int y = std::max(0, std::min(height - 1, cy + dy));
        const uint8_t *line = trimap + size_t(y) * size_t(stride);
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const int x = std::max(0, std::min(width - 1, cx + dx));
            const uint8_t v = line[x];
            if (v >= 160)
                ++fg;
            else if (v <= 96)
                ++bg;
            else
                ++unknown;
        }
    }
    if (unknown > 0)
        return 128;
    return fg >= bg ? 255 : 0;
}

/* The model's frozen preprocessing (must match scripts/pb-export-matting-model.py):
 *   RGB / 255, then (x - mean) / std with the manifest's ImageNet constants,
 *   channel 3 = trimap in {0, 0.5, 1}, zero padding to the fixed square. */
const float kMean[3] = {123.675f / 255.0f, 116.280f / 255.0f, 103.530f / 255.0f};
const float kStd[3] = {58.395f / 255.0f, 57.120f / 255.0f, 57.375f / 255.0f};

struct Letterbox
{
    int size = 0;
    int drawX = 0;
    int drawY = 0;
    int drawW = 0;
    int drawH = 0;
    double scale = 1.0;
};

Letterbox compute_letterbox(const PbmcRoi &roi, int size)
{
    Letterbox lb;
    lb.size = size;
    lb.scale = std::min(double(size) / double(std::max(1, roi.width)),
                        double(size) / double(std::max(1, roi.height)));
    lb.drawW = std::max(1, int(std::lround(double(roi.width) * lb.scale)));
    lb.drawH = std::max(1, int(std::lround(double(roi.height) * lb.scale)));
    lb.drawW = std::min(size, lb.drawW);
    lb.drawH = std::min(size, lb.drawH);
    lb.drawX = (size - lb.drawW) / 2;
    lb.drawY = (size - lb.drawH) / 2;
    return lb;
}

int32_t preprocess(const PbmcImage &rgb, const PbmcImage &trimap,
                   const PbmcRoi &roi, const Letterbox &lb,
                   std::vector<float> &tensor, std::string *error)
{
    if (rgb.data == nullptr || trimap.data == nullptr)
    {
        *error = "null image buffer";
        return PBMC_ERR_ARGUMENT;
    }
    const int size = lb.size;
    tensor.assign(size_t(size) * size_t(size) * 4, 0.0f);
    const double inv = 1.0 / lb.scale;

    for (int dy = 0; dy < lb.drawH; ++dy)
    {
        const double sy = double(roi.y) + (double(dy) + 0.5) * inv - 0.5;
        const size_t destRow = size_t(lb.drawY + dy);
        for (int dx = 0; dx < lb.drawW; ++dx)
        {
            const double sx = double(roi.x) + (double(dx) + 0.5) * inv - 0.5;
            float c[3];
            sample_rgb_bilinear(rgb.data, rgb.width, rgb.height, rgb.stride, sx, sy, c);
            const size_t di = (destRow * size_t(size) + size_t(lb.drawX + dx));
            tensor[di] = (c[0] - kMean[0]) / kStd[0];
            tensor[size_t(size) * size_t(size) + di] = (c[1] - kMean[1]) / kStd[1];
            tensor[size_t(size) * size_t(size) * 2 + di] = (c[2] - kMean[2]) / kStd[2];
            const uint8_t tri = sample_trimap_nearest(trimap.data, trimap.width,
                                                      trimap.height, trimap.stride,
                                                      sx, sy, inv);
            tensor[size_t(size) * size_t(size) * 3 + di] =
                tri == 255 ? 1.0f : (tri == 0 ? 0.0f : 0.5f);
        }
    }
    return PBMC_OK;
}

/* Bilinear resize of the alpha crop back to ROI size. */
void postprocess(const std::vector<float> &alpha, int size, const Letterbox &lb,
                 const PbmcRoi &roi, float *out)
{
    const double scaleX = double(lb.drawW) / double(std::max(1, roi.width));
    const double scaleY = double(lb.drawH) / double(std::max(1, roi.height));
    for (int y = 0; y < roi.height; ++y)
    {
        const double sy = double(lb.drawY) + (double(y) + 0.5) * scaleY - 0.5;
        const int y0 = std::max(lb.drawY, std::min(lb.drawY + lb.drawH - 1, int(std::floor(sy))));
        const int y1 = std::min(lb.drawY + lb.drawH - 1, y0 + 1);
        const double fy = std::max(0.0, std::min(1.0, sy - y0));
        for (int x = 0; x < roi.width; ++x)
        {
            const double sx = double(lb.drawX) + (double(x) + 0.5) * scaleX - 0.5;
            const int x0 = std::max(lb.drawX, std::min(lb.drawX + lb.drawW - 1, int(std::floor(sx))));
            const int x1 = std::min(lb.drawX + lb.drawW - 1, x0 + 1);
            const double fx = std::max(0.0, std::min(1.0, sx - x0));
            const float a = alpha[size_t(y0) * size_t(size) + size_t(x0)];
            const float b = alpha[size_t(y0) * size_t(size) + size_t(x1)];
            const float c = alpha[size_t(y1) * size_t(size) + size_t(x0)];
            const float d = alpha[size_t(y1) * size_t(size) + size_t(x1)];
            const float top = a * float(1 - fx) + b * float(fx);
            const float bottom = c * float(1 - fx) + d * float(fx);
            float v = top * float(1 - fy) + bottom * float(fy);
            v = std::max(0.0f, std::min(1.0f, v));
            out[size_t(y) * size_t(roi.width) + size_t(x)] = v;
        }
    }
}

/* ------------------------------------------------------------- API methods */

const char *pbmc_version(void)
{
    return "nai-paintbrush-matting/1";
}

int32_t pbmc_probe(PbmcCapabilities *out)
{
    if (!out || out->struct_size != sizeof(PbmcCapabilities))
        return PBMC_ERR_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(PbmcCapabilities);

    try
    {
        const char *version = OrtGetApiBase()->GetVersionString();
        if (version)
            std::snprintf(out->ort_version, sizeof(out->ort_version), "%s", version);
    }
    catch (...)
    {
        std::snprintf(out->error, sizeof(out->error), "ONNX Runtime API unavailable");
        return PBMC_ERR_DLL;
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
        std::snprintf(out->device_name, sizeof(out->device_name), "%s", cuda.deviceName.c_str());
    if (!cuda.error.empty() && out->cuda_provider_available)
        std::snprintf(out->error, sizeof(out->error), "%s", cuda.error.c_str());
    return PBMC_OK;
}

int32_t pbmc_create_session(const PbmcSessionConfig *config, PbmcSession **out_session,
                            char *err, int32_t err_len)
{
    if (!config || !out_session || config->struct_size != sizeof(PbmcSessionConfig))
    {
        set_error(err, err_len, "invalid session config");
        return PBMC_ERR_ARGUMENT;
    }
    *out_session = nullptr;
    if (config->preset_size <= 0 || config->preset_size % 32 != 0)
    {
        set_error(err, err_len, "preset size must be a positive multiple of 32");
        return PBMC_ERR_ARGUMENT;
    }
    if (!config->model_path || !*config->model_path)
    {
        set_error(err, err_len, "model path is empty");
        return PBMC_ERR_ARGUMENT;
    }
    if (!file_exists(config->model_path))
    {
        set_error(err, err_len, std::string("model not found: ") + config->model_path);
        return PBMC_ERR_MODEL;
    }

    const auto start = std::chrono::steady_clock::now();
    std::string modelFile = config->model_path;

    /* Verify provenance against the manifest when one is supplied. */
    if (config->manifest_path && *config->manifest_path)
    {
        const std::string manifest = read_text(config->manifest_path);
        if (manifest.empty())
        {
            set_error(err, err_len, std::string("manifest unreadable: ") + config->manifest_path);
            return PBMC_ERR_MODEL;
        }
        const GraphEntry entry = find_graph(manifest, config->preset_size);
        if (!entry.found)
        {
            set_error(err, err_len, "manifest has no graph for preset size "
                                        + std::to_string(config->preset_size));
            return PBMC_ERR_MODEL;
        }
        /* The manifest names the file; resolve it next to the manifest so the
         * runtime cannot silently load a different copy. */
        std::string dir(config->manifest_path);
        const size_t slash = dir.find_last_of("\\/");
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
        const std::string candidate = dir + entry.file;
        if (file_exists(candidate))
            modelFile = candidate;
        else if (entry.file != std::string(config->model_path))
        {
            set_error(err, err_len, "manifest graph file missing: " + candidate);
            return PBMC_ERR_MODEL;
        }
        std::string hashError;
        const std::string actual = sha256_file(modelFile, &hashError);
        if (actual.empty())
        {
            set_error(err, err_len, "cannot hash model: " + hashError);
            return PBMC_ERR_MODEL;
        }
        if (actual != entry.sha256)
        {
            set_error(err, err_len, "model hash mismatch (manifest " + entry.sha256
                                        + ", actual " + actual + ")");
            return PBMC_ERR_MODEL;
        }
    }

    auto session = std::make_unique<PbmcSessionImpl>();
    session->size = config->preset_size;
    session->modelPath = modelFile;
    session->deviceIndex = 0;

    const bool wantGpu = config->device != PBMC_DEVICE_CPU;
    const bool requireGpu = config->device == PBMC_DEVICE_GPU;

    try
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.EnableMemPattern();
        options.EnableCpuMemArena();
        /* Required to inspect which EP owns the compute nodes: a registered
         * provider is not proof that the expensive operators run on it. */
        options.AddConfigEntry("session.record_ep_graph_assignment_info", "1");
        if (config->intra_threads > 0)
            options.SetIntraOpNumThreads(config->intra_threads);
        if (config->profile_dir && *config->profile_dir)
        {
            std::string prefix(config->profile_dir);
            prefix += "\\pb_matting";
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
                              std::string("CUDA execution provider unavailable: ") + e.what());
                    return PBMC_ERR_DEVICE;
                }
                /* AUTO: continue with CPU and report it honestly. */
                session->backend = std::string("CPUExecutionProvider (CUDA init failed: ")
                                   + e.what() + ")";
            }
        }

        const std::wstring widePath = widen(modelFile);
        session->session = Ort::Session(g_env.get(), widePath.c_str(), options);
        session->hostMemory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        session->useGpu = gpuAppended;
        session->deviceIo = false;

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
                    return PBMC_ERR_DEVICE;
                }
                session->backend += " [no CUDA nodes; CPU fallback]";
                session->useGpu = false;
            }
            try
            {
                session->cudaMemory = Ort::MemoryInfo("Cuda", OrtDeviceAllocator,
                                                     session->deviceIndex,
                                                     OrtMemTypeDefault);
                session->deviceIo = true;
            }
            catch (...)
            {
                session->deviceIo = false;
            }
        }
        else
        {
            session->backend = "CPUExecutionProvider";
            session->useGpu = false;
        }

        session->loadMs = ms_since(start);
    }
    catch (const Ort::Exception &e)
    {
        set_error(err, err_len, std::string("session creation failed: ") + e.what());
        return PBMC_ERR_DEVICE;
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("session creation failed: ") + e.what());
        return PBMC_ERR_MODEL;
    }

    *out_session = reinterpret_cast<PbmcSession *>(session.release());
    return PBMC_OK;
}

void pbmc_destroy_session(PbmcSession *session)
{
    delete reinterpret_cast<PbmcSessionImpl *>(session);
}

int32_t pbmc_get_backend_info(PbmcSession *session, PbmcRunInfo *out)
{
    if (!session || !out || out->struct_size != sizeof(PbmcRunInfo))
        return PBMC_ERR_ARGUMENT;
    auto *impl = reinterpret_cast<PbmcSessionImpl *>(session);
    fill_run_info(out, *impl);
    return PBMC_OK;
}

int32_t pbmc_refine(PbmcSession *session, const PbmcImage *rgb, const PbmcImage *trimap,
                    const PbmcRoi *roiIn, float *outAlpha, int64_t outCapacity,
                    PbmcRunInfo *info, char *err, int32_t err_len)
{
    if (!session || !rgb || !trimap || !outAlpha)
    {
        set_error(err, err_len, "invalid refine arguments");
        return PBMC_ERR_ARGUMENT;
    }
    auto *impl = reinterpret_cast<PbmcSessionImpl *>(session);
    std::lock_guard<std::mutex> lock(impl->runMutex);
    impl->cancelRequested.store(false);

    const auto totalStart = std::chrono::steady_clock::now();
    PbmcRunInfo localInfo{};
    localInfo.struct_size = sizeof(PbmcRunInfo);
    fill_run_info(&localInfo, *impl);

    PbmcRoi roi{};
    if (roiIn && roiIn->width > 0 && roiIn->height > 0)
        roi = *roiIn;
    else
    {
        roi.x = 0;
        roi.y = 0;
        roi.width = rgb->width;
        roi.height = rgb->height;
    }
    roi.x = std::max(0, std::min(rgb->width - 1, roi.x));
    roi.y = std::max(0, std::min(rgb->height - 1, roi.y));
    roi.width = std::max(1, std::min(rgb->width - roi.x, roi.width));
    roi.height = std::max(1, std::min(rgb->height - roi.y, roi.height));

    const int64_t needed = int64_t(roi.width) * int64_t(roi.height);
    if (outCapacity < needed)
    {
        set_error(err, err_len, "alpha buffer too small");
        return PBMC_ERR_ARGUMENT;
    }

    const Letterbox lb = compute_letterbox(roi, impl->size);
    localInfo.roi_x = roi.x;
    localInfo.roi_y = roi.y;
    localInfo.roi_w = roi.width;
    localInfo.roi_h = roi.height;

    const auto preStart = std::chrono::steady_clock::now();
    std::string prepError;
    const int32_t prep = preprocess(*rgb, *trimap, roi, lb, impl->hostInput, &prepError);
    if (prep != PBMC_OK)
    {
        set_error(err, err_len, prepError);
        return prep;
    }
    localInfo.preprocess_ms = ms_since(preStart);

    int32_t result = PBMC_OK;
    std::vector<float> hostOutput;

    try
    {
        const int64_t count = int64_t(impl->size) * int64_t(impl->size);
        const int64_t inputElements = count * 4; /* NCHW: 4 channels */
        const std::array<int64_t, 4> shape = {1, 4, impl->size, impl->size};
        const std::array<int64_t, 4> outShape = {1, 1, impl->size, impl->size};

        const auto inferStart = std::chrono::steady_clock::now();

        bool ran = false;
        if (impl->deviceIo)
        {
            CudaCopier copier;
            if (copier.usable())
            {
                try
                {
                    OrtAllocator *rawAllocator = nullptr;
                    Ort::ThrowOnError(Ort::GetApi().CreateAllocator(
                        impl->session, impl->cudaMemory, &rawAllocator));
                    Ort::Allocator deviceAllocator(rawAllocator);
                    float *deviceInput = static_cast<float *>(
                        deviceAllocator.Alloc(size_t(inputElements) * sizeof(float)));
                    if (deviceInput)
                    {
                        /* Explicit host -> device copy. The tensor is created
                         * over the device pointer, so ORT performs no copy of
                         * its own on input. */
                        const bool uploaded = copier.upload(
                            deviceInput, impl->hostInput.data(),
                            size_t(inputElements) * sizeof(float));
                        if (uploaded)
                        {
                            Ort::Value input = Ort::Value::CreateTensor<float>(
                                impl->cudaMemory, deviceInput, size_t(inputElements),
                                shape.data(), shape.size());
                            Ort::IoBinding binding(impl->session);
                            binding.BindInput("pixel_values", input);
                            binding.BindOutput("alpha", impl->cudaMemory);

                            Ort::RunOptions runOptions;
                            {
                                std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
                                impl->activeRun = &runOptions;
                            }
                            impl->session.Run(runOptions, binding);
                            {
                                std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
                                impl->activeRun = nullptr;
                            }
                            binding.SynchronizeOutputs();

                            const auto outputs = binding.GetOutputValues();
                            if (!outputs.empty())
                            {
                                const float *deviceOut = outputs[0].GetTensorData<float>();
                                hostOutput.resize(size_t(count));
                                if (copier.download(hostOutput.data(), deviceOut,
                                                    size_t(count) * sizeof(float)))
                                    ran = true;
                                else
                                    hostOutput.clear();
                            }
                        }
                        deviceAllocator.Free(deviceInput);
                    }
                }
                catch (const std::exception &)
                {
                    /* Fall through to the host binding path below; the fallback
                     * is reported in the backend line. */
                    impl->deviceIo = false;
                }
                if (!ran)
                    impl->deviceIo = false;
            }
            else
                impl->deviceIo = false;
        }

        if (!ran)
        {
            Ort::Value input = Ort::Value::CreateTensor<float>(
                impl->hostMemory, impl->hostInput.data(), size_t(inputElements),
                shape.data(), shape.size());
            hostOutput.resize(size_t(count));
            Ort::Value output = Ort::Value::CreateTensor<float>(
                impl->hostMemory, hostOutput.data(), size_t(count),
                outShape.data(), outShape.size());

            Ort::RunOptions runOptions;
            {
                std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
                impl->activeRun = &runOptions;
            }
            const char *inputNames[] = {"pixel_values"};
            const char *outputNames[] = {"alpha"};
            impl->session.Run(runOptions, inputNames, &input, 1, outputNames, &output, 1);
            {
                std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
                impl->activeRun = nullptr;
            }
        }

        localInfo.inference_ms = ms_since(inferStart);
        localInfo.used_gpu = impl->useGpu ? 1 : 0;
        /* The preprocessing buffer is kept for the next frame (persistent
         * allocation; no per-frame grow/shrink). */
    }
    catch (const Ort::Exception &e)
    {
        set_error(err, err_len, std::string("inference failed: ") + e.what());
        result = (std::string(e.what()).find("terminated") != std::string::npos)
                     ? PBMC_ERR_CANCELLED
                     : PBMC_ERR_INFERENCE;
    }
    catch (const std::exception &e)
    {
        set_error(err, err_len, std::string("inference failed: ") + e.what());
        result = PBMC_ERR_INFERENCE;
    }

    {
        std::lock_guard<std::mutex> cancelLock(impl->cancelMutex);
        impl->activeRun = nullptr;
    }
    if (result != PBMC_OK)
        return result;
    if (impl->cancelRequested.load())
    {
        set_error(err, err_len, "cancelled");
        return PBMC_ERR_CANCELLED;
    }
    if (hostOutput.size() != size_t(impl->size) * size_t(impl->size))
    {
        set_error(err, err_len, "unexpected output tensor shape");
        return PBMC_ERR_INFERENCE;
    }

    const auto postStart = std::chrono::steady_clock::now();
    postprocess(hostOutput, impl->size, lb, roi, outAlpha);
    localInfo.postprocess_ms = ms_since(postStart);
    localInfo.total_ms = ms_since(totalStart);

    if (info)
    {
        const uint32_t structSize = info->struct_size;
        *info = localInfo;
        info->struct_size = structSize ? structSize : sizeof(PbmcRunInfo);
    }
    return PBMC_OK;
}

void pbmc_cancel_current(PbmcSession *session)
{
    if (!session)
        return;
    auto *impl = reinterpret_cast<PbmcSessionImpl *>(session);
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

const char *pbmc_last_error(PbmcSession *session)
{
    if (!session)
        return "";
    return reinterpret_cast<PbmcSessionImpl *>(session)->error.c_str();
}

const PbmcApi g_api = {
    PBMC_ABI_VERSION,
    sizeof(PbmcApi),
    pbmc_version,
    pbmc_probe,
    pbmc_create_session,
    pbmc_destroy_session,
    pbmc_get_backend_info,
    pbmc_refine,
    pbmc_cancel_current,
    pbmc_last_error,
};

} // namespace

extern "C" const PbmcApi *pbmc_get_api(void)
{
    return &g_api;
}
