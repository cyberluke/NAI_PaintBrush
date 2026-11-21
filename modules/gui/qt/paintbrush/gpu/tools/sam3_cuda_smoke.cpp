/*****************************************************************************
 * sam3_cuda_smoke.cpp : native acceptance harness for the CUDA SAM3 bridge
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Loads sam3_cuda.dll through the exact C ABI the VLC plugin uses, runs one
 * backend per invocation, saves masks, and prints machine-readable latency /
 * device lines. Run it twice (gpu, cpu) and compare the saved masks with
 * --iou to prove correctness.
 *
 * Examples:
 *   sam3_cuda_smoke --mode gpu --image frame0.png --point-x 315 --point-y 250 \
 *                   --frames-dir frames --frames 5 --out gpu
 *   sam3_cuda_smoke --mode cpu --image frame0.png --point-x 315 --point-y 250 \
 *                   --frames-dir frames --frames 3 --out cpu
 *   sam3_cuda_smoke --iou gpu_seg.png cpu_seg.png
 *****************************************************************************/

#include "sam3_cuda_api.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;
};

bool load_image(const std::string &path, Image &image)
{
    int channels = 0;
    unsigned char *data = stbi_load(path.c_str(), &image.width, &image.height,
                                    &channels, 3);
    if (!data)
        return false;
    image.rgb.assign(data, data + size_t(image.width) * image.height * 3);
    stbi_image_free(data);
    return true;
}

bool save_mask(const std::string &path, const Sam3cDetection &det)
{
    if (!det.mask || det.mask_width <= 0 || det.mask_height <= 0)
        return false;
    return stbi_write_png(path.c_str(), det.mask_width, det.mask_height, 1,
                          det.mask, det.mask_width) != 0;
}

struct Mask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

Mask copy_mask(const Sam3cDetection &det)
{
    Mask mask;
    if (!det.mask || det.mask_width <= 0 || det.mask_height <= 0)
        return mask;
    mask.width = det.mask_width;
    mask.height = det.mask_height;
    mask.pixels.assign(det.mask, det.mask + size_t(mask.width) * mask.height);
    return mask;
}

double mask_iou(const Mask &a, const Mask &b)
{
    if (a.width != b.width || a.height != b.height || a.pixels.empty() ||
        b.pixels.empty())
        return -1.0;
    size_t intersection = 0;
    size_t union_area = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const bool pa = a.pixels[i] > 127;
        const bool pb = b.pixels[i] > 127;
        if (pa && pb)
            ++intersection;
        if (pa || pb)
            ++union_area;
    }
    return union_area ? double(intersection) / double(union_area) : -1.0;
}

int iou_mode(const char *a_path, const char *b_path)
{
    int aw = 0, ah = 0, ac = 0, bw = 0, bh = 0, bc = 0;
    unsigned char *a = stbi_load(a_path, &aw, &ah, &ac, 1);
    unsigned char *b = stbi_load(b_path, &bw, &bh, &bc, 1);
    int rc = 2;
    if (a && b && aw == bw && ah == bh) {
        size_t intersection = 0, union_area = 0;
        const size_t n = size_t(aw) * ah;
        for (size_t i = 0; i < n; ++i) {
            const bool pa = a[i] > 127;
            const bool pb = b[i] > 127;
            if (pa && pb)
                ++intersection;
            if (pa || pb)
                ++union_area;
        }
        const double iou = union_area ? double(intersection) / union_area : -1.0;
        std::printf("{\"stage\":\"iou\",\"a\":\"%s\",\"b\":\"%s\",\"width\":%d,"
                    "\"height\":%d,\"iou\":%.6f,\"intersection\":%zu,"
                    "\"union\":%zu}\n",
                    a_path, b_path, aw, ah, iou, intersection, union_area);
        rc = 0;
    } else {
        std::fprintf(stderr, "mask compare failed: missing or size mismatch\n");
    }
    stbi_image_free(a);
    stbi_image_free(b);
    return rc;
}

void print_usage()
{
    std::fprintf(stderr,
        "usage:\n"
        "  sam3_cuda_smoke --mode gpu|cpu|auto --model <ggml> --image <png>\n"
        "                  [--point-x N --point-y N] [--box x0,y0,x1,y1]\n"
        "                  [--frames-dir DIR --frames N] [--frame-prefix frame_]\n"
        "                  [--out PREFIX] [--dll sam3_cuda.dll]\n"
        "  sam3_cuda_smoke --iou <mask_a.png> <mask_b.png>\n");
}

std::string frame_path(const std::string &dir, const std::string &prefix, int i)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%s%03d.png", prefix.c_str(), i);
    return dir + "\\" + name;
}

} // namespace

int main(int argc, char **argv)
{
    std::string dll = "sam3_cuda.dll";
    std::string model = "D:\\_SATIN_AI\\models\\sam3-f16.ggml";
    std::string image_path;
    std::string frames_dir;
    std::string frame_prefix = "frame_";
    std::string out_prefix = "smoke";
    std::string mode = "gpu";
    float point_x = 315.0f;
    float point_y = 250.0f;
    bool use_box = false;
    Sam3cBox box = {0, 0, 0, 0};
    int frames = 0;

    if (argc >= 4 && std::strcmp(argv[1], "--iou") == 0)
        return iou_mode(argv[2], argv[3]);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--dll") dll = next("--dll");
        else if (arg == "--model") model = next("--model");
        else if (arg == "--image") image_path = next("--image");
        else if (arg == "--frames-dir") frames_dir = next("--frames-dir");
        else if (arg == "--frame-prefix") frame_prefix = next("--frame-prefix");
        else if (arg == "--frames") frames = std::atoi(next("--frames"));
        else if (arg == "--point-x") point_x = float(std::atof(next("--point-x")));
        else if (arg == "--point-y") point_y = float(std::atof(next("--point-y")));
        else if (arg == "--box") {
            const char *v = next("--box");
            if (std::sscanf(v, "%f,%f,%f,%f", &box.x0, &box.y0, &box.x1,
                            &box.y1) != 4) {
                std::fprintf(stderr, "bad --box: %s\n", v);
                return 2;
            }
            use_box = true;
        }
        else if (arg == "--out") out_prefix = next("--out");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--help" || arg == "-h") { print_usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", arg.c_str()); print_usage(); return 2; }
    }

    HMODULE module = LoadLibraryA(dll.c_str());
    if (!module) {
        std::fprintf(stderr, "{\"stage\":\"dll\",\"ok\":false,\"dll\":\"%s\","
                            "\"win32_error\":%lu}\n", dll.c_str(), GetLastError());
        return 3;
    }
    auto get_api = reinterpret_cast<Sam3cGetApiFn>(
        GetProcAddress(module, SAM3C_GET_API_SYMBOL));
    if (!get_api) {
        std::fprintf(stderr, "sam3_cuda: missing %s\n", SAM3C_GET_API_SYMBOL);
        return 3;
    }
    const Sam3cApi *api = get_api();
    if (!api || api->abi_version != SAM3C_ABI_VERSION ||
        api->struct_size != sizeof(Sam3cApi)) {
        std::fprintf(stderr, "sam3_cuda: ABI mismatch (got %u/%u, want %u/%zu)\n",
                     api ? api->abi_version : 0,
                     api ? api->struct_size : 0,
                     SAM3C_ABI_VERSION, sizeof(Sam3cApi));
        return 3;
    }
    std::printf("{\"stage\":\"dll\",\"ok\":true,\"version\":\"%s\","
                "\"abi\":%u}\n", api->version(), api->abi_version);

    const bool want_gpu = (mode == "gpu" || mode == "auto");
    const bool require_gpu = (mode == "gpu");
    char err[512] = {};

    const auto load_start = Clock::now();
    Sam3cContext *ctx = api->create_context(model.c_str(), 8, want_gpu ? 1 : 0,
                                            require_gpu ? 1 : 0, 0,
                                            err, sizeof(err));
    const double load_ms = elapsed_ms(load_start);
    if (!ctx) {
        std::fprintf(stderr, "{\"stage\":\"load\",\"ok\":false,\"error\":\"%s\"}\n", err);
        return 4;
    }

    const bool is_gpu = api->backend_is_gpu(ctx) != 0;
    std::printf("{\"stage\":\"load\",\"ok\":true,\"mode\":\"%s\","
                "\"backend\":\"%s\",\"gpu\":%s,\"device\":\"%s\","
                "\"vram_total_mb\":%d,\"vram_used_mb\":%d,"
                "\"model_vram_mb\":%d,\"ms\":%.1f}\n",
                mode.c_str(), api->backend_name(ctx),
                is_gpu ? "true" : "false", api->device_name(ctx),
                api->vram_total_mb(ctx), api->vram_used_mb(ctx),
                api->model_vram_mb(ctx), load_ms);

    if ((mode == "gpu") && !is_gpu) {
        std::fprintf(stderr, "{\"stage\":\"load\",\"ok\":false,"
                             "\"error\":\"GPU mode did not load a GPU backend\"}\n");
        api->destroy_context(ctx);
        return 5;
    }

    if (image_path.empty()) {
        std::fprintf(stderr, "no --image provided\n");
        api->destroy_context(ctx);
        return 2;
    }

    Image image;
    if (!load_image(image_path, image)) {
        std::fprintf(stderr, "cannot load image: %s\n", image_path.c_str());
        api->destroy_context(ctx);
        return 2;
    }

    const auto encode_start = Clock::now();
    const int32_t encoded = api->encode_image(ctx, image.rgb.data(), image.width,
                                              image.height, image.width * 3,
                                              err, sizeof(err));
    const double encode_ms = elapsed_ms(encode_start);
    if (!encoded) {
        std::fprintf(stderr, "{\"stage\":\"encode\",\"ok\":false,\"error\":\"%s\"}\n", err);
        api->destroy_context(ctx);
        return 6;
    }
    std::printf("{\"stage\":\"encode\",\"ok\":true,\"width\":%d,\"height\":%d,"
                "\"ms\":%.1f}\n", image.width, image.height, encode_ms);

    Sam3cPoint point = { point_x, point_y };
    Sam3cResult seg{};
    const auto seg_start = Clock::now();
    const int32_t segmented = api->segment_pvs(ctx, &point, 1, nullptr, 0,
                                               use_box ? &box : nullptr,
                                               use_box ? 1 : 0, 1,
                                               &seg, err, sizeof(err));
    const double seg_ms = elapsed_ms(seg_start);
    if (!segmented) {
        std::fprintf(stderr, "{\"stage\":\"segment\",\"ok\":false,\"error\":\"%s\"}\n", err);
        api->destroy_context(ctx);
        return 7;
    }

    std::printf("{\"stage\":\"segment\",\"ok\":true,\"count\":%d,\"ms\":%.1f}\n",
                seg.count, seg_ms);
    Mask seg_mask;
    int32_t best_index = -1;
    for (int32_t i = 0; i < seg.count; ++i) {
        const Sam3cDetection &det = seg.detections[i];
        std::printf("{\"stage\":\"detection\",\"index\":%d,\"score\":%.4f,"
                    "\"iou_score\":%.4f,\"instance\":%d,\"mask\":\"%dx%d\","
                    "\"token\":%d}\n", i, det.score, det.iou_score,
                    det.instance_id, det.mask_width, det.mask_height,
                    det.token_length);
        if (best_index < 0 || det.score > seg.detections[best_index].score)
            best_index = i;
    }
    if (best_index >= 0) {
        seg_mask = copy_mask(seg.detections[best_index]);
        save_mask(out_prefix + "_seg.png", seg.detections[best_index]);
    }

    if (!frames_dir.empty() && frames > 0) {
        if (!api->create_visual_tracker(ctx, 0.1f, 30, 16, 16, err, sizeof(err))) {
            std::fprintf(stderr, "{\"stage\":\"tracker\",\"ok\":false,\"error\":\"%s\"}\n", err);
            api->free_result(&seg);
            api->destroy_context(ctx);
            return 8;
        }
        int32_t instance_id = -1;
        if (!api->tracker_add_instance(ctx, &point, 1, nullptr, 0,
                                       use_box ? &box : nullptr,
                                       use_box ? 1 : 0, &instance_id,
                                       err, sizeof(err))) {
            std::fprintf(stderr, "{\"stage\":\"add_instance\",\"ok\":false,"
                                 "\"error\":\"%s\"}\n", err);
            api->free_result(&seg);
            api->destroy_context(ctx);
            return 9;
        }
        std::printf("{\"stage\":\"add_instance\",\"ok\":true,\"instance\":%d}\n",
                    instance_id);

        Mask previous = seg_mask;
        for (int f = 1; f < frames; ++f) {
            Image frame;
            const std::string path = frame_path(frames_dir, frame_prefix, f);
            if (!load_image(path, frame)) {
                std::fprintf(stderr, "{\"stage\":\"track\",\"frame\":%d,"
                                     "\"ok\":false,\"error\":\"cannot load %s\"}\n",
                             f, path.c_str());
                break;
            }
            Sam3cResult tracked{};
            const auto track_start = Clock::now();
            const int32_t ok = api->propagate_frame(ctx, frame.rgb.data(),
                                                    frame.width, frame.height,
                                                    frame.width * 3, &tracked,
                                                    err, sizeof(err));
            const double track_ms = elapsed_ms(track_start);
            if (!ok) {
                std::fprintf(stderr, "{\"stage\":\"track\",\"frame\":%d,"
                                     "\"ok\":false,\"error\":\"%s\"}\n", f, err);
                break;
            }
            double iou = -1.0;
            Mask current;
            const Sam3cDetection *ours = nullptr;
            for (int32_t i = 0; i < tracked.count; ++i) {
                if (tracked.detections[i].instance_id == instance_id) {
                    ours = &tracked.detections[i];
                    break;
                }
            }
            if (!ours && tracked.count > 0)
                ours = &tracked.detections[0];
            if (ours) {
                current = copy_mask(*ours);
                iou = mask_iou(previous, current);
                char name[64];
                std::snprintf(name, sizeof(name), "_track_%03d.png", f);
                save_mask(out_prefix + name, *ours);
            }
            std::printf("{\"stage\":\"track\",\"frame\":%d,\"ok\":true,"
                        "\"count\":%d,\"iou_prev\":%.4f,\"ms\":%.1f}\n",
                        f, tracked.count, iou, track_ms);
            if (!current.pixels.empty())
                previous = std::move(current);
            api->free_result(&tracked);
        }
    }

    api->free_result(&seg);
    api->destroy_context(ctx);
    FreeLibrary(module);
    return 0;
}
