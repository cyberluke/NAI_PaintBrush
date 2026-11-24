/*****************************************************************************
 * lama_runtime.cpp : MSVC/ONNX Runtime implementation of pb_lama_cuda
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Production rules enforced here:
 *
 *   - one persistent Ort::Session per LaMa session (never reload per call);
 *   - the CUDA execution provider is requested explicitly, the CPU provider is
 *     never used as a silent substitute in GPU mode;
 *   - the pinned graph is a FIXED 512 export; input/output names and shapes are
 *     validated at load time instead of being assumed;
 *   - a square context ROI is cropped around the (dilated) mask, resized to
 *     512, and the decoded result is feathered back into the full canvas;
 *   - unmasked pixels are copied byte-for-byte; the model never repaints the
 *     whole canvas;
 *   - disconnected masks are split into bounded context passes when the
 *     components are far apart (<= 8 passes), so two distant objects do not
 *     collapse into one low-resolution square;
 *   - scratch memory is bounded (ROI mask/alpha buffers are capped) and the
 *     fixed model tensors are reused across calls;
 *   - every failure returns a specific status code and a readable message;
 *   - cancel_current() terminates a run in flight through ORT's own
 *     RunOptionsSetTerminate (no unsafe thread killing).
 *****************************************************************************/

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "lama_runtime.h"

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

double ms_since(std::chrono::steady_clock::time_point start)
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

std::string lower_hex(std::string text)
{
    for (char &c : text)
    {
        if (c >= 'A' && c <= 'F')
            c = char(c - 'A' + 'a');
    }
    return text;
}

/* --------------------------------------------------------- SHA-256 (compact)
 * Used to verify the ONNX graph against the pinned model hash before it is
 * loaded. This is not a security boundary (the file is local), it is an
 * integrity and provenance check so a stale or partial copy cannot masquerade
 * as the pinned model. */
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
            env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "nai-paintbrush-lama");
        return *env;
    }
};

RuntimeEnv g_env;

/* Bounded scratch: two uint8 ROI planes plus one line buffer. 48 MP allows a
 * full 6000x6000 context at 8-bit scratch (~144 MB transient) and refuses a
 * pathological 16K full-canvas mask with a precise error instead of silently
 * exhausting memory. Individual component passes stay far below it. */
constexpr int64_t kMaxRoiPixels = 48ll * 1024ll * 1024ll;
constexpr int kMaxComponentsAnalyzed = 64;
constexpr size_t kMaxLabelPixels = 16u * 1024u * 1024u;
constexpr int kMaxComponentsSplit = 7; /* first N individually + rest group */

struct Rect
{
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; /* inclusive pixel bounds */

    bool valid() const { return x1 >= x0 && y1 >= y0; }
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
    void include(const Rect &other)
    {
        if (!other.valid())
            return;
        if (!valid())
        {
            *this = other;
            return;
        }
        x0 = std::min(x0, other.x0);
        y0 = std::min(y0, other.y0);
        x1 = std::max(x1, other.x1);
        y1 = std::max(y1, other.y1);
    }
};

struct Component
{
    int32_t label = 0;
    Rect box;
    int64_t area = 0;
};

struct MaskAnalysis
{
    Rect bbox;
    int64_t pixels = 0;
    bool haveLabels = false;
    int labelW = 0;
    int labelH = 0;
    int labelX = 0;
    int labelY = 0;
    std::vector<int32_t> labels;
    std::vector<Component> components;
};

struct Roi
{
    int x = 0, y = 0, w = 0, h = 0;
};

struct PassSpec
{
    Rect box;                    /* source-pixel bounds of the pass          */
    bool useLabels = false;      /* composite only allowed labels            */
    std::vector<uint8_t> allowed;/* index by component label                 */
};

struct PblmSessionImpl
{
    Ort::Session session{nullptr};
    Ort::MemoryInfo hostMemory{nullptr};

    std::string backend = "uninitialised";
    std::string error;
    std::string modelPath;
    std::string modelSha;
    std::string providerAssignment;
    std::string imageInputName;
    std::string maskInputName;
    std::string outputName;

    int size = 512;
    int deviceIndex = 0;
    bool useGpu = false;
    int cudaNodes = 0;
    int cpuNodes = 0;
    int maskGrowPx = -1;
    double loadMs = 0.0;

    std::mutex runMutex;          /* one inference at a time                  */
    std::mutex cancelMutex;
    Ort::RunOptions *activeRun = nullptr;
    std::atomic<bool> cancelRequested{false};

    /* Reused fixed-size model tensors (512*512*3 and 512*512). */
    std::vector<float> imageTensor;
    std::vector<float> maskTensor;
    std::vector<float> outputTensor;
};

const char *status_text(int32_t status)
{
    switch (status)
    {
    case PBLM_OK: return "ok";
    case PBLM_ERR_ARGUMENT: return "invalid argument";
    case PBLM_ERR_DLL: return "runtime dependency unavailable";
    case PBLM_ERR_MODEL: return "model invalid";
    case PBLM_ERR_DEVICE: return "device backend unavailable";
    case PBLM_ERR_INFERENCE: return "inference failed";
    case PBLM_ERR_CANCELLED: return "cancelled";
    case PBLM_ERR_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

void fill_run_info(PblmRunInfo *info, const PblmSessionImpl &session)
{
    if (!info)
        return;
    info->used_gpu = session.useGpu ? 1 : 0;
    info->cuda_node_count = session.cudaNodes;
    info->cpu_node_count = session.cpuNodes;
    info->device_index = session.deviceIndex;
    info->input_size = session.size;
    std::snprintf(info->backend, sizeof(info->backend), "%s", session.backend.c_str());
    std::snprintf(info->model_sha256, sizeof(info->model_sha256), "%s",
                  session.modelSha.c_str());
}

void count_provider_assignment(PblmSessionImpl &session)
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

/* --------------------------------------------------------- graph I/O checks */

struct TensorSpec
{
    std::string name;
    std::vector<int64_t> shape;
    ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
};

bool read_tensor_spec(const Ort::Session &session, bool input, size_t index,
                      TensorSpec *spec, std::string *error)
{
    Ort::AllocatorWithDefaultOptions allocator;
    try
    {
        Ort::AllocatedStringPtr name = input
            ? session.GetInputNameAllocated(index, allocator)
            : session.GetOutputNameAllocated(index, allocator);
        spec->name = name ? name.get() : "";
        Ort::TypeInfo type = input ? session.GetInputTypeInfo(index)
                                   : session.GetOutputTypeInfo(index);
        const auto tensor = type.GetTensorTypeAndShapeInfo();
        spec->elementType = tensor.GetElementType();
        spec->shape = tensor.GetShape();
        return true;
    }
    catch (const std::exception &e)
    {
        if (error)
            *error = std::string(input ? "input " : "output ")
                     + std::to_string(index) + " metadata failed: " + e.what();
        return false;
    }
}

bool validate_graph_io(const Ort::Session &session, int size, PblmSessionImpl *impl,
                       std::string *error)
{
    const size_t inputCount = session.GetInputCount();
    const size_t outputCount = session.GetOutputCount();
    if (inputCount != 2 || outputCount < 1)
    {
        *error = "graph must expose exactly 2 inputs (image, mask) and 1 output; got "
                 + std::to_string(inputCount) + " inputs, "
                 + std::to_string(outputCount) + " outputs";
        return false;
    }

    TensorSpec specs[2];
    for (size_t i = 0; i < 2; ++i)
    {
        if (!read_tensor_spec(session, true, i, &specs[i], error))
            return false;
    }
    const TensorSpec *image = nullptr;
    const TensorSpec *mask = nullptr;
    for (const TensorSpec &spec : specs)
    {
        if (spec.name == "image")
            image = &spec;
        else if (spec.name == "mask")
            mask = &spec;
    }
    if (!image || !mask)
    {
        *error = "graph inputs must be named 'image' and 'mask' (found '"
                 + specs[0].name + "', '" + specs[1].name + "')";
        return false;
    }

    auto check_shape = [size, error](const TensorSpec &spec, int channels,
                                     const char *what) {
        const std::vector<int64_t> &s = spec.shape;
        if (s.size() != 4)
        {
            *error = std::string(what) + " must be 4-D [batch,channels,h,w]";
            return false;
        }
        const int64_t batch = s[0];
        if (batch != 1 && batch != -1)
        {
            *error = std::string(what) + " batch must be 1 (or dynamic)";
            return false;
        }
        if (s[1] != channels)
        {
            *error = std::string(what) + " channels must be "
                     + std::to_string(channels);
            return false;
        }
        if (s[2] != size || s[3] != size)
        {
            *error = std::string(what) + " must be " + std::to_string(size)
                     + "x" + std::to_string(size) + " (fixed export); got "
                     + std::to_string(s[2]) + "x" + std::to_string(s[3]);
            return false;
        }
        return true;
    };

    if (!check_shape(*image, 3, "image input"))
        return false;
    if (!check_shape(*mask, 1, "mask input"))
        return false;
    if (image->elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
        || mask->elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
    {
        *error = "graph inputs must be float32";
        return false;
    }

    TensorSpec output;
    if (!read_tensor_spec(session, false, 0, &output, error))
        return false;
    if (output.elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
    {
        *error = "graph output must be float32";
        return false;
    }
    if (!check_shape(output, 3, "output"))
        return false;

    impl->imageInputName = image->name;
    impl->maskInputName = mask->name;
    impl->outputName = output.name;
    return true;
}

/* -------------------------------------------------------------- mask analysis */

bool scan_mask_bbox(const PblmMask &mask, int threshold, Rect *bbox, int64_t *count)
{
    Rect box;
    int64_t total = 0;
    for (int y = 0; y < mask.height; ++y)
    {
        const uint8_t *row = mask.data + size_t(y) * size_t(mask.stride);
        for (int x = 0; x < mask.width; ++x)
        {
            if (row[x] >= threshold)
            {
                ++total;
                if (!box.valid())
                    box = Rect{x, y, x, y};
                else
                {
                    box.x0 = std::min(box.x0, x);
                    box.y0 = std::min(box.y0, y);
                    box.x1 = std::max(box.x1, x);
                    box.y1 = std::max(box.y1, y);
                }
            }
        }
    }
    *bbox = box;
    *count = total;
    return total > 0;
}

/* Label 4-connected components over the mask bbox. Returns false (with no
 * error) when the mask is too large or too fragmented to label within the
 * bounded budget; the caller then uses one combined pass. */
bool analyze_components(const PblmMask &mask, int threshold, const Rect &bbox,
                        MaskAnalysis *analysis, const std::atomic<bool> &cancel)
{
    const int lw = bbox.width();
    const int lh = bbox.height();
    if (size_t(lw) * size_t(lh) > kMaxLabelPixels)
        return false;

    analysis->labels.assign(size_t(lw) * size_t(lh), 0);
    std::vector<int32_t> queue;
    queue.reserve(4096);

    int nextLabel = 1;
    for (int y = 0; y < lh; ++y)
    {
        const uint8_t *row = mask.data + size_t(bbox.y0 + y) * size_t(mask.stride)
                             + size_t(bbox.x0);
        for (int x = 0; x < lw; ++x)
        {
            if (row[x] < threshold || analysis->labels[size_t(y) * lw + x] != 0)
                continue;
            if (nextLabel > kMaxComponentsAnalyzed)
            {
                analysis->labels.clear();
                analysis->components.clear();
                return false;
            }

            Component component;
            component.label = nextLabel;
            component.box = Rect{x, y, x, y};
            queue.clear();
            queue.push_back(y * lw + x);
            analysis->labels[size_t(y) * lw + x] = nextLabel;

            size_t head = 0;
            while (head < queue.size())
            {
                const int32_t flat = queue[head++];
                const int cx = flat % lw;
                const int cy = flat / lw;
                component.box.x0 = std::min(component.box.x0, cx);
                component.box.y0 = std::min(component.box.y0, cy);
                component.box.x1 = std::max(component.box.x1, cx);
                component.box.y1 = std::max(component.box.y1, cy);
                ++component.area;

                const int nx[4] = {cx + 1, cx - 1, cx, cx};
                const int ny[4] = {cy, cy, cy + 1, cy - 1};
                for (int k = 0; k < 4; ++k)
                {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= lw || ny[k] >= lh)
                        continue;
                    const size_t ni = size_t(ny[k]) * lw + nx[k];
                    if (analysis->labels[ni] != 0)
                        continue;
                    const uint8_t value = *(mask.data
                        + size_t(bbox.y0 + ny[k]) * size_t(mask.stride)
                        + size_t(bbox.x0 + nx[k]));
                    if (value < threshold)
                        continue;
                    analysis->labels[ni] = nextLabel;
                    queue.push_back(int32_t(ni));
                }

                if ((head & 0xffffu) == 0 && cancel.load())
                    return false;
            }

            /* Component boxes are stored relative to the mask bbox. */
            analysis->components.push_back(component);
            ++nextLabel;
        }
    }

    analysis->haveLabels = !analysis->components.empty();
    analysis->labelW = lw;
    analysis->labelH = lh;
    analysis->labelX = bbox.x0;
    analysis->labelY = bbox.y0;
    return true;
}

/* ----------------------------------------------------------- ROI geometry */

Roi context_roi(const Rect &box, int imageW, int imageH)
{
    const int bw = box.width();
    const int bh = box.height();
    const int longest = std::max(bw, bh);

    /* 50% context on every side (ROI = 2x the object), with a sane floor for
     * tiny masks and clamped to the image's larger dimension. */
    const int pad = std::max(32, (longest + 1) / 2);
    int side = std::max(longest + pad * 2, 128);
    side = std::min(side, std::max(imageW, imageH));

    const int cx = box.x0 + bw / 2;
    const int cy = box.y0 + bh / 2;

    Roi roi;
    roi.w = side;
    roi.h = side;
    if (side <= imageW)
        roi.x = std::max(0, std::min(imageW - side, cx - side / 2));
    else
        roi.x = 0; /* square wider than the image: extend right, replicate  */
    if (side <= imageH)
        roi.y = std::max(0, std::min(imageH - side, cy - side / 2));
    else
        roi.y = 0;
    return roi;
}

/* -------------------------------------------------------- mask buffers */

void dilate_binary_inplace(uint8_t *data, int w, int h, int radius,
                           std::vector<uint8_t> &line)
{
    if (radius <= 0 || w <= 0 || h <= 0)
        return;

    line.resize(size_t(w));
    for (int y = 0; y < h; ++y)
    {
        uint8_t *row = data + size_t(y) * size_t(w);
        int count = 0;
        for (int i = std::max(0, -radius); i <= std::min(w - 1, radius); ++i)
            count += row[i] ? 1 : 0;
        for (int x = 0; x < w; ++x)
        {
            line[x] = count > 0 ? 1 : 0;
            const int add = x + 1 + radius;
            if (add < w)
                count += row[add] ? 1 : 0;
            const int remove = x - radius;
            if (remove >= 0)
                count -= row[remove] ? 1 : 0;
        }
        std::memcpy(row, line.data(), size_t(w));
    }

    line.resize(size_t(h));
    for (int x = 0; x < w; ++x)
    {
        int count = 0;
        for (int i = std::max(0, -radius); i <= std::min(h - 1, radius); ++i)
            count += data[size_t(i) * size_t(w) + size_t(x)] ? 1 : 0;
        for (int y = 0; y < h; ++y)
        {
            line[y] = count > 0 ? 1 : 0;
            const int add = y + 1 + radius;
            if (add < h)
                count += data[size_t(add) * size_t(w) + size_t(x)] ? 1 : 0;
            const int remove = y - radius;
            if (remove >= 0)
                count -= data[size_t(remove) * size_t(w) + size_t(x)] ? 1 : 0;
        }
        for (int y = 0; y < h; ++y)
            data[size_t(y) * size_t(w) + size_t(x)] = line[y];
    }
}

/* Box blur of a 0/1 plane into 0..255. In-place with a one-line temp. */
void blur_binary_to_alpha(uint8_t *data, int w, int h, int radius,
                          std::vector<uint8_t> &line)
{
    if (radius <= 0)
    {
        for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i)
            data[i] = data[i] ? 255 : 0;
        return;
    }
    const int window = radius * 2 + 1;

    line.resize(size_t(w));
    for (int y = 0; y < h; ++y)
    {
        uint8_t *row = data + size_t(y) * size_t(w);
        int sum = 0;
        for (int i = std::max(0, -radius); i <= std::min(w - 1, radius); ++i)
            sum += row[i] ? 1 : 0;
        for (int x = 0; x < w; ++x)
        {
            line[x] = uint8_t((sum * 255 + window / 2) / window);
            const int add = x + 1 + radius;
            if (add < w)
                sum += row[add] ? 1 : 0;
            const int remove = x - radius;
            if (remove >= 0)
                sum -= row[remove] ? 1 : 0;
        }
        std::memcpy(row, line.data(), size_t(w));
    }

    line.resize(size_t(h));
    for (int x = 0; x < w; ++x)
    {
        int sum = 0;
        for (int i = std::max(0, -radius); i <= std::min(h - 1, radius); ++i)
            sum += data[size_t(i) * size_t(w) + size_t(x)] ? 1 : 0;
        for (int y = 0; y < h; ++y)
        {
            line[y] = uint8_t((sum * 255 + window / 2) / window);
            const int add = y + 1 + radius;
            if (add < h)
                sum += data[size_t(add) * size_t(w) + size_t(x)] ? 1 : 0;
            const int remove = y - radius;
            if (remove >= 0)
                sum -= data[size_t(remove) * size_t(w) + size_t(x)] ? 1 : 0;
        }
        for (int y = 0; y < h; ++y)
            data[size_t(y) * size_t(w) + size_t(x)] = line[y];
    }
}

/* ------------------------------------------------------------- sampling */

inline const uint8_t *pixel_ptr(const PblmImage &image, int x, int y)
{
    return image.data + size_t(y) * size_t(image.stride)
           + size_t(x) * size_t(image.channels);
}

void sample_rgb_bilinear(const PblmImage &image, double sx, double sy, float out[3])
{
    sx = std::max(0.0, std::min(double(image.width) - 1.0, sx));
    sy = std::max(0.0, std::min(double(image.height) - 1.0, sy));
    const int x0 = int(std::floor(sx));
    const int y0 = int(std::floor(sy));
    const int x1 = std::min(image.width - 1, x0 + 1);
    const int y1 = std::min(image.height - 1, y0 + 1);
    const double fx = sx - x0;
    const double fy = sy - y0;
    const uint8_t *p00 = pixel_ptr(image, x0, y0);
    const uint8_t *p10 = pixel_ptr(image, x1, y0);
    const uint8_t *p01 = pixel_ptr(image, x0, y1);
    const uint8_t *p11 = pixel_ptr(image, x1, y1);
    for (int c = 0; c < 3; ++c)
    {
        const double a = double(p00[c]) * (1 - fx) + double(p10[c]) * fx;
        const double b = double(p01[c]) * (1 - fx) + double(p11[c]) * fx;
        out[c] = float((a * (1 - fy) + b * fy) / 255.0);
    }
}

/* ---------------------------------------------------------- model run */

int32_t run_model(PblmSessionImpl &impl, std::string *error, double *inferenceMs)
{
    const auto start = std::chrono::steady_clock::now();
    const int64_t plane = int64_t(impl.size) * int64_t(impl.size);
    const int64_t imageElements = plane * 3;
    impl.outputTensor.resize(size_t(imageElements));

    const std::array<int64_t, 4> imageShape = {1, 3, impl.size, impl.size};
    const std::array<int64_t, 4> maskShape = {1, 1, impl.size, impl.size};
    const std::array<int64_t, 4> outputShape = {1, 3, impl.size, impl.size};

    try
    {
        Ort::Value image = Ort::Value::CreateTensor<float>(
            impl.hostMemory, impl.imageTensor.data(), size_t(imageElements),
            imageShape.data(), imageShape.size());
        Ort::Value mask = Ort::Value::CreateTensor<float>(
            impl.hostMemory, impl.maskTensor.data(), size_t(plane),
            maskShape.data(), maskShape.size());
        Ort::Value output = Ort::Value::CreateTensor<float>(
            impl.hostMemory, impl.outputTensor.data(), size_t(imageElements),
            outputShape.data(), outputShape.size());

        Ort::RunOptions runOptions;
        {
            std::lock_guard<std::mutex> lock(impl.cancelMutex);
            impl.activeRun = &runOptions;
        }
        const char *inputNames[2] = {impl.imageInputName.c_str(),
                                     impl.maskInputName.c_str()};
        const char *outputNames[1] = {impl.outputName.c_str()};
        Ort::Value inputs[2] = {std::move(image), std::move(mask)};
        impl.session.Run(runOptions, inputNames, inputs, 2, outputNames, &output, 1);
        {
            std::lock_guard<std::mutex> lock(impl.cancelMutex);
            impl.activeRun = nullptr;
        }
    }
    catch (const Ort::Exception &e)
    {
        {
            std::lock_guard<std::mutex> lock(impl.cancelMutex);
            impl.activeRun = nullptr;
        }
        const std::string what = e.what();
        if (impl.cancelRequested.load()
            || what.find("terminat") != std::string::npos
            || what.find("cancel") != std::string::npos)
        {
            if (error)
                *error = "cancelled";
            return PBLM_ERR_CANCELLED;
        }
        if (error)
            *error = "inference failed: " + what;
        return PBLM_ERR_INFERENCE;
    }
    catch (const std::exception &e)
    {
        {
            std::lock_guard<std::mutex> lock(impl.cancelMutex);
            impl.activeRun = nullptr;
        }
        if (error)
            *error = "inference failed: " + std::string(e.what());
        return PBLM_ERR_INFERENCE;
    }

    if (impl.cancelRequested.load())
    {
        if (error)
            *error = "cancelled";
        return PBLM_ERR_CANCELLED;
    }
    *inferenceMs = ms_since(start);
    return PBLM_OK;
}

/* --------------------------------------------------------- one ROI pass */

int32_t process_pass(PblmSessionImpl &impl, const PblmImage &image,
                     const PblmMask &mask, const MaskAnalysis &analysis,
                     const PassSpec &pass, int growRadius, int featherRadius,
                     uint8_t *outRgba, PblmRunInfo *aggregate, std::string *error)
{
    const Roi roi = context_roi(pass.box, image.width, image.height);
    const int64_t roiPixels = int64_t(roi.w) * int64_t(roi.h);
    if (roiPixels <= 0 || roiPixels > kMaxRoiPixels)
    {
        *error = "context ROI " + std::to_string(roi.w) + "x"
                 + std::to_string(roi.h) + " exceeds the bounded scratch budget";
        return PBLM_ERR_UNSUPPORTED;
    }

    const auto passStart = std::chrono::steady_clock::now();

    /* Binary mask for this pass, then a dilated copy for the model input. */
    std::vector<uint8_t> origRoi(size_t(roiPixels), 0);
    std::vector<uint8_t> dilatedRoi;

    const int clipX0 = std::max(0, roi.x);
    const int clipY0 = std::max(0, roi.y);
    const int clipX1 = std::min(image.width, roi.x + roi.w);
    const int clipY1 = std::min(image.height, roi.y + roi.h);
    for (int y = clipY0; y < clipY1; ++y)
    {
        const uint8_t *maskRow = mask.data + size_t(y) * size_t(mask.stride);
        const int labelY = y - analysis.labelY;
        const int32_t *labelRow = nullptr;
        if (pass.useLabels && labelY >= 0 && labelY < analysis.labelH)
            labelRow = analysis.labels.data()
                       + size_t(labelY) * size_t(analysis.labelW);
        uint8_t *dst = origRoi.data() + size_t(y - roi.y) * size_t(roi.w)
                       + size_t(clipX0 - roi.x);
        for (int x = clipX0; x < clipX1; ++x, ++dst)
        {
            bool masked = false;
            if (pass.useLabels && labelRow)
            {
                const int labelX = x - analysis.labelX;
                if (labelX >= 0 && labelX < analysis.labelW)
                {
                    const int32_t label = labelRow[labelX];
                    masked = label > 0 && size_t(label) < pass.allowed.size()
                             && pass.allowed[size_t(label)] != 0;
                }
            }
            else
                masked = maskRow[x] >= (mask.threshold > 0 ? mask.threshold : 128);
            *dst = masked ? 1 : 0;
        }
        if ((y & 0x3f) == 0 && impl.cancelRequested.load())
        {
            *error = "cancelled";
            return PBLM_ERR_CANCELLED;
        }
    }

    dilatedRoi = origRoi;
    std::vector<uint8_t> line;
    dilate_binary_inplace(dilatedRoi.data(), roi.w, roi.h, growRadius, line);

    /* Model tensors: RGB bilinear from the source (edge-replicated outside the
     * image), mask max-pooled over each model pixel's source footprint. */
    const int size = impl.size;
    const int64_t plane = int64_t(size) * int64_t(size);
    impl.imageTensor.resize(size_t(plane * 3));
    impl.maskTensor.resize(size_t(plane));

    const int samplesX = std::max(1, std::min(8, int(std::ceil(double(roi.w) / size))));
    const int samplesY = std::max(1, std::min(8, int(std::ceil(double(roi.h) / size))));

    for (int my = 0; my < size; ++my)
    {
        const double v = (double(my) + 0.5) * double(roi.h) / double(size);
        const double srcY = double(roi.y) + v - 0.5;
        const int maskV0 = int(double(my) * double(roi.h) / double(size));
        const int maskV1 = std::max(maskV0 + 1,
                                    int((double(my) + 1.0) * double(roi.h) / double(size)));

        for (int mx = 0; mx < size; ++mx)
        {
            const double u = (double(mx) + 0.5) * double(roi.w) / double(size);
            const double srcX = double(roi.x) + u - 0.5;
            float rgb[3];
            sample_rgb_bilinear(image, srcX, srcY, rgb);
            const size_t di = size_t(my) * size_t(size) + size_t(mx);
            impl.imageTensor[di] = rgb[0];
            impl.imageTensor[size_t(plane) + di] = rgb[1];
            impl.imageTensor[size_t(plane) * 2 + di] = rgb[2];

            bool hole = false;
            const int maskU0 = int(double(mx) * double(roi.w) / double(size));
            const int maskU1 = std::max(maskU0 + 1,
                                        int((double(mx) + 1.0) * double(roi.w) / double(size)));
            for (int sy = 0; sy < samplesY && !hole; ++sy)
            {
                const int yy = maskV0 + (maskV1 - maskV0) * (2 * sy + 1) / (2 * samplesY);
                const int cy = std::max(0, std::min(roi.h - 1, yy));
                for (int sx = 0; sx < samplesX; ++sx)
                {
                    const int xx = maskU0 + (maskU1 - maskU0) * (2 * sx + 1) / (2 * samplesX);
                    const int cx = std::max(0, std::min(roi.w - 1, xx));
                    if (dilatedRoi[size_t(cy) * size_t(roi.w) + size_t(cx)])
                    {
                        hole = true;
                        break;
                    }
                }
            }
            impl.maskTensor[di] = hole ? 1.0f : 0.0f;
        }
        if ((my & 0x3f) == 0 && impl.cancelRequested.load())
        {
            *error = "cancelled";
            return PBLM_ERR_CANCELLED;
        }
    }

    double inferenceMs = -1.0;
    std::string runError;
    const double preprocessMs = ms_since(passStart);
    const int32_t status = run_model(impl, &runError, &inferenceMs);
    if (status != PBLM_OK)
    {
        *error = runError;
        return status;
    }
    const auto postStart = std::chrono::steady_clock::now();

    /* Determine the export's output scale. The pinned Carve/LaMa-ONNX wrapper
     * clamps inpainted * 255, so the tensor is in 0..255; other exports may
     * return 0..1. Pick the scale from the data instead of assuming one. */
    float maxValue = 0.0f;
    for (float value : impl.outputTensor)
        maxValue = std::max(maxValue, value);
    const float outputScale = maxValue > 1.5f ? (1.0f / 255.0f) : 1.0f;

    /* Feather alpha: blur the ORIGINAL (non-dilated) pass mask and clip it so
     * nothing outside the mask is ever modified. */
    {
        /* Reuse the dilated plane's memory for the alpha. */
        dilatedRoi = origRoi;
        blur_binary_to_alpha(dilatedRoi.data(), roi.w, roi.h, featherRadius, line);
        for (size_t i = 0, n = size_t(roiPixels); i < n; ++i)
            dilatedRoi[i] = origRoi[i] ? dilatedRoi[i] : 0;
    }

    /* Composite into the caller's full-canvas RGBA (already a source copy). */
    const int compX0 = std::max(0, roi.x);
    const int compY0 = std::max(0, roi.y);
    const int compX1 = std::min(image.width, roi.x + roi.w);
    const int compY1 = std::min(image.height, roi.y + roi.h);
    for (int y = compY0; y < compY1; ++y)
    {
        uint8_t *dstRow = outRgba + (size_t(y) * size_t(image.width) + size_t(compX0)) * 4;
        const uint8_t *alphaRow = dilatedRoi.data()
                                  + size_t(y - roi.y) * size_t(roi.w)
                                  + size_t(compX0 - roi.x);
        const double modelY = (double(y - roi.y) + 0.5) * double(size) / double(roi.h) - 0.5;
        const int my0 = std::max(0, std::min(size - 1, int(std::floor(modelY))));
        const int my1 = std::min(size - 1, my0 + 1);
        const double fy = std::max(0.0, std::min(1.0, modelY - my0));

        for (int x = compX0; x < compX1; ++x, dstRow += 4)
        {
            const int alpha = alphaRow[x - compX0];
            if (alpha == 0)
                continue; /* source pixel already copied byte-for-byte */

            const double modelX = (double(x - roi.x) + 0.5) * double(size) / double(roi.w) - 0.5;
            const int mx0 = std::max(0, std::min(size - 1, int(std::floor(modelX))));
            const int mx1 = std::min(size - 1, mx0 + 1);
            const double fx = std::max(0.0, std::min(1.0, modelX - mx0));

            for (int c = 0; c < 3; ++c)
            {
                const size_t planeOffset = size_t(c) * size_t(plane);
                const float a = impl.outputTensor[planeOffset + size_t(my0) * size_t(size) + size_t(mx0)];
                const float b = impl.outputTensor[planeOffset + size_t(my0) * size_t(size) + size_t(mx1)];
                const float cc = impl.outputTensor[planeOffset + size_t(my1) * size_t(size) + size_t(mx0)];
                const float d = impl.outputTensor[planeOffset + size_t(my1) * size_t(size) + size_t(mx1)];
                const float top = a * float(1 - fx) + b * float(fx);
                const float bottom = cc * float(1 - fx) + d * float(fx);
                float value = (top * float(1 - fy) + bottom * float(fy)) * outputScale;
                value = std::max(0.0f, std::min(1.0f, value));
                const int generated = int(value * 255.0f + 0.5f);
                const int source = dstRow[c];
                dstRow[c] = uint8_t((source * (255 - alpha) + generated * alpha + 127) / 255);
            }
            /* alpha channel is preserved from the source (already copied)   */
        }
        if ((y & 0x3f) == 0 && impl.cancelRequested.load())
        {
            *error = "cancelled";
            return PBLM_ERR_CANCELLED;
        }
    }

    if (aggregate)
    {
        if (aggregate->roi_w <= 0)
        {
            aggregate->roi_x = roi.x;
            aggregate->roi_y = roi.y;
            aggregate->roi_w = roi.w;
            aggregate->roi_h = roi.h;
        }
        else
        {
            const int x0 = std::min(aggregate->roi_x, roi.x);
            const int y0 = std::min(aggregate->roi_y, roi.y);
            const int x1 = std::max(aggregate->roi_x + aggregate->roi_w, roi.x + roi.w);
            const int y1 = std::max(aggregate->roi_y + aggregate->roi_h, roi.y + roi.h);
            aggregate->roi_x = x0;
            aggregate->roi_y = y0;
            aggregate->roi_w = x1 - x0;
            aggregate->roi_h = y1 - y0;
        }
        aggregate->preprocess_ms += preprocessMs;
        aggregate->inference_ms += inferenceMs;
        aggregate->postprocess_ms += ms_since(postStart);
    }
    return PBLM_OK;
}

/* ------------------------------------------------------------- API methods */

const char *pblm_version(void)
{
    return "nai-paintbrush-lama/1";
}

int32_t pblm_probe(PblmCapabilities *out)
{
    if (!out || out->struct_size != sizeof(PblmCapabilities))
        return PBLM_ERR_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(PblmCapabilities);

    try
    {
        const char *version = OrtGetApiBase()->GetVersionString();
        if (version)
            std::snprintf(out->ort_version, sizeof(out->ort_version), "%s", version);
    }
    catch (...)
    {
        std::snprintf(out->error, sizeof(out->error), "ONNX Runtime API unavailable");
        return PBLM_ERR_DLL;
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
    return PBLM_OK;
}

int32_t pblm_create_session(const PblmSessionConfig *config, PblmSession **outSession,
                            char *err, int32_t errLen)
{
    if (!config || !outSession || config->struct_size != sizeof(PblmSessionConfig))
    {
        set_error(err, errLen, "invalid session config");
        return PBLM_ERR_ARGUMENT;
    }
    *outSession = nullptr;
    if (config->input_size <= 0 || config->input_size % 32 != 0)
    {
        set_error(err, errLen, "input size must be a positive multiple of 32");
        return PBLM_ERR_ARGUMENT;
    }
    if (config->device < PBLM_DEVICE_AUTO || config->device > PBLM_DEVICE_CPU)
    {
        set_error(err, errLen, "invalid device tier");
        return PBLM_ERR_ARGUMENT;
    }
    if (!config->model_path || !*config->model_path)
    {
        set_error(err, errLen, "model path is empty");
        return PBLM_ERR_ARGUMENT;
    }
    if (!file_exists(config->model_path))
    {
        set_error(err, errLen, std::string("model not found: ") + config->model_path);
        return PBLM_ERR_MODEL;
    }

    const auto start = std::chrono::steady_clock::now();
    auto session = std::make_unique<PblmSessionImpl>();
    session->size = config->input_size;
    session->modelPath = config->model_path;
    session->deviceIndex = 0;
    session->maskGrowPx = config->mask_grow_px;
    session->imageTensor.resize(size_t(config->input_size) * size_t(config->input_size) * 3);
    session->maskTensor.resize(size_t(config->input_size) * size_t(config->input_size));

    /* Pinned-hash verification (when the caller supplies the pin). */
    if (config->expected_sha256 && *config->expected_sha256)
    {
        std::string hashError;
        const std::string actual = sha256_file(config->model_path, &hashError);
        if (actual.empty())
        {
            set_error(err, errLen, "cannot hash model: " + hashError);
            return PBLM_ERR_MODEL;
        }
        session->modelSha = actual;
        const std::string expected = lower_hex(config->expected_sha256);
        if (actual != expected)
        {
            set_error(err, errLen, "model hash mismatch (expected " + expected
                                       + ", actual " + actual + ")");
            return PBLM_ERR_MODEL;
        }
    }

    const bool wantGpu = config->device != PBLM_DEVICE_CPU;
    const bool requireGpu = config->device == PBLM_DEVICE_GPU;

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
            prefix += "\\pb_lama";
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
                    set_error(err, errLen,
                              std::string("CUDA execution provider unavailable: ") + e.what());
                    return PBLM_ERR_DEVICE;
                }
                session->backend = std::string("CPUExecutionProvider (CUDA init failed: ")
                                   + e.what() + ")";
            }
        }

        const std::wstring widePath = widen(session->modelPath);
        session->session = Ort::Session(g_env.get(), widePath.c_str(), options);
        session->hostMemory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
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
                    set_error(err, errLen,
                              "CUDA provider was registered but no graph node was "
                              "assigned to it (" + session->providerAssignment + ")");
                    return PBLM_ERR_DEVICE;
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

        if (config->require_gpu_operators && !session->useGpu)
        {
            set_error(err, errLen,
                      "GPU execution was required but the session is running on the CPU");
            return PBLM_ERR_DEVICE;
        }

        std::string ioError;
        if (!validate_graph_io(session->session, session->size, session.get(), &ioError))
        {
            set_error(err, errLen, "model I/O mismatch: " + ioError);
            return PBLM_ERR_MODEL;
        }
        if (session->modelSha.empty())
            session->modelSha = sha256_file(session->modelPath, nullptr);

        session->loadMs = ms_since(start);
    }
    catch (const Ort::Exception &e)
    {
        set_error(err, errLen, std::string("session creation failed: ") + e.what());
        return PBLM_ERR_DEVICE;
    }
    catch (const std::exception &e)
    {
        set_error(err, errLen, std::string("session creation failed: ") + e.what());
        return PBLM_ERR_MODEL;
    }

    *outSession = reinterpret_cast<PblmSession *>(session.release());
    return PBLM_OK;
}

void pblm_destroy_session(PblmSession *session)
{
    delete reinterpret_cast<PblmSessionImpl *>(session);
}

int32_t pblm_get_backend_info(PblmSession *session, PblmRunInfo *out)
{
    if (!session || !out || out->struct_size != sizeof(PblmRunInfo))
        return PBLM_ERR_ARGUMENT;
    auto *impl = reinterpret_cast<PblmSessionImpl *>(session);
    fill_run_info(out, *impl);
    return PBLM_OK;
}

void copy_source_to_rgba(const PblmImage &image, uint8_t *out)
{
    for (int y = 0; y < image.height; ++y)
    {
        const uint8_t *src = image.data + size_t(y) * size_t(image.stride);
        uint8_t *dst = out + size_t(y) * size_t(image.width) * 4;
        if (image.channels == 4)
            std::memcpy(dst, src, size_t(image.width) * 4);
        else
        {
            for (int x = 0; x < image.width; ++x, src += 3, dst += 4)
            {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
            }
        }
    }
}

int32_t pblm_inpaint(PblmSession *session, const PblmImage *rgb, const PblmMask *mask,
                     uint8_t *outRgba, int64_t outCapacity, PblmRunInfo *info,
                     char *err, int32_t errLen)
{
    if (!session || !rgb || !mask || !outRgba)
    {
        set_error(err, errLen, "invalid inpaint arguments");
        return PBLM_ERR_ARGUMENT;
    }
    if (rgb->width <= 0 || rgb->height <= 0
        || (rgb->channels != 3 && rgb->channels != 4)
        || rgb->stride < rgb->width * rgb->channels)
    {
        set_error(err, errLen, "invalid image buffer");
        return PBLM_ERR_ARGUMENT;
    }
    if (mask->width != rgb->width || mask->height != rgb->height
        || mask->stride < mask->width)
    {
        set_error(err, errLen, "mask size mismatch");
        return PBLM_ERR_ARGUMENT;
    }
    const int64_t needed = int64_t(rgb->width) * int64_t(rgb->height) * 4;
    if (outCapacity < needed)
    {
        set_error(err, errLen, "output buffer too small");
        return PBLM_ERR_ARGUMENT;
    }

    auto *impl = reinterpret_cast<PblmSessionImpl *>(session);
    std::lock_guard<std::mutex> lock(impl->runMutex);
    impl->cancelRequested.store(false);

    const auto totalStart = std::chrono::steady_clock::now();
    PblmRunInfo local{};
    local.struct_size = sizeof(PblmRunInfo);
    fill_run_info(&local, *impl);

    /* Start from an exact source copy so every unmasked pixel is preserved
     * byte-for-byte, including alpha and pixels outside all ROI passes. */
    copy_source_to_rgba(*rgb, outRgba);

    const int threshold = mask->threshold > 0 ? mask->threshold : 128;
    MaskAnalysis analysis;
    if (!scan_mask_bbox(*mask, threshold, &analysis.bbox, &analysis.pixels))
    {
        set_error(err, errLen, "mask has no masked pixels");
        return PBLM_ERR_ARGUMENT;
    }

    const bool labeled = analyze_components(*mask, threshold, analysis.bbox,
                                            &analysis, impl->cancelRequested);
    if (impl->cancelRequested.load())
    {
        set_error(err, errLen, "cancelled");
        return PBLM_ERR_CANCELLED;
    }

    /* Decide between one combined pass and component-split passes. */
    std::vector<PassSpec> passes;
    if (!labeled || analysis.components.size() <= 1)
    {
        PassSpec pass;
        pass.box = analysis.bbox;
        pass.useLabels = false;
        passes.push_back(std::move(pass));
    }
    else
    {
        int maxCompSide = 0;
        for (const Component &component : analysis.components)
            maxCompSide = std::max(maxCompSide,
                                   std::max(component.box.width(), component.box.height()));
        const int combinedSide = std::max(analysis.bbox.width(), analysis.bbox.height());

        /* Components that live inside a compact cluster share one context;
         * distant components get their own high-resolution pass. */
        if (combinedSide <= 2 * std::max(1, maxCompSide))
        {
            PassSpec pass;
            pass.box = analysis.bbox;
            pass.useLabels = false;
            passes.push_back(std::move(pass));
        }
        else
        {
            std::vector<size_t> order(analysis.components.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(),
                             [&analysis](size_t a, size_t b) {
                                 return analysis.components[a].area
                                        > analysis.components[b].area;
                             });

            const size_t splitCount = std::min<size_t>(kMaxComponentsSplit,
                                                       order.size());
            std::vector<uint8_t> inTop(analysis.components.size() + 1, 0);
            for (size_t i = 0; i < splitCount; ++i)
            {
                const Component &component = analysis.components[order[i]];
                PassSpec pass;
                pass.box = Rect{component.box.x0 + analysis.labelX,
                                component.box.y0 + analysis.labelY,
                                component.box.x1 + analysis.labelX,
                                component.box.y1 + analysis.labelY};
                pass.useLabels = true;
                pass.allowed.assign(analysis.components.size() + 1, 0);
                pass.allowed[size_t(component.label)] = 1;
                inTop[size_t(component.label)] = 1;
                passes.push_back(std::move(pass));
            }

            bool hasRest = false;
            Rect restBox;
            for (const Component &component : analysis.components)
            {
                if (inTop[size_t(component.label)])
                    continue;
                hasRest = true;
                restBox.include(Rect{component.box.x0 + analysis.labelX,
                                     component.box.y0 + analysis.labelY,
                                     component.box.x1 + analysis.labelX,
                                     component.box.y1 + analysis.labelY});
            }
            if (hasRest)
            {
                PassSpec rest;
                rest.box = restBox;
                rest.useLabels = true;
                rest.allowed.assign(analysis.components.size() + 1, 0);
                for (const Component &component : analysis.components)
                {
                    if (!inTop[size_t(component.label)])
                        rest.allowed[size_t(component.label)] = 1;
                }
                passes.push_back(std::move(rest));
            }
        }
    }

    const int longestImageSide = std::max(rgb->width, rgb->height);
    int growRadius = impl->maskGrowPx >= 0
                         ? impl->maskGrowPx
                         : int(std::lround(0.006 * double(longestImageSide)));
    growRadius = std::max(0, std::min(64, growRadius));
    int featherRadius = int(std::lround(0.003 * double(longestImageSide)));
    featherRadius = std::max(1, std::min(8, featherRadius));

    local.components = labeled ? int(analysis.components.size()) : 0;

    for (const PassSpec &pass : passes)
    {
        std::string passError;
        const int32_t status = process_pass(*impl, *rgb, *mask, analysis, pass,
                                            growRadius, featherRadius, outRgba,
                                            &local, &passError);
        if (status != PBLM_OK)
        {
            set_error(err, errLen, passError.empty() ? status_text(status) : passError);
            return status;
        }
    }

    local.total_ms = ms_since(totalStart);
    local.used_gpu = impl->useGpu ? 1 : 0;
    if (info)
    {
        const uint32_t structSize = info->struct_size;
        *info = local;
        info->struct_size = structSize ? structSize : sizeof(PblmRunInfo);
    }
    return PBLM_OK;
}

void pblm_cancel_current(PblmSession *session)
{
    if (!session)
        return;
    auto *impl = reinterpret_cast<PblmSessionImpl *>(session);
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

const char *pblm_last_error(PblmSession *session)
{
    if (!session)
        return "";
    return reinterpret_cast<PblmSessionImpl *>(session)->error.c_str();
}

const PblmApi g_api = {
    PBLM_ABI_VERSION,
    sizeof(PblmApi),
    pblm_version,
    pblm_probe,
    pblm_create_session,
    pblm_destroy_session,
    pblm_get_backend_info,
    pblm_inpaint,
    pblm_cancel_current,
    pblm_last_error,
};

} // namespace

extern "C" const PblmApi *pblm_get_api(void)
{
    return &g_api;
}
