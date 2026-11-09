"""A fake installer for testing tools/radio-flash (installer/SPEC.md, Tests 3).

It serves the staging and flash-job API of installer/main/stage.c and
installer/main/flash_job.c on localhost, with the same rules, and misbehaves at
random like the bench WiFi link: it drops connections before or after handling a
request, stalls past the client's timeout, and reboots. A reboot loses RAM state
(the stage check, the job, the presence of parts in regions not yet complete) and
keeps what the C3 keeps in flash: the stage bytes and the stage header with the
manifest and each region's present flag. The flash job writes into a simulated
ESP8266 flash.
"""

import hashlib
import http.server
import json
import os
import random
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import make_stage  # noqa: E402

SECTOR = 4096
PART = 65536
STAGE_SIZE = make_stage.stage_partition()[1]
MAX_REGIONS = 64
HEADER_SIZE = SECTOR  # sector 0 of the stage is the header
TARGET_SIZE = 4 * 1024 * 1024


class Store:
    """What survives a reboot: the C3's stage partition and NVS, and the
    target's flash."""

    def __init__(self):
        self.stage = bytearray(b"\xff" * STAGE_SIZE)
        self.label = ""
        self.regions = []      # manifest, with "stage" offsets and "present" flags
        self.target = bytearray(TARGET_SIZE)
        self.puts = []         # (region, offset, length) of every PUT handled
        self.violations = []   # PUTs for parts the bridge already held


class Drop(Exception):
    """Close the connection without an answer."""


class Reboot(Exception):
    """Close the connection and reboot the bridge."""


class FakeBridge:
    def __init__(self, store=None, chaos=0.0, seed=0, stall=1.0):
        self.store = store or Store()
        self.chaos = chaos
        self.rng = random.Random(seed)
        self.stall = stall
        self.lock = threading.Lock()
        self.port = 0
        self.server = None
        self.thread = None
        self.boots = 0
        self.events = {"drop": 0, "drop_after": 0, "stall": 0, "reboot": 0}
        self._boot()

    @property
    def host(self):
        return f"127.0.0.1:{self.port}"

    # ---- lifecycle --------------------------------------------------------

    def _sectors(self, r):
        return set(range(r["stage"] // SECTOR, (r["stage"] + r["length"]) // SECTOR))

    def _flag(self, r):
        """Set the region's present flag from its sectors."""
        r["present"] = self._sectors(r) <= self.present

    def _boot(self):
        # Sector presence is RAM state; the header flags rebuild it.
        self.present = set()
        for r in self.store.regions:
            if not r["erase"] and r["present"]:
                self.present |= self._sectors(r)
        self.checked = False
        self.job = 0  # the job counter lives in RAM
        self.job_state = {"job": 0, "state": "idle", "region": 0, "bytes_done": 0,
                          "bytes_total": 0, "baud": 0, "rewrites": 0, "elapsed_ms": 0,
                          "error": "", "regions": []}
        self.job_thread = None
        bridge = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                bridge._handle(self)

            def do_POST(self):
                bridge._handle(self)

            def do_PUT(self):
                bridge._handle(self)

        http.server.HTTPServer.allow_reuse_address = True
        self.server = http.server.HTTPServer(("127.0.0.1", self.port), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.boots += 1

    def _reboot(self):
        self.server.shutdown()
        self.server.server_close()
        time.sleep(0.2)
        self._boot()

    def stop(self):
        if self.job_thread:
            self.job_thread.join()
        self.server.shutdown()
        self.server.server_close()

    # ---- request handling -------------------------------------------------

    def _handle(self, h):
        mode = None
        job_running = self.job_state["state"] == "running"
        if self.chaos:
            x = self.rng.random()
            c = self.chaos
            if x < c * 0.4:
                mode = "drop"
            elif x < c * 0.7:
                mode = "drop_after"
            elif x < c * 0.9:
                mode = "stall"
            elif x < c and not job_running:
                mode = "reboot"
        if mode:
            self.events[mode] += 1
        try:
            if mode == "drop":
                raise Drop()
            if mode == "stall":
                time.sleep(self.stall)
            status, doc = self._route(h, reboot_midway=(mode == "reboot"))
            if mode == "reboot":
                raise Reboot()
            if mode == "drop_after":
                raise Drop()
        except Drop:
            h.close_connection = True
            return
        except Reboot:
            h.close_connection = True
            threading.Thread(target=self._reboot, daemon=True).start()
            return
        body = json.dumps(doc).encode()
        try:
            h.send_response(status)
            h.send_header("Content-Type", "application/json")
            h.send_header("Content-Length", str(len(body)))
            h.end_headers()
            h.wfile.write(body)
        except OSError:
            pass  # the client gave up (stall)

    def _route(self, h, reboot_midway):
        url = urllib.parse.urlparse(h.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        n = int(h.headers.get("Content-Length") or 0)
        body = h.rfile.read(n) if n else b""
        if reboot_midway and not (h.command == "PUT" and url.path == "/stage/part"):
            raise Reboot()
        key = (h.command, url.path)
        if key in (("POST", "/stage"), ("PUT", "/stage/part"), ("POST", "/stage/check")) \
                and self.job_state["state"] == "running":
            return 409, {"error": "flash job running"}
        if key == ("POST", "/stage"):
            return self._post_stage(body)
        if key == ("GET", "/stage"):
            return 200, self._state()
        if key == ("PUT", "/stage/part"):
            return self._put_part(int(q["region"]), int(q["offset"]), body, reboot_midway)
        if key == ("POST", "/stage/check"):
            return self._check()
        if key == ("POST", "/flash"):
            return self._post_flash(json.loads(body or b"{}"))
        if key == ("GET", "/flash"):
            return 200, dict(self.job_state)
        return 404, {"error": "not found"}

    def _state(self):
        st = self.store
        regions = []
        for r in st.regions:
            nparts = 0 if r["erase"] else (r["length"] + PART - 1) // PART
            parts = []
            for k in range(nparts):
                lo = r["stage"] + k * PART
                hi = r["stage"] + min(r["length"], (k + 1) * PART)
                parts.append(all(s in self.present for s in range(lo // SECTOR, hi // SECTOR)))
            regions.append({"target": r["target"], "length": r["length"], "erase": r["erase"],
                            "stage": r["stage"], "parts": parts})
        return {"stage_size": STAGE_SIZE, "regions": regions}

    def _post_stage(self, body):
        doc = json.loads(body)
        regions = []
        stage = HEADER_SIZE
        if not 0 < len(doc["regions"]) <= MAX_REGIONS:
            return 400, {"error": "region count out of range"}
        for r in doc["regions"]:
            r = {k: r[k] for k in ("target", "length", "sha256", "erase")}
            if r["target"] % SECTOR or r["length"] % SECTOR or not r["length"]:
                return 400, {"error": "region target or length not 4 KB-aligned"}
            for o in regions:
                if r["target"] < o["target"] + o["length"] and o["target"] < r["target"] + r["length"]:
                    return 400, {"error": "regions overlap"}
            r["stage"] = 0
            r["present"] = False
            if not r["erase"]:
                if r["length"] > STAGE_SIZE - stage:
                    return 400, {"error": "data regions do not fit in the stage"}
                r["stage"] = stage
                stage += r["length"]
            regions.append(r)
        strip = lambda rs: [{k: r[k] for k in ("target", "length", "sha256", "erase")} for r in rs]
        kept = bool(self.store.regions) and strip(regions) == strip(self.store.regions)
        if not kept:
            self.store.regions = regions
            self.present = set()
            self.checked = False
        self.store.label = doc.get("label", "")
        doc = self._state()
        doc["kept"] = kept
        return 200, doc

    def _put_part(self, i, off, data, reboot_midway):
        st = self.store
        if i >= len(st.regions) or st.regions[i]["erase"]:
            return 400, {"error": "no such region"}
        r = st.regions[i]
        if off % SECTOR or not data or len(data) > PART or len(data) % SECTOR \
                or off + len(data) > r["length"]:
            return 400, {"error": "part size invalid"}
        addr = r["stage"] + off
        sectors = range(addr // SECTOR, (addr + len(data)) // SECTOR)
        if all(s in self.present for s in sectors):
            st.violations.append((i, off))
        self.present -= set(sectors)
        self._flag(r)
        self.checked = False
        for n, s in enumerate(sectors):
            if reboot_midway and n == len(sectors) // 2:
                raise Reboot()  # half written, presence already cleared
            st.stage[s * SECTOR:(s + 1) * SECTOR] = data[n * SECTOR:(n + 1) * SECTOR]
        self.present |= set(sectors)
        self._flag(r)
        st.puts.append((i, off, len(data)))
        return 200, {"region": i, "offset": off, "length": len(data)}

    def _check(self):
        st = self.store
        if not st.regions:
            return 409, {"error": "no manifest staged"}
        results = []
        for r in st.regions:
            ok = True
            if not r["erase"]:
                data = bytes(st.stage[r["stage"]:r["stage"] + r["length"]])
                ok = hashlib.sha256(data).hexdigest() == r["sha256"]
                sectors = self._sectors(r)
                self.present = self.present | sectors if ok else self.present - sectors
                self._flag(r)
            results.append("ok" if ok else "mismatch")
        self.checked = all(v == "ok" for v in results)
        return 200, {"ok": self.checked, "regions": results}

    def _post_flash(self, doc):
        if self.job_state["state"] == "running":
            return 409, {"error": "flash job running"}
        if not self.checked:
            return 409, {"error": "stage check has not passed"}
        self.job += 1
        regions = list(self.store.regions)
        self.job_state = {"job": self.job, "state": "running", "region": 0, "bytes_done": 0,
                          "bytes_total": sum(r["length"] for r in regions),
                          "baud": doc.get("baud", 460800), "rewrites": 0, "elapsed_ms": 0,
                          "error": "",
                          "regions": [{"target": r["target"], "verified": False} for r in regions]}
        self.job_thread = threading.Thread(target=self._job, args=(regions,), daemon=True)
        self.job_thread.start()
        return 202, {"job": self.job}

    def _job(self, regions):
        st = self.store
        t0 = time.monotonic()
        for i, r in enumerate(regions):
            self.job_state["region"] = i
            if r["erase"]:
                data = b"\xff" * r["length"]
            else:
                data = bytes(st.stage[r["stage"]:r["stage"] + r["length"]])
            st.target[r["target"]:r["target"] + r["length"]] = data
            got = hashlib.md5(st.target[r["target"]:r["target"] + r["length"]]).digest()
            self.job_state["regions"][i]["verified"] = got == hashlib.md5(data).digest()
            self.job_state["bytes_done"] += r["length"]
            time.sleep(0.01)
        self.job_state["elapsed_ms"] = int((time.monotonic() - t0) * 1000)
        self.job_state["state"] = "done"
