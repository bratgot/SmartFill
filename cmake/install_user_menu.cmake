# nuke-ai-fill / cmake / install_user_menu.cmake
#
# Runs during `cmake --install`. Idempotently appends a registration
# block to the user's main .nuke/menu.py so Nuke picks up our plugin
# folder at startup. Never overwrites existing content - either adds
# the block once or detects the marker and skips.
#
# The marker comment is the source of truth. If the user later wants
# to remove the registration, they delete everything between the
# markers (inclusive). On the next cmake --install, the script sees
# no marker and re-appends.
#
# Variables available at install time:
#   CMAKE_INSTALL_PREFIX - set to the --prefix argument, e.g.
#                          C:/Users/you/.nuke when installing the
#                          plugin to that location.
#
# Per NDK_NOTES 8.2.

set(_user_menu      "${CMAKE_INSTALL_PREFIX}/menu.py")
set(_marker_begin   "# --- nuke-ai-fill (managed by cmake --install) ---")
set(_marker_end     "# --- end nuke-ai-fill ---")
set(_plugin_dir     "nuke-ai-fill")

# Block to append. The leading newline ensures separation from any
# prior content; final newline keeps the file POSIX-clean.
set(_block "
${_marker_begin}
import nuke
nuke.pluginAddPath('./${_plugin_dir}')
${_marker_end}
")

# Read existing content (empty if file does not yet exist).
set(_existing "")
if(EXISTS "${_user_menu}")
    file(READ "${_user_menu}" _existing)
endif()

# If the marker is already present, do nothing. This is the
# idempotent path - safe to run cmake --install repeatedly.
string(FIND "${_existing}" "${_marker_begin}" _found_idx)
if(_found_idx GREATER -1)
    message(STATUS "menu.py already registers nuke-ai-fill (marker at offset ${_found_idx}); skipping")
    return()
endif()

# Ensure parent dir exists (it should, since cmake --install just
# wrote DLLs into a sibling of menu.py, but defensive).
get_filename_component(_menu_dir "${_user_menu}" DIRECTORY)
file(MAKE_DIRECTORY "${_menu_dir}")

# If the existing file has no trailing newline, add one before the
# block so we do not glue our marker onto the user's last line.
if(_existing AND NOT _existing MATCHES "\n$")
    file(APPEND "${_user_menu}" "\n")
endif()

file(APPEND "${_user_menu}" "${_block}")

if(_existing)
    message(STATUS "Appended nuke-ai-fill registration to existing ${_user_menu}")
else()
    message(STATUS "Created ${_user_menu} with nuke-ai-fill registration")
endif()
