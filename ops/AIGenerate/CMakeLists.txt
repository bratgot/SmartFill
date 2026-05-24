# nuke-ai-fill / ops / AIGenerate / CMakeLists.txt
#
# Per NDK_NOTES section 1.6: MODULE, not SHARED. Nuke LoadLibrarys the
# plugin; nothing links against it.
#
# Per NDK_NOTES section 1.7: output filename MUST match Op::Description
# name string. The Description registers "AIGenerate", DLL is AIGenerate.dll.

add_library(AIGenerate MODULE
    AIGenerate.cpp
)

# Pin the target to C++ only. After sd.cpp's add_subdirectory enables
# the C language project-wide, CMake otherwise tries to detect C compiler
# features for our target and fails with:
#   "No known features for C compiler '' version ."
# LINKER_LANGUAGE CXX tells CMake this target's primary language is C++
# and no C feature detection is needed.
set_target_properties(AIGenerate PROPERTIES
    LINKER_LANGUAGE CXX
    OUTPUT_NAME "AIGenerate"
    PREFIX ""
    SUFFIX ".dll"
    FOLDER "ops"
)

target_link_libraries(AIGenerate
    PRIVATE
        Nuke::NDK
        nuke_ai_fill_core
        nuke_ai_fill_flags
        StableDiffusionCpp::lib
)

source_group("" FILES AIGenerate.cpp)
