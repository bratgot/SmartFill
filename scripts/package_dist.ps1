# scripts/package_dist.ps1
#
# Build a transferable distribution folder for nuke-ai-fill, split into:
#   dist/code/   - small, frequently updated (plugin DLLs, Python, tools)
#   dist/data/   - large, stable (runtime DLLs, models)
# Plus dist/install.ps1 and dist/README.md so the target machine has
# everything needed to install or update without consulting docs.
#
# IMPORTANT: PowerShell blocks unsigned scripts by default. After saving
# or downloading this file, unblock it first:
#     Unblock-File .\scripts\package_dist.ps1
# Or invoke as:
#     powershell -ExecutionPolicy Bypass -File .\scripts\package_dist.ps1
#
# Usage:
#   .\scripts\package_dist.ps1
#   .\scripts\package_dist.ps1 -Source 'D:\custom\install' -Dest 'D:\out\dist'
#   .\scripts\package_dist.ps1 -LargeFileMB 100      # different size threshold
#
# The "code" vs "data" split is by file size: DLLs under -LargeFileMB go
# to code/, larger ones go to data/runtime/. Models always go to data/.

[CmdletBinding()]
param(
    [string]$Source = "$env:USERPROFILE\.nuke\nuke-ai-fill",
    [string]$Dest,
    [int]$LargeFileMB = 50
)

$ErrorActionPreference = 'Stop'

# ---- Resolve paths ---------------------------------------------------------

if (-not $Dest) {
    $Dest = Join-Path $PSScriptRoot "..\dist"
}
$Dest   = [System.IO.Path]::GetFullPath($Dest)
$Source = [System.IO.Path]::GetFullPath($Source)
$RepoRoot  = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$RepoTools = Join-Path $RepoRoot "tools"

if (-not (Test-Path $Source)) {
    Write-Error "Source install not found: $Source"
    return
}

Write-Host ""
Write-Host "Packaging nuke-ai-fill distribution"
Write-Host "  Source: $Source"
Write-Host "  Dest:   $Dest"
Write-Host "  Split:  files < $LargeFileMB MB -> code/, larger -> data/runtime/"
Write-Host ""

# ---- Clean and recreate layout ---------------------------------------------

if (Test-Path $Dest) {
    Write-Host "Cleaning existing $Dest"
    Remove-Item $Dest -Recurse -Force
}
$null = New-Item -ItemType Directory -Path "$Dest\code\tools"  -Force
$null = New-Item -ItemType Directory -Path "$Dest\data\runtime" -Force
$null = New-Item -ItemType Directory -Path "$Dest\data\models"  -Force

# ---- DLLs: split by size ---------------------------------------------------

Write-Host "Copying DLLs..."

# Plugin DLLs are always treated as code (frequently rebuilt) regardless
# of size. Add new plugins here.
$PluginDlls = @('AIGenerate.dll', 'AISmartFill.dll')

Get-ChildItem -Path $Source -Filter "*.dll" -File | ForEach-Object {
    if ($_.Name -in $PluginDlls) {
        $bin = "$Dest\code"
        $bucket = "code (plugin)"
    } elseif ($_.Length -lt ($LargeFileMB * 1MB)) {
        $bin = "$Dest\code"
        $bucket = "code"
    } else {
        $bin = "$Dest\data\runtime"
        $bucket = "data/runtime"
    }
    Copy-Item $_.FullName $bin
    Write-Host ("  {0,-44} {1,9:N1} MB -> {2}" -f $_.Name, ($_.Length/1MB), $bucket)
}

# ---- Root-level supporting files (Python, configs) ------------------------

Write-Host ""
Write-Host "Copying scripts and configs..."
Get-ChildItem -Path $Source -File |
    Where-Object { $_.Extension -in '.py', '.json', '.cfg', '.toml', '.txt', '.md' } |
    ForEach-Object {
        Copy-Item $_.FullName "$Dest\code"
        Write-Host ("  {0,-44} {1,9:N1} KB -> code" -f $_.Name, ($_.Length/1KB))
    }

# ---- tools/ from the repo (the conversion script, etc) -------------------

if (Test-Path $RepoTools) {
    Write-Host ""
    Write-Host "Copying tools/ from repo..."
    Get-ChildItem -Path $RepoTools -File | ForEach-Object {
        Copy-Item $_.FullName "$Dest\code\tools"
        Write-Host ("  tools/{0,-38} {1,9:N1} KB -> code/tools" -f $_.Name, ($_.Length/1KB))
    }
}

# ---- Models (data, heavy) -------------------------------------------------

$modelsSrc = Join-Path $Source "models"
if (Test-Path $modelsSrc) {
    Write-Host ""
    Write-Host "Copying models (slow)..."
    $modelFiles = Get-ChildItem -Path $modelsSrc -Recurse -File
    $totalSize  = ($modelFiles | Measure-Object Length -Sum).Sum
    Write-Host ("  {0} files, {1:N1} GB total" -f $modelFiles.Count, ($totalSize/1GB))
    Copy-Item "$modelsSrc\*" "$Dest\data\models\" -Recurse -Force
    Write-Host "  done."
}

# ---- Generate install.ps1 -------------------------------------------------
# Single-quoted here-string keeps $vars and `r `n escapes literal so the
# generated install.ps1 evaluates them at install time, not packaging time.

$installPs1 = @'
# nuke-ai-fill installer
#
# IMPORTANT: PowerShell may block this script with an execution policy
# error. Unblock it once before running:
#     Unblock-File .\install.ps1
# Or invoke as:
#     powershell -ExecutionPolicy Bypass -File .\install.ps1
#
# Usage:
#   .\install.ps1                       # install everything (first time)
#   .\install.ps1 -Target 'D:\nuke'     # custom location
#   .\install.ps1 -CodeOnly             # fast update: just refresh code/
#
# Append-only files: menu.py and init.py at the target are never
# overwritten. Their managed content lives between markers:
#     # >>> nuke-ai-fill auto-managed >>>
#     ...managed block, regenerated on every install...
#     # <<< nuke-ai-fill auto-managed <<<
# Anything outside the markers is preserved across updates.

[CmdletBinding()]
param(
    [string]$Target = "$env:USERPROFILE\.nuke\nuke-ai-fill",
    [switch]$CodeOnly
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

# Files we never overwrite. Wrapped in markers for safe re-runs.
$AnchoredFiles = @('menu.py', 'init.py')
$BeginMarker   = '# >>> nuke-ai-fill auto-managed >>>'
$EndMarker     = '# <<< nuke-ai-fill auto-managed <<<'

function Update-AnchoredFile {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Target
    )
    if (-not (Test-Path $Source)) { return }

    $sourceContent = Get-Content $Source -Raw
    if ($null -eq $sourceContent) { $sourceContent = '' }
    $sourceContent = $sourceContent.TrimEnd("`r","`n")
    $block = "${BeginMarker}`r`n${sourceContent}`r`n${EndMarker}"

    $name = Split-Path $Target -Leaf

    if (-not (Test-Path $Target)) {
        Set-Content -Path $Target -Value $block -NoNewline
        Write-Host "  ${name}: created with managed block"
        return
    }

    $targetContent = Get-Content $Target -Raw
    if ($null -eq $targetContent) { $targetContent = '' }

    $bp = $targetContent.IndexOf($BeginMarker)
    $ep = $targetContent.IndexOf($EndMarker)
    if ($bp -ge 0 -and $ep -gt $bp) {
        # Existing managed block found -- replace just between markers.
        $before = $targetContent.Substring(0, $bp)
        $after  = $targetContent.Substring($ep + $EndMarker.Length)
        $new = $before + $block + $after
        Set-Content -Path $Target -Value $new -NoNewline
        Write-Host "  ${name}: managed block updated (other content preserved)"
    } else {
        # No managed block -- append, preserving existing file content.
        $tail = $targetContent.TrimEnd("`r","`n")
        $sep = if ($tail.Length -gt 0) { "`r`n`r`n" } else { '' }
        $new = $tail + $sep + $block
        Set-Content -Path $Target -Value $new -NoNewline
        Write-Host "  ${name}: managed block appended (existing content preserved)"
    }
}

Write-Host ""
Write-Host "Installing nuke-ai-fill"
Write-Host "  From: $here"
Write-Host "  To:   $Target"
if ($CodeOnly) { Write-Host "  Mode: code-only (skipping data/)" }
Write-Host ""

$null = New-Item -ItemType Directory -Path $Target -Force

# ---- code/ -> Target (bulk copy non-anchored, then handle anchored) ----
Write-Host "Installing code..."
$codeRoot = Join-Path $here 'code'

Get-ChildItem $codeRoot -Recurse -File | ForEach-Object {
    # Skip anchored files at the root; we will handle them after.
    if (($_.Directory.FullName -eq $codeRoot) -and ($_.Name -in $AnchoredFiles)) {
        return
    }
    $rel = $_.FullName.Substring($codeRoot.Length + 1)
    $dst = Join-Path $Target $rel
    $dstDir = Split-Path $dst -Parent
    if ($dstDir -and -not (Test-Path $dstDir)) {
        $null = New-Item -ItemType Directory -Path $dstDir -Force
    }
    Copy-Item $_.FullName $dst -Force
}

# Anchored files: append/update, never overwrite
foreach ($name in $AnchoredFiles) {
    $src = Join-Path $codeRoot $name
    if (Test-Path $src) {
        $dst = Join-Path $Target $name
        Update-AnchoredFile -Source $src -Target $dst
    }
}
Write-Host "  done."

if (-not $CodeOnly) {
    if (Test-Path "$here\data\runtime") {
        Write-Host "Installing runtime DLLs..."
        Copy-Item "$here\data\runtime\*" $Target -Recurse -Force
        Write-Host "  done."
    }

    if (Test-Path "$here\data\models") {
        Write-Host "Installing models (this takes a while)..."
        $null = New-Item -ItemType Directory -Path "$Target\models" -Force
        Copy-Item "$here\data\models\*" "$Target\models\" -Recurse -Force
        Write-Host "  done."
    }
}

Write-Host ""
Write-Host "Installed to $Target"
Write-Host ""
Write-Host "If Nuke doesn't see the plugins, add this to %USERPROFILE%\.nuke\init.py:"
Write-Host ""
Write-Host "  nuke.pluginAddPath(r'$Target')"
Write-Host ""
'@

Set-Content -Path "$Dest\install.ps1" -Value $installPs1 -Encoding UTF8
Write-Host ""
Write-Host "Wrote install.ps1"

# ---- Generate README.md ---------------------------------------------------

$codeBytes = (Get-ChildItem "$Dest\code" -Recurse -File | Measure-Object Length -Sum).Sum
$dataBytes = (Get-ChildItem "$Dest\data" -Recurse -File | Measure-Object Length -Sum).Sum

$readme = @"
# nuke-ai-fill distribution

Transferable build of the nuke-ai-fill Nuke plugins (AIGenerate and
AISmartFill), split for efficient distribution:

- **``code/``** ($("{0:N1}" -f ($codeBytes / 1MB)) MB) -- small, frequently updated. Plugin DLLs,
  Python scripts, conversion tools. Copy on every dev rebuild.
- **``data/``** ($("{0:N1}" -f ($dataBytes / 1GB)) GB) -- large, stable. CUDA + ONNX runtime DLLs
  and all model files. Copy once during first install.

## Before you run anything

PowerShell refuses to run unsigned scripts by default. Unblock
``install.ps1`` once before first use:

``````powershell
Unblock-File install.ps1
``````

## First install

1. Copy this whole folder to the target machine (USB, network share,
   whatever fits your network).
2. Open PowerShell in this folder and run:

   ``````
   .\install.ps1
   ``````

   Installs to ``%USERPROFILE%\.nuke\nuke-ai-fill\`` by default. Use
   ``-Target <path>`` for somewhere else.

3. Confirm Nuke picks up the plugins. If they don't appear in the node
   menu, add this to ``%USERPROFILE%\.nuke\init.py``:

   ``````python
   nuke.pluginAddPath(r'~/.nuke/nuke-ai-fill')
   ``````

## Updating after a rebuild

Just transfer the new ``code/`` folder and re-run with ``-CodeOnly``:

``````
.\install.ps1 -CodeOnly
``````

Skips all the gigabytes in ``data/``. Plugin DLLs and Python scripts
are refreshed in seconds.

## Append-only files (menu.py, init.py)

These two files at the install target are **never overwritten**. Their
nuke-ai-fill content lives between markers:

``````
# >>> nuke-ai-fill auto-managed >>>
...regenerated on every install...
# <<< nuke-ai-fill auto-managed <<<
``````

Anything you add **outside** the markers is preserved across updates.
Anything you edit **inside** the markers will be overwritten on the
next install -- edit dev-side and re-package instead.

## Converting new SDXL ControlNets

The included ``code/tools/convert_sdxl_controlnet.py`` rewrites
diffusers-format SDXL ControlNet ``.safetensors`` files into the
legacy naming sd.cpp expects. Required once per ControlNet model.
Needs Python with ``safetensors`` and ``numpy`` installed. See
NDK_NOTES section 22 in the repo for background.

If the target machine has no Python, do the conversion on the dev
machine first and ship the already-converted file in
``data/models/controlnets/``.

## Layout

``````
dist/
|-- code/                  $("{0:N1}" -f ($codeBytes / 1MB)) MB
|   |-- AIGenerate.dll
|   |-- AISmartFill.dll
|   |-- (small runtime + Python)
|   \`-- tools/
|       \`-- convert_sdxl_controlnet.py
|-- data/                  $("{0:N1}" -f ($dataBytes / 1GB)) GB
|   |-- runtime/           (large CUDA / ONNX DLLs)
|   \`-- models/            (.gguf / .safetensors / .onnx)
|-- install.ps1
\`-- README.md
``````
"@

Set-Content -Path "$Dest\README.md" -Value $readme -Encoding UTF8
Write-Host "Wrote README.md"

# ---- Summary --------------------------------------------------------------

Write-Host ""
Write-Host "Distribution ready: $Dest"
Write-Host ("  code:  {0,8:N1} MB" -f ($codeBytes / 1MB))
Write-Host ("  data:  {0,8:N2} GB" -f ($dataBytes / 1GB))
Write-Host ("  total: {0,8:N2} GB" -f (($codeBytes + $dataBytes) / 1GB))
Write-Host ""
Write-Host "Reminder: PowerShell may block the generated install.ps1 on the"
Write-Host "target machine. Unblock it there before running:"
Write-Host "    Unblock-File .\install.ps1"
Write-Host ""
