cmake_minimum_required(VERSION 3.20)
project(sam3_cuda_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SAM3_SOURCE_DIR : checkout of greenjava/sam3.cpp (cuda branch)
# SAM3_BUILD_DIR  : its MSVC Release build tree (sam3.lib + ggml import libs)
if(NOT SAM3_SOURCE_DIR OR NOT SAM3_BUILD_DIR)
    message(FATAL_ERROR "Set -DSAM3_SOURCE_DIR=... and -DSAM3_BUILD_DIR=...")
endif()

find_package(CUDAToolkit REQUIRED)

add_library(sam3_cuda_bridge SHARED sam3_cuda_bridge.cpp)

target_include_directories(sam3_cuda_bridge PRIVATE
    "${SAM3_SOURCE_DIR}"
    "${SAM3_SOURCE_DIR}/ggml/include"
)

if(MSVC)
    target_compile_options(sam3_cuda_bridge PRIVATE /utf-8 /EHsc /MD)
endif()

target_link_libraries(sam3_cuda_bridge PRIVATE
    "${SAM3_BUILD_DIR}/Release/sam3.lib"
    "${SAM3_BUILD_DIR}/ggml/src/Release/ggml.lib"
    "${SAM3_BUILD_DIR}/ggml/src/Release/ggml-base.lib"
    "${SAM3_BUILD_DIR}/ggml/src/Release/ggml-cpu.lib"
    "${SAM3_BUILD_DIR}/ggml/src/ggml-cuda/Release/ggml-cuda.lib"
    CUDA::cudart
)

set_target_properties(sam3_cuda_bridge PROPERTIES
    OUTPUT_NAME "sam3_cuda"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# ggml is built shared by the fork; keep the runtime set together so the
# bridge and the smoke tool can run from one directory.
foreach(ggml_dll ggml.dll ggml-base.dll ggml-cpu.dll ggml-cuda.dll)
    if(EXISTS "${SAM3_BUILD_DIR}/bin/Release/${ggml_dll}")
        add_custom_command(TARGET sam3_cuda_bridge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SAM3_BUILD_DIR}/bin/Release/${ggml_dll}"
                "$<TARGET_FILE_DIR:sam3_cuda_bridge>/${ggml_dll}")
    endif()
endforeach()

install(TARGETS sam3_cuda_bridge RUNTIME DESTINATION bin)
install(FILES sam3_cuda_api.h DESTINATION include)

# Native acceptance harness. It loads the DLL through LoadLibrary exactly like
# the MinGW plugin, so it exercises the same C ABI path.
add_executable(sam3_cuda_smoke tools/sam3_cuda_smoke.cpp)
target_include_directories(sam3_cuda_smoke PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SAM3_SOURCE_DIR}/stb"
)
if(MSVC)
    target_compile_options(sam3_cuda_smoke PRIVATE /utf-8 /EHsc /MD)
endif()
set_target_properties(sam3_cuda_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
