# deploy.ps1
#
# Move staged source files from C:\dev\SmartFill\files into the
# clean project tree at C:\dev\SmartFill.
#
# Validates ASCII purity on every .h/.cpp/.cu/.cuh per NDK_NOTES 6.1
# before placing the file. Refuses to install non-ASCII source.
#
# Usage:
#   .\deploy.ps1                  # apply changes
#   .\deploy.ps1 -DryRun          # show plan, do nothing
#   .\deploy.ps1 -Force           # overwrite existing target files
#   .\deploy.ps1 -KeepSource      # leave the staging dir intact after move

[CmdletBinding()]
param(
    [string]$SourceDir  = "C:\dev\SmartFill\files",
    [string]$DestRoot   = "C:\dev\SmartFill",
    [switch]$Force,
    [switch]$DryRun,
    [switch]$KeepSource
)

$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------------
# Manifest
# ----------------------------------------------------------------------

$Manifest = @(
    @{ Source = 'PLAN.md';                  Target = 'PLAN.md';                          Ascii = $false },
    @{ Source = 'README.md';                Target = 'README.md';                        Ascii = $false },
    @{ Source = 'LICENSE';                  Target = 'LICENSE';                          Ascii = $false },
    @{ Source = 'THIRD_PARTY_LICENSES.md';  Target = 'THIRD_PARTY_LICENSES.md';          Ascii = $false },
    @{ Source = '.gitignore';               Target = '.gitignore';                       Ascii = $false },
    @{ Source = 'menu.py';                  Target = 'menu.py';                          Ascii = $false },
    @{ Source = 'install_user_menu.cmake';  Target = 'cmake\install_user_menu.cmake';    Ascii = $true  },
    @{ Source = 'find_onnxruntime.cmake';   Target = 'cmake\find_onnxruntime.cmake';     Ascii = $true  },
    @{ Source = 'find_sdcpp.cmake';         Target = 'cmake\find_sdcpp.cmake';           Ascii = $true  },
    @{ Source = 'ai_hash.h';                Target = 'core\include\ai_hash.h';           Ascii = $true  },
    @{ Source = 'ai_cache.h';               Target = 'core\include\ai_cache.h';          Ascii = $true  },
    @{ Source = 'ai_worker.h';              Target = 'core\include\ai_worker.h';         Ascii = $true  },
    @{ Source = 'lama_session.h';           Target = 'core\include\lama_session.h';      Ascii = $true  },
    @{ Source = 'lama_tiler.h';             Target = 'core\include\lama_tiler.h';        Ascii = $true  },
    @{ Source = 'sd_session.h';             Target = 'core\include\sd_session.h';        Ascii = $true  },
    @{ Source = 'image_cache.h';            Target = 'core\include\image_cache.h';       Ascii = $true  },
    @{ Source = 'plugin_path.h';            Target = 'core\include\plugin_path.h';       Ascii = $true  },
    @{ Source = 'ai_hash.cpp';              Target = 'core\src\ai_hash.cpp';             Ascii = $true  },
    @{ Source = 'ai_cache.cpp';             Target = 'core\src\ai_cache.cpp';            Ascii = $true  },
    @{ Source = 'ai_worker.cpp';            Target = 'core\src\ai_worker.cpp';           Ascii = $true  },
    @{ Source = 'lama_session.cpp';         Target = 'core\src\lama_session.cpp';        Ascii = $true  },
    @{ Source = 'lama_tiler.cpp';           Target = 'core\src\lama_tiler.cpp';          Ascii = $true  },
    @{ Source = 'sd_session.cpp';           Target = 'core\src\sd_session.cpp';          Ascii = $true  },
    @{ Source = 'image_cache.cpp';          Target = 'core\src\image_cache.cpp';         Ascii = $true  },
    @{ Source = 'plugin_path.cpp';          Target = 'core\src\plugin_path.cpp';         Ascii = $true  },
    @{ Source = 'AISmartFill.cpp';          Target = 'ops\AISmartFill\AISmartFill.cpp';  Ascii = $true  },
    @{ Source = 'dll_bootstrap.cpp';        Target = 'ops\AISmartFill\dll_bootstrap.cpp'; Ascii = $true },
    @{ Source = 'AIGenerate.cpp';           Target = 'ops\AIGenerate\AIGenerate.cpp';    Ascii = $true  },
    @{ Source = 'AIGenerate_CMakeLists.txt'; Target = 'ops\AIGenerate\CMakeLists.txt';   Ascii = $true  }
)

$CMakeManifest = @(
    @{ Signature = 'project(nuke_ai_fill';                   Target = 'CMakeLists.txt'                 },
    @{ Signature = '# nuke-ai-fill / core / CMakeLists.txt'; Target = 'core\CMakeLists.txt'            },
    @{ Signature = 'add_library(AISmartFill MODULE';         Target = 'ops\AISmartFill\CMakeLists.txt' }
)

# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

function Test-Ascii {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    foreach ($b in $bytes) {
        if ($b -ge 0x80) { return $false }
    }
    return $true
}

function Get-FileHashSha256 {
    param([string]$Path)
    (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLower()
}

function Plan-Move {
    param([string]$From, [string]$To, [bool]$RequireAscii)
    if (-not (Test-Path $From)) {
        # Not in staging. If already at destination, that's a no-op,
        # not a blocker - user only re-downloaded what changed.
        if (Test-Path $To) {
            return @{ Status = 'already-deployed'; From = $From; To = $To }
        }
        return @{ Status = 'missing'; From = $From; To = $To }
    }
    if ($RequireAscii -and -not (Test-Ascii $From)) {
        return @{ Status = 'bad-ascii'; From = $From; To = $To }
    }
    if (Test-Path $To) {
        if ((Get-FileHashSha256 $From) -eq (Get-FileHashSha256 $To)) {
            return @{ Status = 'identical'; From = $From; To = $To }
        }
        if (-not $Force) {
            return @{ Status = 'conflict'; From = $From; To = $To }
        }
        return @{ Status = 'overwrite'; From = $From; To = $To }
    }
    return @{ Status = 'move'; From = $From; To = $To }
}

function Find-CMakeFiles {
    param([string]$SourceDir)
    # Relaxed: 'CMakeLists' anywhere in the name, not just as a prefix.
    Get-ChildItem -Path $SourceDir -Filter '*CMakeLists*.txt' -File `
        -ErrorAction SilentlyContinue | Sort-Object Name
}

# ----------------------------------------------------------------------
# Pre-flight
# ----------------------------------------------------------------------

if (-not (Test-Path $SourceDir)) {
    Write-Host "Source directory not found: $SourceDir" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "nuke-ai-fill deploy" -ForegroundColor Cyan
Write-Host "  Source : $SourceDir"
Write-Host "  Dest   : $DestRoot"
if ($DryRun) { Write-Host "  Mode   : DRY RUN (no files will be moved)" -ForegroundColor Yellow }
if ($Force)  { Write-Host "  Force  : ON (existing files will be overwritten)" -ForegroundColor Yellow }
Write-Host ""

# ----------------------------------------------------------------------
# Build plan
# ----------------------------------------------------------------------

$plans = @()

foreach ($entry in $Manifest) {
    $src = Join-Path $SourceDir $entry.Source
    $dst = Join-Path $DestRoot  $entry.Target
    $plans += Plan-Move -From $src -To $dst -RequireAscii $entry.Ascii
}

$cmakeFiles   = @(Find-CMakeFiles $SourceDir)
$cmakeMapping = @{}

foreach ($f in $cmakeFiles) {
    $content = Get-Content -Path $f.FullName -Raw -ErrorAction SilentlyContinue
    $matched = $false
    foreach ($cm in $CMakeManifest) {
        if ($content -and $content.Contains($cm.Signature)) {
            $cmakeMapping[$f.FullName] = Join-Path $DestRoot $cm.Target
            $matched = $true
            break
        }
    }
    if (-not $matched) {
        Write-Host "  WARNING: unrecognized CMakeLists file: $($f.FullName)" -ForegroundColor Yellow
    }
}

# Detect content-identical duplicates targeting the same destination.
$dupGroups     = $cmakeMapping.GetEnumerator() | Group-Object -Property Value
$dupesToRemove = @()
foreach ($g in $dupGroups) {
    if ($g.Count -gt 1) {
        $sortedByName = $g.Group | Sort-Object -Property Key
        $keeper       = $sortedByName[0].Key
        $keeperHash   = Get-FileHashSha256 $keeper
        foreach ($other in $sortedByName[1..($sortedByName.Count - 1)]) {
            if ((Get-FileHashSha256 $other.Key) -eq $keeperHash) {
                $dupesToRemove += $other.Key
            } else {
                Write-Host "  WARNING: multiple files map to $($g.Name) with DIFFERENT content:" -ForegroundColor Yellow
                Write-Host "    keeping : $keeper"
                Write-Host "    ignoring: $($other.Key)"
            }
            $cmakeMapping.Remove($other.Key) | Out-Null
        }
    }
}

$cmakeTargetsFound    = $cmakeMapping.Values | Sort-Object -Unique
$cmakeTargetsExpected = $CMakeManifest | ForEach-Object { Join-Path $DestRoot $_.Target } | Sort-Object -Unique
$cmakeMissing         = $cmakeTargetsExpected | Where-Object { $_ -notin $cmakeTargetsFound }
foreach ($m in $cmakeMissing) {
    if (Test-Path $m) {
        $plans += @{ Status = 'already-deployed'; From = '<no source provided>'; To = $m }
    } else {
        $plans += @{ Status = 'missing'; From = '<no CMakeLists with matching signature>'; To = $m }
    }
}

foreach ($pair in $cmakeMapping.GetEnumerator()) {
    $plans += Plan-Move -From $pair.Key -To $pair.Value -RequireAscii $false
}

# ----------------------------------------------------------------------
# Show plan
# ----------------------------------------------------------------------

Write-Host "Plan:" -ForegroundColor Cyan
$statusColor = @{
    'move' = 'White'; 'overwrite' = 'Yellow'; 'identical' = 'DarkGray'
    'already-deployed' = 'DarkGray'
    'conflict' = 'Red'; 'missing' = 'Red'; 'bad-ascii' = 'Red'
}
foreach ($p in $plans) {
    $color = $statusColor[$p.Status]; if (-not $color) { $color = 'White' }
    $fromShort = $p.From -replace [regex]::Escape($SourceDir), '<src>'
    $toShort   = $p.To   -replace [regex]::Escape($DestRoot),  '<dst>'
    Write-Host ("  [{0,-9}] {1}  ->  {2}" -f $p.Status, $fromShort, $toShort) -ForegroundColor $color
}
if ($dupesToRemove.Count -gt 0) {
    Write-Host ""
    Write-Host "Duplicate sources (same content, will be removed):" -ForegroundColor DarkGray
    foreach ($d in $dupesToRemove) {
        Write-Host ("  [cleanup ] " + ($d -replace [regex]::Escape($SourceDir), '<src>')) -ForegroundColor DarkGray
    }
}
Write-Host ""

# ----------------------------------------------------------------------
# Bail on blockers
# ----------------------------------------------------------------------

$blockers = @($plans | Where-Object { $_.Status -in @('conflict','missing','bad-ascii') })
if ($blockers.Count -gt 0) {
    Write-Host "Refusing to proceed:" -ForegroundColor Red
    foreach ($b in $blockers) {
        switch ($b.Status) {
            'conflict'  { Write-Host "  CONFLICT  : $($b.To)  (use -Force to overwrite)" }
            'missing'   { Write-Host "  MISSING   : $($b.From)" }
            'bad-ascii' { Write-Host "  BAD ASCII : $($b.From)" }
        }
    }
    Write-Host ""
    exit 2
}

if ($DryRun) {
    Write-Host "Dry run complete. Re-run without -DryRun to apply." -ForegroundColor Cyan
    exit 0
}

# ----------------------------------------------------------------------
# Execute
# ----------------------------------------------------------------------

$movedCount = 0; $skippedCount = 0; $cleanedCount = 0
foreach ($p in $plans) {
    if ($p.Status -eq 'identical') {
        Remove-Item -Path $p.From -Force; $skippedCount++; continue
    }
    if ($p.Status -eq 'already-deployed') {
        $skippedCount++; continue
    }
    $dir = Split-Path -Parent $p.To
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    if ($p.Status -eq 'overwrite' -and (Test-Path $p.To)) { Remove-Item -Path $p.To -Force }
    Move-Item -Path $p.From -Destination $p.To
    $movedCount++
}
foreach ($d in $dupesToRemove) {
    if (Test-Path $d) { Remove-Item -Path $d -Force; $cleanedCount++ }
}

Write-Host ""
Write-Host "Moved   : $movedCount" -ForegroundColor Green
Write-Host "Skipped : $skippedCount (already in place)" -ForegroundColor DarkGray
if ($cleanedCount -gt 0) {
    Write-Host "Cleaned : $cleanedCount (duplicate sources)" -ForegroundColor DarkGray
}

if (-not $KeepSource) {
    $remaining = @(Get-ChildItem -Path $SourceDir -Force -ErrorAction SilentlyContinue)
    if ($remaining.Count -eq 0) {
        Remove-Item -Path $SourceDir -Force
        Write-Host "Removed empty staging dir: $SourceDir" -ForegroundColor DarkGray
    } else {
        Write-Host ""
        Write-Host "Staging dir not empty - left in place:" -ForegroundColor Yellow
        foreach ($r in $remaining) { Write-Host "  $($r.Name)" }
    }
}

Write-Host ""
Write-Host "Project tree at $DestRoot :" -ForegroundColor Cyan
Get-ChildItem -Path $DestRoot -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\files\\' -and $_.FullName -notmatch '\\build\\' } |
    Sort-Object FullName |
    ForEach-Object {
        Write-Host ("  " + $_.FullName.Substring($DestRoot.Length).TrimStart('\'))
    }
Write-Host ""
Write-Host "Done." -ForegroundColor Green
