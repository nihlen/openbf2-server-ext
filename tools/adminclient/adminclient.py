#!/usr/bin/env python3
"""Reference WebAdmin for the openbf2-server-ext admin channel, protocol version 1
(docs/findings/admin-phase1.md; design in docs/admin-channel-draft.md).

The module dials out, so this is a listener. It does TLS 1.3 with a self-signed certificate it
creates on first use (and prints the SPKI pin for serverext.cfg's adminCertSha256), the HMAC
handshake, the snapshot, subscriptions, every command and a fuzz mode. It also writes the
golden-byte fixtures the C++ and C# codecs share.

Python 3 standard library plus the openssl command-line tool (certificate creation only).

  python3 adminclient.py --key <hex> --print-pin                        # make the certificate, print the pin
  python3 adminclient.py --key <hex> --scenario listen --seconds 60 --sub players:5 --sub damage
  python3 adminclient.py --key <hex> --scenario commands
  python3 adminclient.py --key <hex> --scenario resync
  python3 adminclient.py --key <hex> --fuzz
  python3 adminclient.py --write-golden ../../tests/fixtures/admin-v1-golden.txt

  python3 adminclient.py --key <hex> --scenario script                  # scripts; with a bot also the host API
  python3 adminclient.py --key <hex> --scenario runscript --script-file x.js --script-args '{"a":1}' --seconds 20
  python3 adminclient.py --key <hex> --scenario scriptends              # a map change and a closed link end scripts
  python3 adminclient.py --key <hex> --scenario seat                    # SeatPreference: capability, Ok, Error 4 and 7
  python3 adminclient.py --key <hex> --scenario seat --seat-player 0 --seat 1 --seat-keep --seconds 600
  python3 adminclient.py --key <hex> --scenario collision --seconds 40   # SetCollision; with serverext_client.py
                                                                        # --scenario wreck: does the heli fall through?

Scenarios: listen, commands, resync, stall, soak, wrongkey, mapchange, script and more. Port 16567 is refused.
"""

import argparse
import hashlib
import hmac
import json
import math
import os
import random
import socket
import ssl
import struct
import subprocess
import sys
import time

VERSION = 1

# ---------------------------------------------------------------- message types

HELLO, CHALLENGE, AUTH, WELCOME = 0x1001, 0x1002, 0x1003, 0x1004
SNAPSHOT, REQUEST_SNAPSHOT, SUBSCRIBE, UNSUBSCRIBE = 0x1101, 0x1102, 0x1103, 0x1104
PLAYER_STREAM, VEHICLE_STREAM, PROJECTILE_STREAM, GAME_STREAM = 0x1111, 0x1112, 0x1113, 0x1114
AIM_STREAM = 0x1115
EV_GAME_STATE, EV_ROUND_START, EV_ROUND_END, EV_PLAYER_CONNECT = 0x1201, 0x1202, 0x1203, 0x1204
EV_PLAYER_DISCONNECT, EV_SPAWN, EV_DEATH, EV_REVIVE, EV_TEAM_CHANGE = 0x1205, 0x1206, 0x1207, 0x1208, 0x1209
EV_SCORE_CHANGE, EV_KILL, EV_DAMAGE, EV_ENTER_VEHICLE, EV_EXIT_VEHICLE = 0x120A, 0x120B, 0x120C, 0x120D, 0x120E
EV_VEHICLE_DESTROYED, EV_CONTROL_POINT, EV_CHAT, EV_PROJECTILE_FIRED = 0x120F, 0x1210, 0x1211, 0x1212
EV_PROJECTILE_HIT, EV_KICK, EV_BAN, EV_BOT = 0x1213, 0x1214, 0x1215, 0x1216
EV_WEAPON_FIRE, EV_WEAPONS, EV_PROJECTILE_COLLISION, EV_PROJECTILE_TRACE = 0x1217, 0x1218, 0x1219, 0x121A
EV_VOTE_RESULT, EV_SCRIPT_FINISHED, EV_SCRIPT_EVENT = 0x121B, 0x121C, 0x121D
CMD_CONSOLE, CMD_SAY, CMD_TELEPORT, CMD_SET_HEALTH, CMD_KILL_PLAYER = 0x1301, 0x1302, 0x1303, 0x1304, 0x1305
CMD_SET_TEAM, CMD_SET_SCORE, CMD_RANK, CMD_MEDAL, CMD_GAME_EVENT = 0x1306, 0x1307, 0x1308, 0x1309, 0x130A
CMD_HUD_EVENT, CMD_KICK, CMD_BAN, CMD_MAP, CMD_LIST_OBJECTS = 0x130B, 0x130C, 0x130D, 0x130E, 0x130F
CMD_SET_OBJECT_ACTIVE, CMD_SPAWN_OBJECT, CMD_DELETE_OBJECT = 0x1310, 0x1311, 0x1312
CMD_START_VOTE, CMD_CANCEL_VOTE = 0x1313, 0x1314
CMD_HUD_EVENT_RAW, CMD_ROUND_CLOCK, CMD_SQUAD_ORDER = 0x1315, 0x1316, 0x1317
CMD_START_SCRIPT, CMD_STOP_SCRIPT, CMD_SCRIPT_MESSAGE, CMD_LIST_SCRIPTS = 0x1318, 0x1319, 0x131A, 0x131B
CMD_SEAT_PREFERENCE, CMD_SET_COLLISION = 0x131C, 0x131D
OK, CONSOLE_RESULT, OBJECT_LIST, OBJECT_SPAWNED, SCRIPT_LIST = 0x1401, 0x1402, 0x1403, 0x1404, 0x1405
PING, PONG, ERROR = 0x1501, 0x1502, 0xFFFF

NAMES = {v: k for k, v in list(globals().items()) if isinstance(v, int) and k.isupper() and 0x1000 <= v <= 0xFFFF
         and k not in ("VERSION",)}
EVENT_TYPES = range(0x1201, 0x121E)
CAP_CONSOLE, CAP_OBJECTS, CAP_IDENTITY, CAP_SCRIPTS, CAP_SEAT_PREFERENCE, CAP_SET_COLLISION = 1, 2, 4, 8, 16, 32
SEAT_FIRST, SEAT_SECOND, SEAT_NONE = 0, 1, 255
VOTE_OUTCOMES = {1: "passed", 2: "failed", 3: "cancelled", 4: "level unload"}

STREAMS = {"players": 1, "vehicles": 2, "projectiles": 3, "game": 4, "aim": 5, "damage": 16, "projectile-events": 17,
           "fire": 18, "weapons": 19, "replay": 20}
COLLISION_TERRAIN, COLLISION_OBJECT = 0, 1
COLLISION_ROOT, COLLISION_SOLDIER, COLLISION_REWOUND = 1, 2, 4
TRACE_CUT, TRACE_COLLIDED = 1, 2
AIM_CAMERA, AIM_WEAPON = 1, 2
REF_PLAYER, REF_NET, REF_CONSOLE = 1, 2, 3
ERR_VERSION, ERR_MALFORMED, ERR_UNKNOWN_TYPE, ERR_UNKNOWN_PLAYER = 1, 2, 3, 4
ERR_UNAVAILABLE, ERR_REFUSED, ERR_INVALID, ERR_AUTH, ERR_DISABLED = 5, 6, 7, 8, 9
INFO_AI, INFO_BOT_CHANNEL, INFO_CONNECTED, INFO_IDENTITY = 1, 2, 4, 8
KILL_KINDS = {1: "kill", 2: "teamkill", 3: "suicide", 4: "roadkill", 5: "no-attacker"}
DISC = {0: "unknown", 1: "left", 2: "kicked", 3: "timed out", 4: "banned", 5: "other"}

WEBADMIN_LABEL = b"OBF2-ADMIN-1 webadmin"
SERVER_LABEL = b"OBF2-ADMIN-1 server"
MAX_OUTBOUND = 1024 * 1024
MAX_INBOUND = 64 * 1024


def ts():
    return time.strftime("%H:%M:%S")


def log(*a):
    print(ts(), *a, flush=True)


# ---------------------------------------------------------------- codec

class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack("<B", v); return self
    def i16(self, v): self.b += struct.pack("<h", v); return self
    def u16(self, v): self.b += struct.pack("<H", v); return self
    def i32(self, v): self.b += struct.pack("<i", v); return self
    def u32(self, v): self.b += struct.pack("<I", v); return self
    def u64(self, v): self.b += struct.pack("<Q", v); return self
    def f32(self, v): self.b += struct.pack("<f", v); return self
    def f64(self, v): self.b += struct.pack("<d", v); return self
    def raw(self, v): self.b += bytes(v); return self
    def vec3(self, v): self.b += struct.pack("<3f", *v); return self

    def s(self, v):
        data = v.encode("utf-8") if isinstance(v, str) else bytes(v)
        self.b += struct.pack("<H", len(data)) + data
        return self


class R:
    def __init__(self, data):
        self.d = bytes(data)
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("short payload")
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u8(self): return self.take(1)[0]
    def i16(self): return struct.unpack("<h", self.take(2))[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def i32(self): return struct.unpack("<i", self.take(4))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def f32(self): return struct.unpack("<f", self.take(4))[0]
    def f64(self): return struct.unpack("<d", self.take(8))[0]
    def vec3(self): return struct.unpack("<3f", self.take(12))
    def s(self): return self.take(self.u16()).decode("utf-8")
    def done(self): return self.p == len(self.d)


def frame(t, payload=b"", reserved=0):
    return struct.pack("<IHH", len(payload), t, reserved) + bytes(payload)


# Structures (docs/findings/admin-phase1.md, State).

def w_game(w, g):
    w.u8(g["status"]).u8(g["begin_round"]).f32(g["time_to_next"]).i32(g["tickets"][0]).i32(g["tickets"][1])
    w.f32(g["round_time"]).f32(g["time_limit"]).u8(g["winner"]).u8(g["victory_type"])


def r_game(r):
    return {"status": r.u8(), "begin_round": r.u8(), "time_to_next": r.f32(), "tickets": (r.i32(), r.i32()),
            "round_time": r.f32(), "time_limit": r.f32(), "winner": r.u8(), "victory_type": r.u8()}


def w_pstate(w, s):
    w.u8(s["team"]).u8(s["squad"]).u8(s["life"]).u8(s["flags"]).u16(s["ping"]).u32(s["root_net"]).u32(s["root_obj"])
    w.u8(s["seat"]).vec3(s["pos"]).vec3(s["rot"]).vec3(s["vel"]).f32(s["health"]).u8(s["view_mode"]).u16(s["lag_ms"])


def r_pstate(r):
    return {"team": r.u8(), "squad": r.u8(), "life": r.u8(), "flags": r.u8(), "ping": r.u16(), "root_net": r.u32(),
            "root_obj": r.u32(), "seat": r.u8(), "pos": r.vec3(), "rot": r.vec3(), "vel": r.vec3(), "health": r.f32(),
            "view_mode": r.u8(), "lag_ms": r.u16()}


def w_score(w, s):
    w.i32(s["score"]).i32(s["team_score"]).i32(s["kills"]).i32(s["deaths"]).i32(s["team_kills"]).i16(s["rank"])


def r_score(r):
    return {"score": r.i32(), "team_score": r.i32(), "kills": r.i32(), "deaths": r.i32(), "team_kills": r.i32(),
            "rank": r.i16()}


def w_pinfo(w, p):
    w.u8(p["id"]).s(p["name"]).u8(p["flags"]).i32(p["profile"]).s(p["ip"]).s(p["hash"])
    w_score(w, p["score"])
    w_pstate(w, p["state"])


def r_pinfo(r):
    p = {"id": r.u8(), "name": r.s(), "flags": r.u8(), "profile": r.i32(), "ip": r.s(), "hash": r.s()}
    p["score"] = r_score(r)
    p["state"] = r_pstate(r)
    return p


def w_vehicle(w, v):
    w.u32(v["net"]).u32(v["obj"]).s(v["template"]).u8(v["team"]).u8(v["flags"]).vec3(v["pos"]).vec3(v["rot"])
    w.vec3(v["vel"]).f32(v["health"]).u8(v["seats"]).u32(v["occupied"])


def r_vehicle(r):
    return {"net": r.u32(), "obj": r.u32(), "template": r.s(), "team": r.u8(), "flags": r.u8(), "pos": r.vec3(),
            "rot": r.vec3(), "vel": r.vec3(), "health": r.f32(), "seats": r.u8(), "occupied": r.u32()}


def w_cp(w, c):
    w.u32(c["obj"]).u32(c["net"]).s(c["template"]).u8(c["team"]).u8(c["flag"]).f32(c["take_over"])
    w.u8(c["capturing"]).vec3(c["pos"])


def r_cp(r):
    return {"obj": r.u32(), "net": r.u32(), "template": r.s(), "team": r.u8(), "flag": r.u8(), "take_over": r.f32(),
            "capturing": r.u8(), "pos": r.vec3()}


def w_proj(w, p):
    w.u32(p["id"]).s(p["template"]).i16(p["owner"]).u8(p["team"]).u8(p["flags"]).u32(p["target"])
    w.vec3(p["pos"]).vec3(p["rot"]).vec3(p["vel"]).f32(p["ttl"])


def r_proj(r):
    return {"id": r.u32(), "template": r.s(), "owner": r.i16(), "team": r.u8(), "flags": r.u8(), "target": r.u32(),
            "pos": r.vec3(), "rot": r.vec3(), "vel": r.vec3(), "ttl": r.f32()}


def w_aim(w, a):
    w.u8(a["player"]).u8(a["flags"]).vec3(a["camera_pos"]).vec3(a["camera_rot"]).vec3(a["weapon_pos"])
    w.vec3(a["weapon_rot"])


def r_aim(r):
    return {"player": r.u8(), "flags": r.u8(), "camera_pos": r.vec3(), "camera_rot": r.vec3(), "weapon_pos": r.vec3(),
            "weapon_rot": r.vec3()}


def decode(t, payload):
    """Decodes a module -> WebAdmin message into a dict. Raises ValueError on a malformed payload."""
    r = R(payload)
    m = {"type": t, "name": NAMES.get(t, "0x%04X" % t)}
    if t == HELLO:
        m.update(version=r.u16(), server_id=r.s(), nonce=r.take(32), build=r.s())
    elif t == AUTH:
        m.update(proof=r.take(32))
    elif t == WELCOME:
        m.update(version=r.u16(), session=r.u32(), tick=r.u32(), tick_rate=r.f32(), server_name=r.s(),
                 game_port=r.u16(), gamespy_port=r.u16(), max_players=r.u8(), caps=r.u8(), map=r.s(), mode=r.s(),
                 map_size=r.u16(), map_index=r.u16())
        m["maps"] = [(r.s(), r.s(), r.u16()) for _ in range(r.u16())]
        m.update(team1=r.s(), team2=r.s(), game=r_game(r))
    elif t == SNAPSHOT:
        m.update(request=r.u32(), last_seq=r.u32(), tick=r.u32(), time=r.f64(), game=r_game(r))
        m["players"] = [r_pinfo(r) for _ in range(r.u8())]
        m["vehicles"] = [r_vehicle(r) for _ in range(r.u16())]
        m["cps"] = [r_cp(r) for _ in range(r.u16())]
        m["projectiles"] = [r_proj(r) for _ in range(r.u16())]
    elif t == PLAYER_STREAM:
        m.update(tick=r.u32(), time=r.f64())
        m["players"] = [(r.u8(), r_pstate(r)) for _ in range(r.u8())]
    elif t == VEHICLE_STREAM:
        m.update(tick=r.u32(), time=r.f64())
        m["vehicles"] = [r_vehicle(r) for _ in range(r.u16())]
    elif t == PROJECTILE_STREAM:
        m.update(tick=r.u32(), time=r.f64())
        m["projectiles"] = [r_proj(r) for _ in range(r.u16())]
    elif t == GAME_STREAM:
        m.update(tick=r.u32(), time=r.f64(), game=r_game(r))
    elif t == AIM_STREAM:
        m.update(tick=r.u32(), time=r.f64())
        m["aims"] = [r_aim(r) for _ in range(r.u8())]
    elif t in EVENT_TYPES:
        m.update(seq=r.u32(), tick=r.u32())
        if t == EV_GAME_STATE:
            m.update(game=r_game(r), previous=r.u8())
        elif t == EV_ROUND_START:
            m.update(map=r.s(), mode=r.s(), tickets=(r.i32(), r.i32()))
        elif t == EV_ROUND_END:
            m.update(tickets=(r.i32(), r.i32()), winner=r.u8(), victory_type=r.u8())
        elif t == EV_PLAYER_CONNECT:
            m.update(player=r_pinfo(r))
        elif t == EV_PLAYER_DISCONNECT:
            m.update(player=r.u8(), reason=r.u8(), engine_reason=r.u16())
        elif t == EV_SPAWN:
            m.update(player=r.u8(), team=r.u8(), root_net=r.u32(), root_obj=r.u32(), template=r.s(), pos=r.vec3(),
                     rot=r.vec3())
        elif t == EV_DEATH:
            m.update(player=r.u8(), team=r.u8(), net=r.u32(), template=r.s(), pos=r.vec3())
        elif t == EV_REVIVE:
            m.update(player=r.u8(), medic=r.i16())
        elif t == EV_TEAM_CHANGE:
            m.update(player=r.u8(), old_team=r.u8(), new_team=r.u8(), flags=r.u8())
        elif t == EV_SCORE_CHANGE:
            m.update(player=r.u8(), score=r_score(r))
        elif t == EV_KILL:
            m.update(victim=r.u8(), attacker=r.i16(), kind=r.u8(), weapon=r.s(), victim_team=r.u8(),
                     victim_net=r.u32(), victim_template=r.s(), victim_pos=r.vec3(), attacker_team=r.u8(),
                     attacker_vehicle_net=r.u32(), attacker_vehicle_template=r.s(), attacker_pos=r.vec3())
            m["assists"] = [(r.u8(), r.i32()) for _ in range(r.u8())]
        elif t == EV_DAMAGE:
            m.update(victim_player=r.i16(), victim_net=r.u32(), victim_obj=r.u32(), victim_template=r.s(),
                     attacker=r.i16(), weapon=r.s(), projectile=r.s(), amount=r.f32(), before=r.f32(), after=r.f32(),
                     max=r.f32(), pos=r.vec3(), flags=r.u8())
        elif t in (EV_ENTER_VEHICLE, EV_EXIT_VEHICLE):
            m.update(player=r.u8(), seat=r.u8(), flags=r.u8(), root_net=r.u32(), root_obj=r.u32(), template=r.s(),
                     seat_template=r.s())
        elif t == EV_VEHICLE_DESTROYED:
            m.update(net=r.u32(), obj=r.u32(), template=r.s(), attacker=r.i16(), pos=r.vec3())
        elif t == EV_CONTROL_POINT:
            m.update(kind=r.u8(), cp=r_cp(r))
        elif t == EV_CHAT:
            m.update(sender=r.i16(), team=r.u8(), channel=r.u8(), channel_name=r.s(), engine_flags=r.u32(),
                     text_flags=r.u8(), text=r.s())
        elif t == EV_PROJECTILE_FIRED:
            m.update(projectile=r_proj(r))
        elif t == EV_PROJECTILE_HIT:
            m.update(id=r.u32(), template=r.s(), owner=r.i16(), flags=r.u8(), victim_net=r.u32(), pos=r.vec3())
        elif t == EV_KICK:
            m.update(player=r.u8(), name=r.s())
        elif t == EV_BAN:
            m.update(player=r.u8(), name=r.s(), ban_type=r.u8(), expiry=r.u32())
        elif t == EV_BOT:
            m.update(player=r.u8(), kind=r.u8(), team=r.u8(), connection=r.u32())
        elif t == EV_WEAPON_FIRE:
            m.update(shooter=r.i16(), root_net=r.u32(), weapon=r.s(), projectile=r.s(), rounds=r.u8(),
                     origin=r.vec3(), velocity=r.vec3())
        elif t == EV_WEAPONS:
            m.update(player=r.u8())
            m["slots"] = [(r.u8(), r.s()) for _ in range(r.u8())]
        elif t == EV_PROJECTILE_COLLISION:
            m.update(id=r.u32(), template=r.s(), shooter=r.i16(), kind=r.u8(), flags=r.u8(), root_net=r.u32(),
                     root_obj=r.u32(), root_template=r.s(), part_template=r.s(), part_player=r.i16(), pos=r.vec3(),
                     normal=r.vec3(), speed=r.vec3(), material=r.u16(), latency=r.f32(), part_pos=r.vec3(),
                     part_rot=r.vec3(), root_pos=r.vec3(), root_rot=r.vec3())
        elif t == EV_PROJECTILE_TRACE:
            m.update(id=r.u32(), template=r.s(), shooter=r.i16(), first_tick=r.u32(), flags=r.u8())
            m["targets"] = [(r.u32(), r.s()) for _ in range(r.u8())]
            samples = []
            for _ in range(r.u16()):
                smp = {"pos": r.vec3(), "vel": r.vec3(), "lag": r.f32()}
                smp["targets"] = [{"pos": r.vec3(), "rot": r.vec3(), "tested_pos": r.vec3(), "tested_rot": r.vec3()}
                                  for _ in m["targets"]]
                samples.append(smp)
            m["samples"] = samples
        elif t == EV_VOTE_RESULT:
            m.update(request=r.u32(), text=r.s(), outcome=r.u8(), yes=r.u16(), no=r.u16(), eligible=r.u16())
        elif t == EV_SCRIPT_FINISHED:
            m.update(instance=r.u32(), script=r.s(), reason=r.u8(), result=r.s(), calls=r.u32(), total_us=r.u32(),
                     max_us=r.u32())
        elif t == EV_SCRIPT_EVENT:
            m.update(instance=r.u32(), kind=r.u8(), payload=r.s())
    elif t == OK:
        m.update(request=r.u32())
    elif t == CONSOLE_RESULT:
        m.update(request=r.u32(), truncated=r.u8())
        m["lines"] = [r.s() for _ in range(r.u16())]
    elif t == OBJECT_LIST:
        m.update(request=r.u32(), more=r.u8())
        m["objects"] = [{"obj": r.u32(), "net": r.u32(), "template": r.s(), "pos": r.vec3(), "rot": r.vec3(),
                         "health": r.f32(), "flags": r.u8()} for _ in range(r.u16())]
    elif t == OBJECT_SPAWNED:
        m.update(request=r.u32(), obj=r.u32(), net=r.u32())
    elif t == SCRIPT_LIST:
        m.update(request=r.u32())
        m["scripts"] = [(r.u32(), r.s()) for _ in range(r.u8())]
    elif t in (PING, PONG):
        m.update(request=r.u32(), clock=r.u64())
    elif t == ERROR:
        m.update(request=r.u32(), code=r.u16(), message=r.s())
    else:
        raise ValueError("unknown module message 0x%04X" % t)
    if not r.done():
        raise ValueError("%d trailing bytes in %s" % (len(r.d) - r.p, m["name"]))
    return m


# Commands (WebAdmin -> module). Each returns a payload starting with the request id.

def p_console(req, line): return W().u32(req).s(line).b
def p_say(req, target, ident, text): return W().u32(req).u8(target).u8(ident).s(text).b
def p_ref(w, kind, ident): return w.u8(kind).u32(ident)


def p_teleport(req, kind, ident, pos=None, rot=None):
    w = W().u32(req)
    p_ref(w, kind, ident)
    w.u8((1 if pos is not None else 0) | (2 if rot is not None else 0))
    return w.vec3(pos or (0, 0, 0)).vec3(rot or (0, 0, 0)).b


def p_set_health(req, kind, ident, value, fraction=False):
    w = W().u32(req)
    p_ref(w, kind, ident)
    return w.u8(1 if fraction else 0).f32(value).b


def p_kill(req, player, destroy_vehicle=False): return W().u32(req).u8(player).u8(1 if destroy_vehicle else 0).b
def p_set_team(req, player, team, kill=False): return W().u32(req).u8(player).u8(team).u8(1 if kill else 0).b


def p_set_score(req, player, mask, values):
    w = W().u32(req).u8(player).u8(mask)
    for v in values:
        w.i32(v)
    return w.b


def p_rank(req, player, rank, event=True): return W().u32(req).u8(player).u8(rank).u8(1 if event else 0).b
def p_medal(req, player, medal, value): return W().u32(req).u8(player).i32(medal).i32(value).b
def p_event(req, player, event, data): return W().u32(req).u8(player).u8(event).i32(data).b
def p_kick(req, player, reason=""): return W().u32(req).u8(player).s(reason).b


def p_ban(req, player, by_key=False, duration=0, seconds=0, reason=""):
    return W().u32(req).u8(player).u8(1 if by_key else 0).u8(duration).u32(seconds).s(reason).b


def p_map(req, action, index=0, name="", mode="", size=0):
    return W().u32(req).u8(action).u16(index).s(name).s(mode).u16(size).b


def p_list_objects(req, template, max_results=4096): return W().u32(req).s(template).u16(max_results).b


def p_object_ref(req, kind, ident):
    return p_ref(W().u32(req), kind, ident).b


def p_spawn(req, template, pos, rot, team=0): return W().u32(req).s(template).vec3(pos).vec3(rot).u8(team).b


def p_start_vote(req, text, seconds=30, team=0, percent=50, min_votes=1):
    return W().u32(req).s(text).u16(seconds).u8(team).u8(percent).u8(min_votes).b


def p_hud_event_raw(req, player, event, payload=b""):
    """R7: player -1 is every playing client; payload at most 52 bytes."""
    w = W().u32(req).i16(player).u8(event).u8(len(payload))
    for x in payload:
        w.u8(x)
    return w.b


def p_round_clock(req, player, seconds): return W().u32(req).i16(player).f32(seconds).b


def p_seat_preference(req, player, seat): return W().u32(req).u8(player).u8(seat).b
def p_set_collision(req, kind, ident, on): return p_ref(W().u32(req), kind, ident).u8(1 if on else 0).b


def p_start_script(req, instance, name, source, args="", api=1, flags=0):
    """args is JSON text, or empty for none; source is JavaScript."""
    return W().u32(req).u32(instance).u8(api).u8(flags).s(name).s(args).s(source).b


def p_script_message(req, instance, message): return W().u32(req).u32(instance).s(message).b


def p_squad_order(req, team, squad, order, commander=False, target_net=0, points=((0, 0, 0),) * 3):
    w = W().u32(req).u8(team).u8(squad).u8(order).u8(1 if commander else 0).u32(target_net)
    for p in points:
        w.vec3(p)
    return w.b


def spotted_payload(spotter, target, team, x, z, vehicle_type):
    """HUD 43's payload (SpottedEvent's rebroadcast layout): u16 spotter, u16 target, u8 team at +4,
    f32 x at +8, f32 z at +12, i32 vehicle type at +16."""
    return struct.pack("<HHB3xffi", spotter, target, team, x, z, vehicle_type)


def p_subscribe(req, stream, rate=1, filters=()):
    w = W().u32(req).u8(stream).u8(rate).u8(len(filters))
    for f in filters:
        w.s(f)
    return w.b


def p_ping(req, clock): return W().u32(req).u64(clock).b


# ---------------------------------------------------------------- golden fixtures

def golden_samples():
    """Canonical messages, mirrored by tests/AdminTests.cpp (testGolden). name -> (type, payload)."""
    s = {}
    pstate = {"team": 2, "squad": 3, "life": 2, "flags": 5, "ping": 48, "root_net": 1819, "root_obj": 29328,
              "seat": 1, "pos": (1.5, 100.25, -3.0), "rot": (90.0, -5.0, 2.5), "vel": (10.0, 0.5, -1.0),
              "health": 0.75, "view_mode": 3, "lag_ms": 245}
    score = {"score": 12, "team_score": 4, "kills": 5, "deaths": 2, "team_kills": 1, "rank": 7}
    pinfo = {"id": 3, "name": "§netsky", "flags": INFO_CONNECTED | INFO_IDENTITY, "profile": 43001234,
             "ip": "10.0.0.7", "hash": "0123456789abcdef0123456789abcdef", "score": score, "state": pstate}
    vehicle = {"net": 1820, "obj": 31000, "template": "ahe_ah1z", "team": 1, "flags": 0, "pos": (4.0, 50.0, 6.0),
               "rot": (45.0, 1.0, -1.0), "vel": (0.0, -0.5, 3.0), "health": 0.9, "seats": 2, "occupied": 1}
    cp = {"obj": 400, "net": 0, "template": "cpname_dalian_harbour", "team": 2, "flag": 2, "take_over": 0.5,
          "capturing": 1, "pos": (1.0, 2.0, 3.0)}
    proj = {"id": 0x10000 + 77, "template": "agm114_hellfire_tv", "owner": 254, "team": 1, "flags": 1,
            "target": 1820, "pos": (5.0, 6.0, 7.0), "rot": (10.0, 20.0, 30.0), "vel": (100.0, 0.0, -1.0), "ttl": 9.5}
    game = {"status": 1, "begin_round": 3, "time_to_next": 12.5, "tickets": (150, 0), "round_time": 61.0,
            "time_limit": 1800.0, "winner": 2, "victory_type": 3}
    nonce = bytes(range(32))

    s["hello"] = (HELLO, W().u16(1).s("127.0.0.1:16573").raw(nonce).s("libserverext admin 1").b)
    s["challenge"] = (CHALLENGE, bytes(range(0xC0, 0xE0)) + bytes(range(0x40, 0x60)))
    s["auth"] = (AUTH, bytes(range(0x10, 0x30)))
    w = W().u16(1).u32(3).u32(900).f32(30.0).s("serverext admin").u16(16573).u16(29905).u8(16).u8(3)
    w.s("dalian_2_v_2").s("gpm_cq").u16(16).u16(0).u16(2)
    w.s("dalian_2_v_2").s("gpm_cq").u16(16).s("daqing_2_v_2").s("gpm_cq").u16(16).s("CH").s("US")
    w_game(w, game)
    s["welcome"] = (WELCOME, w.b)
    w = W().u32(17).u32(42).u32(1000).f64(33.5)
    w_game(w, game)
    w.u8(1)
    w_pinfo(w, pinfo)
    w.u16(1)
    w_vehicle(w, vehicle)
    w.u16(1)
    w_cp(w, cp)
    w.u16(1)
    w_proj(w, proj)
    s["snapshot"] = (SNAPSHOT, w.b)
    w = W().u32(5).f64(1.0).u8(1).u8(7)
    w_pstate(w, pstate)
    s["player_stream"] = (PLAYER_STREAM, w.b)
    w = W().u32(5).f64(1.0).u16(1)
    w_vehicle(w, vehicle)
    s["vehicle_stream"] = (VEHICLE_STREAM, w.b)
    w = W().u32(5).f64(1.0).u16(1)
    w_proj(w, proj)
    s["projectile_stream"] = (PROJECTILE_STREAM, w.b)
    w = W().u32(5).f64(1.0)
    w_game(w, game)
    s["game_stream"] = (GAME_STREAM, w.b)

    def ev(seq):
        return W().u32(seq).u32(10)
    w = ev(1)
    w_game(w, game)
    s["ev_game_state"] = (EV_GAME_STATE, w.u8(3).b)
    s["ev_round_start"] = (EV_ROUND_START, ev(2).s("dalian_2_v_2").s("gpm_cq").i32(100).i32(90).b)
    s["ev_round_end"] = (EV_ROUND_END, ev(3).i32(100).i32(90).u8(1).u8(2).b)
    w = ev(4)
    w_pinfo(w, pinfo)
    s["ev_player_connect"] = (EV_PLAYER_CONNECT, w.b)
    s["ev_player_disconnect"] = (EV_PLAYER_DISCONNECT, ev(5).u8(3).u8(2).u16(21).b)
    s["ev_spawn"] = (EV_SPAWN, ev(6).u8(3).u8(2).u32(1800).u32(5000).s("us_heavy_soldier").vec3((1, 2, 3))
                     .vec3((4, 5, 6)).b)
    s["ev_death"] = (EV_DEATH, ev(7).u8(3).u8(2).u32(1800).s("us_heavy_soldier").vec3((1, 2, 3)).b)
    s["ev_revive"] = (EV_REVIVE, ev(8).u8(3).i16(-1).b)
    s["ev_team_change"] = (EV_TEAM_CHANGE, ev(9).u8(255).u8(2).u8(1).u8(1).b)
    w = ev(10).u8(3)
    w_score(w, score)
    s["ev_score_change"] = (EV_SCORE_CHANGE, w.b)
    s["ev_kill"] = (EV_KILL, ev(11).u8(3).i16(255).u8(1).s("ahe_z10_S8Launcher").u8(2).u32(1800)
                    .s("us_heavy_soldier").vec3((1, 2, 3)).u8(1).u32(1819).s("ahe_z10").vec3((4, 5, 6)).u8(1)
                    .u8(254).i32(2).b)
    s["ev_damage"] = (EV_DAMAGE, ev(12).i16(3).u32(1800).u32(5000).s("us_heavy_soldier").i16(255).s("w").s("p")
                      .f32(10).f32(100).f32(90).f32(100).vec3((1, 2, 3)).u8(0).b)
    s["ev_enter_vehicle"] = (EV_ENTER_VEHICLE, ev(13).u8(255).u8(0).u8(0).u32(1819).u32(29328).s("ahe_z10")
                             .s("ahe_z10").b)
    s["ev_exit_vehicle"] = (EV_EXIT_VEHICLE, ev(14).u8(255).u8(0).u8(0).u32(1819).u32(29328).s("ahe_z10")
                            .s("ahe_z10").b)
    s["ev_vehicle_destroyed"] = (EV_VEHICLE_DESTROYED, ev(15).u32(1819).u32(29328).s("ahe_z10").i16(-1)
                                 .vec3((1, 2, 3)).b)
    w = ev(16).u8(1)
    w_cp(w, cp)
    s["ev_control_point"] = (EV_CONTROL_POINT, w.b)
    s["ev_chat"] = (EV_CHAT, ev(17).i16(-1).u8(0).u8(12).s("ServerMessage").u32(0).u8(0).s("rdy").b)
    w = ev(18)
    w_proj(w, proj)
    s["ev_projectile_fired"] = (EV_PROJECTILE_FIRED, w.b)
    s["ev_projectile_hit"] = (EV_PROJECTILE_HIT, ev(19).u32(0x10000 + 77).s("agm114_hellfire_tv").i16(254).u8(3)
                              .u32(1820).vec3((5, 6, 7)).b)
    s["ev_kick"] = (EV_KICK, ev(20).u8(3).s("x").b)
    s["ev_ban"] = (EV_BAN, ev(21).u8(3).s("x").u8(1).u32(0).b)
    s["ev_bot"] = (EV_BOT, ev(22).u8(255).u8(1).u8(1).u32(2).b)
    w = W().u32(5).f64(1.0).u8(1)
    w_aim(w, {"player": 7, "flags": AIM_CAMERA | AIM_WEAPON, "camera_pos": (1.5, 101.5, -3.0),
              "camera_rot": (90.0, -10.0, 0.0), "weapon_pos": (1.5, 99.0, -1.0), "weapon_rot": (85.0, -12.5, 2.5)})
    s["aim_stream"] = (AIM_STREAM, w.b)
    s["ev_weapon_fire"] = (EV_WEAPON_FIRE, ev(23).i16(255).u32(1819).s("ahe_z10_30mm").s("ahe_z10_30mm_projectile")
                           .u8(3).vec3((1.0, 2.0, 3.0)).vec3((400.0, -5.0, 10.0)).b)
    s["ev_weapons"] = (EV_WEAPONS, ev(24).u8(255).u8(2).u8(0).s("ahe_z10_30mm").u8(1).s("ahe_z10_FlareLauncher").b)
    s["ev_projectile_collision"] = (
        EV_PROJECTILE_COLLISION,
        ev(25).u32(1852).s("agm114_hellfire_tv").i16(254).u8(COLLISION_OBJECT).u8(COLLISION_REWOUND).u32(1799)
        .u32(29312).s("ahe_ah1z").s("agm114_hellfiredummy").i16(-1).vec3((210.5, 199.25, 112.0))
        .vec3((1.0, 0.0, 0.0)).vec3((-30.0, 2.5, 120.0)).u16(56).f32(0.25).vec3((211.5, 199.0, 112.25))
        .vec3((180.0, 2.0, 25.5)).vec3((206.0, 199.5, 110.0)).vec3((180.0, 2.0, 25.5)).b)
    w = ev(26).u32(1852).s("agm114_hellfire_tv").i16(254).u32(5000).u8(TRACE_COLLIDED).u8(1).u32(1799).s("ahe_ah1z")
    w.u16(2)
    for k in range(2):
        w.vec3((100.0 + k, 200.0, 50.0 + 4 * k)).vec3((0.0, -1.0, 125.0)).f32(0.25)
        w.vec3((210.0 - k, 199.0, 112.0)).vec3((180.0, 2.0, 25.5)).vec3((217.5 - k, 199.5, 111.0)).vec3((180.0, 1.5, 25.0))
    s["ev_projectile_trace"] = (EV_PROJECTILE_TRACE, w.b)

    s["ok"] = (OK, W().u32(9).b)
    s["console_result"] = (CONSOLE_RESULT, W().u32(10).u8(1).u16(3).s("a").s("").s("§").b)
    s["object_list"] = (OBJECT_LIST, W().u32(11).u8(1).u16(1).u32(5).u32(0).s("fence").vec3((1, 2, 3))
                        .vec3((4, 5, 6)).f32(-1.0).u8(0).b)
    s["object_spawned"] = (OBJECT_SPAWNED, W().u32(12).u32(300).u32(0).b)
    s["ping"] = (PING, W().u32(13).u64(0x0102030405060708).b)
    s["pong"] = (PONG, W().u32(13).u64(0x0102030405060708).b)
    s["error"] = (ERROR, W().u32(14).u16(9).s("off").b)

    # WebAdmin -> module commands, one each.
    s["cmd_request_snapshot"] = (REQUEST_SNAPSHOT, W().u32(1).b)
    s["cmd_subscribe"] = (SUBSCRIBE, p_subscribe(3, 3, 5, ["agm114_hellfire_tv"]))
    s["cmd_unsubscribe"] = (UNSUBSCRIBE, W().u32(5).u8(2).b)
    s["cmd_console"] = (CMD_CONSOLE, p_console(6, "sv.serverName"))
    s["cmd_say"] = (CMD_SAY, p_say(7, 2, 3, "§C1001hi"))
    s["cmd_teleport"] = (CMD_TELEPORT, p_teleport(8, REF_PLAYER, 3, (1, 1, 1), (1, 1, 1)))
    s["cmd_set_health"] = (CMD_SET_HEALTH, p_set_health(9, REF_NET, 1819, 0.5, True))
    s["cmd_kill_player"] = (CMD_KILL_PLAYER, p_kill(10, 3, True))
    s["cmd_set_team"] = (CMD_SET_TEAM, p_set_team(11, 3, 2, True))
    s["cmd_set_score"] = (CMD_SET_SCORE, p_set_score(12, 3, 0x1F, [0, -1, -2, -3, -4]))
    s["cmd_rank"] = (CMD_RANK, p_rank(13, 3, 21, True))
    s["cmd_medal"] = (CMD_MEDAL, p_medal(14, 3, 2051907, 1))
    s["cmd_game_event"] = (CMD_GAME_EVENT, p_event(15, 3, 0, 0))
    s["cmd_hud_event"] = (CMD_HUD_EVENT, p_event(16, 3, 78, -1))
    s["cmd_kick"] = (CMD_KICK, p_kick(17, 3, ""))
    s["cmd_ban"] = (CMD_BAN, p_ban(18, 3, True, 2, 3600, "tk"))
    s["cmd_map"] = (CMD_MAP, p_map(19, 4, 0, "dalian_2_v_2", "gpm_cq", 16))
    s["cmd_list_objects"] = (CMD_LIST_OBJECTS, p_list_objects(20, "fence_wood", 100))
    s["cmd_set_object_active"] = (CMD_SET_OBJECT_ACTIVE, p_object_ref(21, REF_CONSOLE, 29328))
    s["cmd_spawn_object"] = (CMD_SPAWN_OBJECT, p_spawn(22, "ahe_ah1z", (2, 2, 2), (2, 2, 2), 1))
    s["cmd_delete_object"] = (CMD_DELETE_OBJECT, p_object_ref(23, REF_NET, 5))
    s["cmd_ping"] = (PING, p_ping(24, 99))
    s["cmd_subscribe_fire"] = (SUBSCRIBE, p_subscribe(25, 18, 0, ["ahe_z10_FlareLauncher"]))
    s["cmd_subscribe_replay"] = (SUBSCRIBE, p_subscribe(26, 20, 0, ["agm114_hellfire_tv"]))
    s["cmd_start_vote"] = (CMD_START_VOTE, p_start_vote(27, "Enable mod X?", 30, 2, 60, 3))
    s["cmd_cancel_vote"] = (CMD_CANCEL_VOTE, W().u32(28).b)
    s["ev_vote_result"] = (EV_VOTE_RESULT, ev(27).u32(27).s("Enable mod §X?").u8(1).u16(3).u16(1).u16(5).b)
    s["cmd_hud_event_raw"] = (CMD_HUD_EVENT_RAW, p_hud_event_raw(29, -1, 43, spotted_payload(0, 0, 2, 100.0, -50.0, 1)))
    s["cmd_round_clock"] = (CMD_ROUND_CLOCK, p_round_clock(30, 3, 90.5))
    s["cmd_squad_order"] = (CMD_SQUAD_ORDER, p_squad_order(31, 2, 1, 8, True, 1819,
                                                           ((10.0, 0, 0), (0, 20.0, 0), (0, 0, 30.0))))
    s["cmd_start_script"] = (CMD_START_SCRIPT, p_start_script(30, 7, "nasa", "script.onTick(() => {});",
                                                              '{"altitude":300}'))
    s["cmd_stop_script"] = (CMD_STOP_SCRIPT, W().u32(31).u32(7).b)
    s["cmd_script_message"] = (CMD_SCRIPT_MESSAGE, p_script_message(32, 7, '{"altitude":250}'))
    s["cmd_list_scripts"] = (CMD_LIST_SCRIPTS, W().u32(33).b)
    s["script_list"] = (SCRIPT_LIST, W().u32(33).u8(2).u32(7).s("nasa").u32(9).s("dance").b)
    s["ev_script_finished"] = (EV_SCRIPT_FINISHED, ev(28).u32(7).s("nasa").u8(1).s('{"ticks":90}').u32(90).u32(1234)
                               .u32(56).b)
    s["ev_script_event"] = (EV_SCRIPT_EVENT, ev(29).u32(7).u8(1).s('{"player":3,"y":301.5}').b)
    s["cmd_seat_preference"] = (CMD_SEAT_PREFERENCE, p_seat_preference(34, 3, SEAT_SECOND))
    s["cmd_seat_preference_none"] = (CMD_SEAT_PREFERENCE, p_seat_preference(35, 3, SEAT_NONE))
    s["cmd_set_collision_off"] = (CMD_SET_COLLISION, p_set_collision(36, REF_PLAYER, 3, False))
    s["cmd_set_collision_on"] = (CMD_SET_COLLISION, p_set_collision(37, REF_NET, 1819, True))
    return s


def write_golden(path):
    s = golden_samples()
    with open(path, "w", newline="\n") as f:
        f.write("# Admin channel v1 golden frames (tools/adminclient/adminclient.py golden_samples).\n")
        f.write("# name type-hex frame-hex. Read by tests/AdminTests.cpp and the C# codec tests.\n")
        for name, (t, payload) in s.items():
            data = frame(t, payload)
            # Every module message round-trips through the decoder.
            if not name.startswith("cmd_") and name != "challenge":
                decode(t, payload)
            f.write("%s %04x %s\n" % (name, t, data.hex()))
    log("wrote %d golden frames to %s" % (len(s), path))
    return 0


# ---------------------------------------------------------------- TLS certificate

def make_certificate(cert_dir):
    os.makedirs(cert_dir, exist_ok=True)
    cert, key = os.path.join(cert_dir, "webadmin.crt"), os.path.join(cert_dir, "webadmin.key")
    if not (os.path.exists(cert) and os.path.exists(key)):
        subprocess.check_call(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                               "-nodes", "-keyout", key, "-out", cert, "-days", "36500", "-subj",
                               "/CN=openbf2 webadmin test"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def spki_pin(cert_path):
    pub = subprocess.check_output(["openssl", "x509", "-in", cert_path, "-pubkey", "-noout"])
    der = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=pub, stdout=subprocess.PIPE,
                         check=True).stdout
    return hashlib.sha256(der).hexdigest()


# ---------------------------------------------------------------- the link

class Closed(Exception):
    pass


class Link:
    """One accepted module connection after the handshake."""

    def __init__(self, sock, args, key):
        self.sock = sock
        self.args = args
        self.key = key
        self.buf = b""
        self.req = 1000
        self.expected_seq = None
        self.welcome = None
        self.snapshot = None
        self.events = []
        self.gaps = 0
        self.resyncs = 0
        self.bytes_by_type = {}
        self.frames_by_type = {}
        self.pending_snapshot = None
        self.drop_next_event = 0
        self.last_player_stream = None
        self.last_vehicle_stream = None
        self.last_game_stream = None
        self.stream_frames = {}
        self.mark = None
        self.on_frame = None   # called with every decoded message

    # -- framing
    def send(self, t, payload=b""):
        self.sock.sendall(frame(t, payload))

    def recv_frame(self, timeout):
        self.sock.settimeout(timeout)
        while True:
            if len(self.buf) >= 8:
                n, t, res = struct.unpack("<IHH", self.buf[:8])
                if n > MAX_OUTBOUND or res:
                    raise ValueError("bad frame header from the module: length %d reserved %d" % (n, res))
                if len(self.buf) >= 8 + n:
                    payload = self.buf[8:8 + n]
                    self.buf = self.buf[8 + n:]
                    self.bytes_by_type[t] = self.bytes_by_type.get(t, 0) + 8 + n
                    self.frames_by_type[t] = self.frames_by_type.get(t, 0) + 1
                    return t, payload
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except (ConnectionError, ssl.SSLError, OSError) as e:
                raise Closed(str(e))
            if not chunk:
                raise Closed("the module closed the connection")
            self.buf += chunk

    def next_request(self):
        self.req += 1
        return self.req

    # -- dispatch of everything the module sends
    def handle(self, t, payload):
        m = decode(t, payload)
        if t == PING:
            self.send(PONG, W().u32(m["request"]).u64(m["clock"]).b)
        elif t in EVENT_TYPES:
            if self.drop_next_event:
                self.drop_next_event -= 1
                log("  (simulating a lost event: dropping seq %d %s)" % (m["seq"], m["name"]))
                return m
            self.check_seq(m)
            self.events.append(m)
            if self.args.verbose or m["type"] not in (EV_SCORE_CHANGE, EV_DAMAGE, EV_PROJECTILE_COLLISION, EV_PROJECTILE_TRACE):
                log("  event", describe(m))
        elif t == SNAPSHOT:
            self.snapshot = m
            self.expected_seq = m["last_seq"] + 1
            if m["request"]:
                self.resyncs += 1
            log("  Snapshot req %d lastSeq %d: %d players, %d vehicles, %d control points, %d projectiles, status %d"
                % (m["request"], m["last_seq"], len(m["players"]), len(m["vehicles"]), len(m["cps"]),
                   len(m["projectiles"]), m["game"]["status"]))
        elif t == PLAYER_STREAM:
            self.last_player_stream = m
        elif t == VEHICLE_STREAM:
            self.last_vehicle_stream = m
        elif t == GAME_STREAM:
            self.last_game_stream = m
        if t in (PLAYER_STREAM, VEHICLE_STREAM, PROJECTILE_STREAM, GAME_STREAM, AIM_STREAM):
            self.stream_frames[t] = self.stream_frames.get(t, 0) + 1
        if self.on_frame:
            self.on_frame(m)
        return m

    def check_seq(self, m):
        if self.expected_seq is None:
            return
        if m["seq"] < self.expected_seq:
            return   # already in the snapshot
        if m["seq"] > self.expected_seq:
            self.gaps += 1
            log("  GAP: expected seq %d, got %d; RequestSnapshot" % (self.expected_seq, m["seq"]))
            self.pending_snapshot = self.next_request()
            self.send(REQUEST_SNAPSHOT, W().u32(self.pending_snapshot).b)
        self.expected_seq = m["seq"] + 1

    def pump(self, seconds):
        """Reads and handles everything for the given time."""
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            f = self.recv_frame(min(left, 0.5))
            if f:
                self.handle(*f)

    def request(self, t, payload, timeout=5.0):
        """Sends a command (payload starts with its request id) and returns the reply for it."""
        req = struct.unpack("<I", payload[:4])[0]
        self.mark = len(self.events)   # events the command raises synchronously precede its reply
        self.send(t, payload)
        end = time.time() + timeout
        while time.time() < end:
            f = self.recv_frame(0.5)
            if not f:
                continue
            m = self.handle(*f)
            if m["type"] in (OK, CONSOLE_RESULT, OBJECT_LIST, OBJECT_SPAWNED, SCRIPT_LIST, ERROR, PONG, SNAPSHOT) and \
                    m.get("request") == req:
                return m
        raise TimeoutError("no reply to request %d (%s)" % (req, NAMES.get(t, t)))

    def wait_event(self, pred, timeout):
        """Waits for an event since the last request was sent (or since now when none was)."""
        start = self.mark if self.mark is not None else len(self.events)
        self.mark = None
        end = time.time() + timeout
        while time.time() < end:
            for m in self.events[start:]:
                if pred(m):
                    return m
            start = len(self.events)
            f = self.recv_frame(0.3)
            if f:
                self.handle(*f)
        for m in self.events[start:]:
            if pred(m):
                return m
        return None


def describe(m):
    n = m["name"]
    if n == "EV_KILL":
        return "%s seq %d: %s victim %d (team %d, %s at %.0f %.0f %.0f) by %d with '%s' (vehicle %s %d)%s" % (
            n, m["seq"], KILL_KINDS.get(m["kind"], m["kind"]), m["victim"], m["victim_team"], m["victim_template"],
            m["victim_pos"][0], m["victim_pos"][1], m["victim_pos"][2], m["attacker"], m["weapon"],
            m["attacker_vehicle_template"], m["attacker_vehicle_net"],
            (" assists %s" % m["assists"]) if m["assists"] else "")
    if n == "EV_DAMAGE":
        return "%s seq %d: victim player %d '%s' (net %d) by %d weapon '%s' projectile '%s': %.1f (%.1f -> %.1f of %.1f) flags %d" % (
            n, m["seq"], m["victim_player"], m["victim_template"], m["victim_net"], m["attacker"], m["weapon"],
            m["projectile"], m["amount"], m["before"], m["after"], m["max"], m["flags"])
    if n == "EV_CHAT":
        return "%s seq %d: from %d team %d channel %d '%s' flags %d: %r" % (
            n, m["seq"], m["sender"], m["team"], m["channel"], m["channel_name"], m["engine_flags"], m["text"])
    if n == "EV_PLAYER_CONNECT":
        p = m["player"]
        return "%s seq %d: %d '%s' flags %d team %d ip '%s' hash '%s'" % (
            n, m["seq"], p["id"], p["name"], p["flags"], p["state"]["team"], p["ip"], p["hash"])
    if n == "EV_PLAYER_DISCONNECT":
        return "%s seq %d: %d reason %s (engine %d)" % (n, m["seq"], m["player"], DISC.get(m["reason"], m["reason"]),
                                                       m["engine_reason"])
    keep = {k: v for k, v in m.items() if k not in ("type", "name")}
    for k, v in keep.items():
        if isinstance(v, tuple) and v and isinstance(v[0], float):
            keep[k] = tuple(round(x, 1) for x in v)
    return "%s %s" % (n, keep)


# ---------------------------------------------------------------- accepting and the handshake

class Listener:
    def __init__(self, args):
        self.args = args
        if args.port in (16567, 16569, 29901, 4721, 4300):
            sys.exit("refusing port %d (reserved for other environments)" % args.port)
        self.key = bytes.fromhex(args.key) if args.key else b""
        self.ctx = None
        if not args.no_tls:
            cert, key = make_certificate(args.cert_dir)
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            self.ctx.load_cert_chain(cert, key)
            self.pin = spki_pin(cert)
            log("TLS 1.3 listener certificate %s, SPKI pin (adminCertSha256) %s" % (cert, self.pin))
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if args.rcvbuf:
            self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.rcvbuf)
        self.srv.bind((args.bind, args.port))
        self.srv.listen(4)
        log("listening on %s:%d (%s)" % (args.bind, args.port, "TLS" if self.ctx else "plain"))

    def accept(self, timeout=60.0, expect_tls_failure=False):
        """Accepts the next module connection and runs the handshake. Returns a Link."""
        self.srv.settimeout(timeout)
        raw, addr = self.srv.accept()
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        t0 = time.time()
        sock = raw
        if self.ctx:
            try:
                raw.settimeout(10)
                sock = self.ctx.wrap_socket(raw, server_side=True)
            except (ssl.SSLError, OSError) as e:
                raw.close()
                if expect_tls_failure:
                    return str(e)
                raise Closed("TLS handshake failed: %s" % e)
            if expect_tls_failure:
                sock.close()
                raise AssertionError("the TLS handshake succeeded although a failure was expected")
        link = Link(sock, self.args, self.key)
        f = link.recv_frame(10)
        if not f or f[0] != HELLO:
            raise Closed("no Hello")
        hello = decode(*f)
        if hello["version"] != VERSION:
            link.send(ERROR, W().u32(0).u16(ERR_VERSION).s("version %d not supported" % hello["version"]).b)
            raise Closed("version %d" % hello["version"])
        if self.args.server_id and hello["server_id"] != self.args.server_id:
            link.send(ERROR, W().u32(0).u16(ERR_AUTH).s("unknown server id").b)
            raise Closed("unknown server id %r" % hello["server_id"])
        nw = os.urandom(32)
        sid = hello["server_id"].encode("utf-8")
        tr = lambda label: label + b"\0" + struct.pack("<H", len(sid)) + sid + hello["nonce"] + nw
        proof_key = bytes.fromhex(self.args.challenge_key) if self.args.challenge_key else self.key
        link.send(CHALLENGE, nw + hmac.new(proof_key, tr(WEBADMIN_LABEL), hashlib.sha256).digest())
        f = link.recv_frame(10)
        if not f:
            raise Closed("no Auth")
        if f[0] == ERROR:
            e = decode(*f)
            raise Closed("the module refused the Challenge: Error %d %s" % (e["code"], e["message"]))
        if f[0] != AUTH:
            raise Closed("expected Auth, got 0x%04X" % f[0])
        auth = decode(*f)
        if not hmac.compare_digest(auth["proof"], hmac.new(self.key, tr(SERVER_LABEL), hashlib.sha256).digest()):
            link.send(ERROR, W().u32(0).u16(ERR_AUTH).s("bad proof").b)
            raise Closed("the module's proof is wrong")
        f = link.recv_frame(10)
        if not f or f[0] != WELCOME:
            raise Closed("no Welcome")
        link.welcome = decode(*f)
        w = link.welcome
        log("link from %s:%d: server id '%s' build '%s'; handshake %.0f ms; Welcome session %d tick %d '%s' ports %d/%d "
            "max %d caps %d map '%s' %s size %d index %d, %d maps, teams '%s'/'%s', status %d tickets %s" % (
                addr[0], addr[1], hello["server_id"], hello["build"], (time.time() - t0) * 1000, w["session"],
                w["tick"], w["server_name"], w["game_port"], w["gamespy_port"], w["max_players"], w["caps"], w["map"],
                w["mode"], w["map_size"], w["map_index"], len(w["maps"]), w["team1"], w["team2"],
                w["game"]["status"], w["game"]["tickets"]))
        f = link.recv_frame(10)
        if not f or f[0] != SNAPSHOT:
            raise Closed("no Snapshot after Welcome")
        link.handle(*f)
        return link


# ---------------------------------------------------------------- checks

class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, cond, what):
        if cond:
            self.passed += 1
            log("PASS", what)
        else:
            self.failed.append(what)
            log("FAIL", what)
        return cond

    def summary(self, name):
        log("%s: %d passed, %d failed%s" % (name, self.passed, len(self.failed),
                                            ("".join("\n  - " + f for f in self.failed)) if self.failed else ""))
        return 0 if not self.failed else 1


def bots_in(snapshot):
    return [p for p in snapshot["players"] if p["flags"] & INFO_BOT_CHANNEL]


def subscribe_all(link, args):
    for spec in args.sub:
        name, _, rate = spec.partition(":")
        stream = STREAMS[name]
        filters = []
        if stream in (3, 17, 20):
            filters = args.projectile_templates.split(",")
        elif stream == 18 and args.fire_templates:
            filters = args.fire_templates.split(",")
        r = link.request(SUBSCRIBE, p_subscribe(link.next_request(), stream, int(rate or 1), filters))
        log("  Subscribe %s -> %s" % (spec, r["name"]))


def report_link(link, seconds):
    log("link totals over %.0f s: %d events, %d gaps, %d resyncs; frames %s" % (
        seconds, len(link.events), link.gaps, link.resyncs,
        ", ".join("%s %d" % (NAMES.get(t, hex(t)), n) for t, n in sorted(link.frames_by_type.items()))))
    log("bytes per second: " + ", ".join("%s %.0f" % (NAMES.get(t, hex(t)), b / max(seconds, 1e-9))
                                         for t, b in sorted(link.bytes_by_type.items())))


# ---------------------------------------------------------------- scenarios

def scenario_listen(lst, args):
    link = lst.accept(args.accept_timeout)
    subscribe_all(link, args)
    t0 = time.time()
    try:
        link.pump(args.seconds)
    except Closed as e:
        log("link closed:", e)
    report_link(link, time.time() - t0)
    return 0


def scenario_r7(lst, args):
    """R7: HudEventRaw, RoundClock and SquadOrder against a running server. The server side only (what a client
    shows needs a retail client): each command is accepted or refused as the contract says, and the server keeps
    ticking (a Ping after them)."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    if args.vote_delay > 0:
        link.pump(args.vote_delay)
    r = link.request(CMD_HUD_EVENT_RAW, p_hud_event_raw(rq(), -1, 66, struct.pack("<i", 5)))
    c.check(r["name"] == "OK", "HudEventRaw 66 (help hint 5) to every client -> Ok")
    r = link.request(CMD_HUD_EVENT_RAW, p_hud_event_raw(rq(), -1, 43, spotted_payload(0, 0, 1, -200.0, 150.0, 1)))
    c.check(r["name"] == "OK", "HudEventRaw 43 (spotted marker) -> Ok")
    r = link.request(CMD_HUD_EVENT_RAW, p_hud_event_raw(rq(), -1, 66, bytes(52)))
    c.check(r["name"] == "OK", "HudEventRaw with a 52-byte payload -> Ok")
    r = link.request(CMD_HUD_EVENT_RAW, p_hud_event_raw(rq(), -1, 73, b""))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "HudEventRaw 73 (frees its payload) -> Error 7")
    r = link.request(CMD_HUD_EVENT_RAW, p_hud_event_raw(rq(), 200, 66, b""))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNKNOWN_PLAYER, "HudEventRaw to no player -> Error 4")
    r = link.request(CMD_ROUND_CLOCK, p_round_clock(rq(), -1, 60.0))
    c.check(r["name"] == "OK", "RoundClock 60 s to every client -> Ok")
    bots = bots_in(link.snapshot)
    if bots:
        r = link.request(CMD_ROUND_CLOCK, p_round_clock(rq(), bots[0]["id"], 0.0))
        c.check(r["name"] == "OK", "RoundClock 0 s to one player -> Ok")
    r = link.request(CMD_SQUAD_ORDER, p_squad_order(rq(), 1, 9, 6, False, 0, ((10, 150, 20),) * 3))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "SquadOrder to an empty squad -> Error 6")
    r = link.request(PING, p_ping(rq(), 77))
    c.check(r["name"] == "PONG" and r["clock"] == 77, "the server still ticks (Ping -> Pong)")
    c.summary("r7")
    return 0 if not c.failed else 1


def scenario_seat(lst, args):
    """SeatPreference (docs/admin-channel-draft.md "Seat preferences"): Welcome capability bit 4, Error 4 for no
    such player, Error 7 for a seat that is not 0, 1 or 255, and Ok for a player on the server (--seat-player, else
    the snapshot's first). With --seat-keep the preference is left set and the link held for --seconds, so a live
    spawn can be watched in the server log ("seat order: ..."); without it the preference is cleared again."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    caps = link.welcome["caps"]
    c.check(caps & CAP_SEAT_PREFERENCE, "Welcome capability bit 4 (SeatPreference) is set: caps %d" % caps)
    ids = [p["id"] for p in link.snapshot["players"]]
    missing = next(i for i in range(256) if i not in ids)
    r = link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), missing, SEAT_SECOND))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNKNOWN_PLAYER, "SeatPreference for no player (%d) -> Error 4" % missing)
    for bad in (2, 254):
        r = link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), ids[0] if ids else missing, bad))
        c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "SeatPreference seat %d -> Error 7" % bad)
    target = args.seat_player if args.seat_player >= 0 else (ids[0] if ids else -1)
    if target < 0:
        log("no player on the server: the Ok path is not tested")
    else:
        r = link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), target, args.seat))
        c.check(r["name"] == "OK", "SeatPreference player %d seat %d -> Ok" % (target, args.seat))
        if args.seat_keep:
            log("holding the link for %.0f s with player %d's preference set" % (args.seconds, target))
            link.pump(args.seconds)
        else:
            r = link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), target, SEAT_NONE))
            c.check(r["name"] == "OK", "SeatPreference player %d seat 255 (clear) -> Ok" % target)
    if args.seat_pair:
        seat_pair(link, c, args)
    r = link.request(PING, p_ping(rq(), 78))
    c.check(r["name"] == "PONG" and r["clock"] == 78, "the server still ticks (Ping -> Pong)")
    return c.summary("seat")


def seat_pair(link, c, args):
    """--seat-pair: two bot-channel bots on one team with the same spawn group (their pad), killed in the same tick,
    once without a preference and then with each kind. This goes through the engine's spawn pass, so it shows the
    module moving a real spawnAtTick; it does not show seats (the bots spawn on foot)."""
    rq = link.next_request
    ids, teams, end = None, {}, time.time() + 60
    while ids is None and time.time() < end:
        link.request(REQUEST_SNAPSHOT, W().u32(rq()).b)   # the bots may come after the link
        teams = {}
        for p in bots_in(link.snapshot):
            teams.setdefault(p["state"]["team"], []).append(p["id"])
        ids = next((v[:2] for v in teams.values() if len(v) >= 2), None)
        if ids is None:
            link.pump(2.0)
    if not c.check(ids is not None, "two bot-channel bots on one team (%s)" % teams):
        return
    a, b = ids
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 10, []))

    def alive(pid):
        st = dict(link.last_player_stream["players"]).get(pid) if link.last_player_stream else None
        return bool(st and st["life"] == 1)

    def round_(label, prefs):
        for pid in (a, b):
            r = link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), pid, prefs.get(pid, SEAT_NONE)))
            c.check(r["name"] == "OK", "%s: SeatPreference %d %d -> Ok" % (label, pid, prefs.get(pid, SEAT_NONE)))
        end = time.time() + 40
        while time.time() < end and not (alive(a) and alive(b)):
            link.pump(0.3)
        if not c.check(alive(a) and alive(b), "%s: both bots alive" % label):
            return None
        start = len(link.events)
        ra, rb = rq(), rq()
        link.send(CMD_KILL_PLAYER, p_kill(ra, a))   # back to back: run in the same tick
        link.send(CMD_KILL_PLAYER, p_kill(rb, b))
        deaths, spawns = {}, {}
        end = time.time() + 60
        while time.time() < end and len(spawns) < 2:
            link.pump(0.3)
            for m in link.events[start:]:
                if m["type"] == EV_DEATH and m["player"] in (a, b):
                    deaths.setdefault(m["player"], m["tick"])
                elif m["type"] == EV_SPAWN and m["player"] in deaths:
                    spawns.setdefault(m["player"], m["tick"])
        c.check(len(deaths) == 2 and deaths[a] == deaths[b], "%s: both died in one tick %s" % (label, deaths))
        if not c.check(len(spawns) == 2, "%s: both respawned %s" % (label, spawns)):
            return None
        log("%s: deaths %s, spawns %s" % (label, deaths, spawns))
        return spawns

    s = round_("no preference", {})
    if s:
        c.check(s[a] == s[b], "no preference: both spawn in the same pass (tick %d and %d)" % (s[a], s[b]))
    s = round_("A second seat", {a: SEAT_SECOND})
    if s:
        c.check(s[a] >= s[b] + 3, "A (%d) wants the second seat: A spawns a pass after B (tick %d, B %d)" % (a, s[a], s[b]))
    s = round_("B second seat", {b: SEAT_SECOND})
    if s:
        c.check(s[b] >= s[a] + 3, "B (%d) wants the second seat: B spawns a pass after A (tick %d, A %d)" % (b, s[b], s[a]))
    s = round_("B first seat", {b: SEAT_FIRST})
    if s:
        c.check(s[a] >= s[b] + 3, "B (%d) wants the first seat: A spawns a pass after B (tick %d, B %d)" % (b, s[a], s[b]))
    for pid in (a, b):
        link.request(CMD_SEAT_PREFERENCE, p_seat_preference(rq(), pid, SEAT_NONE))


def scenario_collision(lst, args):
    """SetCollision (docs/admin-channel-draft.md "Command semantics"): Welcome capability bit 5, Error 4, 5 and 7, and
    with a bot-channel bot seated in a vehicle (serverext_client.py --scenario wreck: it climbs, then its pilot is
    removed and the heli falls): collision off on the bot's root object (sent twice: the second changes nothing, see
    the server log's "collision off ... N of M objects changed"), the vehicle's height and health logged for --seconds
    or until it is destroyed, then on by networkable id (twice). --collision-control leaves collision on: the
    comparison run. --collision-off-seconds turns it on again after that long, while the height is still logged."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    caps = link.welcome["caps"]
    c.check(caps & CAP_SET_COLLISION, "Welcome capability bit 5 (SetCollision) is set: caps %d" % caps)
    ids = [p["id"] for p in link.snapshot["players"]]
    missing = next(i for i in range(256) if i not in ids)
    r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_PLAYER, missing, False))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNKNOWN_PLAYER, "SetCollision of no player (%d) -> Error 4" % missing)
    r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_CONSOLE, 0xFFFFFFF0, False))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "SetCollision of an unknown console id -> Error 5")
    r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_NET, 0, True))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "SetCollision of networkable id 0 -> Error 5")
    r = link.request(CMD_SET_COLLISION, W().u32(rq()).u8(REF_NET).u32(1).u8(2).b)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "SetCollision flags 2 -> Error 7 (%s)" % r.get("message"))
    r = link.request(CMD_SET_COLLISION, W().u32(rq()).u8(4).u32(1).u8(0).b)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "SetCollision reference kind 4 -> Error 7")

    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    link.request(SUBSCRIBE, p_subscribe(rq(), 2, 10))
    seated, end = None, time.time() + args.collision_wait
    while seated is None and time.time() < end:
        link.pump(0.2)
        for pid, st in (link.last_player_stream or {}).get("players", []):
            if st["life"] == 1 and st["flags"] & 1 and st["root_net"]:
                seated = (pid, st)
    if not c.check(seated is not None, "a player seated in a vehicle within %.0f s" % args.collision_wait):
        return c.summary("collision")
    pid, st = seated
    net = st["root_net"]
    if args.collision_control:
        log("control run: collision stays on for player %d's vehicle %d" % (pid, net))
    else:
        for n in (1, 2):
            r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_PLAYER, pid, False))
            c.check(r["name"] == "OK", "SetCollision off (%d) of player %d's root object (vehicle %d) -> Ok" % (n, pid, net))
    y0 = st["pos"][1]
    start = time.time()
    low, last, end, next_log, back_on = y0, None, start + args.seconds, 0.0, False
    while time.time() < end:
        link.pump(0.2)
        if args.collision_off_seconds > 0 and not back_on and time.time() - start >= args.collision_off_seconds:
            back_on = True
            r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_NET, net, True))
            c.check(r["name"] == "OK", "SetCollision on after %.1f s by networkable id %d -> %s (lowest so far %+.1f)" % (
                args.collision_off_seconds, net, r["name"], low - y0))
        v = [x for x in (link.last_vehicle_stream or {}).get("vehicles", []) if x["net"] == net]
        if not v:
            if last is not None:
                log("vehicle %d is gone from the vehicle stream" % net)
                break
            continue
        last = v[0]
        low = min(low, last["pos"][1])
        if time.time() >= next_log:
            next_log = time.time() + 1.0
            log("vehicle %d y %.1f (%+.1f from the start, lowest %+.1f), health %.2f, flags %d" % (
                net, last["pos"][1], last["pos"][1] - y0, low - y0, last["health"], last["flags"]))
        if last["flags"] & 1:
            ev = [m for m in link.events if m["type"] == EV_VEHICLE_DESTROYED and m["net"] == net]
            y = ev[0]["pos"][1] if ev else last["pos"][1]
            log("vehicle %d destroyed at y %.1f (%+.1f from the start)" % (net, y, y - y0))
            break
    destroyed = [m for m in link.events if m["type"] == EV_VEHICLE_DESTROYED and m["net"] == net]
    log("%s: vehicle %d lowest y %.1f (%+.1f from the start y %.1f), last %s, VehicleDestroyed events %d" % (
        "control" if args.collision_control else "collision off", net, low, low - y0, y0,
        last and "y %.1f health %.2f flags %d" % (last["pos"][1], last["health"], last["flags"]), len(destroyed)))
    for n in (1, 2):
        r = link.request(CMD_SET_COLLISION, p_set_collision(rq(), REF_NET, net, True))
        c.check(r["name"] == "OK" or (r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE),
                "SetCollision on (%d) by networkable id %d -> %s%s" % (n, net, r["name"],
                                                                        " (the vehicle is gone)" if r["name"] == "ERROR" else ""))
    r = link.request(PING, p_ping(rq(), 79))
    c.check(r["name"] == "PONG" and r["clock"] == 79, "the server still ticks (Ping -> Pong)")
    return c.summary("collision")


def scenario_vote(lst, args):
    """Starts a server vote (--vote-text, --vote-seconds, --vote-team, --vote-percent, --vote-min) and prints what
    the players are sent: the module's chat lines (vote lines, confirmations, the result), the kills that push rows,
    and VoteResult. Ballots need retail clients pressing PgUp/PgDn. With no human player the module refuses the vote
    (Error 6); the scenario then checks that CancelVote is refused too and stops."""
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    if args.vote_delay > 0:
        link.pump(args.vote_delay)
    r = link.request(CMD_START_VOTE, p_start_vote(rq(), args.vote_text, args.vote_seconds, args.vote_team,
                                                 args.vote_percent, args.vote_min))
    if r["name"] != "OK":
        log("StartVote -> %s %s %s" % (r["name"], r.get("code"), r.get("message")))
        r2 = link.request(CMD_CANCEL_VOTE, W().u32(rq()).b)
        log("CancelVote with no vote running -> %s %s %s" % (r2["name"], r2.get("code"), r2.get("message")))
        ok = r["name"] == "ERROR" and r["code"] == ERR_REFUSED and r2["name"] == "ERROR" and r2["code"] == ERR_REFUSED
        return 0 if ok else 1
    r2 = link.request(CMD_START_VOTE, p_start_vote(rq(), "second", 10))
    log("a second StartVote -> %s %s %s" % (r2["name"], r2.get("code"), r2.get("message")))
    t0 = time.time()
    seen = len(link.events)
    result = None
    lines = 0
    rows_sent = 0
    try:
        while result is None and time.time() < t0 + args.vote_seconds + 15:
            # --vote-rows: server messages from 3 s on, one per 0.5 s, each a row in every box.
            if rows_sent < args.vote_rows and time.time() >= t0 + 3.0 + 0.5 * rows_sent:
                rows_sent += 1
                link.request(CMD_SAY, p_say(rq(), 0, 0, "row %d" % rows_sent))
            link.pump(0.25)
            for m in link.events[seen:]:
                if m["type"] == EV_CHAT and m["sender"] == -1:
                    lines += 1
                    to = ("player %d" % m["engine_flags"]) if m["channel"] == 14 else m["channel_name"]
                    log("%6.1f s  -> %-16s %s" % (time.time() - t0, to, m["text"]))
                elif m["type"] == EV_KILL:
                    log("%6.1f s  kill (a row in every box): %d -> %d" % (time.time() - t0, m["attacker"], m["victim"]))
                elif m["type"] == EV_VOTE_RESULT:
                    result = m
            seen = len(link.events)
    except Closed as e:
        log("link closed:", e)
        return 1
    if result is None:
        log("no VoteResult")
        return 1
    log("VoteResult: %s, yes %d, no %d, voters %d (%d server lines seen): %s" % (
        VOTE_OUTCOMES.get(result["outcome"], result["outcome"]), result["yes"], result["no"], result["eligible"], lines,
        result["text"]))
    return 0

SCRIPT_REASONS = {1: "finished", 2: "stopped", 3: "error", 4: "over budget", 5: "level unload"}


def script_events(link, instance, start=0):
    return [m for m in link.events[start:] if m["type"] == EV_SCRIPT_EVENT and m["instance"] == instance]


def wait_finished(link, instance, timeout=5.0):
    """Waits for a script's ScriptFinished, looking at every event received so far too."""
    end = time.time() + timeout
    while True:
        for m in link.events:
            if m["type"] == EV_SCRIPT_FINISHED and m["instance"] == instance:
                return m
        if time.time() >= end:
            return None
        link.pump(0.1)


def scenario_script(lst, args):
    """Scripts (docs/admin-channel-draft.md "Scripts"): the commands, the life cycle and every end reason against a
    running server; with a bot-channel bot in the snapshot (serverext_client.py) also the host API against the player
    stream: game.player, teleport, setVelocity (a soft ceiling) and setAngularVelocity (axis and sign)."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    if args.vote_delay > 0:
        link.pump(args.vote_delay)
    c.check(link.welcome["caps"] & CAP_SCRIPTS, "Welcome capability bit 3 (scripts) is set: caps %d" % link.welcome["caps"])

    def start(instance, name, source, script_args="", api=1, flags=0):
        return link.request(CMD_START_SCRIPT, p_start_script(rq(), instance, name, source, script_args, api, flags))

    # A script that counts ticks, emits each one and finishes itself.
    r = start(1, "count", "let n = 0;\n"
                          "script.onStop(reason => script.emit({ stop: reason }));\n"
                          "script.onTick(tick => {\n"
                          "  n++;\n"
                          "  script.emit({ n, tick, gameTick: game.tick });\n"
                          "  if (n === script.args.ticks) script.finish({ ticks: n });\n"
                          "});\n", '{"ticks":5}')
    c.check(r["name"] == "OK", "StartScript -> Ok (%s)" % r.get("message", ""))
    fin = wait_finished(link, 1)
    evs = [json.loads(m["payload"]) for m in script_events(link, 1) if m["kind"] == 1]
    c.check(fin is not None and fin["reason"] == 1 and fin["result"] == '{"ticks":5}' and fin["script"] == "count",
            "ScriptFinished reason 1 with the result: %s" % (fin and {k: fin[k] for k in ("reason", "result", "calls", "total_us", "max_us")},))
    c.check(len(evs) == 6 and [e.get("n") for e in evs[:5]] == [1, 2, 3, 4, 5] and evs[5] == {"stop": "finished"},
            "five ScriptEvents, one per tick, then the stop handler's: %s" % (evs,))
    ticks = [e["tick"] for e in evs[:5]]
    c.check(all(b - a == 1 for a, b in zip(ticks, ticks[1:])) and all(e["tick"] == e["gameTick"] for e in evs[:5]),
            "consecutive ticks, and the handler's tick is game.tick: %s" % (ticks,))
    c.check(fin is not None and fin["calls"] == 7, "7 calls: the top level, 5 ticks and the stop handler (%s)" % (fin and fin["calls"],))

    # The world as the script sees it, against Welcome and the snapshot.
    snap = link.request(REQUEST_SNAPSHOT, W().u32(rq()).b)
    r = start(2, "world", "script.emit({ api: script.api, name: script.name, args: script.args, map: game.map,\n"
                          "  rate: game.tickRate, tick: game.tick, time: game.time, players: game.players() });\n"
                          "script.log('hello', 1, true);\n"
                          "game.say('§C1001script say é');\n"
                          "script.finish();\n")
    fin = wait_finished(link, 2)
    evs = script_events(link, 2)
    world = json.loads(evs[0]["payload"]) if evs else {}
    ids = sorted(p["id"] for p in snap["players"])
    c.check(r["name"] == "OK" and fin is not None and fin["reason"] == 1 and fin["result"] == "",
            "a script that finishes in its top-level code: Ok, then ScriptFinished reason 1 with no result")
    c.check(world.get("api") == 1 and world.get("name") == "world" and world.get("args") is None and
            world.get("map") == link.welcome["map"] and world.get("rate") == 30 and sorted(world.get("players", [])) == ids,
            "script.api/name/args, game.map/tickRate/players(): %s (snapshot players %s)" % (world, ids))
    c.check(len(evs) == 2 and evs[1]["kind"] == 2 and evs[1]["payload"] == "hello 1 true", "script.log -> ScriptEvent kind 2 %r" % (evs[1:],))
    chat = link.wait_event(lambda m: m["type"] == EV_CHAT and "script say" in m["text"], 3)
    c.check(chat is not None and chat["text"] == "§C1001script say é" and chat["sender"] == -1,
            "game.say -> Chat event from the server: %r" % (chat and chat["text"],))

    # A long-running script: ListScripts, ScriptMessage, StopScript.
    r = start(3, "echo", "script.onMessage(m => script.emit({ got: m }));\n"
                         "script.onStop(reason => script.emit({ stop: reason }));\n"
                         "script.onTick(() => {});\n")
    c.check(r["name"] == "OK", "StartScript of a script that keeps running -> Ok")
    r = link.request(CMD_LIST_SCRIPTS, W().u32(rq()).b)
    c.check(r["name"] == "SCRIPT_LIST" and r["scripts"] == [(3, "echo")], "ListScripts -> %s" % (r.get("scripts"),))
    r = start(3, "again", "1;")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "StartScript with a running script's instance -> Error 6")
    r = link.request(CMD_SCRIPT_MESSAGE, p_script_message(rq(), 3, '{"altitude":250}'))
    ev = link.wait_event(lambda m: m["type"] == EV_SCRIPT_EVENT and m["instance"] == 3, 3)
    c.check(r["name"] == "OK" and ev is not None and json.loads(ev["payload"]) == {"got": {"altitude": 250}},
            "ScriptMessage -> Ok and the handler's ScriptEvent %r" % (ev and ev["payload"],))
    r = link.request(CMD_SCRIPT_MESSAGE, p_script_message(rq(), 3, "{altitude:250}"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "ScriptMessage that is not JSON -> Error 7 (%s)" % r.get("message"))
    r = link.request(CMD_SCRIPT_MESSAGE, p_script_message(rq(), 99, "1"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "ScriptMessage to no script -> Error 5")
    link.pump(1.0)
    seen = len(link.events)
    r = link.request(CMD_STOP_SCRIPT, W().u32(rq()).u32(3).b)
    fin = wait_finished(link, 3)
    stop_ev = script_events(link, 3, seen)
    c.check(r["name"] == "OK" and fin is not None and fin["reason"] == 2 and fin["result"] == "" and
            len(stop_ev) == 1 and json.loads(stop_ev[0]["payload"]) == {"stop": "stopped"},
            "StopScript -> Ok, the stop handler's event, ScriptFinished reason 2 after %s calls" % (fin and fin["calls"],))
    if fin:
        log("  the idle script: %d calls, CPU %d us in all (%.2f us per call), longest %d us" % (
            fin["calls"], fin["total_us"], fin["total_us"] / max(fin["calls"], 1), fin["max_us"]))
    r = link.request(CMD_STOP_SCRIPT, W().u32(rq()).u32(3).b)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "StopScript of no script -> Error 5")
    r = link.request(CMD_LIST_SCRIPTS, W().u32(rq()).b)
    c.check(r["name"] == "SCRIPT_LIST" and r["scripts"] == [], "ListScripts -> none")

    # Errors.
    r = start(4, "throws", "function boom() {\n  throw new Error('boom');\n}\nscript.onTick(() => boom());\n")
    fin = wait_finished(link, 4)
    c.check(r["name"] == "OK" and fin is not None and fin["reason"] == 3 and "Error: boom" in fin["result"] and
            "throws:2:" in fin["result"], "a handler that throws -> ScriptFinished reason 3: %r" % (fin and fin["result"],))
    r = start(5, "loops", "script.onTick(() => { for (;;) {} });\n")
    fin = wait_finished(link, 5)
    c.check(r["name"] == "OK" and fin is not None and fin["reason"] == 4 and "over the time budget" in fin["result"],
            "an endless loop -> ScriptFinished reason 4: %r, longest call %s us" % (
                fin and fin["result"].split("\n")[0], fin and fin["max_us"]))
    r = link.request(PING, p_ping(rq(), 77))
    c.check(r["name"] == "PONG" and r["clock"] == 77, "the server still ticks (Ping -> Pong)")
    r = start(6, "syntax", "let a = 1;\nlet b = ;\n")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID and "SyntaxError" in r["message"] and "syntax:2:" in r["message"],
            "a syntax error -> Error 7 with its line: %r" % (r.get("message"),))
    r = start(6, "toplevel", "null.x;\n")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID and "TypeError" in r["message"],
            "top-level code that throws -> Error 7: %r" % (r.get("message", "").split("\n")[0],))
    r = start(6, "args", "1;", "{nope}")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "arguments that are not JSON -> Error 7 (%s)" % r.get("message", "").split("\n")[0])
    r = start(6, "newer", "1;", "", 2)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "script API 2 -> Error 5 (%s)" % r.get("message"))
    r = start(6, "flags", "1;", "", 1, 1)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "flags 1 -> Error 7")
    r = start(0, "zero", "1;")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "instance 0 -> Error 7")
    r = start(6, "bad\tname", "1;")
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "a name with a control character -> Error 7")
    r = start(6, "absent", "script.emit(['Date', 'RegExp', 'Proxy', 'setTimeout', 'console', 'std', 'os']\n"
                            "  .filter(n => typeof globalThis[n] !== 'undefined'));\n"
                            "let e1 = ''; try { eval('1'); } catch (e) { e1 = e.name; }\n"
                            "let e2 = ''; try { new Function('return 1'); } catch (e) { e2 = e.name; }\n"
                            "script.emit([e1, e2]); script.finish();\n")
    fin = wait_finished(link, 6)
    evs = [m["payload"] for m in script_events(link, 6)]
    c.check(r["name"] == "OK" and evs == ['[]', '["TypeError","TypeError"]'],
            "no Date, RegExp, Proxy, timers, console, std or os; eval and Function throw: %s" % (evs,))

    # Eight scripts at most; then all end with the ninth refused.
    ok = 0
    for i in range(8):
        r = start(10 + i, "slot%d" % i, "script.onTick(() => {});\n")
        ok += r["name"] == "OK"
    r = start(18, "ninth", "1;")
    c.check(ok == 8 and r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "8 scripts run, the ninth -> Error 6")
    link.pump(2.0)
    for i in range(8):
        link.request(CMD_STOP_SCRIPT, W().u32(rq()).u32(10 + i).b)
    link.pump(0.3)
    fins = [m for m in link.events if m["type"] == EV_SCRIPT_FINISHED and 10 <= m["instance"] < 18]
    c.check(len(fins) == 8 and all(m["reason"] == 2 for m in fins), "all eight stopped")
    if fins:
        calls = sum(m["calls"] for m in fins)
        log("  eight idle scripts for 2 s: %d calls, CPU %.2f us per call, longest call %d us" % (
            calls, sum(m["total_us"] for m in fins) / max(calls, 1), max(m["max_us"] for m in fins)))

    bots = bots_in(link.snapshot if not snap else snap)
    if not bots:
        log("no bot-channel bot in the snapshot: the host API checks against a player are skipped "
            "(start serverext_client.py first)")
        return c.summary("script")
    bid = bots[0]["id"]
    log("using bot %d '%s'" % (bid, bots[0]["name"]))
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    link.pump(0.5)

    def stream_state():
        return dict(link.last_player_stream["players"]).get(bid) if link.last_player_stream else None

    # game.player against the player stream of the same tick.
    states = {}
    link.on_frame = lambda m: states.__setitem__(m["tick"], dict(m["players"]).get(bid)) if m["type"] == PLAYER_STREAM else None
    r = start(20, "player", "let n = 0;\n"
                            "script.onTick(tick => { script.emit({ tick, p: game.player(script.args.id) }); if (++n === 10) script.finish(); });\n",
              '{"id":%d}' % bid)
    fin = wait_finished(link, 20)
    link.pump(0.2)
    link.on_frame = None
    samples = [json.loads(m["payload"]) for m in script_events(link, 20)]
    compared = 0
    worst = 0.0
    same = True
    for smp in samples:
        st = states.get(smp["tick"])
        p = smp["p"]
        if not st or not p or not p["position"]:
            continue
        compared += 1
        worst = max(worst, max(abs(a - b) for a, b in zip(p["position"], st["pos"])),
                    max(abs(a - b) for a, b in zip(p["velocity"], st["vel"])))
        rot = max(abs((a - b + 180.0) % 360.0 - 180.0) for a, b in zip(p["rotation"], st["rot"]))
        same = same and rot < 0.01 and p["team"] == st["team"] and p["rootId"] == st["root_net"] and \
            p["alive"] == (st["life"] != 0) and p["seated"] == bool(st["flags"] & 1) and \
            p["seat"] == (-1 if st["seat"] == 255 else st["seat"]) and p["bot"] is True and p["id"] == bid
    first = samples[0]["p"] if samples else None
    c.check(r["name"] == "OK" and compared >= 5 and same,
            "game.player(%d) in %d ticks equals the player stream of the same tick (team, alive, seat, rootId, rotation): %s" % (
                bid, compared, first and {k: first[k] for k in ("name", "team", "alive", "bot", "seated", "seat", "rootId", "vehicle")}))
    # The script runs before this tick's streams are built, so both read the same state.
    c.check(compared >= 5 and worst < 0.01, "its position and velocity equal the stream's of the same tick (largest difference %.4f)" % worst)
    r = start(21, "nobody", "script.emit([game.player(250), game.teleport(250, [0, 100, 0], null), game.setVelocity(250, [0, 0, 0])]); script.finish();")
    wait_finished(link, 21)
    evs = [m["payload"] for m in script_events(link, 21)]
    c.check(evs == ["[null,false,false]"], "an unknown player: game.player null, teleport and setVelocity false (%s)" % (evs,))

    st = stream_state()
    if not st or not st["life"]:
        c.check(False, "the bot is not alive: teleport and speed checks skipped")
        return c.summary("script")
    in_vehicle = bool(st["flags"] & 1)
    what = "helicopter" if in_vehicle else "soldier"
    log("the bot is %s at %.1f %.1f %.1f, speed %.1f %.1f %.1f" % (("in a vehicle" if in_vehicle else "on foot",) + tuple(st["pos"]) + tuple(st["vel"])))
    track = []
    link.on_frame = lambda m: track.append((m["tick"], dict(m["players"]).get(bid))) if m["type"] == PLAYER_STREAM else None

    if in_vehicle:
        # A soft ceiling 15 m below a hovering helicopter whose pilot keeps pulling up to its own height:
        # setVelocity every tick it is above, as the .nasa script will do it.
        ceiling = st["pos"][1] - 15.0
        r = start(24, "ceiling",
                  "const { id, ceiling } = script.args;\n"
                  "let pushes = 0, highest = -1e9;\n"
                  "script.onTick(() => {\n"
                  "  const p = game.player(id);\n"
                  "  if (!p || !p.position) return;\n"
                  "  const over = p.position[1] - ceiling;\n"
                  "  if (pushes > 0 && over > highest) highest = over;\n"
                  "  if (over <= 0) return;\n"
                  "  const down = -Math.min(30, over * 2);\n"
                  "  if (p.velocity[1] > down) { game.setVelocity(id, [p.velocity[0], down, p.velocity[2]]); pushes++; }\n"
                  "});\n"
                  "script.onStop(() => script.emit({ pushes, highest }));\n", json.dumps({"id": bid, "ceiling": ceiling}))
        link.pump(8.0)
        seen = len(track)
        link.pump(4.0)
        link.request(CMD_STOP_SCRIPT, W().u32(rq()).u32(24).b)
        fin = wait_finished(link, 24)
        evs = [json.loads(m["payload"]) for m in script_events(link, 24)]
        ys = [s["pos"][1] for _, s in track if s]
        late = [s["pos"][1] for _, s in track[seen:] if s]
        c.check(r["name"] == "OK" and ys and late and ys[0] > ceiling + 10 and max(late) < ceiling + 3.0 and
                evs and evs[0]["pushes"] > 30,
                "a soft ceiling 15 m below a hovering %s: y %.1f at the start; in seconds 8-12 between %.1f and %.1f "
                "(ceiling %.1f); %s pushes, highest %.2f m over after the first" % (
                    what, ys[0] if ys else float("nan"), min(late) if late else float("nan"),
                    max(late) if late else float("nan"), ceiling, evs[0]["pushes"] if evs else "?",
                    evs[0]["highest"] if evs else float("nan")))
        if fin:
            log("  the ceiling script: %d calls, CPU %.2f us per call, longest %d us" % (
                fin["calls"], fin["total_us"] / max(fin["calls"], 1), fin["max_us"]))
        link.pump(8.0)   # the pilot climbs back to its height

    # game.setVelocity once: the speed the stream shows in the next ticks.
    del track[:]
    push = [0, 15, 0] if in_vehicle else [0, 8, 0]
    r = start(23, "push", "script.emit({ tick: game.tick, ok: game.setVelocity(script.args.id, script.args.v) }); script.finish();",
              json.dumps({"id": bid, "v": push}))
    wait_finished(link, 23)
    link.pump(0.6)
    evs = [json.loads(m["payload"]) for m in script_events(link, 23)]
    set_tick = evs[0]["tick"] if evs else 0
    after_set = [(t, s["vel"][1], s["pos"][1]) for t, s in track if s and t >= set_tick][:10]
    c.check(r["name"] == "OK" and evs and evs[0]["ok"] and after_set and after_set[0][1] > push[1] * 0.4,
            "game.setVelocity %s once on a %s: vertical speed in the following ticks %s, height %+.2f m after %d ticks" % (
                push, what, ["%d: %.1f" % (t, v) for t, v, _ in after_set],
                (after_set[-1][2] - after_set[0][2]) if after_set else float("nan"), len(after_set)))
    link.pump(3.0)

    # game.setVelocity every tick for a second: a held speed.
    del track[:]
    hold = [0, 0, 6] if in_vehicle else [0, 0, 3]
    r = start(25, "hold", "const { id, v } = script.args; let n = 0;\n"
                          "const from = game.player(id).position;\n"
                          "script.onTick(() => { game.setVelocity(id, v); if (++n === 30) { const to = game.player(id).position;\n"
                          "  script.finish({ moved: [to[0] - from[0], to[1] - from[1], to[2] - from[2]] }); } });\n",
              json.dumps({"id": bid, "v": hold}))
    fin = wait_finished(link, 25)
    moved = json.loads(fin["result"])["moved"] if fin and fin["reason"] == 1 else None
    c.check(r["name"] == "OK" and moved is not None and abs(moved[2] - hold[2]) < hold[2] * 0.5,
            "game.setVelocity %s every tick for 30 ticks on a %s: moved %s m (the speed times one second is %s)" % (
                hold, what, moved and [round(x, 2) for x in moved], hold))
    link.pump(4.0)

    # game.teleport: 40 m up, the rotation kept.
    st = stream_state()
    target = [st["pos"][0], st["pos"][1] + 40.0, st["pos"][2]]
    r = start(22, "teleport", "script.emit(game.teleport(script.args.id, script.args.pos, null)); script.finish();",
              json.dumps({"id": bid, "pos": target}))
    wait_finished(link, 22)
    link.pump(0.15)
    after = stream_state()
    c.check(r["name"] == "OK" and after and abs(after["pos"][1] - target[1]) < 5.0 and
            abs((after["rot"][0] - st["rot"][0] + 180.0) % 360.0 - 180.0) < 5.0,
            "game.teleport of a %s 40 m up: y %.1f -> %.1f (target %.1f), yaw %.1f -> %.1f" % (
                what, st["pos"][1], after["pos"][1] if after else float("nan"), target[1], st["rot"][0],
                after["rot"][0] if after else float("nan")))
    link.pump(8.0 if in_vehicle else 4.0)

    # game.setAngularVelocity about each world axis, from a level attitude with the nose north (yaw 0), set every
    # tick for half a second: which angle turns, and which way. The script reads the rotation itself.
    for axis, name in ((1, "y"), (0, "x"), (2, "z")):
        v = [0.0, 0.0, 0.0]
        v[axis] = 1.0
        r = start(30 + axis, "spin-" + name,
                  "const { id, v } = script.args;\nlet n = 0;\nconst rot = [];\n"
                  "const yaw = game.player(id).rotation[0];\n"
                  "game.teleport(id, null, [0, 0, 0]);\n"
                  "script.onTick(() => {\n"
                  "  rot.push(game.player(id).rotation);\n"
                  "  if (++n === 16) { game.setAngularVelocity(id, [0, 0, 0]); game.teleport(id, null, [yaw, 0, 0]); script.finish({ rot }); return; }\n"
                  "  game.setAngularVelocity(id, v);\n"
                  "});\n", json.dumps({"id": bid, "v": v}))
        fin = wait_finished(link, 30 + axis)
        rots = json.loads(fin["result"])["rot"] if fin and fin["reason"] == 1 else []
        if len(rots) == 16:
            turned = [sum(((b[k] - a[k] + 180.0) % 360.0) - 180.0 for a, b in zip(rots, rots[1:])) for k in range(3)]
            text = "game.setAngularVelocity 1 rad/s about world %s on a level %s facing north, 15 ticks (28.6 degrees): yaw %+.1f, pitch %+.1f, roll %+.1f degrees (from %s)" % (
                (name,) + (what,) + tuple(turned) + ([round(x, 1) for x in rots[0]],))
            if in_vehicle:
                c.check(r["name"] == "OK" and max(abs(t) for t in turned) > 15.0, text)
            else:
                log("NOTE " + text)
        else:
            c.check(False, "setAngularVelocity about %s: %s" % (name, fin and fin["result"]))
        link.pump(5.0 if in_vehicle else 1.0)
    link.on_frame = None
    r = link.request(PING, p_ping(rq(), 78))
    c.check(r["name"] == "PONG", "the server still ticks (Ping -> Pong)")
    return c.summary("script")


def scenario_runscript(lst, args):
    """Runs one script file (--script-file, JavaScript) with --script-args for --seconds, printing its ScriptEvents
    (log lines and emitted data), then stops it and prints its ScriptFinished with the call count and CPU time.
    For trying a script on a test server without WebAdmin."""
    with open(args.script_file, encoding="utf-8") as f:
        source = f.read()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    if args.vote_delay > 0:
        link.pump(args.vote_delay)
    name = args.script_name or os.path.splitext(os.path.basename(args.script_file))[0]
    r = link.request(CMD_START_SCRIPT, p_start_script(rq(), 1, name, source, args.script_args))
    if r["name"] != "OK":
        log("StartScript -> %s %s: %s" % (r["name"], r.get("code"), r.get("message")))
        return 1
    fin = None
    seen = 0
    end = time.time() + args.seconds
    stopped = False
    try:
        while fin is None and time.time() < end + 3:
            link.pump(0.2)
            for m in link.events[seen:]:
                if m["type"] == EV_SCRIPT_EVENT:
                    log("%s tick %d: %s" % ("log " if m["kind"] == 2 else "emit", m["tick"], m["payload"]))
                elif m["type"] == EV_SCRIPT_FINISHED:
                    fin = m
            seen = len(link.events)
            if not stopped and time.time() >= end:
                stopped = True
                link.request(CMD_STOP_SCRIPT, W().u32(rq()).u32(1).b)
    except Closed as e:
        log("link closed:", e)
        return 1
    if fin is None:
        log("no ScriptFinished")
        return 1
    log("ScriptFinished: %s after %d calls, CPU %d us in all (%.2f us per call), longest call %d us%s" % (
        SCRIPT_REASONS.get(fin["reason"], fin["reason"]), fin["calls"], fin["total_us"],
        fin["total_us"] / max(fin["calls"], 1), fin["max_us"], (": " + fin["result"]) if fin["result"] else ""))
    return 0 if fin["reason"] in (1, 2) else 1


def scenario_scriptends(lst, args):
    """Scripts and the two ends that come from outside: a level unload (Map action 2, the next map) ends every script
    with reason 5 after its stop handlers, and a closing link ends them without a report (the stop handler's chat
    line "script stop: link closed" shows in a bot-channel client's Chat messages and in the module's log)."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    if args.vote_delay > 0:
        link.pump(args.vote_delay)
    source = ("script.onTick(() => {});\n"
              "script.onStop(reason => { script.emit({ stop: reason, players: game.players().length, map: game.map });\n"
              "  game.say('script stop: ' + reason); });\n")
    r1 = link.request(CMD_START_SCRIPT, p_start_script(rq(), 1, "first", source))
    r2 = link.request(CMD_START_SCRIPT, p_start_script(rq(), 2, "second", "script.onTick(() => {});\n"))
    c.check(r1["name"] == "OK" and r2["name"] == "OK", "two scripts started")
    link.pump(1.0)
    before = link.welcome["map"]
    r = link.request(CMD_MAP, p_map(rq(), 2))
    c.check(r["name"] == "OK", "Map next (action 2) -> Ok")
    f1 = wait_finished(link, 1, 30)
    f2 = wait_finished(link, 2, 5)
    evs = [json.loads(m["payload"]) for m in script_events(link, 1)]
    c.check(f1 is not None and f2 is not None and f1["reason"] == 5 and f2["reason"] == 5 and f1["result"] == "",
            "both scripts ended with reason 5 (level unload): %s" % ([f and f["reason"] for f in (f1, f2)],))
    c.check(len(evs) == 1 and evs[0].get("stop") == "level unload" and evs[0].get("map") == before,
            "the stop handler ran first and still saw the game: %s" % (evs,))
    try:
        ev = link.wait_event(lambda m: m["type"] == EV_ROUND_START, 60)
    except Closed as e:
        ev = None
        c.check(False, "the link closed during the map change: %s" % e)
    r = link.request(CMD_LIST_SCRIPTS, W().u32(rq()).b, timeout=10)
    c.check(r["name"] == "SCRIPT_LIST" and r["scripts"] == [], "after the map change (RoundStart %s): ListScripts -> none" % (
        ev and ev.get("map"),))
    r = link.request(CMD_START_SCRIPT, p_start_script(rq(), 3, "after", source))
    c.check(r["name"] == "OK", "a script starts on the new map")
    link.pump(1.0)
    session = link.welcome["session"]
    link.sock.close()
    log("closed the link with script 3 running")
    link2 = lst.accept(args.accept_timeout)
    rq2 = link2.next_request
    r = link2.request(CMD_LIST_SCRIPTS, W().u32(rq2()).b)
    c.check(link2.welcome["session"] == session + 1 and r["name"] == "SCRIPT_LIST" and r["scripts"] == [],
            "the next link (session %d -> %d): ListScripts -> none" % (session, link2.welcome["session"]))
    r = link2.request(CMD_START_SCRIPT, p_start_script(rq2(), 3, "again", "script.emit(game.map); script.finish();"))
    fin = wait_finished(link2, 3)
    c.check(r["name"] == "OK" and fin is not None and fin["reason"] == 1, "instance 3 is free again and a script runs")
    r = link2.request(PING, p_ping(rq2(), 79))
    c.check(r["name"] == "PONG", "the server still ticks (Ping -> Pong)")
    return c.summary("scriptends")


def scenario_commands(lst, args):
    c = Checks()
    link = lst.accept(args.accept_timeout)
    snap = link.snapshot
    bots = bots_in(snap)
    if not bots:
        log("no bot-channel bots in the snapshot; start serverext_client.py first")
        return 2
    bot = bots[0]
    bid = bot["id"]
    log("using bot %d '%s' team %d" % (bid, bot["name"], bot["state"]["team"]))
    rq = link.next_request

    r = link.request(PING, p_ping(rq(), 12345))
    c.check(r["name"] == "PONG" and r["clock"] == 12345, "Ping -> Pong from the game thread")
    r = link.request(REQUEST_SNAPSHOT, W().u32(rq()).b)
    c.check(r["name"] == "SNAPSHOT" and r["request"] == link.req and r["last_seq"] >= 0, "RequestSnapshot -> Snapshot")

    r = link.request(CMD_CONSOLE, p_console(rq(), "sv.serverName"))
    c.check(r["name"] == "CONSOLE_RESULT" and r["lines"] and r["lines"][0] == link.welcome["server_name"],
            "Console sv.serverName -> %r" % (r.get("lines"),))
    r = link.request(CMD_CONSOLE, p_console(rq(), "maplist.list"))
    c.check(r["name"] == "CONSOLE_RESULT" and len(r["lines"]) >= 1, "Console maplist.list -> %d lines" % len(r.get("lines", [])))
    r = link.request(CMD_CONSOLE, p_console(rq(), "a\nb"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Console with a newline -> Error 7")

    for target, ident, label in ((0, 0, "all"), (1, 2, "team 2"), (2, bid, "player %d" % bid)):
        text = "§C1001admin say %s é" % label
        r = link.request(CMD_SAY, p_say(rq(), target, ident, text))
        ev = link.wait_event(lambda m: m["type"] == EV_CHAT and "admin say %s" % label in m["text"], 3)
        c.check(r["name"] == "OK" and ev is not None and ev["text"] == text,
                "Say to %s -> Ok and Chat event channel %s text %r" % (label, ev and ev["channel"], ev and ev["text"]))
    r = link.request(CMD_SAY, p_say(rq(), 2, 250, "x"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNKNOWN_PLAYER, "Say to an unknown player -> Error 4")

    # Teleport the bot's root (its soldier or vehicle) up 50 m, keep rotation; then rotation only.
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    link.pump(0.3)
    state = dict(link.last_player_stream["players"]).get(bid) if link.last_player_stream else None
    if state and state["life"]:
        target = (state["pos"][0], state["pos"][1] + 50.0, state["pos"][2])
        r = link.request(CMD_TELEPORT, p_teleport(rq(), REF_PLAYER, bid, pos=target))
        link.pump(0.2)
        after = dict(link.last_player_stream["players"]).get(bid)
        moved = after and abs(after["pos"][1] - target[1]) < 3.0
        c.check(r["name"] == "OK" and moved, "Teleport position: y %.1f -> %.1f (target %.1f)" % (
            state["pos"][1], after["pos"][1] if after else float("nan"), target[1]))
        if after and after["root_net"]:
            r = link.request(CMD_TELEPORT, p_teleport(rq(), REF_NET, after["root_net"], rot=(77.0, 0.0, 0.0)))
            link.pump(0.2)
            after2 = dict(link.last_player_stream["players"]).get(bid)
            c.check(r["name"] == "OK" and after2 is not None,
                    "Teleport rotation by networkable id -> Ok (yaw now %.1f; soldiers keep their own look yaw)" %
                    (after2["rot"][0] if after2 else float("nan")))
    else:
        c.check(False, "Teleport: the bot is not alive")
    r = link.request(CMD_TELEPORT, p_teleport(rq(), REF_PLAYER, bid, pos=(0, 0, 9000)))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Teleport outside the world -> Error 7")
    r = link.request(CMD_TELEPORT, p_teleport(rq(), REF_CONSOLE, 0xFFFFFFF0, pos=(0, 100, 0)))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "Teleport of an unknown console id -> Error 5")

    # SetHealth on an empty vehicle (fraction), read back from the vehicle stream.
    link.request(SUBSCRIBE, p_subscribe(rq(), 2, 10))
    link.pump(0.4)
    veh = None
    for v in (link.last_vehicle_stream or {}).get("vehicles", []):
        if v["health"] > 0 and not v["occupied"]:
            veh = v
            break
    if veh:
        r = link.request(CMD_SET_HEALTH, p_set_health(rq(), REF_NET, veh["net"], 0.5, True))
        link.pump(0.4)
        v2 = [v for v in link.last_vehicle_stream["vehicles"] if v["net"] == veh["net"]]
        c.check(r["name"] == "OK" and v2 and abs(v2[0]["health"] - 0.5) < 0.02,
                "SetHealth 0.5 on '%s' (net %d) -> health %.2f" % (veh["template"], veh["net"],
                                                                    v2[0]["health"] if v2 else -9))
        link.request(CMD_SET_HEALTH, p_set_health(rq(), REF_NET, veh["net"], 1.0, True))
    else:
        c.check(False, "SetHealth: no empty vehicle with armour in the vehicle stream")

    # SetScore -> ScoreChange.
    r = link.request(CMD_SET_SCORE, p_set_score(rq(), bid, 0x1F, [11, 22, 33, 44, 5]))
    ev = link.wait_event(lambda m: m["type"] == EV_SCORE_CHANGE and m["player"] == bid and m["score"]["score"] == 11, 3)
    c.check(r["name"] == "OK" and ev and ev["score"]["team_score"] == 22 and ev["score"]["kills"] == 33 and
            ev["score"]["deaths"] == 44 and ev["score"]["team_kills"] == 5, "SetScore -> ScoreChange %s" % (ev and ev["score"]))
    r = link.request(CMD_RANK, p_rank(rq(), bid, 4, True))
    ev = link.wait_event(lambda m: m["type"] == EV_SCORE_CHANGE and m["player"] == bid and m["score"]["rank"] == 4, 3)
    c.check(r["name"] == "OK" and ev is not None, "Rank 4 with the event -> Ok and ScoreChange rank 4")
    for name, t, payload in (("Medal", CMD_MEDAL, p_medal(rq(), bid, 2051907, 1)),
                             ("GameEvent 0", CMD_GAME_EVENT, p_event(rq(), bid, 0, 0)),
                             ("HudEvent 0", CMD_HUD_EVENT, p_event(rq(), bid, 0, 0))):
        r = link.request(t, payload)
        c.check(r["name"] == "OK", "%s -> %s" % (name, r["name"]))
    r = link.request(CMD_GAME_EVENT, p_event(rq(), 250, 1, 0))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNKNOWN_PLAYER, "GameEvent to an unknown player -> Error 4")

    # Kick and Ban refuse bots.
    r = link.request(CMD_KICK, p_kick(rq(), bid, "test"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "Kick a bot -> Error 6 (%s)" % r.get("message"))
    r = link.request(CMD_BAN, p_ban(rq(), bid, False, 0, 0, "test"))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "Ban a bot -> Error 6")

    # ListObjects, SetObjectActive, SpawnObject and DeleteObject.
    veh_template = veh["template"] if veh else "ahe_ah1z"
    r = link.request(CMD_LIST_OBJECTS, p_list_objects(rq(), veh_template, 50))
    objs = r.get("objects", [])
    c.check(r["name"] == "OBJECT_LIST" and len(objs) >= 1 and all(o["template"].lower() == veh_template.lower() for o in objs),
            "ListObjects '%s' -> %d objects (first console id %s, net id %s)" % (
                veh_template, len(objs), objs[0]["obj"] if objs else "-", objs[0]["net"] if objs else "-"))
    if veh:
        match = [o for o in objs if o["net"] == veh["net"]]
        c.check(bool(match) and match[0]["obj"] == veh["obj"],
                "ListObjects console id equals the vehicle stream's console id for net %d" % veh["net"])
        r = link.request(CMD_SET_OBJECT_ACTIVE, p_object_ref(rq(), REF_NET, veh["net"]))
        r2 = link.request(CMD_CONSOLE, p_console(rq(), "object.templateName"))
        c.check(r["name"] == "OK" and r2["name"] == "CONSOLE_RESULT" and r2["lines"] and
                r2["lines"][0].lower() == veh_template.lower(),
                "SetObjectActive by networkable id, then Console object.templateName -> %r" % (r2.get("lines"),))
    r = link.request(CMD_LIST_OBJECTS, p_list_objects(rq(), "no_such_template_xyz", 5))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "ListObjects of an unknown template -> Error 5")
    spawn_pos = (bot["state"]["pos"][0] + 30.0, bot["state"]["pos"][1] + 40.0, bot["state"]["pos"][2]) \
        if bot["state"]["life"] else (0.0, 300.0, 0.0)
    r = link.request(CMD_SPAWN_OBJECT, p_spawn(rq(), args.spawn_template, spawn_pos, (90.0, 0.0, 0.0), 0))
    if c.check(r["name"] == "OBJECT_SPAWNED", "SpawnObject '%s' -> %s console id %s net id %s" % (
            args.spawn_template, r["name"], r.get("obj"), r.get("net"))):
        spawned = r
        r = link.request(CMD_LIST_OBJECTS, p_list_objects(rq(), args.spawn_template, 4096))
        found = [o for o in r.get("objects", []) if o["obj"] == spawned["obj"]]
        c.check(bool(found) and abs(found[0]["pos"][1] - spawn_pos[1]) < 2.0 and abs(found[0]["rot"][0] - 90.0) < 1.0,
                "the spawned object is listed at y %.1f yaw %.1f" % (found[0]["pos"][1], found[0]["rot"][0]) if found
                else "the spawned object is listed")
        r = link.request(CMD_DELETE_OBJECT, p_object_ref(rq(), REF_CONSOLE, spawned["obj"]))
        link.pump(0.2)
        r2 = link.request(CMD_LIST_OBJECTS, p_list_objects(rq(), args.spawn_template, 4096))
        gone = not [o for o in r2.get("objects", []) if o["obj"] == spawned["obj"]]
        c.check(r["name"] == "OK" and gone, "DeleteObject by console id -> Ok and gone from ListObjects")
    r = link.request(CMD_SPAWN_OBJECT, p_spawn(rq(), "no_such_template_xyz", (0, 100, 0), (0, 0, 0)))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "SpawnObject of an unknown template -> Error 5")
    if bot["state"]["root_obj"] != 0xFFFFFFFF:
        r = link.request(CMD_DELETE_OBJECT, p_object_ref(rq(), REF_CONSOLE, bot["state"]["root_obj"]))
        c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "DeleteObject of a player's soldier/vehicle -> Error 6")

    # SetTeam with kill -> TeamChange (+ Bot team change) and Death; then back.
    old_team = bot["state"]["team"]
    new_team = 2 if old_team == 1 else 1
    r = link.request(CMD_SET_TEAM, p_set_team(rq(), bid, new_team, kill=True))
    tc = link.wait_event(lambda m: m["type"] == EV_TEAM_CHANGE and m["player"] == bid and m["new_team"] == new_team, 3)
    c.check(r["name"] == "OK" and tc is not None and tc["flags"] & 1,
            "SetTeam %d with kill -> Ok, TeamChange %s" % (new_team, tc and (tc["old_team"], tc["new_team"], tc["flags"])))
    be = link.wait_event(lambda m: m["type"] == EV_BOT and m["player"] == bid and m["kind"] == 3, 3)
    c.check(be is not None and be["team"] == new_team, "Bot event (team change) for the bot-channel bot")
    r = link.request(CMD_SET_TEAM, p_set_team(rq(), bid, old_team, kill=False))
    c.check(r["name"] == "OK", "SetTeam back to %d" % old_team)

    # KillPlayer: wait for the bot to be alive, then kill -> Death (and no Kill).
    alive = None
    for _ in range(40):
        link.pump(0.5)
        st = dict(link.last_player_stream["players"]).get(bid) if link.last_player_stream else None
        if st and st["life"] == 1:
            alive = st
            break
    if alive:
        r = link.request(CMD_KILL_PLAYER, p_kill(rq(), bid))
        death = link.wait_event(lambda m: m["type"] == EV_DEATH and m["player"] == bid, 3)
        c.check(r["name"] == "OK" and death is not None, "KillPlayer -> Ok and Death %s" % (death and describe(death)))
        r = link.request(CMD_KILL_PLAYER, p_kill(rq(), bid))
        c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "KillPlayer on a dead player -> Error 6 (%s %s)" % (
            r["name"], r.get("message", "")))
        sp = link.wait_event(lambda m: m["type"] == EV_SPAWN and m["player"] == bid, 25)
        c.check(sp is not None, "the killed bot respawns: Spawn %s" % (sp and describe(sp)))
    else:
        c.check(False, "KillPlayer: the bot did not become alive")

    # Invalid values never reach the engine; the link stays up.
    r = link.request(CMD_SET_TEAM, p_set_team(rq(), bid, 3))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "SetTeam 3 -> Error 7")
    r = link.request(SUBSCRIBE, p_subscribe(rq(), 9, 1))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Subscribe to stream 9 -> Error 7")
    r = link.request(CMD_MAP, p_map(rq(), 4, 0, "no_such_map", "gpm_cq", 0))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_UNAVAILABLE, "Map to a map not in the rotation -> Error 5")
    if args.map_restart:
        r = link.request(CMD_MAP, p_map(rq(), 1))
        gs = link.wait_event(lambda m: m["type"] == EV_GAME_STATE, 5)
        c.check(r["name"] == "OK" and gs is not None, "Map restart (action 1) -> Ok and GameState %s" % (gs and describe(gs)))
    link.pump(1.0)
    c.check(link.gaps == 0, "no sequence gaps (%d events)" % len(link.events))
    return c.summary("commands")


def scenario_resync(lst, args):
    """A gap inside a session (simulated loss) resolved by RequestSnapshot, then a dropped link."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    session = link.welcome["session"]
    rq = link.next_request
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    # Events to lose: chat lines.
    link.drop_next_event = 1
    for i in range(3):
        link.request(CMD_SAY, p_say(rq(), 0, 0, "resync %d" % i))
    link.pump(1.0)
    c.check(link.gaps == 1 and link.resyncs == 1, "a lost event is a gap; RequestSnapshot resynchronises (gaps %d, "
            "resyncs %d)" % (link.gaps, link.resyncs))
    snap_seq = link.snapshot["last_seq"]
    link.request(CMD_SAY, p_say(rq(), 0, 0, "after resync"))
    ev = link.wait_event(lambda m: m["type"] == EV_CHAT and m["text"] == "after resync", 3)
    c.check(ev is not None and ev["seq"] > snap_seq and link.gaps == 1,
            "events after the snapshot continue from lastSeq %d (next seq %s)" % (snap_seq, ev and ev["seq"]))
    # Drop the link mid-stream: the module reconnects with backoff, new session, fresh snapshot.
    t0 = time.time()
    link.sock.close()
    log("closed the link mid-stream; waiting for the module to dial again")
    link2 = lst.accept(args.accept_timeout)
    dt = time.time() - t0
    c.check(link2.welcome["session"] == session + 1 and link2.snapshot["last_seq"] == 0,
            "reconnected after %.1f s: session %d -> %d, Snapshot lastSeq 0" % (dt, session, link2.welcome["session"]))
    c.check(dt < 5.0, "reconnect delay after an authenticated link is about 1 s (%.1f s)" % dt)
    link2.request(CMD_SAY, p_say(link2.next_request(), 0, 0, "new session"))
    ev = link2.wait_event(lambda m: m["type"] == EV_CHAT and m["text"] == "new session", 3)
    c.check(ev is not None and ev["seq"] >= 1, "the new session numbers events from 1 (got %s)" % (ev and ev["seq"]))
    link2.sock.close()
    return c.summary("resync")


def scenario_stall(lst, args):
    """Stop reading while the module produces replies and events; the module must drop events (gap) or close the
    link at four times adminQueueBytes, never block the server."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    # Paced below the module's 64 commands per tick, so the command queue keeps up while the
    # replies pile up behind a reader that has stopped: first in the kernel's socket buffers, then in
    # the module's own queue.
    log("stalling for %.0f s: %d RequestSnapshot and %d Say per 10 ms, not reading" % (
        args.stall_seconds, args.stall_snapshots, args.stall_says))
    end = time.time() + args.stall_seconds
    n = 0
    try:
        while time.time() < end:
            for i in range(args.stall_snapshots):
                link.send(REQUEST_SNAPSHOT, W().u32(rq()).b)
            for i in range(args.stall_says):
                link.send(CMD_SAY, p_say(rq(), 0, 0, "stall %d" % n))
                n += 1
            time.sleep(0.01)
    except OSError as e:
        log("send failed while stalled: %s" % e)
    log("reading again")
    closed = None
    try:
        link.pump(5.0)
    except Closed as e:
        closed = str(e)
    if closed:
        log("gaps seen before the close: %d" % link.gaps)
        log("the module closed the link (%s); waiting for its reconnect" % closed)
        link2 = lst.accept(args.accept_timeout)
        c.check(link2.welcome["session"] > link.welcome["session"],
                "overloaded link closed and reconnected (session %d -> %d)" % (link.welcome["session"],
                                                                             link2.welcome["session"]))
        link2.sock.close()
    if not closed:
        c.check(link.gaps >= 1 and link.resyncs >= 1,
                "events dropped while the queue was full: %d gaps, %d resyncs" % (link.gaps, link.resyncs))
        link.sock.close()
    return c.summary("stall")


def scenario_soak(lst, args):
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    link.request(SUBSCRIBE, p_subscribe(rq(), 2, 5))
    link.request(SUBSCRIBE, p_subscribe(rq(), 4, 1))
    link.request(SUBSCRIBE, p_subscribe(rq(), 16, 1))
    link.request(SUBSCRIBE, p_subscribe(rq(), 3, 10, args.projectile_templates.split(",")))
    link.request(SUBSCRIBE, p_subscribe(rq(), 17, 1, args.projectile_templates.split(",")))
    t0 = time.time()
    next_ping = t0 + 25
    closed = None
    pings = 0
    try:
        while time.time() - t0 < args.seconds:
            link.pump(1.0)
            if time.time() >= next_ping:
                r = link.request(PING, p_ping(rq(), int(time.time() * 1000)))
                pings += r["name"] == "PONG"
                next_ping += 25
    except Closed as e:
        closed = str(e)
    dt = time.time() - t0
    report_link(link, dt)
    players = link.stream_frames.get(PLAYER_STREAM, 0)
    log("soak: %.0f s, player stream frames %d (%.1f Hz), vehicle %d, game %d, projectile %d; %d pings answered; "
        "closed: %s" % (dt, players, players / dt, link.stream_frames.get(VEHICLE_STREAM, 0),
                        link.stream_frames.get(GAME_STREAM, 0), link.stream_frames.get(PROJECTILE_STREAM, 0), pings,
                        closed))
    c = Checks()
    c.check(closed is None, "the link stayed up for %.0f s" % dt)
    c.check(link.gaps == 0, "no sequence gaps")
    c.check(players / dt > 25.0, "player stream at about 30 Hz (%.1f Hz)" % (players / dt))
    return c.summary("soak")


def scenario_wrongkey(lst, args):
    """The listener uses a different key for its proof: the module must refuse with Error 8 and back off."""
    c = Checks()
    attempts = []
    for i in range(3):
        try:
            lst.accept(args.accept_timeout)
            c.check(False, "the module accepted a wrong proof")
            return c.summary("wrongkey")
        except Closed as e:
            attempts.append(time.time())
            log("attempt %d refused: %s" % (i + 1, e))
    gaps = [b - a for a, b in zip(attempts, attempts[1:])]
    c.check(len(attempts) == 3, "three attempts refused with Error 8")
    c.check(len(gaps) == 2 and gaps[1] > gaps[0] * 1.3, "the backoff grows between attempts: %s s" %
            ", ".join("%.1f" % g for g in gaps))
    return c.summary("wrongkey")


def scenario_wrongpin(lst, args):
    """The listener's certificate is not the pinned one: the module aborts TLS before sending anything."""
    c = Checks()
    for i in range(2):
        err = lst.accept(args.accept_timeout, expect_tls_failure=True)
        c.check(isinstance(err, str), "TLS refused by the module (attempt %d): %s" % (i + 1, err))
    return c.summary("wrongpin")


def scenario_mapchange(lst, args):
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    maps = link.welcome["maps"]
    cur = link.welcome["map_index"]
    log("map list: %s (current %d)" % (maps, cur))
    r = link.request(CMD_MAP, p_map(rq(), 2))
    c.check(r["name"] == "OK", "Map next (action 2) -> Ok")
    states = []
    end = time.time() + args.seconds
    closed = None
    try:
        while time.time() < end:
            ev = link.wait_event(lambda m: m["type"] in (EV_GAME_STATE, EV_ROUND_START, EV_ROUND_END), 2)
            if ev:
                states.append(ev)
                if ev["type"] == EV_ROUND_START and len(states) > 1:
                    break
    except Closed as e:
        closed = str(e)
    log("game events: %s" % [describe(s) for s in states])
    c.check(closed is None, "the link survived the map change")
    c.check(any(s["type"] == EV_ROUND_END for s in states) or any(s["type"] == EV_GAME_STATE for s in states),
            "GameState/RoundEnd during the change")
    r = link.request(REQUEST_SNAPSHOT, W().u32(rq()).b, timeout=10)
    log("after the change: status %d, %d players, %d vehicles, %d control points" % (
        r["game"]["status"], len(r["players"]), len(r["vehicles"]), len(r["cps"])))
    c.check(r["name"] == "SNAPSHOT", "Snapshot after the map change")
    return c.summary("mapchange")


def forward(rot):
    """The unit forward axis of a Rot (yaw, pitch, roll degrees; positive pitch is nose down)."""
    y, p = math.radians(rot[0]), math.radians(rot[1])
    return (math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p))


def angle_deg(a, b):
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    if na < 1e-6 or nb < 1e-6:
        return float("nan")
    d = sum(x * y for x, y in zip(a, b)) / (na * nb)
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


def median(xs):
    xs = sorted(x for x in xs if x == x)
    return xs[len(xs) // 2] if xs else float("nan")


def scenario_viewer(lst, args):
    """The viewer additions against a firing crew (serverext_client.py --scenario viewer): view mode in
    PlayerState, AimStream, WeaponFire and Weapons. Each launch's direction (its velocity less the root's)
    is compared with the shooter's weapon and camera forward axes at that tick."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    aims = {}      # tick -> {player: aim}
    states = {}    # tick -> {player: state}
    alive_at = {}  # tick -> set of living players (player stream)
    weapons = []   # (time, event)

    def on_frame(m):
        if m["type"] == AIM_STREAM:
            aims[m["tick"]] = {a["player"]: a for a in m["aims"]}
        elif m["type"] == PLAYER_STREAM:
            states[m["tick"]] = {pid: st for pid, st in m["players"]}
            alive_at[m["tick"]] = {pid for pid, st in m["players"] if st["life"] != 0}
        elif m["type"] == EV_WEAPONS:
            weapons.append((time.time(), m))
    link.on_frame = on_frame

    players = [p["id"] for p in link.snapshot["players"]]
    for spec in ("players:30", "aim:30", "vehicles:5"):
        name, _, rate = spec.partition(":")
        r = link.request(SUBSCRIBE, p_subscribe(rq(), STREAMS[name], int(rate)))
        c.check(r["name"] == "OK", "Subscribe %s -> %s" % (spec, r["name"]))
    r = link.request(SUBSCRIBE, p_subscribe(rq(), 18, 0, args.fire_templates.split(",") if args.fire_templates else []))
    c.check(r["name"] == "OK", "Subscribe fire -> %s" % r["name"])
    r = link.request(SUBSCRIBE, p_subscribe(rq(), 19, 0))
    t_sub = time.time()
    link.pump(1.0)
    first = {m["player"] for t, m in weapons if t >= t_sub}
    c.check(r["name"] == "OK" and set(players) <= first,
            "Subscribe weapons -> %s; Weapons for every snapshot player %s: %s" % (r["name"], sorted(players), sorted(first)))
    r = link.request(SUBSCRIBE, p_subscribe(rq(), 5, 10, ["x"]))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Subscribe aim with a filter -> Error 7 (%s)" % r.get("message"))

    t0 = time.time()
    resynced = False
    while time.time() - t0 < args.seconds:
        link.pump(1.0)
        if not resynced and time.time() - t0 > args.seconds / 2:
            resynced = True
            t_snap = time.time()
            s = link.request(REQUEST_SNAPSHOT, W().u32(rq()).b, timeout=10)
            link.pump(0.5)
            after = {m["player"] for t, m in weapons if t >= t_snap}
            snap_players = {p["id"] for p in s["players"]}
            c.check(snap_players <= after, "after RequestSnapshot every player's Weapons again: %s of %s" % (
                sorted(after), sorted(snap_players)))
    seconds = time.time() - t0
    link.on_frame = None

    # Aim stream: rate and contents.
    n_aim = link.stream_frames.get(AIM_STREAM, 0)
    c.check(n_aim > 0, "AimStream frames: %d (%.1f per second over the whole link)" % (n_aim, n_aim / max(seconds, 1)))
    dead_entries = [(t, p) for t, a in aims.items() if t in alive_at for p in a if p not in alive_at[t]]
    c.check(not dead_entries, "AimStream has living players only (%d entries of dead players)" % len(dead_entries))
    flags = {}
    for a in aims.values():
        for p, x in a.items():
            flags.setdefault(p, set()).add(x["flags"])
    log("aim flags per player: %s" % {p: sorted(f) for p, f in flags.items()})

    # View modes seen in the player stream.
    views = {}
    for st in states.values():
        for pid, s in st.items():
            views.setdefault(pid, set()).add(s["view_mode"])
    log("view modes per player: %s" % {p: sorted(v) for p, v in views.items()})
    c.check(any(len(v - {255}) >= 3 for v in views.values()), "some player went through at least three view modes")

    # WeaponFire: per template, rounds, and the launch direction against the aim.
    fires = [m for m in link.events if m["type"] == EV_WEAPON_FIRE]
    by = {}
    for f in fires:
        k = (f["weapon"], f["projectile"])
        g = by.setdefault(k, dict(events=0, rounds=0, weapon=[], camera=[], speed=[]))
        g["events"] += 1
        g["rounds"] += f["rounds"]
        st = states.get(f["tick"]) or states.get(f["tick"] - 1) or {}
        root_vel = st.get(f["shooter"], {}).get("vel", (0.0, 0.0, 0.0)) if f["shooter"] >= 0 else (0.0, 0.0, 0.0)
        own = tuple(v - r for v, r in zip(f["velocity"], root_vel))
        g["speed"].append(math.sqrt(sum(x * x for x in own)))
        a = None
        for dt in (0, -1, 1, -2, 2):
            a = (aims.get(f["tick"] + dt) or {}).get(f["shooter"])
            if a:
                break
        if a and a["flags"] & AIM_WEAPON:
            g["weapon"].append(angle_deg(own, forward(a["weapon_rot"])))
        if a and a["flags"] & AIM_CAMERA:
            g["camera"].append(angle_deg(own, forward(a["camera_rot"])))
    c.check(bool(fires), "WeaponFire events: %d" % len(fires))
    for (wt, pt), g in sorted(by.items()):
        log("  %s / %s: %d events, %d rounds, own speed median %.0f m/s; launch vs weapon axis median %.1f max %.1f deg, "
            "vs camera axis median %.1f max %.1f deg (%d/%d with aim)" % (
                wt, pt, g["events"], g["rounds"], median(g["speed"]), median(g["weapon"]),
                max(g["weapon"]) if g["weapon"] else float("nan"), median(g["camera"]),
                max(g["camera"]) if g["camera"] else float("nan"), len(g["weapon"]), g["events"]))
    c.check(any("flare" in (wt + pt).lower() for wt, pt in by), "flares reported as WeaponFire")
    aimed = [min(median(g["weapon"]), median(g["camera"])) for g in by.values() if g["weapon"] or g["camera"]]
    c.check(bool(aimed), "launch directions compared with the aim for %d templates" % len(aimed))

    # Weapons: changes during the run.
    log("Weapons events: %d; distinct sets per player: %s" % (len(weapons), {
        p: sorted({tuple(s for _, s in m["slots"]) for _, m in weapons if m["player"] == p}) for p in
        {m["player"] for _, m in weapons}}))
    c.check(any(len({tuple(s for _, s in m["slots"]) for _, m in weapons if m["player"] == p}) > 1
                for p in {m["player"] for _, m in weapons}), "some player's weapons changed during the run")
    c.check(link.gaps == 0, "no sequence gaps (%d)" % link.gaps)
    report_link(link, seconds)
    return c.summary("viewer")


def scenario_replay(lst, args):
    """The hit/miss replay (stream 20 and the lag in PlayerState) against tools/testclient/tvscan.py: a scripted
    gunner shooting TV missiles at a target's passes. Forces the server's lag compensation with
    phy-latencyComp-debug (--lags, in blocks), then checks every ProjectileTrace against itself (each sample's
    tested pose is the engine's rewind of the trace's own current poses) and every ProjectileCollision against its
    trace (a rewound hull's tested pose is the trace's for that tick). --console-setup runs console lines first
    (test servers only: the scan uses ObjectTemplate.damage 1 so the target survives)."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    templates = args.projectile_templates.split(",")
    lag_ms = {}      # forced lag (s) -> [PlayerState lag_ms of every player]
    blocks = []      # (tick from which the lag holds, lag)
    current = [None]

    def on_frame(m):
        if m["type"] == PLAYER_STREAM and current[0] is not None and blocks and m["tick"] > blocks[-1][0] + 30:
            lag_ms.setdefault(current[0], []).extend(s["lag_ms"] for _, s in m["players"])
    link.on_frame = on_frame

    r = link.request(SUBSCRIBE, p_subscribe(rq(), 20, 0))
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Subscribe replay without a filter -> Error 7 (%s)" %
            r.get("message"))
    r = link.request(SUBSCRIBE, p_subscribe(rq(), 20, 0, templates))
    c.check(r["name"] == "OK", "Subscribe replay %s -> %s" % (templates, r["name"]))
    r = link.request(SUBSCRIBE, p_subscribe(rq(), STREAMS["players"], 5))
    c.check(r["name"] == "OK", "Subscribe players:5 -> %s" % r["name"])
    for line in filter(None, (x.strip() for x in args.console_setup.split("|"))):
        r = link.request(CMD_CONSOLE, p_console(rq(), line))
        log("  console %r -> %s" % (line, r["name"]))

    lags = [float(x) for x in args.lags.split(",")]
    t0 = time.time()
    for lag in lags:
        r = link.request(CMD_CONSOLE, p_console(rq(), "vars.set phy-latencyComp-debug %s" % (lag if lag > 0 else -1)))
        tick = link.last_player_stream["tick"] if link.last_player_stream else 0
        blocks.append((tick, lag))
        current[0] = lag
        log("  forced lag %.3f s from about tick %d" % (lag, tick))
        link.pump(args.seconds / len(lags))
    link.request(CMD_CONSOLE, p_console(rq(), "vars.set phy-latencyComp-debug -1"))
    link.pump(2.0)
    seconds = time.time() - t0
    link.on_frame = None

    # PlayerState lag.
    for lag, values in sorted(lag_ms.items()):
        want = int(lag * 1000 + 0.5) if lag > 0 else 0
        c.check(values and all(v == want for v in values), "PlayerState lag_ms with the lag forced to %.3f s: %s (want %d)"
                % (lag, sorted(set(values))[:8], want))

    traces = [m for m in link.events if m["type"] == EV_PROJECTILE_TRACE]
    cols = [m for m in link.events if m["type"] == EV_PROJECTILE_COLLISION]
    c.check(len(traces) > 10, "ProjectileTrace events: %d; ProjectileCollision events: %d" % (len(traces), len(cols)))

    def lag_at(tick):
        lag = None
        for t, l in blocks:
            if t <= tick:
                lag = l
        return lag

    # Each trace: lag per sample, and the tested pose against the trace's own current poses.
    worst, compared, lag_bad, targets = 0.0, 0, 0, 0
    for tr in traces:
        n = len(tr["samples"])
        first, last = tr["first_tick"], tr["first_tick"] + n - 1
        want = lag_at(first)
        if want is None or lag_at(last) != want or first < blocks[0][0] + 60:
            continue   # spans a switch, or the forced value may not have reached the server yet
        for smp in tr["samples"]:
            if abs(smp["lag"] - (want if want > 0 else 0.0)) > 1e-6:
                lag_bad += 1
        for ti in range(len(tr["targets"])):
            targets += 1
            for k, smp in enumerate(tr["samples"]):
                lag = smp["lag"]
                if lag <= 1.0 / 30.0:
                    if k + 1 >= n:
                        continue
                    expect = tr["samples"][k + 1]["targets"][ti]["pos"]   # where the coming tick leaves it
                else:
                    x = k + 1 - lag * 30.0           # the engine's rewind, in samples
                    i = int(math.floor(x))
                    if i < 0 or i + 1 >= n:
                        continue
                    f = x - i
                    a, b = tr["samples"][i]["targets"][ti]["pos"], tr["samples"][i + 1]["targets"][ti]["pos"]
                    expect = tuple(a[j] + (b[j] - a[j]) * f for j in range(3))
                got = smp["targets"][ti]["tested_pos"]
                worst = max(worst, math.sqrt(sum((got[j] - expect[j]) ** 2 for j in range(3))))
                compared += 1
    c.check(lag_bad == 0, "every trace sample carries the forced lag (%d wrong)" % lag_bad)
    c.check(compared > 100 and worst < 0.01,
            "tested pose = the engine's rewind of the trace's own poses: %d samples of %d targets, worst %.4f m" % (
                compared, targets, worst))

    # Collisions against their traces.
    by_id = {}
    for tr in traces:
        by_id.setdefault(tr["id"], []).append(tr)
    matched, hull_worst, parts, worst_by = 0, 0.0, {}, {}
    for col in cols:
        parts[col["part_template"] or "(terrain)"] = parts.get(col["part_template"] or "(terrain)", 0) + 1
        tr = next((t for t in by_id.get(col["id"], [])
                   if t["first_tick"] <= col["tick"] < t["first_tick"] + len(t["samples"])), None)
        if not tr or not (col["flags"] & COLLISION_ROOT) or col["kind"] != COLLISION_OBJECT:
            continue
        ti = next((i for i, (net, _) in enumerate(tr["targets"]) if net == col["root_net"]), None)
        if ti is None:
            continue
        smp = tr["samples"][col["tick"] - tr["first_tick"]]
        got, want = col["part_pos"], smp["targets"][ti]["tested_pos"]
        d = math.sqrt(sum((got[j] - want[j]) ** 2 for j in range(3)))
        hull_worst = max(hull_worst, d)
        key = "rewound" if col["flags"] & COLLISION_REWOUND else "not rewound"
        worst_by[key] = max(worst_by.get(key, 0.0), d)
        matched += 1
    log("parts touched: %s" % sorted(parts.items(), key=lambda x: -x[1]))
    log("hull collision vs trace, worst by kind: %s" % {k: round(v, 4) for k, v in worst_by.items()})
    c.check(matched > 0 and hull_worst < 0.01,
            "hull collisions match their trace's tested pose for that tick: %d, worst %.4f m" % (matched, hull_worst))
    rewound = [col for col in cols if col["flags"] & COLLISION_ROOT and col["kind"] == COLLISION_OBJECT]
    want_flag = [((col["flags"] & COLLISION_REWOUND) != 0) == ((lag_at(col["tick"]) or 0) > 1.0 / 30.0)
                 for col in rewound if lag_at(col["tick"]) is not None]
    c.check(want_flag and all(want_flag), "hull hits flagged rewound exactly when the lag is over one tick (%d of %d)" % (
        sum(want_flag), len(want_flag)))
    children = [col for col in cols if col["kind"] == COLLISION_OBJECT and not col["flags"] & (COLLISION_ROOT | COLLISION_SOLDIER)]
    c.check(all(not col["flags"] & COLLISION_REWOUND for col in children),
            "child parts are never flagged rewound (%d child hits)" % len(children))
    collided = [tr for tr in traces if tr["flags"] & TRACE_COLLIDED]
    c.check(all(any(col["id"] == tr["id"] and tr["first_tick"] <= col["tick"] < tr["first_tick"] + len(tr["samples"])
                    for col in cols) for tr in collided),
            "every trace that ended at a collision has its ProjectileCollision (%d)" % len(collided))
    c.check(link.gaps == 0, "no sequence gaps (%d)" % link.gaps)
    report_link(link, seconds)
    return c.summary("replay")


def scenario_extras(lst, args):
    """Kick notice through the console, KillPlayer destroying a seated bot's vehicle, Unsubscribe."""
    c = Checks()
    link = lst.accept(args.accept_timeout)
    rq = link.next_request
    link.request(SUBSCRIBE, p_subscribe(rq(), 1, 30))
    link.request(SUBSCRIBE, p_subscribe(rq(), 2, 10))
    seated = None
    for _ in range(60):
        link.pump(0.5)
        for pid, st in (link.last_player_stream or {}).get("players", []):
            if st["life"] == 1 and st["flags"] & 1 and st["seat"] == 0:
                seated = (pid, st)
        if seated:
            break
    bots = bots_in(link.snapshot)
    if bots:
        bid = bots[-1]["id"]
        r = link.request(CMD_CONSOLE, p_console(rq(), "admin.kickPlayer %d" % bid))
        ev = link.wait_event(lambda m: m["type"] == EV_KICK and m["player"] == bid, 3)
        c.check(r["name"] == "CONSOLE_RESULT" and ev is not None,
                "Console admin.kickPlayer on bot %d -> Kick event %s (a bot has no connection, so it stays)" % (bid, ev and describe(ev)))
    if c.check(seated is not None, "a bot seated as a pilot: %s" % (seated and seated[0])):
        pid, st = seated
        r = link.request(CMD_SET_HEALTH, p_set_health(rq(), REF_NET, st["root_net"], 0.0))
        c.check(r["name"] == "ERROR" and r["code"] == ERR_REFUSED, "SetHealth 0 on an occupied vehicle -> Error 6 (%s)" % r.get("message"))
        r = link.request(CMD_KILL_PLAYER, p_kill(rq(), pid, destroy_vehicle=True))
        death = link.wait_event(lambda m: m["type"] == EV_DEATH and m["player"] == pid, 3)
        link.pump(1.0)
        v = [x for x in link.last_vehicle_stream["vehicles"] if x["net"] == st["root_net"]]
        destroyed = [m for m in link.events if m["type"] == EV_VEHICLE_DESTROYED and m["net"] == st["root_net"]]
        c.check(r["name"] == "OK" and death is not None,
                "KillPlayer with destroy-vehicle -> Ok, Death %s" % (death and describe(death)))
        c.check(bool(v) and (v[0]["flags"] & 1 or v[0]["health"] <= 0.0),
                "the vehicle's hit points are 0: %s; VehicleDestroyed events %d" % (
                    v and (v[0]["health"], v[0]["flags"]), len(destroyed)))
    n = link.stream_frames.get(PLAYER_STREAM, 0)
    r = link.request(UNSUBSCRIBE, W().u32(rq()).u8(1).b)
    link.pump(0.2)
    n1 = link.stream_frames.get(PLAYER_STREAM, 0)
    link.pump(1.0)
    c.check(r["name"] == "OK" and link.stream_frames.get(PLAYER_STREAM, 0) == n1,
            "Unsubscribe players -> no more PlayerStream frames (%d before)" % n)
    r = link.request(UNSUBSCRIBE, W().u32(rq()).u8(9).b)
    c.check(r["name"] == "ERROR" and r["code"] == ERR_INVALID, "Unsubscribe stream 9 -> Error 7")
    link.sock.close()
    return c.summary("extras")


def run_fuzz(lst, args):
    """Hostile WebAdmin: malformed handshakes and malformed frames after authentication. The module must close
    each link with an Error (or silently), reconnect, and the server must keep running."""
    c = Checks()
    rng = random.Random(args.seed)
    rounds = 0
    errors = 0
    for i in range(args.fuzz_rounds):
        try:
            link = lst.accept(args.accept_timeout)
        except Closed as e:
            log("fuzz round %d: handshake ended: %s" % (i, e))
            continue
        rounds += 1
        kind = i % 6
        if kind == 0:
            data = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 300)))
        elif kind == 1:
            data = struct.pack("<IHH", 70000, CMD_CONSOLE, 0)       # oversize
        elif kind == 2:
            data = frame(CMD_CONSOLE, p_console(1, "x"), reserved=5)
        elif kind == 3:
            data = frame(0x7777, b"\0" * 8)                            # unknown type
        elif kind == 4:
            data = frame(CHALLENGE, b"\0" * 64)                       # second Challenge
        else:
            t = rng.choice([CMD_TELEPORT, CMD_SET_SCORE, CMD_BAN, CMD_MAP, SUBSCRIBE, CMD_SPAWN_OBJECT])
            data = frame(t, bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 40))))
        try:
            link.sock.sendall(data)
            end = time.time() + 3
            got_error = False
            while time.time() < end:
                f = link.recv_frame(0.5)
                if f and f[0] == ERROR:
                    e = decode(*f)
                    got_error = True
                    if e["request"] == 0:
                        log("fuzz round %d (kind %d): Error %d %s" % (i, kind, e["code"], e["message"]))
                        break
        except Closed:
            pass
        errors += got_error
        link.sock.close()
    c.check(rounds == args.fuzz_rounds, "the module dialled back after every fuzz round (%d/%d)" % (rounds, args.fuzz_rounds))
    c.check(errors >= rounds * 0.5, "most rounds ended with an Error frame (%d of %d)" % (errors, rounds))
    # A last clean link proves the server and the module are still fine.
    link = lst.accept(args.accept_timeout)
    r = link.request(PING, p_ping(link.next_request(), 7))
    c.check(r["name"] == "PONG", "a clean link after the fuzz: Ping -> Pong")
    link.sock.close()
    return c.summary("fuzz")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=16710)
    ap.add_argument("--key", default="", help="adminKey, hex")
    ap.add_argument("--challenge-key", default="", help="wrongkey: prove with this key instead")
    ap.add_argument("--server-id", default="", help="refuse a Hello with another server id")
    ap.add_argument("--no-tls", action="store_true", help="plain TCP (the module's adminTls 0, loopback only)")
    ap.add_argument("--cert-dir", default=os.path.expanduser("~/serverext-admin/cert"))
    ap.add_argument("--print-pin", action="store_true", help="create the certificate if needed, print its pin, exit")
    ap.add_argument("--write-golden", default="", help="write the golden fixtures to this file and exit")
    ap.add_argument("--scenario", default="listen",
                    choices=["listen", "commands", "resync", "stall", "soak", "wrongkey", "wrongpin", "mapchange", "extras",
                             "viewer", "replay", "vote", "r7", "script", "runscript", "scriptends", "seat",
                             "collision"])
    ap.add_argument("--fuzz", action="store_true")
    ap.add_argument("--fuzz-rounds", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--accept-timeout", type=float, default=120.0)
    ap.add_argument("--sub", action="append", default=[], help="listen: stream[:rate], e.g. players:30, damage")
    ap.add_argument("--projectile-templates", default="ahe_z10_S8Launcher_Projectile,AHE_AH1Z_HydraLauncher_Projectile,agm114_hellfire_tv")
    ap.add_argument("--fire-templates", default="", help="comma-separated weapon or projectile templates for --sub fire (none: every launch)")
    ap.add_argument("--spawn-template", default="ahe_ah1z", help="commands: SpawnObject/DeleteObject template")
    ap.add_argument("--map-restart", action="store_true", help="commands: also run Map action 1 (round restart)")
    ap.add_argument("--stall-snapshots", type=int, default=12, help="stall: RequestSnapshot per 10 ms")
    ap.add_argument("--stall-says", type=int, default=2, help="stall: Say per 10 ms")
    ap.add_argument("--stall-seconds", type=float, default=15.0)
    ap.add_argument("--rcvbuf", type=int, default=0, help="SO_RCVBUF for accepted links (stall tests)")
    ap.add_argument("--lags", default="0.25,0.15,0", help="replay: forced lag blocks, seconds (0 = off)")
    ap.add_argument("--console-setup", default="", help="replay: console lines run first, separated by |")
    ap.add_argument("--vote-text", default="Enable mod X?", help="vote: the question")
    ap.add_argument("--vote-seconds", type=int, default=30, help="vote: 5..600")
    ap.add_argument("--vote-team", type=int, default=0, help="vote: 0 everyone, 1 or 2")
    ap.add_argument("--vote-percent", type=int, default=50, help="vote: yes share of the ballots needed to pass")
    ap.add_argument("--vote-min", type=int, default=1, help="vote: ballots needed to pass")
    ap.add_argument("--vote-rows", type=int, default=0, help="vote: server messages to send from 3 s on (rows)")
    ap.add_argument("--vote-delay", type=float, default=0.0, help="vote, r7, script: seconds to wait after the link first")
    ap.add_argument("--script-file", default="", help="runscript: the JavaScript file to run")
    ap.add_argument("--script-args", default="", help="runscript: the script's arguments, JSON text")
    ap.add_argument("--script-name", default="", help="runscript: the script's name (default: the file's)")
    ap.add_argument("--seat-player", type=int, default=-1, help="seat: the player (default: the snapshot's first)")
    ap.add_argument("--seat", type=int, default=SEAT_SECOND, help="seat: 0 first seat, 1 second seat, 255 none")
    ap.add_argument("--seat-keep", action="store_true", help="seat: leave the preference set, hold the link --seconds")
    ap.add_argument("--seat-pair", action="store_true",
                    help="seat: with two bot-channel bots on one team, kill both in one tick and compare their respawns")
    ap.add_argument("--collision-wait", type=float, default=90.0,
                    help="collision: seconds to wait for a player seated in a vehicle")
    ap.add_argument("--collision-control", action="store_true", help="collision: leave collision on (comparison run)")
    ap.add_argument("--collision-off-seconds", type=float, default=0.0,
                    help="collision: turn collision on again after this many seconds (0: at the end)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.write_golden:
        return write_golden(args.write_golden)
    if args.print_pin:
        cert, _ = make_certificate(args.cert_dir)
        print(spki_pin(cert))
        return 0
    if not args.key:
        sys.exit("--key is required")
    lst = Listener(args)
    try:
        if args.fuzz:
            return run_fuzz(lst, args)
        return {"listen": scenario_listen, "commands": scenario_commands, "resync": scenario_resync,
                "stall": scenario_stall, "soak": scenario_soak, "wrongkey": scenario_wrongkey,
                "wrongpin": scenario_wrongpin, "mapchange": scenario_mapchange,
                "extras": scenario_extras, "viewer": scenario_viewer, "replay": scenario_replay,
                "vote": scenario_vote, "r7": scenario_r7, "script": scenario_script,
                "runscript": scenario_runscript, "scriptends": scenario_scriptends,
                "seat": scenario_seat, "collision": scenario_collision}[args.scenario](lst, args)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
