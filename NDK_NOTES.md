# Nuke NDK Notes

Hard-won gotchas, build rules, and architectural choices for Nuke 14 NDK
plugin development on Windows. Every item below has cost a debugging
round somewhere -- nothing is theoretical.

Distilled from the NukeParticles project's `CLAUDE.md` plus discoveries
made while building this plugin.

---

## 1. Build environment

### 1.1 Compiler version

**Nuke 14 requires Visual Studio 2019.** Not 2022. The Foundry builds Nuke
with VS 2019's compilers; binaries built with later versions are unlikely
to load successfully. CMake generator: `"Visual Studio 16 2019"`.

For other Nuke versions:
- Nuke 13: Visual Studio 2017
- Nuke 15+: still VS 2019 at time of writing, verify against the NDK docs

### 1.2 CMake target

`find_package(Nuke)` exposes `Nuke::NDK`, NOT `Nuke::DDImage`. CMake error
"but the target was not found" almost always means a typo or guess.

```cmake
find_package(Nuke REQUIRED)
target_link_libraries(MyPlugin PRIVATE Nuke::NDK)
```

### 1.3 Required defines

These must be set before any DDImage include:

```cmake
target_compile_definitions(MyPlugin PRIVATE
    NOMINMAX
    _USE_MATH_DEFINES
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
```

`fnVC.h` contains `#error` directives that fail the build without
`NOMINMAX` and `_USE_MATH_DEFINES`. The other two are recommended for
quieter builds.

### 1.4 Compile flags

- **`/W3`, not `/W4`.** DDK headers emit `C4251` (dll-interface), `C4244`
  (streamsize -> int), and a few others at /W4. They live inside Foundry's
  own headers and can't be silenced from plugin code.
- **`/MD`** (dynamic Release CRT) to match Nuke's runtime. Static CRT
  (`/MT`) causes runtime conflicts.
- **`/EHsc`** for standard exception handling.

### 1.5 CMAKE_BUILD_TYPE on multi-config generators

Visual Studio is a multi-config generator. Passing `-DCMAKE_BUILD_TYPE=Release`
at configure time produces "manually-specified variables were not used."
Use `--config Release` at build time instead:

```powershell
cmake -G "Visual Studio 16 2019" -A x64 ...   # no CMAKE_BUILD_TYPE
cmake --build build --config Release
```

### 1.6 Build target type

**`MODULE`, not `SHARED`.** Nuke `LoadLibrary`s plugins; nothing else links
against them. `SHARED` produces a needless `.lib` import library and
confuses CMake's default SHARED-library handling.

```cmake
add_library(MyPlugin MODULE src/MyPlugin.cpp)
```

### 1.7 Output filename

Must exactly match the `Op::Description` name string. Mismatch produces
"plugin did not define X" at Nuke load time, where X is the DLL filename
minus extension. One source file -> one DLL -> one Description.

---

## 2. Windows header collisions

`windows.h` brings in graphics-era cruft that collides with sensibly-named
Nuke types. `WIN32_LEAN_AND_MEAN` does NOT exclude `wingdi.h` -- these
collisions must be handled explicitly.

### 2.1 `POINTS` macro

`wingdi.h` defines `POINTS` as a `#define`, clobbering `DD::Image::Point::POINTS`
enum. Fix: `#undef` after `windows.h`, before any DDImage include:

```cpp
#include <windows.h>
#ifdef POINTS
#  undef POINTS
#endif
#include "DDImage/Point.h"
```

### 2.2 `Polygon` function

`wingdi.h` declares a global `Polygon(HDC, ...)` function (GDI drawing
primitive) that collides with `DD::Image::Polygon` class. Different from
POINTS -- it's a function, not a macro, so `#undef` won't work. Fix: fully
qualify the class:

```cpp
DD::Image::Polygon* poly = new DD::Image::Polygon(/*vertices=*/4, /*closed=*/true);
```

A `using namespace DD::Image;` alone doesn't disambiguate; both names are
in scope and the compiler can't choose between class and function.

---

## 3. NDK API gotchas

### 3.1 ModifyGeo is for deformers, not topology changes

`ModifyGeo` is documented as a deformer harness: it copies the input point
list into the output and assumes the plugin only mutates points. It has
no API to delete primitives on a single object. For topology-changing
operations (simplification, remeshing, anything that adds/removes faces),
subclass `GeoOp` directly and override `geometry_engine`.

### 3.2 Topology-rewrite pattern

```cpp
void geometry_engine(Scene& scene, GeometryList& out) override {
    GeoOp* in = dynamic_cast<GeoOp*>(input0());
    if (!in) return;
    in->get_geometry(scene, out);

    // Snapshot all objects' data FIRST. After delete_objects() runs,
    // the input GeoInfos are gone and cannot be read.
    std::vector<MyResult> results(out.objects());
    for (unsigned obj = 0; obj < out.objects(); ++obj) {
        results[obj] = processObject(out[obj]);
    }

    out.delete_objects();

    for (unsigned obj = 0; obj < results.size(); ++obj) {
        out.add_object(obj);
        // ... add_primitive(), writable_points(), writable_attribute() ...
    }
}
```

### 3.3 `get_typed_attribute` takes 2 args

```cpp
const Attribute* a = info.get_typed_attribute("uv", VECTOR4_ATTRIB);
```

No group parameter. It walks all groups in priority order (vertex first,
then point) and returns the first match of the specified type. If you
need to constrain by group, use `get_group_attribute(group, name)` (takes
2 args but no type) and check `type` on the result.

### 3.4 `delete_primitives` doesn't exist

`GeometryList` has no per-object primitive deletion. Either:

- `delete_objects()` wipes the entire list and you rebuild (the right
  approach inside `geometry_engine`)
- Stay within ModifyGeo's contract and only mutate points

### 3.5 `vertex_offset_` is protected

The trailing underscore is Foundry's convention for protected backing
members. The public accessor drops the underscore:

```cpp
const unsigned slot = prim->vertex_offset() + local_v;  // OK
const unsigned bad  = prim->vertex_offset_ + local_v;   // C2248
```

### 3.6 UV attribute formats

Nuke's classic 3D system stores UVs on `Group_Vertices` as `VECTOR4_ATTRIB`
natively (s, t, r, q for homogeneous unprojection). Some readers and
external pipelines produce `VECTOR2_ATTRIB`. Defensive code handles both:

```cpp
const Attribute* uv = info.get_typed_attribute("uv", VECTOR4_ATTRIB);
bool uv_is_v2 = false;
if (!uv) {
    uv = info.get_typed_attribute("uv", VECTOR2_ATTRIB);
    uv_is_v2 = (uv != nullptr);
}
```

UV is per-corner (`Group_Vertices`), not per-point. Two corners that share
a point but have different UVs are how UV seams are represented in Nuke.

### 3.7 SoA pointer invalidation

After any call that may resize Nuke's internal buffers (`writable_points`
followed by `resize`, attribute resize, etc.), re-fetch pointers before
use. Caching `Vector3* P = ...; resize(N); P[i] = ...;` is undefined
behavior -- the second access reads stale memory.

Pattern: do all allocations first, fetch pointers once afterward, bulk
fill, never cache across resizes.

---

## 4. TCL evaluation trap

Nuke runs string knob values through a TCL evaluator before storing or
displaying. `[...]`, `{...}`, `$var`, and `\` are all special.

- `[foo bar]` is parsed as "call TCL command `foo` with argument `bar`."
  Unknown command -> the knob value becomes an error string like
  "missing close-bracket."
- Backslashes are escape characters: `C:\dev\NukeParticles` becomes
  `C:devNukeParticles` (because `\d` and `\N` get consumed). `\b` is a
  backspace that DELETES the preceding character. `\n` is a newline.
  `\u` starts a unicode escape.

**Mitigations:**

1. **Multiline string knobs (JSON, config text):** call
   `SetFlags(f, Knob::NO_ANIMATION)` immediately after the knob
   declaration. The DDK docs explicitly note that flag "stops TCL
   expressions from being evaluated in string knobs."
2. **File paths:** never use backslashes. Use forward slashes everywhere
   (Windows file APIs accept both). When converting from
   `std::filesystem::path`, use `.generic_string()` not `.string()`.
3. **Strip whitespace from File_knob values** before use. Nuke occasionally
   appends a trailing newline.
4. **Prompt knobs for AI plugins:** Stable Diffusion / FLUX prompts use
   `(word:1.2)` weighting syntax and bracket emphasis like `[neg]`. Both
   collide with TCL. `Knob::NO_ANIMATION` on prompt knobs is mandatory,
   not optional.

---

## 5. Threading rules

Production-derived. Each one has crashed Nuke in a previous project.

### 5.1 UI-thread-only operations

Never call these from a worker thread (`engine`, `geometry_engine`, any
`tbb::parallel_for`, bake worker threads, CUDA stream callbacks):

- `_close()`, `invalidate()`, `asapUpdate()`
- `Knob::set_text`, `Knob::set_value`, `Knob::changed()`
- Any other knob mutation

All knob mutation happens on the main thread, in `knob_changed` or
`_validate`.

### 5.2 Defensive try/catch at boundaries

Every Op callback running in Nuke-owned code (`knob_changed`, `execute`,
`engine`, `geometry_engine`, `_validate`) wraps its body in try/catch.
**Unhandled C++ exceptions escaping into Nuke's call stack crash the
host.** The catch + log + return-with-no-op pattern keeps the session
alive and leaves a trail.

```cpp
void geometry_engine(Scene& scene, GeometryList& out) override {
    try {
        realWork(scene, out);
    }
    catch (const std::exception& e) {
        error("MyPlugin failed: %s", e.what());
    }
    catch (...) {
        error("MyPlugin failed: unknown exception");
    }
}
```

### 5.3 Background-worker -> main-thread notification

For long-running tasks (ML inference, network fetches, anything that
cannot run inside `engine()`), the worker thread:

1. Must NOT touch knobs (per 5.1)
2. Writes its result to a content-addressed cache (file path
   precomputed on the main thread before launch)
3. Flips an atomic state flag (Idle / Running / Done / Error / Cancelled)
4. Exits cleanly

The main thread polls the state flag in `_validate`. To trigger
`_validate` while the user is idle, a Python idle timer registered from
`menu.py` calls `forceValidate()` on any node whose status knob reports
Cooking, then auto-deregisters when no cooking nodes remain.

Cancellation is cooperative: the worker checks an atomic flag between
work units (denoising steps, tile iterations) and exits early. Never
forcibly kill threads -- destructors of session objects (ORT, ggml, CUDA
streams) must run.

### 5.4 Content-addressed cache keys use upstream hash, not pixels

Do NOT hash pixel data directly to build cache keys. A 4K RGBA image is
~130 MB to SHA-256 (~400ms per `_validate` call) and -- more importantly --
the cost is redundant. Nuke's `Op::hash_op()` already incorporates all
upstream node state including time and parameters.

```cpp
const uint64_t src_hash = input0()->hash_op().getHash();
```

Key the cache on `src_hash` + the current Op's knob values + model
file paths and mtimes. Sampled pixel hashing was tried and failed -- a
64-sample grid leaves gaps where single-pixel changes are invisible.

---

## 6. Source file rules

### 6.1 Strict ASCII in C++ sources

`.h`, `.cpp`, `.cu`, `.cuh` files are pure ASCII. No smart quotes,
em-dashes, ellipses, en-dashes, or non-breaking spaces. Comments included.

UTF-8 is fine for `.md`, `.json`, `.py`, and shader string literals.

This is not aesthetic preference -- it has caused MSVC, nvcc, and
multi-encoding tooling to silently corrupt builds in production projects.

Quick check:

```bash
python3 -c "import sys; d=open('file.cpp','rb').read(); print(not any(b>=0x80 for b in d))"
```

PowerShell equivalent:

```powershell
$bad = [IO.File]::ReadAllBytes('file.cpp') | Where-Object { $_ -ge 0x80 }
if ($bad) { Write-Host "NON-ASCII present" } else { Write-Host "ASCII clean" }
```

Deploy scripts should validate this before placing source files.

---

## 7. Plugin development workflow

### 7.1 Hot reload

After rebuilding a plugin DLL on disk, Nuke's in-memory registry is stale
until one of:

- Restart Nuke
- "Update All Plugins" from the menu (releases file locks briefly)
- `nuke.pluginAddPath(r"<path>", addToSysPath=False)` from Script Editor

### 7.2 File lock

A running Nuke holds a file lock on currently-loaded plugin DLLs. Linker
fails with "permission denied" trying to overwrite. Either close Nuke for
full rebuilds, or develop with "Update All Plugins" in mind.

### 7.3 Knob defaults are evaluated once per node construction

Changing a knob's default value in source code only affects nodes added
after the change. Existing nodes in a saved `.nk` script keep their
serialized values until reset. During development, **always add a fresh
node** to test default-value changes.

### 7.4 GUI Nuke eats stderr on Windows

`printf` and `std::cerr` from a plugin DLL go nowhere when Nuke is started
as a GUI process. Use the Op's `error()` / `warning()` for anything the
user needs to see, or write to a log file directly (mutex-protected,
flushed on every write).

---

## 8. Distribution

### 8.1 Plugin folder pattern

Don't drop plugin files directly into `%USERPROFILE%\.nuke\`. Use a
subfolder:

```
%USERPROFILE%\.nuke\
|-- menu.py                           # user's customizations
`-- MyPlugin\                         # one folder per plugin
    |-- MyPlugin.dll
    |-- menu.py                       # plugin's own toolbar registration
    `-- ABOUT.txt
```

Then add `nuke.pluginAddPath('./MyPlugin')` to `.nuke\menu.py`. Each
plugin stays isolated; the main menu.py doesn't get polluted; uninstall
is just deleting the subfolder.

### 8.2 Idempotent menu.py append

When auto-installing, append to the user's `.nuke\menu.py` with a marker
comment:

```python
# --- MyPlugin (auto-added by deploy.ps1) ---
import nuke
nuke.pluginAddPath('./MyPlugin')
# --- end MyPlugin ---
```

Detect the marker on subsequent runs and skip; never overwrite the file
wholesale, never duplicate the block.

### 8.3 License redistribution

If you bundle MIT or BSD-licensed dependencies (meshoptimizer, etc.), the
dependency's license text MUST travel with your binary. The MIT license
specifies: "The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software."

Reproduce the upstream license under a "Third-party software" section in
your own LICENSE file, and ship LICENSE alongside the DLL in your
distribution zip.

### 8.4 cmake --install + install(SCRIPT) is the canonical deploy

When CMake already drives the build, prefer its install machinery over
hand-rolled PowerShell. Combine `install(TARGETS)` and `install(FILES)`
for artifacts with `install(SCRIPT)` for the idempotent menu.py append
described in 8.2:

```cmake
install(TARGETS MyPlugin DESTINATION my-plugin)
install(FILES menu.py    DESTINATION my-plugin)
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_user_menu.cmake")
```

The script gets `CMAKE_INSTALL_PREFIX` set to whatever `--prefix` the
user passed, so `${CMAKE_INSTALL_PREFIX}/menu.py` resolves correctly to
`~/.nuke/menu.py` when called as:

```powershell
cmake --install build --config Release --prefix "$env:USERPROFILE\.nuke"
```

The script reads existing menu.py content, checks for the marker
comment, and either appends the registration block or no-ops. Same
guarantees as 8.2, in CMake.

This obviates needing a separate deploy step after build.

---

## 9. meshoptimizer specifics

(Relevant to this project; general guidance for any project using
meshoptimizer in a Nuke plugin.)

### 9.1 vertex_lock support

`vertex_lock` (per-vertex protect flag) is only honored by
`meshopt_simplifyWithAttributes`. `meshopt_simplifySloppy` ignores it
silently. Any "preserve features" behavior must use the non-sloppy path.

### 9.2 Attribute weight semantics

In `meshopt_simplifyWithAttributes`, the `attribute_weights` array is
PER-FLOAT, not per-attribute. A 2D UV with weights `{1.0f, 1.0f}` means
"weight u and v equally at 1.0 each." For per-attribute control, set both
floats to the same value.

### 9.3 Simplifier collapses entirely

If the simplifier returns 0 indices, the mesh collapsed to nothing
(target_error too high or topology degenerate). Don't emit a zero-triangle
object downstream -- warn and skip, or pass the original through.

### 9.4 The fat-vertex bridge

Nuke stores UVs per-corner (`Group_Vertices`). meshoptimizer wants
per-vertex attributes. The bridge: build "fat vertices" keyed by
(point_index, u, v) tuples. UV seams in the input become unique fat
vertices that the simplifier naturally preserves as topological
discontinuities.

For seam-ignoring modes, key by point_index alone -- pick any one of the
UVs at that point (first-seen) as the representative.

---

## 10. PowerShell script signing

Downloaded `.ps1` files from this chat (or anywhere) are marked as
"from internet" via NTFS Zone.Identifier, and most default execution
policies will refuse to run them. Fixes:

```powershell
# Quickest, single-run:
powershell -ExecutionPolicy Bypass -File .\deploy.ps1

# Clear the mark on a specific file:
Unblock-File .\deploy.ps1

# Permanent fix for the current user (one-time):
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```

`RemoteSigned` is the sensible default: local scripts run, downloaded
ones need `Unblock-File` once.

---

## 11. find_package(Nuke) gotchas

Multi-Nuke installs make CMake's package discovery treacherous. Hit
during nuke-ai-fill bring-up against Nuke 14.1v8 with Nuke 17.0v1 also
installed.

### 11.1 NukeConfig.cmake lives in a subdirectory, not the install root

`find_package(Nuke)` wants the path passed via `-DNuke_DIR` to be the
directory *containing* `NukeConfig.cmake`. For Nuke 14.1v8 (and
probably all modern Nuke), that's `<install>/cmake/`, not the install
root:

```
C:/Program Files/Nuke14.1v8/cmake/NukeConfig.cmake   <-- the actual file
C:/Program Files/Nuke14.1v8/                          <-- install root, NOT this
```

CMake invocation:

```cmake
-DNuke_DIR="C:/Program Files/Nuke14.1v8/cmake"
```

If you point at the install root, CMake silently falls through to other
search paths -- see 11.2.

### 11.2 CMake user-package-registry can ambush your -DNuke_DIR

CMake searches for Nuke in this priority order:

1. `Nuke_DIR` cache variable
2. CMake user-package-registry (`HKCU\Software\Kitware\CMake\Packages\Nuke`)
3. Standard system paths

If your `-DNuke_DIR` points at a directory that does NOT contain
`NukeConfig.cmake`, CMake silently moves to step 2. Nuke installers
(observed in Nuke 17.0+, possibly earlier) register themselves in the
user-package-registry. Result: you ask for Nuke 14 and get Nuke 17
without any warning.

**Always confirm the resolved version in the configure output:**

```
-- Found NDK: C:/Program Files/Nuke14.1v8   <-- check this matches intent
```

If the wrong Nuke is selected despite a correct `-DNuke_DIR`, bypass the
registry:

```cmake
# At project scope:
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
find_package(Nuke REQUIRED)

# Or per-call:
find_package(Nuke REQUIRED NO_CMAKE_PACKAGE_REGISTRY)
```

To inspect the registry from PowerShell:

```powershell
Get-ChildItem 'HKCU:\Software\Kitware\CMake\Packages\Nuke' -ErrorAction SilentlyContinue
```

### 11.3 install(FILES ... OPTIONAL) is unreliable on Windows

The CMake docs say `OPTIONAL` suppresses install-time errors for files
that don't exist. In practice on Windows it can fail with a confusing
error:

```
file INSTALL cannot find "C:/path/to/LICENSE": File exists.
```

Workaround: gate install rules with a configure-time `if(EXISTS)` check
instead of relying on OPTIONAL:

```cmake
foreach(_doc IN ITEMS LICENSE README.md THIRD_PARTY_LICENSES.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}"
                DESTINATION my-plugin)
    endif()
endforeach()
```

Re-run `cmake` configure after creating the file to pick it up.

### 11.4 pwsh.exe warning during build is benign

Foundry's `NukeConfig.cmake` invokes PowerShell Core (`pwsh.exe`) at
some stage. If you only have Windows PowerShell (`powershell.exe`)
installed, you'll see:

```
'pwsh.exe' is not recognized as an internal or external command,
operable program or batch file.
```

Doesn't affect the build -- the DLL still gets produced correctly.
Install PowerShell Core from the Microsoft Store or
github.com/PowerShell/PowerShell to silence the warning. Purely cosmetic.

### 11.5 Clear the build dir when changing Nuke_DIR

CMake caches the found Nuke path in `build/CMakeCache.txt`. If
configure picked the wrong Nuke version the first time, re-running
configure with a corrected `-DNuke_DIR` may stick with the cached
value. Nuke the build dir:

```powershell
Remove-Item C:\dev\MyPlugin\build -Recurse -Force
```

Then re-configure from scratch.

---

## 12. Windows ships its own onnxruntime.dll in System32

This is a Windows 11 build-onward thing. The OS includes ONNX Runtime
at `C:\Windows\System32\onnxruntime.dll` as part of the Windows AI
Machine Learning infrastructure (HKLM\Software\Microsoft\Windows\
CurrentVersion\AppModel\Runtime, and the WinSxS
`amd64_microsoft-windows-ai-machinelearning_*` assemblies).

### 12.1 Why it bites

Windows' DLL search order for a plugin DLL loading dependencies:

1. Already-loaded modules with same base name
2. Application directory (Nuke's directory)
3. **System32** -- wins here
4. PATH
5. Directory of the loading DLL (our plugin folder)

Our plugin's bundled `onnxruntime.dll` (modern ORT, API 20+) sits at
step 5, never reached. System32's older copy (API 17) loads instead.
Symptom: `"The requested API version [20] is not available, only API
versions [1, 17] are supported in this build. Current ORT Version is:
1.17.1"` thrown from `Ort::Global<T>::api_` static initialization.

### 12.2 The fix: lib-priority static init + DELAYLOAD

Two combined tricks force our copy to win:

**A) `/DELAYLOAD:onnxruntime.dll`** at link time. Defers implicit
import resolution from "DLL load time" to "first call to an imported
symbol". Without this, System32's ORT is bound at load before our
DllMain runs.

```cmake
target_link_options(MyPlugin PRIVATE "/DELAYLOAD:onnxruntime.dll")
target_link_libraries(MyPlugin PRIVATE delayimp.lib)
```

**B) `#pragma init_seg(lib)`** plus a global static helper that calls
`LoadLibraryExW` on the adjacent `onnxruntime.dll` with
`LOAD_WITH_ALTERED_SEARCH_PATH`. This runs at lib-priority static
init, which fires BEFORE the user-priority static init where
ORT's `Global<T>::api_` is constructed. By the time `OrtGetApiBase()`
is called, our DLL is already in the loaded-module table.

```cpp
#pragma init_seg(lib)

namespace {
struct EarlyPreloader {
    EarlyPreloader() { preload_adjacent_onnxruntime(); }
};
static EarlyPreloader g_early_preloader;
}
```

### 12.3 No CRT in the early preloader

`init_seg(lib)` runs very early. The CRT is not fully initialized.
`fopen`, `printf`, `_wfopen_s`, even `std::string` constructors that
allocate -- all can crash. Use Win32 only: `CreateFileW`, `WriteFile`,
`GetModuleHandleExW`, `GetModuleFileNameW`, `LoadLibraryExW`,
`WideCharToMultiByte`. Format numbers and strings by hand into stack
buffers.

### 12.4 Diagnostic log to %TEMP%

When the preload silently fails, you have nothing. Write a log via
`CreateFileW` + `WriteFile` to `%TEMP%\<plugin>-bootstrap.log` at the
top of every static init step. After a failed plugin load, read the
log to see exactly where it died. Common `GetLastError()` values:
- 126 (ERROR_MOD_NOT_FOUND): missing dependent DLL
- 193 (ERROR_BAD_EXE_FORMAT): 32/64-bit mismatch
- 1114 (ERROR_DLL_INIT_FAILED): the DLL's DllMain returned FALSE

### 12.5 Verifying the right DLL got loaded

After triggering an ORT call in Nuke (e.g. by pressing a Bake button),
PowerShell can confirm which onnxruntime.dll is actually loaded:

```powershell
$nuke = Get-Process | Where-Object { $_.ProcessName -match 'Nuke' } |
        Sort-Object StartTime -Descending | Select-Object -First 1
$nuke.Modules | Where-Object { $_.ModuleName -like 'onnx*' } |
    Select-Object ModuleName, FileName, FileVersion |
    Format-Table -AutoSize
```

Wrong: `C:\Windows\System32\onnxruntime.dll`
Right: `<plugin install dir>\onnxruntime.dll`

This same trick generalizes to any DLL Windows tries to satisfy from
System32 (CUDA, OpenCL, anything else AI-related).

---

## 13. DDImage knob storage types are not std::string

`File_knob`, `String_knob`, and similar string-backed knobs take
`const char**`, not `std::string*`. Compile error:

```
error C2664: cannot convert argument 2 from 'std::string *' to 'const char **'
```

The correct pattern is `const char*` member variables with empty-string
or nullptr initial values:

```cpp
class MyOp : public Iop {
    const char* model_path_;
    const char* status_text_;
public:
    MyOp(Node* n)
        : Iop(n)
        , model_path_("")
        , status_text_("(idle)")
    {}

    void knobs(Knob_Callback f) override {
        File_knob(f, &model_path_, "model_path", "Model");
        String_knob(f, &status_text_, "status", "Status");
    }
};
```

### 13.1 Programmatic mutation: Knob::set_text, never direct assignment

Writing directly to the backing pointer (`status_text_ = "new value"`)
updates memory but does NOT cause the panel display to refresh. The
correct path is via the knob object:

```cpp
void set_status(const char* s) {
    if (Knob* k = knob("status")) {
        k->set_text(s ? s : "");
    }
}
```

Per NDK_NOTES 5.1, `Knob::set_text` is main-thread only. Call from
`knob_changed`, `_validate`, or other UI-thread contexts. Never from
a worker thread.

### 13.2 Numeric knobs: same rule, Knob::set_value

Same pattern for `Float_knob`, `Int_knob` etc -- direct writes to the
backing variable update memory but do not refresh the panel. Use
`Knob::set_value(double)`:

```cpp
void set_progress(float v) {
    progress_ = v;  // keep our copy in sync
    if (Knob* k = knob("progress")) {
        k->set_value(static_cast<double>(v));
    }
}
```

### 13.3 Reading knob values from C++

The backing `const char*` member IS updated by Nuke when the user types
into the panel. Reading is just `const char* current = model_path_;`.
Null-defensive wrapper for use with std::string APIs:

```cpp
std::string ck_str(const char* s) {
    return s ? std::string(s) : std::string{};
}
```

---

## 14. Output cache invalidation requires more than invalidate()

When an Op's `engine()` output depends on internal state that the
input-chain hash doesn't capture (e.g. a disk cache that becomes
readable after a background bake), three things must all be done for
Nuke to recook and the viewer to refresh:

### 14.1 Override append(Hash&) to include internal state

Nuke's default `Op::append` only hashes the input chain. If your
output depends on something else (loaded cache buffer, model path,
internal version counter), include it in `append`:

```cpp
void append(Hash& hash) override {
    Iop::append(hash);
    hash.append(loaded_key_);  // changes when cache loads
}
```

Without this, the hash stays identical before and after the bake
completes, and Nuke serves the cooked-from-stale-state pixels even
after `invalidate()`.

### 14.2 Call invalidate() AND asapUpdate() at the transition

`invalidate()` marks the op cache stale. `asapUpdate()` triggers the
UI redraw that actually requests the recook. One without the other
is insufficient in practice:

```cpp
if (cache_just_became_available()) {
    invalidate();
    asapUpdate();
}
```

Both are main-thread only.

### 14.3 The viewer-refresh problem may still persist

Even with append override + invalidate + asapUpdate, the viewer may
not auto-refresh after a long-running background bake completes. The
phenomenon: data is on disk, hash changes, but the viewer keeps
showing the pre-bake cooked pixels until the user manually nudges
something (disconnect/reconnect an input, change a knob).

This is the same class of problem Foundry's `Inference` node solves
internally with hooks the public NDK doesn't expose. Workarounds
from Python via menu.py polling:

```python
def _nudge_viewer():
    try:
        v = nuke.activeViewer()
        if v is None: return
        # Touching a Viewer knob value forces it to recook downstream.
        # 'hide_input' is a safe no-op knob to flip.
        k = v.node().knob('hide_input')
        if k is not None:
            k.setValue(k.value())
    except Exception:
        pass
```

This works imperfectly. The honest assessment: full auto-refresh of
the Viewer from a custom op after a background bake remains an open
problem.

---

## 15. Python knob_changed callbacks miss C++ knob mutations

`nuke.addKnobChanged(callback, nodeClass='MyOp')` fires for user-driven
knob changes (typing, button presses, slider drags). It does NOT
reliably fire when C++ code mutates a knob during the same handler --
Nuke has re-entry guards.

### 15.1 Don't trigger Python polling from knob events

If you want Python code to react to a Bake button press, do not write:

```python
def callback():
    knob = nuke.thisKnob()
    if knob.name() == 'bake':  # may never fire
        start_polling()
```

If your C++ `knob_changed` mutates other knobs during the bake-press
handler (like updating a status display), Python's `nuke.thisKnob()`
reports the last-mutated knob ('status'), not the user's actual press
('bake').

### 15.2 Continuous polling is bulletproof

Run a main-thread timer unconditionally; check node state in the
poll function:

```python
import threading
import nuke

_POLL_INTERVAL_S = 0.3
_active_markers = ("Cooking", "Writing", "Loading")

def _poll_step():
    try:
        for n in nuke.allNodes():
            if n.Class() != "MyOp": continue
            sk = n.knob("status")
            if sk is None: continue
            if any(m in sk.value() for m in _active_markers):
                n.forceValidate()
    except Exception:
        pass
    # Always reschedule -- cheap and reliable
    t = threading.Timer(_POLL_INTERVAL_S,
                       lambda: nuke.executeInMainThread(_poll_step))
    t.daemon = True
    t.start()

nuke.executeInMainThread(_poll_step)
```

Cost: ~3 node iterations per second when nothing is happening.
Negligible. The "always reschedule, no trigger detection" pattern is
the only one that's reliably worked across Nuke versions and across
the re-entry edge cases.

---

## 16. ONNX model preprocessing must match the export's convention

Different ONNX exports of the same model architecture can use radically
different preprocessing. Document conventions from the model's README
or HF discussions; do not assume.

### 16.1 LaMa-ONNX (Carve) specific

From huggingface.co/Carve/LaMa-ONNX discussions:

- **Input image**: float32 in **[0, 1]**, layout NCHW (1,3,512,512)
- **Input mask**: float32 binary 0/1, layout NCHW (1,1,512,512), 1 = inpaint
- **NO pre-masking** of input image -- the network handles internally
- **Output**: float32 in **[0, 255]** -- divide by 255 to get [0, 1]

The original PyTorch model output is [0,1]; the ONNX export multiplies
by 255 internally. Missing the /255 produces values up to ~230 in the
inpainted region, which Nuke's viewer clips to white.

### 16.2 Reading the model spec from Python

When in doubt, introspect:

```python
import onnxruntime as ort
sess = ort.InferenceSession(r"path/to/model.onnx")
for inp in sess.get_inputs():
    print(f"Input  {inp.name:20} shape={inp.shape} type={inp.type}")
for out in sess.get_outputs():
    print(f"Output {out.name:20} shape={out.shape} type={out.type}")
meta = sess.get_modelmeta()
print(f"Custom metadata: {dict(meta.custom_metadata_map)}")
```

Custom metadata is empty for most community exports; you'll have to
read the model's docs or test empirically.

### 16.3 Diagnosing range mismatches via cache inspection

If output looks wrong (white blobs, weird colors), inspect the actual
written values, not what the model "should" output:

```python
import struct, numpy as np
with open(cache_path, 'rb') as f:
    f.read(8)  # magic
    version, w, h, c, _ = struct.unpack('<IIIII', f.read(20))
    data = np.frombuffer(f.read(), dtype=np.float32).reshape(h, w, c)
print(f"min={data.min()} max={data.max()} mean={data.mean()}")
```

`max > 2` strongly suggests an output-scale mismatch (model emits
[0,255], code treats as [0,1]).

---

## 17. Content-addressed disk caches and async bakes

The combined pattern -- background worker, Bake button, content-keyed
disk cache, lazy in-memory load -- works well for slow ML inference.
Several details matter.

### 17.1 Cache key must be deterministic

The key is SHA-256 of (input op hashes + all knobs that affect output
+ model file path). Cache hits short-circuit the whole bake; cache
misses spawn the worker. Misses on identical inputs mean the key
function is non-deterministic somewhere.

```cpp
nf::CacheKeyBuilder kb;
kb.add(std::string("MyOp.v1"));
if (input(0)) {
    Hash h;
    input(0)->append(h);
    kb.add_op_hash(static_cast<uint64_t>(h.value()));
}
kb.add(threshold_);  // every knob that affects output
kb.add(static_cast<int32_t>(backend_));
kb.add(normalize_path(ck_str(model_path_)));
return kb.finalize();
```

### 17.2 Atomic write via .tmp + rename

Writing the cache file directly is unsafe: a crash mid-write leaves
a corrupted file that a concurrent reader will load as garbage.
Standard fix:

```cpp
const std::string tmp_path = path + ".tmp";
// write to tmp_path
std::filesystem::remove(path);  // Windows: rename won't overwrite
std::filesystem::rename(tmp_path, path);
```

### 17.3 Cache invalidation on code changes

Cache files persist across plugin rebuilds. If a buggy version of
the plugin wrote bad data, subsequent fixed-version bakes hit the
"already cached" short-circuit and serve the bad data. ALWAYS clear
the cache when changing code that affects output:

```powershell
Remove-Item "$env:APPDATA\<plugin>\cache\*.aifill" -Force
```

Include a Clear Cache button in the Op's panel for users.

### 17.4 Worker thread can't touch knobs

Per NDK_NOTES 5.1, only main thread touches knobs. The worker
writes progress and status to an atomic state structure
(`AiWorkerContext`); the main-thread polling (via menu.py timer
firing `forceValidate`) reads that state during `_validate` and
updates the knobs.

---

## 18. Trust library `_init` functions; override only what you control

When a third-party C API provides struct-init functions (`*_init`,
`*_defaults`, etc.), the values they set are not placeholder
defaults -- they encode internal assumptions about how downstream
code will use the struct. Overriding fields with values that look
right based on documentation is a common path to silent failures
that take hours to diagnose.

### 18.1 The stable-diffusion.cpp lesson

We spent half a day debugging FLUX schnell generation producing
all-white output (every pixel = 1.0). The pipeline ran end-to-end:
model loaded, conditioner ran, sampling ran for the full 4 steps,
VAE decoded, image was written to cache. Just every pixel was 1.0.

The cause was NaN propagation through inference. The cause of THAT
was that we had overridden fields in `sd_ctx_params_t` and
`sd_img_gen_params_t` with values from sd.cpp's documentation and
the FLUX paper:

```cpp
// Looks reasonable. Is wrong.
ctx_params.prediction              = FLUX_FLOW_PRED;
ctx_params.wtype                   = SD_TYPE_Q4_0;
ctx_params.rng_type                = CUDA_RNG;
ctx_params.sampler_rng_type        = CUDA_RNG;
ctx_params.backend                 = "CUDA";

gen_params.sample_params.scheduler                   = DISCRETE_SCHEDULER;
gen_params.sample_params.eta                         = 0.0f;
gen_params.sample_params.guidance.distilled_guidance = 0.0f;
gen_params.clip_skip                                 = 0;
```

Each value justifiable in isolation. Combined, they broke inference.

The fix:

```cpp
sd_ctx_params_init(&ctx_params);

// Override ONLY the paths and a few essentials:
ctx_params.diffusion_model_path = ...;
ctx_params.vae_path             = ...;
ctx_params.clip_l_path          = ...;
ctx_params.t5xxl_path           = ...;
ctx_params.n_threads            = -1;
ctx_params.vae_decode_only      = false;
ctx_params.free_params_immediately = false;
ctx_params.enable_mmap          = false;
ctx_params.lora_apply_mode      = LORA_APPLY_AUTO;

// Everything else - wtype, prediction, scheduler, RNGs, backend -
// stays at _init's defaults. sd.cpp auto-detects the right values
// from the loaded model file.
```

### 18.2 The diagnostic technique

When a library ships a reference CLI tool, that tool is the ground
truth for "how this library should be called." For sd.cpp it's
`sd-cli.exe`; for ONNX Runtime it's `onnxruntime_perf_test`; etc.

If your library call isn't producing expected output, run the
reference CLI with the SAME inputs and `-v`/`--verbose` to get a
dump of the parameter struct. Compare its values against yours,
field by field. The divergence will jump out:

```
sd-cli (works):                  our code (broken):
  wtype: NONE                      wtype: Q4_0
  prediction: NONE                 prediction: FLUX_FLOW_PRED
  scheduler: NONE                  scheduler: DISCRETE_SCHEDULER
  eta: inf  (sentinel)             eta: 0.0
  distilled_guidance: 3.50         distilled_guidance: 0.0
```

`NONE` and `inf` in those dumps are sentinel values that mean
"use the library's auto-detect / sampler-default behavior." They
are NOT placeholder zeros. Setting them to `0` or `0.0f` is what
causes the NaN cascade.

### 18.3 General principle

For any library struct with an `_init` function:

1. Call `_init` first.
2. Override the user-controlled fields (paths, dimensions, prompts).
3. Override fields where you have a documented, non-default reason
   (vae_decode_only=false because we may add img2img later).
4. Leave everything else alone.

Comments documenting why you DIDN'T set certain fields are as
valuable as comments documenting why you DID set others. Both
save the next person (often future you) hours of debugging.

---

## 19. Diffusion VAE compute buffer is the hidden VRAM cost

When budgeting VRAM for diffusion model inference, the model
weights are not the largest allocation. The VAE decode compute
buffer can be larger than the entire diffusion model.

### 19.1 Numbers for FLUX at 1024x1024

```
flux params (weights):     ~6.5 GB
flux compute buffer:       ~2.4 GB
t5 params (weights):       ~4.8 GB
clip_l params (weights):   ~0.25 GB
vae params (weights):      ~0.16 GB
vae compute buffer:        ~6.6 GB    <-- larger than everything else
```

This is allocated AFTER the diffusion sampling completes, just
before the latent gets decoded. So a generation can sample
successfully through all 4 (or 20, or 50) denoising steps and
then OOM during VAE decode.

### 19.2 VAE tiling fixes it

sd.cpp exposes tiled VAE decode that splits the latent into tiles
and decodes them one at a time. Compute buffer drops to a few
hundred MB:

```cpp
gen_params.vae_tiling_params.enabled         = true;
gen_params.vae_tiling_params.temporal_tiling = false;
gen_params.vae_tiling_params.tile_size_x     = 0;     // auto
gen_params.vae_tiling_params.tile_size_y     = 0;
gen_params.vae_tiling_params.target_overlap  = 0.5f;
gen_params.vae_tiling_params.rel_size_x      = 0.5f;  // tile = 1/2 image
gen_params.vae_tiling_params.rel_size_y      = 0.5f;
gen_params.vae_tiling_params.extra_tiling_args = nullptr;
```

Trade-off: tiled decode is ~20% slower (the overlap region gets
decoded twice and blended). Seams between tiles are visible if
overlap is small; 0.5 overlap hides them effectively.

For FLUX at 1024x1024 with `rel_size = 0.5`, sd.cpp uses 9 tiles
(3x3) of 512x512 with overlap blending, dropping VAE buffer from
6.6 GB to ~1.5 GB.

### 19.3 General pattern

For any image-generative model:
- Model weights = "static" VRAM (loaded once, reused).
- Sampling compute buffer = activations during denoising.
- VAE/decoder compute buffer = activations during pixel synthesis.

The last one is usually the biggest single allocation and gets
forgotten in capacity planning. If your card has tight VRAM, look
for the tiling/chunked-decode option before lowering quality.

---

## 20. GGUF file format compatibility is not implied by extension

GGUF is a container format, not a quantization scheme. Files with
the same `.gguf` extension and similar names can use different
internal conventions for quantization scale storage. They will
load successfully in libraries that don't recognize the mismatch,
then produce garbage during inference.

### 20.1 The specific incompatibility we hit

`flux1-schnell-Q4_K.gguf` files exist in multiple HuggingFace
repos:

| Repo | Target tool | Works with sd.cpp? |
|---|---|---|
| `leejet/FLUX.1-schnell-gguf` | stable-diffusion.cpp | yes |
| `city96/FLUX.1-schnell-gguf` | ComfyUI-GGUF custom node | NO |
| `lllyasviel/FLUX.1-schnell-gguf` | ComfyUI-GGUF custom node | NO |
| `unsloth/FLUX.1-schnell-GGUF` | Unsloth | unknown |
| `calcuis/flux1-gguf` | unclear | NO |

These files all open with sd.cpp's GGUF loader. The tensor counts
match. The architecture detection succeeds (sd.cpp prints "Version:
Flux"). The model loads. Inference runs. The output is white.

The bug is that ComfyUI-GGUF stores Q4_K scale factors in a
different block layout than what sd.cpp dequantizes. sd.cpp reads
the wrong bytes as scale factors, dequantizes weights to garbage,
the weights produce NaN activations, the VAE saturates output to
1.0.

### 20.2 How to tell

Only safe heuristic: source from the same author or repo that
shipped the inference library. For sd.cpp specifically:

- `leejet/FLUX.1-schnell-gguf` (only `Q2_K`, `Q4_0`, `Q8_0` exist)
- `leejet/FLUX.1-dev-gguf` (same three quants)

If a quant doesn't exist in leejet's repo (e.g. Q4_K, Q3_K, Q5_K
for FLUX schnell), do NOT use a stand-in from another repo with
the same name. Pick a different quant level that leejet ships.

### 20.3 The diagnostic that catches this

```python
# python diagnostic for image cache files containing model output
import struct, numpy as np
with open(cache_path, 'rb') as f:
    f.read(8)
    version, w, h, c, _ = struct.unpack('<IIIII', f.read(20))
    data = np.frombuffer(f.read(), dtype=np.float32).reshape(h, w, c)
print(f'min={data.min()} max={data.max()} std={data.std()}')
```

`min=max=1.0, std=0.0` means every pixel is saturated. Combined
with "inference ran without errors," this points at NaN in the
latent, which means dequantization or matmul produced NaN, which
most often means file format mismatch.

If the diagnostic shows a real image (`std > 0.1`, varied
percentiles), you're past this class of bug.

### 20.4 The general principle

GGUF, ONNX, safetensors, .pt -- these are all containers, not
contracts. "It loaded successfully" tells you nothing about whether
the values inside will produce correct math. When debugging a
generative model that "runs but produces garbage," **swap the
weight file before tweaking anything in the inference config.**

## 21. Downloading large model files on Windows: never use `Invoke-WebRequest`

Multi-GB model downloads (SDXL bases, ControlNets, T5 encoders,
GGUFs) on Windows have one consistent pitfall: PowerShell's
`Invoke-WebRequest` cmdlet is shockingly slow at this. We saw a
2.5 GB ControlNet crawling at 0.3 - 1.8 MB/s when the same file
downloaded from a browser at over 100 MB/s on the same machine
and the same connection.

The cause is `$ProgressPreference = 'Continue'` (the default).
The progress bar rendering for binary streams is so expensive
that it throttles the download itself. This is documented
behavior, not a bug, and applies to both PowerShell 5.1 and 7.x.

### 21.1 Fast download options, in order of preference

1. **A browser**. For one-off downloads of public-URL model
   files, just open the URL and click download. No code, no
   gotchas, hits the connection's full bandwidth.

2. **`curl.exe`** (real curl, ships with Windows 10+ at
   `C:\Windows\System32\curl.exe`):
   ```powershell
   curl.exe -L -o "C:\path\to\file.safetensors" `
     "https://huggingface.co/.../resolve/main/file.safetensors"
   ```
   `-L` follows redirects (HuggingFace uses 302s for LFS
   files). Hits full bandwidth, real progress bar, ~10-50x
   faster than `Invoke-WebRequest`.

   Note: must say `curl.exe` explicitly, not bare `curl`.
   PowerShell aliases `curl` to `Invoke-WebRequest`, which
   defeats the entire point.

3. **`Invoke-WebRequest` with progress disabled** if you must
   stay in pure PowerShell (e.g. for a script you're handing
   off):
   ```powershell
   $ProgressPreference = 'SilentlyContinue'
   Invoke-WebRequest -Uri $url -OutFile $dest
   $ProgressPreference = 'Continue'
   ```
   Still slower than curl.exe but a 10x+ speedup vs. the
   default.

### 21.2 When to bother

For files under ~50 MB it doesn't matter; `Invoke-WebRequest`
is fine. The issue is binary streams over a few hundred MB,
which is exactly where AI/ML model files live. Anything
labeled "SDXL", "FLUX", "T5", "ControlNet", or sitting on
HuggingFace LFS is a candidate.

### 21.3 The general principle

PowerShell cmdlets are convenient for typical admin work
(small JSON APIs, config files, small scripts). They are not
designed as bulk-binary download tools. For multi-GB files,
reach for a tool built for the job — `curl.exe`, a browser,
or `aria2c` if you want parallel segments. Don't trust the
default cmdlet just because it's there.

## 22. sd.cpp ControlNet loader doesn't translate diffusers names

A class of upstream limitation that costs hours if you don't know
about it: sd.cpp's UNet base loader has a tensor-name conversion
table that maps diffusers naming (`down_blocks.*`, `up_blocks.*`,
`mid_block.*`, `time_embedding.*`, `add_embedding.*`) to legacy
A1111/ComfyUI naming (`input_blocks.*`, `output_blocks.*`,
`middle_block.*`, `time_embed.*`, `label_emb.*`). The
ControlNet loader does NOT have that table.

This means SDXL ControlNets fail to load even when they look right.
Symptom: hundreds of `unknown tensor 'down_blocks.X...'` warnings
during load, followed by hundreds of `tensor 'input_blocks.X...'
not in model file` errors. Loader returns failure; ensure_loaded
returns NULL.

### 22.1 The naming difference

Same architecture, two conventions:

| diffusers                         | legacy (sd.cpp ControlNet) |
| --------------------------------- | -------------------------- |
| `conv_in.*`                       | `input_blocks.0.0.*`       |
| `time_embedding.linear_{1,2}.*`   | `time_embed.{0,2}.*`       |
| `add_embedding.linear_{1,2}.*`    | `label_emb.0.{0,2}.*`      |
| `controlnet_cond_embedding.*`     | `input_hint_block.*`       |
| `controlnet_down_blocks.N.*`      | `zero_convs.N.0.*`         |
| `controlnet_mid_block.*`          | `middle_block_out.0.*`     |
| `down_blocks.X.resnets.Y.*`       | `input_blocks.<idx>.0.*`   |
| `down_blocks.X.attentions.Y.*`    | `input_blocks.<idx>.1.*`   |
| `down_blocks.X.downsamplers.0.*`  | `input_blocks.<idx>.0.op.*`|
| `mid_block.resnets.{0,1}.*`       | `middle_block.{0,2}.*`     |
| `mid_block.attentions.0.*`        | `middle_block.1.*`         |

Within each ResBlock, the substructure rename is also needed:
`conv1` -> `in_layers.2`, `norm1` -> `in_layers.0`, `conv2` ->
`out_layers.3`, `norm2` -> `out_layers.0`, `time_emb_proj` ->
`emb_layers.1`, `conv_shortcut` -> `skip_connection`.

### 22.2 What we learned the hard way

The lllyasviel/sd_control_collection on HuggingFace looks like it
contains converted files (filenames suggest A1111 use), but most
of them are just re-uploads of the diffusers originals in
diffusers naming. Don't trust filenames; check tensor names first.

The `*lora.safetensors` files in that collection are a different
trap: they ARE in legacy naming, but every weight is decomposed
into rank-N `.down`/`.up` factors -- they're ControlLoRA, not
ControlNet. sd.cpp's ControlNet loader can't merge LoRA factors
at load time; it needs full weights.

There is no published full SDXL ControlNet in legacy naming that
we found.

### 22.3 The fix: tools/convert_sdxl_controlnet.py

A standalone Python script under `tools/` in this repo does the
diffusers -> legacy rename and writes a new safetensors file.
Dependencies: `safetensors`, `numpy`. Usage:

```
python tools/convert_sdxl_controlnet.py \
    diffusers_xl_canny_full.safetensors \
    sdxl_canny_legacy.safetensors
```

The converted file loads cleanly in sd.cpp with no unknown
tensors. Run-once per ControlNet model; output is permanent.

### 22.4 The general principle

When a C++ inference library accepts multiple weight formats,
the format-conversion logic tends to be unevenly applied across
sub-systems. The UNet path gets it because everyone uses it; the
ControlNet path doesn't because most users either run with
ComfyUI-format files or use a different inference engine. The
fix is upstream PR territory; the workaround is convert the
file once on disk. Always inspect a few tensor names with
`safetensors` Python tools before assuming a downloaded file is
"the right format."

### 22.5 The proj_in/proj_out Linear-vs-Conv gotcha

After renaming was solved, a second mismatch appeared:

```
tensor 'input_blocks.4.1.proj_in.weight' has wrong shape:
  got [640, 640, 1, 1], expected [1, 1, 640, 640]
```

Same dimensions, different order. Cause: SDXL's
`Transformer2DModel` is instantiated with
`use_linear_projection=True`, so `proj_in` and `proj_out` are
`nn.Linear` -- 2D weights `[out, in]`. Legacy ComfyUI / sd.cpp
expect them as 1x1 `nn.Conv2d` -- 4D weights `[out, in, 1, 1]`.
Mathematically equivalent (a 1x1 conv applied per-spatial-pixel
is identical to a Linear), but the storage layout differs and
sd.cpp's loader does strict shape comparison.

The fix is a one-line reshape during conversion:

```python
if (k.endswith('.proj_in.weight') or k.endswith('.proj_out.weight')) \
       and t.ndim == 2:
    t = t.reshape(t.shape[0], t.shape[1], 1, 1)
```

This is SDXL-specific. SD1.5 ControlNets use Conv2d natively
(`use_linear_projection=False`), so their proj_in/out are
already 4D and don't need reshaping. The conversion script
checks `t.ndim == 2` defensively so it works for both.

`attn1`/`attn2` internal projections (`to_q/k/v.weight`,
`to_out.0.weight`) and `ff.net.*` are nn.Linear in both
diffusers and legacy, so they need no reshape -- only `proj_in`
and `proj_out` differ.

---

*Updated through the nuke-ai-fill FLUX integration (May 2026),
including the Windows-System32-ONNX battle, DDImage knob-storage
discovery, viewer-refresh investigation, Carve LaMa-ONNX
normalization convention, sd.cpp _init-defaults discipline, VAE
tiling for VRAM headroom, and GGUF file-format-vs-tool
compatibility, the Windows large-file download (curl.exe over
Invoke-WebRequest) lesson, and the diffusers-vs-legacy SDXL
ControlNet tensor naming conversion. Add an entry here when a new gotcha costs a
debugging round.*
