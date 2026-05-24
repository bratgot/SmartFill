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

target_link_libraries(AISmartFill
    PRIVATE
        Nuke::NDK
        nuke_ai_fill_core
        nuke_ai_fill_flags
)

set_target_properties(AISmartFill PROPERTIES
    OUTPUT_NAME "AISmartFill"
    PREFIX ""              # no "lib" prefix on any platform
    SUFFIX ".dll"          # explicit on Windows; harmless elsewhere
    FOLDER "ops"
)

# Source-level grouping for IDE solution explorer.
source_group("" FILES AISmartFill.cpp)
