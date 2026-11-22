/*****************************************************************************
 * matting_cuda_api.h : narrow C ABI for the MSVC/ONNX Runtime matting runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The VLC application is built with MinGW (GCC). ONNX Runtime's CUDA provider
 * on Windows is an MSVC build, so neural matting lives in a separate native
 * DLL:
 *
 *   libqt_plugin.dll (MinGW) --LoadLibrary--> pb_matting_cuda.dll (MSVC + ORT)
 *
 * Nothing C++ crosses this boundary. Handles are opaque, images are raw
 * buffers, the caller owns the output alpha buffer, and every call has an
 * error buffer. C++ objects, STL containers, ORT sessions and exceptions stay
 * entirely inside pb_matting_cuda.dll.
 *
 * ABI NOTE: this header is shared by the MSVC runtime and the MinGW loader.
 * Keep it C99-compatible with fixed-width integer types on every field.
 *****************************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PBMC_ABI_VERSION 1u
#define PBMC_GET_API_SYMBOL "pbmc_get_api"

#if defined(_WIN32)
# if defined(PBMC_BUILD_DLL)
#  define PBMC_API __declspec(dllexport)
# else
#  define PBMC_API
# endif
#else
# define PBMC_API
#endif

/* Return codes. 0 is success; every failure has a specific code so the UI can
 * distinguish "no GPU" from "bad model" from "cancelled". */
enum PbmcStatus
{
    PBMC_OK = 0,
    PBMC_ERR_ARGUMENT = 1,
    PBMC_ERR_DLL = 2,        /* runtime dependency unavailable                */
    PBMC_ERR_MODEL = 3,      /* ONNX graph or manifest invalid               */
    PBMC_ERR_DEVICE = 4,     /* requested GPU backend cannot be created      */
    PBMC_ERR_INFERENCE = 5,  /* Run() failed                                 */
    PBMC_ERR_CANCELLED = 6,  /* cancelled through cancel_current()           */
    PBMC_ERR_UNSUPPORTED = 7 /* capability not implemented by this build     */
};

/* Device contract, mirrors the SAM3 tier semantics. */
enum PbmcDevice
{
    PBMC_DEVICE_AUTO = 0, /* GPU if usable, explicit CPU fallback allowed   */
    PBMC_DEVICE_GPU = 1,  /* GPU required; never silently run on CPU        */
    PBMC_DEVICE_CPU = 2   /* CPU explicitly selected                        */
};

typedef struct PbmcSession PbmcSession;

typedef struct PbmcCapabilities
{
    uint32_t struct_size;             /* caller sets to sizeof(PbmcCapabilities) */
    int32_t  ort_available;           /* ONNX Runtime loaded                     */
    int32_t  cuda_provider_available; /* CUDAExecutionProvider is registered      */
    int32_t  cuda_device_count;       /* adapters reported by the CUDA driver     */
    int32_t  cpu_fallback_available;  /* CPUExecutionProvider is registered      */
    char     ort_version[64];
    char     device_name[160];        /* first CUDA adapter, empty if none       */
    char     error[512];
} PbmcCapabilities;

typedef struct PbmcSessionConfig
{
    uint32_t struct_size;
    const char *model_path;      /* ONNX graph for `preset_size`                */
    const char *manifest_path;   /* manifest.json (provenance + preprocessing)  */
    const char *profile_dir;     /* optional ORT profiling output directory     */
    int32_t device;              /* PbmcDevice                                  */
    int32_t preset_size;         /* square graph size, e.g. 512 or 1024         */
    int32_t intra_threads;       /* CPU threads, 0 = ORT default                */
    /* 1 = GPU mode fails when the graph's compute nodes were not assigned to
     * the CUDA EP (a registered provider does not prove operator placement). */
    int32_t require_gpu_operators;
} PbmcSessionConfig;

/* One strided image. `data` is owned by the caller and must stay alive for the
 * duration of the call. RGB is 3 bytes per pixel; trimap is 1 byte per pixel
 * with values 0 (background), 128 (unknown), 255 (foreground). */
typedef struct PbmcImage
{
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride; /* bytes per row, >= width * channels                    */
} PbmcImage;

/* Region of interest in source-image pixels. An empty (0) width/height means
 * the whole image. The runtime letterboxes this ROI into the fixed model
 * square; it never distorts the subject. */
typedef struct PbmcRoi
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} PbmcRoi;

typedef struct PbmcRunInfo
{
    uint32_t struct_size;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    double total_ms;
    int32_t input_width;   /* model-space square size                         */
    int32_t input_height;
    int32_t roi_x;
    int32_t roi_y;
    int32_t roi_w;
    int32_t roi_h;
    int32_t used_gpu;      /* 1 = the CUDA EP performed the inference         */
    int32_t cuda_node_count;   /* graph nodes assigned to the CUDA EP         */
    int32_t cpu_node_count;    /* graph nodes left on the CPU EP              */
    int32_t device_index;
    char backend[96];      /* e.g. "CUDAExecutionProvider (NVIDIA RTX 4090)"  */
    char error[512];
} PbmcRunInfo;

typedef struct PbmcApi
{
    uint32_t abi_version;
    uint32_t struct_size;

    const char *(*version)(void);

    /* Probe the runtime and CUDA readiness WITHOUT loading a model. Safe from
     * the GUI thread (it only inspects loaded libraries / driver state). */
    int32_t (*probe_capabilities)(PbmcCapabilities *out);

    /* Create a persistent session (the model stays resident; never reload per
     * frame). `out_session` receives an opaque handle on success. */
    int32_t (*create_session)(const PbmcSessionConfig *config,
                              PbmcSession **out_session,
                              char *err, int32_t err_len);

    void (*destroy_session)(PbmcSession *session);

    /* Actual backend chosen by the session. `out->struct_size` must be set. */
    int32_t (*get_backend_info)(PbmcSession *session, PbmcRunInfo *out);

    /* Run one refinement. `out_alpha` is caller-allocated with at least
     * roi.width * roi.height floats; values are written in [0,1] row-major.
     * `roi` may be NULL (whole image). `info` may be NULL. */
    int32_t (*refine_frame)(PbmcSession *session,
                            const PbmcImage *rgb,
                            const PbmcImage *trimap,
                            const PbmcRoi *roi,
                            float *out_alpha,
                            int64_t out_alpha_capacity,
                            PbmcRunInfo *info,
                            char *err, int32_t err_len);

    /* Ask a Run() that is currently executing on another thread to stop.
     * Returns immediately; the blocked refine_frame() returns
     * PBMC_ERR_CANCELLED. Safe to call when nothing is running. */
    void (*cancel_current)(PbmcSession *session);

    const char *(*get_last_error)(PbmcSession *session);
} PbmcApi;

typedef const PbmcApi *(*PbmcGetApiFn)(void);

/* Exported by pb_matting_cuda.dll; also resolvable through GetProcAddress so
 * a missing DLL stays a recoverable condition for the application. */
PBMC_API const PbmcApi *pbmc_get_api(void);

#ifdef __cplusplus
}
#endif
