cmake_minimum_required(VERSION 3.24)
project(pb_human_parser_cuda LANGUAGES CXX)

# The VLC application is MinGW; this DLL is MSVC because ONNX Runtime's CUDA
# provider is an MSVC build. It is a leaf component: it links ONLY the ORT
# import library and the Windows SDK (the CUDA driver is loaded dynamically).
#
# Build in a dedicated directory. Never point this at the application's
# win64-meson tree; the MSVC leaf runtime and the MinGW plugin are independent.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED ORT_ROOT OR ORT_ROOT STREQUAL "")
    message(FATAL_ERROR "ORT_ROOT must point at the extracted Microsoft.ML.OnnxRuntime.Gpu.Windows package")
endif()

set(ORT_INCLUDE "${ORT_ROOT}/buildTransitive/native/include")
set(ORT_LIB "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime.lib")
if(NOT EXISTS "${ORT_LIB}")
    message(FATAL_ERROR "onnxruntime.lib not found under ${ORT_ROOT}")
endif()

add_library(pb_human_parser_cuda SHARED
    human_parser_runtime.cpp
    sha256.h
    ../human_parser_cuda_api.h
)

target_include_directories(pb_human_parser_cuda PRIVATE "${ORT_INCLUDE}")
target_include_directories(pb_human_parser_cuda PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(pb_human_parser_cuda PRIVATE "${ORT_LIB}")

target_compile_definitions(pb_human_parser_cuda PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN PBHP_BUILD_DLL)
if(MSVC)
    target_compile_options(pb_human_parser_cuda PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(pb_human_parser_cuda PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pb_human_parser_cuda"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Acceptance smoke: loads the real DLL through the C ABI, runs the pinned
# FASHN graph on the model-card example and exercises alpha composition on the
# supplied Lamborghini cutout.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/human_parser_smoke.cpp")
    add_executable(human_parser_smoke
        tools/human_parser_smoke.cpp
        tools/wic_image.h
    )
    target_include_directories(human_parser_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_definitions(human_parser_smoke PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(human_parser_smoke PRIVATE
        windowscodecs ole32)
    if(MSVC)
        target_compile_options(human_parser_smoke PRIVATE
            /W4 /permissive- /EHsc /utf-8)
    endif()
    set_target_properties(human_parser_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

install(TARGETS pb_human_parser_cuda RUNTIME DESTINATION .)
