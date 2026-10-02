"""d4r-manifest (tools/d4r_manifest.c) against kernels/tools/kernel_manifest.py, on a synthetic nvngx_dlss.dll.

The DLL holds a version resource, an uncompressed and an LZ4-compressed fatbin and decoys, so the C tool that
Windows installs use must list exactly what the Python tool lists.

Run: python3 -m unittest discover -s tests -v
"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PTX_K = b"""//
.version 8.5
.target sm_89
.visible .entry dltss_pwin_enc0_layer(
    .param .u64 p0
)
{ ret; }
.visible .entry dltss_pwin_dec0_layer( .param .u64 p0 ) { ret; }
""" + b"// padding " * 40 + b"\0\0"
PTX_M = (b"//\n.version 8.5\n.visible .entry rrlite_enc1_4x4(\n.param .u64 p0\n)\n{ ret; }\n" +
         b"// repeated repeated repeated repeated text " * 60)


def lz4_compress(data):
    """A plain greedy LZ4 block compressor (enough for a test vector with real matches)."""
    out = bytearray()
    table = {}
    i = anchor = 0

    def length_bytes(value):
        while value >= 255:
            out.append(255)
            value -= 255
        out.append(value)

    while i + 4 <= len(data) - 5:
        key = data[i:i + 4]
        candidate = table.get(key)
        table[key] = i
        if candidate is None or i - candidate > 0xffff:
            i += 1
            continue
        match = 4
        while i + match < len(data) - 5 and data[candidate + match] == data[i + match]:
            match += 1
        literals = i - anchor
        out.append((min(literals, 15) << 4) | min(match - 4, 15))
        if literals >= 15:
            length_bytes(literals - 15)
        out += data[anchor:i]
        out += struct.pack("<H", i - candidate)
        if match - 4 >= 15:
            length_bytes(match - 4 - 15)
        i += match
        anchor = i
    literals = len(data) - anchor
    out.append(min(literals, 15) << 4)
    if literals >= 15:
        length_bytes(literals - 15)
    out += data[anchor:]
    return bytes(out)


def fatbin(ptx, compress=False):
    payload = lz4_compress(ptx) if compress else ptx
    flags = 0x2000 if compress else 0
    entry = struct.pack("<HHIQ", 1, 0x101, 64, len(payload)) + b"\0" * 24 + struct.pack("<QQQ", flags, 0,
                                                                                          len(ptx) if compress else 0)
    assert len(entry) == 64
    body = entry + payload
    return struct.pack("<IHHQ", 0xba55ed50, 1, 16, len(body)) + body


def fake_dll(major=310, minor=7, build=0):
    version = struct.pack("<IIII", 0xfeef04bd, 0x10000, (major << 16) | minor, build << 16)
    decoy = struct.pack("<IHHQ", 0xba55ed50, 2, 16, 64)  # wrong version: skipped
    return (b"MZ" + b"\x90" * 62 + version + b"\0" * 32 + decoy + b"junk" * 9 + fatbin(PTX_K) + b"\xcc" * 13 +
            fatbin(PTX_M, compress=True) + b"tail")


class ManifestToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.tool = Path(cls.build.name) / "d4r-manifest"
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "tools"),
                        str(ROOT / "tools" / "d4r_manifest.c"), "-o", str(cls.tool)], check=True, capture_output=True,
                       text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.kernels = self.root / "kernels"
        self.dll = self.root / "nvngx_dlss.dll"
        self.dll.write_bytes(fake_dll())

    def folder(self, name, kernels):
        directory = self.kernels / name
        directory.mkdir(parents=True, exist_ok=True)
        for kernel in kernels:
            (directory / f"{kernel}.hsaco").write_bytes(b"code object")
        return directory

    @staticmethod
    def listing(directory):
        lines = (directory / "d4r-kernels.txt").read_text().splitlines()
        return [line for line in lines if not line.startswith("#")]

    def test_lz4_vector_round_trips_through_the_python_decoder(self):
        import sys
        sys.path.insert(0, str(ROOT / "kernels" / "tools"))
        from extract_dlss_ptx import lz4_block
        compressed = lz4_compress(PTX_M)
        self.assertLess(len(compressed), len(PTX_M) // 2)  # real matches, not literals only
        self.assertEqual(lz4_block(compressed, len(PTX_M)), PTX_M)

    def test_same_manifests_as_the_python_tool(self):
        folders = [self.folder("gfx1201", ["dltss_pwin_enc0_layer", "dltss_pwin_dec0_layer", "rrlite_enc1_4x4"]),
                   self.folder("gfx1201-fp8", ["rrlite_enc1_4x4"]),
                   self.folder("accuracy/gfx1201", ["dltss_pwin_dec0_layer"])]
        result = subprocess.run([str(self.tool), str(self.kernels), str(self.dll)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        ours = {folder: self.listing(folder) for folder in folders}
        for folder in folders:
            subprocess.run(["python3", str(ROOT / "kernels" / "tools" / "kernel_manifest.py"), str(folder),
                            str(self.dll)], check=True, capture_output=True, text=True)
            self.assertEqual(ours[folder], self.listing(folder), folder)
        self.assertEqual(len(ours[folders[0]]), 3)

    def test_two_dll_versions_list_both_hashes(self):
        other = self.root / "nvngx_dlss_3109.dll"
        other.write_bytes(fake_dll(minor=9, build=1).replace(b"padding", b"PADDING"))
        folder = self.folder("gfx1101", ["dltss_pwin_enc0_layer"])
        result = subprocess.run([str(self.tool), str(self.kernels), str(self.dll), str(other)], capture_output=True,
                                text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        ours = self.listing(folder)
        subprocess.run(["python3", str(ROOT / "kernels" / "tools" / "kernel_manifest.py"), str(folder), str(self.dll),
                        str(other)], check=True, capture_output=True, text=True)
        self.assertEqual(ours, self.listing(folder))
        self.assertEqual(len(ours), 2)

    def test_other_dlss_versions_are_refused_unless_forced(self):
        self.dll.write_bytes(fake_dll(minor=10))
        folder = self.folder("gfx1201", ["dltss_pwin_enc0_layer"])
        result = subprocess.run([str(self.tool), str(self.kernels), str(self.dll)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("310.10", result.stderr)
        self.assertFalse((folder / "d4r-kernels.txt").exists())
        result = subprocess.run([str(self.tool), "--force", str(self.kernels), str(self.dll)], capture_output=True,
                                text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((folder / "d4r-kernels.txt").exists())

    def test_kernel_missing_from_the_dll_writes_no_manifest(self):
        folder = self.folder("gfx1100", ["dltss_pwin_enc0_layer", "not_in_dlss"])
        result = subprocess.run([str(self.tool), str(self.kernels), str(self.dll)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not_in_dlss", result.stderr)
        self.assertFalse((folder / "d4r-kernels.txt").exists())


if __name__ == "__main__":
    unittest.main()
