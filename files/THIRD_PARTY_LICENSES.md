# Third-Party Licenses

This file lists all third-party software bundled with or required by
SmartFill, along with their licenses. Licenses are reproduced as
required by the original terms (typically the BSD/MIT/Apache "include
copyright and permission notice" clause).

## Current state

Phase 1 (stub release) has no bundled third-party dependencies. The
plugin links only against The Foundry's Nuke NDK headers/libraries,
which are not redistributed.

## To be added as dependencies land

When the following dependencies are integrated, their license text and
attribution will be reproduced here in full:

### ONNX Runtime (Microsoft) - MIT

Bundled binary distribution. Required for AISmartFill LaMa inference.
Source: https://github.com/microsoft/onnxruntime

### stable-diffusion.cpp (leejet) - MIT

Source distribution as a git submodule. Required for AIGenerate.
Source: https://github.com/leejet/stable-diffusion.cpp

### ggml - MIT

Vendored inside stable-diffusion.cpp.
Source: https://github.com/ggerganov/ggml

### LaMa model weights (Carve ONNX export) - Apache 2.0

Downloaded by the user at install time; not redistributed.
Source: https://huggingface.co/Carve/LaMa-ONNX

### FLUX.1-schnell model weights (Black Forest Labs) - Apache 2.0

Downloaded by the user at install time; not redistributed.
Source: https://huggingface.co/black-forest-labs/FLUX.1-schnell

## Plugin-side

The Op API surface uses the Foundry NDK, which is proprietary to The
Foundry Visionmongers. SmartFill links against it dynamically; no NDK
code is included in this repository.
