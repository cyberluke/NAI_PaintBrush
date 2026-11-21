/*****************************************************************************
 * sam3_cuda_bridge.cpp : MSVC/CUDA implementation of the narrow C ABI
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Built with MSVC + CUDA against the greenjava/sam3.cpp `cuda` fork and its
 * pinned ggml `sam3-ops` submodule. Everything C++ (STL, shared_ptr,
 * unique_ptr, CUDA runtime) stays inside this DLL. The VLC plugin only sees
 * the POD API in sam3_cuda_api.h and opaque context handles.
 *****************************************************************************/

#include "sam3_cuda_api.h"

#include "sam3.h"

#include <cuda_runtime.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Sam3cContext {
    std::shared_ptr<sam3_model> model;
    sam3_state_ptr              state;
    sam3_tracker_ptr            tracker;
    std::mutex                  mutex;

    std::string backend = "none";
    std::string device  = "unknown";
    int32_t     vram_used_mb  = 0;
    int32_t     vram_total_mb = 0;
    int32_t     model_vram_mb = 0;
};

namespace {

void set_error(char *err, int32_t err_len, const char *fmt, ...)
{
    if (!err || err_len <= 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, size_t(err_len), fmt, ap);
    va_end(ap);
    err[err_len - 1] = '\0';
}

bool require_context(Sam3cContext *ctx, char *err, int32_t err_len)
{
    if (!ctx) {
        set_error(err, err_len, "SAM3 CUDA context is null");
        return false;
    }
    if (!ctx->model || !ctx->state) {
        set_error(err, err_len, "SAM3 CUDA context is not loaded");
        return false;
    }
    return true;
}

void free_detection(Sam3cDetection &det)
{
    std::free(det.mask);
    std::free(det.token);
    det.mask = nullptr;
    det.token = nullptr;
}

void free_result_internal(Sam3cDetection *dets, int32_t count)
{
    if (!dets)
        return;
    for (int32_t i = 0; i < count; ++i)
        free_detection(dets[i]);
    std::free(dets);
}

bool fill_image(sam3_image &img, const uint8_t *rgb, int32_t width,
                int32_t height, int32_t stride, std::string &error)
{
    if (!rgb || width <= 0 || height <= 0 || stride < width * 3) {
        error = "invalid RGB buffer (need width>0, height>0, stride>=width*3)";
        return false;
    }
    img.width = width;
    img.height = height;
    img.channels = 3;
    img.data.resize(size_t(width) * size_t(height) * 3);
    const size_t row_bytes = size_t(width) * 3;
    for (int32_t y = 0; y < height; ++y)
        std::memcpy(img.data.data() + size_t(y) * row_bytes,
                    rgb + size_t(y) * size_t(stride), row_bytes);
    return true;
}

bool copy_result(const sam3_result &src, Sam3cResult *out, std::string &error)
{
    if (!out) {
        error = "null Sam3cResult";
        return false;
    }
    out->count = 0;
    out->detections = nullptr;
    if (src.detections.empty())
        return true;

    auto *dets = static_cast<Sam3cDetection *>(
        std::calloc(src.detections.size(), sizeof(Sam3cDetection)));
    if (!dets) {
        error = "out of memory allocating detections";
        return false;
    }

    for (size_t i = 0; i < src.detections.size(); ++i) {
        const sam3_detection &d = src.detections[i];
        Sam3cDetection &o = dets[i];
        o.box = { d.box.x0, d.box.y0, d.box.x1, d.box.y1 };
        o.score = d.score;
        o.iou_score = d.iou_score;
        o.instance_id = d.instance_id;
        o.mask_width = d.mask.width;
        o.mask_height = d.mask.height;

        const size_t mask_bytes = size_t(d.mask.width) * size_t(d.mask.height);
        if (d.mask.width > 0 && d.mask.height > 0 &&
            d.mask.data.size() == mask_bytes) {
            auto *mask = static_cast<uint8_t *>(std::malloc(mask_bytes));
            if (!mask) {
                free_result_internal(dets, int32_t(src.detections.size()));
                error = "out of memory copying a mask";
                return false;
            }
            std::memcpy(mask, d.mask.data.data(), mask_bytes);
            o.mask = mask;
        }

        if (!d.sam_token.empty()) {
            const size_t token_bytes = d.sam_token.size() * sizeof(float);
            auto *token = static_cast<float *>(std::malloc(token_bytes));
            if (!token) {
                free_result_internal(dets, int32_t(src.detections.size()));
                error = "out of memory copying a detection token";
                return false;
            }
            std::memcpy(token, d.sam_token.data(), token_bytes);
            o.token = token;
            o.token_length = int32_t(d.sam_token.size());
        }
    }

    out->count = int32_t(src.detections.size());
    out->detections = dets;
    return true;
}

sam3_pvs_params make_pvs(const Sam3cPoint *pos, int32_t n_pos,
                         const Sam3cPoint *neg, int32_t n_neg,
                         const Sam3cBox *box, int32_t use_box,
                         int32_t multimask)
{
    sam3_pvs_params pvs;
    pvs.multimask = multimask != 0;
    pvs.use_box = use_box != 0;
    if (box)
        pvs.box = { box->x0, box->y0, box->x1, box->y1 };
    for (int32_t i = 0; i < n_pos; ++i)
        pvs.pos_points.push_back({ pos[i].x, pos[i].y });
    for (int32_t i = 0; i < n_neg; ++i)
        pvs.neg_points.push_back({ neg[i].x, neg[i].y });
    return pvs;
}

const char *api_version()
{
    return SAM3_VERSION " (sam3_cuda bridge 1)";
}

int32_t probe_gpu(char *device_name, int32_t device_name_len,
                  char *err, int32_t err_len)
{
    try {
        int count = 0;
        const cudaError_t count_err = cudaGetDeviceCount(&count);
        if (count_err != cudaSuccess || count <= 0) {
            set_error(err, err_len, "CUDA device probe failed: %s",
                      cudaGetErrorString(count_err));
            return 0;
        }
        cudaDeviceProp prop;
        const cudaError_t prop_err = cudaGetDeviceProperties(&prop, 0);
        if (prop_err != cudaSuccess) {
            set_error(err, err_len, "CUDA device query failed: %s",
                      cudaGetErrorString(prop_err));
            return 0;
        }
        if (device_name && device_name_len > 0) {
            std::snprintf(device_name, size_t(device_name_len), "%s", prop.name);
            device_name[device_name_len - 1] = '\0';
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "CUDA device probe failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "CUDA device probe failed: unknown exception");
        return 0;
    }
}

Sam3cContext * create_context(const char *model_path, int32_t n_threads,
                              int32_t use_gpu, int32_t require_gpu,
                              int32_t encode_img_size,
                              char *err, int32_t err_len)
{
    try {
        if (!model_path || !*model_path) {
            set_error(err, err_len, "SAM3 model path is empty");
            return nullptr;
        }

        auto ctx = std::make_unique<Sam3cContext>();

        /* Baseline before model allocation. Covers the CUDA context too; the
         * delta is therefore a conservative model+bridge VRAM figure. */
        size_t free_before = 0;
        size_t total = 0;
        if (use_gpu)
            cudaMemGetInfo(&free_before, &total);

        sam3_params params;
        params.model_path = model_path;
        params.n_threads = n_threads > 0 ? n_threads : 4;
        params.use_gpu = use_gpu != 0;
        params.encode_img_size = encode_img_size;

        auto model = sam3_load_model(params);
        if (!model) {
            set_error(err, err_len, "SAM3 model load failed: %s", model_path);
            return nullptr;
        }

        const bool is_gpu = sam3_backend_is_gpu(*model);
        const char *backend = sam3_backend_name(*model);
        if (require_gpu && !is_gpu) {
            set_error(err, err_len,
                      "GPU was requested but SAM3 initialised the %s backend",
                      backend ? backend : "unknown");
            return nullptr;
        }

        auto state = sam3_create_state(*model, params);
        if (!state) {
            set_error(err, err_len,
                      "SAM3 state creation failed on the %s backend",
                      backend ? backend : "unknown");
            return nullptr;
        }

        ctx->model = std::move(model);
        ctx->state = std::move(state);
        ctx->backend = backend ? backend : "unknown";

        if (is_gpu) {
            cudaDeviceProp prop;
            if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess)
                ctx->device = prop.name;
            else
                ctx->device = "CUDA device (name unavailable)";

            size_t free_after = 0;
            cudaMemGetInfo(&free_after, &total);
            ctx->vram_total_mb = int32_t(total / (1024 * 1024));
            ctx->vram_used_mb = int32_t((total - free_after) / (1024 * 1024));
            if (free_before > free_after)
                ctx->model_vram_mb =
                    int32_t((free_before - free_after) / (1024 * 1024));
        } else {
            ctx->device = "CPU";
        }

        return ctx.release();
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 CUDA context failed: %s", e.what());
        return nullptr;
    } catch (...) {
        set_error(err, err_len, "SAM3 CUDA context failed: unknown exception");
        return nullptr;
    }
}

void destroy_context(Sam3cContext *ctx)
{
    delete ctx;
}

const char *backend_name(Sam3cContext *ctx)
{
    return ctx ? ctx->backend.c_str() : "none";
}

int32_t backend_is_gpu(Sam3cContext *ctx)
{
    if (!ctx)
        return 0;
    return (ctx->backend.find("CUDA") != std::string::npos ||
            ctx->backend.find("Vulkan") != std::string::npos ||
            ctx->backend.find("Metal") != std::string::npos ||
            ctx->backend.find("SYCL") != std::string::npos) ? 1 : 0;
}

const char *device_name(Sam3cContext *ctx)
{
    return ctx ? ctx->device.c_str() : "unknown";
}

int32_t vram_used_mb(Sam3cContext *ctx)
{
    return ctx ? ctx->vram_used_mb : 0;
}

int32_t vram_total_mb(Sam3cContext *ctx)
{
    return ctx ? ctx->vram_total_mb : 0;
}

int32_t model_vram_mb(Sam3cContext *ctx)
{
    return ctx ? ctx->model_vram_mb : 0;
}

int32_t encode_image(Sam3cContext *ctx, const uint8_t *rgb, int32_t width,
                     int32_t height, int32_t stride,
                     char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;

        sam3_image image;
        std::string error;
        if (!fill_image(image, rgb, width, height, stride, error)) {
            set_error(err, err_len, "%s", error.c_str());
            return 0;
        }

        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (!sam3_encode_image(*ctx->state, *ctx->model, image)) {
            set_error(err, err_len, "SAM3 frame encoding failed on %s",
                      ctx->backend.c_str());
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 encode failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 encode failed: unknown exception");
        return 0;
    }
}

int32_t segment_pvs(Sam3cContext *ctx,
                    const Sam3cPoint *pos, int32_t n_pos,
                    const Sam3cPoint *neg, int32_t n_neg,
                    const Sam3cBox *box, int32_t use_box,
                    int32_t multimask,
                    Sam3cResult *out, char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;
        if ((n_pos > 0 && !pos) || (n_neg > 0 && !neg) || (use_box && !box)) {
            set_error(err, err_len, "invalid PVS prompt buffers");
            return 0;
        }

        sam3_pvs_params pvs = make_pvs(pos, n_pos, neg, n_neg, box, use_box,
                                       multimask);
        sam3_result result;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            result = sam3_segment_pvs(*ctx->state, *ctx->model, pvs);
        }

        std::string error;
        if (!copy_result(result, out, error)) {
            set_error(err, err_len, "%s", error.c_str());
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 PVS failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 PVS failed: unknown exception");
        return 0;
    }
}

int32_t segment_pcs(Sam3cContext *ctx, const char *text_prompt,
                    const Sam3cBox *pos, int32_t n_pos,
                    const Sam3cBox *neg, int32_t n_neg,
                    float score_threshold, float nms_threshold,
                    Sam3cResult *out, char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;
        if ((n_pos > 0 && !pos) || (n_neg > 0 && !neg)) {
            set_error(err, err_len, "invalid PCS exemplar buffers");
            return 0;
        }

        sam3_pcs_params pcs;
        pcs.text_prompt = text_prompt ? text_prompt : "";
        pcs.score_threshold = score_threshold;
        pcs.nms_threshold = nms_threshold;
        for (int32_t i = 0; i < n_pos; ++i)
            pcs.pos_exemplars.push_back({ pos[i].x0, pos[i].y0,
                                          pos[i].x1, pos[i].y1 });
        for (int32_t i = 0; i < n_neg; ++i)
            pcs.neg_exemplars.push_back({ neg[i].x0, neg[i].y0,
                                          neg[i].x1, neg[i].y1 });

        sam3_result result;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            result = sam3_segment_pcs(*ctx->state, *ctx->model, pcs);
        }

        std::string error;
        if (!copy_result(result, out, error)) {
            set_error(err, err_len, "%s", error.c_str());
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 PCS failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 PCS failed: unknown exception");
        return 0;
    }
}

int32_t create_visual_tracker(Sam3cContext *ctx, float assoc_iou,
                              int32_t max_keep_alive, int32_t recondition_every,
                              int32_t fill_hole_area,
                              char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;

        sam3_visual_track_params vp;
        vp.assoc_iou_threshold = assoc_iou;
        vp.max_keep_alive = max_keep_alive;
        vp.recondition_every = recondition_every;
        vp.fill_hole_area = fill_hole_area;

        std::lock_guard<std::mutex> lock(ctx->mutex);
        auto tracker = sam3_create_visual_tracker(*ctx->model, vp);
        if (!tracker) {
            set_error(err, err_len, "SAM3 visual tracker creation failed on %s",
                      ctx->backend.c_str());
            return 0;
        }
        ctx->tracker = std::move(tracker);
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 tracker creation failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 tracker creation failed: unknown exception");
        return 0;
    }
}

int32_t tracker_add_instance(Sam3cContext *ctx,
                             const Sam3cPoint *pos, int32_t n_pos,
                             const Sam3cPoint *neg, int32_t n_neg,
                             const Sam3cBox *box, int32_t use_box,
                             int32_t *out_instance_id,
                             char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;
        if (!ctx->tracker) {
            set_error(err, err_len, "SAM3 tracker is not created");
            return 0;
        }
        if ((n_pos > 0 && !pos) || (n_neg > 0 && !neg) || (use_box && !box)) {
            set_error(err, err_len, "invalid tracker prompt buffers");
            return 0;
        }

        sam3_pvs_params pvs = make_pvs(pos, n_pos, neg, n_neg, box, use_box, 0);
        int id = -1;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            id = sam3_tracker_add_instance(*ctx->tracker, *ctx->state,
                                           *ctx->model, pvs);
        }
        if (id < 0) {
            set_error(err, err_len, "SAM3 tracker add-instance failed");
            return 0;
        }
        if (out_instance_id)
            *out_instance_id = id;
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 add-instance failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 add-instance failed: unknown exception");
        return 0;
    }
}

int32_t propagate_frame(Sam3cContext *ctx, const uint8_t *rgb, int32_t width,
                        int32_t height, int32_t stride,
                        Sam3cResult *out, char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;
        if (!ctx->tracker) {
            set_error(err, err_len, "SAM3 tracker is not created");
            return 0;
        }

        sam3_image image;
        std::string error;
        if (!fill_image(image, rgb, width, height, stride, error)) {
            set_error(err, err_len, "%s", error.c_str());
            return 0;
        }

        sam3_result result;
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            result = sam3_propagate_frame(*ctx->tracker, *ctx->state,
                                          *ctx->model, image);
        }

        if (!copy_result(result, out, error)) {
            set_error(err, err_len, "%s", error.c_str());
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 propagation failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 propagation failed: unknown exception");
        return 0;
    }
}

int32_t refine_instance(Sam3cContext *ctx, int32_t instance_id,
                        const Sam3cPoint *pos, int32_t n_pos,
                        const Sam3cPoint *neg, int32_t n_neg,
                        char *err, int32_t err_len)
{
    try {
        if (!require_context(ctx, err, err_len))
            return 0;
        if (!ctx->tracker) {
            set_error(err, err_len, "SAM3 tracker is not created");
            return 0;
        }
        if ((n_pos > 0 && !pos) || (n_neg > 0 && !neg)) {
            set_error(err, err_len, "invalid refine prompt buffers");
            return 0;
        }

        std::vector<sam3_point> pos_points;
        std::vector<sam3_point> neg_points;
        pos_points.reserve(size_t(n_pos));
        neg_points.reserve(size_t(n_neg));
        for (int32_t i = 0; i < n_pos; ++i)
            pos_points.push_back({ pos[i].x, pos[i].y });
        for (int32_t i = 0; i < n_neg; ++i)
            neg_points.push_back({ neg[i].x, neg[i].y });

        std::lock_guard<std::mutex> lock(ctx->mutex);
        if (!sam3_refine_instance(*ctx->tracker, *ctx->state, *ctx->model,
                                  instance_id, pos_points, neg_points)) {
            set_error(err, err_len, "SAM3 refine-instance failed");
            return 0;
        }
        return 1;
    } catch (const std::exception &e) {
        set_error(err, err_len, "SAM3 refine failed: %s", e.what());
        return 0;
    } catch (...) {
        set_error(err, err_len, "SAM3 refine failed: unknown exception");
        return 0;
    }
}

void tracker_reset(Sam3cContext *ctx)
{
    if (!ctx || !ctx->tracker)
        return;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    sam3_tracker_reset(*ctx->tracker);
}

int32_t tracker_frame_index(Sam3cContext *ctx)
{
    if (!ctx || !ctx->tracker)
        return -1;
    std::lock_guard<std::mutex> lock(ctx->mutex);
    return sam3_tracker_frame_index(*ctx->tracker);
}

void free_result(Sam3cResult *result)
{
    if (!result)
        return;
    free_result_internal(result->detections, result->count);
    result->detections = nullptr;
    result->count = 0;
}

Sam3cApi make_api()
{
    Sam3cApi api{};
    api.abi_version = SAM3C_ABI_VERSION;
    api.struct_size = sizeof(Sam3cApi);
    api.version = &api_version;
    api.probe_gpu = &probe_gpu;
    api.create_context = &create_context;
    api.destroy_context = &destroy_context;
    api.backend_name = &backend_name;
    api.backend_is_gpu = &backend_is_gpu;
    api.device_name = &device_name;
    api.vram_used_mb = &vram_used_mb;
    api.vram_total_mb = &vram_total_mb;
    api.model_vram_mb = &model_vram_mb;
    api.encode_image = &encode_image;
    api.segment_pvs = &segment_pvs;
    api.segment_pcs = &segment_pcs;
    api.create_visual_tracker = &create_visual_tracker;
    api.tracker_add_instance = &tracker_add_instance;
    api.propagate_frame = &propagate_frame;
    api.refine_instance = &refine_instance;
    api.tracker_reset = &tracker_reset;
    api.tracker_frame_index = &tracker_frame_index;
    api.free_result = &free_result;
    return api;
}

} // namespace

extern "C" __declspec(dllexport) const Sam3cApi *sam3c_get_api(void)
{
    static const Sam3cApi api = make_api();
    return &api;
}
