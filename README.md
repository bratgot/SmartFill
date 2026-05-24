# SmartFill

AI-powered context-aware fill and generation Ops for The Foundry's Nuke 14
on Windows. MIT-licensed, no proprietary runtimes, no use restrictions.

## Status

**Phase 1 - foundation (current).** AISmartFill Op loads in Nuke 14.1v8,
two-input Op (Source + Mask) with a stub fill that paints masked pixels
neutral grey. Threading, content-addressed cache, and SHA-256 infrastructure
in place and unit-tested. Real LaMa inference via ONNX Runtime is the next
phase.

## Planned

- **AISmartFill**: LaMa-based context-aware fill, ONNX Runtime backend.
  Fast, deterministic, no prompt needed. For paint-out, wire removal,
  rotopaint cleanup, simple object removal.
- **AIGenerate**: stable-diffusion.cpp powered txt2img / img2img /
  inpaint, using FLUX.1-schnell (Apache 2.0) as the default model.

## Stack

| Layer       | Component                  | License    |
|-------------|----------------------------|------------|
| Library     | stable-diffusion.cpp       | MIT        |
| Library     | ggml                       | MIT        |
| Library     | ONNX Runtime               | MIT        |
| Model       | FLUX.1-schnell             | Apache 2.0 |
| Model       | LaMa (Carve ONNX)          | Apache 2.0 |
| This repo   |                            | MIT        |

No CreativeML OpenRAIL-M models, no FLUX.1-dev (non-commercial), no
TensorRT (proprietary). Every dependency is MIT or Apache 2.0.

## Requirements

- Windows 11
- Nuke 14.1+ (tested on 14.1v8)
- Visual Studio 2019 (or VS 2022 with v142 toolset)
- CMake 3.22+
- CUDA 12.8+ (for native Blackwell sm_120; 12.6 works via PTX JIT)

## One-time dependency setup

SmartFill is designed for air-gapped studio machines: it makes no
network calls at build time or runtime. The required external
dependencies (ONNX Runtime, LaMa model) are downloaded once on a
connected machine and bundled into the install.

### ONNX Runtime (GPU build, CUDA 12)

Download once from the official release page:

  https://github.com/microsoft/onnxruntime/releases

Pick `onnxruntime-win-x64-gpu-1.20.1.zip` (or newer CUDA-12 GPU
build). Extract anywhere; remember the path:

```
C:\dev\onnxruntime-win-x64-gpu-1.20.1\
    include\
    lib\
    ...
```

CMake will detect this path via the `-DORT_DIR` flag at configure
time. Its runtime DLLs get copied into the plugin install folder
automatically, so they ship with the plugin and never need to be on
PATH at the runtime machine.

### LaMa model (Carve ONNX export, Apache 2.0)

Download once from Hugging Face:

  https://huggingface.co/Carve/LaMa-ONNX

Get `lama_fp32.onnx` (~200 MB). Place it at:

```
C:\dev\SmartFill\models\lama_fp32.onnx
```

The CMake install rule copies it into `nuke-ai-fill/models/` so the
plugin finds it at a known relative path.

### CUDA runtime

ONNX Runtime's CUDA execution provider needs `cudart64_*.dll`,
`cublas64_*.dll`, and `cublasLt64_*.dll` from CUDA Toolkit at runtime.
The Toolkit installer places these on PATH, so on the build/dev
machine they work out of the box. For air-gapped deployment, copy
them from the CUDA Toolkit `bin/` directory into the plugin folder
alongside the other DLLs.

## Build

```powershell
cd C:\dev\SmartFill

cmake -G "Visual Studio 16 2019" -A x64 `
      -DNuke_DIR="C:/Program Files/Nuke14.1v8/cmake" `
      -DORT_DIR="C:/dev/onnxruntime-win-x64-gpu-1.20.1" `
      -DBUILD_GENERATE=OFF `
      -B build .

cmake --build build --config Release
cmake --install build --config Release --prefix "$env:USERPROFILE\.nuke"
```

`-DORT_DIR` is optional; if omitted, the LaMa inference path is
disabled but the plugin still loads (phase 1 stub behavior).

The install step:

1. Copies `AISmartFill.dll` and the plugin's `menu.py` to
   `$env:USERPROFILE\.nuke\nuke-ai-fill\`
2. Idempotently appends a registration block to your main
   `$env:USERPROFILE\.nuke\menu.py` via marker-comment detection

Launch Nuke; `AISmartFill` appears under `Nodes -> Filter`.

## Repository layout

```
SmartFill/
|-- CMakeLists.txt              root configuration
|-- PLAN.md                     architecture and roadmap
|-- LICENSE                     MIT
|-- THIRD_PARTY_LICENSES.md     dependency attributions (populated as deps land)
|-- menu.py                     plugin-side menu registration
|-- deploy.ps1                  manifest-driven file staging helper
|-- cmake/
|   `-- install_user_menu.cmake idempotent user-menu.py append
|-- core/                       shared library: hashing, threading, cache
|   |-- include/                public headers
|   `-- src/                    implementation
`-- ops/
    `-- AISmartFill/            the AI Smart Fill Op
```

## Development notes

See [PLAN.md](PLAN.md) for architecture, phase plan, and design
constraints (threading discipline, cache keying, model licensing
considerations).

The repo follows the hard-won Nuke NDK conventions captured in the
author's `NDK_NOTES.md` (Visual Studio toolset version, MODULE vs
SHARED target type, ASCII-only C++ sources, TCL evaluation safety on
prompt knobs, async worker boundaries, etc).

## License

MIT for this codebase. See [LICENSE](LICENSE).

Third-party dependencies retain their original licenses; see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
