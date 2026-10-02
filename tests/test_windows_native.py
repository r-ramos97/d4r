"""The native Windows nvcuda bridge (scripts/build_windows_nvcuda.sh) against a mock ZLUDA and HIP SDK.

tests/windows/bridge_test.c loads the bridge the way the NGX shim and NVIDIA's NGX core do. The test runs
it on Windows directly, elsewhere under Wine; it is skipped without MinGW-w64 (and Wine off Windows).

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
ON_WINDOWS = sys.platform == "win32"
WINE = None if ON_WINDOWS else shutil.which("wine") or shutil.which("wine64")

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


@unittest.skipUnless(shutil.which(CC) and (ON_WINDOWS or WINE), "needs MinGW-w64 and Wine (or Windows)")
class WindowsNativeBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        build = cls.work / "build"
        build.mkdir()
        windows = ROOT / "tests" / "windows"
        subprocess.run(["bash", str(ROOT / "scripts" / "build_windows_nvcuda.sh")], check=True,
                       capture_output=True, text=True)

        def compile_dll(source, output):
            subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-I", str(ROOT / "tools"),
                            str(windows / source), "-o", str(output)], check=True, capture_output=True, text=True)

        cls.d4r = cls.work / "game" / "d4r"
        (cls.d4r / "zluda").mkdir(parents=True)
        shutil.copy(ROOT / "build" / "windows" / "nvcuda.dll", cls.d4r / "nvcuda.dll")
        compile_dll("mock_zluda.c", cls.d4r / "zluda" / "zluda_nvcuda.dll")
        (cls.work / "hip" / "bin").mkdir(parents=True)
        compile_dll("mock_hip.c", cls.work / "hip" / "bin" / "amdhip64_7.dll")
        cls.exe = build / "bridge_test.exe"
        subprocess.run([CC, "-O2", "-Wall", "-Wextra", "-Werror", str(windows / "bridge_test.c"), "-o",
                        str(cls.exe)], check=True, capture_output=True, text=True)

        # the release layout: a widened and an FP8 set for the RX 9070 XT, nothing for the iGPU
        cls.ptx = cls.work / "module.ptx"
        cls.ptx.write_text(PTX)
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

    def run_test_program(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("D4R_", "HIP_PATH"))}
        env["HIP_PATH"] = windows_path(self.work / "hip")
        command = [str(self.exe), windows_path(self.d4r), windows_path(self.cache), windows_path(self.ptx)]
        if not ON_WINDOWS:
            env.setdefault("WINEPREFIX", str(Path.home() / ".cache" / "d4r-test-wineprefix"))
            env["WINEDEBUG"] = "-all"
            env["WINEDLLOVERRIDES"] = "mscoree,mshtml="
            command.insert(0, WINE)
        return subprocess.run(command, env=env, text=True, capture_output=True, timeout=600)

    def test_bridge_against_mock_zluda_and_hip(self):
        result = self.run_test_program()
        lines = [line for line in result.stdout.splitlines() if line.startswith(("PASS", "FAIL"))]
        self.assertTrue(lines, result.stdout + result.stderr)
        failed = [line for line in lines if line.startswith("FAIL")]
        self.assertEqual(failed, [], result.stdout + result.stderr[-4000:])
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
