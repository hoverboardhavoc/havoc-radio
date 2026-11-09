#!/usr/bin/env python3
"""DNS replies of the host server (installer/SPEC.md, Tests 1).

Usage: test_dns.py SERVER_BINARY STAGE_FILE

dig gets 192.168.4.1 for iotc.kuailece.cn and for an arbitrary name, NOERROR with
no answers for AAAA; malformed packets get no reply and the server keeps answering.
"""

import os
import re
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hostserver import Checks, HostServer  # noqa: E402


def dig(port, name, qtype):
    out = subprocess.run(["dig", "@127.0.0.1", "-p", str(port), name, qtype, "+tries=1", "+time=2"],
                         capture_output=True, text=True).stdout
    status = re.search(r"status: (\w+)", out)
    answers = re.search(r"ANSWER: (\d+)", out)
    records = re.findall(r"^(\S+)\s+(\d+)\s+IN\s+(\S+)\s+(\S+)$", out, re.M)
    return (status.group(1) if status else None, int(answers.group(1)) if answers else None, records)


def no_reply(port, packet):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    s.sendto(packet, ("127.0.0.1", port))
    try:
        s.recvfrom(512)
        return False
    except socket.timeout:
        return True
    finally:
        s.close()


def main():
    binary, stage_path = sys.argv[1], sys.argv[2]
    check = Checks("dns")
    srv = HostServer(binary, stage_path)
    try:
        for name in ("iotc.kuailece.cn", "some-other.example.org"):
            status, count, records = dig(srv.dns_port, name, "A")
            check(status == "NOERROR" and count == 1, "%s A: status %s, %s answers" % (name, status, count))
            check(records == [(name + ".", "60", "A", "192.168.4.1")], "%s A records %s" % (name, records))
            check(srv.wait_for("dns: %s (type 1)" % name, 1) is not None, "log: query name %s" % name)
        status, count, records = dig(srv.dns_port, "iotc.kuailece.cn", "AAAA")
        check(status == "NOERROR" and count == 0 and not records,
              "AAAA: status %s, %s answers %s" % (status, count, records))

        query = bytes.fromhex("1234 0100 0001 0000 0000 0000") + b"\x04iotc\x08kuailece\x02cn\x00" + b"\x00\x01\x00\x01"
        check(no_reply(srv.dns_port, query[:5]), "short packet dropped")
        check(no_reply(srv.dns_port, query[:-2]), "truncated question dropped")
        check(no_reply(srv.dns_port, query[:12] + b"\xc0\x0c" + query[-4:]), "compressed question name dropped")
        response = bytearray(query)
        response[2] |= 0x80
        check(no_reply(srv.dns_port, bytes(response)), "response packet dropped")
        check(srv.wait_for("dns: dropped", 1) is not None, "log: dropped packet")
        status, count, _ = dig(srv.dns_port, "iotc.kuailece.cn", "A")
        check(status == "NOERROR" and count == 1, "still answering after malformed packets")
    finally:
        srv.stop()
    return check.report()


if __name__ == "__main__":
    sys.exit(main())
