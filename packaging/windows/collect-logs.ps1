<#
.SYNOPSIS
    Collects d4r's logs and the facts about this PC that a bug report needs into one zip file.

.DESCRIPTION
    Run it in the game folder after a test or a game session:

        powershell -ExecutionPolicy Bypass -File d4r\collect-logs.ps1

    It writes d4r-report-<date>.zip in the game folder with d4r's, NVAPI's and OptiScaler's logs, d4r.ini,
    OptiScaler.ini, the interop report, the output of setup.ps1 -CheckOnly, and a system.txt with the Windows
    build, the GPUs and their drivers, the HIP SDK, and the versions of the NVIDIA and d4r files. The logs contain
    file paths, which may include your Windows user name.
#>
param([string]$GameFolder = ".")

$game = (Resolve-Path -LiteralPath $GameFolder).Path
$d4r = Join-Path $game "d4r"
if (-not (Test-Path -LiteralPath (Join-Path $d4r "nvngx.dll"))) {
    Write-Host "no d4r\nvngx.dll here; run this in the game folder with the d4r package"
    exit 2
}
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$staging = Join-Path ([IO.Path]::GetTempPath()) "d4r-report-$stamp"
New-Item -ItemType Directory -Path $staging | Out-Null

# the logs and settings, where they exist
$files = @(
    (Join-Path $d4r "d4r_nvngx.log"), (Join-Path $game "d4r_nvapi.log"), (Join-Path $d4r "interop-report.txt"),
    (Join-Path $d4r "d4r.ini"), (Join-Path $game "OptiScaler.log"), (Join-Path $game "OptiScaler.ini"),
    (Join-Path $d4r "source\SOURCES.txt")
)
$collected = @()
foreach ($file in $files) {
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        Copy-Item -LiteralPath $file -Destination $staging
        $collected += Split-Path -Leaf $file
    }
}

# setup.ps1's check, without changing anything
$setup = Join-Path $d4r "setup.ps1"
if (Test-Path -LiteralPath $setup) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $setup -GameFolder $game -CheckOnly *>&1 |
        Out-File -LiteralPath (Join-Path $staging "setup-check.txt") -Encoding utf8
    $collected += "setup-check.txt"
}

function Version-Of([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "missing" }
    $item = Get-Item -LiteralPath $path
    $version = $item.VersionInfo.FileVersion
    if (-not $version) { $version = "no version resource" }
    return "{0} ({1} bytes, {2:yyyy-MM-dd})" -f $version, $item.Length, $item.LastWriteTime
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("d4r report $stamp")
$os = Get-CimInstance Win32_OperatingSystem
$lines.Add("Windows: $($os.Caption) $($os.Version) build $($os.BuildNumber)")
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$lines.Add("CPU: $($cpu.Name)")
$lines.Add(("RAM: {0:N1} GB" -f ($os.TotalVisibleMemorySize / 1MB)))
foreach ($gpu in Get-CimInstance Win32_VideoController) {
    $lines.Add("GPU: $($gpu.Name), driver $($gpu.DriverVersion) ($($gpu.DriverDate)), status $($gpu.Status)")
}
$hip = $env:HIP_PATH
$lines.Add("HIP_PATH: $(if ($hip) { $hip } else { 'not set' })")
if ($hip) {
    foreach ($name in "amdhip64_7.dll", "amdhip64_6.dll") {
        $path = Join-Path $hip "bin\$name"
        if (Test-Path -LiteralPath $path) { $lines.Add("  $name $(Version-Of $path)") }
    }
}
foreach ($name in "amdhip64_7.dll", "amdhip64_6.dll", "amdhip64.dll") {
    $path = Join-Path $env:WINDIR "System32\$name"
    if (Test-Path -LiteralPath $path) { $lines.Add("System32\$name $(Version-Of $path)") }
}
$lines.Add("")
$lines.Add("Game folder: $game")
foreach ($name in "nvapi64.dll", "version.dll", "dxgi.dll", "winmm.dll", "OptiScaler.dll", "nvngx_dlss.dll") {
    $path = Join-Path $game $name
    if (Test-Path -LiteralPath $path) { $lines.Add("  $name $(Version-Of $path)") }
}
foreach ($name in "nvngx.dll", "nvcuda.dll", "nvngx_dlss.dll", "ngx\_nvngx.dll", "zluda\zluda_nvcuda.dll") {
    $lines.Add("  d4r\$name $(Version-Of (Join-Path $d4r $name))")
}
$kernels = Join-Path $d4r "kernels"
if (Test-Path -LiteralPath $kernels) {
    foreach ($folder in Get-ChildItem -LiteralPath $kernels -Directory) {
        $manifest = Test-Path -LiteralPath (Join-Path $folder.FullName "d4r-kernels.txt")
        $count = (Get-ChildItem -LiteralPath $folder.FullName -Filter *.hsaco).Count
        $lines.Add("  d4r\kernels\$($folder.Name): $count kernels, manifest $(if ($manifest) { 'present' } else { 'missing' })")
    }
}
foreach ($cache in (Join-Path $env:LOCALAPPDATA "zluda"), (Join-Path $env:LOCALAPPDATA "d4r")) {
    if (Test-Path -LiteralPath $cache) {
        $size = (Get-ChildItem -LiteralPath $cache -Recurse -File -ErrorAction SilentlyContinue |
                 Measure-Object -Property Length -Sum).Sum
        $lines.Add(("cache {0}: {1:N1} MB" -f $cache, ($size / 1MB)))
    }
}
$lines | Out-File -LiteralPath (Join-Path $staging "system.txt") -Encoding utf8
$collected += "system.txt"

$zip = Join-Path $game "d4r-report-$stamp.zip"
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip -Force
Remove-Item -LiteralPath $staging -Recurse -Force
Write-Host "Collected: $($collected -join ', ')"
Write-Host "Report: $zip"
Write-Host "It contains file paths, which may include your Windows user name."
