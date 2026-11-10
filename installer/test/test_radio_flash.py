"""Tests for installer/tools/radio-flash against the fake installer (installer/SPEC.md, Tests 3).

    python3 installer/test/test_radio_flash.py

The Havoc Radio and stock-backup cases use the real images when they are on
this machine (HAVOC_PROJECT_ARGS, from `make firmware`, and STOCK_IMAGE) and are
skipped otherwise.
"""

import hashlib
import importlib.machinery
import importlib.util
import os
import random
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)             # installer/
BUILD = os.path.join(ROOT, "build")
sys.path.insert(0, HERE)

import fake_bridge  # noqa: E402

HAVOC_PROJECT_ARGS = os.environ.get("HAVOC_PROJECT_ARGS", os.path.join(os.path.dirname(ROOT), "radio", "build", "flash_project_args"))
STOCK_IMAGE = os.environ.get("STOCK_IMAGE", "")


def write(path, data):
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def load_tool():
    path = os.path.join(ROOT, "tools", "radio-flash")
    loader = importlib.machinery.SourceFileLoader("radio_flash", path)
    spec = importlib.util.spec_from_loader("radio_flash", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    # Fast timings: the fake bridge stalls for 1 s, past this timeout.
    mod.TIMEOUT = 0.5
    mod.BACKOFF_MIN = 0.01
    mod.BACKOFF_MAX = 0.05
    mod.POLL_INTERVAL = 0.05
    mod.log = lambda msg: None
    return mod


rf = load_tool()
SECTOR = rf.SECTOR


def expected_flash(base, images):
    """The target flash after writing (offset, bytes) images onto base, with
    each image's last sector padded with 0xFF."""
    out = bytearray(base)
    for off, data in images:
        padded = data + b"\xff" * (-len(data) % SECTOR)
        out[off:off + len(padded)] = padded
    return bytes(out)


def havoc_images():
    settings, pairs = rf.parse_project_args(HAVOC_PROJECT_ARGS)
    images = []
    for off, path in pairs:
        data = read(path)
        if off == 0:
            data = rf.apply_flash_settings(data, settings)
        images.append((off, data))
    return settings, pairs, images


def total_parts(regions):
    return sum((r["length"] + rf.PART - 1) // rf.PART for r in regions if not r["erase"])


class RegionSplit(unittest.TestCase):
    def test_synthetic(self):
        rnd = random.Random(1)
        data = bytes(rnd.randrange(256) for _ in range(SECTOR))       # data
        data += b"\xff" * (3 * SECTOR)                                # erase run
        data += bytes(rnd.randrange(256) for _ in range(SECTOR + 10))  # data, short tail
        regions = rf.split_regions(0x20000, data)
        self.assertEqual([(r["target"], r["length"], r["erase"]) for r in regions],
                         [(0x20000, SECTOR, False), (0x21000, 3 * SECTOR, True),
                          (0x24000, 2 * SECTOR, False)])
        tail = data[5 * SECTOR:] + b"\xff" * (SECTOR - 10)
        self.assertEqual(regions[2]["data"], data[4 * SECTOR:5 * SECTOR] + tail)
        self.assertEqual(regions[1]["sha256"], hashlib.sha256(b"\xff" * 3 * SECTOR).hexdigest())
        self.assertEqual(regions[2]["sha256"], hashlib.sha256(regions[2]["data"]).hexdigest())

    def test_partial_erased_tail_is_data(self):
        # A short last sector is padded and staged, even when it is all 0xFF.
        regions = rf.split_regions(0, b"\x00" * SECTOR + b"\xff" * 100)
        self.assertEqual([(r["length"], r["erase"]) for r in regions], [(2 * SECTOR, False)])
        self.assertEqual(regions[0]["data"], b"\x00" * SECTOR + b"\xff" * SECTOR)

    def test_all_erased(self):
        regions = rf.split_regions(0xd000, b"\xff" * 2 * SECTOR)
        self.assertEqual([(r["target"], r["length"], r["erase"]) for r in regions],
                         [(0xd000, 2 * SECTOR, True)])

    def test_unaligned_offset(self):
        with self.assertRaises(rf.FlashError):
            rf.split_regions(0x100, b"\x00")

    def test_overlapping_images(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            a, b = os.path.join(d, "a.bin"), os.path.join(d, "b.bin")
            write(a, b"\x01" * (SECTOR + 1))
            write(b, b"\x02" * SECTOR)
            with self.assertRaises(rf.FlashError):
                rf.build_regions([(0x0, a), (0x1000, b)])
            self.assertEqual(len(rf.build_regions([(0x2000, b), (0x0, a)])), 2)

    def test_flash_settings(self):
        hdr = bytes([0xE9, 0x03, 0x00, 0x20]) + b"\x00" * 12
        out = rf.apply_flash_settings(hdr, {"flash_mode": "dio", "flash_freq": "40m",
                                            "flash_size": "4MB"})
        self.assertEqual(out[:4], bytes([0xE9, 0x03, 0x02, 0x40]))
        out = rf.apply_flash_settings(hdr, {"flash_mode": "keep", "flash_freq": "80m",
                                            "flash_size": "keep"})
        self.assertEqual(out[:4], bytes([0xE9, 0x03, 0x00, 0x2F]))
        self.assertEqual(rf.apply_flash_settings(b"\xaa\x50\x01\x02", {"flash_mode": "dio"}),
                         b"\xaa\x50\x01\x02")

    def test_project_args(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            os.mkdir(os.path.join(d, "bootloader"))
            write(os.path.join(d, "bootloader", "bootloader.bin"),
                  bytes([0xE9, 0x03, 0x00, 0x00]) + b"\x00" * 100)
            write(os.path.join(d, "app.bin"), b"\x11" * 5000)
            args = os.path.join(d, "flash_project_args")
            write(args, "--flash_mode dio --flash_freq 40m --flash_size 4MB\n"
                                  "0x10000 app.bin\n0x0 bootloader/bootloader.bin\n")
            settings, pairs = rf.parse_project_args(args)
            self.assertEqual(settings, {"flash_mode": "dio", "flash_freq": "40m", "flash_size": "4MB"})
            regions = rf.build_regions(pairs, settings)
            self.assertEqual([(r["target"], r["length"]) for r in regions],
                             [(0x0, SECTOR), (0x10000, 2 * SECTOR)])
            self.assertEqual(regions[0]["data"][:4], bytes([0xE9, 0x03, 0x02, 0x40]))

    @unittest.skipUnless(os.path.exists(HAVOC_PROJECT_ARGS), "no Havoc Radio build")
    def test_havoc_radio(self):
        settings, pairs, images = havoc_images()
        regions = rf.build_regions(pairs, settings)
        sectors = {off: (len(data) + SECTOR - 1) // SECTOR for off, data in images}
        self.assertEqual([(r["target"], r["length"], r["erase"]) for r in regions],
                         [(0x0, sectors[0x0] * SECTOR, False), (0x8000, SECTOR, False),
                          (0xd000, 0x2000, True), (0x100000, sectors[0x100000] * SECTOR, False)])
        rebuilt = bytearray(b"\x00" * 0x200000)
        for r in regions:
            rebuilt[r["target"]:r["target"] + r["length"]] = r["data"] or b"\xff" * r["length"]
        self.assertEqual(bytes(rebuilt), expected_flash(b"\x00" * 0x200000, images))

    @unittest.skipUnless(os.path.exists(STOCK_IMAGE), "no stock backup")
    def test_stock_backup(self):
        data = read(STOCK_IMAGE)
        regions = rf.build_regions([(0, STOCK_IMAGE)])
        self.assertEqual(len(regions), 11)
        self.assertEqual(sum(1 for r in regions if r["erase"]), 5)
        staged = sum(r["length"] for r in regions if not r["erase"])
        self.assertEqual(staged, 239 * SECTOR)  # 978,944 B, fits the stage
        self.assertEqual(sum(r["length"] for r in regions), len(data))
        rebuilt = b"".join(r["data"] or b"\xff" * r["length"] for r in regions)
        self.assertEqual(rebuilt, data)
        # Erase-only runs really are whole erased sectors; data regions hold none.
        for r in regions:
            secs = [r["data"][i:i + SECTOR] for i in range(0, r["length"], SECTOR)] if r["data"] else []
            self.assertFalse(any(s == rf.ERASED for s in secs))


class Upload(unittest.TestCase):
    def setUp(self):
        self.fakes = []

    def tearDown(self):
        for f in self.fakes:
            f.stop()

    def fake(self, **kw):
        f = fake_bridge.FakeBridge(**kw)
        self.fakes.append(f)
        return f

    def random_image(self, seed, size):
        rnd = random.Random(seed)
        data = bytearray(rnd.randbytes(size))
        # Erased runs inside the image.
        for lo, hi in ((0x30000, 0x38000), (0x80000, 0x81000)):
            if hi <= size:
                data[lo:hi] = b"\xff" * (hi - lo)
        return bytes(data)

    def check_result(self, fake, regions, ok, base, images):
        self.assertTrue(ok)
        st = fake.store
        self.assertEqual(bytes(st.target), expected_flash(base, images))
        # The staged bytes equal the images.
        for sr, r in zip(st.regions, regions):
            if not r["erase"]:
                self.assertEqual(bytes(st.stage[sr["stage"]:sr["stage"] + sr["length"]]), r["data"])
        # No part was sent while the bridge held it.
        self.assertEqual(st.violations, [])

    def test_clean_link(self):
        image = self.random_image(2, 0x100000)
        f = self.fake()
        regions = rf.split_regions(0, image)
        ok = rf.flash(f.host, regions)
        self.check_result(f, regions, ok, bytes(rf.SECTOR * 1024), [(0, image)])
        self.assertEqual(len(f.store.puts), total_parts(regions))

    def test_chaos(self):
        image = self.random_image(3, 0x0c0000)
        regions = rf.split_regions(0x10000, image)
        events = {}
        for seed in range(6):
            with self.subTest(seed=seed):
                f = self.fake(chaos=0.3, seed=seed)
                ok = rf.flash(f.host, regions)
                self.check_result(f, regions, ok, bytes(4 * 1024 * 1024), [(0x10000, image)])
                for k, v in f.events.items():
                    events[k] = events.get(k, 0) + v
        # Every kind of failure happened at least once.
        self.assertTrue(all(events[k] > 0 for k in ("drop", "drop_after", "stall", "reboot")), events)

    @unittest.skipUnless(os.path.exists(HAVOC_PROJECT_ARGS), "no Havoc Radio build")
    def test_havoc_radio_chaos(self):
        settings, pairs, images = havoc_images()
        regions = rf.build_regions(pairs, settings)
        f = self.fake(chaos=0.3, seed=10)
        ok = rf.flash(f.host, regions)
        self.check_result(f, regions, ok, bytes(4 * 1024 * 1024), images)
        self.assertGreater(f.boots, 1)

    @unittest.skipUnless(os.path.exists(STOCK_IMAGE), "no stock backup")
    def test_stock_backup_chaos(self):
        data = read(STOCK_IMAGE)
        regions = rf.build_regions([(0, STOCK_IMAGE)])
        f = self.fake(chaos=0.3, seed=11)
        ok = rf.flash(f.host, regions)
        self.check_result(f, regions, ok, bytes(4 * 1024 * 1024), [(0, data)])

    def test_resume_after_interrupt_and_reboot(self):
        image = self.random_image(4, 0x0a0000)
        regions = rf.split_regions(0, image)
        f = self.fake()
        bridge = rf.Bridge(f.host)

        class Stop(Exception):
            pass

        def stop_after(n):
            count = [0]

            def on_part(i, k):
                count[0] += 1
                if count[0] == n:
                    raise Stop()
            return on_part

        # Regions: 3 parts, erase, 5 parts, erase, 2 parts. Four parts complete
        # the first region and one part of the second.
        self.assertEqual([sum(1 for _ in range(0, r["length"], rf.PART)) for r in regions if not r["erase"]],
                         [3, 5, 2])
        with self.assertRaises(Stop):
            rf.upload(bridge, regions, on_part=stop_after(4))
        self.assertEqual(len(f.store.puts), 4)
        f._reboot()  # complete regions survive; the part of the second does not
        sent = rf.upload(rf.Bridge(f.host), regions)
        self.assertEqual(sent, total_parts(regions) - 3)
        self.assertEqual(len(f.store.puts), total_parts(regions) + 1)
        self.assertEqual(f.store.violations, [])
        # Everything staged: a third run sends nothing, also after a reboot.
        self.assertEqual(rf.upload(rf.Bridge(f.host), regions), 0)
        f._reboot()
        self.assertEqual(rf.upload(rf.Bridge(f.host), regions), 0)

    def test_new_manifest_restages(self):
        a = rf.split_regions(0, self.random_image(5, 0x40000))
        b = rf.split_regions(0, self.random_image(6, 0x40000))
        f = self.fake()
        self.assertEqual(rf.upload(rf.Bridge(f.host), a), total_parts(a))
        self.assertEqual(rf.upload(rf.Bridge(f.host), b), total_parts(b))
        self.assertEqual(rf.upload(rf.Bridge(f.host), a), total_parts(a))
        self.assertEqual(f.store.violations, [])

    def test_check_mismatch_reuploads_region(self):
        image = self.random_image(7, 0x60000)
        regions = rf.split_regions(0, image)
        f = self.fake()
        rf.upload(rf.Bridge(f.host), regions)
        r = f.store.regions[-1]
        self.assertFalse(r["erase"])
        f.store.stage[r["stage"] + 5] ^= 0xFF  # stage flash went bad
        sent = rf.upload(rf.Bridge(f.host), regions)
        self.assertEqual(sent, (r["length"] + rf.PART - 1) // rf.PART)
        self.assertTrue(rf.flash(f.host, regions))

    def test_stage_too_small(self):
        # The header takes the stage's first sector.
        image = random.Random(8).randbytes(fake_bridge.STAGE_SIZE)
        f = self.fake()
        with self.assertRaises(rf.HTTPStatusError) as cm:
            rf.upload(rf.Bridge(f.host), rf.split_regions(0, image))
        self.assertEqual(cm.exception.status, 400)

    def test_label(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as d:
            small, big = os.path.join(d, "boot.bin"), os.path.join(d, "havoc-radio.bin")
            write(small, b"\x01" * 100)
            write(big, b"\x02" * 5000)
            self.assertEqual(rf.image_label([(0x0, small), (0x10000, big)]), "havoc-radio.bin")
            f = self.fake()
            rf.upload(rf.Bridge(f.host), rf.build_regions([(0x0, small)]), label="boot.bin")
            self.assertEqual(f.store.label, "boot.bin")

    def test_flash_refused_without_check(self):
        f = self.fake()
        with self.assertRaises(rf.FlashError):
            rf.run_job(rf.Bridge(f.host), 460800, True)


if __name__ == "__main__":
    unittest.main()
