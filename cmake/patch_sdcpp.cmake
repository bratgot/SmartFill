# nuke-ai-fill / cmake / patch_sdcpp.cmake
#
# Apply local modifications to the vendored stable-diffusion.cpp source
# tree. These patches survive fresh clones, third_party gitignore, and
# sd.cpp upgrades cleanly.
#
# Each patch is idempotent: re-running has no effect if already applied.
# If the OLD_TEXT can''t be found AND the NEW_TEXT isn''t there either,
# a warning is emitted -- that means sd.cpp has changed upstream and
# the patch needs revisiting.
#
# Requires: SDCPP_DIR must be set to the resolved sd.cpp source root.
#           Included from find_sdcpp.cmake after SDCPP_DIR validation.
#
# Patches applied:
#   1. src/control.hpp: CONTROL_NET_GRAPH_SIZE 1536 -> 32768
#      Reason: 1536 is the SD1.5 default. SDXL ControlNet''s attention-
#      heavy forward graph (10 transformer layers in input_blocks 7/8
#      and middle_block) overflows it during the first sampling step
#      with `GGML_ASSERT(cgraph->n_nodes < cgraph->size) failed` at
#      ggml.c:6913. 32768 matches sd.cpp''s own MAX_GRAPH_SIZE constant
#      in ggml_extend.hpp and provides ~21x headroom. See NDK_NOTES
#      section 22 for the full story.

if(NOT SDCPP_DIR)
    message(FATAL_ERROR "patch_sdcpp: SDCPP_DIR is not set")
endif()

# ----------------------------------------------------------------------
# Helper: apply a string replacement to a file in the sd.cpp tree.
#
# Idempotent. Three outcomes:
#   - OLD_TEXT present       -> replace + log "applied"
#   - NEW_TEXT present       -> silent (already patched)
#   - neither present        -> warning (upstream changed; revisit)
#
# Uses string(FIND) for the presence check rather than MATCHES, so the
# patterns are treated as literal strings (not regexes).
# ----------------------------------------------------------------------
function(_patch_sdcpp_replace REL_PATH OLD_TEXT NEW_TEXT DESCRIPTION)
    set(_target "${SDCPP_DIR}/${REL_PATH}")
    if(NOT EXISTS "${_target}")
        message(WARNING "patch_sdcpp: file not found: ${_target} (skipping ${DESCRIPTION})")
        return()
    endif()

    file(READ "${_target}" _content)

    string(FIND "${_content}" "${OLD_TEXT}" _old_pos)
    if(_old_pos GREATER -1)
        string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" _patched "${_content}")
        file(WRITE "${_target}" "${_patched}")
        message(STATUS "patch_sdcpp: applied -- ${DESCRIPTION}")
        return()
    endif()

    string(FIND "${_content}" "${NEW_TEXT}" _new_pos)
    if(_new_pos GREATER -1)
        # Already patched. Silent.
        return()
    endif()

    message(WARNING
        "patch_sdcpp: in ${REL_PATH}, neither original nor patched text was found. "
        "sd.cpp may have changed upstream; patch needs to be revisited.\n"
        "  Description: ${DESCRIPTION}\n"
        "  Looking for OLD: ${OLD_TEXT}\n"
        "  ...or already patched NEW: ${NEW_TEXT}")
endfunction()

# ----------------------------------------------------------------------
# Patch 1: bump CONTROL_NET_GRAPH_SIZE so SDXL ControlNet fits.
# ----------------------------------------------------------------------
_patch_sdcpp_replace(
    "src/control.hpp"
    "#define CONTROL_NET_GRAPH_SIZE 1536"
    "#define CONTROL_NET_GRAPH_SIZE 32768"
    "control.hpp: CONTROL_NET_GRAPH_SIZE 1536 -> 32768 (SDXL ControlNet support)"
)

# Future patches go here, one _patch_sdcpp_replace() call each.