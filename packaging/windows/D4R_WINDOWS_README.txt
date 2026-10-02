d4r @VERSION@ for native Windows (preview): NVIDIA DLSS on AMD Radeon RDNA3 and RDNA4
================================================================================

This is a test build of d4r for Windows 10 and 11, without Linux or Proton. It runs NVIDIA's own DLSS
Super Resolution on an AMD Radeon RDNA3 or RDNA4 GPU in DirectX 12 games: OptiScaler catches the game's
DLSS calls, d4r runs NVIDIA's DLSS through ZLUDA (CUDA on AMD's HIP), with the heaviest DLSS 4 and 4.5
network layers replaced by kernels written for RDNA3 and RDNA4.

Status: the Windows build is new and has not run on a real GPU yet. Every part was tested against mock
libraries on Windows and Wine; the first runs on real hardware will find what is still missing. By default
each frame shows the newest finished DLSS result, one frame old (FrameAge = 1). FrameAge = 0 in
d4r\d4r.ini shows each frame's own result (experimental: the game's GPU work waits for DLSS). DLSS's
inputs and output stay in video memory when AMD's driver lets HIP share it with D3D12; otherwise they are
copied through system memory, which costs some speed. test-dlss.ps1 checks both on your PC first.
docs/windows.md in the source repository explains the design and the plan.


What you need
-------------
- An AMD RDNA3 or RDNA4 GPU (Radeon RX 7000 or RX 9000 series) and a current AMD Adrenalin driver.
- The AMD HIP SDK for Windows (6.4 or later; it sets HIP_PATH). ZLUDA uses its HIP runtime and compiler.
- OptiScaler 0.9.4: its OptiScaler.dll and OptiScaler.ini.
- Two NVIDIA files, which this package does not include:
  - nvngx_dlss.dll, the DLSS library, version 310.7 or 310.9 for the native kernels (tested on Linux:
    310.7.0 and 310.9.1). Many games ship one, often an older version. Other versions run too, but the
    kernels whose code changed run without native kernels (slower).
  - _nvngx.dll, NVIDIA's NGX runtime from an NVIDIA Windows display driver (tested on Linux: driver
    596.36, file version 32.0.15.9636). Open the driver installer with 7-Zip and look for _nvngx.dll.


Install
-------
1. Extract this package into the folder that holds the game's main .exe. For Unreal Engine games that is
   <game>\<Project>\Binaries\Win64\, next to <Project>-Win64-Shipping.exe. You get nvapi64.dll,
   version.dll, this file and a d4r folder. (OptiScaler enables DLSS only on an NVIDIA GPU; d4r's
   version.dll loads d4r's nvapi64.dll before OptiScaler starts, so it sees one. If the folder already has
   a version.dll from another mod, the two cannot be used together.)
2. Copy OptiScaler.dll from OptiScaler 0.9.4 into the same folder, renamed to dxgi.dll, together with
   its OptiScaler.ini.
3. Copy nvngx_dlss.dll into the d4r folder, and _nvngx.dll into d4r\ngx.
4. Open PowerShell in the game folder and run:
       powershell -ExecutionPolicy Bypass -File d4r\setup.ps1
   It checks everything, writes the native kernel manifests from your nvngx_dlss.dll (310.7 or 310.9) and
   puts d4r's settings in OptiScaler.ini (the original stays as OptiScaler.ini.d4r-backup). Fix what it
   reports and run it again until it says everything is in place.
5. Optional, but the best first test: run DLSS without the game, on synthetic frames:
       powershell -ExecutionPolicy Bypass -File d4r\test-dlss.ps1
   It first checks whether your driver can share video memory between D3D12 and HIP (report in
   d4r\interop-report.txt), then shows each step of DLSS and writes d4r\test-output.raw.bmp, the last
   frame DLSS produced.
6. Start the game and choose DLSS in its graphics settings.

The first time DLSS starts the game can freeze for a minute or more while DLSS's GPU kernels are
compiled. ZLUDA caches them in %LOCALAPPDATA%\zluda, so later starts are quick.


Settings
--------
d4r\d4r.ini holds d4r's settings for the game: the DLSS model (K: DLSS 4, default; E: DLSS 3 CNN; M:
DLSS 4.5), FrameAge (0 = each frame's own result, experimental; 1 = one frame old, the default; 2-3 =
more fps), PreferAccuracy and, on RDNA4, NativeFp8. The
file explains each one. Restart the game after a change.

PCs with two AMD GPUs (for example a Ryzen 7000/9000 with its integrated graphics enabled and a Radeon
card): d4r runs DLSS on the GPU the game renders with. To force another one, add D4R_HIP_DEVICE = <number>
under [Env] in d4r.ini; d4r\d4r_nvngx.log lists the numbers ("HIP device").


If something goes wrong
-----------------------
The quickest way to report a problem: run
       powershell -ExecutionPolicy Bypass -File d4r\collect-logs.ps1
in the game folder. It puts everything below, and the facts about your PC a report needs (Windows build,
GPU drivers, HIP SDK, file versions), into d4r-report-<date>.zip. These are the files it collects:
- d4r\d4r_nvngx.log: d4r's log, rewritten at every launch. Lines with "d4r:" and "nvcuda bridge:" name
  missing files or libraries; "native kernel" lines say which native kernels were used.
- d4r_nvapi.log: every NVAPI function the game, OptiScaler and NGX asked for. "unimplemented" lines show
  what may still be needed.
- d4r\interop-report.txt, written by d4r\test-dlss.ps1.
- OptiScaler.log (set LogToFile=true in OptiScaler.ini's [Log] section).
- The output of d4r\setup.ps1 -CheckOnly.

Do not use d4r in games with anti-cheat: OptiScaler's DLL injection can get an account banned.
Not supported: DLSS Frame Generation, DLSS Ray Reconstruction, DirectX 11 and Vulkan games.


Uninstall
---------
Delete nvapi64.dll, version.dll, dxgi.dll, OptiScaler.ini, OptiScaler.log, this file and the d4r folder from the game
folder (restore OptiScaler.ini.d4r-backup if you used OptiScaler before). The caches in
%LOCALAPPDATA%\zluda and %LOCALAPPDATA%\d4r can be deleted too.


Licenses
--------
d4r is Apache License 2.0. ZLUDA is Apache 2.0 or MIT and contains LLVM (Apache 2.0 with LLVM
exceptions); d4r\licenses has the texts and d4r\source\SOURCES.txt says where each file comes from. d4r
is not affiliated with NVIDIA, AMD or the OptiScaler project. NVIDIA's files are yours to supply, under
NVIDIA's terms.
