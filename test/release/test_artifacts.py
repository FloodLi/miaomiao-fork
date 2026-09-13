"""Release package structural checks; uses only public synthetic binary data."""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("public_check", Path(__file__).resolve().parents[2] / "tools/check_public_docs.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class ReleaseArtifacts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = checker.ROOT
        self.addCleanup(setattr, checker, "ROOT", root)
        checker.ROOT = Path(self.temp.name)
        self.release = checker.ROOT / "release"
        self.release.mkdir()
        app = bytearray(512)
        app[0] = 0xE9
        app[32:36] = bytes.fromhex("3254cdab")
        version = checker.EXPECTED["project_version"].encode("ascii")
        app[48:48 + len(version)] = version
        full = bytearray(b"\xff") * 0xC20000
        full[0x20000:0x20000 + len(app)] = app
        for name, data in (("web_upgrade", app), ("epaper_uploader_full_16MB", full)):
            (self.release / (name + ".bin")).write_bytes(data)
            with zipfile.ZipFile(self.release / (name + ".zip"), "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(name + ".bin", data)

    def failures(self):
        failures = []
        with contextlib.redirect_stdout(io.StringIO()):
            checker.check_release_contents(failures)
        return [entry.split(": ", 1)[0] for entry in failures]

    def test_matching_packages(self):
        self.assertEqual([], self.failures())

    def test_wrong_version(self):
        with (self.release / "web_upgrade.bin").open("r+b") as app:
            app.seek(48)
            app.write(b"old\0")
        self.assertIn("release app descriptor has current version", self.failures())

    def test_full_app_mismatch(self):
        with (self.release / "epaper_uploader_full_16MB.bin").open("r+b") as full:
            full.seek(0x20000)
            full.write(b"\0")
        self.assertIn("full image embeds the exact OTA app at 0x20000", self.failures())

    def test_wrong_full_extent(self):
        with (self.release / "epaper_uploader_full_16MB.bin").open("ab") as full:
            full.write(b"\xff")
        self.assertIn("full image stops before user SPIFFS", self.failures())

    def test_extra_zip_file(self):
        with zipfile.ZipFile(self.release / "web_upgrade.zip", "a") as archive:
            archive.writestr("unexpected.txt", "not a release input")
        self.assertIn("web_upgrade ZIP contains only the matching BIN", self.failures())

    def test_corrupt_zip(self):
        (self.release / "web_upgrade.zip").write_bytes(b"invalid zip")
        self.assertIn("web_upgrade ZIP contains only the matching BIN", self.failures())

    def test_short_app(self):
        (self.release / "web_upgrade.bin").write_bytes(b"\xe9")
        self.assertIn("release app fits 3 MiB partition", self.failures())


if __name__ == "__main__":
    unittest.main()
