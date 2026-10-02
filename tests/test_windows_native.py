"""The native Windows nvcuda bridge and the shim's Windows mode against a mock ZLUDA, HIP SDK and NGX core.

tests/windows/bridge_test.c loads the bridge (scripts/build_windows_nvcuda.sh) the way the NGX shim and
NVIDIA's NGX core do; tests/windows/shim_test.c initialises NGX through the shim of a portable install. The
programs run on Windows directly and under Wine elsewhere. They are built with MinGW-w64 (the shim also needs
clang-cl), or taken from D4R_TEST_PREBUILT, a folder written by `python3 tests/test_windows_native.py --build
DIR` (CI builds them on Linux and runs them on a Windows runner).

Run: python3 -m unittest discover -s tests -v
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = os.environ.get("MINGW_CC", "x86_64-w64-mingw32-gcc")
CLANG_CL = os.environ.get("CLANG_CL") or shutil.which("clang-cl") or next(
    (str(path) for path in sorted(Path("/usr/lib").glob("llvm-*/bin/clang-cl"), reverse=True)), None)
ON_WINDOWS = sys.platform == "win32"
WINE = None if ON_WINDOWS else shutil.which("wine") or shutil.which("wine64")
PREBUILT = os.environ.get("D4R_TEST_PREBUILT")

PTX = """//
.version 8.5
.target sm_89
.address_size 64

.visible .entry dltss_pwin_enc0_layer(
    .param .u64 dltss_pwin_enc0_layer_param_0
)
{
    ret;
}

.visible .entry dltss_pwin_dec0_layer(
    .param .u64 dltss_pwin_dec0_layer_param_0
)
{
    ret;
}
"""


def fnv1a64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def windows_path(path):
    return str(path) if ON_WINDOWS else "Z:" + str(path).replace("/", "\\")


def build_binaries(out):
    """The bridge, the shim (when clang-cl is available), the mocks and the test programs, into out."""
    out.mkdir(parents=True, exist_ok=True)
    windows = ROOT / "tests" / "windows"

    def run(command, **kwargs):
        subprocess.run([str(part) for part in command], check=True, capture_output=True, text=True, **kwargs)

    run(["bash", ROOT / "scripts" / "build_windows_nvcuda.sh"])
    shutil.copy(ROOT / "build" / "windows" / "nvcuda.dll", out / "nvcuda.dll")
    flags = [CC, "-O2", "-Wall", "-Wextra", "-Werror", "-I", ROOT / "tools"]
    run(flags + ["-shared", windows / "mock_zluda.c", "-o", out / "zluda_nvcuda.dll"])
    run(flags + ["-shared", windows / "mock_hip.c", "-o", out / "amdhip64_7.dll"])
    if not CLANG_CL:
        run(flags + ["-shared", windows / "mock_ngx_core.c", "-o", out / "_nvngx.dll"])
    run(flags + [windows / "bridge_test.c", "-o", out / "bridge_test.exe"])
    run(flags + [windows / "shim_test.c", "-o", out / "shim_test.exe"])
    run(["bash", ROOT / "scripts" / "build_windows_tools.sh"])
    for name in ("nvapi64.dll", "version.dll", "d4r-manifest.exe"):
        shutil.copy(ROOT / "build" / "windows" / name, out / name)
    run(flags + [windows / "nvapi_test.c", "-o", out / "nvapi_test.exe"])
    run(flags + ["-shared", windows / "fake_optiscaler.c", "-lversion", "-o", out / "fake_optiscaler.dll"])
    run(flags + [windows / "preload_test.c", "-o", out / "preload_test.exe"])
    run(["bash", ROOT / "scripts" / "build_d3d12_native_interop_probe.sh"])
    run([os.environ.get("MINGW_CXX", "x86_64-w64-mingw32-g++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
         "-Wno-missing-field-initializers", "-I", ROOT / "tools", "-static", "-static-libgcc", "-static-libstdc++",
         windows / "inline_test.cpp", "-ld3d12", "-ldxgi", "-o", out / "inline_test.exe"])
    run([os.environ.get("MINGW_CXX", "x86_64-w64-mingw32-g++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
         "-Wno-missing-field-initializers", "-I", ROOT / "tools", "-static", "-static-libgcc", "-static-libstdc++",
         windows / "convert_test.cpp", "-ld3d12", "-ldxgi", "-o", out / "convert_test.exe"])
    shutil.copy(ROOT / "build" / "windows" / "d4r-interop-probe.exe", out / "d4r-interop-probe.exe")
    if CLANG_CL:
        cxx = os.environ.get("MINGW_CXX", "x86_64-w64-mingw32-g++")
        env = dict(os.environ, CLANG_CL=CLANG_CL, MINGW_CXX=cxx)
        # the shim, and the D3D12 harness that drives it as a game does
        run(["bash", ROOT / "scripts" / "build_d3d12_dlss_harness.sh"], env=env)
        shutil.copy(ROOT / "build" / "d4r_nvngx.dll", out / "d4r_nvngx.dll")
        shutil.copy(ROOT / "build" / "d3d12_dlss_harness.exe", out / "d4r-harness.exe")
        # the mock NGX core with real parameter objects (the shim's MSVC-ABI implementation)
        run(flags + ["-DMOCK_NGX_PARAMETERS", "-c", windows / "mock_ngx_core.c", "-o", out / "mock_ngx_core.o"])
        run([cxx, "-shared", out / "mock_ngx_core.o", ROOT / "tools" / "d4r_ngx_param_host.cpp",
             ROOT / "build" / "d4r_ngx_param_msvc.obj", "-static", "-static-libgcc", "-static-libstdc++",
             "-o", out / "_nvngx.dll"])
        (out / "mock_ngx_core.o").unlink()


@unittest.skipUnless((PREBUILT or shutil.which(CC)) and (ON_WINDOWS or WINE),
                     "needs MinGW-w64 or D4R_TEST_PREBUILT, and Windows or Wine")
class WindowsNativeBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        cls.binaries = Path(PREBUILT) if PREBUILT else cls.work / "build"
        if not PREBUILT:
            build_binaries(cls.binaries)

        cls.d4r = cls.work / "game" / "d4r"
        (cls.d4r / "zluda").mkdir(parents=True)
        shutil.copy(cls.binaries / "nvcuda.dll", cls.d4r / "nvcuda.dll")
        shutil.copy(cls.binaries / "zluda_nvcuda.dll", cls.d4r / "zluda" / "zluda_nvcuda.dll")
        (cls.work / "hip" / "bin").mkdir(parents=True)
        shutil.copy(cls.binaries / "amdhip64_7.dll", cls.work / "hip" / "bin" / "amdhip64_7.dll")
        cls.exe = cls.binaries / "bridge_test.exe"

        # the release layout: a widened and an FP8 set for the RX 9070 XT, a set for the RX 7900 XTX, nothing for
        # the iGPU (mock_hip lists all three)
        cls.ptx = cls.work / "module.ptx"
        cls.ptx.write_bytes(PTX.encode())
        hash_ = fnv1a64(PTX.encode())
        for folder in ("gfx1201", "gfx1201-fp8", "gfx1100"):
            kernels = cls.d4r / "kernels" / folder
            kernels.mkdir(parents=True)
            (kernels / "d4r-kernels.txt").write_text(
                f"dltss_pwin_enc0_layer {hash_:016x}\ndltss_pwin_dec0_layer {hash_ ^ 1:016x}\n")
            for name in ("dltss_pwin_enc0_layer", "dltss_pwin_dec0_layer"):
                (kernels / f"{name}.hsaco").write_text(f"{folder} {name.split('_')[2]}")
        cls.cache = cls.work / "cache"
        cls.cache.mkdir()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_program(self, *args, extra_env=None):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("D4R_", "HIP_PATH"))}
        env["HIP_PATH"] = windows_path(self.work / "hip")
        env.update(extra_env or {})
        command = [str(arg) for arg in args]
        if not ON_WINDOWS:
            env.setdefault("WINEPREFIX", str(Path.home() / ".cache" / "d4r-test-wineprefix"))
            Path(env["WINEPREFIX"]).parent.mkdir(parents=True, exist_ok=True)  # Wine creates only the prefix
            env["WINEDEBUG"] = "-all"
            env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
            command.insert(0, WINE)
        return subprocess.run(command, env=env, text=True, capture_output=True, timeout=600)

    def run_test_program(self):
        return self.run_program(self.exe, windows_path(self.d4r), windows_path(self.cache), windows_path(self.ptx))

    def assert_all_pass(self, result):
        lines = [line for line in result.stdout.splitlines() if line.startswith(("PASS", "FAIL"))]
        self.assertTrue(lines, result.stdout + result.stderr)
        failed = [line for line in lines if line.startswith("FAIL")]
        self.assertEqual(failed, [], result.stdout + result.stderr[-4000:])
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_bridge_against_mock_zluda_and_hip(self):
        self.assert_all_pass(self.run_test_program())

    def test_nvapi_identity(self):
        """d4r's Windows nvapi64.dll: an Ada GPU for OptiScaler and NGX, and a log of what was asked for."""
        folder = self.work / "nvapi"
        folder.mkdir(exist_ok=True)
        shutil.copy(self.binaries / "nvapi64.dll", folder / "nvapi64.dll")
        self.assert_all_pass(self.run_program(self.binaries / "nvapi_test.exe", windows_path(folder / "nvapi64.dll"),
                                              extra_env={"D4R_NVAPI_LUID": "1:abcd"}))

    def test_nvapi_loaded_before_optiscaler(self):
        """OptiScaler enables DLSS only if NVIDIA's NVAPI is there when it starts: d4r's version.dll, which
        OptiScaler imports from the game folder, must load d4r's nvapi64.dll before OptiScaler's DllMain."""
        game = self.work / "preload-game"
        game.mkdir(exist_ok=True)
        for name in ("version.dll", "nvapi64.dll", "preload_test.exe"):
            shutil.copy(self.binaries / name, game / name)
        shutil.copy(self.binaries / "fake_optiscaler.dll", game / "dxgi.dll")
        self.assert_all_pass(self.run_program(game / "preload_test.exe", windows_path(game),
                                              extra_env={"WINEDLLOVERRIDES": "mscoree,mshtml=;dxgi,version=n,b"}))

    def test_interop_probe_runs_to_its_report(self):
        """d4r-interop-probe.exe on Windows' software D3D12 renderer against the mock HIP and ZLUDA, whose
        "device" memory is host memory: nothing is really shared, so it must report that the VRAM path fails,
        after running every D3D12 step (including a GPU wait nobody satisfies) without hanging or crashing."""
        result = self.run_program(self.binaries / "d4r-interop-probe.exe", windows_path(self.d4r / "nvcuda.dll"),
                                  "64", "32", extra_env={"D4R_PROBE_ADAPTER": "warp"})
        if not ON_WINDOWS and result.returncode == 2:
            self.skipTest("this Wine has no D3D12 (vkd3d needs Vulkan): " + result.stdout.strip().splitlines()[-1])
        output = result.stdout
        self.assertEqual(result.returncode, 1, output + result.stderr[-3000:])
        self.assertIn("as a D3D12 resource (hipExternalMemoryHandleTypeD3D12Resource): imported", output)
        self.assertIn("D3D12 texture -> shared buffer -> CUDA: FAIL", output)
        self.assertIn("D3D12 fence as a HIP external semaphore: imported", output)
        self.assertIn("round trip D3D12 -> HIP -> D3D12: FAIL (no result within 10 s", output)
        self.assertIn("RESULT: VRAM sharing FAILS (D3D12 resource), GPU sync: shared fence no, marker no, "
                      "same-frame wait no", output)
        self.assertIn("Same-frame results do not work here", output)

    def test_missing_hip_fails_cleanly(self):
        """Without AMD's HIP runtime, ZLUDA's first HIP call would raise an exception that ends the process (a
        game): the bridge fails cuInit instead, saying why, and d4r-interop-probe.exe ends on its RESULT line."""
        folder = self.work / "no-hip"  # not beside the mock amdhip64_7.dll, which the search path would find
        (folder / "hip" / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy(self.binaries / "d4r-interop-probe.exe", folder / "d4r-interop-probe.exe")
        result = self.run_program(folder / "d4r-interop-probe.exe", windows_path(self.d4r / "nvcuda.dll"), "64", "32",
                                  extra_env={"D4R_PROBE_ADAPTER": "warp", "HIP_PATH": windows_path(folder / "hip")})
        output = result.stdout
        if not ON_WINDOWS and "cuInit" not in output:
            self.skipTest("this Wine has no D3D12 (vkd3d needs Vulkan): " + output.strip().splitlines()[-1])
        self.assertEqual(result.returncode, 2, output + result.stderr[-3000:])
        self.assertIn("cuInit through the bridge failed", output)
        self.assertIn("AMD's HIP runtime (amdhip64_7.dll or amdhip64_6.dll) is not installed", output)
        self.assertIn("RESULT: the check stopped early", output)

    def test_same_frame_wait_shaders(self):
        """tools/d4r_d3d12_inline.h's wait and copy shaders, compiled by Windows' d3dcompiler_47 and run on WARP:
        which output slot each case shows, the spin limit, and that the chosen slot's bytes are copied."""
        result = self.run_program(self.binaries / "inline_test.exe")
        if result.returncode == 3 and not ON_WINDOWS:
            self.skipTest("this Wine has no D3D12 (vkd3d needs Vulkan)")
        self.assert_all_pass(result)

    def test_format_conversion_shaders(self):
        """tools/d4r_d3d12_convert.h on WARP: each game format the VRAM path converts, into DLSS's canonical
        layout and back out, against the host path's conversions."""
        result = self.run_program(self.binaries / "convert_test.exe")
        if result.returncode == 3 and not ON_WINDOWS:
            self.skipTest("this Wine has no D3D12 (vkd3d needs Vulkan)")
        self.assert_all_pass(result)

    @unittest.skipUnless(ON_WINDOWS, "needs Windows' D3D12 (Wine's vkd3d lacks WriteBufferImmediate and cs_5_1)")
    def test_harness_end_to_end_on_warp(self):
        """The D3D12 harness drives the shim as a game does, on WARP (with the D3D12 debug layer when Windows has
        it), through the bridge to the mock NGX core, ZLUDA and HIP: the host path (whose output the mock NGX's
        nearest-neighbour upscale reaches), the startup round trip that keeps the shim off a VRAM path whose bytes
        do not cross, the D3D12 VRAM path, its format conversion shaders and the same-frame wait. Every D3D12 call
        the shim records runs on Microsoft's runtime."""
        if not (self.binaries / "d4r-harness.exe").exists():
            self.skipTest("the shim build needs clang-cl")
        game = self.work / "harness-game"
        if game.exists():
            shutil.rmtree(game)
        d4r = game / "d4r"
        (d4r / "zluda").mkdir(parents=True)
        (d4r / "ngx").mkdir()
        shutil.copy(self.binaries / "d4r_nvngx.dll", d4r / "nvngx.dll")
        shutil.copy(self.binaries / "nvcuda.dll", d4r / "nvcuda.dll")
        shutil.copy(self.binaries / "zluda_nvcuda.dll", d4r / "zluda" / "zluda_nvcuda.dll")
        shutil.copy(self.binaries / "_nvngx.dll", d4r / "ngx" / "_nvngx.dll")
        shutil.copy(ROOT / "packaging" / "windows" / "d4r.ini", d4r / "d4r.ini")
        (d4r / "nvngx_dlss.dll").write_bytes(b"placeholder")
        shutil.copy(self.binaries / "nvapi64.dll", game / "nvapi64.dll")
        shutil.copy(self.binaries / "d4r-harness.exe", game / "d4r-harness.exe")
        base = {"D4R_PLATFORM": "windows", "D4R_HARNESS_ADAPTER": "warp", "D4R_HARNESS_D3D12_DEBUG": "1",
                "D4R_HARNESS_FRAME_WAIT_MS": "100", "D4R_SHIM_INLINE_SPINS": "20000"}
        # the mock HIP's "device" memory is host memory that D3D12 never sees: the VRAM scenarios skip the shim's
        # round-trip check (D4R_SHIM_VRAM_CHECK=0), which must otherwise catch exactly that
        vram = {"D4R_SHIM_VRAM_CHECK": "0"}
        same_frame = dict(vram, D4R_SHIM_SPLIT_FRAME="1", D4R_SHIM_MAX_IN_FLIGHT="3")
        scenarios = [
            ("host path", {"D4R_SHIM_VRAM_INTEROP": "0"}, ["VRAM interop off for this feature"], True),
            ("host path, RGBA8", {"D4R_SHIM_VRAM_INTEROP": "0", "D4R_HARNESS_RGBA8": "1"},
             ["VRAM interop off for this feature"], True),
            ("VRAM interop's round-trip check (the mocks share nothing)", {},
             ["but the bytes do not cross (HIP does not see what D3D12 wrote into the shared buffer)",
              "VRAM interop off for this feature"], True),
            ("VRAM interop", vram, ["VRAM interop: ready (native Windows", "VRAM interop on for this feature"], False),
            ("VRAM interop, RGBA8 (conversion shaders)", dict(vram, D4R_HARNESS_RGBA8="1"),
             ["converting colour, output on the GPU", "VRAM interop on for this feature"], False),
            ("same-frame results", same_frame, ["GPU-side wait in the game's command list"], False),
            ("same-frame results, RGBA8 (the present buffer through the output conversion)",
             dict(same_frame, D4R_HARNESS_RGBA8="1"),
             ["converting colour, output on the GPU", "GPU-side wait in the game's command list"], False),
        ]
        for name, extra, lines, data in scenarios:
            with self.subTest(name):
                env = dict(base, **extra)
                result = self.run_program(game / "d4r-harness.exe", windows_path(d4r / "nvngx.dll"),
                                          windows_path(game / "out.raw"), "4", "64", "32", "128", "64",
                                          extra_env=env)
                log = (d4r / "d4r_nvngx.log").read_text(errors="replace") if (d4r / "d4r_nvngx.log").exists() else ""
                context = f"{name}\n--- harness\n{result.stdout}\n{result.stderr[-3000:]}\n--- log\n{log[-6000:]}"
                self.assertEqual(result.returncode, 0, context)
                self.assertIn("CreateFeature -> 0x00000001", result.stdout, context)
                for line in lines:
                    self.assertIn(line, log, context)
                self.assertNotIn("D3D12 debug error", result.stdout, context)
                if data:
                    # the mock NGX's upscale of the synthetic scene reached the game's output texture
                    self.assertRegex(result.stdout, r"output read back: [1-9][0-9]* of", context)

    def test_shim_initialises_ngx_through_the_bridge(self):
        """A portable install on native Windows: OptiScaler loads d4r\\nvngx.dll, which reads d4r.ini and
        initialises the (mock) NGX core, whose CUDA calls reach the (mock) ZLUDA through the bridge."""
        if not (self.binaries / "d4r_nvngx.dll").exists():
            self.skipTest("the shim build needs clang-cl")
        shutil.copy(self.binaries / "d4r_nvngx.dll", self.d4r / "nvngx.dll")
        shutil.copy(ROOT / "packaging" / "windows" / "d4r.ini", self.d4r / "d4r.ini")
        (self.d4r / "nvngx_dlss.dll").write_bytes(b"placeholder")
        (self.d4r / "ngx").mkdir(exist_ok=True)
        shutil.copy(self.binaries / "_nvngx.dll", self.d4r / "ngx" / "_nvngx.dll")
        exe = self.d4r.parent / "shim_test.exe"  # in the game's folder, which holds d4r\\
        shutil.copy(self.binaries / "shim_test.exe", exe)
        shutil.copy(self.binaries / "nvapi64.dll", self.d4r.parent / "nvapi64.dll")
        result = self.run_program(exe, windows_path(self.d4r),
                                  extra_env={"D4R_PLATFORM": "windows", "D4R_TEST_MODULE": windows_path(self.ptx)})
        log = (self.d4r / "d4r_nvngx.log").read_text(errors="replace") if (self.d4r / "d4r_nvngx.log").exists() else ""
        self.assert_all_pass(result)
        self.assertIn("native Windows", log)
        self.assertIn("D4R_ZLUDA_LIBCUDA=", log)
        self.assertIn("zluda_nvcuda.dll", log)
        self.assertRegex(log, r"NVAPI for NGX: .*\\game\\d4r\\\.\.\\nvapi64\.dll|NVAPI for NGX: .*\\game\\nvapi64\.dll")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--build":
        build_binaries(Path(sys.argv[2]).resolve())
    else:
        unittest.main()
