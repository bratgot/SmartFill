// nuke-ai-fill / ops / AISmartFill / dll_bootstrap.cpp
//
// Pre-load adjacent onnxruntime.dll before MSVC C++ static initializers
// (specifically ORT's Ort::Global<T>::api_) trigger implicit-import
// resolution against Windows 11's System32\onnxruntime.dll.
//
// This file runs at the EARLIEST possible point in DLL load - it uses
// only Win32 APIs (no CRT, no C++ stdlib, no exceptions). That's
// required because init_seg(lib) priority runs before most of the CRT
// is fully ready, and any crash here takes the whole DLL load down.
//
// Diagnostic logging goes to %TEMP%\nuke-ai-fill-bootstrap.log via
// CreateFileW + WriteFile only. Each load overwrites the previous log.
//
// Per NDK_NOTES 6.1: strict ASCII.

#ifdef _WIN32

#include <windows.h>

#pragma init_seg(lib)

namespace {

template <typename T, size_t N>
constexpr size_t array_size(const T (&)[N]) noexcept { return N; }

// Anchor function for GetModuleHandleEx.
void bootstrap_anchor() noexcept {}

// ----------------------------------------------------------------------
// Log file handle - kept open for the lifetime of the static globals
// in this TU. Uses HANDLE rather than FILE* to avoid any CRT dependency.
// ----------------------------------------------------------------------

HANDLE g_log_handle = INVALID_HANDLE_VALUE;

// Locate %TEMP%\nuke-ai-fill-bootstrap.log via Win32 only.
void open_log()
{
    wchar_t tmp_dir[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"TEMP", tmp_dir, array_size(tmp_dir));
    if (n == 0 || n >= array_size(tmp_dir)) {
        // Fall back to C:\ if TEMP is unset.
        tmp_dir[0] = L'C'; tmp_dir[1] = L':'; tmp_dir[2] = L'\\'; tmp_dir[3] = 0;
    }

    wchar_t log_path[MAX_PATH * 2];
    // Manual wide-string concat: <tmp_dir>\nuke-ai-fill-bootstrap.log
    size_t i = 0;
    for (; tmp_dir[i] && i < array_size(log_path) - 64; ++i) {
        log_path[i] = tmp_dir[i];
    }
    // Ensure trailing backslash.
    if (i > 0 && log_path[i - 1] != L'\\') {
        log_path[i++] = L'\\';
    }
    static const wchar_t name[] = L"nuke-ai-fill-bootstrap.log";
    for (size_t j = 0; name[j]; ++j, ++i) {
        if (i >= array_size(log_path) - 1) return;
        log_path[i] = name[j];
    }
    log_path[i] = 0;

    g_log_handle = CreateFileW(
        log_path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,   // truncate on each load
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

// Convert wide string to UTF-8 into a caller-provided buffer.
// Returns number of bytes written (excluding any null terminator).
int wide_to_utf8(const wchar_t* w, char* out, int out_size)
{
    if (!w || !out || out_size <= 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, nullptr, nullptr);
    // n includes the null terminator on success; subtract it.
    return (n > 0) ? (n - 1) : 0;
}

// Write a buffer to the log handle. No-op if log is not open.
void log_raw(const char* buf, DWORD len)
{
    if (g_log_handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_log_handle, buf, len, &written, nullptr);
    FlushFileBuffers(g_log_handle);
}

void log_str(const char* s)
{
    DWORD len = 0;
    while (s[len]) ++len;
    log_raw(s, len);
}

void log_line(const char* s)
{
    log_str(s);
    log_raw("\r\n", 2);
}

// Format a DWORD as decimal into buf. Returns length written.
int format_decimal(DWORD value, char* buf, int buf_size)
{
    if (buf_size < 2) return 0;
    char tmp[16];
    int n = 0;
    if (value == 0) { tmp[n++] = '0'; }
    while (value > 0 && n < 15) {
        tmp[n++] = '0' + static_cast<char>(value % 10);
        value /= 10;
    }
    // Reverse into buf.
    int out = 0;
    while (n > 0 && out < buf_size - 1) {
        buf[out++] = tmp[--n];
    }
    buf[out] = 0;
    return out;
}

// Format "Label: <wide-string>\r\n" without using printf.
void log_wide(const char* label, const wchar_t* w)
{
    log_str(label);
    log_str(": ");
    char utf8[1024];
    wide_to_utf8(w, utf8, static_cast<int>(sizeof(utf8)));
    log_str(utf8);
    log_raw("\r\n", 2);
}

void log_decimal(const char* label, DWORD value)
{
    log_str(label);
    log_str(": ");
    char num[16];
    format_decimal(value, num, sizeof(num));
    log_str(num);
    log_raw("\r\n", 2);
}

// ----------------------------------------------------------------------
// The actual preload
// ----------------------------------------------------------------------

bool preload_adjacent_onnxruntime()
{
    log_line("preload_adjacent_onnxruntime: starting");

    HMODULE self = nullptr;
    BOOL got = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&bootstrap_anchor),
        &self);
    if (!got) {
        log_decimal("GetModuleHandleExW failed, GetLastError", GetLastError());
        return false;
    }

    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(self, path, array_size(path));
    if (n == 0 || n >= array_size(path)) {
        log_decimal("GetModuleFileNameW failed, GetLastError", GetLastError());
        return false;
    }
    log_wide("plugin DLL path", path);

    // Strip filename, keep directory (with trailing slash).
    for (DWORD i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i] = 0;
            break;
        }
    }

    // Build "<dir>onnxruntime.dll"
    wchar_t dll_path[MAX_PATH * 4];
    size_t i = 0;
    for (; i < array_size(dll_path) - 16 && path[i]; ++i) {
        dll_path[i] = path[i];
    }
    static const wchar_t suffix[] = L"onnxruntime.dll";
    for (size_t j = 0; suffix[j]; ++j, ++i) {
        if (i >= array_size(dll_path) - 1) {
            log_line("dll_path buffer overflow");
            return false;
        }
        dll_path[i] = suffix[j];
    }
    dll_path[i] = 0;
    log_wide("attempting to load", dll_path);

    // Verify file exists.
    DWORD attrs = GetFileAttributesW(dll_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        log_decimal("file does not exist, GetLastError", GetLastError());
        return false;
    }
    log_line("file exists, calling LoadLibraryExW");

    HMODULE h = LoadLibraryExW(
        dll_path,
        nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!h) {
        DWORD err = GetLastError();
        log_decimal("LoadLibraryExW FAILED, GetLastError", err);
        if (err == 126) {
            log_line("ERROR_MOD_NOT_FOUND - missing dependency for onnxruntime.dll");
            log_line("Run dumpbin /dependents on the dll to find which DLL is missing");
        } else if (err == 193) {
            log_line("ERROR_BAD_EXE_FORMAT - wrong architecture (32/64-bit mismatch?)");
        } else if (err == 1114) {
            log_line("ERROR_DLL_INIT_FAILED - the DLL's DllMain returned false");
        }
        return false;
    }

    wchar_t loaded_path[MAX_PATH * 4];
    DWORD ln = GetModuleFileNameW(h, loaded_path, array_size(loaded_path));
    if (ln > 0 && ln < array_size(loaded_path)) {
        log_wide("LoadLibraryExW succeeded; loaded module path", loaded_path);
    } else {
        log_line("LoadLibraryExW succeeded but could not query loaded path");
    }
    return true;
}

// ----------------------------------------------------------------------
// Static initializer at lib priority. Runs before user-priority statics
// such as ORT's Ort::Global<T>::api_ initializer.
// ----------------------------------------------------------------------

struct EarlyPreloader {
    EarlyPreloader() {
        open_log();
        log_line("EarlyPreloader running at lib-priority static init");
        const bool ok = preload_adjacent_onnxruntime();
        log_line(ok ? "EarlyPreloader: SUCCESS" : "EarlyPreloader: FAILURE");
    }
};

static EarlyPreloader g_early_preloader;

} // anonymous namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE /*inst*/, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        log_line("DllMain DLL_PROCESS_ATTACH");
    }
    return TRUE;
}

#endif // _WIN32
