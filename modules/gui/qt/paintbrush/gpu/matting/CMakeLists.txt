cmake_minimum_required(VERSION 3.24)
project(pb_matting_cuda LANGUAGES CXX)

# The VLC application is MinGW; this DLL is MSVC because ONNX Runtime's CUDA
# provider is an MSVC build. It is a leaf component: it links ONLY the ORT
# import library and the Windows SDK (the CUDA driver is loaded dynamically).

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

add_library(pb_matting_cuda SHARED
    matting_runtime.cpp
    ../matting_cuda_api.h
)

target_include_directories(pb_matting_cuda PRIVATE "${ORT_INCLUDE}")
target_include_directories(pb_matting_cuda PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(pb_matting_cuda PRIVATE "${ORT_LIB}")

target_compile_definitions(pb_matting_cuda PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN PBMC_BUILD_DLL)
if(MSVC)
    target_compile_options(pb_matting_cuda PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(pb_matting_cuda PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pb_matting_cuda"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Acceptance smoke: links the import library and calls the DLL in-place.
add_executable(matting_smoke tools/matting_smoke.cpp)
target_link_libraries(matting_smoke PRIVATE pb_matting_cuda)
target_include_directories(matting_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
set_target_properties(matting_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

install(TARGETS pb_matting_cuda RUNTIME DESTINATION .)
