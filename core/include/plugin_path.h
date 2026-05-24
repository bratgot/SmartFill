// nuke-ai-fill / core / plugin_path.h
//
// Locate the plugin DLL (the one containing this code) at runtime.
// Used by the Op to find sibling files - model files, default cache
// directories, etc - relative to where the plugin was installed,
// without hardcoding paths or requiring user configuration.
//
// On Windows uses GetModuleHandleEx + GetModuleFileNameW with the
// address of a function in this translation unit. Because this code
// is compiled into the static core library that's linked into the
// Op MODULE DLL, the resolved module is the Op DLL itself.
//
// All returned paths use forward slashes (NDK_NOTES section 4).
// Strict ASCII per NDK_NOTES 6.1.

#ifndef NUKE_AI_FILL_PLUGIN_PATH_H
#define NUKE_AI_FILL_PLUGIN_PATH_H

#include <string>

namespace nukeaifill {

// Absolute path of the plugin DLL containing the call site, with
// forward slashes. Returns empty string on failure (very rare;
// would only happen if Windows lost track of the loaded module).
std::string current_plugin_path();

// Directory containing the plugin DLL. Convenience helper.
// Returns empty on failure. Has no trailing slash.
std::string current_plugin_dir();

// Default cache directory, OS-appropriate, forward slashes.
//   Windows: %APPDATA%/nuke-ai-fill/cache
//   Linux:   $XDG_CACHE_HOME/nuke-ai-fill (or ~/.cache/nuke-ai-fill)
//   macOS:   ~/Library/Caches/nuke-ai-fill
// The directory is NOT created here; caller does that via AiCache.
std::string default_cache_dir();

} // namespace nukeaifill

#endif // NUKE_AI_FILL_PLUGIN_PATH_H
