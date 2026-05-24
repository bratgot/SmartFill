# nuke-ai-fill

AI-powered context-aware fill and text-to-image generation Ops for
The Foundry's Nuke 14 on Windows. MIT-licensed, no proprietary
runtimes, no usage tracking, no internet required at runtime.

## What's included

- **AISmartFill** -- LaMa-based context-aware fill via ONNX Runtime.
  Two-input Op (Source + Mask) with a Bake button. Inference runs in
  a background thread; results cache to disk under
  `%APPDATA%\nuke-ai-fill\cache\`. Re-cooks against the cache hit
  instantly. Useful for paint-out, wire removal, rotopaint cleanup,
  small-to-medium object removal. Typical bake time: 5--10 seconds
  on a recent NVIDIA GPU.

- **AIGenerate** -- FLUX.1-schnell text-to-image via stable-diffusion.cpp.
  Zero-input Op with prompt, dimensions, seed, and step count knobs.
  Same Bake + cache + status pattern as AISmartFill. Uses CUDA via
  ggml-cuda with graph-cut VRAM offload to fit on 16 GB cards.
  Typical generation: ~25 seconds for 1024x1024 on RTX 5060 Ti.

## Architecture

Both Ops share a common `nuke_ai_fill_core` static library containing:

- `AiWorker` -- single-thread background worker with cancel support
- `image_cache` -- content-addressed disk cache for inference results
- `plugin_path` -- resolves model and cache directories
- `LamaSession` -- PIMPL wrapper around ONNX Runtime for LaMa
- `SdSession` -- PIMPL wrapper around stable-diffusion.cpp for FLUX

The Ops themselves (`AISmartFill`, `AIGenerate`) layer in Nuke
integration: knob layout, async polling, cache key computation, viewer
output. Each Op compiles to its own DLL and registers under
`Filter/` (AISmartFill) or `Image/` (AIGenerate) in the Nuke node menu.

## Hardware requirements

- Windows 10 or 11, x64
- NVIDIA GPU with 8+ GB VRAM for FLUX, 4+ GB for LaMa-only use
- NVIDIA driver supporting CUDA 12.x (no CUDA Toolkit install required
  at runtime; just the driver)
- ~12 GB free disk for FLUX model files
- ~3 GB free CPU RAM during FLUX inference

For AISmartFill alone, even a 4 GB Pascal-or-newer card works. The
FLUX models are the constraining factor.

## Installation

### Pre-built distribution

Use `package.ps1` on a machine with the plugin already built and
installed, then transfer the resulting zip to your target machine.
See INSTALL.md in the generated zip for unpacking instructions.

### From source

Build prerequisites:
- Visual Studio 2019 (16.11+) or 2022 Build Tools, x64
- CMake 3.22+
- Nuke 14.1 NDK (comes with Nuke 14.1)
- CUDA Toolkit 12.6 or 12.9 (for building ggml-cuda)
- ONNX Runtime 1.20.1 with CUDA EP, extracted to a known directory

```powershell
# Set environment for CUDA detection
$cuda = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
$env:CUDA_PATH = $cuda

# Vendor stable-diffusion.cpp
mkdir third_party
git clone --recursive https://github.com/leejet/stable-diffusion.cpp.git third_party/stable-diffusion.cpp

# Configure (one-time)
cmake -G "Visual Studio 16 2019" -A x64 -T "cuda=$cuda" `
      -DNuke_DIR="C:/Program Files/Nuke14.1v8/cmake" `
      -DORT_DIR="C:/path/to/onnxruntime-win-x64-gpu-1.20.1" `
      -DBUILD_GENERATE=ON `
      -DCMAKE_CUDA_COMPILER="$cuda\bin\nvcc.exe" `
      -DCUDAToolkit_ROOT="$cuda" `
      -DCMAKE_CUDA_ARCHITECTURES="89" `
      -B build .

# Build (10--15 min first time, due to ggml-cuda kernel compilation)
cmake --build build --config Release

# Install to user's .nuke directory
cmake --install build --config Release --prefix "$env:USERPROFILE\.nuke"
```

`CMAKE_CUDA_ARCHITECTURES` should match your GPU's compute capability:
- 75 (Turing, RTX 20xx)
- 86 (Ampere, RTX 30xx, A40, A100)
- 89 (Ada Lovelace, RTX 40xx)
- 120 (Blackwell, RTX 50xx)

If targeting multiple cards, pass a semicolon-separated list:
`-DCMAKE_CUDA_ARCHITECTURES="75;86;89"`. Larger lists make builds
much slower.

## Model files

Place in `%USERPROFILE%\.nuke\nuke-ai-fill\models\`:

| File | Source | Size |
|---|---|---|
| `lama_fp32.onnx` | huggingface.co/Carve/LaMa-ONNX | ~200 MB |
| `flux1-schnell-q4_0.gguf` | **huggingface.co/leejet/FLUX.1-schnell-gguf** | ~6.5 GB |
| `ae.safetensors` | huggingface.co/black-forest-labs/FLUX.1-schnell | ~335 MB |
| `clip_l.safetensors` | huggingface.co/comfyanonymous/flux_text_encoders | ~246 MB |
| `t5-v1_1-xxl-encoder-Q8_0.gguf` | huggingface.co/city96/t5-v1_1-xxl-encoder-gguf | ~5 GB |

The FLUX diffusion model file MUST come from leejet's repository.
GGUF files with the same name from other repos (city96, lllyasviel,
calcuis) use a different quantization convention and produce all-white
output when loaded by stable-diffusion.cpp.

## Licensing

MIT for nuke-ai-fill itself. All bundled dependencies are MIT,
Apache 2.0, BSD-3-Clause, or Public Domain. Commercial use by VFX
studios on paying client work is permitted. See `LICENSING.md` for
the practical summary and `THIRD_PARTY_LICENSES.md` for full
attribution.

**Important**: do not substitute FLUX.1-dev for FLUX.1-schnell. The
[dev] model is under a non-commercial license that requires a
separate paid agreement with Black Forest Labs for studio use.

## Known limitations

- **Viewer auto-refresh after Bake**: completed bakes don't always
  refresh the viewer immediately. Workaround: disconnect/reconnect a
  downstream input, or change any knob value, to force a re-cook.

- **CUDA architecture mismatch**: a plugin built for one GPU
  generation may JIT-compile slowly on a different generation the
  first time. For multi-card studios, build with multiple
  architectures (see installation notes above).

- **FLUX VRAM**: a 12-billion-parameter model needs roughly 9-13 GB
  of VRAM at Q4_0 + tiled VAE decode. 16 GB cards have enough
  headroom for production use. 8 GB cards will work via graph-cut
  CPU offload but at significantly reduced throughput.

## Documentation

- `LICENSING.md` -- quick commercial-use reference
- `THIRD_PARTY_LICENSES.md` -- full attribution for all components
- `NDK_NOTES.md` (separate file in author's working notes) -- Nuke
  NDK pitfalls and patterns discovered building this. Not bundled
  with the distribution but available on request.

## Repository

https://github.com/bratgot/SmartFill
