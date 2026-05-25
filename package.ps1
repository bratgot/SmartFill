# nuke-ai-fill / package.ps1
#
# Bundle the installed plugin into a portable zip for transfer to
# another Windows machine running the same Nuke version.
#
# Usage:
#   .\package.ps1                              # zip without models (~30MB)
#   .\package.ps1 -IncludeModels               # zip with models (~12GB)
#   .\package.ps1 -OutputDir C:\Temp           # specify output location
#   .\package.ps1 -IncludeCuda                 # also bundle CUDA runtime DLLs
#
# Output: <OutputDir>\nuke-ai-fill-<date>.zip
#
# Per NDK_NOTES section 10: PowerShell scripts downloaded onto a
# Windows machine need either Unblock-File before running, or
# invocation via:
#   powershell -ExecutionPolicy Bypass -File .\package.ps1
# One-time relief on the receiving machine:
#   Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned

[CmdletBinding()]
param(
    [string] $InstallDir = "$env:USERPROFILE\.nuke\nuke-ai-fill",
    [string] $OutputDir  = "$env:USERPROFILE\Desktop",
    [switch] $IncludeModels,
    [switch] $IncludeCuda,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------------
# Sanity checks
# ----------------------------------------------------------------------

if (-not (Test-Path $InstallDir)) {
    Write-Error "InstallDir does not exist: $InstallDir"
    return
}

# Required files: at least the two plugin DLLs and menu.py.
$required = @(
    "$InstallDir\menu.py",
    "$InstallDir\AISmartFill.dll",
    "$InstallDir\AIGenerate.dll",
    "$InstallDir\onnxruntime.dll"
)
foreach ($f in $required) {
    if (-not (Test-Path $f)) {
        Write-Error "Required file missing: $f. Run 'cmake --install build --config Release --prefix `"`$env:USERPROFILE\.nuke`"' first."
        return
    }
}

# Stamp the output filename
$stamp = (Get-Date).ToString("yyyyMMdd-HHmmss")
$bundle_name = "nuke-ai-fill-$stamp"
$staging_dir = Join-Path $OutputDir $bundle_name
$zip_path    = Join-Path $OutputDir "$bundle_name.zip"

if ((Test-Path $staging_dir) -or (Test-Path $zip_path)) {
    if ($Force) {
        Remove-Item $staging_dir -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $zip_path -Force -ErrorAction SilentlyContinue
    } else {
        Write-Error "Output already exists: $staging_dir or $zip_path. Use -Force to overwrite."
        return
    }
}

New-Item -ItemType Directory -Path $staging_dir | Out-Null

Write-Host ""
Write-Host "Packaging nuke-ai-fill from $InstallDir" -ForegroundColor Cyan
Write-Host "Output: $zip_path" -ForegroundColor Cyan
Write-Host ""

# ----------------------------------------------------------------------
# Always include: plugin DLLs, ONNX Runtime DLLs, menu.py, docs
# ----------------------------------------------------------------------

$file_groups = @(
    @{
        Name = "Plugin DLLs"
        Items = @("AIGenerate.dll", "AISmartFill.dll")
    },
    @{
        Name = "ONNX Runtime"
        Items = @(
            "onnxruntime.dll",
            "onnxruntime_providers_cuda.dll",
            "onnxruntime_providers_shared.dll",
            "onnxruntime_providers_tensorrt.dll"
        )
    },
    @{
        Name = "Menu and docs"
        Items = @("menu.py", "LICENSE", "LICENSING.md", "README.md", "THIRD_PARTY_LICENSES.md")
    }
)

foreach ($group in $file_groups) {
    Write-Host "  $($group.Name):" -ForegroundColor Yellow
    foreach ($item in $group.Items) {
        $src = Join-Path $InstallDir $item
        if (Test-Path $src) {
            Copy-Item $src $staging_dir
            $size_mb = [math]::Round((Get-Item $src).Length / 1MB, 1)
            Write-Host "    + $item ($size_mb MB)"
        } else {
            Write-Host "    - $item (not present, skipping)" -ForegroundColor DarkGray
        }
    }
}

# ----------------------------------------------------------------------
# LaMa ONNX model - small enough to always include
# ----------------------------------------------------------------------

$lama_src = Join-Path $InstallDir "models\lama_fp32.onnx"
if (Test-Path $lama_src) {
    New-Item -ItemType Directory -Path "$staging_dir\models" -Force | Out-Null
    Copy-Item $lama_src "$staging_dir\models\"
    $size_mb = [math]::Round((Get-Item $lama_src).Length / 1MB, 1)
    Write-Host "  LaMa model:" -ForegroundColor Yellow
    Write-Host "    + lama_fp32.onnx ($size_mb MB)"
}

# ----------------------------------------------------------------------
# FLUX model files - optional (huge)
# ----------------------------------------------------------------------

if ($IncludeModels) {
    Write-Host "  FLUX models (large):" -ForegroundColor Yellow
    $flux_files = @(
        "flux1-schnell-q4_0.gguf",
        "t5-v1_1-xxl-encoder-Q8_0.gguf",
        "clip_l.safetensors",
        "ae.safetensors"
    )
    New-Item -ItemType Directory -Path "$staging_dir\models" -Force | Out-Null
    foreach ($mf in $flux_files) {
        $src = Join-Path $InstallDir "models\$mf"
        if (Test-Path $src) {
            Copy-Item $src "$staging_dir\models\"
            $size_mb = [math]::Round((Get-Item $src).Length / 1MB)
            Write-Host "    + $mf ($size_mb MB)"
        } else {
            Write-Host "    - $mf (not present)" -ForegroundColor DarkGray
        }
    }
} else {
    Write-Host "  FLUX models: SKIPPED (use -IncludeModels to bundle ~12GB of weights)" -ForegroundColor DarkGray
}

# ----------------------------------------------------------------------
# CUDA runtime DLLs - optional, only needed if target lacks CUDA Toolkit
# ----------------------------------------------------------------------

if ($IncludeCuda) {
    Write-Host "  CUDA runtime DLLs:" -ForegroundColor Yellow
    $cuda_paths = @(
        "$env:CUDA_PATH\bin",
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin",
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin"
    )
    $cuda_dlls = @("cudart64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll")
    $cuda_dir = $null
    foreach ($p in $cuda_paths) {
        if ($p -and (Test-Path $p)) {
            $cuda_dir = $p
            break
        }
    }
    if (-not $cuda_dir) {
        Write-Host "    - CUDA bin dir not found; skipping" -ForegroundColor DarkGray
    } else {
        foreach ($dll in $cuda_dlls) {
            $src = Join-Path $cuda_dir $dll
            if (Test-Path $src) {
                Copy-Item $src $staging_dir
                $size_mb = [math]::Round((Get-Item $src).Length / 1MB, 1)
                Write-Host "    + $dll ($size_mb MB)"
            } else {
                Write-Host "    - $dll (not present in $cuda_dir)" -ForegroundColor DarkGray
            }
        }
    }
}

# ----------------------------------------------------------------------
# Generate INSTALL.md for the receiving end
# ----------------------------------------------------------------------

$install_md = @"
# nuke-ai-fill installation

Bundle generated: $stamp
Source machine: $env:COMPUTERNAME

## Install

1. Unzip this archive somewhere convenient.

2. Copy the contents of the unzipped folder to:

       %USERPROFILE%\.nuke\nuke-ai-fill\

   (Create that directory first if it does not exist.)

3. If your user's Nuke .nuke directory does not already load plugins
   from this subfolder, edit ``%USERPROFILE%\.nuke\init.py`` and append:

       import nuke
       nuke.pluginAddPath('./nuke-ai-fill')

   Then add the same path import to ``%USERPROFILE%\.nuke\menu.py``:

       import nuke
       nuke.pluginAddPath('./nuke-ai-fill')

   (Or copy the included ``menu.py`` into ``%USERPROFILE%\.nuke\menu.py``
   if your machine has no existing menu.py.)

4. Restart Nuke. The Image menu should now contain ``AIGenerate``;
   the Filter menu should contain ``AISmartFill``.

## Model files

The plugin needs these in ``%USERPROFILE%\.nuke\nuke-ai-fill\models\``:

| File | Source | Size |
|---|---|---|
| ``lama_fp32.onnx`` | (included if present on source) | ~200 MB |
| ``flux1-schnell-q4_0.gguf`` | huggingface.co/leejet/FLUX.1-schnell-gguf | ~6.5 GB |
| ``ae.safetensors`` | huggingface.co/black-forest-labs/FLUX.1-schnell | ~335 MB |
| ``clip_l.safetensors`` | huggingface.co/comfyanonymous/flux_text_encoders | ~246 MB |
| ``t5-v1_1-xxl-encoder-Q8_0.gguf`` | huggingface.co/city96/t5-v1_1-xxl-encoder-gguf | ~5 GB |

Important for FLUX:
- The diffusion model MUST come from leejet's repo (Q4_0 specifically).
  Other repos publish "Q4_K" GGUF files that use ComfyUI conventions
  which load successfully but produce all-white output.
- Don't rename T5 from .gguf to .safetensors or vice versa even if
  the auto-detection looks for a specific name - point the path knob
  at the actual filename.

## Hardware requirements

- Windows 10/11 x64
- NVIDIA GPU with 8+ GB VRAM (FLUX schnell), 4+ GB (AISmartFill alone)
- CUDA 12.x driver (any 12.x version of the NVIDIA driver is fine;
  you do NOT need the CUDA Toolkit installed, only the driver)
- ~12 GB free disk for FLUX models
- ~3 GB free CPU RAM during inference

If the target machine has no NVIDIA CUDA driver, ``AISmartFill`` may
still work via CPU but will be slow. ``AIGenerate`` requires CUDA
in practice (CPU inference for FLUX takes 5-15 minutes per image).

## Troubleshooting

- **Bake button does nothing**: Check Nuke's terminal for ``[AIGenerate]``
  or ``[AISmartFill]`` log lines. If the plugin didn't load, check
  ``%TEMP%\nuke-ai-fill-bootstrap.log`` for ORT preload failure.

- **AIGenerate outputs solid white**: The diffusion model GGUF is the
  wrong source. Re-download from leejet/FLUX.1-schnell-gguf.

- **AIGenerate runs out of VRAM**: T5 file should be the Q8 GGUF
  (~5 GB), not the FP16 safetensors (~10 GB). The combined working
  set on a 16 GB card should be ~12 GB after VAE tiling kicks in.

- **Viewer doesn't refresh after bake**: Known issue. Manually
  disconnect/reconnect a downstream input, or change any knob value,
  to force a re-cook.
"@

Set-Content -Path "$staging_dir\INSTALL.md" -Value $install_md -Encoding ASCII
Write-Host "  + INSTALL.md (auto-generated)" -ForegroundColor Yellow

# ----------------------------------------------------------------------
# Total size before zip
# ----------------------------------------------------------------------

$total_size = (Get-ChildItem $staging_dir -Recurse -File | Measure-Object -Property Length -Sum).Sum
$total_mb = [math]::Round($total_size / 1MB, 1)
Write-Host ""
Write-Host "Staging total: $total_mb MB" -ForegroundColor Cyan

# ----------------------------------------------------------------------
# Zip it up
# ----------------------------------------------------------------------

Write-Host ""
Write-Host "Compressing..." -ForegroundColor Cyan
Compress-Archive -Path "$staging_dir\*" -DestinationPath $zip_path -CompressionLevel Optimal -Force

$zip_size_mb = [math]::Round((Get-Item $zip_path).Length / 1MB, 1)
Write-Host ""
Write-Host "Bundle ready:" -ForegroundColor Green
Write-Host "  $zip_path" -ForegroundColor Green
Write-Host "  $zip_size_mb MB compressed (from $total_mb MB)" -ForegroundColor Green
Write-Host ""

# ----------------------------------------------------------------------
# Cleanup staging unless requested otherwise
# ----------------------------------------------------------------------

if (-not $env:NUKE_AI_FILL_KEEP_STAGING) {
    Remove-Item $staging_dir -Recurse -Force
    Write-Host "Staging directory cleaned up." -ForegroundColor DarkGray
} else {
    Write-Host "Staging directory kept at: $staging_dir" -ForegroundColor DarkGray
}
