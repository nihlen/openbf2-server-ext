#!/usr/bin/env python3
"""Test client for the openbf2-server-ext bot channel (docs/protocol.md v0).

Python 3 standard library only, so it runs unchanged on Windows and in WSL with no build step.

Normal mode: connect, Hello, CreateBot, wait until the bot is alive, EnterVehicle(nearest, seat 0),
then fly a scripted sequence that ends in a hover held by a crude PID on collective. Prints the
bot's TickState lines. Survives deaths (re-enters) and map changes (recreates the bot).

  python serverext_client.py --hover-seconds 60
  python serverext_client.py --stop-sending-after 20 --exit-after 30     # watchdog
  python serverext_client.py --fuzz                                      # hostile input
"""

import argparse
import math
import random
import socket
import struct
import sys
import time

HELLO, WELCOME = 0x0001, 0x0002
CREATE_BOT, BOT_CREATED, ENTER_VEHICLE, REMOVE_BOT, ACK, BOT_REMOVED = 0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106
SET_INPUT, TICK_STATE, ERROR = 0x0201, 0x0301, 0xFFFF
NAMES = {HELLO: "Hello", WELCOME: "Welcome", CREATE_BOT: "CreateBot", BOT_CREATED: "BotCreated",
         ENTER_VEHICLE: "EnterVehicle", REMOVE_BOT: "RemoveBot", ACK: "Ack", BOT_REMOVED: "BotRemoved",
         SET_INPUT: "SetInput", TICK_STATE: "TickState", ERROR: "Error"}
ENTRY = struct.Struct("<BBBBI3f3f3f3ff")


def ts():
    return time.strftime("%H:%M:%S")


def log(*a):
    print(ts(), *a, flush=True)


def pstr(s):
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def frame(mtype, payload=b"", reserved=0):
    return struct.pack("<IHH", len(payload), mtype, reserved) + payload


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def take(self, fmt):
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += struct.calcsize(fmt)
        return v if len(v) > 1 else v[0]

    def str(self):
        n = self.take("<H")
        s = self.d[self.p:self.p + n].decode("utf-8", "replace")
        self.p += n
        return s


class Conn:
    def __init__(self, host, port, timeout=5.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def recv_frame(self, timeout=None):
        """Returns (type, payload), or None on timeout. Raises ConnectionError when closed."""
        if timeout is not None:
            self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 8:
                n, t, _ = struct.unpack_from("<IHH", self.buf)
                if len(self.buf) >= 8 + n:
                    p = self.buf[8:8 + n]
                    self.buf = self.buf[8 + n:]
                    return t, p
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def parse_error(p):
    r = Reader(p)
    return r.take("<I"), r.take("<H"), r.str()


def parse_tickstate(p):
    r = Reader(p)
    tick, t, n = r.take("<I"), r.take("<d"), r.take("<B")
    entries = []
    for _ in range(n):
        v = ENTRY.unpack_from(p, r.p)
        r.p += ENTRY.size
        entries.append(dict(id=v[0], flags=v[1], team=v[2], seat=v[3], root=v[4], pos=v[5:8], ypr=v[8:11],
                            vel=v[11:14], angvel=v[14:17], health=v[17]))
    return tick, t, entries


def set_input(pid, tick, pitch, roll, coll, pedals, buttons=0):
    return frame(SET_INPUT, struct.pack("<BIffffI", pid, tick, pitch, roll, coll, pedals, buttons))


def clamp(v, lo=-1.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def hello(c, name="testclient", version=0):
    c.send(frame(HELLO, struct.pack("<H", version) + pstr(name)))
    while True:
        f = c.recv_frame(timeout=5)
        if f is None:
            raise RuntimeError("no Welcome within 5 s")
        t, p = f
        if t == WELCOME:
            r = Reader(p)
            v, rate, tick, m, mode = r.take("<H"), r.take("<f"), r.take("<I"), r.str(), r.str()
            log(f"Welcome: protocol {v}, {rate:.0f} Hz, tick {tick}, map '{m}', mode '{mode}'")
            return tick
        if t == ERROR:
            raise RuntimeError("Hello refused: %r" % (parse_error(p),))


# ------------------------------------------------------------------------------------- bot flight

class Pilot:
    """Script: spin-up with collective 0, then hold a target height above the pad with a PID on
    collective, plus (optionally) short cyclic/pedal pulses to show each axis's response."""

    def __init__(self, args):
        self.a = args
        self.reset()

    def reset(self):
        self.pad_y = None
        self.t0 = None
        self.integ = 0.0
        self.hover_since = None
        self.last = (0.0, 0.0, 0.0, 0.0)
        self.yaw_hold = None
        self.prev_t = None
        self.prev_att = (0.0, 0.0, 0.0)
        self.ip = self.ir = 0.0
        self.ivf = self.ivr = 0.0
        self.diag = (0.0, 0.0)

    def command(self, e, now):
        if self.t0 is None:
            self.t0 = now
            self.pad_y = e["pos"][1]
        t = now - self.t0
        y, vy = e["pos"][1], e["vel"][1]
        yaw, pitch, roll = e["ypr"]
        wx, wy, wz = e["angvel"]
        cyc_p = cyc_r = ped = 0.0
        if t < self.a.spinup:
            coll = 0.0
            phase = "spinup"
        else:
            target = self.pad_y + self.a.hover_height
            err = target - y
            self.integ = clamp(self.integ + err * self.a.ki / 30.0, -0.8, 0.8)
            coll = clamp(self.a.coll0 + self.a.kp * err - self.a.kd * vy + self.integ)
            phase = "hover" if abs(err) < 3.0 else "climb"
            if phase == "hover" and self.hover_since is None:
                self.hover_since = now
            # Station keeping: body-frame velocity -> target attitude -> cyclic, plus heading hold.
            # Signs measured on the Z10 (see docs/findings/module-v0.md): pitch input + = nose down
            # (pitch angle +), roll input + = bank right (roll angle -), pedals + = yaw right.
            if self.a.hold:
                cyc_p, cyc_r, ped = self.station_keep(e, now)
            if self.a.blips and self.hover_since is not None:
                ht = now - self.hover_since
                for start, axis, val in ((10, "ped", 0.3), (15, "ped", -0.3), (20, "roll", 0.2), (25, "roll", -0.2),
                                         (30, "pitch", 0.2), (35, "pitch", -0.2)):
                    if start <= ht < start + 0.5:
                        phase = f"blip {axis} {val:+.1f}"
                        if axis == "ped":
                            ped = val
                        elif axis == "roll":
                            cyc_r = val
                        else:
                            cyc_p = val
        self.last = (cyc_p, cyc_r, coll, ped)
        return self.last, phase

    def station_keep(self, e, now):
        a = self.a
        yaw, pitch, roll = e["ypr"]
        vx, _, vz = e["vel"]
        if self.yaw_hold is None:
            self.yaw_hold = yaw
        yr = math.radians(yaw)
        v_fwd = vx * math.sin(yr) + vz * math.cos(yr)
        v_right = vx * math.cos(yr) - vz * math.sin(yr)
        dt = (now - self.prev_t) if self.prev_t else 1 / 30.0
        dt = max(dt, 1 / 60.0)
        dp = (pitch - self.prev_att[0]) / dt if self.prev_t else 0.0
        dr = (roll - self.prev_att[1]) / dt if self.prev_t else 0.0
        dyaw = ((yaw - self.prev_att[2] + 540) % 360 - 180) / dt if self.prev_t else 0.0
        self.prev_t, self.prev_att = now, (pitch, roll, yaw)
        # Moving forward -> nose up (negative pitch); moving right -> bank left (positive roll).
        self.ivf = clamp(self.ivf + v_fwd * dt, -20.0, 20.0)
        self.ivr = clamp(self.ivr + v_right * dt, -20.0, 20.0)
        pitch_des = clamp(-a.kv * v_fwd - a.kvi * self.ivf, -a.max_tilt, a.max_tilt)
        roll_des = clamp(a.kv * v_right + a.kvi * self.ivr, -a.max_tilt, a.max_tilt)
        ep, er = pitch_des - pitch, roll_des - roll
        self.ip = clamp(self.ip + ep * a.att_ki * dt, -0.5, 0.5)
        self.ir = clamp(self.ir + er * a.att_ki * dt, -0.5, 0.5)
        cyc_p = clamp(a.att_kp * ep - a.att_kd * dp + self.ip)
        cyc_r = clamp(-(a.att_kp * er - a.att_kd * dr + self.ir))
        eyaw = (self.yaw_hold - yaw + 540) % 360 - 180
        ped = clamp(a.yaw_kp * eyaw - a.yaw_kd * dyaw)
        self.diag = (v_fwd, v_right)
        return cyc_p, cyc_r, ped


def fmt_entry(e):
    return ("id %3d fl %d team %d seat %3d root %5d pos %8.2f %7.2f %8.2f ypr %7.1f %6.1f %6.1f vel %6.2f %6.2f %6.2f "
            "w %6.3f %6.3f %6.3f hp %5.2f" % ((e["id"], e["flags"], e["team"], e["seat"], e["root"]) + tuple(e["pos"]) +
                                              tuple(e["ypr"]) + tuple(e["vel"]) + tuple(e["angvel"]) + (e["health"],)))


def run_bot(args):
    c = Conn(args.host, args.port)
    hello(c, args.client_name)
    req = [0]

    def next_req():
        req[0] += 1
        return req[0]

    state = "create"
    bot = None
    create_req = enter_req = None
    next_try = 0.0
    pilot = Pilot(args)
    start = time.time()
    hover_ok_since = None
    hover_start_pos = None
    hover_reported = False
    last_print = 0
    max_alt_err = 0.0
    stop_sending = False
    last_tick = None
    gaps = 0
    states = 0

    while True:
        now = time.time()
        if args.exit_after and now - start > args.exit_after:
            log(f"exit after {args.exit_after} s; {states} TickStates, {gaps} tick gaps")
            break
        if args.stop_sending_after and now - start > args.stop_sending_after and not stop_sending:
            stop_sending = True
            log("STOP SENDING SetInput now (watchdog test); still reading state")

        if state == "create" and now >= next_try:
            create_req = next_req()
            c.send(frame(CREATE_BOT, struct.pack("<I", create_req) + pstr(args.name) + struct.pack("<B", args.team)))
            state = "wait_created"
            next_try = now + 2.0

        f = c.recv_frame(timeout=1.0)
        if f is None:
            continue
        t, p = f
        if t == BOT_CREATED:
            rid, pid = struct.unpack("<IB", p)
            log(f"BotCreated req {rid} -> player {pid}")
            bot = pid
            state = "wait_alive"
            pilot.reset()
        elif t == ACK:
            rid = struct.unpack("<I", p)[0]
            log(f"Ack req {rid}")
            if rid == enter_req:
                state = "fly"
                pilot.reset()
        elif t == BOT_REMOVED:
            pid, reason = struct.unpack("<BB", p)
            log(f"BotRemoved player {pid} reason {reason}")
            if pid == bot:
                bot = None
                state = "create"
                next_try = time.time() + 2.0
                hover_ok_since = None
        elif t == ERROR:
            rid, code, msg = parse_error(p)
            log(f"Error req {rid} code {code}: {msg}")
            if rid == create_req and state == "wait_created":
                state = "create"
            elif rid == enter_req and state == "wait_enter":
                state = "wait_alive"
                next_try = time.time() + 1.0
        elif t == TICK_STATE:
            tick, simt, entries = parse_tickstate(p)
            states += 1
            if last_tick is not None and tick != last_tick + 1:
                gaps += 1
            last_tick = tick
            me = next((e for e in entries if e["id"] == bot and e["flags"] & 2), None) if bot is not None else None
            if me is None:
                continue
            alive, inveh = bool(me["flags"] & 1), bool(me["flags"] & 4)
            cmd = (0.0, 0.0, 0.0, 0.0)
            phase = state
            if state == "wait_alive" and alive and not inveh and time.time() >= next_try:
                enter_req = next_req()
                c.send(frame(ENTER_VEHICLE, struct.pack("<IBIB", enter_req, bot, args.vehicle, 0)))
                log(f"bot alive on foot at {me['pos'][0]:.1f} {me['pos'][1]:.1f} {me['pos'][2]:.1f}; EnterVehicle req {enter_req}")
                state = "wait_enter"
            elif state == "fly":
                if not alive or not inveh:
                    log(f"bot left the vehicle (alive {int(alive)}); waiting to re-enter")
                    state = "wait_alive"
                    next_try = time.time() + 1.0
                    hover_ok_since = None
                else:
                    cmd, phase = pilot.command(me, time.time())
                    if phase == "hover" or phase.startswith("blip"):
                        err = abs(me["pos"][1] - (pilot.pad_y + args.hover_height))
                        if hover_ok_since is None:
                            hover_ok_since = time.time()
                            max_alt_err = 0.0
                            hover_start_pos = me["pos"]
                            log(f"hover reached at height {me['pos'][1] - pilot.pad_y:.1f} m above the pad")
                        max_alt_err = max(max_alt_err, err)
                        held = time.time() - hover_ok_since
                        if held >= args.hover_seconds and not hover_reported:
                            hover_reported = True
                            drift = math.hypot(me["pos"][0] - hover_start_pos[0], me["pos"][2] - hover_start_pos[2])
                            log(f"HOVER HELD {held:.0f} s; max altitude error {max_alt_err:.2f} m; "
                                f"horizontal drift {drift:.1f} m; health {me['health']:.2f}")
                            if args.exit_after_hover:
                                break
            if not stop_sending and state in ("fly", "wait_alive", "wait_enter"):
                c.send(set_input(bot, tick, *cmd))
            if tick - last_print >= args.print_every:
                last_print = tick
                print(f"{ts()} tick {tick} t {simt:9.2f} {phase:14s} cmd p{cmd[0]:+.2f} r{cmd[1]:+.2f} "
                      f"c{cmd[2]:+.2f} y{cmd[3]:+.2f} vf{pilot.diag[0]:+5.1f} vr{pilot.diag[1]:+5.1f} | {fmt_entry(me)} | others {len(entries) - 1}", flush=True)

    if bot is not None and not args.no_remove:
        rid = next_req()
        c.send(frame(REMOVE_BOT, struct.pack("<IB", rid, bot)))
        end = time.time() + 3
        while time.time() < end:
            f = c.recv_frame(timeout=0.5)
            if f and f[0] in (ACK, BOT_REMOVED, ERROR):
                log(NAMES[f[0]], f[1].hex())
                if f[0] == BOT_REMOVED:
                    break
    c.close()
    return 0 if (hover_reported or not args.require_hover) else 1


# ------------------------------------------------------------------------------------- fuzz

def expect_error(c, label, want_code=None, timeout=3.0):
    """Reads until an Error and the close; returns a result line."""
    code = None
    try:
        while True:
            f = c.recv_frame(timeout=timeout)
            if f is None:
                return f"{label}: no Error/close within {timeout} s (code so far {code})", False
            if f[0] == ERROR:
                code = parse_error(f[1])[1]
    except (ConnectionError, OSError):
        pass
    ok = code is not None and (want_code is None or code == want_code)
    return f"{label}: Error code {code}, closed", ok


def run_fuzz(args):
    H, P = args.host, args.port
    results = []

    def case(label, payload, want=None, hello_first=True):
        c = Conn(H, P)
        try:
            if hello_first:
                hello(c, "fuzz")
            c.send(payload)
            results.append(expect_error(c, label, want))
        finally:
            c.close()

    case("garbage before Hello", b"\x00\x01\x02\x03garbage!", None, hello_first=False)
    case("SetInput before Hello", set_input(1, 0, 0, 0, 0, 0), 2, hello_first=False)
    case("Hello version 1", frame(HELLO, struct.pack("<H", 1) + pstr("x")), 1, hello_first=False)
    case("unknown type 0x7777", frame(0x7777, b"abc"), 3)
    case("module-only type Welcome", frame(WELCOME, b""), 3)
    case("oversize length 65537", struct.pack("<IHH", 65537, SET_INPUT, 0), 2)
    case("length 0xFFFFFFFF", struct.pack("<IHH", 0xFFFFFFFF, SET_INPUT, 0), 2)
    case("reserved != 0", frame(SET_INPUT, b"\x00" * 25, reserved=7), 2)
    case("short SetInput", frame(SET_INPUT, b"\x00" * 10), 2)
    case("long CreateBot string length", frame(CREATE_BOT, struct.pack("<IH", 1, 5000) + b"ab"), 2)
    case("Hello twice", frame(HELLO, struct.pack("<H", 0) + pstr("x")), 2)
    case("max-size unknown frame", frame(0x4242, b"\xAA" * 65536), 3)

    # Truncated frame, then hang up mid-frame.
    c = Conn(H, P)
    hello(c, "fuzz")
    c.send(struct.pack("<IHH", 1000, CREATE_BOT, 0) + b"\x01" * 10)
    c.close()
    results.append(("truncated frame then hang-up: sent", True))

    # Random noise, many connections.
    rng = random.Random(7)
    closed = 0
    for i in range(args.fuzz_rounds):
        c = Conn(H, P)
        try:
            if i % 2:
                hello(c, "fuzz")
            blob = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 2000)))
            c.send(blob)
            line, ok = expect_error(c, "noise", None, timeout=0.5)
            closed += ok
        except (ConnectionError, OSError):
            closed += 1
        finally:
            c.close()
    results.append((f"{args.fuzz_rounds} random-noise connections: {closed} ended with Error+close", True))

    # Connect and hang up at once, repeatedly.
    for _ in range(100):
        Conn(H, P).close()
    results.append(("100 connect-and-close", True))

    # A second concurrent connection is refused; per-request errors keep the connection.
    c = Conn(H, P)
    hello(c, "fuzz-main")
    c2 = Conn(H, P)
    try:
        got = c2.recv_frame(timeout=2.0)
        results.append((f"second connection: still open after 2 s ({got!r})", False))
    except (ConnectionError, OSError):
        results.append(("second connection: closed by the server", True))
    c2.close()

    def want_error(label, payload, code):
        c.send(payload)
        end = time.time() + 3
        while time.time() < end:
            f = c.recv_frame(timeout=1)
            if f and f[0] == ERROR:
                got = parse_error(f[1])
                results.append((f"{label}: Error code {got[1]} '{got[2]}', connection kept", got[1] == code))
                return
        results.append((f"{label}: no Error", False))

    want_error("SetInput unknown player", set_input(3, 0, 0, 0, 0, 0), 4)
    want_error("CreateBot team 3", frame(CREATE_BOT, struct.pack("<I", 50) + pstr("[BOT] x") + b"\x03"), 7)
    want_error("CreateBot empty name", frame(CREATE_BOT, struct.pack("<I", 51) + pstr("") + b"\x01"), 7)
    want_error("EnterVehicle foreign player", frame(ENTER_VEHICLE, struct.pack("<IBIB", 52, 1, 0, 0)), 4)
    want_error("RemoveBot foreign player", frame(REMOVE_BOT, struct.pack("<IB", 53, 1)), 4)

    # A real bot: NaN, inf and out-of-range inputs, buttons, bad vehicle ids.
    c.send(frame(CREATE_BOT, struct.pack("<I", 60) + pstr("[BOT] fuzz") + b"\x02"))
    bot = None
    end = time.time() + 5
    while time.time() < end and bot is None:
        f = c.recv_frame(timeout=1)
        if f and f[0] == BOT_CREATED:
            bot = struct.unpack("<IB", f[1])[1]
        elif f and f[0] == ERROR:
            results.append(("CreateBot for fuzz: %r" % (parse_error(f[1]),), False))
            break
    if bot is not None:
        want_error("SetInput NaN", set_input(bot, 0, float("nan"), 0, 0, 0), 7)
        want_error("SetInput +inf", set_input(bot, 0, 0, 0, float("inf"), 0), 7)
        c.send(set_input(bot, 0, 5.0, -5.0, 1e30, -1e30, 0xFFFFFFFF))   # clamped, buttons ignored
        want_error("EnterVehicle id 70000", frame(ENTER_VEHICLE, struct.pack("<IBIB", 61, bot, 70000, 0)), 5)
        want_error("EnterVehicle id 65535", frame(ENTER_VEHICLE, struct.pack("<IBIB", 62, bot, 65535, 0)), 5)
        want_error("EnterVehicle seat 200", frame(ENTER_VEHICLE, struct.pack("<IBIB", 63, bot, 0, 200)), 5)
        # Flood: many SetInputs in one burst (latest wins; the queue bound may close the connection).
        burst = set_input(bot, 0, 0.1, 0, 0, 0) * 20000
        try:
            c.send(burst)
            results.append(("20000-frame SetInput burst sent", True))
        except OSError as ex:
            results.append((f"20000-frame burst: {ex}", True))
    c.close()

    # Slow reader: Hello, then do not read for 5 s; the server must drop old TickStates, not queue.
    # Straight after the flood above: a fresh connection must not inherit its backlog.
    try:
        c = Conn(H, P)
        hello(c, "fuzz-slow")
        time.sleep(5)
        c.close()
        results.append(("Hello right after a flood, then slow reader 5 s", True))
    except (RuntimeError, OSError) as ex:
        results.append((f"Hello right after a flood: {ex}", False))

    # Finally: the server is still ticking at 30 Hz.
    c = Conn(H, P)
    hello(c, "fuzz-check")
    ticks = []
    end = time.time() + 3
    while time.time() < end:
        f = c.recv_frame(timeout=1)
        if f and f[0] == TICK_STATE:
            ticks.append((time.time(), parse_tickstate(f[1])[0]))
    c.close()
    if len(ticks) >= 2:
        rate = (ticks[-1][1] - ticks[0][1]) / (ticks[-1][0] - ticks[0][0])
        results.append((f"server still ticking: {len(ticks)} TickStates in 3 s, {rate:.1f} ticks/s", 25 <= rate <= 35))
    else:
        results.append(("server still ticking: NO TickState", False))

    bad = 0
    for line, ok in results:
        print(("PASS " if ok else "FAIL ") + line, flush=True)
        bad += not ok
    print(f"fuzz: {len(results) - bad}/{len(results)} passed", flush=True)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=16700)
    ap.add_argument("--client-name", default="serverext testclient")
    ap.add_argument("--name", default="[BOT] test")
    ap.add_argument("--team", type=int, default=1)
    ap.add_argument("--vehicle", type=int, default=0, help="networkable id, 0 = nearest within 30 m")
    ap.add_argument("--spinup", type=float, default=5.0, help="seconds of collective 0 after entering")
    ap.add_argument("--hover-height", type=float, default=15.0, help="metres above the pad")
    ap.add_argument("--hover-seconds", type=float, default=60.0)
    ap.add_argument("--coll0", type=float, default=0.0, help="collective feed-forward")
    ap.add_argument("--kp", type=float, default=0.08)
    ap.add_argument("--kd", type=float, default=0.25)
    ap.add_argument("--ki", type=float, default=0.03)
    ap.add_argument("--no-hold", dest="hold", action="store_false", help="no velocity/attitude/heading hold")
    ap.add_argument("--kv", type=float, default=1.5, help="degrees of tilt per m/s of drift")
    ap.add_argument("--kvi", type=float, default=0.3, help="degrees of tilt per metre of accumulated drift")
    ap.add_argument("--max-tilt", type=float, default=10.0, help="degrees")
    ap.add_argument("--att-kp", type=float, default=0.06, help="cyclic per degree of attitude error")
    ap.add_argument("--att-kd", type=float, default=0.04, help="cyclic per degree/s")
    ap.add_argument("--att-ki", type=float, default=0.02, help="cyclic per degree*s")
    ap.add_argument("--yaw-kp", type=float, default=0.03)
    ap.add_argument("--yaw-kd", type=float, default=0.03)
    ap.add_argument("--blips", action="store_true", help="short pedal/roll/pitch pulses during the hover")
    ap.add_argument("--print-every", type=int, default=30, help="ticks between TickState lines")
    ap.add_argument("--exit-after", type=float, default=0.0)
    ap.add_argument("--exit-after-hover", action="store_true")
    ap.add_argument("--require-hover", action="store_true")
    ap.add_argument("--stop-sending-after", type=float, default=0.0)
    ap.add_argument("--no-remove", action="store_true", help="do not RemoveBot on exit (tests disconnect cleanup)")
    ap.add_argument("--fuzz", action="store_true")
    ap.add_argument("--fuzz-rounds", type=int, default=300)
    args = ap.parse_args()
    try:
        return run_fuzz(args) if args.fuzz else run_bot(args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
