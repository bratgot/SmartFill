# nuke-ai-fill root CMakeLists.txt
#
# Build:
#   cmake -G "Visual Studio 16 2019" -A x64 ^
#         -DNuke_DIR="C:/Program Files/Nuke14.1v6" ^
#         -DSD_CUDA=ON ^
#         -DORT_DIR="C:/dev/onnxruntime-win-x64-gpu-1.20.1" ^
#         -B build .
#   cmake --build build --config Release
#
# CMAKE_BUILD_TYPE is intentionally NOT used (multi-config generator).

cmake_minimum_required(VERSION 3.22)

project(nuke_ai_fill
    VERSION 0.1.0
    DESCRIPTION "MIT/Apache AI inpainting and generation Ops for Nuke 14"
    LANGUAGES CXX
)

# ----------------------------------------------------------------------
# Global C++ settings
# ----------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Per NDK_NOTES section 1.5: do not allow CMAKE_BUILD_TYPE on multi-config.
if(CMAKE_CONFIGURATION_TYPES)
    if(DEFINED CMAKE_BUILD_TYPE AND NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
        message(FATAL_ERROR
            "Do not pass -DCMAKE_BUILD_TYPE on a multi-config generator. "
            "Use --config Release at build time instead.")
    endif()
endif()

# ----------------------------------------------------------------------
# Options
# ----------------------------------------------------------------------

option(BUILD_SMARTFILL    "Build AISmartFill Op (LaMa via ONNX Runtime)" ON)
option(BUILD_GENERATE     "Build AIGenerate Op (stable-diffusion.cpp)"   ON)
option(SD_CUDA            "stable-diffusion.cpp: CUDA backend"           ON)
option(SD_VULKAN          "stable-diffusion.cpp: Vulkan backend"         OFF)
option(SD_AVX2            "stable-diffusion.cpp: CPU AVX2 baseline"      ON)

# Paths to external dependencies. Override on the cmake command line.
set(ORT_DIR ""    CACHE PATH "Path to onnxruntime-win-x64-gpu install root")
set(SD_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stable-diffusion.cpp"
    CACHE PATH "Path to stable-diffusion.cpp source (submodule by default)")

# ----------------------------------------------------------------------
# Find Nuke (NDK_NOTES section 1.2: target is Nuke::NDK, NOT Nuke::DDImage)
# ----------------------------------------------------------------------

find_package(Nuke REQUIRED)

if(NOT TARGET Nuke::NDK)
    message(FATAL_ERROR
        "find_package(Nuke) succeeded but Nuke::NDK target was not exposed. "
        "Check that Nuke_DIR points at a Nuke 14+ install with NDK headers.")
endif()

# ----------------------------------------------------------------------
# Global compile definitions and flags
# Per NDK_NOTES section 1.3 and 1.4
# ----------------------------------------------------------------------

add_library(nuke_ai_fill_flags INTERFACE)

target_compile_definitions(nuke_ai_fill_flags INTERFACE
    NOMINMAX
    _USE_MATH_DEFINES
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    # /W3 not /W4 - DDK headers emit warnings inside Foundry's own code.
    target_compile_options(nuke_ai_fill_flags INTERFACE
        /W3
        /EHsc
        /MP                   # multi-process compile (build speed)
        /Zc:__cplusplus       # report real __cplusplus value
        /permissive-          # stricter standard conformance
    )
    # /MD (dynamic Release CRT) to match Nuke. Set per-config to keep
    # Debug builds usable when developing.
    set_property(TARGET nuke_ai_fill_flags PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ----------------------------------------------------------------------
# Output directories: all Ops land next to each other for easy deploy
# ----------------------------------------------------------------------

set(NUKE_AI_FILL_PLUGIN_DIR "${CMAKE_BINARY_DIR}/plugin")
foreach(cfg Release RelWithDebInfo Debug MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${NUKE_AI_FILL_PLUGIN_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} "${NUKE_AI_FILL_PLUGIN_DIR}")
endforeach()

# ----------------------------------------------------------------------
# Subdirectories
# ----------------------------------------------------------------------

add_subdirectory(core)

if(BUILD_SMARTFILL)
    add_subdirectory(ops/AISmartFill)
endif()

if(BUILD_GENERATE)
    add_subdirectory(ops/AIGenerate)
endif()

# ----------------------------------------------------------------------
# Install: copy DLLs + menu.py + license files into deploy folder
# ----------------------------------------------------------------------

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/menu.py"
    DESTINATION nuke-ai-fill
)

# License and readme files install only when they exist at configure
# time. Avoids CMake's OPTIONAL-flag quirks on Windows; if you add
# LICENSE later, re-run cmake configure to pick it up.
foreach(_doc IN ITEMS LICENSE THIRD_PARTY_LICENSES.md README.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}"
                DESTINATION nuke-ai-fill)
    endif()
endforeach()

install(DIRECTORY "${NUKE_AI_FILL_PLUGIN_DIR}/"
    DESTINATION nuke-ai-fill
    FILES_MATCHING PATTERN "*.dll"
)

# Append registration to the user's main .nuke/menu.py. The script
# is idempotent: marker-detected on repeat installs, never overwrites
# user content. Per NDK_NOTES 8.2.
install(SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_user_menu.cmake"
)

# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------

message(STATUS "")
message(STATUS "nuke-ai-fill configuration:")
message(STATUS "  Nuke         : ${Nuke_DIR}")
message(STATUS "  Generator    : ${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_TOOLSET)
    message(STATUS "  Toolset      : ${CMAKE_GENERATOR_TOOLSET}")
endif()
message(STATUS "  AISmartFill  : ${BUILD_SMARTFILL}")
message(STATUS "  AIGenerate   : ${BUILD_GENERATE}")
if(BUILD_SMARTFILL)
    message(STATUS "  ORT_DIR      : ${ORT_DIR}")
endif()
if(BUILD_GENERATE)
    message(STATUS "  SD_CUDA      : ${SD_CUDA}")
    message(STATUS "  SD_VULKAN    : ${SD_VULKAN}")
    message(STATUS "  SD_CPP_DIR   : ${SD_CPP_DIR}")
endif()
message(STATUS "")
