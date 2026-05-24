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

*Updated through the nuke-ai-fill bring-up (May 2026). Add an entry
here when a new gotcha costs a debugging round.*
