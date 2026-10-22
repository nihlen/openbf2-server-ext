#!/usr/bin/env python3
"""Stands in for BF2WebAdmin: accepts mm_webadmin's outbound TCP connection and records every line.

Python 3 stdlib only. It never sends anything, so mm_webadmin gets no commands. Each received line
is written with a wall-clock timestamp, tabs shown as ' | ', and the raw line repr'd if it is not
plain Windows-1252 text. Used for Q9 (docs/findings/q5-q9.md).

  python3 wa_recorder.py --port 4303 --out wa.log

Never use 4300 (the BF2WebAdmin local test's port).
"""

import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4303)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if a.port == 4300:
        sys.exit("refusing port 4300 (BF2WebAdmin)")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(4)
    out = open(a.out, "a", encoding="utf-8", buffering=1)

    def note(s):
        out.write("%s.%03d %s\n" % (time.strftime("%H:%M:%S"), int(time.time() * 1000) % 1000, s))

    note("listening on %s:%d" % (a.host, a.port))
    while True:
        conn, peer = srv.accept()
        note("# connection from %s:%d" % peer)
        buf = b""
        while True:
            try:
                chunk = conn.recv(65536)
            except OSError as e:
                note("# recv error %s" % e)
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("cp1252", "replace")
                note(text.replace("\t", " | ").replace("\b", " \\b "))
        if buf:
            note("# partial line at close: %r" % buf)
        note("# connection closed")
        conn.close()


if __name__ == "__main__":
    main()
