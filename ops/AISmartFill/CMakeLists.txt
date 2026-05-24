# nuke-ai-fill / ops / AISmartFill / CMakeLists.txt
#
# Per NDK_NOTES section 1.6: MODULE, not SHARED. Nuke LoadLibrarys the
# plugin; nothing links against it, so SHARED would emit a useless
# import library and confuse CMake's defaults.
#
# Per NDK_NOTES section 1.7: output filename MUST match the
# Op::Description name string. The Description registers "AISmartFill",
# so the DLL is AISmartFill.dll.

add_library(AISmartFill MODULE
    AISmartFill.cpp
)

# Windows DLL search-order workaround: when ORT is available and we
# need to preempt Windows' built-in onnxruntime.dll from System32,
# include the DllMain bootstrap that LoadLibraryEx's our adjacent copy
# before the loader resolves the implicit import.
if(WIN32 AND ORT_FOUND)
    target_sources(AISmartFill PRIVATE dll_bootstrap.cpp)
endif()

target_link_libraries(AISmartFill
    PRIVATE
        Nuke::NDK
        nuke_ai_fill_core
        nuke_ai_fill_flags
)

# /DELAYLOAD defers the implicit import resolution for onnxruntime.dll
# until first call. This is essential when Windows ships its own
# onnxruntime.dll in System32: without delay-load, that older DLL gets
# bound to our plugin at load time, before our DllMain / lib-priority
# static initializer can preload the bundled copy.
#
# delayimp.lib provides the runtime support for the delay-load stub.
if(WIN32 AND ORT_FOUND)
    target_link_options(AISmartFill PRIVATE
        "/DELAYLOAD:onnxruntime.dll"
    )
    target_link_libraries(AISmartFill PRIVATE delayimp.lib)
endif()

set_target_properties(AISmartFill PROPERTIES
    OUTPUT_NAME "AISmartFill"
    PREFIX ""              # no "lib" prefix on any platform
    SUFFIX ".dll"          # explicit on Windows; harmless elsewhere
    FOLDER "ops"
)

# Source-level grouping for IDE solution explorer.
source_group("" FILES AISmartFill.cpp dll_bootstrap.cpp)
