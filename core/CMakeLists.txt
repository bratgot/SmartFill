# nuke-ai-fill / core / CMakeLists.txt
#
# Static library shared between both Ops. Linked into each MODULE
# plugin DLL, not loaded separately. Built with the same /MD release
# CRT and the same defines as the plugins (via nuke_ai_fill_flags).

set(_core_sources
    src/ai_hash.cpp
    src/ai_worker.cpp
    src/ai_cache.cpp
    src/image_cache.cpp
    src/plugin_path.cpp
)

# LaMa-dependent sources only compile when ONNX Runtime was found at
# configure time. The headers are always present (so includes resolve
# cleanly), but calling any LamaSession/inpaint_image method without
# NUKE_AI_FILL_HAS_LAMA defined is a link error - by design.
if(ORT_FOUND)
    list(APPEND _core_sources
        src/lama_session.cpp
        src/lama_tiler.cpp
    )
endif()

add_library(nuke_ai_fill_core STATIC ${_core_sources})

target_include_directories(nuke_ai_fill_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nuke_ai_fill_core
    PUBLIC
        nuke_ai_fill_flags
)

if(ORT_FOUND)
    target_link_libraries(nuke_ai_fill_core PUBLIC OnnxRuntime::OnnxRuntime)
    # PUBLIC so it propagates to anything linking core - the Op will
    # use this to gate its inference code path.
    target_compile_definitions(nuke_ai_fill_core PUBLIC NUKE_AI_FILL_HAS_LAMA=1)
endif()

# This library does NOT link Nuke::NDK. It is pure C++17 with no
# DD::Image dependency. The Op-side glue that bridges Nuke buffers
# into raw float arrays lives in each Op subdirectory; that's where
# Nuke::NDK gets linked.
#
# Rationale: keeps the core unit-testable without a Nuke install,
# and keeps the hot ML inference path free of any DD::Image header
# bloat that would slow compiles.

set_target_properties(nuke_ai_fill_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "core"
)
