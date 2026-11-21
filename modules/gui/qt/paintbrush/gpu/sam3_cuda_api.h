/*****************************************************************************
 * sam3_cuda_api.h : narrow C ABI for the MSVC/CUDA SAM3 runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The VLC application is built with MinGW (GCC). CUDA on Windows requires the
 * MSVC host compiler, so the CUDA SAM3 runtime is a separate native DLL:
 *
 *   libqt_plugin.dll (MinGW)  --LoadLibrary-->  sam3_cuda.dll (MSVC + CUDA)
 *
 * Nothing C++ crosses this boundary. Handles are opaque, all images are raw
 * RGB buffers, all results are POD arrays allocated by the DLL, and every
 * call has an error buffer. C++ objects, STL containers, CUDA contexts and
 * exceptions stay entirely inside sam3_cuda.dll.
 *
 * ABI NOTE: this header is shared by the MSVC bridge and the MinGW loader
 * (which only uses the function table). Keep it C99-compatible and keep
 * fixed-width integer types on every field.
 *****************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAM3C_ABI_VERSION 1u
#define SAM3C_GET_API_SYMBOL "sam3c_get_api"

typedef struct Sam3cPoint {
    float x;
    float y;
} Sam3cPoint;

typedef struct Sam3cBox {
    float x0;
    float y0;
    float x1;
    float y1;
} Sam3cBox;

/* One detection. `mask` and `token` are owned by the result and released by
 * free_result(); the loader copies what it needs and then frees the result. */
typedef struct Sam3cDetection {
    Sam3cBox box;
    float score;
    float iou_score;
    int32_t instance_id;
    int32_t mask_width;
    int32_t mask_height;
    uint8_t *mask;          /* mask_width * mask_height, values 0 or 255 */
    float *token;           /* token_length floats (may be NULL/0) */
    int32_t token_length;
} Sam3cDetection;

typedef struct Sam3cResult {
    int32_t count;
    Sam3cDetection *detections;
} Sam3cResult;

typedef struct Sam3cContext Sam3cContext;

typedef struct Sam3cApi {
    uint32_t abi_version;
    uint32_t struct_size;

    const char *(*version)(void);

    /* Probe the CUDA runtime without loading a model. Returns 1 and fills
     * device_name when at least one CUDA device is usable; 0 with an error
     * message otherwise. Used by Settings to report the detected adapter. */
    int32_t (*probe_gpu)(char *device_name, int32_t device_name_len,
                         char *err, int32_t err_len);

    /* use_gpu: 1 = try CUDA, 0 = force CPU.
     * require_gpu: 1 = fail when the model did not actually load on a GPU
     * (never silently execute CPU when the user asked for GPU).
     * The effective backend is available via backend_name/backend_is_gpu. */
    Sam3cContext *(*create_context)(const char *model_path, int32_t n_threads,
                                    int32_t use_gpu, int32_t require_gpu,
                                    int32_t encode_img_size,
                                    char *err, int32_t err_len);
    void (*destroy_context)(Sam3cContext *ctx);

    const char *(*backend_name)(Sam3cContext *ctx);
    int32_t (*backend_is_gpu)(Sam3cContext *ctx);
    const char *(*device_name)(Sam3cContext *ctx);
    int32_t (*vram_used_mb)(Sam3cContext *ctx);   /* device-wide, after load */
    int32_t (*vram_total_mb)(Sam3cContext *ctx);
    int32_t (*model_vram_mb)(Sam3cContext *ctx);  /* delta across model load */

    /* strided RGB24 (stride in bytes, >= width * 3) */
    int32_t (*encode_image)(Sam3cContext *ctx, const uint8_t *rgb,
                            int32_t width, int32_t height, int32_t stride,
                            char *err, int32_t err_len);

    int32_t (*segment_pvs)(Sam3cContext *ctx,
                           const Sam3cPoint *pos, int32_t n_pos,
                           const Sam3cPoint *neg, int32_t n_neg,
                           const Sam3cBox *box, int32_t use_box,
                           int32_t multimask,
                           Sam3cResult *out, char *err, int32_t err_len);

    int32_t (*segment_pcs)(Sam3cContext *ctx, const char *text_prompt,
                           const Sam3cBox *pos, int32_t n_pos,
                           const Sam3cBox *neg, int32_t n_neg,
                           float score_threshold, float nms_threshold,
                           Sam3cResult *out, char *err, int32_t err_len);

    int32_t (*create_visual_tracker)(Sam3cContext *ctx, float assoc_iou,
                                     int32_t max_keep_alive,
                                     int32_t recondition_every,
                                     int32_t fill_hole_area,
                                     char *err, int32_t err_len);

    int32_t (*tracker_add_instance)(Sam3cContext *ctx,
                                    const Sam3cPoint *pos, int32_t n_pos,
                                    const Sam3cPoint *neg, int32_t n_neg,
                                    const Sam3cBox *box, int32_t use_box,
                                    int32_t *out_instance_id,
                                    char *err, int32_t err_len);

    int32_t (*propagate_frame)(Sam3cContext *ctx, const uint8_t *rgb,
                               int32_t width, int32_t height, int32_t stride,
                               Sam3cResult *out, char *err, int32_t err_len);

    int32_t (*refine_instance)(Sam3cContext *ctx, int32_t instance_id,
                               const Sam3cPoint *pos, int32_t n_pos,
                               const Sam3cPoint *neg, int32_t n_neg,
                               char *err, int32_t err_len);

    void (*tracker_reset)(Sam3cContext *ctx);
    int32_t (*tracker_frame_index)(Sam3cContext *ctx);

    void (*free_result)(Sam3cResult *result);
} Sam3cApi;

typedef const Sam3cApi *(*Sam3cGetApiFn)(void);

#ifdef __cplusplus
}
#endif
