# nuke-ai-fill / core / CMakeLists.txt
#
# Static library shared between both Ops. Linked into each MODULE
# plugin DLL, not loaded separately. Built with the same /MD release
# CRT and the same defines as the plugins (via nuke_ai_fill_flags).

add_library(nuke_ai_fill_core STATIC
    src/ai_hash.cpp
    src/ai_worker.cpp
    src/ai_cache.cpp
)

target_include_directories(nuke_ai_fill_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nuke_ai_fill_core
    PUBLIC
        nuke_ai_fill_flags
)

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