/*****************************************************************************
 * pb_sam3.h : MinGW-facing SAM3 API implemented by the CUDA bridge shim
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The Paintbrush services were written against the sam3.cpp C++ API. On
 * Windows the CUDA runtime must be an MSVC DLL, so this header keeps the same
 * shape (opaque handles, POD-ish structs, std::vector results) but every call
 * is implemented by pb_sam3_cuda_shim.cpp on the MinGW side, which forwards to
 * sam3_cuda.dll through its narrow C ABI.
 *
 * Device contract:
 *   SAM3_DEVICE_GPU   -> require a GPU backend; fail if CUDA is unavailable
 *   SAM3_DEVICE_AUTO  -> try GPU, report/accept an explicit CPU fallback
 *   SAM3_DEVICE_CPU   -> force CPU
 * The actual backend is always reported by sam3_backend_name()/is_gpu().
 *****************************************************************************/

#ifndef PB_SAM3_H
#define PB_SAM3_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sam3_model;
struct sam3_state;
struct sam3_tracker;

struct sam3_state_deleter   { void operator()(sam3_state * p) const; };
struct sam3_tracker_deleter { void operator()(sam3_tracker * p) const; };

using sam3_state_ptr   = std::unique_ptr<sam3_state,   sam3_state_deleter>;
using sam3_tracker_ptr = std::unique_ptr<sam3_tracker, sam3_tracker_deleter>;

enum sam3_model_type {
    SAM3_MODEL_SAM3        = 0,
    SAM3_MODEL_SAM3_VISUAL = 1,
    SAM3_MODEL_SAM2        = 2,
    SAM3_MODEL_EDGETAM     = 3,
};

struct sam3_point {
    float x;
    float y;
};

struct sam3_box {
    float x0;
    float y0;
    float x1;
    float y1;
};

struct sam3_image {
    int width    = 0;
    int height   = 0;
    int channels = 3;
    std::vector<uint8_t> data;
};

struct sam3_mask {
    int width       = 0;
    int height      = 0;
    float iou_score = 0.0f;
    float obj_score = 0.0f;
    int instance_id = -1;
    std::vector<uint8_t> data;  /* binary 0/255 */
};

struct sam3_detection {
    sam3_box box;
    float score = 0.0f;
    float iou_score = 0.0f;
    int instance_id = -1;
    sam3_mask mask;
    std::vector<float> sam_token;
};

struct sam3_result {
    std::vector<sam3_detection> detections;
};

enum sam3_device_tier {
    SAM3_DEVICE_AUTO = 0,
    SAM3_DEVICE_GPU  = 1,
    SAM3_DEVICE_CPU  = 2,
};

struct sam3_params {
    std::string model_path;
    int n_threads = 4;
    bool use_gpu = true;                      /* kept for source compatibility */
    int seed = 42;
    int encode_img_size = 0;
    sam3_device_tier device_tier = SAM3_DEVICE_AUTO;
};

struct sam3_pcs_params {
    std::string text_prompt;
    std::vector<sam3_box> pos_exemplars;
    std::vector<sam3_box> neg_exemplars;
    float score_threshold = 0.5f;
    float nms_threshold = 0.1f;
};

struct sam3_pvs_params {
    std::vector<sam3_point> pos_points;
    std::vector<sam3_point> neg_points;
    sam3_box box = {0, 0, 0, 0};
    bool use_box = false;
    bool multimask = false;
};

struct sam3_visual_track_params {
    float assoc_iou_threshold = 0.1f;
    int max_keep_alive = 30;
    int recondition_every = 16;
    int fill_hole_area = 16;
};

/* Model lifecycle. A failed load returns nullptr and sam3_last_error() carries
 * the specific reason (DLL missing, CUDA init failure, model parse failure). */
std::shared_ptr<sam3_model> sam3_load_model(const sam3_params & params);
void sam3_free_model(sam3_model & model);
bool sam3_is_visual_only(const sam3_model & model);
sam3_model_type sam3_get_model_type(const sam3_model & model);

/* Actual execution backend selected by the load (never inferred from the
 * requested tier): "CUDA0", "CPU", ... and the adapter name. */
const char * sam3_backend_name(const sam3_model & model);
bool sam3_backend_is_gpu(const sam3_model & model);
const char * sam3_device_name(const sam3_model & model);
const char * sam3_last_error();

sam3_state_ptr sam3_create_state(const sam3_model & model,
                                 const sam3_params & params);
void sam3_free_state(sam3_state & state);

bool sam3_encode_image(sam3_state & state, const sam3_model & model,
                       const sam3_image & image);

sam3_result sam3_segment_pcs(sam3_state & state, const sam3_model & model,
                             const sam3_pcs_params & params);
sam3_result sam3_segment_pvs(sam3_state & state, const sam3_model & model,
                             const sam3_pvs_params & params);

sam3_tracker_ptr sam3_create_visual_tracker(
    const sam3_model & model, const sam3_visual_track_params & params);
int sam3_tracker_add_instance(sam3_tracker & tracker, sam3_state & state,
                              const sam3_model & model,
                              const sam3_pvs_params & pvs_params);
int sam3_tracker_frame_index(const sam3_tracker & tracker);
void sam3_tracker_reset(sam3_tracker & tracker);
sam3_result sam3_propagate_frame(sam3_tracker & tracker, sam3_state & state,
                                 const sam3_model & model,
                                 const sam3_image & frame);
bool sam3_refine_instance(sam3_tracker & tracker, sam3_state & state,
                          const sam3_model & model, int instance_id,
                          const std::vector<sam3_point> & pos_points,
                          const std::vector<sam3_point> & neg_points);

/* Settings capability probe: loads sam3_cuda.dll (once) and asks it for the
 * first CUDA adapter without loading a model. Results are cached; call
 * pbSamGpuProbeReset() after installing a DLL at runtime if needed. */
bool pbSamGpuDllPresent();
bool pbSamGpuDllAvailable();
const char * pbSamGpuDllDeviceName();
const char * pbSamGpuDllProbeError();
void pbSamGpuProbeReset();

#endif /* PB_SAM3_H */
