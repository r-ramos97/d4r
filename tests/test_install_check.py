"""CPU checks for packaging/d4r-check.sh: the GPU and native kernel set it reports must be the ones the
bridge uses (the KFD GPU with the most SIMDs, tools/d4r_native_selection.h for the kernel folder).

Run: python3 -m unittest discover -s tests -v
"""
import itertools
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "packaging" / "d4r-check.sh"
IGPU = (4, 100306)       # Ryzen 7000 integrated GPU, gfx1036
RDNA3 = (108, 110001)    # RX 7700 XT, gfx1101
RDNA4 = (128, 120001)    # RX 9070 XT, gfx1201


class InstallCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.runner = Path(cls.build.name) / "selection"
        source = r'''
#include "d4r_native_selection.h"
int main(int argc, char** argv) {
    (void)d4r_apply_accuracy_policy;
    char path[1024] = {0};
    if (argc != 5 || d4r_select_native_source(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]), path, sizeof(path)) == 0)
        path[0] = '\0';
    puts(path);
    return 0;
}
'''
        subprocess.run(["cc", "-x", "c", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "tools"), "-o", str(cls.runner), "-"],
                       input=source, text=True, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.game = self.root / "Some Game" / "Binaries"  # game folders often contain spaces
        (self.game / "d4r").mkdir(parents=True)
        self.nodes = self.root / "nodes"
        self.gpus()

    def gpus(self, *gpus):
        """KFD topology: node 0 is the CPU, then one node per (simd_count, gfx_target_version)."""
        for node, (simds, target) in enumerate([(0, 0), *gpus]):
            directory = self.nodes / str(node)
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "properties").write_text(
                f"cpu_cores_count {16 if node == 0 else 0}\nsimd_count {simds}\ngfx_target_version {target}\n")

    def kernels(self, name, manifest=True, accuracy=None):
        directory = self.game / "d4r" / "kernels" / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "dltss_pwin_enc0_layer.hsaco").write_bytes(b"fixture")
        if manifest:
            (directory / "d4r-kernels.txt").write_text("dltss_pwin_enc0_layer 0123456789abcdef\n")
        if accuracy is not None:
            (directory / "d4r-accuracy.txt").write_text(accuracy)

    def ini(self, text):
        (self.game / "d4r" / "d4r.ini").write_bytes(text.encode())

    def check(self, **env):
        clean = {k: v for k, v in os.environ.items() if not k.startswith("D4R_")}
        clean.update(env, D4R_CHECK_KFD_NODES=str(self.nodes))
        result = subprocess.run(["sh", str(CHECK), str(self.game)], env=clean, text=True,
                                capture_output=True)
        self.assertEqual(result.stderr, "")
        return result.stdout

    def test_discrete_gpu_wins_over_integrated_gpu_listed_first(self):
        self.gpus(IGPU, RDNA4)
        self.kernels("gfx1201-fp8")
        output = self.check()
        self.assertIn("2 GPUs; d4r uses the one with the most SIMDs (gfx1201)", output)
        self.assertIn("ok       GPU gfx1201: native kernels in d4r/kernels/gfx1201-fp8", output)
        self.assertNotIn("gfx1036", output.replace("(gfx1201)", ""))

    def test_gpu_arch_override(self):
        self.gpus(RDNA4)
        self.kernels("gfx1101")
        output = self.check(D4R_GPU_ARCH="gfx1101")
        self.assertIn("GPU gfx1101 set by D4R_GPU_ARCH", output)
        self.assertIn("ok       GPU gfx1101: native kernels in d4r/kernels/gfx1101", output)

    def test_native_fp8_setting_and_environment_precedence(self):
        self.gpus(RDNA4)
        self.kernels("gfx1201")
        self.kernels("gfx1201-fp8")
        self.assertIn("kernels in d4r/kernels/gfx1201-fp8", self.check())
        self.ini("[Kernels]\r\nnativefp8 = Off ; 16-bit math\r\n")
        self.assertIn("kernels in d4r/kernels/gfx1201\n", self.check())
        self.assertIn("kernels in d4r/kernels/gfx1201-fp8", self.check(D4R_ZLUDA_WMMA_FP8_NATIVE="1"))

    def test_widened_fallback_is_reported(self):
        self.gpus(RDNA4)
        self.kernels("gfx1201")
        output = self.check()
        self.assertIn("kernels in d4r/kernels/gfx1201\n", output)
        self.assertIn("no FP8 variant for gfx1201; using its 16-bit kernels", output)

    def test_prefer_accuracy_reports_the_accuracy_set_or_its_absence(self):
        self.gpus(RDNA3)
        self.kernels("gfx1101")
        self.ini("[Kernels]\nPreferAccuracy = true\n")
        self.assertIn("PreferAccuracy = true but no accuracy kernels for it here", self.check())
        self.kernels("accuracy/gfx1101", accuracy="0\n")
        self.assertIn("PreferAccuracy = true but no accuracy kernels for it here", self.check())
        self.kernels("accuracy/gfx1101", accuracy="1\n")
        self.assertIn("ok       GPU gfx1101: accuracy native kernels in d4r/kernels/accuracy/gfx1101", self.check())

    def test_native_kernels_off(self):
        self.gpus(RDNA3)
        self.kernels("gfx1101")
        self.ini("[Kernels]\nNativeKernels = off\n")
        self.assertIn("NativeKernels = off", self.check())

    def test_unsupported_gpu_has_no_kernels(self):
        self.gpus(IGPU)
        self.kernels("gfx1101")
        self.assertIn("GPU gfx1036: no native kernels for it in this release", self.check())

    def test_kernel_folder_matches_the_bridge_selection(self):
        layouts = {
            "release": [("gfx1101", True, None), ("gfx1201", True, None), ("gfx1201-fp8", True, None),
                        ("accuracy/gfx1101", True, "1\n"), ("accuracy/gfx1201-fp8", True, "1\n")],
            "fast only": [("gfx1100", True, None), ("gfx1201", True, None)],
            "unmarked accuracy": [("gfx1101", True, None), ("accuracy/gfx1101", True, None)],
            "no manifest": [("gfx1101", False, None), ("gfx1201-fp8", False, None)],
        }
        for name, folders in layouts.items():
            for arch, accuracy, fp8 in itertools.product(("gfx1100", "gfx1101", "gfx1201"), (0, 1), (0, 1)):
                with self.subTest(layout=name, arch=arch, accuracy=accuracy, fp8=fp8):
                    self.setUp()
                    for folder, manifest, marker in folders:
                        self.kernels(folder, manifest, marker)
                    kernels = self.game / "d4r" / "kernels"
                    bridge = subprocess.check_output([str(self.runner), str(kernels), arch, str(accuracy), str(fp8)],
                                                     text=True).strip()
                    output = self.check(D4R_GPU_ARCH=arch, D4R_PREFER_ACCURACY=str(accuracy),
                                        D4R_ZLUDA_WMMA_FP8_NATIVE=str(fp8))
                    if bridge:
                        self.assertIn(f"native kernels in {Path(bridge).relative_to(self.game)}\n", output)
                    else:
                        self.assertNotIn("native kernels in", output)


if __name__ == "__main__":
    unittest.main()
