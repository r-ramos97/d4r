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
    run(flags + ["-shared", windows / "mock_ngx_core.c", "-o", out / "_nvngx.dll"])
    run(flags + [windows / "bridge_test.c", "-o", out / "bridge_test.exe"])
    run(flags + [windows / "shim_test.c", "-o", out / "shim_test.exe"])
    if CLANG_CL:
        env = dict(os.environ, CLANG_CL=CLANG_CL, MINGW_CXX=os.environ.get("MINGW_CXX", "x86_64-w64-mingw32-g++"))
        run(["bash", ROOT / "scripts" / "build_d4r_nvngx_shim.sh"], env=env)
        shutil.copy(ROOT / "build" / "d4r_nvngx.dll", out / "d4r_nvngx.dll")


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

        # the release layout: a widened and an FP8 set for the RX 9070 XT, nothing for the iGPU
        cls.ptx = cls.work / "module.ptx"
        cls.ptx.write_bytes(PTX.encode())
        hash_ = fnv1a64(PTX.encode())
        for folder in ("gfx1201", "gfx1201-fp8"):
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
        env = {k: v for k, v in os.environ.items() if not k.startswith(("D4R_", "HIP_PATH"))}
        env["HIP_PATH"] = windows_path(self.work / "hip")
        env.update(extra_env or {})
        command = [str(arg) for arg in args]
        if not ON_WINDOWS:
            env.setdefault("WINEPREFIX", str(Path.home() / ".cache" / "d4r-test-wineprefix"))
            env["WINEDEBUG"] = "-all"
            env["WINEDLLOVERRIDES"] = "mscoree,mshtml="
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

    def test_shim_initialises_ngx_through_the_bridge(self):
        """A portable install on native Windows: OptiScaler loads d4r\\nvngx.dll, which reads d4r.ini and
        initialises the (mock) NGX core, whose CUDA calls reach the (mock) ZLUDA through the bridge."""
        if not (self.binaries / "d4r_nvngx.dll").exists():
            self.skipTest("the shim build needs clang-cl")
        shutil.copy(self.binaries / "d4r_nvngx.dll", self.d4r / "nvngx.dll")
        shutil.copy(ROOT / "packaging" / "d4r.ini", self.d4r / "d4r.ini")
        (self.d4r / "nvngx_dlss.dll").write_bytes(b"placeholder")
        (self.d4r / "ngx").mkdir(exist_ok=True)
        shutil.copy(self.binaries / "_nvngx.dll", self.d4r / "ngx" / "_nvngx.dll")
        exe = self.d4r.parent / "shim_test.exe"  # in the game's folder, which holds d4r\\
        shutil.copy(self.binaries / "shim_test.exe", exe)
        result = self.run_program(exe, windows_path(self.d4r),
                                  extra_env={"D4R_PLATFORM": "windows", "D4R_TEST_MODULE": windows_path(self.ptx)})
        log = (self.d4r / "d4r_nvngx.log").read_text(errors="replace") if (self.d4r / "d4r_nvngx.log").exists() else ""
        self.assert_all_pass(result)
        self.assertIn("native Windows", log)
        self.assertIn("D4R_ZLUDA_LIBCUDA=", log)
        self.assertIn("zluda_nvcuda.dll", log)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--build":
        build_binaries(Path(sys.argv[2]).resolve())
    else:
        unittest.main()
