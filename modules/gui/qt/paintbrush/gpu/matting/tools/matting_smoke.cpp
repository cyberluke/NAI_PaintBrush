/*****************************************************************************
 * matting_smoke.cpp : GPU acceptance smoke for pb_matting_cuda
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * Loads the real runtime, creates a real CUDA session with the exported
 * ViTMatte graph and runs two frames. It proves:
 *   - ONNX Runtime + CUDA provider initialize on this machine;
 *   - the graph's compute nodes are assigned to the CUDA EP (not just
 *     "a provider was requested");
 *   - a fractional alpha is produced and maps back to the requested ROI;
 *   - warm inference is reused (the model is not reloaded).
 *
 * Build: cmake target matting_smoke (MSVC). Run from the packaged runtime
 * directory so the DLL closure resolves.
 *****************************************************************************/

#include "matting_cuda_api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const PbmcApi *api = nullptr;

int fail(const char *what, int32_t status, const char *err)
{
    std::printf("FAIL %s (status=%d %s) %s\n", what, status,
                status == PBMC_ERR_DEVICE ? "device" : "", err ? err : "");
    return 1;
}

std::string arg_value(int argc, char **argv, const char *name, const char *fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return fallback;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string modelDir = arg_value(argc, argv, "--models",
                                            "models");
    const std::string manifest = modelDir + "\\manifest.json";

    api = pbmc_get_api();
    if (!api || api->abi_version != PBMC_ABI_VERSION)
    {
        std::printf("FAIL ABI mismatch\n");
        return 1;
    }
    std::printf("runtime %s abi=%u\n", api->version(), api->abi_version);

    PbmcCapabilities caps{};
    caps.struct_size = sizeof(caps);
    if (api->probe_capabilities(&caps) != PBMC_OK)
    {
        std::printf("FAIL probe: %s\n", caps.error);
        return 1;
    }
    std::printf("ort=%s cudaProvider=%d cudaDevices=%d device=%s\n",
                caps.ort_version, caps.cuda_provider_available,
                caps.cuda_device_count, caps.device_name);
    if (!caps.cuda_provider_available || caps.cuda_device_count <= 0)
    {
        std::printf("FAIL no CUDA provider/device\n");
        return 1;
    }

    int failures = 0;
    for (int size : {512, 1024})
    {
        const std::string model = modelDir + "\\vitmatte-s-" + std::to_string(size) + ".onnx";
        PbmcSessionConfig cfg{};
        cfg.struct_size = sizeof(cfg);
        cfg.model_path = model.c_str();
        cfg.manifest_path = manifest.c_str();
        cfg.device = PBMC_DEVICE_GPU;
        cfg.preset_size = size;
        cfg.require_gpu_operators = 1;

        PbmcSession *session = nullptr;
        char err[512] = {};
        const auto loadStart = std::chrono::steady_clock::now();
        const int32_t created = api->create_session(&cfg, &session, err, sizeof(err));
        const double loadMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - loadStart).count();
        if (created != PBMC_OK)
        {
            failures += fail("create_session", created, err);
            continue;
        }

        PbmcRunInfo backend{};
        backend.struct_size = sizeof(backend);
        api->get_backend_info(session, &backend);
        std::printf("\n[%d] session load %.0f ms backend=%s cudaNodes=%d cpuNodes=%d\n",
                    size, loadMs, backend.backend, backend.cuda_node_count,
                    backend.cpu_node_count);
        if (backend.cuda_node_count <= 0)
        {
            std::printf("FAIL the CUDA EP owns no graph node\n");
            ++failures;
        }

        /* Synthetic frame: turquoise disc on a blue background, 720x1280
         * portrait so the letterbox path is exercised. */
        const int W = 720, H = 1280;
        std::vector<uint8_t> rgb(size_t(W) * H * 3);
        std::vector<uint8_t> trimap(size_t(W) * H);
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                const double dx = x - W / 2.0;
                const double dy = y - H / 2.0;
                const double d = std::sqrt(dx * dx + dy * dy);
                const double a = std::max(0.0, std::min(1.0, (260.0 - d) / 3.0));
                const double r = 24 * a + 40 * (1 - a);
                const double g = 196 * a + 130 * (1 - a);
                const double b = 172 * a + 240 * (1 - a);
                const size_t i = size_t(y) * W + x;
                rgb[i * 3 + 0] = uint8_t(std::lround(r));
                rgb[i * 3 + 1] = uint8_t(std::lround(g));
                rgb[i * 3 + 2] = uint8_t(std::lround(b));
                trimap[i] = d < 250 ? 255 : (d > 270 ? 0 : 128);
            }
        }

        PbmcImage rgbImage{rgb.data(), W, H, W * 3};
        PbmcImage triImage{trimap.data(), W, H, W};
        PbmcRoi roi{0, 0, W, H};
        std::vector<float> alpha(size_t(W) * H);

        for (int run = 0; run < 2; ++run)
        {
            PbmcRunInfo info{};
            info.struct_size = sizeof(info);
            err[0] = '\0';
            const int32_t status = api->refine_frame(session, &rgbImage, &triImage,
                                                     &roi, alpha.data(),
                                                     int64_t(alpha.size()),
                                                     &info, err, sizeof(err));
            if (status != PBMC_OK)
            {
                failures += fail("refine_frame", status, err);
                break;
            }
            int fractional = 0;
            float minA = 1.0f, maxA = 0.0f;
            for (float a : alpha)
            {
                minA = std::min(minA, a);
                maxA = std::max(maxA, a);
                if (a > 0.02f && a < 0.98f)
                    ++fractional;
            }
            std::printf("[%d] run %d: pre=%.1f infer=%.1f post=%.1f total=%.1f ms "
                        "gpu=%d nodes(cuda=%d cpu=%d) alpha=[%.2f,%.2f] fractional=%d\n",
                        size, run, info.preprocess_ms, info.inference_ms,
                        info.postprocess_ms, info.total_ms, info.used_gpu,
                        info.cuda_node_count, info.cpu_node_count, minA, maxA,
                        fractional);
            const float center = alpha[size_t(H / 2) * W + W / 2];
            const float corner = alpha[0];
            std::printf("[%d] center=%.3f corner=%.3f\n", size, center, corner);
            if (center < 0.9f || corner > 0.05f)
            {
                std::printf("FAIL alpha silhouette (center %.3f corner %.3f)\n",
                            center, corner);
                ++failures;
            }
            if (fractional < 200)
            {
                std::printf("FAIL alpha has no fractional boundary (%d)\n", fractional);
                ++failures;
            }
            if (run == 1 && info.inference_ms > 2000.0)
            {
                std::printf("FAIL warm inference implausibly slow (%.0f ms)\n",
                            info.inference_ms);
                ++failures;
            }
        }
        api->destroy_session(session);
    }

    std::printf("\n%s\n", failures == 0 ? "SMOKE PASS" : "SMOKE FAIL");
    return failures == 0 ? 0 : 1;
}
