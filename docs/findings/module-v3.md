# Module v3: TV launch, SetTargetDirection, buttons that survive a superseded SetInput

Status: done on 2026-10-23 by a worker thread; committed locally in this repo. The spec is
`docs/protocol.md` "v3 (approved 2026-10-23)", items 1-3. `docs/protocol.md` and
`docs/admin-channel-draft.md` were not edited. Paths without a repo prefix are OpenBF2 paths under
`Code/BF2/` in the `serverext-r1` worktree at `873d1b1`. Tweak paths are in the netsky `2v2-nocheck`
content, `mods/bf2/objects_server.zip`.

**No engine change was needed.** v3 is module and test-client work only; OpenBF2 `serverext/r1`
stays at `873d1b1`.

Summary:

- **Item 2 (TV launch): the fix belongs in the sidecar.** The TV launcher is not selectable. The
  gunner seat reaches it with **one tick of alt fire** (the seat's weapon toggle), and it fires on
  **fire** (`PIFire`). v2 pressed weapon select 2 (ignored) and then held alt fire (toggled once, and
  alt fire never fires in that seat), so nothing launched. It was not firing into the ground: nothing
  left the launcher. Unguided, a TV missile flies straight on along the launcher's axis and
  self-destructs about 430 m out.
- **Item 3 (`SetTargetDirection`):** `0x0202`, 29 bytes, a per-tick control stream like `SetInput`.
  Steered missiles land on the aim point (0.0 m on the sea, 0.0 m after a mid-flight retarget), follow
  a sweeping aim, and killed an own bot's heli of the other team.
- **Item 1 (button OR):** 240 of 240 one-tick camera presses survived one or two later `SetInput`s in
  the same tick; the module's stats line counted 360 superseded inputs, 300 of which kept a button.

## Item 2: why the gunner bot launched no TV missile

The source investigation was written before any code change; the runtime results confirm it, with
the corrections marked.

### How the TV launcher is reached

The AH-1Z gunner seat `AHE_AH1Z_COgunner` carries two weapons on `PIFire`: the 20 mm
`AHE_AH1Z_Gun` and the TV launcher `AHE_AH1Z_CoGunner_HellFireLauncherTV`. Neither names a fire
input, so both use the default `PIFire` and share fire slot 0.

| Tweak line (`Vehicles/Air/ahe_ah1z/ahe_ah1z.tweak`) | Meaning |
|---|---|
| `:238 ObjectTemplate.specialToggleWeaponInput PIAltFire` (seat) | alt fire is the seat's weapon toggle |
| `:697-700 projectileTemplate agm114_hellfire_tv`, `velocity 75`, `itemIndex 2`, `isSelectable 0` (TV launcher) | weapon select 2 cannot pick it |
| `:666-672 zoom.startCameraId 11`, `useProjectileCamera 1`, `allowUntoggledFire 0` | entering the weapon switches the seat to camera 11 |
| `:673-680 LPTargetComp`, `targetSystem TSTVGuided`, `targetObjectTemplate UniqueTarget`, `disableFireWhenActiveMissile 1`, `maxDistance 9999`, `setTargetAtMaxDistance 1` | TV guidance through a target object; one missile at a time |
| `:630-642 SingleFireComp roundsPerMinute 60`; ammo `magSize 1`, `nrOfMags 8`, `reloadTime 8`, `autoReload 1` | one missile per magazine, 8 s reload, 8 missiles |
| `:227-229` (launcher placed at `1.29/0.14/-3.25`, `setRotation 0/5/0`) | fixed to the airframe; no turret. From a hover it leaves about 4° above the horizon (measured, below) |

The Z-10 (`Vehicles/Air/ahe_z10/ahe_z10.tweak`) is the same: `:275 specialToggleWeaponInput PIAltFire`,
`ahe_z10_HJ8Launcher_TV` with `:656-668` (camera 11, `allowUntoggledFire 0`, `TSTVGuided`,
`disableFireWhenActiveMissile 1`) and `:688-692` (the same `agm114_hellfire_tv` projectile,
`itemIndex 2`, `isSelectable 0`). The Z-10 gun `AHE_Z10_Gun` is `itemIndex 1`, `isSelectable 0`
(`:466-467`). Only the AH-1Z was run.

The projectile (`Weapons/Armament/missiles/agm114_hellfire_tv/agm114_hellfire_tv.tweak`):
`SeekClosestTargetComp` with `seek.targetType TTUnique`, `trackingDelay 0.2`, `maxAngleLock 360`,
`maxDistLock 3000`; `DefaultFollowComp` `maxYaw/maxPitch 1.5`, `changeYaw/changePitch 0.2`,
`minDist 10`; `timeToLive 3.5`, `acceleration 300`, `maxSpeed 125`, `gravityModifier 0.1`,
`damage 3000`, explosion radius 5 m and damage 300. It has `setNetworkableInfo BasicInfo`, so it has a
networkable id and a seek component: the v2 projectile observer reports it as guided.

**Range.** With 3.5 s to live, 75 m/s at launch and 300 m/s² to 125 m/s, it covers about 430 m.
Measured: the unguided missile was last seen 427 m out with `ttl -0.50`. Aim points beyond about
400 m are not reached.

### The engine path, input to launch

1. **Selection.** `ServerGameLogic` runs `selectWeapon` unconditionally for every player with input,
   AI players included (`Game/GameLogic/ServerGameLogic.cpp:1428-1430`).
   - Weapon select 1-9 (`GameLogic.cpp:2713-2721`) go through `switchToWeaponFromIndex`
     (`:2821-2842`), which skips weapons that are not selectable (`:2681-2682`): the TV launcher is
     `isSelectable 0`. **That is why v2's weapon select 2 left the gun active.**
   - The seat's toggle: `checkSpecialToggle(input)` on the seat's PCO (`GameLogic.cpp:2775-2783`). It
     is true on a **rising edge** of the toggle slot above 0.5, when the slot's mapping bit is set
     (`Game/Objects/Game/PlayerControlObject.cpp:2123-2148`). A held key toggles once; the latch clears
     on the first tick it is up. `PIAltFire` (31) is in the module's mapping mask.
   - A toggle runs the "next weapon" cycle (`GameLogic.cpp:2845-2912`): in every fire slot with an
     active weapon, the next weapon by item index. Only the toggle may land on a weapon that is not
     selectable (`:2891-2892`, `weaponObjectIsSelectable(candidate) || specialToggle`), so the next
     weapon button (`PIToggleWeapon`, bit 22) cannot reach the TV launcher either. In the gunner seat
     the toggle flips slot 0 between the gun (item index 1) and the TV launcher (2).
2. **Firing.** The fire key goes to the active weapon with the current firing button
   (`ServerGameLogic.cpp:1380-1389`). In `GenericFireArm::internalHandleMessage`
   (`Game/Objects/Weapons/GenericFireArm.cpp:1030-1155`):
   - `:1069-1071`: a message is dropped while the firing button **is the seat's toggle key and the
     toggle latch is set**. So alt fire itself never fires anything in this seat.
   - `:1090-1122`: nothing fires while `busy()`: the deploy time after a switch
     (`m_deployDoneTick`, `:1872-1875`) or any busy component.
   - `:1137-1141`: the weapon fires on its fire input, `PIFire`, after `requestFire`.
3. **`requestFire`** (`GenericFireArm.cpp:1209-1248`) votes Ammo, Fire, Zoom, Target, Animation:
   - Zoom, `ToggleCameraComp::requestFire` (`Weapons/ZoomComponents/ToggleCameraComp.cpp:199-204`,
     `fireAllowed` `:225-246`): allowed when the seat's active camera is the weapon's change camera
     **or the occupant `getIsAIPlayer()`**. The module's bots are AI-flagged (R1), so
     `allowUntoggledFire 0` does not stop them. Refused while its last missile is in the projectile
     camera.
   - Target, `LPTargetComp::requestFire` (`Weapons/TargetComponents/LPTargetComp.cpp:322-336`): with
     `disableFireWhenActiveMissile`, refused while the previous missile is alive. No lock is needed:
     the base vote (`TargetComp.cpp:181-185`) is true without an on-lock weapon.
   - Ammo: one missile per magazine, then the 8 s reload.
   - No minimum range and no firing arc: the launcher is fixed to the airframe.
4. **Launch.** `FireComp` creates the projectile, sets its team, activates it and gives it the
   launch velocity, then the v2 projectile observer runs (`Weapons/FireComponents/FireComp.cpp:1040-1128`).
   `LPTargetComp::postFire` hands the missile the weapon's target object and calls
   `updateTargetObject(event.start, event.end)` (`LPTargetComp.cpp:302-310`).

So **the v2 sequence could not launch**: weapon select 2 selects nothing, fire held 1-3 s fired the
gun, and alt fire held 3.5-5 s toggled slot 0 to the TV launcher on its first tick and was then
dropped as the toggle key (`GenericFireArm.cpp:1069-1071`). The TV launcher's fire input is `PIFire`,
which was no longer pressed.

**The fix is in the sidecar's button sequence, not the module:** press **alt fire for one tick**,
wait until `TickState`'s slot-0 weapon is the TV launcher (a TickState or two; the toggle also starts
the deploy), then press **fire** for one tick. Holding alt fire is harmless but toggles only once; a
second one-tick press toggles back to the gun. v2's `weapons --tv` test-client scenario now uses this
sequence.

### Where a TV missile goes without a direction (runtime correction)

The source suggested the missile would steer towards the target object's last position, initially
the world origin. **At runtime it does not.** It flies straight on:

- The guidance target is the weapon's own `UniqueTarget` object, created at 0/0/0 when the weapon is
  initialised (`LPTargetComp.cpp:148-185`) and inactive until a direction activates it
  (`setTargetObject(target, true, ...)` → `enableTargetObject`, `LPTargetComp.cpp:338-346`, `:219-227`).
- The seek component only takes an **active** unique target that is the launcher's current one
  (`GameLogic.cpp:1544-1563`, `SeekComponents/SeekClosestTargetComp.cpp:208-280`).
- Every reload deactivates it (`LPTargetComp::reload` → `setTargetObject(target, false, 0)` →
  `disableTargetObject`, `LPTargetComp.cpp:263-269`), so after each launch the target is off again
  until the next direction.
- `postFire`'s `updateTargetObject(event.start, event.end)` does nothing: `FireComp`'s
  `getStartPos`/`getEndPos` are both zero (`Weapons/FireComponents/FireComp.cpp:369-370`, used at
  `GenericFireArm.cpp:1343-1344`) and `updateTargetObject` returns at once when start equals end
  (`LPTargetComp.cpp:378-385`).
- On the server nothing else moves a TV target: `LPTargetComp::updateComponent` refreshes it from the
  player's camera only for **wire**-guided weapons (`LPTargetComp.cpp:271-298`). A human's client
  sends `TargetDirectionEvent` (`Menu/Bf2ObjectSelectionNode.cpp:100-135` → `LPTargetComp.cpp:348-361`
  → `GameEvents/TargetDirectionEvent.cpp:95-130`).

Measured (`tv-2`, a fresh server process, no direction ever sent; `tv-3` repeated it): from a hover
208 m up, heading 130°, the missile left at `(65.2, 5.3, -54.8)` m/s (about 4° above the horizon),
flew 427 m along 130°, rose 14 m and was last seen with `ttl -0.50`. It neither dived nor turned
towards the origin (bearing -45°).

A direction sent **only before the launch** holds: steered for 6 ticks during the previous shot's
reload, then 30 ticks of nothing, then fire: the missile landed 2.7 m from the aim point (`tv-3`
"pre-launch only"). The sidecar can therefore set the aim, then fire, and keep the stream going only
to move it.

### The target point a direction produces

`LPTargetComp::setTargetDirection(start, end)` on the server goes straight to
`updateTargetObject(start, end)` (`LPTargetComp.cpp:348-361`; the event branch is client-only).
`updateTargetObject` (`:374-497`):

- casts a ray from `start` through `end` out to the weapon's `maxDistance` (9999 m), first against
  the terrain heightmap, then against objects, ignoring the weapon's own vehicle and the missile;
- **water counts** (correction: the first draft said it did not). The ray is two-sided
  (`terrainIntersection` passes `true`, `LPTargetComp.cpp:99-106`). `HeightmapCluster` tests a
  two-sided ray against the water level (the vertical case is explicit at
  `Physics/HeightmapCluster.cpp:555-570`; the general case hands `twoSided` to each heightmap,
  `:635-`), and at runtime every sea target sat on the surface. `dalian_2_v_2`'s sea level
  is 141.4 (`levels/dalian_2_v_2/server.zip` `Heightdata.con`, `heightmapcluster.setSeaWaterLevel
  141.4`); the module logged every sea target at y 141.4;
- an object hit on **the weapon's own team** is ignored, so the ray goes on to the terrain behind
  it (`:460-467`). Steering at a teammate's heli marks the ground behind it;
- the target object goes to the object hit, else the terrain or water hit, else
  (`setTargetAtMaxDistance 1`) `start + dir × (the missile's max target distance − the vehicle's
  bounding radius)`;
- the target is activated (`setTargetObject(target, true, ...)` → `enableTargetObject`).

## Byte layouts (protocol version 3)

Written before the code; implemented unchanged. Framing, strings, error codes and every v2 message
are unchanged unless listed. The module accepts only version 3; a v2 `Hello` is `Error` 1 and close.

### `0x0002 Welcome`

Unchanged from v2 except the version field, 3.

### `0x0201 SetInput` (item 1)

Layout unchanged (33 bytes). Semantics:

- When several `SetInput`s for one bot arrive before a tick applies them, **the axes are the last
  one's and the buttons are the OR of all of them**. A one-tick press is never lost to a later
  `SetInput` in the same tick. Buttons are still pressed only for the one tick in which they are
  applied.
- A release cannot cancel a press in the same tick: `press` then `release` in one tick presses for
  that tick.
- The per-minute stats line counts superseded `SetInput`s and how many of them kept a button that
  the later one did not press.

### `0x0202 SetTargetDirection` (sidecar → module, item 3)

| Type | Field |
|---|---|
| `u8` | player id (a bot of this connection) |
| `u32` | the tick number this direction was computed from, as `SetInput`'s (for logs) |
| 3 × `f32` | start: the eye point, metres (a human client sends its camera position) |
| 3 × `f32` | end: a point on the line of sight beyond start |

29 bytes. It is a control stream like `SetInput`, not a request: no reply on success.

- **Latest wins.** The module applies at most one per bot per tick, the last that arrived, in the
  tick handler. The superseded ones are counted.
- **Refusals** are `Error` with request id 0 (as `SetInput`), at most one per bot per tick:
  - 4: not a bot of this connection;
  - 7: a non-finite value, a point outside the world (|x| or |z| > 8192, y outside -1000..10000,
    as `Teleport`), or start and end closer than 0.1 m;
  - 6: the bot is not alive or is man-down, or none of its active weapons (`getActiveWeapon(0..2)`)
    is TV-guided (`IWeaponObject::getTargetSystemType() == TSTVGuided`, 4), or that weapon has no
    target object.
- **What it does:** `IWeaponObject::setTargetDirection(start, end)` on that weapon, the virtual the
  client's `TargetDirectionEvent::executeServer` calls (`GameEvents/TargetDirectionEvent.cpp:95-130`,
  `IWeaponObject.h:140`). See "The target point a direction produces" for where the target lands.
  No engine change.
- It works with no missile in flight (it places the target the next launch flies to) and with one
  (the missile turns to the new target, within its follow rates).
- **Wire-guided weapons are refused**: on the server their target follows the player's camera every
  tick (`LPTargetComp.cpp:271-298`), which would overwrite a direction at once. Neither 2v2 heli has
  one.
- The module logs the first 20 applied directions and every 300th, with the target object's
  position after the call.

## Design notes

Files changed in this repo: `src/Protocol.h/.cpp` (version 3, the message, its parser and value
check), `src/PendingInput.h` (new: the merge rule), `src/Channel.h`, `src/Session.cpp`,
`src/Module.cpp`, `tests/TestMain.cpp`, `tools/testclient/serverext_client.py`, `README.md`.

- **Button OR.** Each bot keeps a `PendingInput`: axes latest wins, buttons ORed since the last
  apply, cleared when the tick applies it or the connection closes. It is engine-free and
  unit-tested. The per-minute stats line, after the cost line:
  `inputs: SetInput superseded N this minute (K kept a button), N run (K); SetTargetDirection applied
  ...`.
- **`SetTargetDirection`.** The I/O thread parses it (exact length, else `Error` 2 and close). On the
  main thread `processCommands` checks the bot and the values (`Error` 4 or 7 at once) and stores the
  latest per bot. The tick handler applies it after the bot's input: it finds the TV weapon through
  `getActiveWeapon(0..2)` and the typed `IWeaponObject::getTargetSystemType`, checks
  `getTargetObject`, then calls `setTargetDirection`. Those are all typed virtuals in
  `IWeaponObject.h`; there is no raw offset.
- **Order within a tick.** A direction is applied in the module's tick handler, before
  `simulatePlayersUpdate` runs the same tick's buttons. So a `SetTargetDirection` in the same tick as
  the alt-fire toggle is refused (the gun is still active); send it from the next `TickState` that
  shows the TV launcher.
- **Cost.** One `setTargetDirection` is a heightmap ray plus one object ray. Applying 30 per second
  for one gunner did not move the handler's p50 or p99 (see the soak).
- **Logs.** Launches (`projectile ... 'agm114_hellfire_tv' launched (net id ..., guided 1)`), the
  first 20 and every 300th applied direction with the resulting target position, and the stats line.

Unit tests: `serverext_tests` 621,334 checks, 0 failures (v2: 621,268); `serverext_admin_tests` 12,823 checks, 0 failures. New: the
`SetTargetDirection` parser (exact 29 bytes, one short, one and two long, empty, all zeros), the
value check (NaN and ±infinity in each of the six coordinates, the world bounds on both points, the
0.1 m minimum), the session (it becomes a command; before `Hello` it is `Error` 2; a malformed one is
`Error` 2), the fuzz sessions (now including it), `Hello` 2 and 4 refused, the `Welcome` version, and
`PendingInput` (press then no buttons, a release cannot cancel, latest axes, no carry-over after an
apply, all 32 bits in one burst). The module builds without warnings.

## Results

### Setup

| Item | Value |
|---|---|
| OpenBF2 | worktree `serverext-r1`, branch `serverext/r1` at `873d1b1` (no change), built into `~/bf2-linux-serverext-v3` (`testenv/build-linux.sh`); `bf2` md5 `5c904e400db57415c38b09149da5d1bf` |
| Module | `~/serverext-build/v3` (`build-module.sh`); `libserverext.so` md5 `da4f53cee62882b47fd6c8edec3de74e` |
| Content | netsky `2v2-nocheck`, fresh run dir `~/serverext-v3/srv` (`prepare-content.sh`) |
| Server | WSL, `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`, `dalian_2_v_2` (`gpm_cq` 16). Game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723, bot channel 127.0.0.1:16701, admin dial-out 127.0.0.1:16710 (last four processes); `allowTeleport 1`, `allowRoundRestart 1`. The first two processes ran with `BF2_TRACE_TV_MISSILES=1` (the tree's opt-in TV trace). Ports 16567, 16569, 16573, 16700, 16703, 16711, 29901, 29905, 4300, 4721 and 4725 were not used; no container was created or touched |
| Clients | `tools/testclient/serverext_client.py` and `tools/adminclient/adminclient.py` (own certificate and key in `~/serverext-v3/cert`, `admin.key`) in WSL, Python 3.10.6; logs `~/serverext-v3/*.log`, server console logs `~/serverext-v3/srv/bf2-*.log` |

Six server processes ran, each stopped with SIGTERM: `run1` (`tv-1`), `run2` (`tv-2`, `tv-3`, `burst`,
`fuzz`), `pre-soak` (admin `commands`), `soak-aborted`, `soak-a` and `soak-b` (the soaks below). The
server is left stopped.

### Item 2 and item 3 (`--scenario tv --team 2`, AH-1Z, pilot and gunner bots)

| Check | Result | Run |
|---|---|---|
| A direction with the gun active | `Error` 6 `SetTargetDirection: the bot has no active TV-guided weapon` | `tv-3` |
| v2's sequence: weapon select 2 | slot 0 stays `AHE_AH1Z_Gun` | `tv-3` |
| v2's sequence: alt fire held 1.5 s | slot 0 toggled once to `AHE_AH1Z_CoGunner_HellFireLauncherTV`, **0 missiles** | `tv-3` |
| One tick of alt fire, twice | back to the gun, then the TV launcher again | `tv-3` |
| Next weapon (bit 22) | stays on the TV launcher | `tv-3` |
| Unguided launch: one tick of fire | launched 3 ticks after the press; `ProjectileList` guided 1, own 1; straight on 427 m along the heli's heading, +14 m, `ttl -0.50`; the launcher then reloading, 7 mags | `tv-2`, `tv-3` |
| Direction only before the launch | 2.7 m from the aim point on the sea | `tv-3` |
| Steered onto the sea, 250 m ahead, 20° right | **0.0 m** from the aim point (last seen at y 141.4 after 2.60 s), no refusal | `tv-3` |
| Retargeted 0.5 s after launch, 10° left → 10° right | **0.0 m** from the new point, 86.8 m from the first | `tv-3` |
| Sweep, 25° left → 25° right over 2.5 s | the missile's bearing from the heli went -6° → +19° with the aim; ended 38 m from the sweep's last point (it reached the sea before the sweep ended) | `tv-3` |
| Steered onto an own bot of the other team (a Z-10 teleported 245 m ahead, hovering) | `Damage` flags 0x3f, victim 253, attacker 254, `AHE_AH1Z_CoGunner_HellFireLauncherTV` / `agm114_hellfire_tv`, 6000 hit points, 875 → -100; `Kill` 253 by 254 | `tv-2` |
| A direction after toggling back to the gun | `Error` 6 | `tv-3` |

`tv-1` 23/25, `tv-2` 25/27 and `tv-3` 23/23 checks. The failures in `tv-1` (aim points below the sea
surface) and `tv-2` (a 60° retarget 0.8 s after launch, and a sweep criterion) were the test's own
geometry, fixed for `tv-3` (contract points 2 and 3 below).

Module log (`bf2-run2.log`: `tv-2` unguided, then `tv-2` sea and own bot):

```
tick 1157: projectile 1852 'agm114_hellfire_tv' launched (net id 1852, guided 1, 1 so far)
tick 1158: bot 254 camera view mode 1 -> 3
tick 1278: bot 254 camera view mode 3 -> 1
tick 1739: bot 254 steer #7 (from tick 1737) start 263.0 209.5 -260.7 end 386.0 141.4 -478.1 -> target 386.0 141.4 -478.1
tick 1745: projectile 1852 'agm114_hellfire_tv' launched (net id 1852, guided 1, 3 so far)
tick 1746: bot 254 steer #14 (from tick 1744) start 263.1 209.6 -260.8 end 386.0 141.4 -478.1 -> target 386.0 141.4 -478.1 (missile in flight)
tick 2921: projectile 1852 'agm114_hellfire_tv' launched (net id 1852, guided 1, 6 so far)
tick 2975: kill: player 253 (team 1) by 254 with 'AHE_AH1Z_CoGunner_HellFireLauncherTV'
```

The first process (`tv-1`, `bf2-run1.log`) aimed below the sea surface:
`steer #17 ... end 385.3 -10.0 -477.5 -> target 301.3 141.4 -329.0`: the ray stopped on the water
90 m out (contract point 2).

**The target direction follows the message.** The TV trace (`steer_dispatch`, one line per applied
direction while a missile is in flight) shows the end point the module passed on every tick. In the
`tv-3` retarget the client switched aim points after tick 5708, and the trace switches on the next
applied tick (`tv-trace-run2.log`):

```
tick=5709 ... event=steer_dispatch start=263.126831,210.059509,-260.839111 end=478.527649,141.399994,-387.680145
tick=5710 ... event=steer_dispatch start=263.136169,210.058456,-260.845062 end=422.161163,141.399994,-453.719849
```

The missile then landed on the new point (0.0 m). In the sweep the end point moved every tick and the
missile's bearing followed it.

### Item 1 (`--scenario burst`)

One bot in a Z-10 pilot seat. Each press is a camera mode key (2 or 3, alternating, so every press
must change `TickState`'s view mode) sent in one TCP write with one or two more `SetInput`s for the
bot, all numbered with the same tick. 60 presses per pattern.

| Pattern | Result |
|---|---|
| press, then a `SetInput` without buttons | 60/60 changed the view, 3 ticks after the `TickState` they answered |
| press, then two without buttons | 60/60 |
| no buttons, the press, then no buttons | 60/60 |
| press, then a `SetInput` with another button (flares) | 60/60 (the flares were launched as well) |

Module stats for that minute: `SetInput superseded 332 this minute (272 kept a button), 360 run
(300)`. 360 is exactly the number of extra `SetInput`s sent (60 + 120 + 120 + 60), so every one
landed in the same tick as the one before it, and 300 is exactly the number whose predecessor held a
press they did not repeat. Under v2's latest wins every one of the 240 presses would have been lost.

### Fuzz (`--fuzz`)

**74/74**, the server still at 30 ticks/s afterwards. New: `Hello` 2 and 4 → `Error` 1;
`SetTargetDirection` before `Hello`, 28, 30 and 0 bytes → `Error` 2 and close; a foreign player →
`Error` 4; NaN start, +inf end, end x 9000, start y -2000, start = end → `Error` 7; on foot (no TV
weapon) → `Error` 6; 3,000 directions in one write → accepted, latest wins.

### Admin channel

With `adminConnect 127.0.0.1:16710` (TLS 1.3, pinned) on the same module: the reference client's
`commands` scenario passed **41/41** with a hovering bot (`admin-commands.log`), and its `listen`
scenario ran through the whole soak (below).

### 10-minute soak (`--scenario soak3`)

`--scenario soak3 --team 2 --soak-seconds 600` (`soak3.log`, server `bf2-soak-b.log`): one connection,
a pilot and a gunner bot in one AH-1Z hovering 60 m over the team 2 pad, nose seaward. Every 75 s
the gunner fires a TV missile and steers it onto the sea 250 m out, 10° to alternating sides; every
other pair of shots is retargeted 0.5 s after launch to the opposite side. Every 45 s the pilot gets
four camera presses, each followed in the same write by a `SetInput` without buttons. The admin
reference client listened for the whole run (`listen`, players at 5 Hz, projectiles at 30 Hz,
damage events).

| Check | Result |
|---|---|
| TickStates | 18,000 in 600 s, 0 gaps |
| TV shots | **8/8 launched, all 8 ended 0.0 m from their aim line** (4 of them retargeted in flight); 2,213 directions applied, 0 superseded, 0 refused |
| Camera presses in superseded `SetInput`s | 52/52 changed the view; the module counted 52 superseded, 52 kept a button |
| Crew | stayed in the heli, 0 losses; 0 `Error`s without a request |
| Server process | alive, 4 UDP sockets bound, RSS 148,228 → 148,232 KB over 13 checks |
| Log lines | 0 `[FRONTIER]`, 0 abort, 0 segfault in each of the six server logs of the run dir |
| Handler cost, soak windows (1,800 ticks each) | p50 110-120 µs, p99 180-250 µs, max 251-709 µs |
| Handler cost, whole process (19,800 ticks) | p50 120 µs, p99 220 µs, max 2,904 µs (the tick that created a bot), 0 ticks over 20 ms |
| Admin channel over the soak | 680 s, 261 events, **0 gaps, 0 resyncs**; 3,401 player and 20,400 projectile stream frames |

v2 measured p50 100-110 µs and p99 160-180 µs in its soak; v3's windows are 10 µs higher at p50
with the admin channel streaming players and projectiles at the same time. The 30 directions a second
while a missile flies cost little: windows with directions had p99 190-250 µs, the two without
180-190 µs, and p50 was 110-120 µs in both.

Two earlier soak runs: the first (`soak-aborted`) was stopped after 3 minutes because its 50°
retargets 0.8 s after launch could not be met (contract point 3); the second (`soak-a`, shots every
30 s) ran the full 600 s with 18,001 TickStates, 0 gaps, 52/52 presses and 8/8 shots on target,
and then found the launcher empty for the remaining 12 shots (contract point 11). After the last
soak the client removed its bots mid-air, so the empty heli fell and burned
(`EV_VEHICLE_DESTROYED ... attacker -1` in the admin log); that is after the measured window.

## Contract points that proved wrong or ambiguous

`docs/protocol.md` was not edited.

1. **v3 item 2's premise.** The v2 gunner bot did not fire into the ground; it never launched. The
   cause was the sidecar's button sequence: the TV launcher is reached with the seat's toggle
   (**alt fire, one tick**) and fired with **fire**. The module needs no change for the launch. The
   bf2-bots sidecar should: press alt fire for one tick when it wants the TV launcher, wait for
   `TickState` to show it in slot 0, send `SetTargetDirection` for the aim, then press fire. Weapon
   select and next weapon cannot reach it.
2. **Water stops the aim ray.** On `dalian_2_v_2` the sea is at y 141.4, and a direction whose ray
   crosses the surface puts the target on the surface, not on the sea bed. A sidecar aiming at "the
   sea" should aim at the surface; the first test run aimed below it and the missile hit the water
   90 m out, short of the intended point.
3. **The missile's turn rate is the limit.** It turns at most about 1.5 rad/s (`follow.maxYaw/maxPitch`).
   A 20° retarget 0.5 s after launch landed exactly; a 60° retarget 0.8 s after launch (with a steep
   dive) did not: the missile reached the sea 82 m off the first point and 178 m short of the second.
   Steep dives from close in also overshoot. Aim early (the direction holds from before the launch).
4. **Range is about 430 m** (3.5 s to live). Aim points beyond about 400 m are not reached; the
   missile self-destructs.
5. **Unguided means straight on**, along the airframe about 4° above the horizon, not towards any
   stale point: every reload deactivates the target, so a launch with no direction since the last
   one flies straight.
6. **Own-team objects are transparent** to the aim ray (`LPTargetComp.cpp:460-467`): a direction at a
   teammate marks the ground behind it. Enemy vehicles stop it, and the missile then homes on them.
7. **A refused direction costs an `Error` per tick.** A sidecar that streams directions while the gun
   is active gets `Error` 6 each tick; it should stream only while `TickState` shows the TV launcher.
8. **The gunner's view mode changes at launch**: `TickState` showed 1 → 3 one tick after each launch
   and back to 1 about 4 s later (`useProjectileCamera`). A sidecar holding a view should not fight it.
9. **`ProjectileList`'s target field is 0 for a TV missile** in every shot here, including the one
   that killed an enemy heli: the unique target object has no networkable id, and its root is itself.
10. **Order within a tick:** a direction in the same tick as the alt-fire toggle is refused, because
    the module applies directions before the engine runs that tick's buttons.
11. **Eight TV missiles a sortie.** The AH-1Z TV launcher has `nrOfMags 8`, one missile each. After the
    eighth launch `TickState` shows it with 0 rounds, 1 magazine (the empty loaded one) and not
    reloading; hovering does not rearm it. A sidecar should treat that state as empty, not as a
    reload to wait for.

## What is left

- The Z-10 gunner's TV launcher was not run (same tweak shape and projectile as the AH-1Z's).
- Wire-guided weapons are refused by design; none is on the 2v2 helis.
- The sidecar side of item 2 (the alt-fire toggle and the aim) is bf2-bots work.

## Remaining frontiers relevant to this change

- No `[FRONTIER]` line was printed in any run.
- The TV trace used for evidence is the tree's own opt-in `[TEMP]` helper
  (`Game/Objects/Projectiles/TVMissileTrace.h`, `BF2_TRACE_TV_MISSILES=1`), not a module addition.

## Gates

| Gate | Result |
|---|---|
| OpenBF2 | no source change; the Linux build of `873d1b1` passes (`python runtime check complete`). The MSVC gates were not rerun: nothing in OpenBF2 changed |
| Module unit tests | `serverext_tests` and `serverext_admin_tests` pass (`build-module.sh`, WSL) |

## Git

- openbf2-server-ext, branch `master`: one commit with the module, tests, test client, README and this
  file. Nothing was pushed; no remotes, issues or pull requests.
- OpenBF2 `serverext/r1`: nothing to commit (no engine change).
