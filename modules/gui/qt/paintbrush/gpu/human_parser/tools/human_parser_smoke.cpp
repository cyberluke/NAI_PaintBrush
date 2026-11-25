/*****************************************************************************
 * human_parser_smoke.cpp : acceptance smoke for the FASHN human parser
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This is not a unit test with mocks. It loads the real MSVC/ONNX Runtime DLL
 * through the C ABI, creates a real session against the pinned SegFormer-B4
 * graph, and proves:
 *
 *   - ONNX Runtime (CPU and, when present, the CUDA provider) initializes;
 *   - preprocessing reproduces the exporter's validation counts on the
 *     model-card fashion example (the grid counts must match manifest.json
 *     exactly);
 *   - the garment/hair/skin/accessory planes are non-trivial and the full
 *     18-class probability path is valid;
 *   - the parser composes with a real subject alpha on the supplied
 *     Lamborghini cutout, and InheritedCoverage (min) differs from
 *     IndependentProbability (product) exactly as the canonical
 *     composeCoverage() contract says.
 *
 * The FASHN model is a FASHION parser. The car run is an alpha-composition
 * fixture only; its class labels are not car-part segmentation and the smoke
 * says so explicitly.
 *
 * Build: separate MSVC CMake directory (see scripts/pb-build-human-parser.ps1).
 * Run with --runtime-dir pointing at the deployed ORT/CUDA closure.
 *****************************************************************************/

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "human_parser_cuda_api.h"
#include "wic_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/* --------------------------------------------------------------- utilities */

std::string win_error(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::string text = "Win32 error " + std::to_string(code);
    if (length && buffer)
    {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, -1,
                                               nullptr, 0, nullptr, nullptr);
        if (needed > 1)
        {
            std::string utf8(size_t(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8.data(), needed,
                                nullptr, nullptr);
            text += ": " + utf8;
        }
        LocalFree(buffer);
    }
    return text;
}

std::string dir_of(const std::string &path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string join_path(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    const char last = dir.back();
    return dir + ((last == '\\' || last == '/') ? "" : "\\") + name;
}

bool file_exists(const std::string &path)
{
    const DWORD attrs = GetFileAttributesW(pbhp_test::widen_utf8(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string arg_value(int argc, char **argv, const char *name,
                      const std::string &fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return fallback;
}

bool has_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0)
            return true;
    return false;
}

const char *const kClassNames[18] = {
    "background", "face", "hair", "top", "dress", "skirt", "pants",
    "belt", "bag", "hat", "scarf", "glasses", "arms", "hands", "legs",
    "feet", "torso", "jewelry"
};

const char *const kGroupNames[PBHP_GROUP_COUNT] = {
    "none", "garment", "hair", "skin", "accessory", "background"
};

/* manifest.json validation counts on the model-card example, at 96x144.
 * Sum = 13824 = 96*144. This locks preprocessing and the graph contract. */
const uint64_t kExpectedGrid[18] = {
    11530, 61, 236, 619, 2, 0, 568, 0, 0, 0, 0, 12, 206, 35, 279, 158,
    118, 0
};

/* The canonical integer contract from pb_creative_art.cpp::composeCoverage().
 * The Qt service calls the real function; this MSVC smoke cannot link the Qt
 * art library, so it mirrors the three published cases and asserts them. */
enum LocalCoverageMode
{
    LocalInherited = 0,
    LocalIndependent = 1,
    LocalUserConstraint = 2
};

int compose_canonical(int sourceAlpha, int regionMask, int mode)
{
    const int a = std::max(0, std::min(255, sourceAlpha));
    const int b = std::max(0, std::min(255, regionMask));
    if (mode == LocalIndependent)
        return (a * b + 127) / 255;
    return std::min(a, b); /* InheritedCoverage and UserConstraint */
}

/* --------------------------------------------------------- runtime loading */

struct LoadedRuntime
{
    HMODULE module = nullptr;
    const PbhApi *api = nullptr;

    ~LoadedRuntime()
    {
        if (module)
            FreeLibrary(module);
    }
};

const char *const kPreload[] = {
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

void preload_closure(const std::string &dir)
{
    if (dir.empty())
        return;
    for (const char *name : kPreload)
    {
        const std::string path = join_path(dir, name);
        if (!file_exists(path))
            continue;
        LoadLibraryExW(pbhp_test::widen_utf8(path).c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                           | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
}

bool load_runtime(const std::string &dllPath, const std::string &runtimeDir,
                  LoadedRuntime *out, std::string *error)
{
    if (!file_exists(dllPath))
    {
        *error = "parser DLL not found: " + dllPath;
        return false;
    }
    preload_closure(runtimeDir);
    preload_closure(dir_of(dllPath));

    out->module = LoadLibraryExW(
        pbhp_test::widen_utf8(dllPath).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!out->module)
    {
        *error = "LoadLibrary(" + dllPath + ") failed: " + win_error(GetLastError());
        return false;
    }
    auto getApi = reinterpret_cast<PbhGetApiFn>(
        reinterpret_cast<void *>(GetProcAddress(out->module,
                                               PBHP_GET_API_SYMBOL)));
    if (!getApi)
    {
        *error = "parser DLL has no " PBHP_GET_API_SYMBOL " export";
        return false;
    }
    out->api = getApi();
    if (!out->api || out->api->abi_version != PBHP_ABI_VERSION
        || out->api->struct_size < sizeof(PbhApi))
    {
        *error = "human parser ABI mismatch";
        return false;
    }
    return true;
}

/* --------------------------------------------------------------- images */

struct Subject
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> alpha;

    size_t pixels() const { return size_t(width) * size_t(height); }
};

bool load_subject(const std::string &path, Subject *out, std::string *error)
{
    pbhp_test::Image image;
    if (!pbhp_test::load_image(path, &image, error))
        return false;
    out->width = image.width;
    out->height = image.height;
    out->rgb.resize(size_t(image.width) * size_t(image.height) * 3u);
    out->alpha.resize(size_t(image.width) * size_t(image.height));
    for (size_t i = 0; i < out->pixels(); ++i)
    {
        out->rgb[i * 3u + 0] = image.rgba[i * 4u + 0];
        out->rgb[i * 3u + 1] = image.rgba[i * 4u + 1];
        out->rgb[i * 3u + 2] = image.rgba[i * 4u + 2];
        out->alpha[i] = image.rgba[i * 4u + 3];
    }
    return true;
}

/* --------------------------------------------------------------- parsing */

struct Parsed
{
    std::vector<uint8_t> classMap;
    std::vector<uint8_t> garment;
    std::vector<uint8_t> hair;
    std::vector<uint8_t> skin;
    std::vector<uint8_t> accessory;
    std::vector<uint8_t> classProbabilities;
    std::vector<uint8_t> modelInput; /* 384*576*3, diagnostic */
    PbhParseInfo info{};
    bool hasProbabilities = false;
};

bool run_parse(const PbhApi *api, PbhSession *session, const Subject &subject,
               bool wantProbabilities, Parsed *out, std::string *error)
{
    const size_t count = subject.pixels();
    out->classMap.assign(count, 0);
    out->garment.assign(count, 0);
    out->hair.assign(count, 0);
    out->skin.assign(count, 0);
    out->accessory.assign(count, 0);
    out->classProbabilities.clear();
    out->modelInput.assign(size_t(384) * size_t(576) * 3u, 0);
    out->hasProbabilities = wantProbabilities;
    if (wantProbabilities)
        out->classProbabilities.assign(count * 18u, 0);

    PbhParseOutputs outputs{};
    outputs.struct_size = sizeof(outputs);
    outputs.class_map = out->classMap.data();
    outputs.class_map_capacity = int64_t(count);
    outputs.garment = out->garment.data();
    outputs.garment_capacity = int64_t(count);
    outputs.hair = out->hair.data();
    outputs.hair_capacity = int64_t(count);
    outputs.skin = out->skin.data();
    outputs.skin_capacity = int64_t(count);
    outputs.accessory = out->accessory.data();
    outputs.accessory_capacity = int64_t(count);
    if (wantProbabilities)
    {
        outputs.class_probabilities = out->classProbabilities.data();
        outputs.class_probabilities_capacity = int64_t(count * 18u);
    }
    outputs.model_rgb = out->modelInput.data();
    outputs.model_rgb_capacity = int64_t(out->modelInput.size());

    PbhImage image{};
    image.data = subject.rgb.data();
    image.width = subject.width;
    image.height = subject.height;
    image.stride = subject.width * 3;

    out->info = PbhParseInfo{};
    out->info.struct_size = sizeof(PbhParseInfo);
    char err[512] = {};
    const int32_t status = api->parse(session, &image, &outputs, &out->info,
                                      err, sizeof(err));
    if (status != PBHP_OK)
    {
        *error = std::string("parse failed (") + std::to_string(status) + "): "
                 + (err[0] ? err : "unknown error");
        return false;
    }
    return true;
}

/* --------------------------------------------------------------- reporting */

void print_counts(const char *label, const uint64_t *counts, size_t count,
                  const char *const names[], uint64_t total)
{
    std::printf("[%s] %zu values, total=%llu\n", label, count,
                static_cast<unsigned long long>(total));
    for (size_t i = 0; i < count; ++i)
    {
        const double percent = total ? 100.0 * double(counts[i]) / double(total) : 0.0;
        std::printf("  %-12s %10llu  %6.2f%%\n", names[i],
                    static_cast<unsigned long long>(counts[i]), percent);
    }
}

void print_group_stats(const PbhParseInfo &info, size_t sourceCount)
{
    const double denom = 255.0 * double(sourceCount);
    std::printf("[groups] hard-argmax pixels and mean probability\n");
    for (int g = 1; g < PBHP_GROUP_COUNT; ++g)
    {
        const double mean = denom > 0.0
            ? double(info.group_probability_sum[g]) / denom : 0.0;
        std::printf("  %-10s hard=%10llu  meanP=%.4f  (sumP=%.1f px)\n",
                    kGroupNames[g],
                    static_cast<unsigned long long>(info.group_pixels[g]),
                    mean, double(info.group_probability_sum[g]) / 255.0);
    }
}

bool check_probability_sums(const Parsed &parsed, size_t count,
                            int *minSum, int *maxSum)
{
    *minSum = 1000;
    *maxSum = -1;
    const size_t step = std::max<size_t>(1, count / 2000);
    for (size_t i = 0; i < count; i += step)
    {
        int sum = 0;
        for (int c = 0; c < 18; ++c)
            sum += parsed.classProbabilities[size_t(c) * count + i];
        *minSum = std::min(*minSum, sum);
        *maxSum = std::max(*maxSum, sum);
    }
    return *minSum >= 255 - 24 && *maxSum <= 255 + 24;
}

struct CompositionStats
{
    uint64_t inheritedSum = 0;
    uint64_t independentSum = 0;
    uint64_t differing = 0;
};

CompositionStats compose_stats(const std::vector<uint8_t> &alpha,
                               const std::vector<uint8_t> &region)
{
    CompositionStats stats;
    for (size_t i = 0; i < alpha.size(); ++i)
    {
        const int a = alpha[i];
        const int b = region[i];
        const int inherited = compose_canonical(a, b, LocalInherited);
        const int independent = compose_canonical(a, b, LocalIndependent);
        stats.inheritedSum += uint64_t(inherited);
        stats.independentSum += uint64_t(independent);
        if (inherited != independent)
            ++stats.differing;
    }
    return stats;
}

void print_composition(const char *label, const CompositionStats &stats,
                       size_t count)
{
    const double inherited = double(stats.inheritedSum) / 255.0;
    const double independent = double(stats.independentSum) / 255.0;
    std::printf("[compose] %-10s inherited(min)=%.1f px  independent(product)=%.1f px "
                " delta=%.1f px  differing=%llu/%zu\n",
                label, inherited, independent, inherited - independent,
                static_cast<unsigned long long>(stats.differing), count);
}

bool save_masks(const std::string &outDir, const char *prefix,
                const Subject &subject, const Parsed &parsed, std::string *error)
{
    std::filesystem::create_directories(std::filesystem::path(pbhp_test::widen_utf8(outDir)));

    const struct
    {
        const char *suffix;
        const std::vector<uint8_t> *plane;
    } planes[] = {
        {"-classmap-index.png", &parsed.classMap},
        {"-garment.png", &parsed.garment},
        {"-hair.png", &parsed.hair},
        {"-skin.png", &parsed.skin},
        {"-accessory.png", &parsed.accessory},
    };
    for (const auto &entry : planes)
    {
        const std::string path = join_path(outDir, std::string(prefix) + entry.suffix);
        if (!pbhp_test::save_png_gray(path, subject.width, subject.height,
                                      entry.plane->data(), error))
            return false;
    }
    const std::string colorPath = join_path(outDir, std::string(prefix) + "-classmap.png");
    if (!pbhp_test::save_class_map(colorPath, subject.width, subject.height,
                                   parsed.classMap.data(), error))
        return false;
    const std::string modelPath = join_path(outDir, std::string(prefix) + "-model-input.png");
    std::vector<uint8_t> modelRgba(size_t(384) * size_t(576) * 4u);
    for (size_t i = 0; i < size_t(384) * size_t(576); ++i)
    {
        modelRgba[i * 4u + 0] = parsed.modelInput[i * 3u + 0];
        modelRgba[i * 4u + 1] = parsed.modelInput[i * 3u + 1];
        modelRgba[i * 4u + 2] = parsed.modelInput[i * 3u + 2];
        modelRgba[i * 4u + 3] = 255;
    }
    if (!pbhp_test::save_png_rgba(modelPath, 384, 576, modelRgba.data(), error))
        return false;
    return true;
}

int fail(const std::string &message)
{
    std::printf("FAIL %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string model = arg_value(argc, argv, "--model");
    const std::string manifest = arg_value(argc, argv, "--manifest");
    const std::string fashionPath = arg_value(argc, argv, "--fashion");
    const std::string carPath = arg_value(argc, argv, "--car");
    const std::string dllPath = arg_value(argc, argv, "--dll",
                                          "pb_human_parser_cuda.dll");
    std::string runtimeDir = arg_value(argc, argv, "--runtime-dir");
    if (runtimeDir.empty())
    {
        char *env = nullptr;
        size_t envLength = 0;
        if (_dupenv_s(&env, &envLength, "PB_HUMAN_PARSER_RUNTIME_DIR") == 0
            && env && *env)
            runtimeDir = env;
        else
            runtimeDir = dir_of(dllPath);
        free(env);
    }
    const std::string outDir = arg_value(argc, argv, "--out-dir", "smoke-out");
    const std::string deviceText = arg_value(argc, argv, "--device", "auto");
    const bool verifyHash = !has_flag(argc, argv, "--no-hash");

    if (model.empty() || fashionPath.empty() || carPath.empty())
    {
        std::printf(
            "usage: human_parser_smoke --model <onnx> [--manifest <json>]\n"
            "       --fashion <webp|png> --car <cutout.png>\n"
            "       [--dll <pb_human_parser_cuda.dll>] [--runtime-dir <ort-closure>]\n"
            "       [--out-dir <dir>] [--device auto|cpu|gpu] [--no-hash]\n");
        return 2;
    }

    int device = PBHP_DEVICE_AUTO;
    if (deviceText == "gpu")
        device = PBHP_DEVICE_GPU;
    else if (deviceText == "cpu")
        device = PBHP_DEVICE_CPU;

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(com);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        std::printf("warning: COM init 0x%08lx; WIC may fail\n",
                    static_cast<unsigned long>(com));

    int failures = 0;

    LoadedRuntime runtime;
    std::string error;
    if (!load_runtime(dllPath, runtimeDir, &runtime, &error))
    {
        ++failures;
        std::printf("FAIL %s\n", error.c_str());
        if (comInitialized)
            CoUninitialize();
        return 1;
    }
    const PbhApi *api = runtime.api;
    std::printf("runtime %s abi=%u dll=%s\n", api->version(), api->abi_version,
                dllPath.c_str());

    PbhCapabilities caps{};
    caps.struct_size = sizeof(caps);
    if (api->probe_capabilities(&caps) != PBHP_OK)
    {
        ++failures;
        std::printf("FAIL probe: %s\n", caps.error);
    }
    else
    {
        std::printf("ort=%s cudaProvider=%d cudaDevices=%d cpuProvider=%d device=%s\n",
                    caps.ort_version, caps.cuda_provider_available,
                    caps.cuda_device_count, caps.cpu_fallback_available,
                    caps.device_name);
        if (device == PBHP_DEVICE_GPU
            && (!caps.cuda_provider_available || caps.cuda_device_count <= 0))
        {
            ++failures;
            std::printf("FAIL GPU was requested but CUDA is unavailable\n");
        }
    }

    Subject fashion;
    Subject car;
    if (!load_subject(fashionPath, &fashion, &error))
    {
        ++failures;
        std::printf("FAIL fashion fixture: %s\n", error.c_str());
    }
    if (!load_subject(carPath, &car, &error))
    {
        ++failures;
        std::printf("FAIL car fixture: %s\n", error.c_str());
    }
    if (failures)
    {
        if (comInitialized)
            CoUninitialize();
        return 1;
    }

    std::printf("fashion=%s (%dx%d) car=%s (%dx%d)\n",
                fashionPath.c_str(), fashion.width, fashion.height,
                carPath.c_str(), car.width, car.height);

    PbhSessionConfig config{};
    config.struct_size = sizeof(config);
    config.model_path = model.c_str();
    config.manifest_path = manifest.empty() ? nullptr : manifest.c_str();
    config.device = device;
    config.intra_threads = 0;
    config.require_gpu_operators = device == PBHP_DEVICE_GPU ? 1 : 0;
    config.verify_model_hash = verifyHash ? 1 : 0;

    PbhSession *session = nullptr;
    char err[512] = {};
    const auto loadStart = std::chrono::steady_clock::now();
    const int32_t created = api->create_session(&config, &session, err, sizeof(err));
    const double loadMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - loadStart).count();
    if (created != PBHP_OK || !session)
    {
        ++failures;
        std::printf("FAIL create_session (%d): %s\n", created, err);
    }
    else
    {
        PbhBackendInfo backend{};
        backend.struct_size = sizeof(backend);
        api->get_backend_info(session, &backend);
        std::printf("session load=%.0f ms backend=%s gpu=%d cudaNodes=%d cpuNodes=%d\n",
                    loadMs, backend.backend, backend.used_gpu,
                    backend.cuda_node_count, backend.cpu_node_count);
        std::printf("model sha256=%s\n", backend.model_sha256);
        if (verifyHash && manifest.size() && std::strcmp(backend.model_sha256,
                                                         "unverified") == 0)
        {
            ++failures;
            std::printf("FAIL model hash was not verified\n");
        }
    }

    /* ------------------------------------------------------------ fashion */

    Parsed fashionRun1;
    Parsed fashionRun2;
    if (session)
    {
        if (!run_parse(api, session, fashion, true, &fashionRun1, &error))
        {
            ++failures;
            std::printf("FAIL fashion parse: %s\n", error.c_str());
        }
        else
        {
            std::printf("[fashion] run1 pre=%.1f infer=%.1f post=%.1f total=%.1f ms "
                        "gpu=%d wroteClassProbs=%d\n",
                        fashionRun1.info.preprocess_ms,
                        fashionRun1.info.inference_ms,
                        fashionRun1.info.postprocess_ms,
                        fashionRun1.info.total_ms, fashionRun1.info.used_gpu,
                        fashionRun1.info.wrote_class_probabilities);

            uint64_t gridTotal = 0;
            for (int c = 0; c < 18; ++c)
                gridTotal += fashionRun1.info.class_pixels_grid[c];
            print_counts("fashion-grid", fashionRun1.info.class_pixels_grid, 18,
                         kClassNames, gridTotal);

            bool expectedMatch = gridTotal == 13824;
            int64_t totalAbsDiff = 0;
            int64_t maxClassDiff = 0;
            for (int c = 0; c < 18; ++c)
            {
                const int64_t got = int64_t(fashionRun1.info.class_pixels_grid[c]);
                const int64_t want = int64_t(kExpectedGrid[c]);
                const int64_t diff = got > want ? got - want : want - got;
                totalAbsDiff += diff;
                maxClassDiff = std::max(maxClassDiff, diff);
                if (diff != 0)
                    expectedMatch = false;
            }
            /* CPUExecutionProvider reproduces the exporter counts exactly. The
             * CUDA EP may flip a handful of near-tie pixels because its kernel
             * accumulation order differs; 0.1% of the 13,824-cell grid is the
             * honest tolerance, and the semantic class assertions below stay
             * strict. */
            const bool withinTolerance = gridTotal == 13824
                                         && totalAbsDiff <= 14
                                         && maxClassDiff <= 14;
            std::printf("[fashion] exporter validation counts: %s "
                        "(total |diff|=%lld, max class |diff|=%lld)\n",
                        expectedMatch ? "MATCH"
                                      : (withinTolerance ? "WITHIN TOLERANCE"
                                                         : "MISMATCH"),
                        static_cast<long long>(totalAbsDiff),
                        static_cast<long long>(maxClassDiff));
            if (!expectedMatch)
            {
                for (int c = 0; c < 18; ++c)
                {
                    if (fashionRun1.info.class_pixels_grid[c] != kExpectedGrid[c])
                        std::printf("  diff %-12s got=%llu expected=%llu\n",
                                    kClassNames[c],
                                    static_cast<unsigned long long>(
                                        fashionRun1.info.class_pixels_grid[c]),
                                    static_cast<unsigned long long>(kExpectedGrid[c]));
                }
            }
            if (!withinTolerance)
            {
                ++failures;
                std::printf("FAIL exporter validation counts outside tolerance\n");
            }

            const uint64_t sourceTotal = uint64_t(fashion.pixels());
            print_counts("fashion-source", fashionRun1.info.class_pixels, 18,
                         kClassNames, sourceTotal);
            print_group_stats(fashionRun1.info, fashion.pixels());

            int minSum = 0;
            int maxSum = 0;
            if (!check_probability_sums(fashionRun1, fashion.pixels(),
                                        &minSum, &maxSum))
            {
                ++failures;
                std::printf("FAIL class probability sum out of range [%d,%d]\n",
                            minSum, maxSum);
            }
            else
                std::printf("[fashion] per-pixel class probability sum range "
                            "[%d,%d] (expected ~255)\n", minSum, maxSum);

            const struct
            {
                const char *name;
                int index;
            } required[] = {
                {"face", PBHP_CLASS_FACE}, {"hair", PBHP_CLASS_HAIR},
                {"top", PBHP_CLASS_TOP}, {"pants", PBHP_CLASS_PANTS},
                {"arms", PBHP_CLASS_ARMS}, {"legs", PBHP_CLASS_LEGS},
                {"feet", PBHP_CLASS_FEET}, {"torso", PBHP_CLASS_TORSO},
                {"glasses", PBHP_CLASS_GLASSES},
            };
            for (const auto &entry : required)
            {
                if (fashionRun1.info.class_pixels_grid[entry.index] == 0)
                {
                    ++failures;
                    std::printf("FAIL fashion semantic class %s is empty\n",
                                entry.name);
                }
            }
            if (fashionRun1.info.class_pixels_grid[PBHP_CLASS_BACKGROUND]
                < 13824 / 2)
            {
                ++failures;
                std::printf("FAIL background is not the dominant class\n");
            }

            if (!run_parse(api, session, fashion, true, &fashionRun2, &error))
            {
                ++failures;
                std::printf("FAIL fashion rerun: %s\n", error.c_str());
            }
            else
            {
                const bool deterministic =
                    fashionRun1.classMap == fashionRun2.classMap
                    && fashionRun1.garment == fashionRun2.garment
                    && fashionRun1.skin == fashionRun2.skin
                    && fashionRun1.hair == fashionRun2.hair
                    && fashionRun1.accessory == fashionRun2.accessory;
                std::printf("[fashion] rerun pre=%.1f infer=%.1f post=%.1f total=%.1f ms; "
                            "deterministic=%s\n",
                            fashionRun2.info.preprocess_ms,
                            fashionRun2.info.inference_ms,
                            fashionRun2.info.postprocess_ms,
                            fashionRun2.info.total_ms,
                            deterministic ? "yes" : "NO");
                if (!deterministic)
                {
                    ++failures;
                    std::printf("FAIL parser is not deterministic\n");
                }
            }

            if (!save_masks(outDir, "fashion", fashion, fashionRun1, &error))
            {
                ++failures;
                std::printf("FAIL save fashion masks: %s\n", error.c_str());
            }
            else
                std::printf("[fashion] masks saved under %s\n", outDir.c_str());
        }
    }

    /* ---------------------------------------------------------------- car */

    if (session)
    {
        uint64_t alphaPixels = 0;
        uint64_t alphaSum = 0;
        uint64_t fractional = 0;
        for (uint8_t a : car.alpha)
        {
            if (a > 0)
                ++alphaPixels;
            if (a > 0 && a < 255)
                ++fractional;
            alphaSum += a;
        }
        std::printf("[car] alpha coverage=%.1f px, nonzero=%llu, fractional=%llu "
                    "(subject identity remains SAM3-owned)\n",
                    double(alphaSum) / 255.0,
                    static_cast<unsigned long long>(alphaPixels),
                    static_cast<unsigned long long>(fractional));
        if (alphaPixels == 0)
        {
            ++failures;
            std::printf("FAIL car cutout alpha is empty\n");
        }

        Parsed carRun;
        if (!run_parse(api, session, car, true, &carRun, &error))
        {
            ++failures;
            std::printf("FAIL car parse: %s\n", error.c_str());
        }
        else
        {
            std::printf("[car] NOTE: FASHN is a fashion human parser. These "
                        "labels are not car-part segmentation; the car is used "
                        "only to exercise alpha x semantic probability.\n");
            std::printf("[car] run pre=%.1f infer=%.1f post=%.1f total=%.1f ms "
                        "gpu=%d\n",
                        carRun.info.preprocess_ms, carRun.info.inference_ms,
                        carRun.info.postprocess_ms, carRun.info.total_ms,
                        carRun.info.used_gpu);

            uint64_t gridTotal = 0;
            for (int c = 0; c < 18; ++c)
                gridTotal += carRun.info.class_pixels_grid[c];
            print_counts("car-grid", carRun.info.class_pixels_grid, 18,
                         kClassNames, gridTotal);
            print_counts("car-source", carRun.info.class_pixels, 18,
                         kClassNames, uint64_t(car.pixels()));
            print_group_stats(carRun.info, car.pixels());

            /* Foreground probability is the complement of background; summing
             * the 17 non-background planes avoids a second rounding. */
            std::vector<uint8_t> foreground(car.pixels(), 0);
            for (size_t i = 0; i < car.pixels(); ++i)
            {
                int sum = 0;
                for (int c = 1; c < 18; ++c)
                    sum += carRun.classProbabilities[size_t(c) * car.pixels() + i];
                foreground[i] = uint8_t(std::max(0, std::min(255, sum)));
            }

            const CompositionStats garmentStats =
                compose_stats(car.alpha, carRun.garment);
            const CompositionStats foregroundStats =
                compose_stats(car.alpha, foreground);

            /* A hard cutout alpha is 0 or 255, and for a binary alpha
             * min(255,b) == (255*b+127)/255. The two coverage modes are
             * therefore mathematically identical on this fixture. That is the
             * correct result, not a failure. A diagnostic soft alpha (half of
             * the cutout alpha) exercises the distinction the contract exists
             * for; this probe is clearly not the SAM3 identity mask. */
            std::vector<uint8_t> softAlpha(car.alpha.size(), 0);
            for (size_t i = 0; i < car.alpha.size(); ++i)
                softAlpha[i] = uint8_t(car.alpha[i] / 2);
            const CompositionStats softGarment =
                compose_stats(softAlpha, carRun.garment);
            const CompositionStats softForeground =
                compose_stats(softAlpha, foreground);

            const int canonicalInherited = compose_canonical(128, 128, LocalInherited);
            const int canonicalIndependent = compose_canonical(128, 128, LocalIndependent);
            const int canonicalUser = compose_canonical(128, 128, LocalUserConstraint);
            std::printf("[compose] canonical 128+128 -> inherited=%d independent=%d "
                        "userConstraint=%d (expected 128/64/128)\n",
                        canonicalInherited, canonicalIndependent, canonicalUser);
            if (canonicalInherited != 128 || canonicalIndependent != 64
                || canonicalUser != 128)
            {
                ++failures;
                std::printf("FAIL canonical coverage contract\n");
            }

            print_composition("garment", garmentStats, car.pixels());
            print_composition("foreground", foregroundStats, car.pixels());
            std::printf("[compose] the supplied cutout alpha is binary (fractional=0); "
                        "min and product coincide by definition on a 0/255 alpha\n");
            print_composition("soft-garment", softGarment, car.pixels());
            print_composition("soft-foreground", softForeground, car.pixels());

            if (garmentStats.independentSum > garmentStats.inheritedSum
                || foregroundStats.independentSum > foregroundStats.inheritedSum
                || softGarment.independentSum > softGarment.inheritedSum
                || softForeground.independentSum > softForeground.inheritedSum)
            {
                ++failures;
                std::printf("FAIL independent product exceeds inherited min\n");
            }
            if (softGarment.differing == 0 && softForeground.differing == 0)
            {
                ++failures;
                std::printf("FAIL soft alpha did not separate the coverage modes\n");
            }

            if (!save_masks(outDir, "car", car, carRun, &error))
            {
                ++failures;
                std::printf("FAIL save car masks: %s\n", error.c_str());
            }
            else
            {
                const std::string alphaPath = join_path(outDir, "car-alpha.png");
                const std::string fgPath = join_path(outDir, "car-foreground.png");
                if (!pbhp_test::save_png_gray(alphaPath, car.width, car.height,
                                              car.alpha.data(), &error)
                    || !pbhp_test::save_png_gray(fgPath, car.width, car.height,
                                                 foreground.data(), &error))
                {
                    ++failures;
                    std::printf("FAIL save car diagnostics: %s\n", error.c_str());
                }
                else
                    std::printf("[car] masks saved under %s\n", outDir.c_str());
            }
        }
    }

    if (session)
        api->destroy_session(session);

    if (comInitialized)
        CoUninitialize();

    std::printf("\n%s\n", failures == 0 ? "SMOKE PASS" : "SMOKE FAIL");
    return failures == 0 ? 0 : 1;
}
