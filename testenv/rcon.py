#!/usr/bin/env python3
"""Sends console commands to a test server over ModManager RCON (mm_rcon). Python 3 stdlib only.

  python rcon.py --port 4723 --password <pw> "exec admin.nextLevel 0" "exec admin.runNextLevel"

Login is ModManager's digest: md5(seed + password), with the seed from the banner. Each command is
sent with the \\x02 prefix, so the reply ends with \\x04. Refuses the ports of other environments.
"""

import argparse
import hashlib
import re
import socket
import sys


def read_until(s, marker, limit=1 << 20):
    buf = b""
    while marker not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
        if len(buf) > limit:
            break
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4723)
    ap.add_argument("--password", required=True)
    ap.add_argument("commands", nargs="+")
    a = ap.parse_args()
    if a.port in (4711, 4721):
        sys.exit("refusing RCON port %d (another environment's)" % a.port)

    s = socket.create_connection((a.host, a.port), timeout=10)
    banner = read_until(s, b"\n\n").decode("latin-1")
    m = re.search(r"Digest seed: (\S+)", banner)
    if not m:
        sys.exit("no digest seed in banner: %r" % banner)
    digest = hashlib.md5((m.group(1) + a.password).encode("latin-1")).hexdigest()
    s.sendall(b"\x02login " + digest.encode() + b"\n")
    reply = read_until(s, b"\x04").decode("latin-1").strip("\x04\n ")
    print("login:", reply)
    if "successful" not in reply:
        sys.exit(1)
    for c in a.commands:
        s.sendall(b"\x02" + c.encode("latin-1") + b"\n")
        print("%s -> %s" % (c, read_until(s, b"\x04").decode("latin-1").strip("\x04\n ")))
    s.close()


if __name__ == "__main__":
    main()
