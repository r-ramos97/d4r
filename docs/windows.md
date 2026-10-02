# Native Windows

d4r was written for Linux and Proton. This page describes the native Windows build: how the pieces map onto
Windows, what differs, how it is built and tested, and what is still open. The user guide that ships in the
package is [packaging/windows/D4R_WINDOWS_README.txt](../packaging/windows/D4R_WINDOWS_README.txt).

**Status:** preview. Every component builds in CI and passes its tests against mock libraries on a Windows
runner and under Wine. Nothing has run on a real AMD GPU under Windows yet; the first hardware runs will show
what is missing.

## How it maps

```
                      Linux / Proton                              Windows
game (D3D12)          vkd3d-proton (d4r-patched)                  AMD's D3D12 driver
OptiScaler            dxgi.dll                                    dxgi.dll
GPU identity          dxvk-nvapi, DXVK_NVAPI_GPU_ARCH=AD100        nvapi64.dll + version.dll (tools/)
d4r\nvngx.dll         the shim, Proton mode                       the same shim, native Windows mode
NGX core + DLSS       _nvngx.dll, nvngx_dlss.dll (CUDA path)      the same files
d4r\nvcuda.dll        Wine builtin bridge (winegcc)               the same source, a MinGW-w64 DLL
ZLUDA                 libcuda.so on ROCm                          zluda\zluda_nvcuda.dll on the HIP SDK
native kernels        d4r\kernels\<gfx target>                    the same code objects
```

### The bridge

`tools/wine_nvcuda_bridge.c` builds either as the Wine builtin or, with `D4R_NATIVE_WINDOWS`
(`scripts/build_windows_nvcuda.sh`), as a native nvcuda.dll. `tools/d4r_bridge_platform.h` holds every
difference: the ZLUDA/HIP calling convention (System V on Linux, Windows x64 here), `dlopen` on `LoadLibrary`,
the environment hand-over (`d4rSetEnv` updates both the C runtime's and the process environment, which ZLUDA's
Rust `std::env` reads), hard links or copies for the per-process native kernel folder, process liveness,
readable memory spans and cache folders.

On Windows the bridge:

- loads ZLUDA from `d4r\zluda\zluda_nvcuda.dll` (`D4R_ZLUDA_LIBCUDA`). ZLUDA's DLL is renamed so that NGX's
  `LoadLibrary("nvcuda.dll")` keeps finding the bridge: the test checks this with the real Windows loader;
- loads HIP and comgr first from `D4R_ROCM_DIR` (d4r.ini `RocmDir`) or the HIP SDK's `HIP_PATH`, so ZLUDA's
  delay-loaded `amdhip64_7.dll` (else `_6`) resolves to that copy;
- shows NGX one CUDA device, the GPU DLSS should run on. HIP on Windows lists every AMD GPU, and with a Ryzen
  7000/9000's integrated GPU enabled that one can come first, where ZLUDA's device 0 would run DLSS. The bridge
  reads HIP's device properties (`hipGetDevicePropertiesR0600`, layout in `tools/d4r_hip_props.h`, offsets
  checked against ZLUDA's bindings) and picks `D4R_HIP_DEVICE` if set, else the GPU whose LUID is the game's
  D3D12 adapter's (the shim names it before NGX starts), else the one with the most compute units;
  `cuDeviceGetCount` then returns 1 and `cuDeviceGet(0)` that GPU. Its gfx target selects the native kernel set
  (`D4R_GPU_ARCH` overrides);
- verifies native kernels exactly as on Linux (`tools/d4r_fatbin.h`): a kernel is served only after NGX loads a
  module whose PTX hash the folder's manifest lists.

`d4rImportVulkanMemory` (winevulkan handles) returns `CUDA_ERROR_NOT_SUPPORTED`. Instead, the bridge maps
D3D12 memory through NT handles: `d4rImportWin32Memory` (`hipImportExternalMemory` as a D3D12 resource, a D3D12
heap or an opaque Win32 handle), and `d4rImportWin32Semaphore`, `d4rWaitSemaphore` and `d4rSignalSemaphore`
for a shared D3D12 fence (`hipImportExternalSemaphore`, waits and signals on the null stream, where NGX runs).
The tests check the descriptors HIP receives at the byte offsets of HIP's headers.

### The shim

The shim detects Wine by its ntdll exports (`D4R_PLATFORM=windows` or `wine` overrides). In native mode the
portable install hands the bridge Windows paths: ZLUDA, the native kernel folder, a HIP installation, and a
native kernel cache in `%LOCALAPPDATA%\d4r` (ZLUDA's own compiled-kernel cache is `%LOCALAPPDATA%\zluda`).
Before NGX's core it loads `nvapi64.dll` from the game folder unless one is loaded already, and logs which one
NGX gets.

### Frames

Under Proton the d4r vkd3d-proton patch splits the game's command list at the DLSS call, so every frame shows
its own result, and Vulkan interop keeps inputs and output in VRAM. AMD's D3D12 driver cannot split a command
list. By default (`FrameAge = 1`) each frame on Windows shows the newest finished result, one frame old: the
shim copies the inputs on the game's command list, writes a frame marker (`WriteBufferImmediate`), runs DLSS
when the marker arrives, and copies the newest finished result into the output.

### Same-frame results (`FrameAge = 0`, experimental)

Instead of splitting the list, the list waits on the GPU (`tools/d4r_d3d12_inline.h`). After the input copies
and a marker that DLSS's HIP stream waits for (`d4rStreamWaitValue32`, as Proton's `GpuWait`), the shim records
two compute dispatches on the game's command list:

1. `wait_main`, one thread, spins with atomic loads until a "released" value in a shared status buffer reaches
   the frame, or until a spin limit (`D4R_SHIM_INLINE_SPINS`, 2000000). It then picks the output slot whose
   result is the newest not newer than the frame: normally the frame's own;
2. `copy_main` copies that slot into a present buffer, which a `CopyTextureRegion` puts into the game's output.

On the HIP side, the worker queues behind DLSS's output copy two null-stream writes (`d4rStreamWriteValue32`):
the frame number of the slot, then the released frame. HIP's queues run alongside the graphics queue, so DLSS
progresses while the graphics queue spins. A frame without a result (dropped) is released by the CPU, and the
split-frame watchdog releases a frame whose wait lasted over 200 ms; either way the wait then shows the newest
older result. The shaders are HLSL compiled at runtime by Windows' `d3dcompiler_47.dll`, with the root signature
in the HLSL; recording them changes the list's compute root signature and pipeline state, as a DLSS evaluation
may. The Windows CI runs them on WARP (`tests/windows/inline_test.cpp`), and `d4r-interop-probe.exe` checks the
whole chain (D3D12 marker → HIP wait → HIP release → D3D12 wait and copy) on the user's GPU and measures a spin.
Without VRAM interop, or if the shaders or the bridge's export are missing, the shim logs why and stays at
`FrameAge = 1` behaviour.

### VRAM interop

Where the inputs and the result go between the game and DLSS depends on the driver:

- **D3D12 shared buffers** (`VramInterop = true`, the default). The shim creates committed buffers in VRAM
  with `D3D12_HEAP_FLAG_SHARED`, the bridge maps them into CUDA (`d4rImportWin32Memory`), and the copies are
  `CopyTextureRegion` calls on the game's command list, into rows aligned to 256 bytes as D3D12's buffer
  footprints require. It is the same pipeline as the Proton VRAM path, with D3D12 copies instead of Vulkan
  ones. Resources in DLSS's own formats (RGBA16F colour and output, 32-bit depth, RG16F motion vectors, R32F
  exposure) are copied as they are. D3D12 has no blit, so the others are converted by compute shaders on the
  game's command list (`tools/d4r_d3d12_convert.h`), the way the host path converts them: colour in RGBA8 or
  BGRA8 (typeless), RGB10A2, R11G11B10 or RGBA32F; D24 and D16 depth; RG32F or RGBA16F motion; R16F, RGBA16F
  or RGBA32F exposure; and outputs in RGBA8, BGRA8, RGB10A2, R11G11B10 or RGBA32F (the output must allow
  unordered access, which DLSS requires anyway). The shaders read and store through typed views in a descriptor
  heap of the shim's, which they bind on the game's command list as a DLSS evaluation may; Windows' CI runs
  every conversion on WARP against the host path's formulas (`tests/windows/convert_test.cpp`). Fully typed
  sRGB colour cannot be viewed as UNORM and stays on the host path. At startup the shim maps one small buffer;
  if HIP refuses it, it logs why and stays on host memory.
- **Host memory** otherwise: readback buffers in, an upload buffer out; about 55 MB per frame cross PCIe at
  1440p Quality.

`d4r\test-dlss.ps1` first runs `d4r\tools\d4r-interop-probe.exe` (`tools/d3d12_native_interop_probe.cpp`),
which answers on the user's PC what only the driver can: whether a shared D3D12 buffer maps into HIP (as a
resource, an opaque handle or a heap), whether data crosses in both directions, whether a shared D3D12 fence
orders D3D12 and HIP work on the GPU (and whether HIP's stream waits for a value D3D12's `WriteBufferImmediate`
writes), and what the copies cost against today's readback.

### NVAPI

NGX picks the network weights by GPU architecture, which it reads through NVAPI; OptiScaler offers its DLSS
backend only when NVAPI says NVIDIA. `tools/d4r_nvapi_windows.c` answers the identity queries (one GPU, AD100,
driver 596.36 like the tested NGX core, the main DXGI adapter's LUID), NGX's own queries (no DLSS override,
driver feature support) and the common driver, CUDA topology and memory queries the way dxvk-nvapi does under
Proton, where NGX's CUDA path is known to run, and returns NULL for the rest, as for a
feature the GPU lacks, unless `D4R_NVAPI_CHAIN` names another nvapi64.dll (fakenvapi) to forward them to. It
logs every interface it is asked for, by name, to `d4r_nvapi.log`.

OptiScaler 0.9.4 decides whether the GPU is NVIDIA's while it starts, in its `DllMain`: it asks an
`nvapi64.dll` that is already loaded, else System32's, and turns its DLSS backend off when neither answers as
NVIDIA's NVAPI (`isNvidia()` in its dllmain.cpp). d4r's nvapi64.dll in the game folder is loaded only later, so
the package also has a `version.dll` (`tools/d4r_preload_version.c`). OptiScaler.dll imports version.dll, which
Windows takes from the game folder, and Windows initialises a DLL's imports before the DLL: this version.dll loads
d4r's nvapi64.dll first and forwards its 17 functions to System32's through one jump each. The test runs a
stand-in OptiScaler that imports version.dll and records, in its own `DllMain`, that nvapi64.dll is already there.
The NVAPI never forwards fakenvapi's private interfaces to a chained fakenvapi, because OptiScaler takes an NVAPI
that answers them for fakenvapi, which means no NVIDIA GPU.

### Native kernel manifests

A kernel folder serves only with its `d4r-kernels.txt`, which lists hashes of the PTX in NVIDIA's
`nvngx_dlss.dll`. The Linux packager writes it with `kernels/tools/kernel_manifest.py`; Windows installs use
`d4r-manifest.exe` (`tools/d4r_manifest.c`, identical output, tested against the Python tool), which
`d4r\setup.ps1` runs on the user's DLL. It accepts only DLSS 310.7 and 310.9, the versions the kernels were
written for: a manifest from another version would bless kernels that changed.

## Building

Everything builds in GitHub Actions:

| Workflow | What |
|---|---|
| `ci.yml` | tests, the shim, both bridges; the Windows tests under Wine and on a Windows runner |
| `windows.yml` | ZLUDA's helper bitcode and the native network kernels (ROCm 7.2.4, Linux), ZLUDA for Windows (`windows-2022`, about an hour for its LLVM) |
| `windows-package.yml` | `scripts/package_windows.sh` with the last `windows.yml` artifacts, and `setup.ps1` on Windows |

Locally, the d4r DLLs need MinGW-w64 and clang-cl (`scripts/package_windows.sh` builds them all). ZLUDA for
Windows needs Visual Studio's C++ tools, CMake, Ninja, Python and Rust on Windows:

```sh
ZLUDA_SUBMODULES=1 scripts/fetch_zluda_source.sh zluda   # ee2f25a + patches/zluda, LLVM, OCKL from Git LFS
# copy the helper bitcode (scripts/build_zluda_ptx_helpers.sh output, built with ROCm on Linux) into zluda/ptx/lib
cd zluda && cargo build --release -p zluda               # target/release/nvcuda.dll -> d4r\zluda\zluda_nvcuda.dll
```

The texture kernels are built from NVIDIA's PTX (`kernels/build.sh tex`) and are not in the package; DLSS runs
those parts through ZLUDA instead, as on Linux without them.

## Testing on hardware

1. `d4r\setup.ps1` until it reports everything in place.
2. `d4r\test-dlss.ps1`: the VRAM sharing check (`d4r\interop-report.txt`), then DLSS on synthetic frames
   through the D3D12 harness, without a game or OptiScaler. It writes `d4r\test-output.raw.bmp`.
3. A game from [SUPPORTED_GAMES.md](../SUPPORTED_GAMES.md).

Logs to look at: `d4r\d4r_nvngx.log` (shim and bridge), `d4r_nvapi.log` (NVAPI calls, including unimplemented
ones), `d4r\interop-report.txt`, `OptiScaler.log`.

## Open questions

- **NGX core outside NVIDIA's driver.** NGX's CUDA path ran under Proton with dxvk-nvapi. Whether it needs more
  NVAPI, registry keys or driver checks on Windows shows in `d4r_nvngx.log` (NGX's own log lines) and
  `d4r_nvapi.log`.
- **OptiScaler's NVIDIA check** on Windows with d4r's NVAPI.
- **HIP on Windows** for what ZLUDA uses for DLSS: texture and surface objects, the d4r patches' launches, and
  loading the native code objects (built with ROCm on Linux) through the Windows HIP runtime.
- **Same-frame results on AMD's driver.** The GPU-side wait needs HIP's queues to run while the graphics
  queue spins, and HIP's null-stream writes to be visible to the shader's atomics; the probe's step 5 checks
  both. Games that do not restore their compute root signature after DLSS would be affected by the wait's
  dispatches (as by DLSS itself on NVIDIA hardware).
- **VRAM interop on AMD's driver.** The shared-buffer path and the probe are written against the HIP and D3D12
  documentation and tested with mocks; the probe's report on real hardware says whether the import works, and
  a shared fence or the marker wait would let DLSS start on the GPU without the CPU polling the frame marker.
  The format conversions bind a descriptor heap on the game's command list; a game that does not restore its
  heaps after DLSS would be affected (as by DLSS itself on NVIDIA hardware).
