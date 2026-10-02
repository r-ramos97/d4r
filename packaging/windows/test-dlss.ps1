<#
.SYNOPSIS
    Runs NVIDIA's DLSS through d4r on synthetic frames, without a game or OptiScaler.

.DESCRIPTION
    Run it in the game folder after d4r\setup.ps1 reports everything in place:

        powershell -ExecutionPolicy Bypass -File d4r\test-dlss.ps1

    It first checks whether AMD's driver lets HIP share video memory with D3D12 (d4r\tools\d4r-interop-probe.exe,
    report in d4r\interop-report.txt), which d4r's VRAM path needs. Then it drives d4r\nvngx.dll the way
    OptiScaler does (d4r\tools\d4r-harness.exe, the D3D12 DLSS harness of the d4r repository): a D3D12 device on
    your GPU, moving synthetic colour, depth and motion vectors at 1280x720, DLSS to 2560x1440. It prints each step
    and the end of d4r's logs, and writes d4r\test-output.raw.bmp, the last DLSS frame. The first run can take
    minutes while ZLUDA compiles DLSS's kernels.

.PARAMETER Model
    DLSS model: K (DLSS 4, default), E (DLSS 3 CNN) or M (DLSS 4.5).

.PARAMETER Frames
    Frames to evaluate (default 60).

.PARAMETER Rgba8
    Colour and output in RGBA8 instead of DLSS's own RGBA16F, on a still scene: checks d4r's format conversion on
    the GPU (as for games whose resources are in other formats).

.PARAMETER SameFrame
    Run as with FrameAge = 0 in d4r.ini: each frame waits on the GPU for its own DLSS result (experimental on
    Windows). The log then says whether the wait worked ("same-frame wait" lines).
#>
param(
    [ValidateSet("K", "E", "M")][string]$Model = "K",
    [int]$Frames = 60,
    [string]$GameFolder = ".",
    [switch]$SameFrame,
    [switch]$Rgba8
)

$game = (Resolve-Path -LiteralPath $GameFolder).Path
$d4r = Join-Path $game "d4r"
$harness = Join-Path $d4r "tools\d4r-harness.exe"
if (-not (Test-Path -LiteralPath $harness -PathType Leaf)) {
    Write-Host "no d4r\tools\d4r-harness.exe here; run this in the game folder with the d4r package"
    exit 2
}
# First, quickly: can AMD's driver share VRAM between D3D12 and HIP? (d4r's VRAM path; the answer goes in
# d4r\interop-report.txt.) A driver that hangs here is stopped after a minute; DLSS then still runs below.
$probe = Join-Path $d4r "tools\d4r-interop-probe.exe"
$report = Join-Path $d4r "interop-report.txt"
if (Test-Path -LiteralPath $probe -PathType Leaf) {
    Write-Host "VRAM sharing check (d4r\tools\d4r-interop-probe.exe)..."
    $process = Start-Process -FilePath $probe -ArgumentList ('"{0}"' -f (Join-Path $d4r "nvcuda.dll")) `
        -RedirectStandardOutput $report -NoNewWindow -PassThru
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        Add-Content -LiteralPath $report "RESULT: the check did not finish within a minute (stopped)"
    }
    Get-Content -LiteralPath $report | Where-Object { $_ -match "^(RESULT|VRAM sharing|d4r keeps|Same-frame)" } |
        ForEach-Object { Write-Host "  $_" }
    Write-Host "  (details in $report)"
    Write-Host ""
}

$env:D4R_DLSS_PRESET = @{ K = "11"; E = "5"; M = "13" }[$Model]
if ($SameFrame) {
    # what d4r.ini's FrameAge = 0 sets; variables set here take precedence over d4r.ini
    $env:D4R_SHIM_SPLIT_FRAME = "1"
    $env:D4R_SHIM_MAX_IN_FLIGHT = "3"
}
if ($Rgba8) {
    $env:D4R_HARNESS_RGBA8 = "1" # a still scene: the harness's moving scenes are RGBA16F
    Remove-Item Env:D4R_HARNESS_MOTION_SCENE -ErrorAction SilentlyContinue
} else {
    $env:D4R_HARNESS_MOTION_SCENE = "1"
}
$output = Join-Path $d4r "test-output.raw"
Remove-Item -LiteralPath "$output.bmp" -ErrorAction SilentlyContinue

Write-Host ("DLSS model $Model, $Frames frames, 1280x720 -> 2560x1440" + $(if ($SameFrame) { ", same-frame results" } else { "" }) +
            $(if ($Rgba8) { ", RGBA8 colour and output (format conversion)" } else { "" }))
$watch = [Diagnostics.Stopwatch]::StartNew()
& $harness (Join-Path $d4r "nvngx.dll") $output $Frames 1280 720 2560 1440
$code = $LASTEXITCODE
Write-Host ("harness exit code {0} after {1:N1} s" -f $code, $watch.Elapsed.TotalSeconds)

foreach ($log in (Join-Path $d4r "d4r_nvngx.log"), (Join-Path $game "d4r_nvapi.log")) {
    if (Test-Path -LiteralPath $log) {
        Write-Host ""
        Write-Host "--- end of $log"
        Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ }
    }
}
Write-Host ""
if ($code -eq 0 -and (Test-Path -LiteralPath "$output.bmp")) {
    Write-Host "DLSS ran. Look at $output.bmp: a sharp moving test pattern means it works; black or noise does not."
    exit 0
}
Write-Host "DLSS did not complete. Please report this output together with d4r\d4r_nvngx.log, d4r_nvapi.log and"
Write-Host "d4r\interop-report.txt."
exit 1
