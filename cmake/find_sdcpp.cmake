# nuke-ai-fill / cmake / find_sdcpp.cmake
#
# Locate stable-diffusion.cpp source and integrate its build into ours.
# Air-gapped: no downloads. User supplies the sd.cpp source tree via
# -DSDCPP_DIR (e.g. cloned from github.com/leejet/stable-diffusion.cpp)
# or places it under third_party/stable-diffusion.cpp in the source root.
#
# Sets:
#   SDCPP_FOUND       - TRUE if sd.cpp was located and configured
#   SDCPP_DIR         - resolved path to sd.cpp source tree
#
# Creates imported target:
#   StableDiffusionCpp::lib - links sd.cpp + ggml as a static library
#
# Notes:
#   - sd.cpp uses CMake. We add_subdirectory it with our overrides
#     to disable executable targets (we only want the library) and
#     enable CUDA backend when CUDA Toolkit is found.
#   - The CUDA backend requires CUDA Toolkit 12.x. CUDA 12.9 has
#     native Blackwell sm_120 support; 12.6 works via PTX JIT.

if(NOT SDCPP_DIR)
    # Try the conventional location under our source tree.
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/stable-diffusion.cpp/CMakeLists.txt")
        set(SDCPP_DIR "${CMAKE_SOURCE_DIR}/third_party/stable-diffusion.cpp")
    endif()
endif()

if(NOT SDCPP_DIR)
    set(SDCPP_FOUND FALSE)
    message(STATUS "find_sdcpp: SDCPP_DIR not set and no third_party/stable-diffusion.cpp; AIGenerate disabled")
    return()
endif()

if(NOT EXISTS "${SDCPP_DIR}/CMakeLists.txt")
    set(SDCPP_FOUND FALSE)
    message(WARNING "find_sdcpp: SDCPP_DIR='${SDCPP_DIR}' has no CMakeLists.txt")
    return()
endif()

# ----------------------------------------------------------------------
# Override sd.cpp build options before add_subdirectory.
#
# Naming: sd.cpp inherits ggml's BUILD_SHARED_LIBS handling. We want
# static so our plugin DLL has zero external sd.cpp dependencies.
# ----------------------------------------------------------------------

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SD_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# sd.cpp executable targets are not needed - we only want the library.
set(SD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# CUDA backend. ggml will detect CUDA Toolkit and enable cublas/cudart.
# If CUDA isn't available, the build falls back to CPU and the plugin
# still works (slower).
if(DEFINED ENV{CUDA_PATH} OR CMAKE_CUDA_COMPILER OR EXISTS "$ENV{CUDA_PATH}")
    set(SD_CUDA ON CACHE BOOL "" FORCE)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
else()
    set(SD_CUDA OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "" FORCE)
endif()

# Disable OpenCL, Vulkan, Metal etc. - they pull in their own runtime
# dependencies that complicate distribution.
set(SD_OPENCL OFF CACHE BOOL "" FORCE)
set(SD_METAL  OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_OPENCL OFF CACHE BOOL "" FORCE)
set(GGML_METAL  OFF CACHE BOOL "" FORCE)

# Use ggml-cuda's MMQ kernels for quantized matmul.
set(GGML_CUDA_FORCE_MMQ ON CACHE BOOL "" FORCE)

# ----------------------------------------------------------------------
# Wrap sd.cpp's CMake into ours via add_subdirectory. EXCLUDE_FROM_ALL
# so the executables aren't built even if SD_BUILD_EXAMPLES sneaks back.
# ----------------------------------------------------------------------

# Save & restore CMP0077 so sd.cpp's option() calls don't shadow our set().
if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

add_subdirectory(
    "${SDCPP_DIR}"
    "${CMAKE_BINARY_DIR}/third_party/stable-diffusion.cpp"
    EXCLUDE_FROM_ALL
)

# sd.cpp's library target name. Confirm by inspecting their CMakeLists;
# at time of writing the target is "stable-diffusion".
if(NOT TARGET stable-diffusion)
    message(WARNING "find_sdcpp: expected target 'stable-diffusion' was not created by add_subdirectory")
    set(SDCPP_FOUND FALSE)
    return()
endif()

# ----------------------------------------------------------------------
# Create our own interface target that wraps sd.cpp's library and its
# include directories. Consumers link against StableDiffusionCpp::lib
# and get the right includes automatically.
# ----------------------------------------------------------------------

if(NOT TARGET StableDiffusionCpp::lib)
    add_library(StableDiffusionCpp::lib INTERFACE IMPORTED)
    set_target_properties(StableDiffusionCpp::lib PROPERTIES
        INTERFACE_LINK_LIBRARIES "stable-diffusion"
        INTERFACE_INCLUDE_DIRECTORIES "${SDCPP_DIR}"
    )
endif()

set(SDCPP_FOUND TRUE)

message(STATUS "find_sdcpp:")
message(STATUS "  SDCPP_DIR        : ${SDCPP_DIR}")
message(STATUS "  SD_CUDA          : ${SD_CUDA}")
message(STATUS "  GGML_CUDA        : ${GGML_CUDA}")
