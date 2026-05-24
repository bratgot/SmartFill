# nuke-ai-fill

Full-MIT/Apache AI inpainting and generation for Nuke 14 (Windows, NDK,
CMake). No use-based restrictions, no revenue caps, no proprietary
runtimes.

## Stack

| Layer        | Component                  | License     |
|--------------|----------------------------|-------------|
| Library      | stable-diffusion.cpp       | MIT         |
| Library      | ggml (submodule of above)  | MIT         |
| Library      | ONNX Runtime               | MIT         |
| Library      | OpenEXR (Nuke ships it)    | BSD-3       |
| Model        | FLUX.1-schnell             | Apache 2.0  |
| Model        | FLUX.2-klein 4B            | Apache 2.0  |
| Model        | LaMa (Carve ONNX export)   | Apache 2.0  |
| Plugin code  | this project               | MIT         |

Apache 2.0 dependencies are MIT-equivalent for our purposes: permissive,
no use restrictions, no copyleft. Patent grant is a plus, not a friction.

Avoided on purpose: any CreativeML OpenRAIL-M model (SD 1.5, SDXL, SD3),
FLUX.1-dev (non-commercial), FLUX.2-klein 9B (non-commercial), TensorRT
(NVIDIA proprietary EULA), Real-ESRGAN (BSD-3 fine but its training data
license is murky; skipping).

## Two Ops

The two use cases want different UX. Better as two DLLs than one mode
switch.

### AISmartFill (LaMa via ONNX Runtime)

- Input 0: source image
- Input 1: mask (any single channel)
- No prompt knob, deterministic, fast (~800ms at 512x512 on RTX 5060 Ti)
- Backend: CUDA EP (CUDA 12.6 + PTX JIT for Blackwell sm_120 until
  ONNX Runtime ships CUDA 12.8 binaries)
- Use cases: paint-out, wire removal, simple object removal,
  rotopaint cleanup, dust busting

### AIGenerate (stable-diffusion.cpp)

- Input 0: source image (optional - txt2img if disconnected)
- Input 1: mask (optional - generative fill if connected)
- Prompt knob, negative prompt, steps, CFG, sampler, scheduler, seed,
  denoising strength
- Default model: FLUX.1-schnell (Apache 2.0, 1-4 step inference)
- Backend: choose at build time; CUDA primary, Vulkan fallback
- Use cases: generative fill, set extension, replacing things,
  text-to-image

## Plugin architecture

Following NDK_NOTES.md exactly:

- MODULE target (not SHARED), one DLL per Op, filename matches
  Op::Description
- /MD /W3 /EHsc on MSVC, NOMINMAX + _USE_MATH_DEFINES +
  WIN32_LEAN_AND_MEAN + _CRT_SECURE_NO_WARNINGS defined globally
- Multi-config build: --config Release at build time, no
  CMAKE_BUILD_TYPE at configure
- ASCII-only source files in .h/.cpp/.cu/.cuh

### Op base class

PlanarIop. ML inference wants whole-image-in-whole-image-out, not
scanline striding. The renderStripe call reads from a content-addressed
on-disk cache; the inference itself runs in a worker thread spawned from
knob_changed when the user hits Bake.

### Worker thread pattern

Required by NDK_NOTES section 5. Inference is seconds-to-minutes per
frame; it cannot run inside engine/renderStripe.

```
knob_changed (UI thread)
    -> compute input hash
    -> if cache hit: trigger asapUpdate, done
    -> else: spawn worker, set status knob to "Cooking..."

worker (background thread)
    -> read input pixels from Op cache (already cooked)
    -> run LaMa or sd.cpp
    -> write result EXR to cache directory keyed by hash
    -> post a "main thread invoke" to Nuke (executeOnMainThread)

main-thread callback
    -> update status knob to "Ready"
    -> invalidate Op so renderStripe re-runs
    -> renderStripe reads cached EXR and emits
```

Worker never touches knobs directly. Worker wraps its body in try/catch.
A status string knob shows progress without ever being mutated from the
worker thread.

### Knob set

Both Ops:

- Backend enum (CUDA / Vulkan / CPU)
- Cache directory (file knob, forward-slash-only)
- Bake button
- Clear cache button
- Status string (read-only, main-thread written)

AIGenerate adds:

- Mode enum (txt2img / img2img / inpaint)
- Model path (file knob, NO_ANIMATION)
- T5 / CLIP / VAE paths for FLUX (file knobs, NO_ANIMATION)
- Prompt (multiline string, NO_ANIMATION) - CRITICAL: SD prompts use
  bracket and paren syntax for emphasis like (word:1.2), [neg], which
  TCL will mangle without NO_ANIMATION
- Negative prompt (multiline string, NO_ANIMATION)
- Steps (int knob)
- CFG scale (float knob)
- Sampler enum
- Scheduler enum
- Seed (int knob, -1 for random)
- Denoising strength (float knob, 0-1, img2img and inpaint only)
- Width / height (int knobs, 64-step)

## Cache design

Content-addressed. Key = SHA-256 of:

- Input image pixels (sampled, not full - 64 evenly-spaced pixel
  hash to avoid hashing 4K every cook)
- Mask pixels (same)
- All knob values that affect inference
- Model file path + mtime

Storage: cache_dir/<hash>.exr. EXR keeps full float precision and
respects Nuke's internal pipeline. Half-float by default to save disk;
configurable to float for archival.

A small JSON sidecar (<hash>.json) records the knob state at bake time
so future-Art can audit what produced what.

## CUDA / Blackwell note

User has RTX 5060 Ti (Blackwell sm_120) and CUDA 12.6 installed.
sm_120 native codegen needs CUDA 12.8+, so 12.6 builds run via PTX JIT
on first launch (one-time ~10s warmup, then cached).

For stable-diffusion.cpp this just works - the CUDA backend tolerates
JIT. For ONNX Runtime, the CUDA EP also tolerates JIT but slower until
warmed.

If JIT cost is unacceptable: build both libraries from source against
CUDA 12.8 SDK, OR use Vulkan backend (one CMake flag) which sidesteps
the CUDA toolchain entirely.

## File structure

```
nuke-ai-fill/
  CMakeLists.txt              root, finds Nuke + deps, defines two MODULE targets
  LICENSE                     MIT
  THIRD_PARTY_LICENSES.md     Apache 2.0 model attribution, MIT lib attribution
  PLAN.md                     this file
  README.md
  menu.py                     toolbar registration, pluginAddPath
  deploy.ps1                  build + install to %USERPROFILE%\.nuke\nuke-ai-fill\

  core/                       static library, shared between Ops
    include/
      ai_worker.h             async worker class
      ai_cache.h              content-addressed EXR cache
      ai_image_buffer.h       Nuke <-> raw float buffer conversion
      ai_hash.h               SHA-256 of input state
    src/
      ai_worker.cpp
      ai_cache.cpp
      ai_image_buffer.cpp
      ai_hash.cpp

  ops/
    AISmartFill/
      CMakeLists.txt
      AISmartFill.cpp         the Op
      lama_session.h          ONNX Runtime LaMa wrapper
      lama_session.cpp
    AIGenerate/
      CMakeLists.txt
      AIGenerate.cpp          the Op
      sd_session.h            stable-diffusion.cpp wrapper
      sd_session.cpp

  third_party/
    stable-diffusion.cpp/     git submodule
    onnxruntime/              binary release, downloaded by CMake on configure
    sha256/                   public-domain single-file SHA-256

  docs/
    ARCHITECTURE.md
    BUILDING.md
    MODELS.md
```

## Build commands

VS 2019 (NDK_NOTES preferred):

```
cmake -G "Visual Studio 16 2019" -A x64 ^
      -DNuke_DIR="C:/Program Files/Nuke14.1v6" ^
      -DSD_CUDA=ON ^
      -DORT_DIR="C:/dev/onnxruntime-win-x64-gpu-1.20.1" ^
      -B build .
cmake --build build --config Release
```

VS 2022 with v142 toolset (if VS 2019 not installed):

```
cmake -G "Visual Studio 17 2022" -A x64 -T v142 ^
      -DNuke_DIR="C:/Program Files/Nuke14.1v6" ^
      -DSD_CUDA=ON ^
      -DORT_DIR="C:/dev/onnxruntime-win-x64-gpu-1.20.1" ^
      -B build .
cmake --build build --config Release
```

Note: v142 toolset must be installed via VS 2022 installer
("MSVC v142 - VS 2019 C++ x64/x86 build tools" component).

## Phase plan

1. Foundation: CMake, core/ai_worker.h, core/ai_cache.h, ai_image_buffer
2. AISmartFill end-to-end with stub LaMa (CPU only, return mask blurred)
3. Wire actual ONNX Runtime + Carve LaMa model
4. AIGenerate with stable-diffusion.cpp submodule + FLUX.1-schnell
5. menu.py + deploy.ps1 + license attribution
6. README with model download instructions

## Out of scope (for now)

- Per-frame animation (cache is keyed per-frame; animated bakes are a
  later feature - probably an Executable subclass that bakes a range)
- Lora support (stable-diffusion.cpp supports it; can add as a file
  knob list later)
- ControlNet (sd.cpp supports for SD 1.5 only; not useful for FLUX)
- Upscaling (ESRGAN training data licensing is unclear; skip)
- PowerPaint, Kontext editing, Qwen Edit (each adds a model surface;
  prove the core first)
