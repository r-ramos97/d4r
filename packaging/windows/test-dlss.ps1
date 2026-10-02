<#
.SYNOPSIS
    Runs NVIDIA's DLSS through d4r on synthetic frames, without a game or OptiScaler.

.DESCRIPTION
    Run it in the game folder after d4r\setup.ps1 reports everything in place:

        powershell -ExecutionPolicy Bypass -File d4r\test-dlss.ps1

    It drives d4r\nvngx.dll the way OptiScaler does (d4r\tools\d4r-harness.exe, the D3D12 DLSS harness of the d4r
    repository): a D3D12 device on your GPU, moving synthetic colour, depth and motion vectors at 1280x720, DLSS to
    2560x1440. It prints each step and the end of d4r's logs, and writes d4r\test-output.raw.bmp, the last DLSS
    frame. The first run can take minutes while ZLUDA compiles DLSS's kernels.

.PARAMETER Model
    DLSS model: K (DLSS 4, default), E (DLSS 3 CNN) or M (DLSS 4.5).

.PARAMETER Frames
    Frames to evaluate (default 60).
#>
param(
    [ValidateSet("K", "E", "M")][string]$Model = "K",
    [int]$Frames = 60,
    [string]$GameFolder = "."
)

$game = (Resolve-Path -LiteralPath $GameFolder).Path
$d4r = Join-Path $game "d4r"
$harness = Join-Path $d4r "tools\d4r-harness.exe"
if (-not (Test-Path -LiteralPath $harness -PathType Leaf)) {
    Write-Host "no d4r\tools\d4r-harness.exe here; run this in the game folder with the d4r package"
    exit 2
}
$env:D4R_DLSS_PRESET = @{ K = "11"; E = "5"; M = "13" }[$Model]
$env:D4R_HARNESS_MOTION_SCENE = "1"
$output = Join-Path $d4r "test-output.raw"
Remove-Item -LiteralPath "$output.bmp" -ErrorAction SilentlyContinue

Write-Host "DLSS model $Model, $Frames frames, 1280x720 -> 2560x1440"
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
Write-Host "DLSS did not complete. Please report this output together with d4r\d4r_nvngx.log and d4r_nvapi.log."
exit 1
