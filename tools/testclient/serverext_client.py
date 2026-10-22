#!/usr/bin/env python3
"""Test client for the openbf2-server-ext bot channel (docs/protocol.md v1).

Python 3 standard library only, so it runs unchanged on Windows and in WSL with no build step.

Normal mode: connect, Hello, CreateBot, wait until the bot is alive, EnterVehicle(nearest, seat 0),
then fly a scripted sequence that ends in a hover held by a crude PID on collective. Prints the
bot's TickState lines. Survives deaths (re-enters) and map changes (recreates the bot).

  python serverext_client.py --hover-seconds 60
  python serverext_client.py --stop-sending-after 20 --exit-after 30     # watchdog
  python serverext_client.py --fuzz                                      # hostile input

v1 scenarios (--scenario): walk, buttons, blockpad, allblocked. See docs/findings/module-v1.md.
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
SPAWN_BLOCKED, RESTART_ROUND = 0x0107, 0x0108
SET_INPUT, TICK_STATE, VEHICLE_LIST, ERROR = 0x0201, 0x0301, 0x0302, 0xFFFF
NAMES = {HELLO: "Hello", WELCOME: "Welcome", CREATE_BOT: "CreateBot", BOT_CREATED: "BotCreated",
         ENTER_VEHICLE: "EnterVehicle", REMOVE_BOT: "RemoveBot", ACK: "Ack", BOT_REMOVED: "BotRemoved",
         SPAWN_BLOCKED: "SpawnBlocked", RESTART_ROUND: "RestartRound", SET_INPUT: "SetInput",
         TICK_STATE: "TickState", VEHICLE_LIST: "VehicleList", ERROR: "Error"}
PROTOCOL_VERSION = 1
ENTRY = struct.Struct("<BBBBI3f3f3f3ff")
VEHICLE_TAIL = struct.Struct("<BB3fffBI")   # after the u32 id and the template string

# SetInput button bits (docs/findings/module-v1.md, Q11).
BTN_FIRE, BTN_ALT_FIRE, BTN_FLARE, BTN_JUMP, BTN_SPRINT, BTN_CROUCH, BTN_PRONE, BTN_RELOAD = (1 << i for i in range(8))
BTN_TOGGLE_CAMERA = 1 << 8
BTN_CAMERA_MODE = [1 << 9, 1 << 10, 1 << 11, 1 << 12]      # PICameraMode1..4
BTN_WEAPON = [1 << (13 + i) for i in range(9)]             # PIWeaponSelect1..9
BTN_TOGGLE_WEAPON, BTN_FIRE_RATE = 1 << 22, 1 << 23
BTN_POSITION = [1 << (24 + i) for i in range(8)]           # PIPositionSelect1..8 (F1..F8)


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


def set_input(pid, tick, pitch, roll, coll, pedals, buttons=0, look_yaw=0.0, look_pitch=0.0):
    """v1 SetInput. On foot: coll = walk forward/back, pedals = strafe, look_yaw = turn."""
    return frame(SET_INPUT, struct.pack("<BIffffffI", pid, tick, pitch, roll, coll, pedals, look_yaw, look_pitch, buttons))


def parse_vehiclelist(p):
    r = Reader(p)
    tick, n = r.take("<I"), r.take("<H")
    out = []
    for _ in range(n):
        vid = r.take("<I")
        name = r.str()
        v = VEHICLE_TAIL.unpack_from(p, r.p)
        r.p += VEHICLE_TAIL.size
        out.append(dict(id=vid, name=name, team=v[0], flags=v[1], pos=v[2:5], yaw=v[5], health=v[6], seats=v[7],
                        occupied=v[8]))
    return tick, out


def clamp(v, lo=-1.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def hello(c, name="testclient", version=PROTOCOL_VERSION):
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
    vlists = 0
    last_vl_log = 0.0

    while True:
        now = time.time()
        if args.exit_after and now - start > args.exit_after:
            log(f"exit after {args.exit_after} s; {states} TickStates, {gaps} tick gaps, {vlists} VehicleLists")
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
        elif t == VEHICLE_LIST:
            vtick, vs = parse_vehiclelist(p)
            vlists += 1
            if time.time() - last_vl_log >= args.vehicle_log_every:
                last_vl_log = time.time()
                log(f"VehicleList #{vlists} tick {vtick}: {len(vs)} vehicles, "
                    f"{sum(1 for v in vs if v['flags'] & 1)} destroyed, {sum(1 for v in vs if v['occupied'])} occupied")
        elif t == SPAWN_BLOCKED:
            log("SpawnBlocked player %d after %d s" % struct.unpack("<BH", p))
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


# ------------------------------------------------------------------------------------- v1 scenarios

def wrap180(a):
    return (a + 540.0) % 360.0 - 180.0


def flat_dist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def bearing(frm, to):
    """Yaw in the protocol's convention (0 = north = +z, 90 = east = +x) from frm towards to."""
    return math.degrees(math.atan2(to[0] - frm[0], to[2] - frm[2]))


class Client:
    """A v1 session that keeps the latest TickState and VehicleList, and sends one SetInput per bot
    per TickState from self.inputs (a bot -> dict of axes). Buttons in self.pulses go out once."""

    def __init__(self, args, name):
        self.a = args
        self.c = Conn(args.host, args.port)
        self.welcome_tick = hello(self.c, name)
        self.req = 0
        self.tick = None
        self.states = {}          # bot id -> latest entry (own bots)
        self.others = []
        self.vehicles = {}        # net id -> entry
        self.vl_count = 0
        self.inputs = {}          # bot id -> dict(pitch, roll, coll, ped, look_yaw, look_pitch, buttons)
        self.pulses = {}          # bot id -> buttons for the next SetInput only
        self.replies = {}         # request id -> (kind, data)
        self.blocked = []         # (time, bot, seconds)
        self.removed = []
        self.results = []

    def next_req(self):
        self.req += 1
        return self.req

    def send_req(self, mtype, body_after_req):
        rid = self.next_req()
        self.c.send(frame(mtype, struct.pack("<I", rid) + body_after_req))
        return rid

    def pump(self, timeout=0.2):
        f = self.c.recv_frame(timeout=timeout)
        if f is None:
            return None
        t, p = f
        if t == TICK_STATE:
            tick, _, entries = parse_tickstate(p)
            self.tick = tick
            self.others = []
            for e in entries:
                if e["flags"] & 2:
                    self.states[e["id"]] = e
                else:
                    self.others.append(e)
            for bot, cmd in list(self.inputs.items()):
                btn = cmd.get("buttons", 0) | self.pulses.pop(bot, 0)
                self.c.send(set_input(bot, tick, cmd.get("pitch", 0.0), cmd.get("roll", 0.0), cmd.get("coll", 0.0),
                                      cmd.get("ped", 0.0), btn, cmd.get("look_yaw", 0.0), cmd.get("look_pitch", 0.0)))
        elif t == VEHICLE_LIST:
            _, vs = parse_vehiclelist(p)
            self.vehicles = {v["id"]: v for v in vs}
            self.vl_count += 1
        elif t == BOT_CREATED:
            rid, pid = struct.unpack("<IB", p)
            self.replies[rid] = ("created", pid)
        elif t == ACK:
            self.replies[struct.unpack("<I", p)[0]] = ("ack", None)
        elif t == ERROR:
            rid, code, msg = parse_error(p)
            self.replies[rid] = ("error", (code, msg))
            if rid == 0:
                log(f"Error (no request) code {code}: {msg}")
        elif t == SPAWN_BLOCKED:
            pid, secs = struct.unpack("<BH", p)
            self.blocked.append((time.time(), pid, secs))
            log(f"SpawnBlocked player {pid} after {secs} s")
        elif t == BOT_REMOVED:
            pid, reason = struct.unpack("<BB", p)
            self.removed.append((pid, reason))
            log(f"BotRemoved player {pid} reason {reason}")
        return t

    def run_for(self, seconds, until=None):
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.2)
            if until and until():
                return True
        return False

    def wait_reply(self, rid, timeout=5.0):
        self.run_for(timeout, lambda: rid in self.replies)
        return self.replies.pop(rid, ("timeout", None))

    def create_bot(self, name, team):
        rid = self.send_req(CREATE_BOT, pstr(name) + struct.pack("<B", team))
        kind, data = self.wait_reply(rid)
        if kind != "created":
            raise RuntimeError(f"CreateBot {name}: {kind} {data}")
        self.inputs[data] = {}
        log(f"created bot {data} '{name}' team {team}")
        return data

    def enter(self, bot, vehicle=0, seat=0):
        rid = self.send_req(ENTER_VEHICLE, struct.pack("<BIB", bot, vehicle, seat))
        return self.wait_reply(rid)

    def remove(self, bot):
        rid = self.send_req(REMOVE_BOT, struct.pack("<B", bot))
        self.inputs.pop(bot, None)
        return self.wait_reply(rid)

    def restart(self):
        rid = self.send_req(RESTART_ROUND, b"")
        return self.wait_reply(rid)

    def alive(self, bot):
        e = self.states.get(bot)
        return e is not None and bool(e["flags"] & 1)

    def in_vehicle(self, bot):
        e = self.states.get(bot)
        return e is not None and bool(e["flags"] & 1) and bool(e["flags"] & 4)

    def wait_alive(self, bot, timeout=30.0):
        return self.run_for(timeout, lambda: self.alive(bot))

    def check(self, label, ok):
        self.results.append((label, bool(ok)))
        log(("PASS " if ok else "FAIL ") + label)
        return ok

    def finish(self):
        for bot in list(self.inputs):
            self.remove(bot)
        self.run_for(1.0)
        self.c.close()
        bad = sum(1 for _, ok in self.results if not ok)
        print(f"scenario: {len(self.results) - bad}/{len(self.results)} checks passed", flush=True)
        return 1 if bad else 0


def walk_to(cl, bot, target_pos, stop_at, timeout=120.0, label="walk"):
    """Turns with look yaw and walks with collective (forward) until within stop_at metres."""
    start = time.time()
    best = None
    best_t = start
    strafe_until = 0.0
    while time.time() - start < timeout:
        cl.pump(0.2)
        e = cl.states.get(bot)
        if not e or not (e["flags"] & 1) or (e["flags"] & 4):
            cl.inputs[bot] = {}
            if e and not (e["flags"] & 1):
                log(f"{label}: bot died")
                return False
            continue
        d = flat_dist(e["pos"], target_pos)
        if d <= stop_at:
            cl.inputs[bot] = {}
            log(f"{label}: arrived, {d:.1f} m from the target after {time.time() - start:.1f} s")
            return True
        err = wrap180(bearing(e["pos"], target_pos) - e["ypr"][0])
        look = clamp(err / 15.0)          # look yaw 1.0 is about 5 degrees per tick
        fwd = 1.0 if abs(err) < 25 else 0.3
        ped = 0.0
        if best is None or d < best - 0.5:
            best, best_t = d, time.time()
        elif time.time() - best_t > 4.0 and time.time() > strafe_until:
            strafe_until = time.time() + 1.5   # stuck: side-step
            best_t = time.time()
            log(f"{label}: no progress at {d:.1f} m; side-stepping")
        if time.time() < strafe_until:
            ped = 1.0
        cl.inputs[bot] = dict(coll=fwd, ped=ped, look_yaw=look)
    cl.inputs[bot] = {}
    log(f"{label}: timeout")
    return False


def free_seat0(v, team):
    return not (v["flags"] & 1) and v["team"] in (0, team) and v["seats"] > 0 and not (v["occupied"] & 1)


def run_walk(args):
    """Spawn on foot, check enterRange enforcement and the strafe sign, walk to a heli more than
    --min-walk metres away using VehicleList, then enter it."""
    cl = Client(args, "serverext testclient walk")
    bot = cl.create_bot(args.name, args.team)
    cl.check("bot spawned", cl.wait_alive(bot))
    cl.run_for(2.0, lambda: cl.vl_count > 0)
    cl.check(f"VehicleList received ({len(cl.vehicles)} vehicles)", cl.vl_count > 0 and cl.vehicles)
    me = cl.states[bot]
    for v in sorted(cl.vehicles.values(), key=lambda v: flat_dist(v["pos"], me["pos"])):
        log(f"  vehicle {v['id']:5d} {v['name']:24s} team {v['team']} flags {v['flags']} hp {v['health']:.2f} "
            f"seats {v['seats']} occ {v['occupied']:#x} dist {flat_dist(v['pos'], me['pos']):6.1f}")

    # enterRange: an explicit id beyond it is refused with Error 5.
    far = [v for v in cl.vehicles.values() if free_seat0(v, args.team) and flat_dist(v["pos"], me["pos"]) > 40]
    if far:
        v = min(far, key=lambda v: flat_dist(v["pos"], me["pos"]))
        kind, data = cl.enter(bot, v["id"], 0)
        cl.check(f"EnterVehicle {v['name']} {flat_dist(v['pos'], me['pos']):.0f} m away refused: {kind} {data}",
                 kind == "error" and data[0] == 5)

    # Strafe sign: pedals +0.6 for 1 s from standstill.
    cl.inputs[bot] = {}
    cl.run_for(1.0)
    p0 = cl.states[bot]["pos"]
    yaw0 = cl.states[bot]["ypr"][0]
    cl.inputs[bot] = dict(ped=0.6)
    cl.run_for(1.0)
    cl.inputs[bot] = {}
    cl.run_for(0.5)
    p1 = cl.states[bot]["pos"]
    yr = math.radians(yaw0)
    right = (p1[0] - p0[0]) * math.cos(yr) - (p1[2] - p0[2]) * math.sin(yr)
    fwd = (p1[0] - p0[0]) * math.sin(yr) + (p1[2] - p0[2]) * math.cos(yr)
    log(f"strafe +0.6 for 1 s: {right:+.2f} m right, {fwd:+.2f} m forward (yaw {yaw0:.1f})")
    cl.check("pedals + strafes the soldier sideways", abs(right) > 0.5 and abs(right) > abs(fwd))
    log(f"MEASURED: pedals + strafes {'RIGHT' if right > 0 else 'LEFT'}")

    # Turn sign: look yaw +0.5 while walking slowly for 1 s.
    cl.inputs[bot] = dict(coll=0.2, look_yaw=0.5)
    cl.run_for(1.0)
    cl.inputs[bot] = {}
    cl.run_for(0.5)
    dyaw = wrap180(cl.states[bot]["ypr"][0] - yaw0)
    log(f"look yaw +0.5 for 1 s while walking: yaw {dyaw:+.1f} degrees")
    cl.check("look yaw + turns the soldier", abs(dyaw) > 10)
    log(f"MEASURED: look yaw + turns {'RIGHT (yaw increases)' if dyaw > 0 else 'LEFT (yaw decreases)'}")

    me = cl.states[bot]
    cands = [v for v in cl.vehicles.values()
             if free_seat0(v, args.team) and flat_dist(v["pos"], me["pos"]) > args.min_walk and "ahe_" in v["name"]]
    if not cands:
        cands = [v for v in cl.vehicles.values() if free_seat0(v, args.team) and flat_dist(v["pos"], me["pos"]) > args.min_walk]
    if not cl.check(f"a free vehicle more than {args.min_walk:.0f} m away ({len(cands)} candidates)", cands):
        return cl.finish()
    target = min(cands, key=lambda v: flat_dist(v["pos"], me["pos"]))
    log(f"walking to {target['name']} id {target['id']} at {flat_dist(target['pos'], me['pos']):.1f} m")
    arrived = walk_to(cl, bot, target["pos"], 7.0)
    cl.check(f"walked to within 7 m of {target['name']}", arrived)
    kind, data = cl.enter(bot, target["id"], 0)
    cl.check(f"EnterVehicle {target['id']} seat 0 after walking: {kind} {data}", kind == "ack")
    cl.run_for(1.0)
    e = cl.states.get(bot)
    cl.check("TickState shows the bot in that vehicle, seat 0",
             e and e["flags"] & 4 and e["root"] == target["id"] and e["seat"] == 0)
    return cl.finish()


def run_buttons(args):
    """Fire on foot at an empty vehicle and watch its health in VehicleList; then, seated in a heli,
    pulse the camera keys. The camera effects are logged by the module (bf2.log)."""
    cl = Client(args, "serverext testclient buttons")
    bot = cl.create_bot(args.name, args.team)
    cl.check("bot spawned", cl.wait_alive(bot))
    cl.run_for(2.0, lambda: cl.vl_count > 0)
    me = cl.states[bot]
    empty = [v for v in cl.vehicles.values() if not v["occupied"] and not (v["flags"] & 1) and v["health"] > 0
             and 5 < flat_dist(v["pos"], me["pos"]) < 40]
    if cl.check(f"an empty vehicle within 40 m to shoot at ({len(empty)})", empty):
        target = min(empty, key=lambda v: flat_dist(v["pos"], me["pos"]))
        log(f"target {target['name']} id {target['id']} hp {target['health']:.3f} at {flat_dist(target['pos'], me['pos']):.1f} m")
        # Face it. Standing still, look yaw turns the head first (Q10), so creep forward slowly
        # while turning: then the body turns directly.
        t0 = time.time()
        while time.time() - t0 < 10:
            cl.pump(0.2)
            e = cl.states[bot]
            err = wrap180(bearing(e["pos"], target["pos"]) - e["ypr"][0])
            if abs(err) < 2:
                break
            cl.inputs[bot] = dict(coll=0.15, look_yaw=clamp(err / 20.0, -0.5, 0.5))
        cl.inputs[bot] = {}
        cl.run_for(1.0)
        e = cl.states[bot]
        log(f"facing error {wrap180(bearing(e['pos'], target['pos']) - e['ypr'][0]):+.1f} degrees")
        hp0 = cl.vehicles[target["id"]]["health"]
        cl.inputs[bot] = dict(buttons=BTN_FIRE)
        cl.run_for(4.0)
        cl.inputs[bot] = {}
        cl.run_for(2.5)
        hp1 = cl.vehicles.get(target["id"], {}).get("health", hp0)
        log(f"target health {hp0:.4f} -> {hp1:.4f} after 4 s of fire")
        cl.check(f"fire hit the empty {target['name']} (health {hp0:.4f} -> {hp1:.4f})", hp1 < hp0)

    # Seat in a heli and pulse the camera keys.
    me = cl.states[bot]
    helis = [v for v in cl.vehicles.values() if free_seat0(v, args.team) and "ahe_" in v["name"]
             and flat_dist(v["pos"], me["pos"]) < 30]
    if cl.check(f"a free heli within 30 m ({len(helis)})", helis):
        h = min(helis, key=lambda v: flat_dist(v["pos"], me["pos"]))
        if flat_dist(h["pos"], me["pos"]) > 25:
            walk_to(cl, bot, h["pos"], 10.0)
        kind, data = cl.enter(bot, h["id"], 0)
        cl.check(f"entered {h['name']}: {kind} {data}", kind == "ack")
        cl.run_for(1.0)
        for label, b in [("toggle camera", BTN_TOGGLE_CAMERA), ("toggle camera", BTN_TOGGLE_CAMERA),
                         ("camera mode 1", BTN_CAMERA_MODE[0]), ("camera mode 2", BTN_CAMERA_MODE[1]),
                         ("camera mode 3", BTN_CAMERA_MODE[2]), ("camera mode 4", BTN_CAMERA_MODE[3]),
                         ("camera mode 1", BTN_CAMERA_MODE[0])]:
            log(f"pulse {label} (bit {b.bit_length() - 1}) on tick {cl.tick}")
            cl.pulses[bot] = b
            cl.run_for(1.0)
        log("camera key effects: see the module's 'camera view mode' lines in bf2.log")
        # Seat change F2 (bit 25): moves the bot to seat 1 if the heli has one.
        cl.pulses[bot] = BTN_POSITION[1]
        cl.run_for(1.0)
        e = cl.states.get(bot)
        log(f"after F2: seat {e['seat'] if e else '?'}")
        cl.check("F2 moved the bot to seat 1", e and e["seat"] == 1)
    return cl.finish()


def standing_bots_until(cl, args, stop):
    """Adds standing bots on args.team one at a time until stop(cl, bot) is true for the newest."""
    bots = []
    for i in range(args.max_bots):
        bot = cl.create_bot(f"{args.name} {i}", args.team)
        bots.append(bot)
        spawned = cl.run_for(args.spawn_wait, lambda: cl.alive(bot) or (cl.blocked and cl.blocked[-1][1] == bot))
        e = cl.states.get(bot)
        where = ("in vehicle %d seat %d" % (e["root"], e["seat"])) if cl.in_vehicle(bot) else (
            "on foot at %.1f %.1f %.1f" % tuple(e["pos"]) if cl.alive(bot) else "NOT spawned")
        log(f"bot {bot}: {where}")
        if stop(cl, bot):
            return bots, bot
    return bots, None


def run_blockpad(args):
    """Fills the team's pad with standing bots, then shows the next bot falling back. On
    dalian_2_v_2 the only fallback is the in-heli enter-on-spawn point."""
    cl = Client(args, "serverext testclient blockpad")
    bots, fb = standing_bots_until(cl, args, lambda c, b: c.in_vehicle(b) or not c.alive(b))
    if not cl.check(f"a bot fell back after {len(bots) - 1} standing bots", fb is not None and cl.alive(fb)):
        return cl.finish()
    e = cl.states[fb]
    cl.check(f"fallback bot {fb} is in a vehicle (root {e['root']} seat {e['seat']})", cl.in_vehicle(fb))
    cl.run_for(10.0)
    cl.check("fallback bot still alive and seated after 10 s", cl.in_vehicle(fb))
    for v in cl.vehicles.values():
        if v["id"] == e["root"]:
            log(f"fallback vehicle: {v['name']} hp {v['health']:.2f} occupied {v['occupied']:#x}")
    return cl.finish()


def run_allblocked(args):
    """Adds bots until one gets SpawnBlocked (pad full, in-heli seats full), then RestartRound."""
    cl = Client(args, "serverext testclient allblocked")
    bots, last = standing_bots_until(cl, args, lambda c, b: bool(c.blocked) and c.blocked[-1][1] == b)
    if not cl.check(f"SpawnBlocked after {len(bots)} bots", last is not None):
        return cl.finish()
    t, pid, secs = cl.blocked[-1]
    cl.check(f"SpawnBlocked seconds >= spawnBlockedSeconds ({secs})", secs >= 15)
    n0 = len(cl.blocked)
    cl.run_for(11.0)
    cl.check(f"SpawnBlocked repeated within 11 s ({len(cl.blocked) - n0} more)", len(cl.blocked) > n0)
    if args.restart == "none":
        return cl.finish()
    kind, data = cl.restart()
    log(f"RestartRound: {kind} {data}")
    if args.restart == "refused":
        cl.check("RestartRound refused with Error 6 (allowRoundRestart 0)", kind == "error" and data[0] == 6)
        return cl.finish()
    cl.check("RestartRound acknowledged", kind == "ack")
    kind2, data2 = cl.restart()
    cl.check(f"second RestartRound within 60 s refused: {kind2} {data2}", kind2 == "error" and data2[0] == 6)
    cl.run_for(3.0)
    dead = [b for b in bots if not cl.alive(b)]
    log(f"3 s after the restart: {len(dead)} of {len(bots)} bots dead")
    cl.run_for(20.0)
    alive = [b for b in bots if cl.alive(b)]
    log(f"23 s after the restart: {len(alive)} of {len(bots)} bots alive")
    cl.check("bots respawned after the restart", len(alive) > 0)
    return cl.finish()


def run_wreck(args):
    """Crash a heli (climb, then remove its pilot), then check that EnterVehicle refuses the wreck by
    id and that EnterVehicle(0) does not pick it."""
    cl = Client(args, "serverext testclient wreck")
    a = cl.create_bot(args.name + " pilot", args.team)
    cl.check("pilot spawned", cl.wait_alive(a))
    kind, data = cl.enter(a, 0, 0)
    if not cl.check(f"pilot entered the nearest heli: {kind} {data}", kind == "ack"):
        return cl.finish()
    cl.run_for(0.5)
    hid = cl.states[a]["root"]
    y0 = cl.states[a]["pos"][1]
    cl.inputs[a] = dict(coll=1.0, pitch=args.wreck_pitch)   # some back-cyclic against the Z10's forward trim
    cl.run_for(25.0, lambda: cl.states[a]["pos"][1] - y0 > args.wreck_climb)
    log(f"heli {hid} climbed {cl.states[a]['pos'][1] - y0:.1f} m; removing the pilot")
    cl.remove(a)
    crashed = cl.run_for(30.0, lambda: hid in cl.vehicles and cl.vehicles[hid]["flags"] & 1)
    v = cl.vehicles.get(hid)
    if not cl.check(f"VehicleList flags heli {hid} destroyed ({v and v['health']})", crashed):
        return cl.finish()
    log(f"wreck at {v['pos'][0]:.1f} {v['pos'][1]:.1f} {v['pos'][2]:.1f}")
    b = cl.create_bot(args.name + " walker", args.team)
    cl.check("second bot spawned", cl.wait_alive(b))
    me = cl.states[b]
    d = flat_dist(me["pos"], v["pos"])
    log(f"second bot {d:.1f} m from the wreck")
    # Destroyed is checked before range, so the explicit id is refused as destroyed at any distance.
    kind, data = cl.enter(b, hid, 0)
    cl.check(f"EnterVehicle(wreck {hid}) refused: {kind} {data}",
             kind == "error" and data[0] == 5 and "destroyed" in data[1])
    if d > 25:
        walk_to(cl, b, v["pos"], 8.0, timeout=30.0)
    e = cl.states.get(b)
    if hid in cl.vehicles and cl.vehicles[hid]["flags"] & 1 and flat_dist(e["pos"], v["pos"]) < 30:
        others = [o for o in cl.vehicles.values() if o["id"] != hid and free_seat0(o, args.team)
                  and flat_dist(o["pos"], e["pos"]) < 30]
        kind, data = cl.enter(b, 0, 0)
        cl.run_for(0.5)
        e = cl.states.get(b)
        log(f"EnterVehicle(0) with the wreck {flat_dist(e['pos'], v['pos']):.1f} m away and {len(others)} other "
            f"free vehicles in range: {kind} {data} root {e['root']}")
        cl.check("EnterVehicle(0) did not pick the wreck", kind == "error" or e["root"] != hid)
    else:
        log("wreck gone or out of range before the EnterVehicle(0) check; not run")
    return cl.finish()


SCENARIOS = {"wreck": run_wreck, "walk": run_walk, "buttons": run_buttons, "blockpad": run_blockpad, "allblocked": run_allblocked}


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
    case("RestartRound before Hello", frame(RESTART_ROUND, struct.pack("<I", 1)), 2, hello_first=False)
    case("Hello version 0 (v0 client)", frame(HELLO, struct.pack("<H", 0) + pstr("x")), 1, hello_first=False)
    case("Hello version 2", frame(HELLO, struct.pack("<H", 2) + pstr("x")), 1, hello_first=False)
    case("unknown type 0x7777", frame(0x7777, b"abc"), 3)
    case("module-only type Welcome", frame(WELCOME, b""), 3)
    case("oversize length 65537", struct.pack("<IHH", 65537, SET_INPUT, 0), 2)
    case("length 0xFFFFFFFF", struct.pack("<IHH", 0xFFFFFFFF, SET_INPUT, 0), 2)
    case("reserved != 0", frame(SET_INPUT, b"\x00" * 33, reserved=7), 2)
    case("short SetInput", frame(SET_INPUT, b"\x00" * 10), 2)
    case("v0-sized SetInput (25 bytes)", frame(SET_INPUT, b"\x00" * 25), 2)
    case("SetInput one byte long (34)", frame(SET_INPUT, b"\x00" * 34), 2)
    case("short RestartRound", frame(RESTART_ROUND, b"\x01\x02"), 2)
    case("long RestartRound", frame(RESTART_ROUND, b"\x00" * 5), 2)
    case("module-only type VehicleList", frame(VEHICLE_LIST, b"\x00" * 6), 3)
    case("module-only type SpawnBlocked", frame(SPAWN_BLOCKED, b"\x01\x00\x00"), 3)
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
    if args.expect_restart_refused:
        want_error("RestartRound with allowRoundRestart 0", frame(RESTART_ROUND, struct.pack("<I", 54)), 6)

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
        want_error("SetInput look yaw NaN", set_input(bot, 0, 0, 0, 0, 0, 0, float("nan"), 0), 7)
        want_error("SetInput look pitch -inf", set_input(bot, 0, 0, 0, 0, 0, 0, 0, float("-inf")), 7)
        # Clamped axes; every button bit for one tick (camera keys on foot are ignored by the engine;
        # no PIUse bit exists). Then neutral again.
        c.send(set_input(bot, 0, 5.0, -5.0, 1e30, -1e30, 0xFFFFFFFF, 99.0, -99.0))
        c.send(set_input(bot, 0, 0, 0, 0, 0))
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

    # Finally: the server is still ticking at 30 Hz, and VehicleList arrives once a second.
    c = Conn(H, P)
    hello(c, "fuzz-check")
    ticks = []
    vls = []
    end = time.time() + 3
    while time.time() < end:
        f = c.recv_frame(timeout=1)
        if f and f[0] == TICK_STATE:
            ticks.append((time.time(), parse_tickstate(f[1])[0]))
        elif f and f[0] == VEHICLE_LIST:
            vls.append(parse_vehiclelist(f[1]))
    c.close()
    results.append((f"VehicleList: {len(vls)} in 3 s, {len(vls[-1][1]) if vls else 0} vehicles", 2 <= len(vls) <= 4))
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
    ap.add_argument("--expect-restart-refused", action="store_true", help="fuzz: the server has allowRoundRestart 0")
    ap.add_argument("--vehicle-log-every", type=float, default=60.0, help="seconds between VehicleList summaries")
    ap.add_argument("--scenario", choices=["hover", "walk", "buttons", "blockpad", "allblocked", "wreck"], default="hover")
    ap.add_argument("--max-bots", type=int, default=12, help="blockpad/allblocked: bots to add at most")
    ap.add_argument("--restart", choices=["none", "refused", "allowed"], default="none",
                    help="allblocked: send RestartRound after SpawnBlocked and expect this outcome")
    ap.add_argument("--min-walk", type=float, default=15.0, help="walk: the heli must be at least this far")
    ap.add_argument("--wreck-climb", type=float, default=10.0, help="wreck: metres to climb before the crash")
    ap.add_argument("--wreck-pitch", type=float, default=-0.3, help="wreck: cyclic pitch during the climb")
    ap.add_argument("--spawn-wait", type=float, default=25.0, help="blockpad/allblocked: seconds to wait for a spawn")
    args = ap.parse_args()
    try:
        if args.fuzz:
            return run_fuzz(args)
        if args.scenario != "hover":
            return SCENARIOS[args.scenario](args)
        return run_bot(args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
