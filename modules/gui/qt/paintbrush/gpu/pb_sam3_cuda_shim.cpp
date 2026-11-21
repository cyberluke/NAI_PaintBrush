/*****************************************************************************
 * pb_sam3_cuda_shim.cpp : MinGW implementation of the Paintbrush SAM3 API
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * This file is compiled into libqt_plugin.dll by MinGW. It implements the
 * sam3_* API the Paintbrush services already use, but forwards every call to
 * sam3_cuda.dll (MSVC + CUDA) through the narrow C ABI in sam3_cuda_api.h.
 *
 *   MinGW services -> sam3_* (this file, MinGW STL) -> C ABI -> MSVC/CUDA
 *
 * GPU/auto/CPU is decided here from sam3_params::device_tier. A GPU request
 * never silently becomes CPU: create_context() is asked to require GPU and
 * returns a specific error if CUDA cannot be initialised. AUTO accepts the
 * CPU fallback and reports the actual backend.
 *****************************************************************************/

#include "pb_sam3.h"
#include "sam3_cuda_api.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_api_mutex;
HMODULE g_api_module = nullptr;
const Sam3cApi *g_api = nullptr;
bool g_api_tried = false;
std::string g_api_error;

thread_local std::string t_last_error;

bool shim_debug()
{
    static const bool enabled = std::getenv("SAM3_SHIM_DEBUG") != nullptr;
    return enabled;
}

void set_last_error(const char *fmt, ...)
{
    char buffer[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    t_last_error = buffer;
}

void set_last_error(const std::string &text)
{
    t_last_error = text;
}

std::string module_dir(HMODULE module)
{
    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(module, path, DWORD(sizeof(path)));
    if (len == 0 || len >= sizeof(path))
        return {};
    std::string full(path, path + len);
    const size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : full.substr(0, slash);
}

HMODULE try_load(const std::string &path)
{
    if (path.empty())
        return nullptr;
    /* VLC's plugin host restricts the default search path, so dependencies
     * sitting beside sam3_cuda.dll (ggml*.dll) are not found by LoadLibraryA.
     * LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR makes the loader use the DLL's own
     * directory for its dependency closure. */
    HMODULE module = LoadLibraryExA(
        path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        module = LoadLibraryA(path.c_str());
    return module;
}

bool ensure_api()
{
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (g_api)
        return true;
    if (g_api_tried) {
        set_last_error(g_api_error);
        return false;
    }
    g_api_tried = true;

    const char *override_path = std::getenv("SAM3_CUDA_DLL");
    if (override_path && *override_path)
        g_api_module = try_load(override_path);

    if (!g_api_module) {
        /* VLC's plugin host can restrict the default DLL search path, so a
         * bare name does not necessarily include the exe directory. Resolve
         * the bridge next to paintbrush.exe explicitly. */
        char exe[MAX_PATH] = {};
        const DWORD exe_len = GetModuleFileNameA(nullptr, exe, DWORD(sizeof(exe)));
        if (exe_len > 0 && exe_len < sizeof(exe)) {
            std::string exe_path(exe, exe + exe_len);
            const size_t slash = exe_path.find_last_of("\\/");
            if (slash != std::string::npos)
                g_api_module = try_load(exe_path.substr(0, slash) +
                                        "\\sam3_cuda.dll");
        }
    }

    if (!g_api_module)
        g_api_module = try_load("sam3_cuda.dll");

    if (!g_api_module) {
        /* Also look beside the plugin itself (modules/), not only the exe. */
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&g_api_module), &self)) {
            const std::string dir = module_dir(self);
            if (!dir.empty())
                g_api_module = try_load(dir + "\\sam3_cuda.dll");
        }
    }

    if (!g_api_module) {
        const DWORD err = GetLastError();
        g_api_error = "sam3_cuda.dll not found (Win32 error " +
                      std::to_string(unsigned(err)) +
                      "). Place it next to vlc.exe with its ggml DLLs.";
        set_last_error(g_api_error);
        std::fprintf(stderr, "sam3_shim: %s\n", g_api_error.c_str());
        return false;
    }

    auto get_api = reinterpret_cast<Sam3cGetApiFn>(
        GetProcAddress(g_api_module, SAM3C_GET_API_SYMBOL));
    if (!get_api) {
        g_api_error = "sam3_cuda.dll does not export " SAM3C_GET_API_SYMBOL;
        set_last_error(g_api_error);
        std::fprintf(stderr, "sam3_shim: %s\n", g_api_error.c_str());
        return false;
    }

    const Sam3cApi *api = get_api();
    if (!api || api->abi_version != SAM3C_ABI_VERSION ||
        api->struct_size != sizeof(Sam3cApi)) {
        g_api_error = "sam3_cuda.dll ABI mismatch";
        set_last_error(g_api_error);
        std::fprintf(stderr, "sam3_shim: %s\n", g_api_error.c_str());
        return false;
    }

    g_api = api;
    std::fprintf(stderr, "sam3_shim: loaded sam3_cuda.dll (%s)\n",
                 api->version());
    return true;
}

std::mutex g_probe_mutex;
bool g_probe_tried = false;
bool g_probe_ok = false;
std::string g_probe_device;
std::string g_probe_error;

void run_probe()
{
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    if (g_probe_tried)
        return;
    g_probe_tried = true;

    if (!ensure_api()) {
        g_probe_error = t_last_error;
        return;
    }

    char device[256] = {};
    char error[512] = {};
    if (g_api->probe_gpu(device, sizeof(device), error, sizeof(error))) {
        g_probe_ok = true;
        g_probe_device = device;
    } else {
        g_probe_error = error[0] ? error : "CUDA device probe failed";
    }
}

bool is_gpu_backend(const char *name)
{
    if (!name)
        return false;
    return std::strstr(name, "CUDA") != nullptr ||
           std::strstr(name, "Vulkan") != nullptr ||
           std::strstr(name, "Metal") != nullptr ||
           std::strstr(name, "SYCL") != nullptr;
}

void copy_result(const Sam3cResult &src, sam3_result &out)
{
    out.detections.clear();
    out.detections.reserve(size_t(src.count));
    for (int32_t i = 0; i < src.count; ++i) {
        const Sam3cDetection &s = src.detections[i];
        sam3_detection d;
        d.box = { s.box.x0, s.box.y0, s.box.x1, s.box.y1 };
        d.score = s.score;
        d.iou_score = s.iou_score;
        d.instance_id = s.instance_id;
        d.mask.width = s.mask_width;
        d.mask.height = s.mask_height;
        d.mask.iou_score = s.iou_score;
        d.mask.obj_score = s.score;
        d.mask.instance_id = s.instance_id;
        if (s.mask && s.mask_width > 0 && s.mask_height > 0) {
            const size_t n = size_t(s.mask_width) * size_t(s.mask_height);
            d.mask.data.assign(s.mask, s.mask + n);
        }
        if (s.token && s.token_length > 0) {
            d.sam_token.assign(s.token, s.token + s.token_length);
        }
        out.detections.push_back(std::move(d));
    }
}

Sam3cBox c_box(const sam3_box &box)
{
    return { box.x0, box.y0, box.x1, box.y1 };
}

std::vector<Sam3cPoint> c_points(const std::vector<sam3_point> &points)
{
    std::vector<Sam3cPoint> out;
    out.reserve(points.size());
    for (const sam3_point &p : points)
        out.push_back({ p.x, p.y });
    return out;
}

} // namespace

struct sam3_model {
    Sam3cContext *ctx = nullptr;
    std::string backend = "none";
    std::string device = "unknown";
    bool gpu = false;

    ~sam3_model()
    {
        if (ctx && g_api)
            g_api->destroy_context(ctx);
        ctx = nullptr;
    }
};

struct sam3_state {
    int unused = 0;
};

struct sam3_tracker {
    Sam3cContext *ctx = nullptr;
    int frame_index = 0;
};

void sam3_state_deleter::operator()(sam3_state *p) const
{
    if (p) {
        sam3_free_state(*p);
        delete p;
    }
}

void sam3_tracker_deleter::operator()(sam3_tracker *p) const
{
    delete p;
}

std::shared_ptr<sam3_model> sam3_load_model(const sam3_params &params)
{
    t_last_error.clear();
    if (params.model_path.empty()) {
        set_last_error("SAM3 model path is empty");
        return nullptr;
    }
    if (!ensure_api())
        return nullptr;

    const sam3_device_tier tier = params.device_tier;
    const int use_gpu = (tier != SAM3_DEVICE_CPU) ? 1 : 0;
    const int require_gpu = (tier == SAM3_DEVICE_GPU) ? 1 : 0;

    char error[512] = {};
    Sam3cContext *ctx = g_api->create_context(
        params.model_path.c_str(),
        params.n_threads,
        use_gpu,
        require_gpu,
        params.encode_img_size,
        error,
        sizeof(error));
    if (!ctx) {
        set_last_error(error[0] ? error : "SAM3 model load failed");
        std::fprintf(stderr, "sam3_shim: load failed (tier=%d): %s\n",
                     int(tier), t_last_error.c_str());
        return nullptr;
    }

    auto model = std::make_shared<sam3_model>();
    model->ctx = ctx;
    model->backend = g_api->backend_name(ctx) ? g_api->backend_name(ctx) : "unknown";
    model->device = g_api->device_name(ctx) ? g_api->device_name(ctx) : "unknown";
    model->gpu = g_api->backend_is_gpu(ctx) != 0;

    std::fprintf(stderr,
                 "sam3_shim: loaded model requested=%s actual=%s gpu=%d "
                 "device='%s' modelVram=%dMB\n",
                 tier == SAM3_DEVICE_GPU ? "gpu" :
                 tier == SAM3_DEVICE_CPU ? "cpu" : "auto",
                 model->backend.c_str(), model->gpu ? 1 : 0,
                 model->device.c_str(), g_api->model_vram_mb(ctx));

    if (tier == SAM3_DEVICE_AUTO && !model->gpu) {
        std::fprintf(stderr,
                     "sam3_shim: AUTO fell back to CPU (GPU not initialised)\n");
    }

    return model;
}

void sam3_free_model(sam3_model &model)
{
    if (model.ctx && g_api)
        g_api->destroy_context(model.ctx);
    model.ctx = nullptr;
}

bool sam3_is_visual_only(const sam3_model &)
{
    return false;
}

sam3_model_type sam3_get_model_type(const sam3_model &)
{
    return SAM3_MODEL_SAM3;
}

const char *sam3_backend_name(const sam3_model &model)
{
    return model.backend.c_str();
}

bool sam3_backend_is_gpu(const sam3_model &model)
{
    return model.gpu;
}

const char *sam3_device_name(const sam3_model &model)
{
    return model.device.c_str();
}

const char *sam3_last_error()
{
    return t_last_error.c_str();
}

sam3_state_ptr sam3_create_state(const sam3_model &model,
                                 const sam3_params &)
{
    if (!model.ctx)
        return nullptr;
    return sam3_state_ptr(new sam3_state());
}

void sam3_free_state(sam3_state &)
{
}

bool sam3_encode_image(sam3_state &, const sam3_model &model,
                       const sam3_image &image)
{
    if (!model.ctx || image.width <= 0 || image.height <= 0 ||
        image.channels != 3 ||
        image.data.size() < size_t(image.width) * size_t(image.height) * 3) {
        set_last_error("invalid image passed to SAM3 encode");
        return false;
    }

    if (shim_debug())
        std::fprintf(stderr, "sam3_shim: encode %dx%d\n", image.width,
                     image.height);

    char error[512] = {};
    if (!g_api->encode_image(model.ctx, image.data.data(), image.width,
                             image.height, image.width * 3,
                             error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 encode failed");
        return false;
    }
    return true;
}

sam3_result sam3_segment_pcs(sam3_state &, const sam3_model &model,
                             const sam3_pcs_params &params)
{
    sam3_result result;
    if (!model.ctx) {
        set_last_error("SAM3 context is not loaded");
        return result;
    }

    std::vector<Sam3cBox> pos, neg;
    for (const sam3_box &b : params.pos_exemplars)
        pos.push_back(c_box(b));
    for (const sam3_box &b : params.neg_exemplars)
        neg.push_back(c_box(b));

    Sam3cResult c_result{};
    char error[512] = {};
    if (!g_api->segment_pcs(model.ctx, params.text_prompt.c_str(),
                            pos.empty() ? nullptr : pos.data(), int32_t(pos.size()),
                            neg.empty() ? nullptr : neg.data(), int32_t(neg.size()),
                            params.score_threshold, params.nms_threshold,
                            &c_result, error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 PCS failed");
        return result;
    }
    copy_result(c_result, result);
    g_api->free_result(&c_result);
    if (shim_debug()) {
        for (const sam3_detection &d : result.detections) {
            size_t fg = 0;
            for (uint8_t v : d.mask.data)
                if (v > 127) ++fg;
            std::fprintf(stderr,
                         "sam3_shim: det score=%.4f mask=%dx%d fg=%zu/%zu\n",
                         d.score, d.mask.width, d.mask.height, fg,
                         d.mask.data.size());
        }
    }
    return result;
}

sam3_result sam3_segment_pvs(sam3_state &, const sam3_model &model,
                             const sam3_pvs_params &params)
{
    sam3_result result;
    if (!model.ctx) {
        set_last_error("SAM3 context is not loaded");
        return result;
    }

    const auto pos = c_points(params.pos_points);
    const auto neg = c_points(params.neg_points);
    const Sam3cBox box = c_box(params.box);

    Sam3cResult c_result{};
    char error[512] = {};
    if (!g_api->segment_pvs(model.ctx,
                            pos.empty() ? nullptr : pos.data(), int32_t(pos.size()),
                            neg.empty() ? nullptr : neg.data(), int32_t(neg.size()),
                            params.use_box ? &box : nullptr,
                            params.use_box ? 1 : 0,
                            params.multimask ? 1 : 0,
                            &c_result, error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 PVS failed");
        return result;
    }
    copy_result(c_result, result);
    g_api->free_result(&c_result);
    if (shim_debug()) {
        for (const sam3_detection &d : result.detections) {
            size_t fg = 0;
            for (uint8_t v : d.mask.data)
                if (v > 127) ++fg;
            std::fprintf(stderr,
                         "sam3_shim: det score=%.4f mask=%dx%d fg=%zu/%zu\n",
                         d.score, d.mask.width, d.mask.height, fg,
                         d.mask.data.size());
        }
    }
    return result;
}

sam3_tracker_ptr sam3_create_visual_tracker(
    const sam3_model &model, const sam3_visual_track_params &params)
{
    if (!model.ctx)
        return nullptr;
    char error[512] = {};
    if (!g_api->create_visual_tracker(model.ctx, params.assoc_iou_threshold,
                                      params.max_keep_alive,
                                      params.recondition_every,
                                      params.fill_hole_area,
                                      error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 tracker creation failed");
        return nullptr;
    }
    auto tracker = sam3_tracker_ptr(new sam3_tracker());
    tracker->ctx = model.ctx;
    return tracker;
}

int sam3_tracker_add_instance(sam3_tracker &, sam3_state &,
                              const sam3_model &model,
                              const sam3_pvs_params &params)
{
    if (!model.ctx)
        return -1;
    const auto pos = c_points(params.pos_points);
    const auto neg = c_points(params.neg_points);
    const Sam3cBox box = c_box(params.box);
    int32_t id = -1;
    char error[512] = {};
    if (!g_api->tracker_add_instance(model.ctx,
                                     pos.empty() ? nullptr : pos.data(), int32_t(pos.size()),
                                     neg.empty() ? nullptr : neg.data(), int32_t(neg.size()),
                                     params.use_box ? &box : nullptr,
                                     params.use_box ? 1 : 0,
                                     &id, error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 add-instance failed");
        return -1;
    }
    return int(id);
}

int sam3_tracker_frame_index(const sam3_tracker &tracker)
{
    return tracker.ctx ? int(g_api->tracker_frame_index(tracker.ctx))
                       : tracker.frame_index;
}

void sam3_tracker_reset(sam3_tracker &tracker)
{
    if (tracker.ctx && g_api)
        g_api->tracker_reset(tracker.ctx);
    tracker.frame_index = 0;
}

sam3_result sam3_propagate_frame(sam3_tracker &, sam3_state &,
                                 const sam3_model &model,
                                 const sam3_image &frame)
{
    sam3_result result;
    if (!model.ctx) {
        set_last_error("SAM3 context is not loaded");
        return result;
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.channels != 3 ||
        frame.data.size() < size_t(frame.width) * size_t(frame.height) * 3) {
        set_last_error("invalid image passed to SAM3 propagation");
        return result;
    }

    Sam3cResult c_result{};
    char error[512] = {};
    if (!g_api->propagate_frame(model.ctx, frame.data.data(), frame.width,
                                frame.height, frame.width * 3,
                                &c_result, error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 propagation failed");
        return result;
    }
    copy_result(c_result, result);
    g_api->free_result(&c_result);
    if (shim_debug()) {
        for (const sam3_detection &d : result.detections) {
            size_t fg = 0;
            for (uint8_t v : d.mask.data)
                if (v > 127) ++fg;
            std::fprintf(stderr,
                         "sam3_shim: det score=%.4f mask=%dx%d fg=%zu/%zu\n",
                         d.score, d.mask.width, d.mask.height, fg,
                         d.mask.data.size());
        }
    }
    return result;
}

bool sam3_refine_instance(sam3_tracker &, sam3_state &,
                          const sam3_model &model, int instance_id,
                          const std::vector<sam3_point> &pos_points,
                          const std::vector<sam3_point> &neg_points)
{
    if (!model.ctx)
        return false;
    const auto pos = c_points(pos_points);
    const auto neg = c_points(neg_points);
    char error[512] = {};
    if (!g_api->refine_instance(model.ctx, instance_id,
                                pos.empty() ? nullptr : pos.data(), int32_t(pos.size()),
                                neg.empty() ? nullptr : neg.data(), int32_t(neg.size()),
                                error, sizeof(error))) {
        set_last_error(error[0] ? error : "SAM3 refine failed");
        return false;
    }
    return true;
}

bool pbSamGpuDllPresent()
{
    return ensure_api();
}

bool pbSamGpuDllAvailable()
{
    run_probe();
    return g_probe_ok;
}

const char *pbSamGpuDllDeviceName()
{
    run_probe();
    return g_probe_device.c_str();
}

const char *pbSamGpuDllProbeError()
{
    run_probe();
    return g_probe_error.c_str();
}

void pbSamGpuProbeReset()
{
    /* Keep the DLL loaded: models/contexts may still be alive. This only
     * clears the cached Settings probe so the next open re-queries CUDA. */
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    g_probe_tried = false;
    g_probe_ok = false;
    g_probe_device.clear();
    g_probe_error.clear();
}
