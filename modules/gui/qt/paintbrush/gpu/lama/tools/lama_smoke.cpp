/*****************************************************************************
 * lama_smoke.cpp : acceptance smoke + live parity for pb_lama_cuda
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Links pb_lama_cuda directly (no MinGW shim) and exercises:
 *
 *   - synthetic centre mask (640x480)
 *   - boundary-touching mask (right/bottom edges)
 *   - disconnected mask (two far-apart components)
 *   - tiny mask (6x6)
 *   - large mask (> 50% of the canvas)
 *
 * Every case verifies that unmasked pixels and alpha are preserved
 * byte-for-byte, that the masked region actually changed, and that the run
 * reports the expected fixed 512 model input. Outputs are written as PNG.
 *
 * With --image/--mask/--expect it also performs a live comparison against the
 * pinned Carve/LaMa-ONNX reference output. The comparison is scored only on
 * the mask interior (where the compositing alpha is 1); unmasked pixels are
 * preserved by design and the reference PNG is the raw model output.
 *
 * PNG I/O uses the Windows Imaging Component, so this tool needs no Python,
 * no OpenCV and no vendored image codecs.
 *****************************************************************************/

#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shellapi.h>

#include "lama_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Image
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

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

void fail_hr(const char *what, HRESULT hr)
{
    std::printf("  WIC error: %s (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
}

struct Wic
{
    ComPtr<IWICImagingFactory> factory;

    bool init()
    {
        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            fail_hr("CoCreateInstance(WIC)", hr);
            return false;
        }
        return true;
    }

    bool load(const std::string &path, Image *image)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = factory->CreateDecoderFromFilename(
            widen(path).c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr))
        {
            fail_hr(("open " + path).c_str(), hr);
            return false;
        }
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr))
        {
            fail_hr("GetFrame", hr);
            return false;
        }
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            fail_hr("format conversion", hr);
            return false;
        }
        UINT width = 0, height = 0;
        converter->GetSize(&width, &height);
        if (width == 0 || height == 0)
            return false;
        image->width = int(width);
        image->height = int(height);
        image->rgba.resize(size_t(width) * size_t(height) * 4);
        hr = converter->CopyPixels(nullptr, width * 4,
                                   UINT(image->rgba.size()), image->rgba.data());
        if (FAILED(hr))
        {
            fail_hr("CopyPixels", hr);
            return false;
        }
        return true;
    }

    bool resize(const Image &source, int width, int height, Image *out)
    {
        if (width <= 0 || height <= 0)
            return false;
        ComPtr<IWICBitmap> bitmap;
        HRESULT hr = factory->CreateBitmapFromMemory(
            UINT(source.width), UINT(source.height), GUID_WICPixelFormat32bppRGBA,
            UINT(source.width) * 4, UINT(source.rgba.size()),
            const_cast<BYTE *>(source.rgba.data()), &bitmap);
        if (FAILED(hr))
        {
            fail_hr("CreateBitmapFromMemory", hr);
            return false;
        }
        ComPtr<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr))
            hr = scaler->Initialize(bitmap.Get(), UINT(width), UINT(height),
                                    WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr))
        {
            fail_hr("CreateBitmapScaler", hr);
            return false;
        }
        out->width = width;
        out->height = height;
        out->rgba.resize(size_t(width) * size_t(height) * 4);
        hr = scaler->CopyPixels(nullptr, UINT(width) * 4,
                                UINT(out->rgba.size()), out->rgba.data());
        if (FAILED(hr))
        {
            fail_hr("scaler CopyPixels", hr);
            return false;
        }
        return true;
    }

    bool savePng(const std::string &path, const Image &image)
    {
        ComPtr<IWICStream> stream;
        HRESULT hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE);
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr))
            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(&frame, &properties);
        if (SUCCEEDED(hr))
            hr = frame->Initialize(properties.Get());
        if (SUCCEEDED(hr))
            hr = frame->SetSize(UINT(image.width), UINT(image.height));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
        {
            std::printf("  WIC encoder refused BGRA (got other format)\n");
            hr = E_FAIL;
        }
        /* WIC's PNG encoder takes BGRA; swap R/B from the runtime's RGBA. */
        std::vector<uint8_t> bgra(image.rgba.size());
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
        {
            bgra[i + 0] = image.rgba[i + 2];
            bgra[i + 1] = image.rgba[i + 1];
            bgra[i + 2] = image.rgba[i + 0];
            bgra[i + 3] = image.rgba[i + 3];
        }
        if (SUCCEEDED(hr))
            hr = frame->WritePixels(UINT(image.height), UINT(image.width) * 4,
                                    UINT(bgra.size()),
                                    const_cast<BYTE *>(bgra.data()));
        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();
        if (FAILED(hr))
        {
            fail_hr(("save " + path).c_str(), hr);
            return false;
        }
        return true;
    }
};

Image makeImage(int width, int height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(size_t(width) * size_t(height) * 4);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint8_t *p = image.rgba.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
            const int checker = ((x / 32) + (y / 32)) & 1;
            p[0] = uint8_t(std::min(255, 30 + x * 180 / std::max(1, width) + checker * 25));
            p[1] = uint8_t(std::min(255, 40 + y * 170 / std::max(1, height) + checker * 20));
            p[2] = uint8_t(std::min(255, 60 + ((x * 13 + y * 7) & 127)));
            p[3] = 255;
        }
    }
    return image;
}

std::vector<uint8_t> makeMask(int width, int height,
                              int x0, int y0, int x1, int y1)
{
    std::vector<uint8_t> mask(size_t(width) * size_t(height), 0);
    for (int y = std::max(0, y0); y <= std::min(height - 1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(width - 1, x1); ++x)
            mask[size_t(y) * size_t(width) + size_t(x)] = 255;
    return mask;
}

std::vector<uint8_t> grayFromRgba(const Image &image)
{
    std::vector<uint8_t> gray(size_t(image.width) * size_t(image.height));
    for (size_t i = 0, n = gray.size(); i < n; ++i)
    {
        const uint8_t *p = image.rgba.data() + i * 4;
        gray[i] = uint8_t((int(p[0]) * 299 + int(p[1]) * 587 + int(p[2]) * 114) / 1000);
    }
    return gray;
}

Image rgbaFromGray(const std::vector<uint8_t> &gray, int width, int height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(gray.size() * 4);
    for (size_t i = 0, n = gray.size(); i < n; ++i)
    {
        uint8_t *p = image.rgba.data() + i * 4;
        p[0] = p[1] = p[2] = gray[i];
        p[3] = 255;
    }
    return image;
}

struct RunResult
{
    int32_t status = -1;
    PblmRunInfo info{};
    std::string error;
    Image output;
    double totalMs = 0.0;
};

RunResult runInpaint(const PblmApi *api, PblmSession *session, const Image &image,
                     const std::vector<uint8_t> &mask, const char *label)
{
    RunResult result;
    result.output.width = image.width;
    result.output.height = image.height;
    result.output.rgba.assign(size_t(image.width) * size_t(image.height) * 4, 0);

    PblmImage rawImage{};
    rawImage.data = image.rgba.data();
    rawImage.width = image.width;
    rawImage.height = image.height;
    rawImage.stride = image.width * 4;
    rawImage.channels = 4;

    PblmMask rawMask{};
    rawMask.data = mask.data();
    rawMask.width = image.width;
    rawMask.height = image.height;
    rawMask.stride = image.width;
    rawMask.threshold = 128;

    result.info.struct_size = sizeof(PblmRunInfo);
    char error[512] = {};
    const auto start = GetTickCount64();
    result.status = api->inpaint(session, &rawImage, &rawMask, result.output.rgba.data(),
                                 int64_t(result.output.rgba.size()), &result.info,
                                 error, sizeof(error));
    result.totalMs = double(GetTickCount64() - start);
    result.error = error;
    if (result.status != PBLM_OK)
        std::printf("  %s: FAILED (%s)\n", label, error);
    return result;
}

bool verifyResult(const Image &source, const std::vector<uint8_t> &mask,
                  const RunResult &result, const char *label,
                  bool requireSplit, int *failures)
{
    if (result.status != PBLM_OK)
    {
        ++*failures;
        return false;
    }
    if (result.info.input_size != 512 || result.info.roi_w <= 0 || result.info.roi_h <= 0)
    {
        std::printf("  %s: FAILED run info (input=%d roi=%dx%d)\n", label,
                    result.info.input_size, result.info.roi_w, result.info.roi_h);
        ++*failures;
        return false;
    }

    size_t unmasked = 0, unmaskedBad = 0, masked = 0, maskedChanged = 0, alphaBad = 0;
    for (size_t i = 0, n = mask.size(); i < n; ++i)
    {
        const uint8_t *a = source.rgba.data() + i * 4;
        const uint8_t *b = result.output.rgba.data() + i * 4;
        if (b[3] != a[3])
            ++alphaBad;
        if (mask[i] >= 128)
        {
            ++masked;
            if (b[0] != a[0] || b[1] != a[1] || b[2] != a[2])
                ++maskedChanged;
        }
        else
        {
            ++unmasked;
            if (b[0] != a[0] || b[1] != a[1] || b[2] != a[2] || b[3] != a[3])
                ++unmaskedBad;
        }
    }
    const double changedFraction = masked ? double(maskedChanged) / double(masked) : 0.0;
    const bool ok = unmaskedBad == 0 && alphaBad == 0 && maskedChanged > 0
                    && (!requireSplit || result.info.components >= 2);
    std::printf("  %s: %s masked=%zu changed=%.1f%% unmasked=%zu unmaskedBad=%zu "
                "alphaBad=%zu components=%d roi=%d,%d %dx%d backend=%s "
                "load=%.1fms mask=%.1fms preprocess=%.1fms infer=%.1fms "
                "composite=%.1fms total=%.1fms wall=%.1fms\n",
                label, ok ? "ok" : "FAIL", masked, changedFraction * 100.0,
                unmasked, unmaskedBad, alphaBad, result.info.components,
                result.info.roi_x, result.info.roi_y, result.info.roi_w,
                result.info.roi_h, result.info.backend, result.info.load_ms,
                result.info.mask_analysis_ms, result.info.preprocess_ms,
                result.info.inference_ms, result.info.postprocess_ms,
                result.info.total_ms, result.totalMs);
    if (!ok)
        ++*failures;
    return ok;
}

struct ParityStats
{
    double maskedMae = 0.0;
    double unmaskedMae = 0.0;
    double overallMae = 0.0;
    double maskedPsnr = 0.0;
    size_t maskedCount = 0;
};

ParityStats compareParity(const Image &result, const Image &expected,
                          const std::vector<uint8_t> &mask, int erodeRadius)
{
    ParityStats stats;
    double maskedAbs = 0.0, unmaskedAbs = 0.0, overallAbs = 0.0, maskedSq = 0.0;
    size_t unmaskedCount = 0, overallCount = 0;
    for (int y = 0; y < result.height; ++y)
    {
        for (int x = 0; x < result.width; ++x)
        {
            const size_t i = size_t(y) * size_t(result.width) + size_t(x);
            const uint8_t *a = result.rgba.data() + i * 4;
            const uint8_t *b = expected.rgba.data() + i * 4;

            bool interior = mask[i] >= 128;
            if (interior && erodeRadius > 0)
            {
                for (int dy = -erodeRadius; dy <= erodeRadius && interior; ++dy)
                {
                    for (int dx = -erodeRadius; dx <= erodeRadius; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= result.width || ny >= result.height
                            || mask[size_t(ny) * size_t(result.width) + size_t(nx)] < 128)
                        {
                            interior = false;
                            break;
                        }
                    }
                }
            }

            double pixelAbs = 0.0, pixelSq = 0.0;
            for (int c = 0; c < 3; ++c)
            {
                const double diff = double(a[c]) - double(b[c]);
                pixelAbs += std::fabs(diff);
                pixelSq += diff * diff;
            }
            pixelAbs /= 3.0;
            pixelSq /= 3.0;

            overallAbs += pixelAbs;
            ++overallCount;
            if (interior)
            {
                maskedAbs += pixelAbs;
                maskedSq += pixelSq;
                ++stats.maskedCount;
            }
            else if (mask[i] < 128)
            {
                unmaskedAbs += pixelAbs;
                ++unmaskedCount;
            }
        }
    }
    stats.overallMae = overallCount ? overallAbs / double(overallCount) : 0.0;
    stats.maskedMae = stats.maskedCount ? maskedAbs / double(stats.maskedCount) : 0.0;
    stats.unmaskedMae = unmaskedCount ? unmaskedAbs / double(unmaskedCount) : 0.0;
    const double mse = stats.maskedCount ? maskedSq / double(stats.maskedCount) : 0.0;
    stats.maskedPsnr = mse > 0.0 ? 10.0 * std::log10((255.0 * 255.0) / mse) : 99.0;
    return stats;
}

bool parseSize(const std::string &text, int *width, int *height)
{
    const size_t x = text.find('x');
    if (x == std::string::npos)
        return false;
    try
    {
        *width = std::stoi(text.substr(0, x));
        *height = std::stoi(text.substr(x + 1));
    }
    catch (...)
    {
        return false;
    }
    return *width > 0 && *height > 0;
}

} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv)
    {
        std::printf("lama_smoke: cannot read the command line\n");
        return 2;
    }
    std::vector<std::string> args;
    for (int i = 1; i < wargc; ++i)
    {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0,
                                               nullptr, nullptr);
        std::string text(size_t(needed > 0 ? needed - 1 : 0), '\0');
        if (needed > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, text.data(), needed,
                                nullptr, nullptr);
        args.push_back(text);
    }
    LocalFree(wargv);

    auto option = [&args](const char *name, const std::string &fallback = {}) {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name)
                return args[i + 1];
        return fallback;
    };
    auto flag = [&args](const char *name) {
        for (const std::string &arg : args)
            if (arg == name)
                return true;
        return false;
    };

    const std::string modelPath = option("--model");
    const std::string deviceText = option("--device", "auto");
    const std::string sha = option("--sha");
    const std::string outDir = option("--outdir", "lama-smoke-out");
    const bool skipSynthetic = flag("--skip-synthetic");

    if (modelPath.empty())
    {
        std::printf("usage: lama_smoke --model <lama_fp32.onnx> [--device auto|gpu|cpu]\n"
                    "       [--outdir <dir>] [--sha <hex>] [--skip-synthetic]\n"
                    "       [--mask-grow <px>] (0 disables dilation for parity)\n"
                    "       [--image <png> --mask <png> --expect <png> "
                    "[--resize WxH] [--parity-mae <value>]]\n");
        return 2;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
    {
        std::printf("lama_smoke: CoInitializeEx failed (0x%08lx)\n",
                    static_cast<unsigned long>(com));
        return 2;
    }

    Wic wic;
    if (!wic.init())
        return 2;

    const PblmApi *api = pblm_get_api();
    if (!api || api->abi_version != PBLM_ABI_VERSION
        || api->struct_size < sizeof(PblmApi))
    {
        std::printf("lama_smoke: ABI mismatch\n");
        return 2;
    }
    std::printf("lama_smoke: runtime=%s abi=%u\n", api->version(), api->abi_version);

    PblmCapabilities caps{};
    caps.struct_size = sizeof(caps);
    if (api->probe_capabilities(&caps) != PBLM_OK)
        std::printf("lama_smoke: probe failed: %s\n", caps.error);
    else
        std::printf("lama_smoke: ORT=%s CUDA=%d devices=%d CPU=%d adapter=%s\n",
                    caps.ort_version, caps.cuda_provider_available,
                    caps.cuda_device_count, caps.cpu_fallback_available,
                    caps.device_name);

    const int device = deviceText == "gpu" ? PBLM_DEVICE_GPU
                     : deviceText == "cpu" ? PBLM_DEVICE_CPU
                                           : PBLM_DEVICE_AUTO;

    PblmSessionConfig config{};
    config.struct_size = sizeof(config);
    config.model_path = modelPath.c_str();
    config.expected_sha256 = sha.empty() ? nullptr : sha.c_str();
    config.device = device;
    config.input_size = 512;
    config.intra_threads = 0;
    config.require_gpu_operators = device == PBLM_DEVICE_GPU ? 1 : 0;
    config.mask_grow_px = -1;
    const std::string growText = option("--mask-grow");
    if (!growText.empty())
    {
        try
        {
            config.mask_grow_px = std::stoi(growText);
        }
        catch (...)
        {
            std::printf("lama_smoke: bad --mask-grow value\n");
        }
    }

    PblmSession *session = nullptr;
    char error[512] = {};
    const auto loadStart = GetTickCount64();
    const int32_t created = api->create_session(&config, &session, error, sizeof(error));
    if (created != PBLM_OK || !session)
    {
        std::printf("lama_smoke: create_session failed (%s)\n", error);
        CoUninitialize();
        return 3;
    }
    std::printf("lama_smoke: session ready in %llu ms\n",
                static_cast<unsigned long long>(GetTickCount64() - loadStart));

    PblmRunInfo backend{};
    backend.struct_size = sizeof(backend);
    api->get_backend_info(session, &backend);
    std::printf("lama_smoke: backend=%s cudaNodes=%d cpuNodes=%d sha256=%s\n",
                backend.backend, backend.cuda_node_count, backend.cpu_node_count,
                backend.model_sha256);

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    int failures = 0;
    int tests = 0;

    if (!skipSynthetic)
    {
        struct Case
        {
            const char *name;
            int width, height;
            int x0, y0, x1, y1;
            int secondX0, secondY0, secondX1, secondY1; /* -1 = none */
            bool requireSplit;
        };
        const Case cases[] = {
            {"centre", 640, 480, 250, 160, 400, 320, 0, 0, -1, -1, false},
            {"boundary", 480, 360, 400, 260, 479, 359, 0, 0, -1, -1, false},
            {"disconnected", 800, 420, 80, 120, 180, 220, -1, -1, -1, -1, true},
            {"tiny", 320, 240, 160, 120, 166, 126, 0, 0, -1, -1, false},
            {"large", 720, 540, 120, 90, 600, 450, 0, 0, -1, -1, false},
        };

        for (const Case &test : cases)
        {
            ++tests;
            Image image = makeImage(test.width, test.height);
            std::vector<uint8_t> mask = makeMask(test.width, test.height, test.x0,
                                                 test.y0, test.x1, test.y1);
            bool second = false;
            if (std::strcmp(test.name, "disconnected") == 0)
            {
                const int sx0 = 620, sy0 = 60, sx1 = 700, sy1 = 150;
                for (int y = sy0; y <= sy1; ++y)
                    for (int x = sx0; x <= sx1; ++x)
                        mask[size_t(y) * size_t(test.width) + size_t(x)] = 255;
                second = true;
            }

            std::printf("lama_smoke: case=%s %dx%d\n", test.name, test.width, test.height);
            const RunResult result = runInpaint(api, session, image, mask, test.name);
            verifyResult(image, mask, result, test.name, test.requireSplit, &failures);

            if (second && result.status == PBLM_OK)
            {
                size_t firstChanged = 0, secondChanged = 0;
                for (int y = 0; y < image.height; ++y)
                {
                    for (int x = 0; x < image.width; ++x)
                    {
                        const size_t i = size_t(y) * size_t(image.width) + size_t(x);
                        if (mask[i] < 128)
                            continue;
                        const uint8_t *a = image.rgba.data() + i * 4;
                        const uint8_t *b = result.output.rgba.data() + i * 4;
                        const bool changed = a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
                        if (x < 400)
                            firstChanged += changed ? 1 : 0;
                        else
                            secondChanged += changed ? 1 : 0;
                    }
                }
                if (firstChanged == 0 || secondChanged == 0)
                {
                    std::printf("  disconnected: FAIL both components not filled "
                                "(first=%zu second=%zu)\n", firstChanged, secondChanged);
                    ++failures;
                }
                else
                    std::printf("  disconnected: both components filled "
                                "(first=%zu second=%zu pixels)\n",
                                firstChanged, secondChanged);
            }

            if (result.status == PBLM_OK)
            {
                const std::string out = outDir + "/lama-" + test.name + ".png";
                if (!wic.savePng(out, result.output))
                    ++failures;
                else
                    std::printf("  wrote %s\n", out.c_str());
            }
        }
    }

    const std::string imagePath = option("--image");
    const std::string maskPath = option("--mask");
    const std::string expectPath = option("--expect");

    const std::string cancelText = option("--cancel-test");
    if (!cancelText.empty())
    {
        ++tests;
        int delayMs = 120;
        try
        {
            delayMs = std::max(1, std::stoi(cancelText));
        }
        catch (...)
        {
            std::printf("lama_smoke: bad --cancel-test value, using 120 ms\n");
        }

        Image image = makeImage(960, 720);
        std::vector<uint8_t> mask = makeMask(960, 720, 280, 160, 760, 620);
        std::vector<uint8_t> out(size_t(image.width) * size_t(image.height) * 4, 0);

        PblmImage rawImage{};
        rawImage.data = image.rgba.data();
        rawImage.width = image.width;
        rawImage.height = image.height;
        rawImage.stride = image.width * 4;
        rawImage.channels = 4;
        PblmMask rawMask{};
        rawMask.data = mask.data();
        rawMask.width = image.width;
        rawMask.height = image.height;
        rawMask.stride = image.width;
        rawMask.threshold = 128;
        PblmRunInfo info{};
        info.struct_size = sizeof(info);
        char cancelError[512] = {};

        std::printf("lama_smoke: cancel-test delay=%dms\n", delayMs);
        std::thread canceller([api, session, delayMs]() {
            Sleep(DWORD(delayMs));
            api->cancel_current(session);
        });
        const auto cancelStart = GetTickCount64();
        const int32_t status = api->inpaint(session, &rawImage, &rawMask, out.data(),
                                            int64_t(out.size()), &info, cancelError,
                                            sizeof(cancelError));
        const unsigned long long elapsed = GetTickCount64() - cancelStart;
        canceller.join();
        if (status == PBLM_ERR_CANCELLED)
            std::printf("  cancel-test: ok cancelled after %llu ms (%s)\n", elapsed,
                        cancelError);
        else
        {
            std::printf("  cancel-test: FAIL status=%d after %llu ms (%s)\n", status,
                        elapsed, cancelError);
            ++failures;
        }
    }

    if (!imagePath.empty() || !maskPath.empty() || !expectPath.empty())
    {
        if (imagePath.empty() || maskPath.empty() || expectPath.empty())
        {
            std::printf("lama_smoke: parity needs --image, --mask and --expect together\n");
            ++failures;
        }
        else
        {
            ++tests;
            Image image, expect;
            if (!wic.load(imagePath, &image) || !wic.load(expectPath, &expect))
                ++failures;
            else
            {
                Image maskImage;
                if (!wic.load(maskPath, &maskImage))
                    ++failures;
                else
                {
                    int resizeW = 0, resizeH = 0;
                    const std::string resizeText = option("--resize");
                    if (!resizeText.empty()
                        && !parseSize(resizeText, &resizeW, &resizeH))
                    {
                        std::printf("lama_smoke: bad --resize value\n");
                        ++failures;
                    }
                    if (!resizeText.empty() && resizeW > 0)
                    {
                        Image resized;
                        if (!wic.resize(image, resizeW, resizeH, &resized)
                            || !wic.resize(maskImage, resizeW, resizeH, &maskImage))
                        {
                            ++failures;
                        }
                        else
                            image = resized;
                    }

                    std::vector<uint8_t> gray = grayFromRgba(maskImage);
                    std::vector<uint8_t> mask(gray.size(), 0);
                    for (size_t i = 0; i < gray.size(); ++i)
                        mask[i] = gray[i] > 0 ? 255 : 0;

                    if (image.width != expect.width || image.height != expect.height)
                    {
                        std::printf("lama_smoke: parity size mismatch image=%dx%d "
                                    "expect=%dx%d\n", image.width, image.height,
                                    expect.width, expect.height);
                        ++failures;
                    }
                    else
                    {
                        std::printf("lama_smoke: parity image=%s mask=%s expect=%s "
                                    "%dx%d\n", imagePath.c_str(), maskPath.c_str(),
                                    expectPath.c_str(), image.width, image.height);
                        const RunResult result = runInpaint(api, session, image, mask,
                                                           "parity");
                        if (result.status != PBLM_OK)
                            ++failures;
                        else
                        {
                            const ParityStats stats =
                                compareParity(result.output, expect, mask, 3);
                            std::printf("  parity masked-interior: mae=%.2f psnr=%.2f dB "
                                        "pixels=%zu\n", stats.maskedMae,
                                        stats.maskedPsnr, stats.maskedCount);
                            std::printf("  parity unmasked mae=%.2f (preserved by "
                                        "design, reference is raw model output)\n",
                                        stats.unmaskedMae);
                            std::printf("  parity overall mae=%.2f\n", stats.overallMae);

                            const std::string thresholdText = option("--parity-mae");
                            if (!thresholdText.empty())
                            {
                                const double threshold = std::stod(thresholdText);
                                if (stats.maskedMae > threshold)
                                {
                                    std::printf("  parity: FAIL mae %.2f > %.2f\n",
                                                stats.maskedMae, threshold);
                                    ++failures;
                                }
                                else
                                    std::printf("  parity: within threshold %.2f\n",
                                                threshold);
                            }

                            const std::string out = outDir + "/lama-parity.png";
                            if (!wic.savePng(out, result.output))
                                ++failures;
                            else
                                std::printf("  wrote %s\n", out.c_str());
                        }
                    }
                }
            }
        }
    }

    api->destroy_session(session);
    /* Release every COM object before tearing down the apartment. */
    wic.factory.Reset();
    CoUninitialize();

    std::printf("lama_smoke: RESULT %s (%d checks)\n", failures ? "FAIL" : "PASS",
                tests);
    return failures ? 1 : 0;
}
