/*****************************************************************************
 * lama_runtime.h : narrow C ABI for the MSVC/ONNX Runtime LaMa runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The VLC application is built with MinGW (GCC). ONNX Runtime's CUDA provider
 * on Windows is an MSVC build, so native LaMa inpainting lives in a separate
 * native DLL:
 *
 *   libqt_plugin.dll (MinGW) --LoadLibrary--> pb_lama_cuda.dll (MSVC + ORT)
 *
 * Nothing C++ crosses this boundary. Handles are opaque, images are raw
 * buffers, the caller owns the output RGBA buffer, and every call has an
 * error buffer. C++ objects, STL containers, ORT sessions and exceptions stay
 * entirely inside pb_lama_cuda.dll.
 *
 * MODEL CONTRACT (Carve/LaMa-ONNX lama_fp32.onnx, Apache-2.0):
 *   image : float32 [1,3,512,512], RGB in [0,1]
 *   mask  : float32 [1,1,512,512], 1 = masked (hole), 0 = keep
 *   output: float32 [1,3,512,512], the pinned export clamps * 255
 *
 * The graph is a FIXED 512 export. The runtime crops a square context ROI
 * around the mask, resizes it to 512, and composites the decoded result back
 * into the full canvas with an inward feather. Unmasked pixels are preserved
 * byte-for-byte; this is not native-resolution synthesis.
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

#define PBLM_ABI_VERSION 1u
#define PBLM_GET_API_SYMBOL "pblm_get_api"

#if defined(_WIN32)
# if defined(PBLM_BUILD_DLL)
#  define PBLM_API __declspec(dllexport)
# else
#  define PBLM_API
# endif
#else
# define PBLM_API
#endif

/* Return codes. 0 is success; every failure has a specific code so the UI can
 * distinguish "no GPU" from "bad model" from "cancelled". */
enum PblmStatus
{
    PBLM_OK = 0,
    PBLM_ERR_ARGUMENT = 1,
    PBLM_ERR_DLL = 2,        /* runtime dependency unavailable                */
    PBLM_ERR_MODEL = 3,      /* ONNX graph invalid or hash mismatch          */
    PBLM_ERR_DEVICE = 4,     /* requested GPU backend cannot be created      */
    PBLM_ERR_INFERENCE = 5,  /* Run() failed                                 */
    PBLM_ERR_CANCELLED = 6,  /* cancelled through cancel_current()           */
    PBLM_ERR_UNSUPPORTED = 7 /* bounded-memory / capability refusal          */
};

/* Device contract, mirrors the matting / SAM3 tier semantics. */
enum PblmDevice
{
    PBLM_DEVICE_AUTO = 0, /* GPU if usable, explicit CPU fallback allowed   */
    PBLM_DEVICE_GPU = 1,  /* GPU required; never silently run on CPU        */
    PBLM_DEVICE_CPU = 2   /* CPU explicitly selected                        */
};

typedef struct PblmSession PblmSession;

typedef struct PblmCapabilities
{
    uint32_t struct_size;             /* caller sets to sizeof(PblmCapabilities) */
    int32_t  ort_available;           /* ONNX Runtime loaded                     */
    int32_t  cuda_provider_available; /* CUDAExecutionProvider is registered      */
    int32_t  cuda_device_count;       /* adapters reported by the CUDA driver     */
    int32_t  cpu_fallback_available;  /* CPUExecutionProvider is registered      */
    char     ort_version[64];
    char     device_name[160];        /* first CUDA adapter, empty if none       */
    char     error[512];
} PblmCapabilities;

typedef struct PblmSessionConfig
{
    uint32_t struct_size;
    const char *model_path;      /* lama_fp32.onnx                              */
    const char *profile_dir;     /* optional ORT profiling output directory     */
    const char *expected_sha256; /* optional lowercase hex pin; verified on load*/
    int32_t device;              /* PblmDevice                                  */
    int32_t input_size;          /* fixed graph size, must be 512 for this graph*/
    int32_t intra_threads;       /* CPU threads, 0 = ORT default                */
    /* 1 = GPU mode fails when the graph's compute nodes were not assigned to
     * the CUDA EP (a registered provider does not prove operator placement). */
    int32_t require_gpu_operators;
    /* Mask dilation before the graph. -1 = automatic (scaled to the image,
     * 2..16 px). 0 = none (diagnostic parity against the raw export). */
    int32_t mask_grow_px;
} PblmSessionConfig;

/* One strided image. `data` is owned by the caller and must stay alive for the
 * duration of the call. channels is 3 (RGB) or 4 (RGBA); alpha is carried
 * through untouched. */
typedef struct PblmImage
{
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;   /* bytes per row, >= width * channels                    */
    int32_t channels; /* 3 = RGB, 4 = RGBA                                     */
} PblmImage;

/* One 8-bit mask. Any value >= threshold (default 128 when threshold <= 0) is
 * masked. It must have the same dimensions as the image. */
typedef struct PblmMask
{
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;   /* bytes per row, >= width                               */
    int32_t threshold;
} PblmMask;

typedef struct PblmRunInfo
{
    uint32_t struct_size;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    double total_ms;
    int32_t input_size;      /* model-space square size                        */
    int32_t roi_x;           /* context ROI in source pixels                   */
    int32_t roi_y;
    int32_t roi_w;
    int32_t roi_h;
    int32_t components;      /* connected components actually processed        */
    int32_t used_gpu;        /* 1 = the CUDA EP performed the inference        */
    int32_t cuda_node_count; /* graph nodes assigned to the CUDA EP            */
    int32_t cpu_node_count;  /* graph nodes left on the CPU EP                 */
    int32_t device_index;
    char backend[96];        /* e.g. "CUDAExecutionProvider (NVIDIA RTX 4090)"  */
    char model_sha256[65];   /* actual hash of the loaded graph                */
    char error[512];
} PblmRunInfo;

typedef struct PblmApi
{
    uint32_t abi_version;
    uint32_t struct_size;

    const char *(*version)(void);

    /* Probe the runtime and CUDA readiness WITHOUT loading a model. Safe from
     * the GUI thread (it only inspects loaded libraries / driver state). */
    int32_t (*probe_capabilities)(PblmCapabilities *out);

    /* Create a persistent session (the model stays resident; never reload per
     * frame). `out_session` receives an opaque handle on success. */
    int32_t (*create_session)(const PblmSessionConfig *config,
                              PblmSession **out_session,
                              char *err, int32_t err_len);

    void (*destroy_session)(PblmSession *session);

    /* Actual backend chosen by the session. `out->struct_size` must be set. */
    int32_t (*get_backend_info)(PblmSession *session, PblmRunInfo *out);

    /* Inpaint the masked region of `rgb`. `out_rgba` is caller-allocated with
     * at least width * height * 4 bytes; the result is full-canvas RGBA8888,
     * unmasked pixels copied byte-for-byte from the input. `info` may be NULL.
     * Runs synchronously on the calling thread; the service layer keeps it off
     * the GUI thread. */
    int32_t (*inpaint)(PblmSession *session,
                       const PblmImage *rgb,
                       const PblmMask *mask,
                       uint8_t *out_rgba,
                       int64_t out_capacity,
                       PblmRunInfo *info,
                       char *err, int32_t err_len);

    /* Ask a Run() that is currently executing on another thread to stop.
     * Returns immediately; the blocked inpaint() returns PBLM_ERR_CANCELLED.
     * Safe to call when nothing is running. */
    void (*cancel_current)(PblmSession *session);

    const char *(*get_last_error)(PblmSession *session);
} PblmApi;

typedef const PblmApi *(*PblmGetApiFn)(void);

/* Exported by pb_lama_cuda.dll; also resolvable through GetProcAddress so
 * a missing DLL stays a recoverable condition for the application. */
PBLM_API const PblmApi *pblm_get_api(void);

#ifdef __cplusplus
}
#endif
