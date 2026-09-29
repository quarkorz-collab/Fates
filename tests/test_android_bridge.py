"""Exercise the Android service lifecycle without requiring an Android SDK."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.request import urlopen


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "frontend"))
sys.path.insert(0, str(ROOT / "android/app/src/main/python"))
import android_bridge  # noqa: E402
import fates_web  # noqa: E402


class AndroidBridgeTests(unittest.TestCase):
    def tearDown(self):
        android_bridge.stop()

    def test_local_only_and_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "index.html").write_text("<html>local Fates</html>", encoding="utf-8")
            with (patch.object(fates_web, "inspect_fates", return_value="Fates test"),
                  patch.object(fates_web, "inspect_symbol_catalog", return_value={"constants": []})):
                first = android_bridge.start(sys.executable, str(root))
                self.assertTrue(first.startswith("http://127.0.0.1:"), first)
                self.assertEqual(first, android_bridge.start(sys.executable, str(root)))
                with urlopen(first, timeout=3) as response:
                    self.assertEqual(response.read(), b"<html>local Fates</html>")
                with urlopen(first + "api/meta", timeout=3) as response:
                    data = json.load(response)
                self.assertEqual(data["platform"], "android")
                self.assertEqual(data["url"], first)
                self.assertEqual(data["symbols"], {"constants": []})
                android_bridge.stop()
                second = android_bridge.start(sys.executable, str(root))
                with urlopen(second + "api/health", timeout=3) as response:
                    self.assertTrue(json.load(response)["ok"])

    def test_missing_assets_never_starts_server(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                android_bridge.start(sys.executable, directory)
        self.assertIsNone(android_bridge._active)


if __name__ == "__main__":
    unittest.main()
