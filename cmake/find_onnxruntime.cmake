# nuke-ai-fill / cmake / find_onnxruntime.cmake
#
# Locate a prebuilt ONNX Runtime installation. No FetchContent, no
# downloads - the user supplies the ORT package once (extracted from
# the official GitHub release zip) and points -DORT_DIR at it.
#
# Sets:
#   ORT_FOUND          - TRUE if a usable ORT install was found
#   ORT_VERSION_STRING - the version detected, if available
#
# Creates imported target:
#   OnnxRuntime::OnnxRuntime - link against this for include + lib
#
# Exposes for install rules:
#   ORT_RUNTIME_DLLS - list of full paths to .dll files that must
#                      be shipped alongside the plugin DLL
#
# Expected layout under ${ORT_DIR}:
#   include/  *.h
#   lib/      onnxruntime.lib (import library)
#   lib/      *.dll           (runtime DLLs in newer releases)
#   bin/      *.dll           (runtime DLLs in older releases)
#
# Newer ORT release zips put DLLs in lib/, older ones in bin/. We
# scan both. CUDA execution provider needs additional DLLs from the
# user's CUDA Toolkit (cublas64_*, cudart64_*) and cuDNN; those are
# NOT redistributed by ORT and must come from the build machine.
# Detected and copied if present in known locations.

# ----------------------------------------------------------------------
# Inputs
# ----------------------------------------------------------------------

if(NOT ORT_DIR)
    set(ORT_FOUND FALSE)
    message(STATUS "find_onnxruntime: ORT_DIR not set; ORT integration disabled")
    return()
endif()

if(NOT EXISTS "${ORT_DIR}")
    set(ORT_FOUND FALSE)
    message(WARNING "find_onnxruntime: ORT_DIR='${ORT_DIR}' does not exist")
    return()
endif()

# ----------------------------------------------------------------------
# Locate the header and import library
# ----------------------------------------------------------------------

find_path(_ort_include_dir
    NAMES onnxruntime_cxx_api.h
    PATHS "${ORT_DIR}/include"
    NO_DEFAULT_PATH
)

# Direct existence check rather than find_library: ORT's layout is
# fixed and find_library's platform-aware patterns (.so/.a on Unix)
# would reject the .lib we want.
set(_ort_import_lib "${ORT_DIR}/lib/onnxruntime.lib")
if(NOT EXISTS "${_ort_import_lib}")
    set(_ort_import_lib "_ort_import_lib-NOTFOUND")
endif()

if(NOT _ort_include_dir OR NOT _ort_import_lib)
    set(ORT_FOUND FALSE)
    message(WARNING "find_onnxruntime: header or import lib not found under '${ORT_DIR}'")
    message(WARNING "  include : ${_ort_include_dir}")
    message(WARNING "  lib     : ${_ort_import_lib}")
    return()
endif()

# ----------------------------------------------------------------------
# Locate runtime DLLs (both lib/ and bin/ since layout varies)
# ----------------------------------------------------------------------

set(ORT_RUNTIME_DLLS)

foreach(_subdir lib bin)
    file(GLOB _ort_dlls "${ORT_DIR}/${_subdir}/*.dll")
    foreach(_dll IN LISTS _ort_dlls)
        # Skip CUDA toolkit copies that might have ended up in the zip
        # (rare but possible with some unofficial repacks). Real CUDA
        # DLLs come from the CUDA toolkit, not from ORT.
        get_filename_component(_name "${_dll}" NAME)
        if(_name MATCHES "^onnxruntime")
            list(APPEND ORT_RUNTIME_DLLS "${_dll}")
        endif()
    endforeach()
endforeach()

list(LENGTH ORT_RUNTIME_DLLS _n_dlls)
if(_n_dlls EQUAL 0)
    set(ORT_FOUND FALSE)
    message(WARNING "find_onnxruntime: no onnxruntime*.dll found under '${ORT_DIR}/(lib|bin)'")
    return()
endif()

# ----------------------------------------------------------------------
# Detect CUDA execution provider support
# ----------------------------------------------------------------------
#
# If the user installed the GPU variant of ORT, we'll see
# onnxruntime_providers_cuda.dll among the DLLs. That requires
# CUDA Toolkit cuBLAS/cuDART DLLs at load time - which the user
# has installed via CUDAToolkit. We don't redistribute those; we
# rely on the standard CUDA install being on PATH at runtime,
# which is the normal behavior of a Windows CUDA Toolkit install.

set(ORT_HAS_CUDA_EP FALSE)
foreach(_dll IN LISTS ORT_RUNTIME_DLLS)
    get_filename_component(_name "${_dll}" NAME)
    if(_name MATCHES "providers_cuda")
        set(ORT_HAS_CUDA_EP TRUE)
        break()
    endif()
endforeach()

# ----------------------------------------------------------------------
# Try to read VERSION_NUMBER file if present (modern ORT releases ship one)
# ----------------------------------------------------------------------

set(ORT_VERSION_STRING "unknown")
if(EXISTS "${ORT_DIR}/VERSION_NUMBER")
    file(READ "${ORT_DIR}/VERSION_NUMBER" _ort_version_raw)
    string(STRIP "${_ort_version_raw}" ORT_VERSION_STRING)
endif()

# ----------------------------------------------------------------------
# Imported target
# ----------------------------------------------------------------------

if(NOT TARGET OnnxRuntime::OnnxRuntime)
    add_library(OnnxRuntime::OnnxRuntime UNKNOWN IMPORTED)
    set_target_properties(OnnxRuntime::OnnxRuntime PROPERTIES
        IMPORTED_LOCATION "${_ort_import_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_ort_include_dir}"
    )
endif()

set(ORT_FOUND TRUE)

message(STATUS "find_onnxruntime:")
message(STATUS "  ORT_DIR          : ${ORT_DIR}")
message(STATUS "  ORT_VERSION      : ${ORT_VERSION_STRING}")
message(STATUS "  ORT_HAS_CUDA_EP  : ${ORT_HAS_CUDA_EP}")
message(STATUS "  ORT_RUNTIME_DLLS : ${_n_dlls} files")
foreach(_dll IN LISTS ORT_RUNTIME_DLLS)
    get_filename_component(_name "${_dll}" NAME)
    message(STATUS "    - ${_name}")
endforeach()
