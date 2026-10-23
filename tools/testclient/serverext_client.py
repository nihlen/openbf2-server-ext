#!/usr/bin/env python3
"""Test client for the openbf2-server-ext bot channel (docs/protocol.md v3; layouts in docs/findings/module-v2.md
and docs/findings/module-v3.md).

Python 3 standard library only, so it runs unchanged on Windows and in WSL with no build step.

Normal mode: connect, Hello, CreateBot, wait until the bot is alive, EnterVehicle(nearest, seat 0),
then fly a scripted sequence that ends in a hover held by a crude PID on collective. Prints the
bot's TickState lines. Survives deaths (re-enters) and map changes (recreates the bot).

  python serverext_client.py --hover-seconds 60
  python serverext_client.py --stop-sending-after 20 --exit-after 30     # watchdog
  python serverext_client.py --fuzz                                      # hostile input

v1 scenarios (--scenario): walk, buttons, blockpad, allblocked. See docs/findings/module-v1.md.
v2 scenarios: view, weapons, multiconn, projectiles, damage, spot, round, teleport, soak2; the watchdog
neutral is q5 with --q5-neutral module. See docs/findings/module-v2.md.
v3 scenarios: burst (item 1, one-tick presses in superseded SetInputs), tv (items 2 and 3, the gunner's TV
missile launched and steered with SetTargetDirection), soak3 (two bots, the gunner guiding TV missiles
periodically). See docs/findings/module-v3.md.
Q12 scenario: q12 (two crews: a TV launch while the other crew's TV missile flies). See
docs/findings/q12-tv-refusal.md.
Q5/Q9 scenarios: q5 (watchdog neutral in flight), q9 (what the server's Python sees). See
docs/findings/q5-q9.md.
"""

import argparse
import hashlib
import math
import random
import re
import socket
import struct
import sys
import time

HELLO, WELCOME = 0x0001, 0x0002
CREATE_BOT, BOT_CREATED, ENTER_VEHICLE, REMOVE_BOT, ACK, BOT_REMOVED = 0x0101, 0x0102, 0x0103, 0x0104, 0x0105, 0x0106
SPAWN_BLOCKED, RESTART_ROUND = 0x0107, 0x0108
TELEPORT, SPOT = 0x0109, 0x010A
SET_INPUT, TICK_STATE, VEHICLE_LIST, ERROR = 0x0201, 0x0301, 0x0302, 0xFFFF
SET_TARGET_DIRECTION = 0x0202
PROJECTILE_LIST, DAMAGE, KILL, CHAT, ROUND_STATE = 0x0303, 0x0304, 0x0305, 0x0306, 0x0307
NAMES = {HELLO: "Hello", WELCOME: "Welcome", CREATE_BOT: "CreateBot", BOT_CREATED: "BotCreated",
         ENTER_VEHICLE: "EnterVehicle", REMOVE_BOT: "RemoveBot", ACK: "Ack", BOT_REMOVED: "BotRemoved",
         SPAWN_BLOCKED: "SpawnBlocked", RESTART_ROUND: "RestartRound", SET_INPUT: "SetInput",
         TICK_STATE: "TickState", VEHICLE_LIST: "VehicleList", ERROR: "Error", TELEPORT: "Teleport", SPOT: "Spot",
         PROJECTILE_LIST: "ProjectileList", DAMAGE: "Damage", KILL: "Kill", CHAT: "Chat", ROUND_STATE: "RoundState",
         SET_TARGET_DIRECTION: "SetTargetDirection"}
PROTOCOL_VERSION = 3
F_ALIVE, F_OWN, F_VEHICLE, F_MANDOWN, F_SPOTTED = 1, 2, 4, 8, 16
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
    """v2: each entry is the v1 60 bytes, then u8 view mode, u8 weapon count and the weapons."""
    r = Reader(p)
    tick, t, n = r.take("<I"), r.take("<d"), r.take("<B")
    entries = []
    for _ in range(n):
        v = ENTRY.unpack_from(p, r.p)
        r.p += ENTRY.size
        view, wc = r.take("<B"), r.take("<B")
        weapons = []
        for _ in range(wc):
            slot = r.take("<B")
            name = r.str()
            ammo, mags, wflags, reload_left, heat = r.take("<hhBff")
            weapons.append(dict(slot=slot, name=name, ammo=ammo, mags=mags, reloading=bool(wflags & 1),
                                reload_left=reload_left, heat=heat))
        entries.append(dict(id=v[0], flags=v[1], team=v[2], seat=v[3], root=v[4], pos=v[5:8], ypr=v[8:11],
                            vel=v[11:14], angvel=v[14:17], health=v[17], view=view, weapons=weapons))
    if r.p != len(p):
        raise ValueError(f"TickState: {len(p) - r.p} trailing bytes")
    return tick, t, entries


def parse_projectiles(p):
    r = Reader(p)
    tick, n = r.take("<I"), r.take("<B")
    out = []
    for _ in range(n):
        pid = r.take("<I")
        name = r.str()
        owner, team, flags, target = r.take("<BBBI")
        v = r.take("<7f")
        out.append(dict(id=pid, name=name, owner=owner if flags & 4 else None, team=team, guided=bool(flags & 1), own=bool(flags & 2),
                        target=target, pos=v[0:3], vel=v[3:6], ttl=v[6]))
    return tick, out


def parse_damage(p):
    r = Reader(p)
    tick, flags, victim, root = r.take("<IBBI")
    vt = r.str()
    attacker = r.take("<B")
    weapon, source = r.str(), r.str()
    amount, before, after, mx, x, y, z = r.take("<7f")
    victim = victim if flags & 16 else None
    attacker = attacker if flags & 32 else None
    return dict(tick=tick, flags=flags, victim=victim, root=root, victim_template=vt, attacker=attacker, weapon=weapon,
                source=source, amount=amount, before=before, after=after, max=mx, pos=(x, y, z))


def parse_kill(p):
    r = Reader(p)
    tick, victim, vteam, attacker, ateam = r.take("<IBBBB")
    weapon = r.str()
    flags = r.take("<B")
    attacker = attacker if flags & 8 else None
    return dict(tick=tick, victim=victim, victim_team=vteam, attacker=attacker, attacker_team=ateam, weapon=weapon,
                flags=flags)


def parse_chat(p):
    r = Reader(p)
    tick, flags, sender, team, channel = r.take("<IBBBB")
    return dict(tick=tick, sender=sender if flags & 1 else None, team=team, channel=channel, text=r.str())


def parse_roundstate(p):
    tick, status, brs, left = struct.unpack("<IBBf", p)
    return dict(tick=tick, status=status, begin_round=brs, left=left)


def teleport_frame(rid, pid, pos, yaw):
    return frame(TELEPORT, struct.pack("<IB4f", rid, pid, pos[0], pos[1], pos[2], yaw))


def spot_frame(rid, pid, target):
    return frame(SPOT, struct.pack("<IBI", rid, pid, target))


def set_input(pid, tick, pitch, roll, coll, pedals, buttons=0, look_yaw=0.0, look_pitch=0.0):
    """v1 SetInput. On foot: coll = walk forward/back, pedals = strafe, look_yaw = turn."""
    return frame(SET_INPUT, struct.pack("<BIffffffI", pid, tick, pitch, roll, coll, pedals, look_yaw, look_pitch, buttons))


def target_direction(pid, tick, start, end):
    """v3 SetTargetDirection: steer the bot's TV missile along the ray from start through end."""
    return frame(SET_TARGET_DIRECTION, struct.pack("<BI6f", pid, tick, start[0], start[1], start[2], end[0], end[1], end[2]))


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
            enter_range, proj_range, features = r.take("<ffB")
            log(f"Welcome: protocol {v}, {rate:.0f} Hz, tick {tick}, map '{m}', mode '{mode}', enterRange {enter_range:.1f} m, "
                f"projectileRange {proj_range:.0f} m, features 0x{features:02x}")
            c.welcome = dict(version=v, tick=tick, map=m, mode=mode, enter_range=enter_range, projectile_range=proj_range,
                             teleport=bool(features & 1), restart=bool(features & 2))
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
        self.before_send = None   # called on each TickState before the SetInputs go out
        self.on_state = None      # called on each TickState after they go out (recording)
        self.welcome = self.c.welcome
        self.projectiles = []     # latest ProjectileList entries
        self.pl_count = 0
        self.projectile_seen = {} # id -> first entry seen
        self.damages = []         # v2 events, in arrival order
        self.kills = []
        self.chats = []
        self.rounds = []
        self.steers = {}          # bot id -> (start, end) sent after its SetInput on every TickState (v3)
        self.extra = {}           # bot id -> SetInput dicts sent right after the main one, once (v3 burst)
        self.errors0 = []         # (time, tick, code, message) of Errors without a request id

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
            if self.before_send:
                self.before_send()
            out = []
            for bot, cmd in list(self.inputs.items()):
                btn = cmd.get("buttons", 0) | self.pulses.pop(bot, 0)
                out.append(set_input(bot, tick, cmd.get("pitch", 0.0), cmd.get("roll", 0.0), cmd.get("coll", 0.0),
                                     cmd.get("ped", 0.0), btn, cmd.get("look_yaw", 0.0), cmd.get("look_pitch", 0.0)))
                for x in self.extra.pop(bot, ()):
                    out.append(set_input(bot, tick, x.get("pitch", cmd.get("pitch", 0.0)), x.get("roll", cmd.get("roll", 0.0)),
                                         x.get("coll", cmd.get("coll", 0.0)), x.get("ped", cmd.get("ped", 0.0)),
                                         x.get("buttons", 0)))
            for bot, (start, end) in list(self.steers.items()):
                out.append(target_direction(bot, tick, start, end))
            if out:
                self.c.send(b"".join(out))   # one write, so a tick's SetInputs arrive together
            if self.on_state:
                self.on_state()
        elif t == VEHICLE_LIST:
            _, vs = parse_vehiclelist(p)
            self.vehicles = {v["id"]: v for v in vs}
            self.vl_count += 1
        elif t == PROJECTILE_LIST:
            _, self.projectiles = parse_projectiles(p)
            self.pl_count += 1
            for e in self.projectiles:
                self.projectile_seen.setdefault(e["id"], e)
        elif t == DAMAGE:
            self.damages.append(parse_damage(p))
        elif t == KILL:
            self.kills.append(parse_kill(p))
        elif t == CHAT:
            self.chats.append(parse_chat(p))
            log(f"Chat: {self.chats[-1]}")
        elif t == ROUND_STATE:
            self.rounds.append(parse_roundstate(p))
            log(f"RoundState: {self.rounds[-1]}")
        elif t == BOT_CREATED:
            rid, pid = struct.unpack("<IB", p)
            self.replies[rid] = ("created", pid)
        elif t == ACK:
            self.replies[struct.unpack("<I", p)[0]] = ("ack", None)
        elif t == ERROR:
            rid, code, msg = parse_error(p)
            self.replies[rid] = ("error", (code, msg))
            if rid == 0:
                self.errors0.append((time.time(), self.tick, code, msg))
                if len(self.errors0) <= 20:
                    log(f"Error (no request) code {code}: {msg}")
        elif t == SPAWN_BLOCKED:
            pid, secs = struct.unpack("<BH", p)
            self.blocked.append((time.time(), pid, secs))
            log(f"SpawnBlocked player {pid} after {secs} s")
        elif t == BOT_REMOVED:
            pid, reason = struct.unpack("<BB", p)
            self.removed.append((pid, reason))
            self.inputs.pop(pid, None)
            self.states.pop(pid, None)
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

    def teleport(self, bot, pos, yaw):
        rid = self.next_req()
        self.c.send(teleport_frame(rid, bot, pos, yaw))
        return self.wait_reply(rid)

    def spot(self, bot, target):
        rid = self.next_req()
        self.c.send(spot_frame(rid, bot, target))
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


# ------------------------------------------------------------------------------------- Q9: the Python view

class Rcon:
    """ModManager RCON (mm_rcon), the same digest login as testenv/rcon.py."""

    def __init__(self, host, port, password):
        if port in (4711, 4721):
            raise SystemExit("refusing RCON port %d (another environment's)" % port)
        self.s = socket.create_connection((host, port), timeout=10)
        m = re.search(r"Digest seed: (\S+)", self._until(b"\n\n").decode("latin-1"))
        if not m:
            raise RuntimeError("no digest seed in the RCON banner")
        digest = hashlib.md5((m.group(1) + password).encode("latin-1")).hexdigest()
        self.s.sendall(b"\x02login " + digest.encode() + b"\n")
        if "successful" not in self._until(b"\x04").decode("latin-1"):
            raise RuntimeError("RCON login failed")

    def _until(self, marker):
        buf = b""
        while marker not in buf:
            chunk = self.s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf

    def __call__(self, cmd):
        self.s.sendall(b"\x02" + cmd.encode("latin-1") + b"\n")
        r = self._until(b"\x04").decode("latin-1").strip("\x04\n ")
        log(f"rcon {cmd!r} ->\n    " + r.replace("\r", "\n").replace("\n", "\n    "))
        return r


def mark(label):
    """A section marker, to line the client log up with the mm_webadmin recording and bf2.log."""
    log(f"===== {label}")


def turn_to(cl, bot, target_bot, timeout=12.0):
    """Points the soldier at target_bot. TickState yaw is the body; standing still, look yaw turns the
    head first (Q10). So creep forward with a small look gain, then keep creeping with no look input
    until the body has caught up with the head (yaw steady), and repeat until within 2 degrees."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        cl.pump(0.2)
        e, f = cl.states.get(bot), cl.states.get(target_bot)
        if not (cl.alive(bot) and cl.alive(target_bot)):
            break
        err = wrap180(bearing(e["pos"], f["pos"]) - e["ypr"][0])
        if abs(err) >= 2:
            cl.inputs[bot] = dict(coll=0.15, look_yaw=clamp(err / 40.0, -0.15, 0.15))
            continue
        # settle: creep without look input until the body yaw stops changing
        cl.inputs[bot] = dict(coll=0.15)
        y0 = e["ypr"][0]
        cl.run_for(0.7)
        e = cl.states[bot]
        if abs(wrap180(e["ypr"][0] - y0)) < 0.5 and abs(wrap180(bearing(e["pos"], cl.states[target_bot]["pos"]) - e["ypr"][0])) < 2:
            break
    cl.inputs[bot] = {}
    cl.run_for(0.5)


def fire_at(cl, shooter, target, seconds=12.0, settle=0.5):
    """Faces the target, then fires in bursts (3 ticks on, 3 off, so a semi-automatic weapon fires
    too) while sweeping the aim +-6 degrees, in case the head still leads the body. Returns True if
    the target died (Python sees the kill at once; the alive flag drops when the soldier is dead,
    not man down, about 2 s later). Only point-blank fire hit reliably in testing, so the shooter
    first walks to within about 1.2 m."""
    e, f = cl.states.get(shooter), cl.states.get(target)
    if e and f and flat_dist(e["pos"], f["pos"]) > 1.5:
        walk_to(cl, shooter, f["pos"], 1.2, timeout=30.0, label="close in")
    turn_to(cl, shooter, target)
    cl.run_for(settle)
    e, f = cl.states.get(shooter), cl.states.get(target)
    if e and f:
        log(f"shooter {shooter} -> target {target}: {flat_dist(e['pos'], f['pos']):.1f} m, height "
            f"{f['pos'][1] - e['pos'][1]:+.1f} m, facing error {wrap180(bearing(e['pos'], f['pos']) - e['ypr'][0]):+.1f}")
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        if cl.pump(0.2) != TICK_STATE:
            continue
        if not cl.alive(target):
            cl.inputs[shooter] = {}
            log(f"target {target} died")
            return True
        if not cl.alive(shooter):
            break
        n += 1
        phase = n % 120                    # look 0.04 is about 0.2 degrees per tick: +6, -6, back to 0
        sweep = 0.04 if (phase < 30 or phase >= 90) else -0.04
        cl.inputs[shooter] = dict(buttons=BTN_FIRE if n % 6 < 3 else 0, look_yaw=sweep)
    cl.inputs[shooter] = {}
    cl.run_for(3.0, lambda: not cl.alive(target))
    return not cl.alive(target)


def wait_respawn(cl, bot, timeout=30.0):
    ok = cl.run_for(timeout, lambda: cl.alive(bot) and not cl.in_vehicle(bot))
    e = cl.states.get(bot)
    log(f"bot {bot} {'respawned at %.1f %.1f %.1f team %d' % (tuple(e['pos']) + (e['team'],)) if ok else 'did not respawn'}")
    return ok


def log_team_states(cl, bots):
    for b in bots:
        e = cl.states.get(b)
        log(f"  bot {b}: " + (fmt_entry(e) if e else "no entry"))


def run_q9(args):
    """Drives bots through connect, spawn, enter/seat change, a crash, a team kill, an admin team
    switch and an enemy kill, kick and ban, a round restart and a map change, with RCON probes of
    what ModManager reports. Run it with tools/testclient/wa_recorder.py as mm_webadmin's peer."""
    rc = Rcon(args.host, args.rcon_port, args.rcon_password)
    cl = Client(args, "serverext testclient q9")
    mark("create A, B (team 1) and C (team 2)")
    A = cl.create_bot("[BOT] q9 A", 1)
    B = cl.create_bot("[BOT] q9 B", 1)
    C = cl.create_bot("[BOT] q9 C", 2)
    for b in (A, B, C):
        cl.check(f"bot {b} spawned", cl.wait_alive(b))
    cl.run_for(2.0)
    log_team_states(cl, (A, B, C))
    mark("RCON views of the bots")
    rc("exec admin.listPlayers")
    rc("bf2cc pl")
    rc("bf2cc si")
    rc(f"bf2cc getplayerhash {A}")

    mark("A enters the nearest heli, seat 0")
    kind, data = cl.enter(A, 0, 0)
    cl.check(f"A entered: {kind} {data}", kind == "ack")
    cl.run_for(3.0)
    mark("A presses F2 (seat 1), then F1 (seat 0)")
    cl.pulses[A] = BTN_POSITION[1]
    cl.run_for(2.0)
    log(f"A seat {cl.states[A]['seat']}")
    cl.pulses[A] = BTN_POSITION[0]
    cl.run_for(2.0)
    log(f"A seat {cl.states[A]['seat']}")

    mark("A climbs and crashes the heli with itself in it")
    cl.inputs[A] = {}
    cl.run_for(5.0)                       # rotor spin-up
    y0 = cl.states[A]["pos"][1]
    ap = Autopilot()
    r = fly(cl, A, ap, dict(alt=y0 + 30.0), until=lambda e, t: e["pos"][1] - y0 > 25.0, timeout=40.0)
    log(f"A climb: {r}, {cl.states[A]['pos'][1] - y0:.1f} m above the pad; now collective -1, nose down")
    cl.before_send = lambda: cl.inputs.__setitem__(A, dict(coll=-1.0, pitch=0.6) if cl.in_vehicle(A) else {})
    died = cl.run_for(30.0, lambda: not cl.alive(A))
    cl.before_send = None
    cl.inputs[A] = {}
    cl.check("A died in the crash", died)
    wait_respawn(cl, A)
    cl.run_for(3.0)

    mark("team kill: B fires at A on the pad")
    tk = fire_at(cl, B, A)
    cl.check("B killed A (team kill)", tk)
    wait_respawn(cl, A)
    cl.run_for(2.0)

    mark(f"admin team switch: bf2cc switchplayer {B} 3 (now, no move) while B is alive")
    rc(f"bf2cc switchplayer {B} 3")
    cl.run_for(3.0)
    log_team_states(cl, (A, B))
    rc("bf2cc pl")
    mark("enemy kill: A (team 1) fires at B (now team 2)")
    kill = fire_at(cl, A, B)
    cl.check("A killed B (now an enemy)", kill)
    mark("where does B respawn after the switch")
    if not cl.alive(B):
        wait_respawn(cl, B)
    cl.run_for(20.0, lambda: cl.alive(B))
    log_team_states(cl, (A, B))
    rc("bf2cc pl")

    mark(f"admin.kickPlayer {C}")
    rc(f"exec admin.kickPlayer {C}")
    cl.run_for(4.0)
    cl.check(f"C still exists after the kick (removed: {cl.removed})", C in cl.states and not cl.removed)
    mark(f"admin.banPlayer {C}")
    rc(f"exec admin.banPlayer {C}")
    cl.run_for(4.0)
    mark(f"iga kick {C}")
    rc(f"iga kick {C} q9 test")
    cl.run_for(8.0)
    log(f"removed so far: {cl.removed}")
    log_team_states(cl, (A, B, C))

    mark("RestartRound (admin.restartMap)")
    kind, data = cl.restart()
    log(f"RestartRound: {kind} {data}")
    cl.run_for(5.0)
    log_team_states(cl, (A, B, C))
    cl.run_for(20.0)
    log_team_states(cl, (A, B, C))
    rc("exec admin.listPlayers")

    mark("map change: admin.runNextLevel")
    rc("exec admin.runNextLevel")
    cl.run_for(90.0, lambda: len(cl.removed) >= 3)
    log(f"removed: {cl.removed}")
    mark("recreate a bot on the new map")
    for _ in range(30):
        try:
            D = cl.create_bot("[BOT] q9 D", 1)
            break
        except RuntimeError as ex:
            log(f"CreateBot: {ex}; retrying")
            cl.run_for(3.0)
    else:
        D = None
    if D is not None:
        cl.check("D spawned on the new map", cl.wait_alive(D))
        cl.run_for(3.0)
    rc("exec admin.listPlayers")
    return cl.finish()


# ------------------------------------------------------------------------------------- Q5: watchdog neutral

class Autopilot:
    """Altitude, speed, heading and bank hold for the q5 trials. Signs as in Pilot.station_keep: pitch
    input + = nose down (pitch angle +), roll input + = bank right (roll angle -), pedals + = yaw right."""

    def __init__(self, att=(0.06, 0.04, 0.02), vel=(1.5, 0.3)):
        self.att, self.vel = att, vel
        self.reset()

    def reset(self):
        self.prev = None
        self.ip = self.ir = self.ialt = 0.0
        self.ivf = self.ivr = 0.0
        self.heading = None

    def step(self, e, tick, alt=None, coll=None, v=0.0, bank=None, heading=None, max_tilt=10.0, idle=False,
             pitch_cmd=None):
        yaw, pitch, roll = e["ypr"]
        vx, vy, vz = e["vel"]
        y = e["pos"][1]
        if self.prev is None or tick <= self.prev[0]:
            dt, dp, dr, dyaw = 1 / 30.0, 0.0, 0.0, 0.0
        else:
            dt = (tick - self.prev[0]) / 30.0
            dp, dr = (pitch - self.prev[1]) / dt, (roll - self.prev[2]) / dt
            dyaw = wrap180(yaw - self.prev[3]) / dt
        self.prev = (tick, pitch, roll, yaw)
        if idle:
            return 0.0, 0.0, 0.0, 0.0
        yr = math.radians(yaw)
        v_fwd = vx * math.sin(yr) + vz * math.cos(yr)
        v_right = vx * math.cos(yr) - vz * math.sin(yr)

        if coll is None:
            err = alt - y
            self.ialt = clamp(self.ialt + err * 0.03 * dt, -0.5, 0.5)
            coll = clamp(0.33 + 0.08 * err - 0.25 * vy + self.ialt)

        # Speed: forward speed -> pitch, with a position integral (station keeping) - or a commanded
        # pitch (pitch_cmd, degrees, + = nose down), which is stable where the speed loop is not.
        kv, kvi = self.vel
        akp, akd, aki = self.att
        ev = v_fwd - v
        self.ivf = clamp(self.ivf + ev * dt, -20.0, 20.0)
        if pitch_cmd is not None:
            pitch_des = pitch_cmd
        else:
            pitch_des = clamp(0.5 * v - kv * ev - kvi * self.ivf, -max_tilt, max_tilt)
        if bank is None:
            self.ivr = clamp(self.ivr + v_right * dt, -20.0, 20.0)
            roll_des = clamp(kv * v_right + kvi * self.ivr, -max_tilt, max_tilt)
        else:
            roll_des = bank
        ep, er = pitch_des - pitch, roll_des - roll
        self.ip = clamp(self.ip + ep * aki * dt, -0.5, 0.5)
        self.ir = clamp(self.ir + er * aki * dt, -0.5, 0.5)
        cyc_p = clamp(akp * ep - akd * dp + self.ip)
        cyc_r = clamp(-(akp * er - akd * dr + self.ir))

        if bank is not None:
            # Coordinated turn rate; bank left (roll +) turns left (yaw decreasing).
            sp = max(math.hypot(vx, vz), 5.0)
            r_des = -math.degrees(9.81 * math.tan(math.radians(bank)) / sp)
            ped = clamp(0.02 * (r_des - dyaw))
            self.heading = None
        else:
            if heading is not None:
                self.heading = heading
            elif self.heading is None:
                self.heading = yaw
            ped = clamp(0.03 * wrap180(self.heading - yaw) - 0.03 * dyaw)
        return cyc_p, cyc_r, coll, ped


def body_speeds(e):
    vx, _, vz = e["vel"]
    yr = math.radians(e["ypr"][0])
    return vx * math.sin(yr) + vz * math.cos(yr), vx * math.cos(yr) - vz * math.sin(yr)


def fly(cl, bot, ap, mode, until=None, timeout=60.0):
    """Flies with the autopilot; mode is a dict of Autopilot.step arguments or a function (e, t) -> dict.
    Returns 'ready' when until(e, t) holds, 'lost' if the bot left the vehicle or died, or 'timeout'."""
    t0 = time.time()

    def ctl():
        e = cl.states.get(bot)
        if e and cl.in_vehicle(bot):
            m = mode(e, time.time() - t0) if callable(mode) else mode
            cp, cr, c, ped = ap.step(e, cl.tick, **m)
            cl.inputs[bot] = dict(pitch=cp, roll=cr, coll=c, ped=ped)
            cl.last_cmd = (cp, cr, c, ped)
            cl.coll_avg = c if getattr(cl, "coll_avg", None) is None else 0.97 * cl.coll_avg + 0.03 * c
            if getattr(cl, "fly_log_every", 0) and cl.tick % cl.fly_log_every == 0:
                vf, vr = body_speeds(e)
                log(f"  fly t {time.time() - t0:5.1f} y {e['pos'][1]:7.1f} ypr {e['ypr'][0]:6.1f} {e['ypr'][1]:+6.1f} "
                    f"{e['ypr'][2]:+6.1f} v_fwd {vf:+6.1f} v_right {vr:+5.1f} vy {e['vel'][1]:+5.1f} | cmd p {cp:+.2f} "
                    f"r {cr:+.2f} c {c:+.2f} y {ped:+.2f}")

    cl.before_send = ctl
    try:
        while time.time() - t0 < timeout:
            cl.pump(0.2)
            if not cl.in_vehicle(bot):
                return "lost"
            if until and until(cl.states[bot], time.time() - t0):
                return "ready"
        return "timeout"
    finally:
        cl.before_send = None


def steady(pred, hold):
    """until() helper: pred(e, t) continuously true for hold seconds."""
    since = [None]

    def f(e, t):
        if pred(e, t):
            if since[0] is None:
                since[0] = t
            return t - since[0] >= hold
        since[0] = None
        return False
    return f


class NeutralEmulator:
    """A candidate server-side watchdog neutral, run by the client after a cut so it can be compared
    with the module's (cyclic and pedals 0, collective held). For the first watchdog_ticks ticks it
    repeats the last input, as the module does before the watchdog trips.
      level: cyclic levels pitch and roll (P-D on the attitude), pedals damp the yaw rate,
             collective held.
      hover: level, plus the collective integrates against vertical speed from the held value, so
             a climb or a descent stops without knowing the hover collective."""

    def __init__(self, mode, last, watchdog_ticks=6):
        self.mode, self.last, self.wd = mode, last, watchdog_ticks
        self.n = 0
        self.prev = None
        self.ci = last[2]

    def step(self, e, tick):
        self.n += 1
        yaw, pitch, roll = e["ypr"]
        if self.prev is None or tick <= self.prev[0]:
            dt, dp, dr, dyaw = 1 / 30.0, 0.0, 0.0, 0.0
        else:
            dt = (tick - self.prev[0]) / 30.0
            dp, dr, dyaw = (pitch - self.prev[1]) / dt, (roll - self.prev[2]) / dt, wrap180(yaw - self.prev[3]) / dt
        self.prev = (tick, pitch, roll, yaw)
        if self.n <= self.wd:
            return self.last
        # pitch input + = nose down (pitch +); roll input + = bank right (roll -); pedals + = yaw right
        cyc_p = clamp(-0.05 * pitch - 0.05 * dp)
        cyc_r = clamp(0.05 * roll + 0.05 * dr)
        ped = clamp(-0.03 * dyaw)
        coll = self.last[2]
        if self.mode == "hover":
            vy = e["vel"][1]
            self.ci = clamp(self.ci - 0.03 * vy * dt)
            coll = clamp(self.ci - 0.03 * vy)
        return cyc_p, cyc_r, coll, ped


def cut_and_record(cl, bot, seconds, neutral="module", last=(0.0, 0.0, 0.0, 0.0)):
    """Stops sending SetInput for the bot (the watchdog trips after watchdogTicks) and records every
    TickState entry for the bot for `seconds`. With neutral 'level' or 'hover' the client keeps
    sending, emulating that candidate neutral instead (NeutralEmulator)."""
    cl.inputs.pop(bot, None)
    rec = []
    cl.on_state = lambda: rec.append((cl.tick, dict(cl.states[bot]) if bot in cl.states else None))
    if neutral != "module":
        em = NeutralEmulator(neutral, last)

        def ctl():
            e = cl.states.get(bot)
            if e and cl.in_vehicle(bot):
                cp, cr, c, ped = em.step(e, cl.tick)
                cl.inputs[bot] = dict(pitch=cp, roll=cr, coll=c, ped=ped)
            else:
                cl.inputs.pop(bot, None)
        cl.before_send = ctl
    cl.run_for(seconds)
    cl.on_state = None
    cl.before_send = None
    return rec


def summarize_cut(name, rec, cmd, pad_y, csv, marks=(10.0,)):
    """Prints the cut's state once a second and a RESULT line at each of `marks` seconds and at the end:
    altitude change, worst attitude, speed and whether the bot was still alive and seated."""
    e0 = rec[0][1]
    t0 = rec[0][0]
    y0 = e0["pos"][1]
    vf0, vr0 = body_speeds(e0)
    log(f"[{name}] cut at tick {t0}: {y0 - pad_y:.1f} m above the pad, last input pitch {cmd[0]:+.2f} roll {cmd[1]:+.2f} "
        f"coll {cmd[2]:+.2f} ped {cmd[3]:+.2f}; ypr {e0['ypr'][0]:.1f} {e0['ypr'][1]:+.1f} {e0['ypr'][2]:+.1f} "
        f"v_fwd {vf0:+.1f} v_right {vr0:+.1f} vy {e0['vel'][1]:+.1f}")
    log(f"[{name}]    t    dAlt   aboveP  pitch    roll   dYaw  v_fwd v_right    vy   speed   hp  fl")
    lost_at = None
    rows = []
    for tick, e in rec:
        t = (tick - t0) / 30.0
        if e is None or not (e["flags"] & 1) or not (e["flags"] & 4):
            if lost_at is None:
                lost_at = t
                log(f"[{name}] {t:5.1f} LOST: flags {e['flags'] if e else -1}")
            if csv:
                csv.write(f"{name},{t:.3f},,,,,,,,,,{e['flags'] if e else -1}\n")
            continue
        if lost_at is not None:
            continue
        da = e["pos"][1] - y0
        vf, vr = body_speeds(e)
        sp = math.sqrt(sum(c * c for c in e["vel"]))
        dyaw = wrap180(e["ypr"][0] - e0["ypr"][0])
        rows.append((t, da, e, sp))
        if csv:
            csv.write(f"{name},{t:.3f},{da:.2f},{e['pos'][1] - pad_y:.2f},{e['ypr'][1]:.2f},{e['ypr'][2]:.2f},"
                      f"{dyaw:.2f},{vf:.2f},{vr:.2f},{e['vel'][1]:.2f},{sp:.2f},{e['health']:.3f},{e['flags']}\n")
        if (tick - t0) % 30 == 0:
            log(f"[{name}] {t:5.1f} {da:+7.1f} {e['pos'][1] - pad_y:7.1f} {e['ypr'][1]:+6.1f} {e['ypr'][2]:+7.1f} "
                f"{dyaw:+6.1f} {vf:+6.1f} {vr:+6.1f} {e['vel'][1]:+6.1f} {sp:6.1f} {e['health']:5.2f} {e['flags']}")
    end_t = (rec[-1][0] - t0) / 30.0
    for m in tuple(marks) + (end_t,):
        part = [r for r in rows if r[0] <= m + 1e-6]
        if not part:
            continue
        t, da, e, sp = part[-1]
        lost = lost_at is not None and lost_at <= m
        verdict = f"LOST at {lost_at:.1f} s" if lost else f"alive and seated at {m:.0f} s"
        log(f"[{name}] RESULT@{m:.0f}s {verdict}; alt change {da:+.1f} m (min {min(r[1] for r in part):+.1f}, max "
            f"{max(r[1] for r in part):+.1f}); max |pitch| {max(abs(r[2]['ypr'][1]) for r in part):.1f}, max |roll| "
            f"{max(abs(r[2]['ypr'][2]) for r in part):.1f}; speed {sp:.1f} m/s; health {e0['health']:.2f} -> {e['health']:.2f}")
    return lost_at is None


def run_q5(args):
    """Cuts the input (watchdog) in several flight states and records the next seconds."""
    cl = Client(args, "serverext testclient q5")
    cl.last_cmd = (0.0, 0.0, 0.0, 0.0)
    cl.fly_log_every = args.q5_fly_log
    csv = open(args.q5_csv, "a") if args.q5_csv else None
    if csv:
        csv.write("trial,t,dalt,above_pad,pitch,roll,dyaw,v_fwd,v_right,vy,speed,health,flags\n")
    bot = cl.create_bot(args.name, args.team)
    ap = Autopilot(tuple(float(x) for x in args.q5_att.split(",")), tuple(float(x) for x in args.q5_vel.split(",")))
    st = dict(pad_y=None)
    center = (19.0, 0.0, -57.0)    # midway between the dalian_2_v_2 bases

    def mount():
        """Make sure the bot is seated in a heli and hovering pad + 60 m."""
        while True:
            if not cl.in_vehicle(bot):
                if not cl.wait_alive(bot, 40.0):
                    raise RuntimeError("bot never spawned")
                cl.run_for(1.0)
                kind, data = cl.enter(bot, 0, 0)
                log(f"EnterVehicle: {kind} {data}")
                if kind != "ack":
                    cl.run_for(3.0)
                    continue
                cl.run_for(0.5)
                st["pad_y"] = cl.states[bot]["pos"][1]
                ap.reset()
                fly(cl, bot, ap, dict(idle=True), timeout=args.spinup)
                ap.reset()
            r = fly(cl, bot, ap, dict(alt=st["pad_y"] + 60.0),
                    until=steady(lambda e, t: abs(e["pos"][1] - st["pad_y"] - 60.0) < 4.0
                                 and math.sqrt(sum(c * c for c in e["vel"])) < 2.5, 3.0), timeout=90.0)
            if r == "ready":
                st["hover_coll"] = cl.coll_avg     # about the last second; the last command alone can be far off
                log(f"mount: hovering {cl.states[bot]['pos'][1] - st['pad_y']:.1f} m above the pad, collective {st['hover_coll']:+.2f}")
                return
            log(f"mount: {r}; trying again")

    def trial_hover():
        return "ready"

    def trial_climb():
        return fly(cl, bot, ap, dict(coll=1.0), until=lambda e, t: t > 3.0, timeout=10.0)

    def trial_descent():
        r = fly(cl, bot, ap, dict(alt=st["pad_y"] + 110.0),
                until=steady(lambda e, t: abs(e["pos"][1] - st["pad_y"] - 110.0) < 4.0
                             and math.sqrt(sum(c * c for c in e["vel"])) < 3.0, 2.0), timeout=45.0)
        if r != "ready":
            return r
        coll = st.get("hover_coll", 0.33) - args.q5_descent_drop
        return fly(cl, bot, ap, dict(coll=coll), until=lambda e, t: t > 2.0 and e["vel"][1] < -3.0, timeout=15.0)

    def cruise(v_target, until, timeout, bank=None):
        """Level at pad + 80 m, then ramp the speed up at 1.5 m/s^2 towards the map centre."""
        r = fly(cl, bot, ap, dict(alt=st["pad_y"] + 80.0),
                until=steady(lambda e, t: abs(e["pos"][1] - st["pad_y"] - 80.0) < 4.0, 1.0), timeout=40.0)
        if r != "ready":
            return r
        hdg = bearing(cl.states[bot]["pos"], center)
        ap.heading = hdg
        return fly(cl, bot, ap, lambda e, t: dict(alt=st["pad_y"] + 80.0, pitch_cmd=v_target, heading=hdg,
                                                  bank=bank), until=until, timeout=timeout)

    def speed_steady():
        """until(): forward speed changing by less than 0.5 m/s per second, level, for 3 s."""
        hist = []

        def f(e, t):
            hist.append((t, body_speeds(e)[0]))
            while hist and hist[0][0] < t - 3.0:
                hist.pop(0)
            return (t > 8.0 and len(hist) > 30 and abs(hist[-1][1] - hist[0][1]) < 1.5
                    and abs(e["vel"][1]) < 1.5 and abs(e["pos"][1] - st["pad_y"] - 80.0) < 8.0)
        return f

    def trial_forward():
        # args.q5_fwd_pitch degrees nose down gives the cruise speed the trial reports (about 30 m/s).
        return cruise(args.q5_fwd_pitch, speed_steady(), 60.0)

    def trial_turn():
        r = cruise(args.q5_turn_pitch, speed_steady(), 50.0)
        if r != "ready":
            return r
        return fly(cl, bot, ap, dict(alt=st["pad_y"] + 80.0, pitch_cmd=args.q5_turn_pitch, bank=args.q5_bank),
                   until=steady(lambda e, t: abs(e["ypr"][2] - args.q5_bank) < 5.0 and abs(e["vel"][1]) < 2.0, 2.0),
                   timeout=25.0)

    trials = dict(hover=trial_hover, climb=trial_climb, descent=trial_descent, forward=trial_forward, turn=trial_turn)
    results = []
    for rep, neutral, name in [(r, n, t) for r in range(args.q5_repeat) for n in args.q5_neutral.split(",")
                               for t in args.q5_trials.split(",")]:
        if True:
            label = f"{name}/{neutral}#{rep + 1}"
            for attempt in range(3):
                mount()
                mark(f"q5 trial {label}: setting up")
                r = trials[name]()
                if r == "ready":
                    break
                log(f"[{label}] setup {r}; again")
            else:
                cl.check(f"{label}: set up", False)
                continue
            cmd = cl.last_cmd
            mark(f"q5 trial {label}: CUT INPUT")
            rec = cut_and_record(cl, bot, args.q5_record, neutral, cmd)
            ok = summarize_cut(label, rec, cmd, st["pad_y"], csv)
            results.append((label, ok))
            if not cl.in_vehicle(bot):
                cl.run_for(2.0)
            ap.reset()
    for label, ok in results:
        log(f"q5 {label}: {'survived' if ok else 'LOST'}")
    if csv:
        csv.close()
    return cl.finish()


# ------------------------------------------------------------------------------------- team switch

def board(cl, bot, team, label, exclude=()):
    """Walks bot to the nearest undestroyed heli it may enter (team 0 or team, pilot seat free, not in
    exclude) and enters its pilot seat. Returns the EnterVehicle reply."""
    e = cl.states.get(bot)
    hs = [v for v in cl.vehicles.values() if "ahe_" in v["name"] and free_seat0(v, team) and v["id"] not in exclude]
    if not e or not hs:
        return ("error", (5, "no heli to board"))
    v = min(hs, key=lambda v: flat_dist(v["pos"], e["pos"]))
    d = flat_dist(v["pos"], e["pos"])
    log(f"{label}: boarding {v['name']} id {v['id']} team {v['team']} at {d:.1f} m")
    if d > 7.0:
        walk_to(cl, bot, v["pos"], 7.0, label=label)
    return cl.enter(bot, v["id"], 0)


def crash(cl, bot, label):
    """Climbs about 25 m in the current heli, then dives into the ground. True if the bot died."""
    cl.inputs[bot] = {}
    cl.run_for(5.0)                       # rotor spin-up
    y0 = cl.states[bot]["pos"][1]
    r = fly(cl, bot, Autopilot(), dict(alt=y0 + 30.0), until=lambda e, t: e["pos"][1] - y0 > 25.0, timeout=40.0)
    log(f"{label} climb: {r}, {cl.states[bot]['pos'][1] - y0:.1f} m; now collective -1, nose down")
    cl.before_send = lambda: cl.inputs.__setitem__(bot, dict(coll=-1.0, pitch=0.6) if cl.in_vehicle(bot) else {})
    died = cl.run_for(30.0, lambda: not cl.alive(bot))
    cl.before_send = None
    cl.inputs[bot] = {}
    return died


def run_teamswitch(args):
    """An admin team switch of a bot (bf2cc switchplayer <id> 3). B goes 1 -> 2 while alive at team
    1's pad: it is refused a seat in the heli A (team 1) holds, walks to another, empty heli (team 0),
    crashes it, respawns at team 2's pad and enters a heli there. Then B crashes again, goes 2 -> 1
    while dead, respawns at team 1's pad and gets the seat in A's heli it was refused before. No
    SpawnBlocked at any point. (An empty heli is team 0, so the team check needs an occupied one, and
    a crash is a more reliable death than on-foot fire.)"""
    rc = Rcon(args.host, args.rcon_port, args.rcon_password)
    cl = Client(args, "serverext testclient teamswitch")
    mark("reset the vehicles: admin.restartMap")
    rc("exec admin.restartMap")
    cl.run_for(8.0)
    for v in sorted(cl.vehicles.values(), key=lambda v: v["id"]):
        log(f"  vehicle {v['id']} {v['name']} team {v['team']} flags {v['flags']} pos "
            f"{v['pos'][0]:.1f} {v['pos'][1]:.1f} {v['pos'][2]:.1f} occ {v['occupied']:#x}")
    mark("create A and B on team 1")
    A = cl.create_bot("[BOT] ts A", 1)
    B = cl.create_bot("[BOT] ts B", 1)
    for b in (A, B):
        cl.check(f"bot {b} spawned", cl.wait_alive(b))
    cl.run_for(2.0, lambda: cl.vl_count > 0)
    log_team_states(cl, (A, B))
    cl.check("A and B are on team 1 in TickState (CreateBot's team won over PlayerConnect's)",
             cl.states[A]["team"] == 1 and cl.states[B]["team"] == 1)
    pad1 = list(cl.states[B]["pos"])
    rc("bf2cc pl")

    mark(f"switch B 1 -> 2 while alive: bf2cc switchplayer {B} 3")
    rc(f"bf2cc switchplayer {B} 3")
    cl.check("TickState shows B on team 2", cl.run_for(3.0, lambda: cl.states[B]["team"] == 2))

    mark("A (team 1) takes the pilot seat of the nearest heli; B (team 2) asks for its seat 1")
    kind, data = cl.enter(A, 0, 0)
    cl.check(f"A entered: {kind} {data}", kind == "ack")
    cl.run_for(1.5)
    held = cl.vehicles.get(cl.states[A]["root"])
    log(f"A holds {held and held['name']} id {held and held['id']} team {held and held['team']} seats {held and held['seats']}")
    if held and held["seats"] > 1:
        kind, data = cl.enter(B, held["id"], 1)
        cl.check(f"EnterVehicle {held['id']} seat 1 (A's team 1 heli) refused for B on team 2: {kind} {data}",
                 kind == "error" and data[0] == 5 and "other team" in data[1])

    mark("B takes another, empty heli near team 1's pad and crashes it")
    kind, data = board(cl, B, 2, "B", exclude=(held["id"],) if held else ())
    cl.check(f"B (team 2) enters an empty heli: {kind} {data}", kind == "ack")
    cl.run_for(1.0)
    cl.check("B died in the crash", cl.in_vehicle(B) and crash(cl, B, "B"))
    blocked0 = len(cl.blocked)
    ok = wait_respawn(cl, B, timeout=40.0)
    cl.check("B respawned after the switch", ok)
    if ok:
        cl.run_for(1.5)
        e = cl.states[B]
        far = flat_dist(e["pos"], pad1)
        log(f"B respawn: {far:.0f} m from team 1's pad")
        cl.check("B respawned on team 2, away from team 1's pad", e["team"] == 2 and far > 100.0)
        kind, data = board(cl, B, 2, "B")
        cl.check(f"B enters a heli at team 2's pad: {kind} {data}", kind == "ack")
        cl.run_for(1.0)
        e = cl.states[B]
        v = cl.vehicles.get(e["root"])
        cl.check(f"TickState shows B in a team 2 vehicle ({v and v['name']} team {v and v['team']})",
                 cl.in_vehicle(B) and v is not None and v["team"] == 2)
    cl.check(f"no SpawnBlocked for B after the switch ({cl.blocked[blocked0:]})",
             not [x for x in cl.blocked[blocked0:] if x[1] == B])

    mark("B crashes the heli at team 2's pad; switch it 2 -> 1 while dead")
    died = cl.in_vehicle(B) and crash(cl, B, "B")
    cl.check("B died in the crash", died)
    if died:
        rc(f"bf2cc switchplayer {B} 3")
        cl.check("TickState shows dead B on team 1", cl.run_for(3.0, lambda: cl.states[B]["team"] == 1))
    blocked0 = len(cl.blocked)
    ok = wait_respawn(cl, B, timeout=40.0)
    cl.check("B respawned after the switch back", ok)
    if ok:
        cl.run_for(1.5)
        e = cl.states[B]
        d = flat_dist(e["pos"], pad1)
        log(f"B respawn: {d:.1f} m from team 1's pad")
        cl.check("B respawned on team 1 at team 1's pad", e["team"] == 1 and d < 40.0)
        if held and cl.in_vehicle(A):
            kind, data = cl.enter(B, held["id"], 1)
            cl.check(f"B (team 1 again) gets seat 1 of A's heli {held['id']}, refused before: {kind} {data}", kind == "ack")
        else:
            kind, data = board(cl, B, 1, "B")
            cl.check(f"B enters a heli near team 1's pad: {kind} {data}", kind == "ack")
        cl.run_for(1.0)
        e = cl.states[B]
        v = cl.vehicles.get(e["root"])
        cl.check(f"TickState shows B in a team 1 vehicle ({v and v['name']} team {v and v['team']}, seat {e['seat']})",
                 cl.in_vehicle(B) and v is not None and v["team"] == 1)
    cl.check(f"no SpawnBlocked for B after the switch back ({cl.blocked[blocked0:]})",
             not [x for x in cl.blocked[blocked0:] if x[1] == B])
    rc("bf2cc pl")
    return cl.finish()


SCENARIOS = {"wreck": run_wreck, "walk": run_walk, "buttons": run_buttons, "blockpad": run_blockpad, "allblocked": run_allblocked,
             "q9": run_q9, "q5": run_q5, "teamswitch": run_teamswitch}


# ------------------------------------------------------------------------------------- v2 scenarios
# docs/findings/module-v2.md. Every shot in these scenarios goes into empty ground or the sea away
# from both bases, or between two bots of this client; nothing else is ever targeted.

MAP_CENTER = (19.0, 0.0, -57.0)     # midway between the dalian_2_v_2 bases


def ensure_boarded(cl, bot, team, label, seat=0, vehicle_id=None, spinup=5.0):
    """Waits until the bot is alive, then boards the nearest usable heli (or vehicle_id) in seat.
    Returns True when seated."""
    for attempt in range(4):
        if cl.in_vehicle(bot) and cl.states[bot]["seat"] == seat:
            return True
        if not cl.wait_alive(bot, 40.0):
            log(f"{label}: bot {bot} never spawned")
            return False
        cl.run_for(1.0, lambda: cl.vl_count > 0)
        if vehicle_id is None:
            kind, data = board(cl, bot, team, label) if seat == 0 else ("error", "no seat")
        else:
            v = cl.vehicles.get(vehicle_id)
            e = cl.states.get(bot)
            if v and e and flat_dist(v["pos"], e["pos"]) > 7.0:
                walk_to(cl, bot, v["pos"], 7.0, label=label)
            kind, data = cl.enter(bot, vehicle_id, seat)
        log(f"{label}: EnterVehicle -> {kind} {data}")
        if kind == "ack":
            cl.inputs[bot] = {}
            cl.run_for(spinup)
            return True
        cl.run_for(3.0)
    return False


def fly_bots(cl, plans, seconds, until=None):
    """Flies several bots at once. plans: bot -> (Autopilot, mode dict or f(e, t) -> dict, buttons f(t) or None).
    Returns 'ready' when until() holds, else 'timeout'."""
    t0 = time.time()

    def ctl():
        t = time.time() - t0
        for bot, (ap, mode, buttons) in plans.items():
            e = cl.states.get(bot)
            if not (e and cl.in_vehicle(bot)):
                cl.inputs[bot] = {}
                continue
            m = mode(e, t) if callable(mode) else mode
            cp, cr, c, ped = ap.step(e, cl.tick, **m)
            cl.inputs[bot] = dict(pitch=cp, roll=cr, coll=c, ped=ped, buttons=buttons(t) if buttons else 0)

    cl.before_send = ctl
    try:
        end = t0 + seconds
        while time.time() < end:
            cl.pump(0.2)
            if until and until():
                return "ready"
        return "timeout"
    finally:
        cl.before_send = None
        for bot in plans:
            cl.inputs[bot] = {}


def climb_to(cl, bot, ap, alt, label, heading=None, timeout=60.0):
    r = fly(cl, bot, ap, dict(alt=alt, heading=heading),
            until=steady(lambda e, t: abs(e["pos"][1] - alt) < 4.0 and math.sqrt(sum(c * c for c in e["vel"])) < 3.0, 2.0),
            timeout=timeout)
    log(f"{label}: climb to {alt:.0f}: {r}")
    return r == "ready"


def seaward(pos):
    """Heading from the map centre through pos, i.e. away from the other base."""
    return bearing(MAP_CENTER, pos)


def run_view(args):
    """Items 1 and 2: Welcome carries enterRange; TickState's view mode follows every camera key."""
    cl = Client(args, "serverext testclient view")
    w = cl.welcome
    cl.check(f"Welcome v2: enterRange {w['enter_range']:.1f} m, projectileRange {w['projectile_range']:.0f} m, "
             f"teleport {w['teleport']}, restart {w['restart']}", w["version"] == PROTOCOL_VERSION and w["enter_range"] > 0)
    if args.enter_range:
        cl.check(f"Welcome enterRange equals serverext.cfg ({args.enter_range})", abs(w["enter_range"] - args.enter_range) < 0.01)
    bot = cl.create_bot(args.name, args.team)
    cl.check("bot spawned", cl.wait_alive(bot))
    cl.run_for(1.0)
    e = cl.states[bot]
    log(f"on foot: view mode {e['view']}, weapons {[w['name'] for w in e['weapons']]}")
    if not cl.check("boarded a heli", ensure_boarded(cl, bot, args.team, "view", spinup=1.0)):
        return cl.finish()
    cl.check(f"in the pilot seat the view mode starts at 1 (inside), got {cl.states[bot]['view']}", cl.states[bot]["view"] == 1)
    # Expected modes from module-v1.md (Q11): toggle cycles 1 -> 2 -> 3, then mode keys 1/2/3/4 give 1/3/4/5.
    steps = [("toggle camera", BTN_TOGGLE_CAMERA, 2), ("toggle camera", BTN_TOGGLE_CAMERA, 3),
             ("camera mode 1", BTN_CAMERA_MODE[0], 1), ("camera mode 2", BTN_CAMERA_MODE[1], 3),
             ("camera mode 3", BTN_CAMERA_MODE[2], 4), ("camera mode 4", BTN_CAMERA_MODE[3], 5),
             ("camera mode 1", BTN_CAMERA_MODE[0], 1)]
    for label, b, want in steps:
        cl.pulses[bot] = b
        sent = cl.tick
        cl.run_for(1.0)
        got = cl.states[bot]["view"]
        cl.check(f"{label} (pulse after tick {sent}): TickState view mode {got}, want {want}", got == want)
    return cl.finish()


def run_weapons(args):
    """Items 4 and 5: weapon state of a pilot (rockets), then rockets fired nose-down into empty ground
    seaward of the base: rounds drop, the magazine reloads, ProjectileList shows the rockets as own. With
    --tv, a second bot in the gunner seat fires the guided missile the same way."""
    cl = Client(args, "serverext testclient weapons")
    pilot = cl.create_bot(args.name, args.team)
    gunner = cl.create_bot(args.name + " gunner", args.team) if args.tv else None
    cl.check("pilot spawned", cl.wait_alive(pilot))
    e = cl.states[pilot]
    cl.check(f"on foot the bot has weapons: {[(w['slot'], w['name'], w['ammo'], w['mags']) for w in e['weapons']]}",
             len(e["weapons"]) > 0)
    if not cl.check("pilot boarded", ensure_boarded(cl, pilot, args.team, "pilot")):
        return cl.finish()
    heli = cl.states[pilot]["root"]
    if gunner is not None:
        cl.check("gunner boarded seat 1", ensure_boarded(cl, gunner, args.team, "gunner", seat=1, vehicle_id=heli, spinup=0.5))
    ws = cl.states[pilot]["weapons"]
    log(f"pilot weapons: {ws}")
    if gunner is not None:
        log(f"gunner weapons: {cl.states[gunner]['weapons'] if gunner in cl.states else None}")
        # v3 item 2: the TV launcher is not selectable; one tick of alt fire (the seat's toggle) selects it.
        cl.pulses[gunner] = BTN_ALT_FIRE
        cl.run_for(1.0)
        log(f"gunner weapons after one tick of alt fire: {cl.states[gunner]['weapons'] if gunner in cl.states else None}")
    cl.check("the pilot seat reports at least one weapon", len(ws) > 0)
    # Select weapon 1 (rockets) for one tick.
    cl.pulses[pilot] = BTN_WEAPON[0]
    cl.run_for(1.0)
    w0 = next((w for w in cl.states[pilot]["weapons"] if w["slot"] == 0), None)
    if not cl.check(f"slot 0 weapon after weapon select 1: {w0}", w0 is not None):
        return cl.finish()
    pad = cl.states[pilot]["pos"]
    hdg = seaward(pad)
    ap = Autopilot()
    if not climb_to(cl, pilot, ap, pad[1] + 45.0, "weapons", heading=hdg):
        return cl.finish()
    ammo0 = next(w for w in cl.states[pilot]["weapons"] if w["slot"] == 0)
    log(f"before firing: {ammo0}, heading {cl.states[pilot]['ypr'][0]:.0f} (seaward {hdg:.0f})")
    seen_before = set(cl.projectile_seen)
    # Nose 15 degrees down, hold altitude, fire for 4 s.
    track = []

    def rec():
        w = next((w for w in cl.states[pilot]["weapons"] if w["slot"] == 0), None) if pilot in cl.states else None
        if w:
            track.append((time.time(), w["ammo"], w["mags"], w["reloading"], w["reload_left"]))
    cl.on_state = rec
    # The pods fire once per press: pulse the trigger, 3 ticks on, 3 off.
    plans = {pilot: (ap, lambda e, t: dict(alt=pad[1] + 45.0, pitch_cmd=15.0, heading=hdg),
                     lambda t: BTN_FIRE if 1.5 < t < 5.5 and cl.tick % 6 < 3 else 0)}
    if gunner is not None:
        # Fire launches the TV missile (unguided: it flies straight on until its time to live).
        plans[gunner] = (Autopilot(), dict(idle=True), lambda t: BTN_FIRE if 1.0 < t < 1.2 else 0)
    fly_bots(cl, plans, 7.0)
    # Level off and hover while the rockets land and the magazine reloads.
    fly_bots(cl, {pilot: (ap, dict(alt=pad[1] + 45.0, heading=hdg), None)}, 14.0)
    cl.on_state = None
    new = [p for i, p in cl.projectile_seen.items() if i not in seen_before]
    own = [p for p in new if p["own"]]
    names = sorted(set(p["name"] for p in new))
    log(f"projectiles seen: {len(new)} new ({len(own)} own), templates {names}, ProjectileLists {cl.pl_count}")
    for p in new[:5]:
        log(f"  {p}")
    cl.check("ProjectileList reported the bot's rockets as own projectiles", len(own) > 0)
    cl.check("every new projectile is owned by one of this client's bots",
             all(p["owner"] in (pilot, gunner) for p in new))
    if new:
        p = new[0]
        cl.check(f"projectile has a template, team {p['team']}, speed {math.sqrt(sum(v * v for v in p['vel'])):.0f} m/s",
                 p["name"] != "" and p["team"] == args.team)
    if gunner is not None:
        guided = [p for p in new if p["guided"]]
        cl.check(f"the gunner's missile is reported as guided ({len(guided)}: {sorted(set(p['name'] for p in guided))})", guided)
    ammos = [a for _, a, _, _, _ in track]
    if ammos:
        log(f"slot 0 rounds over the run: first {ammos[0]}, min {min(ammos)}, last {ammos[-1]}; mags {track[0][2]} -> {track[-1][2]}")
        cl.check("rounds in the magazine dropped while firing", min(ammos) < ammos[0])
        reloads = [r for r in track if r[3]]
        if reloads:
            lefts = [round(r[4], 2) for r in reloads]
            log(f"reload_left over the reload ({len(lefts)} ticks): {lefts[:5]} ... {lefts[-5:]}")
            peak = lefts.index(max(lefts))
            cl.check(f"reloading reported, reload_left falling from {max(lefts):.2f} to {lefts[-1]:.2f}",
                     max(lefts) > 0.5 and lefts[-1] < 0.2 and all(a >= b for a, b in zip(lefts[peak:], lefts[peak + 1:])))
            cl.check("magazine count dropped after the reload", track[-1][2] < track[0][2] or track[-1][1] > min(ammos))
        else:
            log("no reload during the run (the magazine was not emptied)")
    cl.check(f"ProjectileList went empty again (last list has {len(cl.projectiles)})", len(cl.projectiles) == 0)
    return cl.finish()


def run_damage(args):
    """Items 8 and 10, between this client's own bots only. Needs allowTeleport 1.
    1. Rifle at an occupied heli: B (team 2) sits in its pad's heli; A (team 1) is teleported 15 m in
       front of it and fires. Damage events carry both own flags and the heli's hit points.
    2. Rockets at a soldier: A pilots team 1's heli, turned seaward where it stands; C (team 2, on foot)
       is teleported 20 m ahead of its nose. A fires rockets: Damage and Kill with A as attacker, and
       C's man-down bit while its alive flag is still set.
    3. With --crash, A crashes the heli: Damage and Kill without an attacker."""
    cl = Client(args, "serverext testclient damage")
    a = cl.create_bot(args.name + " A", 1)
    b = cl.create_bot(args.name + " B", 2)
    cl.check("A and B spawned", cl.wait_alive(a) and cl.wait_alive(b))

    # 1. Rifle at B's heli.
    if cl.check("B boarded its pad's heli", ensure_boarded(cl, b, 2, "B", spinup=1.0)):
        eb = cl.states[b]
        yb = math.radians(eb["ypr"][0])
        stand = (eb["pos"][0] + 15.0 * math.sin(yb), eb["pos"][1] + 1.0, eb["pos"][2] + 15.0 * math.cos(yb))
        kind, data = cl.teleport(a, stand, eb["ypr"][0] + 180.0)
        cl.check(f"A teleported 15 m in front of B's heli: {kind} {data}", kind == "ack")
        cl.run_for(1.5)
        e = cl.states[a]
        log(f"A -> B's heli: {flat_dist(e['pos'], eb['pos']):.1f} m, facing error "
            f"{wrap180(bearing(e['pos'], eb['pos']) - e['ypr'][0]):+.1f}, A's weapons {e['weapons']}")
        n0 = len(cl.damages)
        n = [0]

        def burst():
            n[0] += 1
            cl.inputs[a] = dict(buttons=BTN_FIRE if n[0] % 6 < 3 else 0)
        cl.before_send = burst
        cl.run_for(8.0)
        cl.before_send = None
        cl.inputs[a] = {}
        cl.run_for(1.0)
        hits = [d for d in cl.damages[n0:] if d["root"] == eb["root"]]
        log(f"Damage events on B's heli: {len(hits)}; first {hits[0] if hits else None}")
        if cl.check("Damage events for B's heli arrived", hits):
            d = hits[0]
            cl.check(f"Damage on the heli: flags 0x{d['flags']:02x} (victim own, attacker own), victim player {d['victim']}, "
                     f"attacker {d['attacker']}, weapon '{d['weapon']}', {d['before']:.1f} -> {d['after']:.1f} of {d['max']:.1f}",
                     d["flags"] & 3 == 3 and d["victim"] == b and d["attacker"] == a and d["weapon"] != ""
                     and d["before"] > d["after"])
            cl.check(f"the last hit's 'after' matches TickState health ({hits[-1]['after'] / hits[-1]['max']:.3f} vs "
                     f"{cl.states[b]['health']:.3f})", abs(hits[-1]["after"] / hits[-1]["max"] - cl.states[b]["health"]) < 0.02)

    # 2. Rockets at C.
    c = cl.create_bot(args.name + " C", 2)
    cl.check("C spawned", cl.wait_alive(c))
    cl.remove(a)
    a = cl.create_bot(args.name + " A", 1)
    if not cl.check("A boarded team 1's heli", ensure_boarded(cl, a, 1, "A")):
        return cl.finish()
    e = cl.states[a]
    hdg = seaward(e["pos"])
    kind, data = cl.teleport(a, (e["pos"][0], e["pos"][1] + 0.5, e["pos"][2]), hdg)
    cl.check(f"A's heli turned seaward ({hdg:.0f}) where it stands: {kind} {data}", kind == "ack")
    cl.run_for(2.0)
    flags_seen = []
    cl.on_state = lambda: flags_seen.append((time.time(), cl.states[c]["flags"], cl.states[c]["pos"])) if c in cl.states else None
    n0, k0 = len(cl.damages), len(cl.kills)
    cl.pulses[a] = BTN_WEAPON[0]
    cl.run_for(0.5)
    for gap in (args.damage_gap, args.damage_gap * 0.7, args.damage_gap * 0.5):
        if not cl.alive(c) or cl.states[c]["flags"] & F_MANDOWN:
            break
        e = cl.states[a]
        yr = math.radians(e["ypr"][0])
        target = (e["pos"][0] + gap * math.sin(yr), e["pos"][1] + 0.5, e["pos"][2] + gap * math.cos(yr))
        kind, data = cl.teleport(c, target, e["ypr"][0] + 180.0)
        cl.check(f"C teleported {gap:.0f} m ahead of A's nose: {kind} {data}", kind == "ack")
        cl.run_for(1.5)
        f = cl.states[c]
        w = next((w for w in e["weapons"] if w["slot"] == 0), None)
        log(f"A -> C: {flat_dist(e['pos'], f['pos']):.1f} m, height {f['pos'][1] - e['pos'][1]:+.1f}, rockets {w}")
        t0 = time.time()
        # The pod fires once per press: pulse the trigger, 3 ticks on, 3 off.
        cl.before_send = lambda: cl.inputs.__setitem__(
            a, dict(buttons=BTN_FIRE if (time.time() - t0 < 3.0 and cl.tick % 6 < 3 and cl.alive(c)
                                         and not (cl.states[c]["flags"] & F_MANDOWN)) else 0))
        cl.run_for(6.0, lambda: not cl.alive(c) or cl.states[c]["flags"] & F_MANDOWN)
        cl.before_send = None
        cl.inputs[a] = {}
    cl.run_for(args.mandown_wait, lambda: not cl.alive(c) or (flags_seen and not flags_seen[-1][1] & F_MANDOWN
                                                              and any(fl & F_MANDOWN for _, fl, _ in flags_seen)))
    cl.run_for(1.0)
    cl.on_state = None
    dmg = [d for d in cl.damages[n0:] if d["victim"] == c]
    kills = [k for k in cl.kills[k0:] if k["victim"] == c]
    log(f"Damage events with victim C: {len(dmg)}; first {dmg[0] if dmg else None}; kills {kills}")
    cl.check("C was killed by the rockets", kills)
    if cl.check("Damage events for C arrived", dmg):
        d = dmg[0]
        cl.check(f"Damage on C: flags 0x{d['flags']:02x}, attacker {d['attacker']}, weapon '{d['weapon']}', source "
                 f"'{d['source']}', {d['before']:.1f} -> {d['after']:.1f} of {d['max']:.1f}",
                 d["flags"] & 3 == 3 and d["attacker"] == a and d["before"] > d["after"])
    if kills:
        k = kills[0]
        cl.check(f"Kill: attacker {k['attacker']} team {k['attacker_team']}, weapon '{k['weapon']}', flags 0x{k['flags']:02x}",
                 k["attacker"] == a and k["flags"] & 3 == 3 and not k["flags"] & 4)
    md = [(t, fl, pos) for t, fl, pos in flags_seen if fl & F_MANDOWN]
    if md:
        # With sv.spawnTime <= sv.manDownTime the engine kills and respawns the bot in one tick, so the
        # alive flag may never read 0: the end of man-down is the bit dropping, at a spawn point.
        t_md, _, pos_md = md[0]
        after = [(t, fl, pos) for t, fl, pos in flags_seen if t > md[-1][0]]
        end = after[0] if after else None
        moved = flat_dist(end[2], pos_md) if end else 0.0
        cl.check(f"C's man-down bit was set with the alive flag, for {(end[0] - t_md) if end else -1:.1f} s",
                 all(fl & F_ALIVE for _, fl, _ in md) and end is not None)
        if end:
            log(f"after man-down: flags 0x{end[1]:02x}, moved {moved:.0f} m (alive 0 seen: "
                f"{any(not fl & F_ALIVE for _, fl, _ in after)})")
            cl.check(f"then C died and respawned: alive flag cleared, or moved {moved:.0f} m to a spawn point in the same tick",
                     any(not fl & F_ALIVE for _, fl, _ in after) or moved > 30.0)
    else:
        cl.check(f"C's man-down bit seen (flags seen {sorted(set(fl for _, fl, _ in flags_seen))})", False)

    # 3. A crash.
    if args.crash:
        cl.run_for(1.0)
        if ensure_boarded(cl, a, 1, "crash"):
            n0 = len(cl.kills)
            died = crash(cl, a, "crash")
            cl.run_for(1.0)
            k = [k for k in cl.kills[n0:] if k["victim"] == a]
            cl.check(f"crash: Kill with no attacker {k[:1]}", died and k and k[0]["attacker"] is None)
            cd = [d for d in cl.damages if d["victim"] == a and d["attacker"] is None]
            cl.check(f"crash: Damage with no attacker ({len(cd)}), victim-own flag", cd and cd[-1]["flags"] & 1)
    return cl.finish()


def run_spot(args):
    """Item 6: A spots B (other team); B's entry carries the spotted bit for about 12 s. Refusals: an own
    team target, a second spot within a second, an unknown id. Needs allowTeleport 1 to bring B close."""
    cl = Client(args, "serverext testclient spot")
    a = cl.create_bot(args.name + " A", 1)
    b = cl.create_bot(args.name + " B", 2)
    a2 = cl.create_bot(args.name + " A2", 1)
    cl.check("bots spawned", cl.wait_alive(a) and cl.wait_alive(b) and cl.wait_alive(a2))
    cl.run_for(1.0)
    ea = cl.states[a]
    kind, data = cl.teleport(b, (ea["pos"][0] + 25.0, ea["pos"][1] + 1.0, ea["pos"][2]), 270.0)
    cl.check(f"teleport B 25 m from A: {kind}", kind == "ack")
    cl.run_for(2.0)
    eb = cl.states[b]
    cl.check(f"B not spotted before (flags 0x{eb['flags']:02x})", not eb["flags"] & F_SPOTTED)
    kind, data = cl.spot(a, eb["root"])
    cl.check(f"A spots B (root {eb['root']}): {kind} {data}", kind == "ack")
    t0 = time.time()
    got = cl.run_for(2.0, lambda: cl.states[b]["flags"] & F_SPOTTED)
    cl.check(f"B's TickState entry has the spotted bit after {time.time() - t0:.2f} s", got)
    kind, data = cl.spot(a, eb["root"])
    cl.check(f"a second spot within a second is refused: {kind} {data}", kind == "error" and data[0] == 6)
    cl.run_for(1.1)
    kind, data = cl.spot(a, cl.states[a2]["root"])
    cl.check(f"spotting a teammate is refused: {kind} {data}", kind == "error" and data[0] == 5)
    cl.run_for(1.1)
    kind, data = cl.spot(a, 65000)
    cl.check(f"spotting an unknown id is refused: {kind} {data}", kind == "error" and data[0] == 5)
    cl.check("A's own entry is not spotted", not cl.states[a]["flags"] & F_SPOTTED)
    t_clear = time.time()
    cleared = cl.run_for(16.0, lambda: not cl.states[b]["flags"] & F_SPOTTED)
    cl.check(f"the spotted bit cleared {time.time() - t0:.1f} s after the spot", cleared and 10.0 < time.time() - t0 < 15.0)
    return cl.finish()


def run_round(args):
    """Item 7: RoundState after Welcome and on a round restart; server chat (game.sayAll / sayTeam
    over RCON) arrives as Chat. Needs --rcon-password and allowRoundRestart 1."""
    cl = Client(args, "serverext testclient round")
    cl.run_for(1.0, lambda: cl.rounds)
    cl.check(f"RoundState after Welcome: {cl.rounds[:1]}", cl.rounds)
    bot = cl.create_bot(args.name, 1)
    cl.wait_alive(bot)
    rc = Rcon("127.0.0.1", args.rcon_port, args.rcon_password)
    rc('exec game.sayAll "rdy"')
    rc('exec game.sayTeam 1 "go"')
    rc('exec game.sayTeam 2 "other team only"')
    cl.run_for(2.0, lambda: len(cl.chats) >= 2)
    texts = [(c["channel"], c["team"], c["text"]) for c in cl.chats]
    cl.check(f"Chat: sayAll 'rdy' on channel 12: {texts}", any(c[0] == 12 and c[2] == "rdy" for c in texts))
    cl.check("Chat: sayTeam 1 'go' on channel 13, team 1", any(c[0] == 13 and c[1] == 1 and c[2] == "go" for c in texts))
    cl.check("Chat: team 2's message not sent (no bot on team 2)", not any(c[2] == "other team only" for c in texts))
    n = len(cl.rounds)
    kind, data = cl.restart()
    cl.check(f"RestartRound: {kind} {data}", kind == "ack")
    cl.run_for(8.0)
    changes = cl.rounds[n:]
    cl.check(f"RoundState changes after the restart: {[(r['status'], r['begin_round']) for r in changes]}", changes)
    return cl.finish()


def run_teleport(args):
    """Item 9: teleport on foot and in a heli; values out of range are Error 7. With
    --expect-teleport-refused the server has allowTeleport 0 and every Teleport is Error 6."""
    cl = Client(args, "serverext testclient teleport")
    bot = cl.create_bot(args.name, args.team)
    cl.check("bot spawned", cl.wait_alive(bot))
    cl.run_for(1.0)
    e = cl.states[bot]
    if args.expect_teleport_refused or not cl.welcome["teleport"]:
        kind, data = cl.teleport(bot, e["pos"], 0.0)
        cl.check(f"Teleport refused with allowTeleport 0: {kind} {data}", kind == "error" and data[0] == 6)
        return cl.finish()
    target = (e["pos"][0] + 40.0, e["pos"][1] + 2.0, e["pos"][2] + 10.0)
    kind, data = cl.teleport(bot, target, 90.0)
    cl.check(f"teleport on foot: {kind} {data}", kind == "ack")
    cl.run_for(1.5)
    e = cl.states[bot]
    cl.check(f"on foot: now {flat_dist(e['pos'], target):.2f} m from the target (horizontal)", flat_dist(e["pos"], target) < 1.5)
    for label, pos, yaw in [("NaN", (float("nan"), 0, 0), 0.0), ("inf yaw", e["pos"], float("inf")),
                            ("x 9000", (9000.0, 100.0, 0.0), 0.0), ("y -2000", (0.0, -2000.0, 0.0), 0.0)]:
        kind, data = cl.teleport(bot, pos, yaw)
        cl.check(f"teleport {label}: {kind} {data}", kind == "error" and data[0] == 7)
    kind, data = cl.teleport(200, e["pos"], 0.0)
    cl.check(f"teleport a foreign player: {kind} {data}", kind == "error" and data[0] == 4)
    # Walk back is not needed: board the nearest heli from the spawn instead.
    cl.remove(bot)
    bot = cl.create_bot(args.name, args.team)
    if not cl.check("boarded a heli", ensure_boarded(cl, bot, args.team, "teleport")):
        return cl.finish()
    e = cl.states[bot]
    up = (e["pos"][0], e["pos"][1] + 120.0, e["pos"][2])
    hdg = seaward(e["pos"])
    kind, data = cl.teleport(bot, up, hdg)
    cl.check(f"teleport the heli 120 m up, yaw {hdg:.0f}: {kind} {data}", kind == "ack")
    cl.run_for(0.3)
    e = cl.states[bot]
    hspeed = math.hypot(e["vel"][0], e["vel"][2])
    cl.check(f"heli at {e['pos'][1] - up[1]:+.1f} m from the target height, yaw {e['ypr'][0]:.1f}, pitch {e['ypr'][1]:.1f}, "
             f"roll {e['ypr'][2]:.1f}, horizontal speed {hspeed:.1f} m/s, vertical {e['vel'][1]:+.1f} m/s (falling from "
             f"rest), still seat {e['seat']}",
             abs(e["pos"][1] - up[1]) < 5.0 and abs(wrap180(e["ypr"][0] - hdg)) < 3.0 and abs(e["ypr"][1]) < 3.0 and
             hspeed < 3.0 and cl.in_vehicle(bot))
    # Hover there, then land back with the watchdog: stop sending and let the neutral hold it.
    ap = Autopilot()
    climb_to(cl, bot, ap, up[1], "teleport hover", heading=hdg, timeout=20.0)
    return cl.finish()


def run_multiconn(args):
    """Item 3: several connections, each with its own bot. Foreign bots are refused (Error 4), the
    connection after maxConnections is closed without a frame, and closing one connection removes only
    its bots."""
    a = Client(args, "serverext testclient conn A")
    b = Client(args, "serverext testclient conn B")
    ba = a.create_bot(args.name + " A", 1)
    bb = b.create_bot(args.name + " B", 2)
    a.check("A's bot spawned", a.wait_alive(ba))
    b.check("B's bot spawned", b.wait_alive(bb))
    a.run_for(0.5)
    b.run_for(0.5)
    a.check(f"A's TickState has its bot as own and not B's on-foot bot ({sorted(a.states)})", ba in a.states and bb not in a.states)
    b.check(f"B's TickState has its bot as own ({sorted(b.states)})", bb in b.states and ba not in b.states)
    # B tries to drive, enter with, teleport, spot with and remove A's bot.
    b.c.send(set_input(ba, b.tick or 0, 0, 0, 0, 0))
    b.run_for(1.0, lambda: 0 in b.replies)
    kind, data = b.replies.pop(0, ("timeout", None))
    b.check(f"B SetInput for A's bot: {kind} {data}", kind == "error" and data[0] == 4)
    kind, data = b.enter(ba, 0, 0)
    b.check(f"B EnterVehicle with A's bot: {kind} {data}", kind == "error" and data[0] == 4)
    rid = b.next_req()
    b.c.send(frame(REMOVE_BOT, struct.pack("<IB", rid, ba)))
    kind, data = b.wait_reply(rid)
    b.check(f"B RemoveBot of A's bot: {kind} {data}", kind == "error" and data[0] == 4)
    kind, data = b.spot(ba, 1)
    b.check(f"B Spot with A's bot: {kind} {data}", kind == "error" and data[0] == 4)
    a.run_for(0.5)
    a.check("A's bot still exists", ba in a.states)
    # Fill the remaining connection slots; one more is closed without a frame.
    extra = []
    for i in range(args.max_connections - 2):
        c = Conn(args.host, args.port)
        hello(c, f"conn extra {i}")
        extra.append(c)
    over = Conn(args.host, args.port)
    try:
        f = over.recv_frame(timeout=2.0)
        a.check(f"connection {args.max_connections + 1} closed: still open after 2 s ({f!r})", False)
    except (ConnectionError, OSError):
        a.check(f"connection {args.max_connections + 1} closed by the server with no frame", True)
    over.close()
    for c in extra:
        c.close()
    # A's connection closes; its bot goes after the watchdog period, B's stays.
    a.c.close()
    b.run_for(2.0)
    b.check("B's bot still alive after A disconnected", b.alive(bb))
    c = Client(args, "serverext testclient conn C")
    bc = c.create_bot(args.name + " C", 1)
    c.check(f"a new connection gets its own bot ({bc})", c.wait_alive(bc))
    t0 = b.tick
    b.run_for(1.0)
    b.check(f"B's connection still gets TickStates (tick {t0} -> {b.tick})", b.tick and t0 and b.tick > t0)
    return max(finish_counts("connection A", a.results), b.finish(), c.finish())


def finish_counts(label, results):
    bad = sum(1 for _, ok in results if not ok)
    print(f"{label}: {len(results) - bad}/{len(results)} checks passed", flush=True)
    return 1 if bad else 0


def run_soak2(args):
    """10-minute soak with two bots and all v2 state: A (team 1) and B (team 2) hover over their pads;
    every 30 s A spots B (B is within range once both climb), every 45 s both cycle their camera views,
    every 60 s A fires a one-tick rocket seaward. Prints a per-minute summary."""
    cl = Client(args, "serverext testclient soak2")
    a = cl.create_bot(args.name + " A", 1)
    b = cl.create_bot(args.name + " B", 2)
    aps = {a: Autopilot(), b: Autopilot()}
    teams = {a: 1, b: 2}
    pads = {}
    heads = {}
    stats = dict(ts=0, gaps=0, last=None, deaths=0, boards=0, spots=0, spot_err=0)
    t_end = time.time() + args.soak_seconds
    t_next_min = time.time() + 60
    t_spot = time.time() + 30
    t_view = time.time() + 45
    t_fire = time.time() + 60

    def on_state():
        stats["ts"] += 1
        if stats["last"] is not None and cl.tick != stats["last"] + 1:
            stats["gaps"] += 1
        stats["last"] = cl.tick

    def ctl():
        now = time.time()
        for bot, ap in aps.items():
            e = cl.states.get(bot)
            if not (e and cl.in_vehicle(bot) and bot in pads):
                continue
            buttons = 0
            if bot == a and t_fire_now[0]:
                buttons |= BTN_FIRE
            cp, cr, c, ped = ap.step(e, cl.tick, alt=pads[bot] + 30.0, heading=heads[bot])
            cl.inputs[bot] = dict(pitch=cp, roll=cr, coll=c, ped=ped, buttons=buttons)
        t_fire_now[0] = False

    t_fire_now = [False]
    cl.on_state = on_state
    while time.time() < t_end:
        for bot in (a, b):
            if not cl.in_vehicle(bot):
                cl.before_send = None
                cl.inputs[bot] = {}
                if bot in pads:
                    stats["deaths"] += 1
                    pads.pop(bot)
                if ensure_boarded(cl, bot, teams[bot], f"soak bot {bot}"):
                    stats["boards"] += 1
                    pads[bot] = cl.states[bot]["pos"][1]
                    heads[bot] = seaward(cl.states[bot]["pos"])
                    aps[bot].reset()
                    cl.pulses[bot] = BTN_WEAPON[0]
        cl.before_send = ctl
        cl.pump(0.2)
        now = time.time()
        if now > t_spot and cl.alive(a) and cl.alive(b):
            t_spot = now + 30
            kind, data = cl.spot(a, cl.states[b]["root"])
            stats["spots" if kind == "ack" else "spot_err"] += 1
        if now > t_view:
            t_view = now + 45
            for bot in (a, b):
                cl.pulses[bot] = BTN_TOGGLE_CAMERA
        if now > t_fire and a in pads:
            t_fire = now + 60
            t_fire_now[0] = True
        if now > t_next_min:
            t_next_min = now + 60
            ea, eb = cl.states.get(a), cl.states.get(b)
            log(f"soak: {stats}; damages {len(cl.damages)} kills {len(cl.kills)} projectiles {len(cl.projectile_seen)} "
                f"chats {len(cl.chats)} rounds {len(cl.rounds)}; A {fmt_entry(ea) if ea else '-'} view {ea['view'] if ea else '-'} "
                f"weapons {[(w['slot'], w['ammo'], w['mags']) for w in ea['weapons']] if ea else '-'}; "
                f"B {fmt_entry(eb) if eb else '-'}")
    cl.before_send = None
    cl.on_state = None
    log(f"soak done: {stats}; damages {len(cl.damages)} kills {len(cl.kills)} projectiles seen {len(cl.projectile_seen)}")
    cl.check(f"soak: {stats['ts']} TickStates in {args.soak_seconds:.0f} s, {stats['gaps']} gaps",
             stats["ts"] > 25 * args.soak_seconds)
    cl.check(f"soak: spots acked {stats['spots']}, refused {stats['spot_err']}", stats["spots"] > 0)
    return cl.finish()


SCENARIOS_V2 = {"view": run_view, "weapons": run_weapons, "damage": run_damage, "spot": run_spot, "round": run_round,
                "teleport": run_teleport, "multiconn": run_multiconn, "soak2": run_soak2}


# ------------------------------------------------------------------------------------- v3

TV_LAUNCHERS = ("ahe_ah1z_cogunner_hellfirelaunchertv", "ahe_z10_hj8launcher_tv")
TV_PROJECTILE = "agm114_hellfire_tv"


def slot0(cl, bot):
    e = cl.states.get(bot)
    return next((w for w in e["weapons"] if w["slot"] == 0), None) if e else None


def slot0_name(cl, bot):
    w = slot0(cl, bot)
    return w["name"] if w else None


def has_tv(cl, bot):
    w = slot0(cl, bot)
    return w is not None and w["name"].lower() in TV_LAUNCHERS


def own_tv_missiles(cl, bot):
    return [p for p in cl.projectiles if p["name"].lower() == TV_PROJECTILE and p["owner"] == bot]


def wait_ticks(cl, n, timeout=10.0):
    start = cl.tick
    return cl.run_for(timeout, lambda: cl.tick is not None and start is not None and cl.tick >= start + n)


class Holder:
    """Holds pilots in a hover (altitude and heading) on every TickState through cl.before_send, so a
    scenario can drive gunners with pulses and steers meanwhile. aim, if set, is called on every
    TickState and returns {bot: (start, end)} for cl.steers."""

    def __init__(self, cl):
        self.cl = cl
        self.plans = {}
        self.aim = None
        cl.before_send = self.ctl

    def hold(self, bot, alt, heading):
        ap = self.plans[bot][0] if bot in self.plans else Autopilot()
        self.plans[bot] = (ap, dict(alt=alt, heading=heading))

    def ctl(self):
        for bot, (ap, mode) in self.plans.items():
            e = self.cl.states.get(bot)
            if not (e and self.cl.in_vehicle(bot)):
                self.cl.inputs[bot] = {}
                continue
            cp, cr, c, ped = ap.step(e, self.cl.tick, **mode)
            self.cl.inputs[bot] = dict(pitch=cp, roll=cr, coll=c, ped=ped)
        if self.aim:
            for bot, ray in self.aim().items():
                if ray:
                    self.cl.steers[bot] = ray
                else:
                    self.cl.steers.pop(bot, None)

    def stop(self):
        self.cl.before_send = None
        for bot in self.plans:
            self.cl.inputs[bot] = {}


def eye(cl, bot):
    """The gunner's eye for SetTargetDirection: its vehicle's position, 1.5 m up."""
    p = cl.states[bot]["pos"]
    return (p[0], p[1] + 1.5, p[2])


def ahead(pos, heading, dist, y):
    h = math.radians(heading)
    return (pos[0] + math.sin(h) * dist, y, pos[2] + math.cos(h) * dist)


def line_dist(p, a, b):
    """Distance from p to the infinite line through a and b."""
    d = [b[i] - a[i] for i in range(3)]
    w = [p[i] - a[i] for i in range(3)]
    dd = sum(x * x for x in d)
    t = sum(w[i] * d[i] for i in range(3)) / dd if dd else 0.0
    return math.sqrt(sum((w[i] - t * d[i]) ** 2 for i in range(3)))


def dist3(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def tv_shot(cl, holder, gunner, label, aim=None, retarget=None, retarget_after=1.0, csv=None):
    """Fires one TV missile from gunner (the TV launcher must be in slot 0). aim() -> (start, end), or None
    for no SetTargetDirection; it is re-evaluated on every TickState, before the launch and in flight.
    retarget() replaces aim retarget_after seconds after the launch. Returns a dict describing the shot."""
    ready = cl.run_for(20.0, lambda: has_tv(cl, gunner) and slot0(cl, gunner)["ammo"] > 0 and
                       not slot0(cl, gunner)["reloading"] and not own_tv_missiles(cl, gunner))
    if not ready:
        log(f"{label}: TV launcher not ready: {slot0(cl, gunner)}")
        return None
    current = [aim]
    holder.aim = (lambda: {gunner: current[0]() if current[0] else None})
    if aim:
        wait_ticks(cl, 6)          # the direction is applied before the trigger is pressed
    w0 = slot0(cl, gunner)
    cl.pulses[gunner] = BTN_FIRE
    press_tick = cl.tick
    t0 = time.time()
    track = []
    launched = None
    gone_since = None
    errors_before = len(cl.errors0)
    while time.time() - t0 < 9.0:
        n = cl.pl_count
        cl.pump(0.2)
        if cl.pl_count == n:
            continue
        mine = own_tv_missiles(cl, gunner)
        for p in mine:
            track.append((cl.tick, p))
        if mine and launched is None:
            launched = time.time()
        if retarget and launched and time.time() - launched >= retarget_after and current[0] is not retarget:
            current[0] = retarget
            log(f"{label}: retargeted at tick {cl.tick}")
        if launched and not mine:
            gone_since = gone_since or time.time()
            if time.time() - gone_since > 0.5:
                break
    holder.aim = None
    cl.steers.pop(gunner, None)
    out = dict(label=label, launched=bool(track), track=track, press_tick=press_tick, weapon_before=w0,
               weapon_after=slot0(cl, gunner), errors=cl.errors0[errors_before:])
    if not track:
        log(f"{label}: no TV missile seen; slot 0 before {w0}, after {out['weapon_after']}")
        return out
    first, last = track[0][1], track[-1][1]
    out.update(first=first, last=last, first_tick=track[0][0], last_tick=track[-1][0], guided=first["guided"],
               own=first["own"], ids=sorted(set(p["id"] for _, p in track)))
    log(f"{label}: missile {first['id']} launched at tick {track[0][0]} (fire pressed after tick {press_tick}), "
        f"guided {first['guided']}, own {first['own']}, target {first['target']}; first at "
        f"{tuple(round(v, 1) for v in first['pos'])} v {tuple(round(v, 1) for v in first['vel'])}; last at "
        f"{tuple(round(v, 1) for v in last['pos'])} after {(track[-1][0] - track[0][0]) / 30.0:.2f} s, ttl {last['ttl']:.2f}")
    if csv:
        with open(csv, "a") as f:
            for tick, p in track:
                f.write(f"{label},{p['id']},{tick},{p['pos'][0]:.2f},{p['pos'][1]:.2f},{p['pos'][2]:.2f},"
                        f"{p['vel'][0]:.2f},{p['vel'][1]:.2f},{p['vel'][2]:.2f},{p['ttl']:.3f},{p['target']}\n")
    return out


def climb_hold(cl, holder, pilot, alt, heading, label, timeout=60.0):
    holder.hold(pilot, alt, heading)
    ok = cl.run_for(timeout, lambda: cl.in_vehicle(pilot) and abs(cl.states[pilot]["pos"][1] - alt) < 4.0 and
                    math.sqrt(sum(c * c for c in cl.states[pilot]["vel"])) < 3.0)
    e = cl.states.get(pilot)
    log(f"{label}: hover at {alt:.0f} m: {ok}; y {e['pos'][1]:.1f}, heading {e['ypr'][0]:.0f}")
    return ok


def board_crew(cl, team, name):
    """A pilot and a gunner in one heli; returns (pilot, gunner, heli) or None."""
    pilot = cl.create_bot(name + " pilot", team)
    gunner = cl.create_bot(name + " gunner", team)
    if not cl.check("pilot boarded", ensure_boarded(cl, pilot, team, "pilot")):
        return None
    heli = cl.states[pilot]["root"]
    hname = cl.vehicles.get(heli, {}).get("name", "?")
    if not cl.check(f"gunner boarded seat 1 of {hname} ({heli})",
                    ensure_boarded(cl, gunner, team, "gunner", seat=1, vehicle_id=heli, spinup=0.5)):
        return None
    cl.run_for(1.0)
    return pilot, gunner, heli


def run_tv(args):
    """Items 2 and 3. On the pad: v2's sequence (weapon select 2, alt fire held) launches nothing; one tick
    of alt fire toggles the gunner's slot 0 between the gun and the TV launcher. In a hover: an unguided
    shot (where the missile goes with no SetTargetDirection), a shot steered onto the sea, a shot
    retargeted in flight, refusals, and with --tv-target a shot steered onto an own bot of the other team."""
    cl = Client(args, "serverext testclient tv")
    crew = board_crew(cl, args.team, args.name)
    if not crew:
        return cl.finish()
    pilot, gunner, heli = crew
    log(f"gunner weapons: {[(w['slot'], w['name'], w['ammo'], w['mags']) for w in cl.states[gunner]['weapons']]}, "
        f"view {cl.states[gunner]['view']}")
    cl.check(f"gunner slot 0 starts at the gun: {slot0_name(cl, gunner)}", slot0(cl, gunner) and not has_tv(cl, gunner))

    # Refused while the gun is active.
    errs = len(cl.errors0)
    cl.steers[gunner] = (eye(cl, gunner), ahead(eye(cl, gunner), 0.0, 100.0, 0.0))
    cl.run_for(0.5)
    cl.steers.pop(gunner, None)
    got = [e for e in cl.errors0[errs:]]
    cl.check(f"SetTargetDirection with the gun active is refused: {got[:1]}", got and got[0][2] == 6)

    # Item 2: v2's sequence.
    pl_before = cl.pl_count
    seen = {p["id"] for p in cl.projectiles}
    cl.pulses[gunner] = BTN_WEAPON[1]
    cl.run_for(1.0)
    cl.check(f"weapon select 2 leaves slot 0 at {slot0_name(cl, gunner)} (the TV launcher is isSelectable 0)",
             not has_tv(cl, gunner))
    launched = []
    cl.on_state = lambda: launched.extend(p for p in own_tv_missiles(cl, gunner) if p["id"] not in seen)
    cl.inputs[gunner] = dict(buttons=BTN_ALT_FIRE)
    cl.run_for(1.5)
    cl.inputs[gunner] = {}
    cl.run_for(1.0)
    cl.on_state = None
    cl.check(f"alt fire held 1.5 s toggled slot 0 once, to {slot0_name(cl, gunner)}", has_tv(cl, gunner))
    cl.check(f"... and launched nothing ({len(launched)} TV missiles)", not launched)
    log(f"gunner view mode with the TV launcher: {cl.states[gunner]['view']}")
    cl.pulses[gunner] = BTN_ALT_FIRE
    cl.run_for(1.0)
    cl.check(f"one tick of alt fire toggles back to {slot0_name(cl, gunner)}", not has_tv(cl, gunner))
    cl.pulses[gunner] = BTN_ALT_FIRE
    cl.run_for(1.0)
    cl.check(f"one more tick toggles to {slot0_name(cl, gunner)}", has_tv(cl, gunner))
    cl.pulses[gunner] = BTN_TOGGLE_WEAPON
    cl.run_for(1.0)
    cl.check(f"next weapon (bit 22) does not leave the TV launcher: {slot0_name(cl, gunner)}", has_tv(cl, gunner))

    # Up to a hover, nose seaward.
    holder = Holder(cl)
    pad = cl.states[pilot]["pos"]
    hdg = seaward(pad)
    alt = pad[1] + 60.0
    if not cl.check("hover reached", climb_hold(cl, holder, pilot, alt, hdg, "tv")):
        holder.stop()
        return cl.finish()
    shots = []
    csv = args.trace_csv or None

    # Unguided: no SetTargetDirection on this server process yet (run this scenario first after a start).
    s = tv_shot(cl, holder, gunner, "unguided", aim=None, csv=csv)
    shots.append(s)
    if cl.check("unguided: a TV missile launched (fire, one tick)", s and s["launched"]):
        cl.check(f"unguided: ProjectileList flags it guided and own ({s['guided']}, {s['own']})", s["guided"] and s["own"])
        l0, l1 = s["first"]["pos"], s["last"]["pos"]
        log(f"unguided: heli heading {hdg:.0f}, missile went towards {bearing(l0, l1):.0f} (world origin at "
            f"{bearing(l0, (0.0, 0.0, 0.0)):.0f}), {flat_dist(l0, l1):.0f} m horizontally, {l0[1] - l1[1]:.0f} m down, "
            f"last y {l1[1]:.1f}, ttl {s['last']['ttl']:.2f}")
        w = s["weapon_after"]
        cl.check(f"after the launch the launcher is empty or reloading: {w}", w and (w["ammo"] == 0 or w["reloading"]))

    def report(label, s, aim_pt, other=None):
        d_pt = dist3(s["last"]["pos"], aim_pt)
        line = f"{label}: aim {tuple(round(v, 1) for v in aim_pt)}; last seen {tuple(round(v, 1) for v in s['last']['pos'])} " \
               f"after {(s['last_tick'] - s['first_tick']) / 30.0:.2f} s, {d_pt:.1f} m from the aim point"
        if other:
            line += f", {dist3(s['last']['pos'], other):.1f} m from the first aim point"
        log(line)
        return d_pt

    sea = args.sea_level
    # Direction sent only before the launch: does the missile still go there once the stream stops?
    pre_pt = ahead(cl.states[pilot]["pos"], hdg - 20.0, args.tv_range, sea)
    start0 = eye(cl, gunner)
    holder.aim = lambda: {gunner: (eye(cl, gunner), pre_pt)}
    wait_ticks(cl, 6)
    holder.aim = None
    cl.steers.pop(gunner, None)
    wait_ticks(cl, 30)
    s = tv_shot(cl, holder, gunner, "pre-launch only", aim=None, csv=csv)
    shots.append(s)
    if cl.check("pre-launch only: a TV missile launched", s and s["launched"]):
        d = report("pre-launch only", s, pre_pt)
        log(f"pre-launch only: {line_dist(s['last']['pos'], start0, pre_pt):.1f} m from the aim line")

    # Steered on every tick onto the sea, args.tv_range ahead and 20 degrees right.
    aim_pt = ahead(cl.states[pilot]["pos"], hdg + 20.0, args.tv_range, sea)
    s = tv_shot(cl, holder, gunner, "sea", aim=lambda: (eye(cl, gunner), aim_pt), csv=csv)
    shots.append(s)
    if cl.check("sea: a TV missile launched", s and s["launched"]):
        d = report("sea", s, aim_pt)
        cl.check(f"sea: the missile ended {d:.1f} m from the aim point on the sea (< 15 m)", d < 15.0)
        cl.check(f"sea: no refusal while steering ({s['errors'][:1]})", not s["errors"])

    # Retargeted in flight: launched at a point 10 degrees left, switched 0.5 s later to 10 degrees right
    # (about 90 m apart). The missile turns at most about 1.5 rad/s (follow.maxYaw), so a much larger or
    # later switch cannot be made before it reaches the sea.
    p1 = ahead(cl.states[pilot]["pos"], hdg - 10.0, args.tv_range, sea)
    p2 = ahead(cl.states[pilot]["pos"], hdg + 10.0, args.tv_range, sea)
    s = tv_shot(cl, holder, gunner, "retarget", aim=lambda: (eye(cl, gunner), p1),
                retarget=lambda: (eye(cl, gunner), p2), retarget_after=0.5, csv=csv)
    shots.append(s)
    if cl.check("retarget: a TV missile launched", s and s["launched"]):
        d2 = report("retarget", s, p2, other=p1)
        d1 = dist3(s["last"]["pos"], p1)
        cl.check(f"retarget: the missile followed the new direction ({d2:.1f} m from the new point, {d1:.1f} m from "
                 f"the first)", d2 < 15.0 and d1 > 50.0)

    # Steered along a sweep: the end point moves 2 degrees a tick from 25 degrees left to 25 right of the
    # nose; the missile's bearing from the heli follows it.
    t_sweep = [None]

    def sweep():
        if t_sweep[0] is None:
            t_sweep[0] = cl.tick
        ang = hdg - 25.0 + min(50.0, 2.0 * (cl.tick - t_sweep[0]) / 3.0)
        return eye(cl, gunner), ahead(cl.states[pilot]["pos"], ang, args.tv_range, sea)

    s = tv_shot(cl, holder, gunner, "sweep", aim=sweep, csv=csv)
    shots.append(s)
    if cl.check("sweep: a TV missile launched", s and s["launched"]):
        hp = cl.states[pilot]["pos"]
        bearings = [wrap180(bearing(hp, p["pos"]) - hdg) for _, p in s["track"][::6]]
        log(f"sweep: missile bearing from the heli relative to the nose every 0.2 s: "
            f"{[round(b) for b in bearings]}")
        final = ahead(hp, hdg + 25.0, args.tv_range, sea)
        d = report("sweep", s, final)
        low = min(bearings[1:])
        cl.check(f"sweep: the missile's bearing followed the sweep (from {low:.0f} up to {bearings[-1]:.0f}, the sweep "
                 f"ended at +25; {d:.1f} m from its last point)", bearings[-1] - low > 20.0 and abs(bearings[-1] - 25.0) < 10.0)

    if args.tv_target:
        shots.append(tv_target(cl, holder, gunner, pilot, hdg, alt, args, csv))

    # The launcher back to the gun: a direction is refused again.
    cl.pulses[gunner] = BTN_ALT_FIRE
    cl.run_for(1.0)
    errs = len(cl.errors0)
    cl.steers[gunner] = (eye(cl, gunner), ahead(eye(cl, gunner), hdg, 100.0, 0.0))
    cl.run_for(0.5)
    cl.steers.pop(gunner, None)
    got = cl.errors0[errs:]
    cl.check(f"after toggling back to {slot0_name(cl, gunner)} a direction is refused: {got[:1]}", got and got[0][2] == 6)
    holder.stop()
    log("TV shots: " + "; ".join(f"{s['label']}: launched {s['launched']}" for s in shots if s))
    return cl.finish()


def tv_target(cl, holder, gunner, pilot, hdg, alt, args, csv):
    """Steers a TV missile onto an own bot of the other team, teleported into a hover args.tv_range ahead."""
    other = 3 - args.team
    t = cl.create_bot(args.name + " target", other)
    if not cl.check("target bot boarded a heli", ensure_boarded(cl, t, other, "target")):
        return None
    pos = ahead(cl.states[pilot]["pos"], hdg, args.tv_range, alt)
    kind, data = cl.teleport(t, pos, (hdg + 180.0) % 360.0)
    if not cl.check(f"target heli teleported {args.tv_range:.0f} m ahead: {kind} {data}", kind == "ack"):
        return None
    holder.hold(t, alt, (hdg + 180.0) % 360.0)
    cl.run_for(4.0)
    et = cl.states[t]
    log(f"target: {tuple(round(v, 1) for v in et['pos'])}, {flat_dist(et['pos'], cl.states[pilot]['pos']):.0f} m from the "
        f"gunner's heli, health {et['health']:.2f}")
    d0, k0 = len(cl.damages), len(cl.kills)
    s = tv_shot(cl, holder, gunner, "own bot",
                aim=lambda: (eye(cl, gunner), cl.states[t]["pos"]) if cl.alive(t) else None, csv=csv)
    cl.run_for(1.0)
    hits = [d for d in cl.damages[d0:] if d["attacker"] == gunner and d["victim"] == t]
    kills = [k for k in cl.kills[k0:] if k["attacker"] == gunner and k["victim"] == t]
    for d in hits[:3]:
        log(f"  Damage {d}")
    for k in kills[:3]:
        log(f"  Kill {k}")
    cl.check(f"own bot: the missile hit the target ({len(hits)} Damage events from the gunner, sources "
             f"{sorted(set(d['source'] for d in hits))})", hits)
    cl.check(f"own bot: the target was killed ({len(kills)} Kill events)", kills)
    holder.plans.pop(t, None)
    cl.inputs[t] = {}
    return s


def run_burst(args):
    """Item 1: a one-tick camera press survives a SetInput that supersedes it in the same tick. Each press
    is sent with one or two more SetInputs for the bot in the same write; every press must change the
    view mode in TickState."""
    cl = Client(args, "serverext testclient burst")
    bot = cl.create_bot(args.name, args.team)
    if not cl.check("boarded a heli", ensure_boarded(cl, bot, args.team, "burst", spinup=1.0)):
        return cl.finish()
    cl.run_for(1.0)
    # Camera mode keys 2 and 3 give view modes 3 and 4 (module-v1.md Q11); both are idempotent, so
    # alternating them changes the view with every press.
    keys = [(BTN_CAMERA_MODE[1], 3), (BTN_CAMERA_MODE[2], 4)]
    cl.pulses[bot] = BTN_CAMERA_MODE[1]
    cl.run_for(1.0)
    log(f"start view mode {cl.states[bot]['view']}")
    patterns = [
        ("press, then a SetInput without buttons", lambda k: (k, [dict()])),
        ("press, then two SetInputs without buttons", lambda k: (k, [dict(), dict()])),
        ("no buttons, the press, then no buttons", lambda k: (0, [dict(buttons=k), dict()])),
        ("press, then a SetInput with another button (flares)", lambda k: (k, [dict(buttons=BTN_FLARE)])),
    ]
    total_lost = 0
    for name, make in patterns:
        lost = 0
        lag = []
        for i in range(args.burst_presses):
            key, want = keys[(i + 1) % 2]
            first, extra = make(key)
            if first:
                cl.pulses[bot] = first
            cl.extra[bot] = extra
            sent = cl.tick
            changed = cl.run_for(1.0, lambda: cl.states[bot]["view"] == want)
            if changed:
                lag.append(cl.tick - sent)
            else:
                lost += 1
                log(f"  {name}: press {i} after tick {sent} did not reach view {want} (view {cl.states[bot]['view']})")
                cl.pulses[bot] = key      # resync for the next press
                cl.run_for(1.0, lambda: cl.states[bot]["view"] == want)
            wait_ticks(cl, 3)
        total_lost += lost
        lag_txt = f"lag {min(lag)}-{max(lag)} ticks" if lag else "no changes"
        cl.check(f"{name}: {args.burst_presses - lost}/{args.burst_presses} presses changed the view ({lag_txt})", lost == 0)
    log(f"burst: {total_lost} presses lost in {len(patterns) * args.burst_presses}")
    return cl.finish()


def run_soak3(args):
    """The v3 soak: a pilot and a gunner in one heli hovering seaward of the pad; every --tv-every seconds (75:
    the 8 missiles a sortie carries last the 10 minutes)
    the gunner fires a TV missile and guides it onto the sea (alternating sides, retargeted in flight on
    every other pair of shots, 0.5 s after launch); every 45 s a burst of camera presses in superseded SetInputs."""
    cl = Client(args, "serverext testclient soak3")
    crew = board_crew(cl, args.team, args.name)
    if not crew:
        return cl.finish()
    pilot, gunner, heli = crew
    cl.pulses[gunner] = BTN_ALT_FIRE
    cl.run_for(1.0)
    cl.check(f"gunner on the TV launcher: {slot0_name(cl, gunner)}", has_tv(cl, gunner))
    holder = Holder(cl)
    pad = cl.states[pilot]["pos"]
    hdg = seaward(pad)
    alt = pad[1] + 60.0
    if not cl.check("hover reached", climb_hold(cl, holder, pilot, alt, hdg, "soak3")):
        holder.stop()
        return cl.finish()
    ticks = []
    cl.on_state = lambda: ticks.append(cl.tick)
    t0 = time.time()
    next_shot = t0 + 5.0
    next_burst = t0 + 20.0
    shots, hits, presses, lost = [], 0, 0, 0
    deaths = 0
    was_alive = True
    while time.time() - t0 < args.soak_seconds:
        cl.pump(0.2)
        alive = cl.in_vehicle(pilot) and cl.in_vehicle(gunner)
        if not alive:
            if was_alive:
                deaths += 1
                log(f"soak3: crew out of the heli at {time.time() - t0:.0f} s")
            was_alive = False
            continue
        was_alive = True
        if time.time() >= next_burst:
            next_burst += 45.0
            for i in range(4):
                key, want = [(BTN_CAMERA_MODE[1], 3), (BTN_CAMERA_MODE[2], 4)][i % 2]
                cl.pulses[pilot] = key
                cl.extra[pilot] = [dict()]
                presses += 1
                if not cl.run_for(1.0, lambda: cl.states[pilot]["view"] == want):
                    lost += 1
                wait_ticks(cl, 3)
        if time.time() >= next_shot:
            next_shot += args.tv_every
            n = len(shots)
            side = 10.0 if n % 2 else -10.0
            p1 = ahead(cl.states[pilot]["pos"], hdg + side, args.tv_range, args.sea_level)
            p2 = ahead(cl.states[pilot]["pos"], hdg - side, args.tv_range, args.sea_level)
            final = p2 if n % 4 >= 2 else p1
            s = tv_shot(cl, holder, gunner, f"soak shot {n + 1}", aim=lambda: (eye(cl, gunner), p1),
                        retarget=(lambda: (eye(cl, gunner), p2)) if n % 4 >= 2 else None, retarget_after=0.5,
                        csv=args.trace_csv or None)
            shots.append(s)
            if s and s["launched"]:
                d = line_dist(s["last"]["pos"], eye(cl, gunner), final)
                hits += d < 15.0
                log(f"soak shot {n + 1}: guided {s['guided']}, ended {d:.1f} m from the aim line")
    cl.on_state = None
    holder.stop()
    gaps = sum(1 for a, b in zip(ticks, ticks[1:]) if b != a + 1)
    launched = sum(1 for s in shots if s and s["launched"])
    cl.check(f"soak3: {len(ticks)} TickStates in {args.soak_seconds:.0f} s, {gaps} gaps", gaps == 0)
    cl.check(f"soak3: {launched}/{len(shots)} TV shots launched, {hits} ended within 15 m of the aim line", launched == len(shots) and hits == launched)
    cl.check(f"soak3: {presses - lost}/{presses} camera presses in superseded SetInputs changed the view", lost == 0)
    cl.check(f"soak3: crew stayed in the heli ({deaths} losses)", deaths == 0)
    cl.check(f"soak3: {len(cl.errors0)} errors without a request", not cl.errors0)
    return cl.finish()


def run_q12(args):
    """Q12 (docs/findings/q12-tv-refusal.md): is a TV launch refused while another launcher's TV missile flies?
    Two crews (pilot and gunner bots in each team's heli) hover seaward of their pads and fire unguided TV
    missiles onto the sea: A then B while A's flies, the reverse, B after A's has ended, and both in one tick.
    A refused press is retried once after the other missile has ended."""
    cl = Client(args, "serverext testclient q12")
    crews = {}
    for team, tag in ((1, "A"), (2, "B")):
        crew = board_crew(cl, team, f"{args.name} {tag}")
        if not crew:
            return cl.finish()
        crews[tag] = crew
    gunners = {tag: c[1] for tag, c in crews.items()}
    for tag, g in gunners.items():
        cl.pulses[g] = BTN_ALT_FIRE
    cl.run_for(1.5)
    for tag, g in gunners.items():
        cl.check(f"gunner {tag} ({g}) on the TV launcher: {slot0_name(cl, g)}", has_tv(cl, g))
    holder = Holder(cl)
    for tag, (pilot, gunner, heli) in crews.items():
        pad = cl.states[pilot]["pos"]
        holder.hold(pilot, pad[1] + 60.0, seaward(pad))
    ok = cl.run_for(60.0, lambda: all(
        cl.in_vehicle(p) and abs(cl.states[p]["pos"][1] - holder.plans[p][1]["alt"]) < 4.0 and
        math.sqrt(sum(c * c for c in cl.states[p]["vel"])) < 3.0 for p, _, _ in crews.values()))
    if not cl.check("both crews hovering", ok):
        holder.stop()
        return cl.finish()

    def flying(g):
        return own_tv_missiles(cl, g)

    def ready(g):
        w = slot0(cl, g)
        return has_tv(cl, g) and w["ammo"] > 0 and not w["reloading"] and not flying(g)

    def all_quiet():
        return not any(p["name"].lower() == TV_PROJECTILE for p in cl.projectiles)

    def wait_ready(tags):
        if not cl.run_for(25.0, lambda: all(ready(gunners[t]) for t in tags) and all_quiet()):
            log(f"q12: not ready: {[(t, slot0(cl, gunners[t])) for t in tags]}")
            return False
        wait_ticks(cl, 15)
        return True

    def press(tags, label):
        """One tick of fire for each gunner in tags (the same TickState); returns {tag: launch tick or None}."""
        before = {t: bool(flying(gunners[t])) for t in tags}
        for t in tags:
            cl.pulses[gunners[t]] = BTN_FIRE
        press_tick = cl.tick
        out = {t: None for t in tags}

        def seen():
            for t in tags:
                if out[t] is None and not before[t] and flying(gunners[t]):
                    p = flying(gunners[t])[0]
                    out[t] = cl.tick
                    log(f"{label}: {t} launched missile {p['id']} at tick {cl.tick} (pressed after {press_tick}), "
                        f"ttl {p['ttl']:.2f}")
            return all(v is not None for v in out.values())
        cl.run_for(1.5, seen)
        others = [(p["id"], p["owner"], round(p["ttl"], 2)) for p in cl.projectiles if p["name"].lower() == TV_PROJECTILE]
        for t in tags:
            if out[t] is None:
                log(f"{label}: {t} pressed after tick {press_tick}: NO launch; TV missiles in flight {others}; "
                    f"slot 0 {slot0(cl, gunners[t])}")
        return out

    def in_flight_of(t):
        return [(p["id"], round(p["ttl"], 2)) for p in flying(gunners[t])]

    def overlap(first, second, label):
        if not wait_ready([first, second]):
            return cl.check(f"{label}: launchers ready", False)
        r1 = press([first], f"{label} {first}")
        if not cl.check(f"{label}: {first} launched", r1[first] is not None):
            return False
        wait_ticks(cl, 15)
        mid = in_flight_of(first)
        log(f"{label}: {first}'s missile in flight before {second} presses: {mid}")
        r2 = press([second], f"{label} {second}")
        launched = r2[second] is not None
        cl.check(f"{label}: {second} launched while {first}'s missile flew ({mid}): {launched}", launched)
        if not launched:
            cl.run_for(10.0, lambda: not flying(gunners[first]))
            wait_ticks(cl, 15)
            r3 = press([second], f"{label} {second} retry")
            cl.check(f"{label}: {second}'s retry after {first}'s missile ended launched", r3[second] is not None)
        cl.run_for(10.0, all_quiet)
        return launched

    overlap("A", "B", "case 1 (A, then B in flight)")
    overlap("B", "A", "case 2 (B, then A in flight)")

    label = "case 3 (B after A's ended)"
    if cl.check(f"{label}: launchers ready", wait_ready(["A", "B"])):
        r1 = press(["A"], f"{label} A")
        cl.check(f"{label}: A launched", r1["A"] is not None)
        cl.run_for(10.0, lambda: not flying(gunners["A"]))
        wait_ticks(cl, 30)
        r2 = press(["B"], f"{label} B")
        cl.check(f"{label}: B launched after A's missile ended", r2["B"] is not None)
        cl.run_for(10.0, all_quiet)

    label = "case 4 (A and B in one tick)"
    if cl.check(f"{label}: launchers ready", wait_ready(["A", "B"])):
        r = press(["A", "B"], label)
        cl.check(f"{label}: both launched: {r}", all(v is not None for v in r.values()))
        cl.run_for(10.0, all_quiet)

    holder.stop()
    cl.check(f"q12: {len(cl.errors0)} errors without a request", not cl.errors0)
    return cl.finish()


SCENARIOS_V3 = {"burst": run_burst, "tv": run_tv, "soak3": run_soak3, "q12": run_q12}


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
    case("Hello version 1 (v1 client)", frame(HELLO, struct.pack("<H", 1) + pstr("x")), 1, hello_first=False)
    case("Hello version 2 (v2 client)", frame(HELLO, struct.pack("<H", 2) + pstr("x")), 1, hello_first=False)
    case("Hello version 4", frame(HELLO, struct.pack("<H", 4) + pstr("x")), 1, hello_first=False)
    case("SetTargetDirection before Hello", target_direction(255, 0, (0, 0, 0), (1, 1, 1)), 2, hello_first=False)
    case("short SetTargetDirection (28 bytes)", frame(SET_TARGET_DIRECTION, b"\x00" * 28), 2)
    case("long SetTargetDirection (30 bytes)", frame(SET_TARGET_DIRECTION, b"\x00" * 30), 2)
    case("empty SetTargetDirection", frame(SET_TARGET_DIRECTION, b""), 2)
    case("Teleport before Hello", teleport_frame(1, 255, (0, 0, 0), 0), 2, hello_first=False)
    case("Spot before Hello", spot_frame(1, 255, 1), 2, hello_first=False)
    case("short Teleport (20 bytes)", frame(TELEPORT, b"\x00" * 20), 2)
    case("long Teleport (22 bytes)", frame(TELEPORT, b"\x00" * 22), 2)
    case("short Spot (8 bytes)", frame(SPOT, b"\x00" * 8), 2)
    case("long Spot (10 bytes)", frame(SPOT, b"\x00" * 10), 2)
    case("empty Teleport", frame(TELEPORT, b""), 2)
    for mt in (PROJECTILE_LIST, DAMAGE, KILL, CHAT, ROUND_STATE):
        case(f"module-only type {NAMES[mt]}", frame(mt, b"\x00" * 10), 3)
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

    # maxConnections concurrent connections are served; one more is refused. Per-request errors keep
    # the connection.
    time.sleep(1.0)    # let the module see the quick closes above (module-v1/q5-q9: a race in the test)
    c = Conn(H, P)
    hello(c, "fuzz-main")
    others = []
    for i in range(args.max_connections - 1):
        o = Conn(H, P)
        hello(o, f"fuzz-extra-{i}")
        others.append(o)
    results.append((f"{args.max_connections} concurrent connections each got a Welcome", True))
    c2 = Conn(H, P)
    try:
        got = c2.recv_frame(timeout=2.0)
        results.append((f"connection {args.max_connections + 1}: still open after 2 s ({got!r})", False))
    except (ConnectionError, OSError):
        results.append((f"connection {args.max_connections + 1}: closed by the server", True))
    c2.close()
    for o in others:
        o.close()

    def want_error(label, payload, code):
        codes = code if isinstance(code, tuple) else (code,)
        c.send(payload)
        end = time.time() + 3
        while time.time() < end:
            f = c.recv_frame(timeout=1)
            if f and f[0] == ERROR:
                got = parse_error(f[1])
                results.append((f"{label}: Error code {got[1]} '{got[2]}', connection kept", got[1] in codes))
                return
        results.append((f"{label}: no Error", False))

    want_error("SetInput unknown player", set_input(3, 0, 0, 0, 0, 0), 4)
    want_error("CreateBot team 3", frame(CREATE_BOT, struct.pack("<I", 50) + pstr("[BOT] x") + b"\x03"), 7)
    want_error("CreateBot empty name", frame(CREATE_BOT, struct.pack("<I", 51) + pstr("") + b"\x01"), 7)
    want_error("EnterVehicle foreign player", frame(ENTER_VEHICLE, struct.pack("<IBIB", 52, 1, 0, 0)), 4)
    want_error("RemoveBot foreign player", frame(REMOVE_BOT, struct.pack("<IB", 53, 1)), 4)
    if args.expect_restart_refused:
        want_error("RestartRound with allowRoundRestart 0", frame(RESTART_ROUND, struct.pack("<I", 54)), 6)
    tp_refused = args.expect_teleport_refused
    want_error("Teleport foreign player", teleport_frame(55, 1, (0, 50, 0), 0), 6 if tp_refused else 4)
    want_error("Spot foreign player", spot_frame(56, 1, 1), 4)
    want_error("SetTargetDirection foreign player", target_direction(1, 0, (0, 50, 0), (10, 0, 10)), 4)

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
        nan = float("nan")
        want_error("Teleport NaN x", teleport_frame(64, bot, (nan, 0, 0), 0), 6 if tp_refused else 7)
        want_error("Teleport -inf yaw", teleport_frame(65, bot, (0, 50, 0), float("-inf")), 6 if tp_refused else 7)
        want_error("Teleport x 1e9", teleport_frame(66, bot, (1e9, 50, 0), 0), 6 if tp_refused else 7)
        want_error("Spot target 0", spot_frame(67, bot, 0), (5, 6))
        want_error("Spot target 70000", spot_frame(68, bot, 70000), (5, 6))
        # v3 SetTargetDirection: bad values are 7; a bot without an active TV-guided weapon is 6.
        want_error("SetTargetDirection NaN start", target_direction(bot, 0, (nan, 50, 0), (10, 0, 10)), 7)
        want_error("SetTargetDirection +inf end", target_direction(bot, 0, (0, 50, 0), (float("inf"), 0, 10)), 7)
        want_error("SetTargetDirection end x 9000", target_direction(bot, 0, (0, 50, 0), (9000, 0, 10)), 7)
        want_error("SetTargetDirection start y -2000", target_direction(bot, 0, (0, -2000, 0), (10, 0, 10)), 7)
        want_error("SetTargetDirection start == end", target_direction(bot, 0, (5, 50, 5), (5, 50, 5)), 7)
        want_error("SetTargetDirection without a TV weapon", target_direction(bot, 0, (0, 50, 0), (10, 0, 10)), 6)
        # A flood of directions: latest wins per tick, so the server applies at most one a tick.
        try:
            c.send(target_direction(bot, 0, (0, 50, 0), (10, 0, 10)) * 3000)
            results.append(("3000-frame SetTargetDirection burst sent", True))
        except OSError as ex:
            results.append((f"3000-frame SetTargetDirection burst: {ex}", True))
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
    ap.add_argument("--scenario", choices=["hover", "walk", "buttons", "blockpad", "allblocked", "wreck", "q9", "q5", "teamswitch",
                                           "view", "weapons", "damage", "spot", "round", "teleport", "multiconn", "soak2",
                                           "burst", "tv", "soak3", "q12"],
                    default="hover")
    ap.add_argument("--rcon-port", type=int, default=4723, help="q9: ModManager RCON port")
    ap.add_argument("--rcon-password", default="", help="q9: ModManager RCON password")
    ap.add_argument("--q5-trials", default="hover,climb,descent,forward,turn")
    ap.add_argument("--q5-repeat", type=int, default=1)
    ap.add_argument("--q5-neutral", default="module",
                    help="q5: comma list of neutrals after the cut: module (the real watchdog), level, hover")
    ap.add_argument("--q5-record", type=float, default=20.0, help="q5: seconds recorded after each cut")
    ap.add_argument("--q5-fwd-pitch", type=float, default=8.0, help="q5: forward trial pitch, degrees nose down")
    ap.add_argument("--q5-turn-pitch", type=float, default=5.0, help="q5: turn trial pitch, degrees nose down")
    ap.add_argument("--q5-att", default="0.06,0.04,0.02", help="q5 autopilot attitude kp,kd,ki")
    ap.add_argument("--q5-vel", default="1.5,0.3", help="q5 autopilot speed kv,kvi")
    ap.add_argument("--q5-bank", type=float, default=30.0, help="q5: turn trial bank angle, degrees (+ = left)")
    ap.add_argument("--q5-csv", default="", help="q5: append every recorded tick to this CSV")
    ap.add_argument("--q5-descent-drop", type=float, default=0.12, help="q5: descent collective below the hover's")
    ap.add_argument("--q5-fly-log", type=int, default=0, help="q5: log the autopilot every N ticks (0 = off)")
    ap.add_argument("--max-bots", type=int, default=12, help="blockpad/allblocked: bots to add at most")
    ap.add_argument("--restart", choices=["none", "refused", "allowed"], default="none",
                    help="allblocked: send RestartRound after SpawnBlocked and expect this outcome")
    ap.add_argument("--min-walk", type=float, default=15.0, help="walk: the heli must be at least this far")
    ap.add_argument("--wreck-climb", type=float, default=10.0, help="wreck: metres to climb before the crash")
    ap.add_argument("--wreck-pitch", type=float, default=-0.3, help="wreck: cyclic pitch during the climb")
    ap.add_argument("--spawn-wait", type=float, default=25.0, help="blockpad/allblocked: seconds to wait for a spawn")
    ap.add_argument("--enter-range", type=float, default=0.0, help="view: the server's enterRange to compare with Welcome")
    ap.add_argument("--tv", action="store_true", help="weapons: a second bot in the gunner seat fires its missile")
    ap.add_argument("--mandown-wait", type=float, default=20.0, help="damage: seconds to wait for a man-down bot to die")
    ap.add_argument("--damage-switch", action="store_true", help="damage: put B on team 2 with an admin switch, not a teleport")
    ap.add_argument("--damage-gap", type=float, default=20.0, help="damage: metres from A's heli nose to C")
    ap.add_argument("--crash", action="store_true", help="damage: also crash a heli (Damage and Kill without attacker)")
    ap.add_argument("--max-connections", type=int, default=4, help="multiconn/fuzz: the server's maxConnections")
    ap.add_argument("--expect-teleport-refused", action="store_true", help="teleport/fuzz: the server has allowTeleport 0")
    ap.add_argument("--soak-seconds", type=float, default=600.0)
    ap.add_argument("--burst-presses", type=int, default=60, help="burst: one-tick camera presses per pattern")
    ap.add_argument("--tv-target", action="store_true", help="tv: also steer a missile onto an own bot of the other team")
    ap.add_argument("--tv-range", type=float, default=250.0, help="tv: metres from the heli to the aim points")
    ap.add_argument("--sea-level", type=float, default=141.4,
                    help="tv: the map's sea level (dalian_2_v_2 Heightdata.con setSeaWaterLevel); aim points lie on it")
    ap.add_argument("--tv-every", type=float, default=75.0,
                    help="soak3: seconds between guided TV shots (the AH-1Z carries 8 and does not rearm in a hover)")
    ap.add_argument("--trace-csv", default="", help="tv: write every TV missile's track to this CSV")
    args = ap.parse_args()
    try:
        if args.fuzz:
            return run_fuzz(args)
        if args.scenario in SCENARIOS_V3:
            return SCENARIOS_V3[args.scenario](args)
        if args.scenario in SCENARIOS_V2:
            return SCENARIOS_V2[args.scenario](args)
        if args.scenario != "hover":
            return SCENARIOS[args.scenario](args)
        return run_bot(args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
