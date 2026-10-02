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
GPU identity          dxvk-nvapi, DXVK_NVAPI_GPU_ARCH=AD100        nvapi64.dll (tools/d4r_nvapi_windows.c)
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
- picks the GPU target from HIP's device properties (`hipGetDevicePropertiesR0600`, layout in
  `tools/d4r_hip_props.h`, offsets checked against ZLUDA's bindings): the device with the most compute units,
  so a Ryzen iGPU never wins over the discrete GPU. `D4R_GPU_ARCH` overrides it;
- verifies native kernels exactly as on Linux (`tools/d4r_fatbin.h`): a kernel is served only after NGX loads a
  module whose PTX hash the folder's manifest lists.

`d4rImportVulkanMemory` (winevulkan handles) returns `CUDA_ERROR_NOT_SUPPORTED`; the shim then uses host memory.

### The shim

The shim detects Wine by its ntdll exports (`D4R_PLATFORM=windows` or `wine` overrides). In native mode the
portable install hands the bridge Windows paths: ZLUDA, the native kernel folder, a HIP installation, and a
native kernel cache in `%LOCALAPPDATA%\d4r` (ZLUDA's own compiled-kernel cache is `%LOCALAPPDATA%\zluda`).
Before NGX's core it loads `nvapi64.dll` from the game folder unless one is loaded already, and logs which one
NGX gets.

### Frames

Under Proton the d4r vkd3d-proton patch splits the game's command list at the DLSS call, so every frame shows
its own result, and Vulkan interop keeps inputs and output in VRAM. Neither exists with AMD's D3D12 driver. The
shim's original path runs instead: it copies the inputs to readback buffers on the game's command list, writes a
frame marker (`WriteBufferImmediate`), runs DLSS when the marker arrives, and copies the newest finished result
into the output from an upload buffer. Each frame therefore shows a result one frame or more old, and inputs and
output cross PCIe (about 55 MB per frame at 1440p Quality). The Windows `d4r.ini` sets `FrameAge = 1`, one
frame in flight, the lowest latency on this path.

### NVAPI

NGX picks the network weights by GPU architecture, which it reads through NVAPI; OptiScaler offers its DLSS
backend only when NVAPI says NVIDIA. `tools/d4r_nvapi_windows.c` answers the identity queries (one GPU, AD100,
driver 596.36 like the tested NGX core, the main DXGI adapter's LUID) and returns NULL for the rest, as for a
feature the GPU lacks, unless `D4R_NVAPI_CHAIN` names another nvapi64.dll (fakenvapi) to forward them to. It
logs every interface it is asked for, by name, to `d4r_nvapi.log`.

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
2. `d4r\test-dlss.ps1`: DLSS on synthetic frames through the D3D12 harness, without a game or OptiScaler. It
   writes `d4r\test-output.raw.bmp`.
3. A game from [SUPPORTED_GAMES.md](../SUPPORTED_GAMES.md).

Logs to look at: `d4r\d4r_nvngx.log` (shim and bridge), `d4r_nvapi.log` (NVAPI calls, including unimplemented
ones), `OptiScaler.log`.

## Open questions

- **NGX core outside NVIDIA's driver.** NGX's CUDA path ran under Proton with dxvk-nvapi. Whether it needs more
  NVAPI, registry keys or driver checks on Windows shows in `d4r_nvngx.log` (NGX's own log lines) and
  `d4r_nvapi.log`.
- **OptiScaler's NVIDIA check** on Windows with d4r's NVAPI.
- **HIP on Windows** for what ZLUDA uses for DLSS: texture and surface objects, the d4r patches' launches, and
  loading the native code objects (built with ROCm on Linux) through the Windows HIP runtime.
- **Same-frame results.** Without vkd3d-proton's split, a D3D12-level equivalent would need the game's queue: a
  hook on `ExecuteCommandLists` cannot split a list the game is still recording into, so this stays a frame late
  unless the game's own frame structure allows otherwise.
- **VRAM interop.** D3D12 resources shared with HIP (`hipImportExternalMemory` with D3D12 handles) and a
  GPU-side wait on the frame marker would remove the PCIe copies; whether AMD's Windows HIP supports them needs
  hardware.
