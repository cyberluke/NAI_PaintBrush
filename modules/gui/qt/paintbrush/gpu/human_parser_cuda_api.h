/*****************************************************************************
 * human_parser_cuda_api.h : narrow C ABI for the MSVC/ONNX Runtime FASHN parser
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * The VLC application is built with MinGW (GCC). ONNX Runtime's CUDA provider
 * on Windows is an MSVC build, so the human parser lives in a separate native
 * DLL, exactly like the neural matting runtime:
 *
 *   libqt_plugin.dll (MinGW) --LoadLibrary--> pb_human_parser_cuda.dll
 *                                                    (MSVC + ONNX Runtime)
 *
 * Nothing C++ crosses this boundary. The session is opaque, images are raw
 * RGB buffers, the caller owns every output buffer, and every call has an
 * error buffer. C++ objects, STL containers, ORT sessions and exceptions stay
 * entirely inside the DLL.
 *
 * SEMANTIC CONTRACT
 *   The parser is a fashion human parser, NOT an object/car segmenter. Its
 *   18 classes describe a person's clothes, hair and skin. On a car it can
 *   only produce meaningless fashion-ish classes; use it on a car solely to
 *   exercise mask composition, never as automotive part segmentation.
 *
 *   The caller owns subject identity. The parser consumes the existing SAM3
 *   subject alpha and supplies independent semantic probabilities. It never
 *   replaces the subject mask and never changes its alpha.
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

#define PBHP_ABI_VERSION 1u
#define PBHP_GET_API_SYMBOL "pbhp_get_api"
#define PBHP_CLASS_COUNT 18
#define PBHP_GROUP_COUNT 6

#if defined(_WIN32)
# if defined(PBHP_BUILD_DLL)
#  define PBHP_API __declspec(dllexport)
# else
#  define PBHP_API
# endif
#else
# define PBHP_API
#endif

/* Return codes. 0 is success; every failure has a specific code so the UI can
 * distinguish "no GPU" from "bad model" from "cancelled". */
enum PbhStatus
{
    PBHP_OK = 0,
    PBHP_ERR_ARGUMENT = 1,
    PBHP_ERR_DLL = 2,        /* runtime dependency unavailable                */
    PBHP_ERR_MODEL = 3,      /* ONNX graph or manifest invalid               */
    PBHP_ERR_DEVICE = 4,     /* requested GPU backend cannot be created      */
    PBHP_ERR_INFERENCE = 5,  /* Run() failed                                 */
    PBHP_ERR_CANCELLED = 6,  /* cancelled through cancel_current()           */
    PBHP_ERR_UNSUPPORTED = 7 /* capability not implemented by this build     */
};

/* Device contract, mirrors the SAM3/matting tier semantics. */
enum PbhDevice
{
    PBHP_DEVICE_AUTO = 0, /* GPU if usable, explicit CPU fallback allowed   */
    PBHP_DEVICE_GPU = 1,  /* GPU required; never silently run on CPU        */
    PBHP_DEVICE_CPU = 2   /* CPU explicitly selected                        */
};

/* FASHN class indices. The order is part of the model contract and matches
 * config.json id2label and manifest.json. Do not reorder. */
enum PbhClass
{
    PBHP_CLASS_BACKGROUND = 0,
    PBHP_CLASS_FACE = 1,
    PBHP_CLASS_HAIR = 2,
    PBHP_CLASS_TOP = 3,
    PBHP_CLASS_DRESS = 4,
    PBHP_CLASS_SKIRT = 5,
    PBHP_CLASS_PANTS = 6,
    PBHP_CLASS_BELT = 7,
    PBHP_CLASS_BAG = 8,
    PBHP_CLASS_HAT = 9,
    PBHP_CLASS_SCARF = 10,
    PBHP_CLASS_GLASSES = 11,
    PBHP_CLASS_ARMS = 12,
    PBHP_CLASS_HANDS = 13,
    PBHP_CLASS_LEGS = 14,
    PBHP_CLASS_FEET = 15,
    PBHP_CLASS_TORSO = 16,
    PBHP_CLASS_JEWELRY = 17
};

/* Material-facing regions. Every one of the 18 classes belongs to exactly one
 * group; background is its own group. A group mask is the SUM of its member
 * class probabilities (the classes are mutually exclusive), quantized to
 * 0..255. It is a probability, not a binary mask and not a finished alpha. */
enum PbhGroup
{
    PBHP_GROUP_NONE = 0,
    PBHP_GROUP_GARMENT = 1,   /* top, dress, skirt, pants                    */
    PBHP_GROUP_HAIR = 2,      /* hair                                        */
    PBHP_GROUP_SKIN = 3,      /* face, arms, hands, legs, feet, torso        */
    PBHP_GROUP_ACCESSORY = 4, /* belt, bag, hat, scarf, glasses, jewelry     */
    PBHP_GROUP_BACKGROUND = 5 /* background                                  */
};

typedef struct PbhSession PbhSession;

typedef struct PbhCapabilities
{
    uint32_t struct_size;             /* caller sets to sizeof(PbhCapabilities) */
    int32_t  ort_available;           /* ONNX Runtime loaded                     */
    int32_t  cuda_provider_available; /* CUDAExecutionProvider is registered      */
    int32_t  cuda_device_count;       /* adapters reported by the CUDA driver     */
    int32_t  cpu_fallback_available;  /* CPUExecutionProvider is registered      */
    char     ort_version[64];
    char     device_name[160];        /* first CUDA adapter, empty if none       */
    char     error[512];
} PbhCapabilities;

typedef struct PbhSessionConfig
{
    uint32_t struct_size;
    const char *model_path;      /* fashn-human-parser.onnx                     */
    const char *manifest_path;   /* optional manifest.json (provenance)         */
    const char *profile_dir;     /* optional ORT profiling output directory     */
    int32_t device;              /* PbhDevice                                   */
    int32_t intra_threads;       /* CPU threads, 0 = ORT default                */
    /* 1 = GPU mode fails when the graph's compute nodes were not assigned to
     * the CUDA EP (a registered provider does not prove operator placement). */
    int32_t require_gpu_operators;
    /* 1 = verify the ONNX bytes against the manifest SHA-256 before load.
     * 0 = skip the hash and report model_sha256 as "unverified" (development
     * only; never use 0 for a packaged build). */
    int32_t verify_model_hash;
} PbhSessionConfig;

/* One strided RGB888 image. `data` is owned by the caller and must stay alive
 * for the duration of the call. */
typedef struct PbhImage
{
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride; /* bytes per row, >= width * 3                          */
} PbhImage;

/* Output buffers. All are optional except class_map.
 *
 *   class_map            W*H bytes, argmax class index 0..17.
 *   garment/hair/skin/   W*H bytes each, group probability 0..255.
 *   accessory
 *   class_probabilities  18*W*H bytes, class-major (plane c at c*W*H),
 *                        softmax probability 0..255.
 *   model_rgb            384*576*3 bytes, the exact RGB buffer sent to the
 *                        graph after INTER_AREA/bilinear preprocessing. This is
 *                        a diagnostic/comparison surface, not a material mask.
 *
 * Capacities are element counts, not bytes. A non-null pointer with an
 * insufficient capacity is an argument error. Buffers are tightly packed;
 * the runtime never assumes an output stride. */
typedef struct PbhParseOutputs
{
    uint32_t struct_size;
    uint8_t *class_map;
    int64_t  class_map_capacity;
    uint8_t *garment;
    int64_t  garment_capacity;
    uint8_t *hair;
    int64_t  hair_capacity;
    uint8_t *skin;
    int64_t  skin_capacity;
    uint8_t *accessory;
    int64_t  accessory_capacity;
    uint8_t *class_probabilities;
    int64_t  class_probabilities_capacity;
    uint8_t *model_rgb;
    int64_t  model_rgb_capacity;
} PbhParseOutputs;

typedef struct PbhBackendInfo
{
    uint32_t struct_size;
    int32_t used_gpu;
    int32_t cuda_node_count;
    int32_t cpu_node_count;
    int32_t device_index;
    int32_t input_width;   /* 384 */
    int32_t input_height;  /* 576 */
    int32_t grid_width;    /* 96  */
    int32_t grid_height;   /* 144 */
    char backend[96];      /* e.g. "CUDAExecutionProvider (NVIDIA RTX 4090)" */
    char model_sha256[65]; /* verified SHA-256, or "unverified"              */
} PbhBackendInfo;

typedef struct PbhParseInfo
{
    uint32_t struct_size;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    double total_ms;
    int32_t input_width;
    int32_t input_height;
    int32_t grid_width;
    int32_t grid_height;
    int32_t source_width;
    int32_t source_height;
    int32_t used_gpu;
    int32_t cuda_node_count;
    int32_t cpu_node_count;
    int32_t device_index;
    int32_t wrote_class_probabilities;
    /* Counts from the HARD argmax. Source counts are at the caller's
     * resolution; grid counts are at 96x144 and are exactly comparable to
     * the exporter's validation counts. */
    uint64_t class_pixels[PBHP_CLASS_COUNT];
    uint64_t class_pixels_grid[PBHP_CLASS_COUNT];
    uint64_t group_pixels[PBHP_GROUP_COUNT];
    /* Sum of the quantized group probability planes over every source pixel.
     * Divide by (255 * W * H) for mean group probability. */
    uint64_t group_probability_sum[PBHP_GROUP_COUNT];
    char backend[96];
    char model_sha256[65];
    char error[512];
} PbhParseInfo;

typedef struct PbhApi
{
    uint32_t abi_version;
    uint32_t struct_size;

    const char *(*version)(void);

    /* Probe the runtime and CUDA readiness WITHOUT loading a model. Safe from
     * the GUI thread (it only inspects loaded libraries / driver state). */
    int32_t (*probe_capabilities)(PbhCapabilities *out);

    /* Create a persistent session (the model stays resident; never reload per
     * frame). `out_session` receives an opaque handle on success. */
    int32_t (*create_session)(const PbhSessionConfig *config,
                              PbhSession **out_session,
                              char *err, int32_t err_len);

    void (*destroy_session)(PbhSession *session);

    /* Actual backend chosen by the session. `out->struct_size` must be set. */
    int32_t (*get_backend_info)(PbhSession *session, PbhBackendInfo *out);

    /* Run one parse. `outputs->struct_size` must be set. `info` may be NULL,
     * but when present `info->struct_size` must be set. */
    int32_t (*parse)(PbhSession *session,
                     const PbhImage *rgb,
                     const PbhParseOutputs *outputs,
                     PbhParseInfo *info,
                     char *err, int32_t err_len);

    /* Ask a Run()/postprocess that is currently executing on another thread to
     * stop. Returns immediately; the blocked parse() returns
     * PBHP_ERR_CANCELLED. Safe to call when nothing is running. */
    void (*cancel_current)(PbhSession *session);

    const char *(*get_last_error)(PbhSession *session);

    /* Static model metadata. Out-of-range indices return 0 / "". */
    int32_t (*class_group)(int32_t class_index);
    const char *(*class_name)(int32_t class_index);
} PbhApi;

typedef const PbhApi *(*PbhGetApiFn)(void);

/* Exported by pb_human_parser_cuda.dll; also resolvable through GetProcAddress
 * so a missing DLL stays a recoverable condition for the application. */
PBHP_API const PbhApi *pbhp_get_api(void);

#ifdef __cplusplus
}
#endif
