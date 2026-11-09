"""A fake installer for testing installer/tools/installer-update (installer/SPEC.md, Tests 3).

It serves GET /api/status and POST /api/installer/update on localhost the way installer/main/self_update.c does:
the answer names the version from the posted image's app description and the slot it was written to, then the
installer restarts (its clock starts again from 0 and it does not answer for a moment) into an outcome:

- "update": the new version from ota_1, unconfirmed at first, confirmed shortly after;
- "refuse": a 400 answer with a reason, no restart;
- "drop": the first upload's connection is closed halfway, the next one is taken as in "update";
- "drop_always": every upload's connection is closed halfway;
- "rollback": the new firmware never answers, the bootloader returns to the old version on ota_0;
- "reset_while_confirming": the new version answers unconfirmed, then a reset returns it to the old version.
"""

import http.server
import json
import threading
import time

APP_DESC_AT = 32  # esp_app_desc_t after the image header and the first segment header
OLD_VERSION = "0.2"
REFUSAL = "the image does not pass its check: truncated or corrupt"
DOWN_S = 0.3      # a restart: no answers for this long
CONFIRM_S = 0.3   # an unconfirmed firmware confirms itself, or is reset, after this long
DROPPED_S = 0.2   # after a dropped upload, the update shows as running this long


def app_image(version, project="installer", size=200000):
    """An app image as far as the installer reads it: magic and the app description's version and project name."""
    image = bytearray(size)
    image[0] = 0xE9
    desc = APP_DESC_AT
    image[desc:desc + 4] = (0xABCD5432).to_bytes(4, "little")
    image[desc + 16:desc + 16 + len(version)] = version.encode()
    image[desc + 48:desc + 48 + len(project)] = project.encode()
    return bytes(image)


class FakeInstaller:
    def __init__(self, outcome):
        self.outcome = outcome
        self.lock = threading.Lock()
        self.posts = 0
        self.restarts = 0
        self.version = OLD_VERSION
        self.slot = "ota_0"
        self.confirmed = True
        self.boot_at = time.monotonic() - 600.0  # up for 10 minutes
        self.down_until = 0.0
        self.busy_until = 0.0
        self.events = []  # (time, action) to run on the clock: restarts and confirmations
        installer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                installer._handle(self)

            def do_POST(self):
                installer._handle(self)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def host(self):
        return "127.0.0.1:%d" % self.server.server_address[1]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    # ---- the installer's state over time ----------------------------------------------

    def _restart(self, version, slot, confirmed):
        self.restarts += 1
        self.version, self.slot, self.confirmed = version, slot, confirmed
        now = time.monotonic()
        self.boot_at = now + DOWN_S
        self.down_until = now + DOWN_S

    def _run_events(self):
        now = time.monotonic()
        for ev in [e for e in self.events if e[0] <= now]:
            self.events.remove(ev)
            ev[1]()

    def _after(self, delay, action):
        self.events.append((time.monotonic() + delay, action))

    def _accepted(self, version):
        old = (self.version, self.slot)
        if self.outcome == "rollback":
            self._after(0.1, lambda: self._restart(*old, True))
            return
        self._after(0.1, lambda: self._restart(version, "ota_1", False))
        if self.outcome == "reset_while_confirming":
            self._after(0.1 + DOWN_S + CONFIRM_S, lambda: self._restart(*old, True))
        else:
            self._after(0.1 + DOWN_S + CONFIRM_S, lambda: setattr(self, "confirmed", True))

    # ---- HTTP ---------------------------------------------------------------------------

    def _reply(self, h, code, doc):
        body = json.dumps(doc).encode()
        h.send_response(code)
        h.send_header("Content-Type", "application/json")
        h.send_header("Content-Length", str(len(body)))
        h.end_headers()
        h.wfile.write(body)

    def _handle(self, h):
        with self.lock:
            self._run_events()
            if time.monotonic() < self.down_until:
                h.close_connection = True
                return
            if h.command == "GET" and h.path == "/api/status":
                now = time.monotonic()
                update = {"written": 1000, "total": 2000} if now < self.busy_until else None
                self._reply(h, 200, {"now": int((now - self.boot_at) * 1000), "installer": {
                    "version": self.version, "built": "Nov  9 2026 12:00:00", "slot": self.slot,
                    "confirmed": self.confirmed, "update": update}})
                return
            if h.command == "POST" and h.path == "/api/installer/update":
                self.posts += 1
                length = int(h.headers["Content-Length"])
                drop = self.outcome == "drop_always" or (self.outcome == "drop" and self.posts == 1)
                if drop:
                    h.rfile.read(length // 2)
                    self.busy_until = time.monotonic() + DROPPED_S
                    h.close_connection = True
                    return
                body = h.rfile.read(length)
                if self.outcome == "refuse":
                    self._reply(h, 400, {"error": REFUSAL})
                    return
                version = body[APP_DESC_AT + 16:APP_DESC_AT + 48].split(b"\0")[0].decode()
                self._reply(h, 200, {"ok": True, "version": version, "slot": "ota_1"})
                self._accepted(version)
                return
            self._reply(h, 404, {"error": "not found"})
