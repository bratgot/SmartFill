// nuke-ai-fill / core / plugin_path.cpp
//
// See plugin_path.h. Strict ASCII per NDK_NOTES 6.1.

#include "plugin_path.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <limits.h>
#  include <unistd.h>
#endif

namespace nukeaifill {

namespace {

// A free function in this TU that GetModuleHandleEx can use to
// identify which module contains it.
void path_resolver_anchor() noexcept {}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string{};
    int n = WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string{};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

void to_forward_slashes(std::string& s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
}

std::string trim_to_dir(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return std::string{};
    return path.substr(0, pos);
}

std::string env_or_empty(const char* name) {
    if (!name) return {};
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

} // anonymous namespace

std::string current_plugin_path() {
#ifdef _WIN32
    HMODULE hmod = nullptr;
    BOOL ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&path_resolver_anchor),
        &hmod);
    if (!ok || hmod == nullptr) return std::string{};

    // MAX_PATH is 260 chars but Windows long paths can exceed that.
    // Try a small buffer first, grow if needed.
    std::wstring buf(512, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(
            hmod, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string{};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() >= 32768) return std::string{};  // give up
        buf.resize(buf.size() * 2);
    }

    std::string utf8 = wide_to_utf8(buf);
    to_forward_slashes(utf8);
    return utf8;
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&path_resolver_anchor), &info) == 0) {
        return std::string{};
    }
    if (info.dli_fname == nullptr) return std::string{};
    std::string s(info.dli_fname);
    to_forward_slashes(s);
    return s;
#endif
}

std::string current_plugin_dir() {
    return trim_to_dir(current_plugin_path());
}

std::string default_cache_dir() {
#ifdef _WIN32
    std::string base = env_or_empty("APPDATA");
    if (base.empty()) {
        base = env_or_empty("USERPROFILE");
        if (base.empty()) return std::string{};
        base += "\\AppData\\Roaming";
    }
    base += "\\nuke-ai-fill\\cache";
    to_forward_slashes(base);
    return base;
#elif defined(__APPLE__)
    std::string home = env_or_empty("HOME");
    if (home.empty()) return std::string{};
    return home + "/Library/Caches/nuke-ai-fill";
#else
    std::string base = env_or_empty("XDG_CACHE_HOME");
    if (base.empty()) {
        std::string home = env_or_empty("HOME");
        if (home.empty()) return std::string{};
        base = home + "/.cache";
    }
    return base + "/nuke-ai-fill";
#endif
}

} // namespace nukeaifill
