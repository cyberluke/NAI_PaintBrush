cmake_minimum_required(VERSION 3.24)
project(pb_lama_cuda LANGUAGES CXX)

# The VLC application is MinGW; this DLL is MSVC because ONNX Runtime's CUDA
# provider is an MSVC build. It is a leaf component: it links ONLY the ORT
# import library and the Windows SDK (the CUDA driver is inspected dynamically).
#
# Build:
#   cmake -S modules/gui/qt/paintbrush/gpu/lama -B <build> ^
#         -G "Visual Studio 17 2022" -A x64 -DORT_ROOT=<...>
#   cmake --build <build> --config Release --parallel 8

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

add_library(pb_lama_cuda SHARED
    lama_runtime.cpp
    lama_runtime.h
)

target_include_directories(pb_lama_cuda PRIVATE "${ORT_INCLUDE}")
target_include_directories(pb_lama_cuda PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pb_lama_cuda PRIVATE "${ORT_LIB}")

target_compile_definitions(pb_lama_cuda PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN PBLM_BUILD_DLL)
if(MSVC)
    target_compile_options(pb_lama_cuda PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(pb_lama_cuda PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pb_lama_cuda"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Acceptance smoke / parity: links the import library and calls the DLL
# in-place. PNG I/O uses the Windows Imaging Component so no extra dependency
# (and no Python) is required.
add_executable(lama_smoke tools/lama_smoke.cpp)
target_link_libraries(lama_smoke PRIVATE pb_lama_cuda windowscodecs ole32)
target_include_directories(lama_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(lama_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
if(MSVC)
    target_compile_options(lama_smoke PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

install(TARGETS pb_lama_cuda RUNTIME DESTINATION .)
