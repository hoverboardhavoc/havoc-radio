"""Starts the host build of the installer servers on free localhost ports."""

import socket
import subprocess
import threading
import time


def free_port(kind):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class HostServer:
    def __init__(self, binary, stage):
        self.dns_port = free_port(socket.SOCK_DGRAM)
        self.update_port = free_port(socket.SOCK_STREAM)
        self.lines = []
        self._cond = threading.Condition()
        self.proc = subprocess.Popen(
            [binary, "--stage", stage, "--dns-port", str(self.dns_port), "--update-port", str(self.update_port)],
            stdout=subprocess.PIPE, text=True)
        threading.Thread(target=self._reader, daemon=True).start()
        if not self.wait_for("ready:", 5):
            self.stop()
            raise RuntimeError("host server did not start: %s" % self.lines)

    def _reader(self):
        for line in self.proc.stdout:
            with self._cond:
                self.lines.append(line.rstrip("\n"))
                self._cond.notify_all()

    def wait_for(self, text, timeout, start=0):
        """Waits for a log line containing text, from line index start. Returns the line or None."""
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for line in self.lines[start:]:
                    if text in line:
                        return line
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self._cond.wait(left)

    def mark(self):
        with self._cond:
            return len(self.lines)

    def stop(self):
        self.proc.kill()
        self.proc.wait()


class Checks:
    def __init__(self, name):
        self.name = name
        self.count = 0
        self.failures = 0

    def __call__(self, cond, what):
        self.count += 1
        if not cond:
            self.failures += 1
            print("FAIL: " + what)

    def report(self, extra=""):
        print("%s: %d checks, %d failures%s" % (self.name, self.count, self.failures, extra))
        return 1 if self.failures else 0
