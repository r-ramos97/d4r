<#
.SYNOPSIS
    Sets up and checks d4r in a game folder on native Windows (docs/windows.md in the d4r repository).

.DESCRIPTION
    Run it in the game folder (the folder with the game's main .exe and the d4r folder):

        powershell -ExecutionPolicy Bypass -File d4r\setup.ps1

    It checks d4r's files, OptiScaler, the AMD HIP runtime and NVIDIA's two files; writes the native kernel
    manifests from a DLSS 310.7 or 310.9 nvngx_dlss.dll (d4r-manifest.exe); and applies d4r's settings to
    OptiScaler.ini, keeping the original as OptiScaler.ini.d4r-backup. With -CheckOnly it changes nothing.
    It exits with 0 when everything is in place, 1 when something is missing.

.PARAMETER GameFolder
    The game folder (default: the current folder).

.PARAMETER CheckOnly
    Only report; do not write manifests or change OptiScaler.ini.
#>
param(
    [string]$GameFolder = ".",
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$script:problems = 0
function Ok([string]$text) { Write-Host "  ok       $text" }
function Bad([string]$text) { Write-Host "  MISSING  $text"; $script:problems++ }
function Note([string]$text) { Write-Host "  note     $text" }

$game = (Resolve-Path -LiteralPath $GameFolder).Path
$d4r = Join-Path $game "d4r"
Write-Host "d4r install in $game"
if (-not (Test-Path -LiteralPath $d4r -PathType Container)) {
    Write-Host "no d4r folder here; run this in the folder with the game's main .exe"
    exit 1
}
if (Get-ChildItem -LiteralPath $game -Filter *.exe -File) { Ok "game executable next to d4r\" }
else { Note "no .exe next to d4r\ (it must be the game's main executable folder; Unreal: <Project>\Binaries\Win64)" }

# --- d4r ---------------------------------------------------------------------------------------------------
Write-Host "d4r"
foreach ($file in "nvapi64.dll", "d4r\nvngx.dll", "d4r\nvcuda.dll", "d4r\zluda\zluda_nvcuda.dll", "d4r\d4r.ini") {
    if (Test-Path -LiteralPath (Join-Path $game $file) -PathType Leaf) { Ok $file }
    else { Bad "$file (extract the d4r package again)" }
}
# d4r's version.dll loads nvapi64.dll before OptiScaler starts, so that OptiScaler enables DLSS
$version = Join-Path $game "version.dll"
if (-not (Test-Path -LiteralPath $version -PathType Leaf)) { Bad "version.dll (extract the d4r package again)" }
elseif ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($version)).Contains("d4r_preload_version")) {
    Ok "version.dll (d4r's: loads nvapi64.dll before OptiScaler)"
}
else { Bad "version.dll is not d4r's (another mod, or OptiScaler installed as version.dll): install OptiScaler as dxgi.dll and extract the d4r package again" }

# --- OptiScaler --------------------------------------------------------------------------------------------
Write-Host "OptiScaler"
$optiNames = "dxgi.dll", "winmm.dll", "dbghelp.dll", "d3d12.dll", "wininet.dll", "winhttp.dll", "OptiScaler.asi"
$opti = $optiNames | Where-Object { Test-Path -LiteralPath (Join-Path $game $_) -PathType Leaf } | Select-Object -First 1
if ($opti) { Ok "OptiScaler as $opti" }
else { Bad "OptiScaler: copy OptiScaler.dll from an OptiScaler 0.9.4 release here as dxgi.dll" }
$ini = Join-Path $game "OptiScaler.ini"
$settingsFile = Join-Path $d4r "optiscaler-d4r.settings"
if (-not (Test-Path -LiteralPath $ini -PathType Leaf)) {
    Bad "OptiScaler.ini: copy it from the same OptiScaler release, then run this script again"
}
elseif (Test-Path -LiteralPath $settingsFile -PathType Leaf) {
    $raw = [IO.File]::ReadAllText($ini)
    $lines = $raw -split "`n"
    $changed = $false
    $mismatch = @()
    foreach ($setting in Get-Content -LiteralPath $settingsFile) {
        $setting = $setting.Trim()
        if ($setting -eq "" -or $setting.StartsWith("#")) { continue }
        $path, $value = $setting -split "=", 2
        $section, $key = $path -split "\.", 2
        $start = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -eq "[$section]") { $start = $i; break } }
        if ($start -lt 0) { Note "OptiScaler.ini has no [$section]"; continue }
        $found = $false
        for ($i = $start + 1; $i -lt $lines.Count -and -not $lines[$i].StartsWith("["); $i++) {
            if ($lines[$i] -match "^$([regex]::Escape($key))\s*=\s*([^\r]*)") {
                $found = $true
                if ($Matches[1].Trim() -ne $value) {
                    $mismatch += "$section.$key=$value"
                    $cr = if ($lines[$i].EndsWith("`r")) { "`r" } else { "" }
                    $lines[$i] = "$key=$value$cr"
                    $changed = $true
                }
                break
            }
        }
        if (-not $found) { Note "OptiScaler.ini has no $key in [$section]" }
    }
    if ($mismatch.Count -eq 0) { Ok "OptiScaler.ini has d4r's settings" }
    elseif ($CheckOnly) { Bad "OptiScaler.ini needs: $($mismatch -join ', ') (run without -CheckOnly to set them)" }
    else {
        $backup = "$ini.d4r-backup"
        if (-not (Test-Path -LiteralPath $backup)) { Copy-Item -LiteralPath $ini -Destination $backup }
        [IO.File]::WriteAllText($ini, ($lines -join "`n"), (New-Object Text.UTF8Encoding($false)))
        Ok "OptiScaler.ini: set $($mismatch -join ', ') (original kept as OptiScaler.ini.d4r-backup)"
    }
}

# --- AMD HIP runtime ---------------------------------------------------------------------------------------
Write-Host "AMD HIP runtime"
$hipDirs = @()
if ($env:HIP_PATH) { $hipDirs += (Join-Path $env:HIP_PATH "bin") }
$hipDirs += (Join-Path $env:WINDIR "System32")
$hip = $null
foreach ($dir in $hipDirs) {
    foreach ($name in "amdhip64_7.dll", "amdhip64_6.dll") {
        $candidate = Join-Path $dir $name
        if (-not $hip -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $hip = $candidate }
    }
}
if ($hip) { Ok "HIP runtime: $hip" }
else { Bad "AMD HIP runtime (amdhip64_7.dll): install the AMD HIP SDK for Windows (it sets HIP_PATH)" }
if ($hip -and -not (Get-ChildItem -LiteralPath (Split-Path $hip) -Filter "amd_comgr*.dll" -File -ErrorAction SilentlyContinue)) {
    Note "no amd_comgr*.dll next to $hip; ZLUDA needs HIP's comgr to compile DLSS's kernels (install the HIP SDK)"
}
foreach ($gpu in Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue) {
    Note "GPU: $($gpu.Name) (driver $($gpu.DriverVersion))"
}

# --- NVIDIA's files ----------------------------------------------------------------------------------------
Write-Host "NVIDIA's files"
$dlss = Join-Path $d4r "nvngx_dlss.dll"
$dlssVersion = $null
if (Test-Path -LiteralPath $dlss -PathType Leaf) {
    $info = (Get-Item -LiteralPath $dlss).VersionInfo
    $dlssVersion = "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)"
    if ($info.FileMajorPart -eq 310 -and ($info.FileMinorPart -eq 7 -or $info.FileMinorPart -eq 9)) {
        Ok "d4r\nvngx_dlss.dll (DLSS $dlssVersion; native kernels written for 310.7 and 310.9)"
    }
    else { Note "d4r\nvngx_dlss.dll is DLSS $dlssVersion: kernels whose code changed run without native kernels (slower)" }
}
else { Bad "d4r\nvngx_dlss.dll: copy NVIDIA's DLSS library (310.7 or 310.9 recommended) here" }
if (Test-Path -LiteralPath (Join-Path $d4r "ngx\_nvngx.dll") -PathType Leaf) { Ok "d4r\ngx\_nvngx.dll (NVIDIA's NGX runtime)" }
else { Bad "d4r\ngx\_nvngx.dll: copy NVIDIA's NGX runtime here (from an NVIDIA driver package; tested: driver 596.36)" }

# --- native kernels ----------------------------------------------------------------------------------------
Write-Host "native kernels"
$kernels = Join-Path $d4r "kernels"
$folders = @()
if (Test-Path -LiteralPath $kernels -PathType Container) {
    $folders = @(Get-ChildItem -LiteralPath $kernels -Directory -Recurse |
        Where-Object { Get-ChildItem -LiteralPath $_.FullName -Filter *.hsaco -File })
}
if ($folders.Count -eq 0) { Note "no native kernels in d4r\kernels: DLSS runs from NVIDIA's code through ZLUDA (much slower)" }
else {
    $unlisted = @($folders | Where-Object { -not (Test-Path -LiteralPath (Join-Path $_.FullName "d4r-kernels.txt")) })
    if ($unlisted.Count -eq 0) { Ok "$($folders.Count) kernel folders with manifests" }
    elseif ($CheckOnly) { Bad "$($unlisted.Count) kernel folders have no manifest yet (run without -CheckOnly)" }
    elseif ($dlssVersion -and $dlssVersion -match "^310\.(7|9)\.") {
        & (Join-Path $d4r "d4r-manifest.exe") $kernels $dlss
        if ($LASTEXITCODE -eq 0) { Ok "manifests written from DLSS $dlssVersion" }
        else { Bad "d4r-manifest.exe failed ($LASTEXITCODE); see above" }
    }
    else {
        Bad "$($unlisted.Count) kernel folders have no manifest: put a DLSS 310.7 or 310.9 nvngx_dlss.dll in d4r\ and run this again (the game may use any version afterwards)"
    }
}

Write-Host ""
if ($script:problems -eq 0) {
    Write-Host "Everything d4r needs is in place. Start the game and choose DLSS; d4r's log is d4r\d4r_nvngx.log."
    exit 0
}
Write-Host "$($script:problems) thing(s) to fix above."
exit 1
