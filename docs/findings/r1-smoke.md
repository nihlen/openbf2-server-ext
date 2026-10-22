# R1 / R1b engine changes and the runtime smoke probe

Status: done on 2026-10-22 (UTC) by a worker thread. The OpenBF2 changes are uncommitted in the
worktree `C:\Users\Alex\.t3\worktrees\OpenBF2\serverext-r1` (branch `serverext/r1`, based on
`ccf3099`), for the author to review. No commits, pushes, issues or PRs. The test server is stopped.

The design record for every non-retail change is `docs/protocol.md` "Engine requirements" R1/R1b,
which stands in for the linked issue that OpenBF2's `AGENTS.md` asks for. Every departure is marked
`[CHOSEN, not recovered]` and cites "openbf2-server-ext docs/protocol.md R1" or "R1b". IDA and
Ghidra were not used. No recovered body was written or changed; the edits are intentional additions,
plus one split of an existing body that leaves its statements unchanged.

## Summary

| Item | Result |
|---|---|
| R1 `IServerExtension` + `createInputPlayer` | Works at runtime. The probe fetched the singleton in `initDll` and created an AI-flagged, connection-less player (`ai 1 remote 1 connected 0`) with no AI library loaded. |
| R1b courier guards | Compiled on all three toolchains. **Not exercised at runtime**: no missile lock, no `PIUse` and no defibrillator happened. |
| Q1 tick hook (EventManager category 7) | Works. Exactly 30 handler calls per second: tick 2160 at 72 s and tick 9600 at 320 s of uptime. It kept firing across two map changes. |
| R2 route (pad spawn, then `IGame::enterVehicle`) | Works. The bot spawned on the `Z10_*` pad, entered the Z10 pilot seat (PCO id 0), and the heli climbed under collective 1.0. |
| Q2 conversion | The axes and signs are consistent with motion at runtime (see [Q2](#q2-and-the-physics-node)). |
| `queryInterface(IID_IPhysicsNode)` | **The contract is wrong.** It returns null for every object, including the heli. The node is a component, so the route is `queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`. |
| Q7 `sv.numPlayersNeededToStart 0` | The bot spawns with **either 0 or 2**. The contract's "a server with only bots never spawns them" is wrong for this tree (see [Q7](#q7-round-start)). |
| Spawn group by walking spawn points | Works on `dalian_2_v_2`, but only with a distance heuristic, which **failed on `daqing_2_v_2`**. A typed lookup is recommended; that is the author's call (see [Spawn group lookup](#spawn-group-lookup-r1-last-bullet)). |
| 5-minute soak | 17 min 31 s on one process. It covered 23 heli entries and 22 exits (21 crashes after the climb, 1 end-of-round kill), one `admin.restartMap`, and two map changes through `admin.runNextLevel`. 0 `[FRONTIER]`, 0 abort or segfault lines; SIGTERM stopped it cleanly. |
| Map change | The engine destroyed the bot at unload. The probe detected that and did not destroy it again. It recreated the bot on the reloaded `dalian_2_v_2`. |
| Toolchains | MSVC x86 full syntax gate: pass. x86 isolated full build: 969/969 compiled, and the diagnostic-link unresolved set is identical to baseline. Linux build: pass. **msvc-x64 strict link: fails, with exactly the same 6 unresolved symbols as unmodified HEAD** (pre-existing VoIP `.def` issue). |

## Part 1: OpenBF2 changes

`git diff --stat` (plus one new file), all under `Code/BF2/`:

| File | Change |
|---|---|
| `Game/GameServer/IServerExtension.h` | **New**, header-only. `IServerExtension` has one pure virtual, `int createInputPlayer(const std::string& name)`, which returns the player id or -1, and a protected non-virtual destructor. It is not an `IBase`. The comment documents the singleton name, ownership (flag 4, borrowed), lookup idiom, lifetime and append-only rule. |
| `Game/GameServer/GameServer.h` | +9 lines at the **end** of the class: a `private:` forward declaration of the nested adapter `class ServerExtension;` and the non-virtual helper `int createPlayerUngated(const std::string&, bool)`. No data members, no virtuals and no base change, so GameServer's layout, base offsets and vtables are unchanged. |
| `Game/GameServer/GameServer.cpp` | (1) `#include "IServerExtension.h"`. (2) Before `GameServer::init`: the adapter `class GameServer::ServerExtension : public IServerExtension`, which holds a `GameServer*` and forwards `createInputPlayer` to `createPlayerUngated(name, true)` (or returns -1 once withdrawn). Also a file-scope `IServerExtension* g_serverExtension`, whose address is the singleton slot. (3) In `init`, just before the `openbf2.con` block: a function-local static adapter, and `registerSingleton({"OpenBF2ServerExtension", &g_serverExtension, 0, 0, 4})`. This is the same flag-4 idiom `Core/Module.cpp` uses for `DebugOn`, and `SettingsRepostitory.cpp:34-36` uses for an existing object. (4) `createPlayer` now keeps only its AI-library gate and calls `createPlayerUngated`. The helper is the former body after the gate, unchanged statement for statement, so `createPlayer(name, true/false)` behaves identically. (5) At the end of `shutdown()`, before `m_shutdownComplete = true`: `unregisterSingleton`, detach the adapter and null the slot. (6) R1b guard in `sendMissileMessageToOccupyingPlayers`: `if (!m_iaiEventCourier) continue;` before `eventMissileLock`, which was `:3340` and is `:3397` after my insertions. |
| `Game/Objects/Game/PlayerControlObjectHelpers.cpp` | R1b guard in `BFfindEntryPoint` (`:327-329`, verified). The `getRequestedVehicle` call only runs when `m_iaiPlayerCourier` is non-null; otherwise `aiRequestedVehicle` stays 0. The existing call lines were re-indented under the `if`. |
| `Game/GameLogic/ServerGameLogic.cpp` | R1b guard in `handleCollisionForResurrecter` (`:3411-3415`, verified): `if (!courier) return false;`. That is the same return the function already makes for a null `getResurrectTarget`. |

Why an adapter object rather than an extra base: a fourth base on `GameServer` would add a vptr
sub-object after `IEventListener` and shift every recovered field offset that the header documents
(`+0xF4…+0x169`). It would also change what `queryInterface` adjusts. The adapter changes no
layout. It is a nested class so that it can call the private helper without a `friend` or a public
entry point.

Why `getSingleton` and not `getSimpleSingleton`: the contract says `getSingleton`, and the engine
already registers non-`IBase` pointers as flag-4 normal singletons. The module side is
`reinterpret_cast<IServerExtension*>(cm->getSingleton("OpenBF2ServerExtension"))`, the same raw
idiom `Core/DllModule.cpp` uses.

Other checks:

- No new `.cpp`, so the image manifests and the syntax gate's file list are unchanged.
- No retail `IGame` or `IGameServer` vtable change.
- Unrelated `g_aiInstance`/courier sites were checked. `Squad.cpp:343-347`, `SquadManager.cpp:471`
  and `:662` are behind `g_aiInstance && isInitialized()`. `ReplenishingAmmoComp.cpp:68` is a getter
  whose callers null-check it. No further guard is needed for v0.

**R1b site 1 semantics, for the author to confirm.** The guard treats a missing courier as "no
requested vehicle", not as "no entry point found". So an AI-flagged player with no AI library now
gets the nearest free seat within 15 m of an enter-on-spawn point, or when it presses `PIUse`,
instead of crashing. A retail courier can also return null. The other reading, making
`BFfindEntryPoint` return null for such a player, would leave an enter-on-spawn bot standing 4 m
above the heli. Neither path was exercised. The contract's "never use the enter-on-spawn group"
still stands until that path is tested.

## Validation per toolchain

| Gate | Command / output dir | Result |
|---|---|---|
| MSVC x86, single TUs | `Code\Tests\syntaxcheck.cmd` on `GameServer.cpp`, `ServerGameLogic.cpp`, `PlayerControlObjectHelpers.cpp` | Pass, no diagnostics |
| MSVC x86 full syntax gate | `cmd /c Code\Tests\syntaxcheck.cmd` | `ALL OK`, exit 0 |
| MSVC x86 isolated full build | `BF2_BUILD_OUT=%TEMP%\serverext-r1\obj_x86_full`, `Code\Build\build.cmd` | `compiled: 969 failed: 0`. The diagnostic `/FORCE` link reports 56 unique unresolved externals (172 factory-free), and `build.cmd` exits 1. **The same build of unmodified HEAD** (`git archive HEAD`, `obj_x86_full_baseline`) gives the same counts and an **identical symbol set** (diffed, both lists). |
| msvc-x64 strict link | x64 Python built into `%TEMP%\serverext-r1\py-x64`; `cmake --preset msvc-x64 -B %TEMP%\serverext-r1\x64build`, target `bf2_w64ded` | **Fails: LNK1120, 6 unresolved externals**, all `BF2VoipServer::CBF2VoipServer::{ctor, dtor, CreateInstance, Initialize, Update, Shutdown}`, referenced from `VoipServerInternal.cpp`. 0 compile errors; the three changed TUs compiled. **Unmodified HEAD fails with an identical set** (`x64build-baseline`, diffed). Cause, from the files: `Code/VoipServerAbi/BF2VoipServer_w32ded.def` exports x86 `__thiscall` names (`QAE`), while x64 references `QEAA`. This predates the change (it came in with `e0fcdaa`). |
| Linux x86-64 | `testenv/build-linux.sh`, out `~/bf2-linux-serverext-r1`, targets `bf2 dice_py_check_run` | Pass. `python runtime check complete`, exit 0 |
| Probe module | `probe/build-probe.sh`, out `~/serverext-probe-build` | Links with `--no-undefined`. Its only dynamic imports are glibc plus weak TM/`__cxa_pure_virtual`. `initDll` is exported. |

## Part 2: the probe

`probe/` in this repo: `CMakeLists.txt`, `src/Probe.cpp` and `build-probe.sh`, all `[TEMP]`.
Linux only. It compiles against the R1 worktree headers with the engine's image define,
force-includes and ABI-relevant GCC flags, plus `-static-libstdc++` like `bf2`.

Constraint found while building it: **`bf2` is not linked with `-rdynamic` and exports no engine
symbols.** A module can only make typed virtual calls and use inline header code. Non-virtual
engine functions such as `world::getRootParent` cannot be called; the probe walks the inline
`IObject::getParent()` instead. The real module has the same constraint.

How it works: `initDll` gets `EventManager`, `OpenBF2ServerExtension`, `PlayerManager`,
`ObjectManager` and `Game`, then registers category 7 and posts the first event. Once a second,
until the bot exists, it walks `ObjectManager::getAllRegisteredObjects()` for control points and
spawn points. It creates the bot, then sets team 1, kit 0 and the on-foot group. When the bot is
alive, it enters the nearest free heli pilot seat within 30 m. In the heli it writes, every tick,
the `PlayerAction::get` mapping mask plus quantized axes: 3 s at 0, then collective 1.0 until 60 m
above the pad, then 0. That deliberately makes the heli crash, which tests destruction and
respawn. It logs once per second.

### Run setup

- Run dir `~/serverext-r1/srv`, made by `testenv/prepare-content.sh` from
  `cr.nihlen.net/library/bf2-netsky:2v2-nocheck`. The `bf2`/`libdice_py.so` pair is the R1 Linux
  build (md5 checked).
- Ports: game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723.
- Changes from the image: those of `prepare-content.sh`, plus `mods/bf2/openbf2.con` =
  `openbf2.addModDll serverext_probe`, `libserverext_probe.so` in `bin/amd-64/`, and
  `sv.numPlayersNeededToStart` set to 0 (runs 1–2) or back to the content's 2 (run 3).
- Path exercised: `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`, with
  real map loads of `dalian_2_v_2` (`gpm_cq` 16) and `daqing_2_v_2`. No `BF2_*` harness variables.
  No client connected.
- The AI library was not loaded: `/proc/<pid>/maps` had 0 `aidll` mappings. The retail
  `libaidll.so` sits in `bin/amd-64` but is only loaded with `UseAI` (`gpm_coop`).

| Run | Duration | Notes |
|---|---|---|
| 1 | about 1 min | First probe build. Showed `queryInterface(IID_IPhysicsNode)` = null, and a probe bug (it stayed in `Fly` after the bot died). Stopped with SIGTERM. |
| 2 | 17 min 31 s | Main run: soak, `admin.restartMap`, `admin.runNextLevel` ×2. Stopped with SIGTERM; UDP 16571 freed. |
| 3 | 60 s | Q7 contrast with `sv.numPlayersNeededToStart 2`. Stopped with SIGTERM. |

Logs: `~/serverext-r1/bf2-run*.log`, `bf2-run2-soak.log` and `bf2-q7-needed2.log` in WSL.

### Stage results (run 2 unless stated)

**Load and `initDll` (R1 registration, Q1).**

```
[TEMP][serverext_probe] loaded; initDll(0x61efc29bd210)
[TEMP][serverext_probe] initDll: EventManager=0x… OpenBF2ServerExtension=0x61ef93e923e0 PlayerManager=0x… ObjectManager=0x… Game=0x…
[TEMP][serverext_probe] initDll: registerEventHandler(7)=1 first postEventNextUpdate handle=1
openbf2.addModDll: loaded 'libserverext_probe.so' (initDll ok)
```

**Spawn points, group and bot creation (R1).** These lines are from run 1; run 2 shows the same
control points, spawn points and groups.

```
  control point '' team 2 at 242.2 147.1 -265.6
  control point '' team 1 at -206.6 152.4 157.9
  spawn point '' group 3 enterOnSpawn 0 at -214.1 152.4 171.0 (15.1 m from base)    (×3, the Z10_1..3 pad)
  spawn point '' group 4 enterOnSpawn 1 at -218.1 156.4 159.1 (12.2 m from base)    (Z10_4, in-heli)
  spawn point '' group 1 enterOnSpawn 0 …  / group 2 enterOnSpawn 1 …               (the AH1Z base)
tick 30: 15290 objects, 2 control points, 8 spawn points; base '', on-foot group 3
tick 30: createInputPlayer -> 255 (player 0x…, ai 1 remote 1 connected 0)
tick 30: bot 255 team 1 kit 0 spawnGroup 3; AI input buffer 0x…
tick 31: bot 255 is alive after 1 ticks, at -217.3 153.6 173.3
```

**Heli entry (R2).** All helis report team 0 when empty. The second AH1Z is 27–30 m away, so the
30 m search can also pick the other base's heli type; the Z10 at 14 m was nearer.

```
  heli '' (ahe_ah1z) 27.2 m away, team 0, seats 2, pilot 0x… occupied 0
  heli '' (ahe_z10) 14.2 m away, team 0, seats 2, pilot 0x… occupied 0
tick 61: enterVehicle(bot 255, seat 0x570a1d3e81c0, false) -> 1; player vehicle now 0x570a1d3e81c0, root 0x570a1d3e81c0
```

**Flight under collective (SetInput path).**

```
tick 150 … collective 0.00 | root 'ahe_z10' pos -218.06 153.89 159.06 ypr 163.7 0.6 -0.0 vel 0.00 -0.00 -0.00 angvel -0.000 -0.000 -0.001
tick 180 … collective 1.00 | root 'ahe_z10' pos -218.06 153.92 159.04 ypr 163.7 0.5 -0.0 vel 0.01 0.03 -0.05 …
tick 330 … collective 1.00 | root 'ahe_z10' pos -217.19 154.76 156.08 ypr 163.7 2.9 -0.0 vel 0.97 1.31 -3.32 angvel -0.015 0.000 -0.004
tick 480 … collective 1.00 | root 'ahe_z10' pos -202.50 173.00 105.58 ypr 163.7 2.8 -0.0 vel 4.33 5.87 -14.91 angvel 0.019 0.000 0.005
tick 600 … collective 1.00 | root 'ahe_z10' pos -187.11 205.77 52.35 ypr 163.8 -7.2 -0.1 vel 2.17 10.52 -7.59 angvel 0.070 0.000 0.021
tick 623: 60 m above the pad; collective -> 0
tick 874: bot left the heli (alive 0); neutral input, back to WaitAlive       (crashed)
tick 934: bot 255 is alive after 60 ticks …                                   (engine respawn on the pad)
tick 964: enterVehicle(bot 255, seat 0x570a1d3ef650, false) -> 1             (the respawned heli)
```

Rotor spin-up takes about 4 s before lift-off. The heli climbed about 60 m in 15 s. That cycle
repeated through run 2 (23 entries in total). The bot alternated between the two Z10 seat objects
as the spawner replaced each wreck.

**`admin.restartMap` (RCON).** This is a round reset without unload
(`ServerGameLogic::restartMap`, `ServerGameLogic.cpp:812`). It does not destroy AI players. The
bot stayed, with the same id, and kept cycling. There was no crash.

**Map change (`admin.runNextLevel`, then `admin.nextLevel 0` + `admin.runNextLevel`).**

```
tick 18629: bot left the heli (alive 0); neutral input, back to WaitAlive   (end of round)
tick 19379: bot id 255 no longer exists (engine removed it); not destroying it again
tick 19380: 17385 objects, 2 control points, 12 spawn points; base '', on-foot group 0   (daqing_2_v_2: heuristic finds nothing)
…
tick 30780: createInputPlayer -> 255 (player 0x570a1dbe0e70, ai 1 remote 1 connected 0)  (dalian_2_v_2 again)
tick 30781: bot 255 is alive after 1 ticks, at -212.2 153.6 173.4
tick 30811: enterVehicle(bot 255, seat 0x570a1b6e3250, false) -> 1
```

The unload destroyed the bot (`GameServer.cpp:1416-1429` path). The probe held only the id, and
`getPlayerFromId` returned null, so there was no double free. The tick hook survived both
unload/load cycles: the handler kept firing on both maps. Whether it fires during the load itself
was not measured; the counter is the probe's own. The probe's spawn-group
heuristic found no group on `daqing_2_v_2`, so no bot existed on that map; that is the probe's
limitation, not the engine's.

**Shutdown.** `stopped pid 9732 (SIGTERM)` at 17:31 uptime. The log had 0 `FRONTIER`, `abort`,
`Segmentation` or `core dumped` lines, and UDP 16571 was unbound afterwards. Whether SIGTERM reaches
`GameServer::shutdown` (and so the new `unregisterSingleton`) was not instrumented.

### Q2 and the physics node

- **`queryInterface(IID_IPhysicsNode)` returns null.** It did so on the soldier, the free camera
  and the `ahe_z10` root for the whole run (the probe logs when the result changes; it never did).
  The engine attaches the node as a **component**, with `object->setComponent(IID_IPhysicsNode, node)`
  (`SimpleObjectTemplate.cpp:415`, `:482`, `TemplateLeaves.cpp:937`). The `+0x44` field that
  `IObject.h:241` describes caches the component's interface; the object itself does not answer
  for it. The working typed route is `IObject::queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`
  (IObject slot 23, as `Spring.cpp:81` uses it). The probe used it to get the velocities above.
- **The conversion is consistent with the motion:**
  - Velocity matches the position deltas: z went 158.40 → 156.08 over 1 s while v_z went −1.33 → −3.32.
  - Positive pitch went with the nose-down, forward acceleration along yaw 163.7° (forward ≈
    (+0.28, −0.96), and v had +x and −z).
  - As the heli pitched up, ω_x was positive. At yaw 163.7° the body right axis is about −x, so
    +x world rotation is nose-up. The measured rate, about 2.8°/s from pitch −0.7 → −3.5 between
    ticks 540 and 570, matches |ω|·0.96 ≈ 2.75°/s.
  - Roll stayed about 0. Yaw and roll were not exercised (no cyclic or pedal input), so their signs
    are verified only by the static source analysis.

### Q7: round start

- With `sv.numPlayersNeededToStart 0` (runs 1–2) **and** with the content's `2` (run 3), the bot
  spawned in 1 tick after creation, with `Players: 0/16` and no human.
- Source: `ServerGameLogic::uPlaying` sets `m_roundGoing = true` when it enters begin-round state 0
  (`ServerGameLogic.cpp:423`). That happens before state 1 waits for
  `getNumberOfAliveHumanPlayers() >= needed` (`:452`). `spawnPlayer` only checks `m_roundGoing`
  and status 1 (`:1923-1928`).
- So the contract's statements are wrong for this tree: "A round needs `sv.numPlayersNeededToStart`
  alive, connected humans, so a server with only bots never spawns them" (How these bots differ),
  and "For bot-only runs, try `sv.numPlayersNeededToStart 0`" (Testing).
- What the setting still changes, from source and not instrumented: with 2 the logic stays in state
  1 (waiting, no countdown, no round timer or ticket start). With 0 it proceeds to states 2/3. In
  state 3 the not-enough-players check counts all players including bots (`:503-505`).
- Bot-only runs therefore need no human. Use `0` if the round timer and tickets matter.

### Spawn group lookup (R1, last bullet)

- Walking `ObjectManager::getAllRegisteredObjects()` for `IID_ISpawnPoint` works, and
  `getSpawnGroupId()` and `getEnterOnSpawn()` return the expected values. The output was 4 groups
  on dalian, pad = 1/3 and in-heli = 2/4, matching the per-(control point, group) allocation.
- **But `ISpawnPoint` exposes neither its team nor its control point,** so the module cannot tell
  which base a group belongs to. The probe paired groups with the team-1 control point by distance
  (≤ 60 m). That worked on `dalian_2_v_2` and failed on `daqing_2_v_2`.
- A typed lookup (team → active on-foot groups, from `SpawnManager::getGroupsForTeam` plus
  `ISpawnGroup::getActive/getSpawnPoints`) would be the smallest robust addition to
  `IServerExtension`. **I did not add it.** That needs the author's or coordinator's decision on its
  shape, because "group 0/1" in the contract are per-control-point layer numbers that the engine
  does not keep after allocation.
- For v0 on dalian, walking works.

## Contract points that proved wrong or incomplete

Reported here; `docs/protocol.md` was not edited.

1. **Conventions / Unverified claims:** `queryInterface(IID_IPhysicsNode)` does not return the
   physics node. Use `IObject::queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`.
2. **How these bots differ / Testing / Q7:** bots spawn without humans whatever
   `sv.numPlayersNeededToStart` is. Q7 can be closed. The "never spawns" bullet and the end-to-end
   check's human requirement are wrong.
3. **R1 last bullet / Q3 gap:** walking spawn points cannot attribute a group to a team or base
   without a heuristic. A typed lookup is recommended (decision above).
4. **Loading / module build:** `bf2` exports no symbols. The module can use only virtual calls and
   inline header code; for example, `world::getRootParent` is unavailable, so walk `getParent()`.
   The contract's "typed virtual calls" rule already implies this, but the module design should
   assume it explicitly.
5. **Map change:** confirmed as written. Also, `admin.restartMap` is a round reset that does
   **not** destroy bots. The end of every round kills them (`alive 0`), and the engine respawns
   them in the next round, so the module must expect a bot to die and respawn without a
   `BotRemoved`.
6. **Testing:** `prepare-content.sh` yields `sv.numPlayersNeededToStart 2` from the content, and the
   retail `libaidll.so` is present in `bin/amd-64` (harmless on `gpm_cq`).
7. **x64 Windows build:** the strict `bf2_w64ded` link is currently broken at HEAD (VoIP `.def`),
   so "Built as `serverext.dll` (Windows x86 and x64)" cannot be validated against a linked x64
   server until that is fixed upstream.

## Remaining frontiers and unexercised paths

- **R1b, all three sites:** not reached at runtime (no enemy missile lock, no `PIUse`, no
  defibrillator). Only compiled.
- **Enter-on-spawn for an AI-flagged player:** now guarded but untested. Keep avoiding it.
- **`IServerExtension` withdrawal in `GameServer::shutdown`:** not observed. SIGTERM shutdown was
  clean, but whether `shutdown()` ran was not instrumented.
- **Q8 (category 7 in retail binaries):** still open. No IDA or Ghidra.
- **Yaw/roll inputs and the client ControlMap stage (Q4):** not exercised. Only collective was
  written.
- **Not exercised at all:** any client connection, a human pressing E near the bot's heli, Docker
  (glibc 2.31), and Windows server images.
- **Existing frontiers this touches:** `AutoRecorder::roundFinished` `[FRONTIER]` (`restartMap`
  arm) was not reached because the server has no auto-recorder. No `[FRONTIER]` line was printed
  in any run.
