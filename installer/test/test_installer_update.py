"""Tests for installer/tools/installer-update against the fake installer (installer/SPEC.md, Tests 3).

    python3 installer/test/test_installer_update.py
"""

import importlib.machinery
import importlib.util
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)             # installer/
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, HERE)

import fake_installer  # noqa: E402

NEW_VERSION = "0.3"


def load_tool():
    path = os.path.join(ROOT, "tools", "installer-update")
    loader = importlib.machinery.SourceFileLoader("installer_update", path)
    spec = importlib.util.spec_from_loader("installer_update", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    mod.POLL_INTERVAL = 0.05
    mod.IDLE_WAIT = 5.0
    mod.RESTART_WAIT = 5.0
    return mod


iu = load_tool()


class InstallerUpdateTest(unittest.TestCase):
    def run_tool(self, outcome):
        """Run installer-update against a fake installer; returns (exit code, log lines, fake)."""
        fake = fake_installer.FakeInstaller(outcome)
        self.addCleanup(fake.stop)
        lines = []
        iu.log = lines.append
        os.makedirs(BUILD, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            path = os.path.join(d, "installer.bin")
            with open(path, "wb") as f:
                f.write(fake_installer.app_image(NEW_VERSION))
            code = iu.main(["--host", fake.host, path])
        return code, lines, fake

    def test_clean_update(self):
        code, lines, fake = self.run_tool("update")
        self.assertEqual(code, 0, lines)
        self.assertEqual((fake.posts, fake.restarts), (1, 1))
        self.assertEqual(lines[-1], "installer 0.3 runs from ota_1, confirmed")
        self.assertTrue(any("confirms itself" in ln for ln in lines), lines)

    def test_refusal(self):
        code, lines, fake = self.run_tool("refuse")
        self.assertEqual(code, 1)
        self.assertEqual((fake.posts, fake.restarts), (1, 0))
        self.assertIn(fake_installer.REFUSAL, lines[-1])
        self.assertIn("HTTP 400", lines[-1])

    def test_dropped_upload_is_sent_again(self):
        code, lines, fake = self.run_tool("drop")
        self.assertEqual(code, 0, lines)
        self.assertEqual((fake.posts, fake.restarts), (2, 1))
        self.assertTrue(any("sending it again (2 of 3)" in ln for ln in lines), lines)
        self.assertEqual(lines[-1], "installer 0.3 runs from ota_1, confirmed")

    def test_upload_gives_up_after_three_attempts(self):
        code, lines, fake = self.run_tool("drop_always")
        self.assertEqual(code, 1)
        self.assertEqual((fake.posts, fake.restarts), (3, 0))
        self.assertIn("upload failed 3 times", lines[-1])

    def test_rollback(self):
        code, lines, fake = self.run_tool("rollback")
        self.assertEqual(code, 1)
        self.assertEqual(fake.restarts, 1)
        self.assertIn("came back on 0.2 from ota_0: the update to 0.3 was rolled back", lines[-1])

    def test_reset_while_confirming_is_a_rollback(self):
        code, lines, fake = self.run_tool("reset_while_confirming")
        self.assertEqual(code, 1)
        self.assertEqual(fake.restarts, 2)
        self.assertTrue(any("confirms itself" in ln for ln in lines), lines)
        self.assertIn("rolled back", lines[-1])

    def test_no_installer(self):
        fake = fake_installer.FakeInstaller("update")
        host = fake.host
        fake.stop()
        lines = []
        iu.log = lines.append
        self.assertEqual(iu.main(["--host", host, os.devnull]), 1)
        self.assertIn("no answer from the installer", lines[-1])


if __name__ == "__main__":
    unittest.main()
