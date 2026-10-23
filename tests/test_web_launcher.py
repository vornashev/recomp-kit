"""tools/web_launcher.py writes the hub page and each game's launcher data."""

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("web_launcher", ROOT / "tools/web_launcher.py")
web_launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web_launcher)


class WebLauncherTests(unittest.TestCase):
    def test_hub_for_two_games(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            other = tmp / "other"
            other.mkdir()
            text = (ROOT / "games/stub/game.toml").read_text().replace('id = "stub"', 'id = "other"')
            (other / "game.toml").write_text(text + '\n[launcher]\ntitle = "Other"\nstore = "https://x.test"\n'
                                                    '\n[setup]\nrequired_dirs = ["data"]\n')
            shutil.copy(ROOT / "games/stub/globals.toml", other / "globals.toml")
            out = tmp / "site"
            games = web_launcher.build([ROOT / "games/stub", other], out)
            self.assertEqual([g["id"] for g in games], ["stub", "other"])
            for name in web_launcher.PAGE_FILES:
                self.assertTrue((out / name).is_file(), name)
            data = json.loads((out / "games.json").read_text())
            self.assertEqual(data[1]["title"], "Other")
            self.assertEqual(data[1]["requiredDirs"], ["data"])
            self.assertEqual(data[0]["executable"], "STUB.EXE")
            self.assertEqual(data[0]["sha256"], "0" * 64)
            with self.assertRaises(ValueError):
                web_launcher.build([ROOT / "games/stub", ROOT / "games/stub"], tmp / "dup")

    def test_web_build_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build = tmp / "build"
            build.mkdir()
            with self.assertRaises(ValueError):
                web_launcher.copy_web_build("stub", build, tmp / "site")
            for name in ("index.html", "App.js", "App.data", "libgen.a"):
                (build / name).write_text(name)
            # A minimal valid module also works when llvm-objcopy is installed.
            (build / "App.wasm").write_bytes(b"\0asm\x01\0\0\0")
            web_launcher.copy_web_build("stub", build, tmp / "site")
            dest = tmp / "site/stub"
            copied = {f.name for f in dest.iterdir()}
            expected = {"App.data", "App.js", "App.wasm", "index.html", "build.json"}
            expected.update("App." + suffix + ".gz" for suffix in ("data", "js", "wasm"))
            if shutil.which("brotli"):
                expected.update("App." + suffix + ".br" for suffix in ("data", "js", "wasm"))
            self.assertEqual(copied, expected)
            metadata = json.loads((dest / "build.json").read_text())
            self.assertEqual(metadata["wasmBytes"], (dest / "App.wasm").stat().st_size)
            self.assertEqual(metadata["dataBytes"], (dest / "App.data").stat().st_size)
    def test_core_suite(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        run = subprocess.run([node, "--test", str(ROOT / "web/launcher/tests/core.test.mjs")],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
