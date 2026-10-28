"""TV-missile hitbox scan: scripted bots for the hit/miss replay tests (docs/findings/tv-hitbox.md).

One stepping bot-channel connection (protocol v4; the server needs serverext.cfg stepMode 1 and allowTeleport 1)
flies a Z-10 crew hovering at G and an AH-1Z target that makes repeatable passes past C on dalian_2_v_2: a nose-on
sideways slide, a broadside forward pass, or head-on towards the gunner. For each pass the gunner fires one TV missile,
steered every tick at the target's predicted intercept moved back along its path by a random aim lag tau, so the shots
spread over the region behind the target. Setup runs in real time, the passes in stepped time. One JSON line per pass
goes to --out. Pair it with adminclient.py --scenario replay (or any admin-channel recorder) for the truth.

  python3 tvscan.py --port <bot port> --out shots.jsonl --minutes 2 [--plan slide:25,cross:20] [--trace]
"""
import argparse
import json
import math
import os
import random
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serverext_client as sc  # noqa: E402

STEP = 0x010C
_hello = sc.hello
sc.hello = lambda c, name="testclient", version=4: _hello(c, name, 4)

G = (190.0, 215.0, -140.0)      # gunner hover
C = (190.0, 200.0, 110.0)       # pass centre, 250 m north of G
ALT = 200.0
# (profile, setting): slide = bank angle (deg), cross/headon = commanded nose-down pitch (deg)
PLAN = [("slide", 12.0), ("slide", 25.0), ("cross", 10.0), ("cross", 20.0), ("headon", 12.0)]
START_Y = {("cross", 10.0): 240.0, ("cross", 20.0): 262.0}
START = {"slide": ((40.0, ALT, 110.0), 180.0), "cross": ((-60.0, ALT, 110.0), 90.0),
         "headon": ((190.0, ALT, 235.0), 180.0)}


class StepClient(sc.Client):
    """Grants one tick per TickState while self.stepping; set_stepping(False) returns the server to real time."""
    stepping = False

    def set_stepping(self, on):
        if self.stepping and not on:
            self.c.send(sc.frame(STEP, struct.pack("<I", 0)))
        self.stepping = on
        if on:
            self.c.send(sc.frame(STEP, struct.pack("<I", 1)))

    def pump(self, timeout=0.2):
        t = super().pump(timeout)
        if t == sc.TICK_STATE and self.stepping:
            self.c.send(sc.frame(STEP, struct.pack("<I", 1)))
        return t


def clamp(v, lo=-1.0, hi=1.0):
    return max(lo, min(hi, v))


class AP(sc.Autopilot):
    """Altitude and heading hold with a commanded pitch and/or roll angle (roll + = bank left), or the
    station-keeping speed loops of sc.Autopilot when none is given."""

    def step2(self, e, tick, alt, heading, pitch_cmd=None, roll_cmd=None, max_tilt=30.0):
        yaw, pitch, roll = e["ypr"]
        vx, vy, vz = e["vel"]
        if self.prev is None or tick <= self.prev[0]:
            dt, dp, dr, dyaw = 1 / 30.0, 0.0, 0.0, 0.0
        else:
            dt = (tick - self.prev[0]) / 30.0
            dp, dr = (pitch - self.prev[1]) / dt, (roll - self.prev[2]) / dt
            dyaw = sc.wrap180(yaw - self.prev[3]) / dt
        self.prev = (tick, pitch, roll, yaw)
        yr = math.radians(yaw)
        v_fwd = vx * math.sin(yr) + vz * math.cos(yr)
        v_right = vx * math.cos(yr) - vz * math.sin(yr)
        err = alt - e["pos"][1]
        self.ialt = clamp(self.ialt + err * 0.05 * dt, -0.6, 0.6)
        tilt = 1.0 / max(math.cos(math.radians(pitch)) * math.cos(math.radians(roll)), 0.5)
        coll = clamp(0.33 * tilt + 0.08 * err - 0.25 * vy + self.ialt)
        kv, kvi = self.vel
        akp, akd, aki = self.att
        if pitch_cmd is not None:
            pitch_des = pitch_cmd
        else:
            self.ivf = clamp(self.ivf + v_fwd * dt, -20.0, 20.0)
            pitch_des = clamp(-kv * v_fwd - kvi * self.ivf, -max_tilt, max_tilt)
        if roll_cmd is not None:
            roll_des = roll_cmd
        else:
            self.ivr = clamp(self.ivr + v_right * dt, -20.0, 20.0)
            roll_des = clamp(kv * v_right + kvi * self.ivr, -max_tilt, max_tilt)
        ep, er = pitch_des - pitch, roll_des - roll
        self.ip = clamp(self.ip + ep * aki * dt, -0.5, 0.5)
        self.ir = clamp(self.ir + er * aki * dt, -0.5, 0.5)
        cyc_p = clamp(akp * ep - akd * dp + self.ip)
        cyc_r = clamp(-(akp * er - akd * dr + self.ir))
        ped = clamp(0.03 * sc.wrap180(heading - yaw) - 0.03 * dyaw)
        return cyc_p, cyc_r, coll, ped


def sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def norm(a):
    return math.sqrt(sum(x * x for x in a))


def main():
    ap_ = argparse.ArgumentParser()
    ap_.add_argument("--port", type=int, default=16720)
    ap_.add_argument("--out", required=True)
    ap_.add_argument("--minutes", type=float, default=10.0)
    ap_.add_argument("--passes", type=int, default=100000)
    ap_.add_argument("--seed", type=int, default=7)
    ap_.add_argument("--plan", default="", help="profile:setting,... instead of PLAN")
    ap_.add_argument("--trace", action="store_true", help="log the target every 15 ticks")
    a = ap_.parse_args()
    random.seed(a.seed)
    plan = [(x.split(":")[0], float(x.split(":")[1])) for x in a.plan.split(",")] if a.plan else PLAN
    args = argparse.Namespace(host="127.0.0.1", port=a.port)
    cl = StepClient(args, "tvscan")
    out = open(a.out, "a")
    t_end = time.time() + a.minutes * 60

    crew = sc.board_crew(cl, 1, "[BOT] scan")
    if not crew:
        sys.exit("crew did not board")
    pilot, gunner, gheli = crew
    st = dict(phase="idle", profile=None)
    T = {"id": None, "ready": False}

    def new_target():
        T["ready"] = False
        stepping = cl.stepping
        cl.set_stepping(False)
        try:
            return _new_target()
        finally:
            cl.set_stepping(stepping)

    def _new_target():
        for attempt in range(6):
            if T["id"] is not None:
                cl.remove(T["id"])
                cl.run_for(1.0)
            T["id"] = cl.create_bot("[BOT] target", 2)
            ok = sc.ensure_boarded(cl, T["id"], 2, "target", spinup=0.3)
            e = cl.states.get(T["id"])
            sc.log(f"target after boarding: ok {ok} flags {e and e['flags']} seat {e and e['seat']} root {e and e['root']} "
                   f"pos {e and [round(x, 1) for x in e['pos']]} health {e and e['health']}")
            if ok and cl.in_vehicle(T["id"]):
                r = cl.teleport(T["id"], (C[0], 220.0, C[2]), 180.0)
                cl.run_for(0.2)
                e = cl.states.get(T["id"])
                sc.log(f"target teleport -> {r}; flags {e['flags']} pos {[round(x, 1) for x in e['pos']]} health {e['health']}")
                st["alt"] = 220.0
                T["ready"] = True
                sc.wait_ticks(cl, 60)
                if cl.in_vehicle(T["id"]):
                    return True
                T["ready"] = False
            sc.wait_ticks(cl, 150)
        return False
    if not new_target():
        sys.exit("target did not board")
    cl.pulses[gunner] = sc.BTN_ALT_FIRE
    cl.run_for(3.0, lambda: sc.has_tv(cl, gunner))
    sc.log(f"gunner slot 0: {sc.slot0(cl, gunner)}")

    gap, tap = sc.Autopilot(), AP()

    def ready():
        w = sc.slot0(cl, gunner)
        return (w is not None and w["name"].lower() in sc.TV_LAUNCHERS and w["ammo"] > 0 and not w["reloading"]
                and not sc.own_tv_missiles(cl, gunner))

    def aim(p, v, tau, m, sm):
        t = norm(sub(p, m)) / sm
        for _ in range(3):
            i = tuple(p[k] + v[k] * (t - tau) for k in range(3))
            t = norm(sub(i, m)) / sm
        return i

    def ctl():
        tick = cl.tick
        e = cl.states.get(pilot)
        if e and cl.in_vehicle(pilot):
            cp, cr, c, ped = gap.step(e, tick, alt=G[1], heading=0.0)
            cl.inputs[pilot] = dict(pitch=cp, roll=cr, coll=c, ped=ped)
        et = cl.states.get(T["id"])
        if not T["ready"]:
            return
        if not (et and cl.in_vehicle(T["id"])):
            if st["phase"] in ("runup", "flight"):
                st.update(phase="done", reason="target lost")
            return
        prof, setting = st["profile"] or ("hold", 0.0)
        hdg = st.get("heading", 180.0)
        if st["phase"] == "idle" or prof == "hold":
            m = dict(alt=st.get("alt", ALT), heading=hdg, max_tilt=8.0)
        elif prof == "slide":
            m = dict(alt=st["alt"], heading=hdg, roll_cmd=setting)       # facing south, bank left = slide east
        else:
            m = dict(alt=st["alt"], heading=hdg, pitch_cmd=setting)
        cp, cr, c, ped = tap.step2(et, tick, **m)
        cl.inputs[T["id"]] = dict(pitch=cp, roll=cr, coll=c, ped=ped)
        if st["phase"] not in ("runup", "flight"):
            cl.steers.pop(gunner, None)
            return
        p, v = et["pos"], et["vel"]
        el = tick - st["t0"]
        st["min_y"] = min(st.get("min_y", 1e9), p[1])
        if a.trace and el % 15 == 0:
            sc.log(f"  t {el / 30:5.1f} pos {p[0]:7.1f} {p[1]:6.1f} {p[2]:7.1f} vel {v[0]:+6.1f} {v[1]:+5.1f} {v[2]:+6.1f} "
                   f"ypr {et['ypr'][0]:6.1f} {et['ypr'][1]:+5.1f} {et['ypr'][2]:+5.1f} cmd {cp:+.2f} {cr:+.2f} {c:+.2f} {ped:+.2f}")
        if p[1] < 160.0 and st["phase"] == "runup":
            st.update(phase="done", reason="low")
            return
        mine = [q for q in sc.own_tv_missiles(cl, gunner) if st.get("launch") is None or q["launch"] == st["launch"]]
        eye = sc.eye(cl, gunner)
        if st["phase"] == "runup":
            if el > 30 * 30:
                st.update(phase="done", reason="timeout")
                return
            if el >= 60:
                i = aim(p, v, st["tau"], eye, 115.0)
                cl.steers[gunner] = (eye, i)
            if prof == "headon":
                t_c = (p[2] - C[2]) / max(-v[2], 1.0)
            else:
                t_c = (C[0] - p[0]) / max(v[0], 1.0)
            t_fly = norm(sub(C, G)) / 115.0 + 0.25
            if t_c < -2.0:
                st.update(phase="done", reason="passed")
            elif el >= 120 and t_c <= t_fly and ready():
                cl.pulses[gunner] = sc.BTN_FIRE
                st.update(phase="flight", fire_tick=tick, speed=norm(v), vel=list(v), pos=list(p), t_c=t_c)
            return
        # flight
        if st.get("launch") is None:
            new = [q for q in sc.own_tv_missiles(cl, gunner) if q["launch"] not in st["seen"]]
            if new:
                st.update(launch=new[0]["launch"], mid=new[0]["id"], first_tick=tick)
                mine = new[:1]
            elif tick - st["fire_tick"] > 30:
                st.update(phase="done", reason="no launch")
                return
        if mine:
            q = mine[0]
            sm = norm(q["vel"])
            if sm < 1.0 and st.get("launch") is not None and tick - st["first_tick"] > 3:
                st.update(phase="done", reason="stopped", end_tick=tick, end_pos=list(q["pos"]))
                return
            i = aim(p, v, st["tau"], q["pos"], max(sm, 60.0))
            if norm(sub(i, q["pos"])) > 0.5:
                cl.steers[gunner] = (q["pos"], i)
        elif st.get("launch") is not None:
            st.update(phase="done", reason="gone", end_tick=tick)
            return
        if tick - st["fire_tick"] > 160:
            st.update(phase="done", reason="ttl")

    sc.log("teleporting the gunner crew to G")
    cl.teleport(pilot, G, 0.0)
    cl.before_send = ctl
    sc.wait_ticks(cl, 60)
    cl.set_stepping(True)
    sc.log("stepping")

    n = 0
    while n < a.passes and time.time() < t_end:
        if not cl.in_vehicle(T["id"]):
            sc.log("target lost; new target bot")
            st.update(phase="idle", profile=None)
            if not new_target():
                sc.log("no target; stopping")
                break
        if not cl.in_vehicle(pilot) or not cl.in_vehicle(gunner):
            sc.log("gunner crew lost; stopping")
            break
        if not T["ready"]:
            continue
        if sc.dist3(cl.states[pilot]["pos"], G) > 15.0:
            cl.teleport(pilot, G, 0.0)
        prof, setting = plan[n % len(plan)]
        tau = random.uniform(-0.1, 0.5)
        start, hdg = START[prof]
        start = (start[0], START_Y.get((prof, setting), start[1]), start[2])
        st["alt"] = start[1]
        st.update(phase="idle", profile=None, heading=hdg)
        cl.teleport(T["id"], start, hdg)
        tap.reset()
        st.update(phase="runup", profile=(prof, setting), tau=tau, t0=cl.tick, launch=None, mid=None,
                  seen={q["launch"] for q in cl.projectiles}, reason=None, end_tick=None, end_pos=None,
                  fire_tick=None, first_tick=None, min_y=1e9)
        cl.run_for(120.0, lambda: st["phase"] == "done")
        rec = {k: st.get(k) for k in ("profile", "tau", "t0", "fire_tick", "first_tick", "end_tick", "launch",
                                      "mid", "reason", "speed", "vel", "pos", "t_c", "end_pos", "min_y")}
        rec["pass"] = n
        out.write(json.dumps(rec) + "\n")
        out.flush()
        if n % 10 == 0 or n < 15 or rec["reason"] not in ("stopped", "gone", "ttl"):
            sc.log(f"pass {n} {prof} {setting} tau {tau:.2f}: {rec['reason']} speed {rec['speed']} launch {rec['launch']} min y {rec['min_y']:.0f}")
        n += 1
        st["phase"] = "idle"
        cl.run_for(60.0, lambda: not sc.own_tv_missiles(cl, gunner))
    sc.log(f"{n} passes")
    cl.before_send = None
    cl.finish()


if __name__ == "__main__":
    main()
