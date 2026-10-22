# Module v1: spawn fallback, vehicle list, on-foot input and buttons

Status: done on 2026-10-22 by a worker thread; committed locally in both repos (OpenBF2 branch
`serverext/r1`, openbf2-server-ext `main`). The Q10/Q11 answers below were written before the module
used them, as the brief requires. Paths without a repo prefix are OpenBF2 paths under `Code/BF2/` in the
`serverext-r1` worktree. The v1 spec is `docs/protocol.md` "v1 (approved 2026-10-22)".

## Q10: what moves, strafes and turns a soldier

The consumer is `Soldier::handlePlayerInput` (`Game/Objects/Game/Soldier.cpp:3716`). Every read
goes through `mappedInput`, so an unmapped slot reads 0 (`Soldier.cpp:278-280`).

| Action | Slot | Shaping | Evidence |
|---|---|---|---|
| Walk forward (+) / back (-) | `PIThrottle` = 3 | `conditionAxis`: clamp to -1..1, sign-preserving square, 0.001 deadzone | `Soldier.cpp:3778`, `:256-267` |
| Strafe | `PIYaw` = 0 | same `conditionAxis` | `Soldier.cpp:3779` |
| Turn (yaw) | `PIMouseLookX` = 4 | `* phy-soldier-look-factor-x` (default 5.0), then the weapon/FOV mouse-speed modifier; no clamp, 0.001 deadzone | `Soldier.cpp:3821-3833`, `:100` |
| Look up/down | `PIMouseLookY` = 5 | same with `phy-soldier-look-factor-y` (5.0); the accumulated angle is clamped to ±85° (`soldier-lookMaxAngle.y`) | `Soldier.cpp:3822`, `:3839`, `:3642-3649` |

- **The spec's assumption holds in part.** Look *is* a separate pair of axes (slots 4 and 5), but
  `PIYaw`, the heli's pedals slot, **strafes** on foot; it does not turn. `PIPitch` and `PIRoll`
  are not read by the soldier at all (no reference in `Soldier.cpp`).
- **So on foot the existing `SetInput` fields mean:** collective = walk forward/back, pedals =
  strafe, cyclic pitch and roll = nothing, look yaw = turn, look pitch = look up/down.
- **Units of the look axes: degrees per tick, times 5.** `m_facingDegrees += appliedYaw`, wrapped at
  ±360 (`Soldier.cpp:3909-3911`), and the rate sent to physics is `appliedYaw / dt`
  (`:4037`). With the defaults, look yaw 1.0 turns up to about 5° per tick (150°/s), scaled further
  by `getMouseSpeedMod()` while zoomed.
- **Turning while standing still is not immediate.** With `|forward| <= 0.01`, yaw first moves only
  the head (`m_lookLeftRightAngle`) up to `soldier-lookMaxAngle.x` (40°); beyond that the body
  follows at `soldier-lookSideRestore` (4 per tick) (`Soldier.cpp:3863-3906`). While walking, yaw
  goes straight to the body. A sidecar that wants a heading should turn while walking or allow for
  the 40° head lead.
- **Signs:** `PIThrottle` + is forward (`IO/Input/ControlMap.h:38`, W is +). The `PIYaw` strafe
  sign and the "up" sign of look pitch are not established from source; they are measured at
  runtime below.
- **Movement is dropped** for a seated (parented) soldier (`Soldier.cpp:3809-3816`), and the whole
  tick is skipped while `m_inputSuppressTimer > 0` (`:3727`), for example just after spawning.

### Transport, range and quantization

- **AI players bypass the wire codec.** `GameServer::simulatePlayerUpdate` uses the bot's own
  buffer: `chosen = static_cast<io::PlayerInput*>(player->getAIPlayerInput());`
  (`Game/GameServer/GameServer.cpp:3512`). Whatever the module writes is read as is: no 0.01
  quantization and no mask rebuild. The quantization in `SetInput` is therefore a human-parity
  choice of the contract (v0), not something the engine imposes on bots.
- **Humans' look axes are quantized like the flight axes.** Slots 4 and 5 are inside
  `PlayerAction`'s six axes: `for (int axis = 0; axis <= 5; ++axis) setAxis(input, axis);`
  (`Game/Common/PlayerAction.cpp:72-73`); each is `short(convertAxisToInternalResolution(v) * 100)`
  (`:27-37`, `:57`) and travels as sign plus 15-bit magnitude
  (`Game/Common/PlayerActionManager.cpp:288-297`). The camera slots 6 and 7 are never transported.
- **Range:** a human's look value is raw mouse counts times sensitivity
  (`IO/Input/ControlMap.cpp:680-700`), so it can exceed 1 (up to ±327.67 on the wire).
- **Decision (conservative):** keep the contract's -1..1 for the look axes, clamped, and quantize
  them with the same two-step human chain as the other axes. -1..1 still allows 150°/s of turn,
  which is plenty for walking to a heli. A wider range is a contract change, not a module choice.
- **Mapping bits:** the module already sets the human mask `PlayerAction::get` produces,
  `0x4000FFFFFFFFFF3F` (axes 0-5, buttons 8-47 and 62; `PlayerAction.cpp:13-18`, `:174-199`). Slots
  4 and 5 are in it, so v0 already wrote them as 0. No mask change is needed.

### In a vehicle

- `Player::handleInput` (`Game/GameLogic/Player.cpp:5087-5104`): on a seat whose camera has
  `getToggleMouseLook`, holding `PIMouseLook` (11) zeroes `PIPitch`, `PIRoll` and `PIYaw` (free
  look); otherwise `PIMouseLookX/Y` are zeroed. Gunner seats and turrets read whatever slot their
  template names (`RotationalBundle.cpp:830`, `:889`; `EngineBase.cpp:245-247`). So a pilot's look
  axes are ignored unless `PIMouseLook` is held, which also kills the cyclic. That is why
  `PIMouseLook` is **not** in the button table below.

## Q11: button bit table

Buttons are level-triggered at the dispatcher: `inputPressed` is `mapped && inputs[index] > 0.5f`
every tick (`Game/GameLogic/ServerGameLogic.cpp:1050-1055`). Anything edge-like is in the consumer.
`checkPlayerTriggers` runs only with game status 1, or 3 for an AI player
(`GameServer.cpp:3549-3555`). The module writes 1.0 for a set bit, for that tick only.

| Bit | Slot | Name | Effect, and how it behaves when held | Evidence |
|---|---|---|---|---|
| 0 | 8 | `PIFire` | Fire; `setFiringState`, fire messages. Level. | `ServerGameLogic.cpp:1362-1387` |
| 1 | 31 | `PIAltFire` | Alt fire. Level. | `:1363`, `:1388` |
| 2 | 47 | `PIFlareFire` | Flares / countermeasures. Level. | `:1364`, `:1389` |
| 3 | 9 | `PIAction` | Jump on foot (standing, on the ground). Level, with post-jump timers. | `Soldier.cpp:3776`, `:3960` |
| 4 | 13 | `PISprint` | Sprint. Level. | `GameLogic.cpp:2533-2542`; `ServerGameLogic.cpp:1351-1358` |
| 5 | 38 | `PICrouch` | Crouch while held; release stands up. | `Soldier.cpp:3476-3496` |
| 6 | 37 | `PILie` | Prone toggle, edge-latched. | `Soldier.cpp:3444-3474` |
| 7 | 32 | `PIReload` | Reload. Level. | `ServerGameLogic.cpp:1417-1421` |
| 8 | 35 | `PIToggleCameraMode` | Cycle the vehicle camera view. **Cycles every tick while held**: send it for one tick. Ignored on foot. | `:1558-1563`, `:1113-1134` |
| 9 | 39 | `PICameraMode1` | View 1 (2 = nose cam from 1 if allowed). Idempotent. | `:1565-1572` |
| 10 | 40 | `PICameraMode2` | View 3. | `:1574` |
| 11 | 41 | `PICameraMode3` | View 4. | `:1576` |
| 12 | 42 | `PICameraMode4` | View 5. | `:1578` |
| 13-21 | 14-22 | `PIWeaponSelect1`-`9` | Select a kit or vehicle weapon slot. Level; gas mask and night vision flip every tick while held. | `GameLogic.cpp:2713-2765`, `:2822-2841` |
| 22 | 45 | `PIToggleWeapon` | Next weapon. Level. | `GameLogic.cpp:2845` |
| 23 | 46 | `PIToggleFireRate` | Fire mode. Level. | `GameLogic.cpp:2918` |
| 24-31 | 23-30 | `PIPositionSelect1`-`8` | Change seat (F1-F8): `exitVehicle` then `enterVehicle` on the root's PCO map. A bot never swaps into an occupied seat (`!getIsAIPlayer()` arm). | `ServerGameLogic.cpp:1433-1548`, `:1474-1488` |

- **Excluded:** `PIUse` (10), as the contract says. Exiting a vehicle uses `PIUse`
  (`toggleEntryPoint(player, true)`, `ServerGameLogic.cpp:1341-1347`), and its enter half calls
  `BFfindEntryPoint` (`:1736`), the R1b-guarded AI path, when the seat allows entering another
  vehicle. So **there is no exit-vehicle button**: the only exit key reaches that path. Seat change
  (bits 24-31) does not: it never calls `BFfindEntryPoint` or the AI courier.
- **Also excluded:** `PIMouseLook` (11, see above), `PIAltSprint` (12), `PISelectFunc` (33),
  `PIDrop` (34, kit pickup), `PIToggleCamera` (36, no consumer found), `PISelectPrimWeapon` and
  `PISelectSecWeapon` (43, 44), and the radio, chat and UI slots 48-63, which the human mask does
  not map. The table fills all 32 bits, so there are no unknown bits to log; the "bits not in the
  table" rule cannot trigger with this table.
- **Observable on the server:** the camera keys change `player->getCamera()->getViewMode()`
  (`ICameraObject` +0x1C), and fire sets `IPlayer::getFiringState()`. Neither emits a log or event
  of its own; the module logs a bot's view-mode changes for the tests.

## Engine change (OpenBF2 `serverext/r1`)

Appended to `IServerExtension` (`Game/GameServer/IServerExtension.h:45-76`), after
`findSpawnGroup`, append-only; each is `[CHOSEN, not recovered]` and cites protocol.md v1 "Blocked
spawns". No retail vtable, layout or behaviour changes; the adapter is the same
`GameServer::ServerExtension` object as R1.

| Addition | Body | Why |
|---|---|---|
| `struct SpawnGroupInfo { id, squad, pointCount, enterOnSpawnPoints, active, onlyForHuman, x, y, z }` | plain data | The module needs a team's groups with enter-on-spawn and a position, and must not depend on `SpawnGroup`'s layout. |
| `int listSpawnGroups(int team, SpawnGroupInfo* out, int capacity)` | `GameServer.cpp:401-435`: `getGroupsForTeam`, then per group `getSquad`, `getSpawnPoints` (counting `getEnterOnSpawn() \|\| getAIEnterOnSpawn()`), `getActive(true)`, `getOnlyForHuman`, `getPos` | Lists every group, active or not, so `CreateBot` can succeed while the pad is blocked. |
| `bool restartRound()` | `GameServer.cpp:439-445`: `static_cast<ServerGameLogic*>(gameLogic)->restartMap()`, exactly what `admin.restartMap` runs (`BanListConsole.cpp:53-56`) | No typed path from a module: there is no console singleton, and `gameLogic` is not a registered singleton. |
| `int getGameStatus()` | `GameServer.cpp:213`: `gameLogic->getGameStatus()`, or -1 | Not foreseen by the contract. Dead players respawn only in status 1 (`ServerGameLogic.cpp:542`, `:1927`); without it, an end-of-round wait looks like a blocked spawn and would trigger a false `SpawnBlocked` (and a sidecar `RestartRound`). |

`getPos` is the group's own lazy average (`SpawnGroup.cpp:116-130`); calling it only refreshes the
cached average, which `SpawnGroup::init` also does.

Validation:

| Gate | Result |
|---|---|
| MSVC x86, one TU (`syntaxcheck.cmd ...\GameServer.cpp`) | Pass |
| MSVC x86 full syntax gate (`syntaxcheck.cmd`) | `ALL OK`, exit 0 (rerun after the last engine edit) |
| Linux x86-64 (`testenv/build-linux.sh`, fresh `~/bf2-linux-serverext-v1`, then incremental) | Pass, 0 errors, `python runtime check complete` |
| msvc-x64 strict link (`%TEMP%\serverext-v1\x64.cmd`, own `x64build`) | `LNK1120: 6 unresolved externals`, all `BF2VoipServer::CBF2VoipServer::*`; 0 compile errors. The sorted symbol set is identical to v0's baseline (diffed). Rerun after the last engine edit. |

## Module design notes

Files changed: `src/Protocol.h/.cpp`, `src/Config.h/.cpp`, `src/Channel.h/.cpp`, `src/Session.cpp`,
`src/Module.cpp`, `tests/TestMain.cpp`, `tools/testclient/serverext_client.py`,
`testenv/deploy-module.sh` (optional v1 keys), `README.md`. Threading, ownership and the v0
behaviour are unchanged except where listed.

- **Version:** `kVersion = 1`. A v0 or v2 `Hello` is `Error` 1 and close.
- **`serverext.cfg`:** `enterRange` (decimal metres, 1 to 1000, default 30), `spawnFallbackSeconds`
  and `spawnBlockedSeconds` (1 to 3600, defaults 5 and 15), `allowRoundRestart` (`0` or `1`,
  default 0). A bad value refuses the load, as in v0.
- **Vehicle set:** `std::map<networkable id, IObject*>`. A full walk of
  `getAllRegisteredObjects()` runs once at load and after each level unload; after that the module
  walks 500 registered objects per tick and resumes from the last key with `upper_bound`, so the
  whole map (about 15,300 objects on dalian) is covered every 31 ticks and new vehicles appear
  within about a second. An entry is used only if `IGame::getObjectFromNetworkableId(id)` still
  returns the same pointer and that object is still a vehicle; the stored pointer is never
  dereferenced. Vanished ids are dropped when the list is built. Each addition and removal is
  logged.
- **What counts as a vehicle:** a root `IPlayerControlObject` that is neither `ISoldier` nor
  `ICameraObject`. The camera exclusion is new (see contract points): every player's
  `MultiPlayerFreeCamera` is a root PCO.
- **`VehicleList`:** sent on the first tick after `Welcome` and every 30 ticks, after that tick's
  `TickState` (`Channel` has a second latest-wins slot, flushed after the state). Tick number as
  `TickState` (module tick - 1). Team is the root PCO's `getTeam()`, 0 if outside 1..255. Destroyed
  = armour hit points at or below 0. Seats = `getPcos()->size()`; the occupied mask uses the seat
  ids 0..31. The encoder stops adding entries before the payload would pass 64 KiB.
- **`EnterVehicle`:** an explicit id is refused (`Error` 5) as "not a vehicle", "the vehicle is
  destroyed" (checked before range, so a far wreck is reported as destroyed), "out of enterRange",
  or the v0 seat and team reasons. Id 0 walks the vehicle set instead of every registered object,
  skips destroyed vehicles, and takes the nearest with the seat free within `enterRange`
  (inclusive). Distances are 3-D from the soldier root to the vehicle root.
- **`CreateBot`:** lists the team's groups. Home = the lowest-id *usable* group (squad <= 0, not
  human-only, at least one point) with no enter-on-spawn point, **active or not**. `Error` 6 only if
  the team has no usable group. If it has only enter-on-spawn groups, the bot has no home and
  targets the first of them. The groups are logged at every `CreateBot`.
- **Spawn fallback:** per bot, the module keeps a target group. While the bot is dead:
  - the target is re-offered with `setSpawnGroup` every 10 ticks, because
    `uPlayingSpawning` clears a group that is inactive at that moment (`ServerGameLogic.cpp:644-657`);
  - the timers run only while `getGameStatus() == 1` and `IPlayer::getTimeToSpawn() <= 0`
    (respawn delay over), i.e. while the engine would actually spawn it;
  - after `spawnFallbackSeconds` on one target the module moves to the next group in its order
    that is active now: home, then the other on-foot groups nearest to home first, then the groups
    with any enter-on-spawn point, nearest first, cycling. A group that is inactive when its turn
    comes counts as tried. "Every group tried" is then the condition for `SpawnBlocked`, sent at
    `spawnBlockedSeconds` of waiting and every 10 s after;
  - on spawn the module logs the group (home or fallback), and if it was not home it sets the
    bot's group back to home, so the next death tries the pad first. Death resets the fallback.
- **`RestartRound`:** `allowRoundRestart 0` is `Error` 6. Otherwise at most one *successful*
  restart per 1,800 ticks (a refused request does not start the window). Then
  `IServerExtension::restartRound()` and `Ack`.
- **`SetInput`:** 33 bytes. The two look axes are validated, clamped and quantized like the
  others and written to `PIMouseLookX/Y`. The button mask is stored with the axes (latest wins)
  and applied for the one tick in which that `SetInput` is consumed; a tick without a new
  `SetInput` has no buttons pressed. The watchdog zeroes look and buttons too.
- **Observability for the tests:** the module logs each bot's camera view-mode change
  (`getCamera()->getViewMode()`) and firing-state change (`getFiringState()`), the latter capped at
  50 lines plus every 100th.

## Runtime validation

Setup: run dir `~/serverext-v1/srv` from `testenv/prepare-content.sh` (netsky `2v2-nocheck`
content) with `bf2` and `libdice_py.so` from a fresh `~/bf2-linux-serverext-v1` build of this
worktree (md5 of `bf2` checked equal), and the module from `~/serverext-build/v1`. Ports: game
16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723, bot channel **127.0.0.1:16701**; VoIP 55124/55126
as usual. No other server ran; no container was created or touched; 16700 and `wslrelay` were left
alone. Path: `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer` on
`dalian_2_v_2` (`gpm_cq` 16), content settings unchanged (`sv.numPlayersNeededToStart 2`,
`sv.spawnTime 2`). All clients ran inside WSL. No game client joined. Four server processes, all
stopped with SIGTERM: (1) `allowRoundRestart 0`, runs 1-6; (2) `1`, run 7; (3) the vehicle-log
build, runs 8-9; (4) the final build (camera filter), runs 10-17, 30 minutes. The test server is
stopped and every port above is free.

Client logs: `%TEMP%\serverext-v1\run*.log`; server logs: `~/serverext-v1/srv/bf2*.log` in WSL.

### Summary

| Stage | Result |
|---|---|
| Load, v1 `Welcome` | Pass: `loaded: protocol v1, bot channel 127.0.0.1:16701, ... enterRange 30.0 m, spawnFallback 5 s, spawnBlocked 15 s, allowRoundRestart 0` |
| `VehicleList` | Pass: first on the tick after `Welcome`, then exactly 1/s (660 in the 660 s soak). dalian_2_v_2 has 4 helis. |
| Walk (scenario c) | Pass 9/9 (twice): out-of-range `EnterVehicle` refused, strafe and turn signs measured, walked 27-29 m to an AH-1Z with `VehicleList` and the on-foot axes, entered it. |
| Enter-on-spawn for the AI bot (R1b site 1) | **Pass**: bot seated in the Z10 by the in-heli point, stayed alive and seated; no crash, no `[FRONTIER]`. Enter-on-spawn groups stay in the fallback. |
| Blocked pad, fallback (scenario a) | Pass 3/3: 3 standing bots fill the pad; the 4th falls back after 5 s to the in-heli point. |
| All blocked, `SpawnBlocked`, `RestartRound` (b) | Pass 4/4 with `allowRoundRestart 0` (refused, `Error` 6) and 6/6 with `1` (Ack, second within 60 s refused, everyone respawns; the fallback bots respawn at home). |
| Buttons (d) | Pass 6/6: camera keys change the view mode exactly per the Q11 table; fire hit an empty Z10 (health 1.00 → 0.96); F2 moved the bot to seat 1. |
| Destroyed vehicles | Pass: explicit wreck id → `Error` 5 "the vehicle is destroyed"; `EnterVehicle(0)` with only a wreck in range → `Error` 5, then Ack 1 s later on the respawned heli. |
| Fuzz (e) | Pass 41/41, server still at 29.8 ticks/s afterwards. |
| Map change | Pass (twice): `BotRemoved` reason 2, recreate, spawn, re-enter, vehicle set rebuilt. |
| 10-minute soak | **Pass**: `HOVER HELD 600 s; max altitude error 2.99 m; horizontal drift 8.3 m; health 1.00`, 19,800 `TickState`s (2 client-side gaps), 660 `VehicleList`s; 0 `[FRONTIER]`, 0 abort, 0 segfault in the whole 30-minute process; RSS 142,416 → 142,420 KB. |
| Per-tick cost | p50 70-80 µs, p99 110-150 µs per 1,800-tick window; under 1 ms everywhere except ticks that create a bot (about 2.2-2.8 ms). Detail below. |

### Enter-on-spawn and the blocked pad (scenario a)

Team 1's groups on dalian, logged at every `CreateBot`:

```
team 1 spawn group 3: 3 points (0 enter-on-spawn), active 1, squad -1, human-only 0, pos -214.5 152.4 172.6
team 1 spawn group 4: 1 points (1 enter-on-spawn), active 1, squad -1, human-only 0, pos -218.1 156.4 159.1
```

A live soldier within 1 m of an on-foot point blocks it (`Game/Objects/SpawnPoint.cpp:309-346`),
so standing bots are a safe, repeatable blocker. It is the same check a parked heli or wreck trips
(`CID_PlayerControlObjectTemplate`). Run 2:

```
tick 1702: bot 255 spawned at group 3 (home) after 1 ticks ready; on foot ... pos -212.2 153.6 173.4
tick 1705: bot 254 spawned at group 3 (home) ...                      pos -217.3 153.6 173.3
tick 1708: bot 253 spawned at group 3 (home) ...                      pos -214.1 153.6 171.0
  team 1 spawn group 3: 3 points (0 enter-on-spawn), active 0, ...
tick 1859: bot 252 spawn fallback: group 3 -> 4 (enter-on-spawn)
tick 1861: bot 252 spawned at group 4 (fallback) after 152 ticks ready; in ahe_z10 seat 0; pos -218.1 153.9 159.1
```

Client: `PASS fallback bot 252 is in a vehicle (root 1819 seat 0)`, `PASS fallback bot still alive
and seated after 10 s`. The path is `spawnPlayer` → `toggleEntryPoint(player, false)` →
`BFfindEntryPoint` with the AI radius 15 and the R1b courier guard. The bot got the nearest free
seat, as a human would. A second fallback bot got seat 1 of the same Z10. Flying a fallback-spawned
bot was not tested; it is seated through the same `enterVehicle` call as `EnterVehicle`.

### All blocked, `SpawnBlocked` and `RestartRound` (scenario b)

With the pad full and both Z10 seats taken, the in-heli group is inactive too (no free seat within
15 m; the AH-1Z on that base is farther). Run 7 (`allowRoundRestart 1`):

```
tick 635: bot 250 has tried every spawn group of team 1 (2)
tick 935: bot 250 SpawnBlocked: not spawned for 15 s with every spawn group tried
tick 1235: bot 250 SpawnBlocked: not spawned for 25 s with every spawn group tried
tick 1267: round restarted on request (admin.restartMap)
tick 1267: bot 255 died   (... 254, 253, 252, 251)
tick 1268: RestartRound refused: last restart 1 ticks ago
tick 1271: bot 252 spawned at group 3 (home) after 3 ticks ready; on foot ...
tick 1271: bot 251 spawned at group 3 (home) after 3 ticks ready; on foot ...
tick 1418: bot 250 spawn fallback: group 3 -> 4 (enter-on-spawn)
tick 1421: bot 250 spawned at group 4 (fallback) after 153 ticks ready; in ahe_z10 seat 0
```

Bots 252 and 251 had spawned by fallback; after the restart they spawned at home, which is the
home-return rule working. Run 3 (`allowRoundRestart 0`): `RestartRound: error (6, 'round restart
is disabled (serverext.cfg allowRoundRestart 0)')`.

Engine behaviour seen here: after the restart, five bots spawned on the three pad points in the
same tick (two pairs share a position). The occupancy test does not see soldiers created earlier in
the same spawn pass. Retail behaviour, not a module defect, but the sidecar can see two bots on one
point after a restart.

### Walking (scenario c) and the measured signs

```
PASS EnterVehicle ahe_ah1z 640 m away refused: error (5, 'the vehicle is out of enterRange')
strafe +0.6 for 1 s: +1.27 m right, -0.00 m forward (yaw 150.0)
MEASURED: pedals + strafes RIGHT
look yaw +0.5 for 1 s while walking: yaw +77.5 degrees
MEASURED: look yaw + turns RIGHT (yaw increases)
walking to ahe_ah1z id 1797 at 29.1 m
walk: arrived, 6.9 m from the target after 6.2 s
PASS EnterVehicle 1797 seat 0 after walking: ack None
PASS TickState shows the bot in that vehicle, seat 0
```

77.5° in 30 ticks at 0.5 is 2.6° per tick, matching the Q10 reading of 5° per tick per unit. The
walker: look yaw = clamp(bearing error / 15°), forward 1.0 when the error is under 25° (else 0.3),
stop at 7 m (the heli's body keeps a soldier about 5 m from its root). Look pitch's sign was not
measured: `TickState` reports the soldier root's attitude, which does not pitch with the view.

### Buttons (scenario d)

Bot seated in a Z10, one-tick pulses one second apart (run 4); the module's log:

```
tick 4124: bot 255 camera view mode 1 -> 2     toggle camera
tick 4154: bot 255 camera view mode 2 -> 3     toggle camera
tick 4185: bot 255 camera view mode 3 -> 1     camera mode 1
tick 4215: bot 255 camera view mode 1 -> 3     camera mode 2
tick 4245: bot 255 camera view mode 3 -> 4     camera mode 3
tick 4276: bot 255 camera view mode 4 -> 5     camera mode 4
tick 4307: bot 255 camera view mode 5 -> 1     camera mode 1
after F2: seat 1 / PASS F2 moved the bot to seat 1
```

Fire (run 5): the bot on foot turned to face the empty team-1 Z10 at 17 m (facing error -1.0°) and
held fire for 4 s: `target health 1.0000 -> 0.9600`, and `firing 1` / `firing 0` in the module log.
The first attempt (run 4) missed, because a standing turn moves the head first and the body was
still 54° off; turning while creeping forward fixes that (Q10). Only empty map objects were shot.

### Destroyed vehicles

Run 13: a bot climbed 18 m in a Z10 and was removed; the heli crashed (`VehicleList` flag 1,
health 0.0). `PASS EnterVehicle(wreck 1819) refused: error (5, 'the vehicle is destroyed')`. Run 17:
at connect the team-1 Z10 was a wreck about 9 m from the pad (`flags 1, health 0.0`) and the only
heli in range: `Error req 2 code 5: no undestroyed vehicle with that seat free within enterRange`,
then `Ack req 3` a second later on the respawned heli. Runs 11, 12, 14 and 15 were test-client
tuning: a 10 m drop leaves a Z10 at 0.65, and back-cyclic during a full-collective climb flips it.

### Fuzz (scenario e)

All 41 cases pass (run 6). New for v1: `RestartRound` before `Hello` (2); `Hello` version 0 and 2
(1); v0-sized and one-byte-long `SetInput` (2); short and long `RestartRound` (2); `VehicleList` and
`SpawnBlocked` sent by the client (3); `RestartRound` with `allowRoundRestart 0` (6, connection
kept); NaN look yaw and -inf look pitch (7); every button bit with out-of-range look axes (clamped,
no error); `VehicleList: 3 in 3 s, 4 vehicles`. Then `server still ticking: 91 TickStates in 3 s,
29.8 ticks/s`.

### Map change

Run 17, `exec admin.nextLevel 0` and `exec admin.runNextLevel` over RCON while the bot hovered:

```
10:57:45 bot left the vehicle (alive 0); waiting to re-enter     (end of round)
10:58:11 BotRemoved player 255 reason 2
10:58:13 BotCreated req 4 -> player 255
10:58:15 Ack req 5
10:58:16 VehicleList #53 tick 33081: 4 vehicles, 0 destroyed, 1 occupied
10:58:29 hover reached at height 12.0 m above the pad
```

Server side (run 9): `level unload` (twice), `vehicle set rebuilt: 4 vehicles in 15284 registered
objects`, `bot 255 no longer exists (level unload)`, recreate, re-enter.

### Soak and cost

Run 16 on server process 4, client inside WSL, 660 s (above). `soak-check.sh` once a minute for
11 minutes: process alive, `udp16571=1`, GameSpy answering `Dalian_2_v_2 gpm_cq openplaying`, RSS
142,416 → 142,420 KB, `frontier/abort lines in bf2.log: 0`.

Handler cost (10 µs buckets, upper edges), from the module's stats lines over the 30-minute
process:

| Window | p50 | p99 | max |
|---|---|---|---|
| Soak windows, 1 bot hovering (ticks 12,600-30,600) | 70-80 µs | 110-150 µs | 132-461 µs |
| Whole process (30,600 ticks) | 70 µs | 130 µs | 2,833 µs (0 ticks over 20 ms) |
| First window (includes the full vehicle walk at load) | 70 µs | 330 µs | 2,522 µs |

The p50 rose from v0's 20 µs because of the 500-object slice scan per tick. The 2-3 ms maxima are
ticks that create a bot (`createInputPlayer` plus the group listing); windows without a `CreateBot`
peak at 132-461 µs. `EnterVehicle(0)`, 3-5 ms in v0, is now a walk of the 4-6 entry vehicle set.

## Contract points that proved wrong or ambiguous

`docs/protocol.md` was not edited. Each item names the reading implemented.

1. **On foot, pedals strafe; they do not turn.** `PIYaw` is the soldier's strafe axis; turning is
   look yaw only, and cyclic pitch and roll do nothing on foot (Q10). Measured: pedals + = strafe
   right, look yaw + = turn right (yaw increases). Look pitch's sign is unmeasured.
2. **Look units:** degrees per tick times `phy-soldier-look-factor` (5), not a rate; -1..1 allows up
   to 150°/s. Standing still, the first 40° turn only the head. Implemented: -1..1, clamped and
   quantized like the other axes (a human's look value can exceed 1; widening is a contract call).
3. **Quantization is not engine-imposed for bots.** AI input bypasses the wire codec
   (`GameServer.cpp:3512`); the module quantizes by contract choice (v0), now for all six axes.
4. **"Vehicle" must exclude cameras.** Every player's `MultiPlayerFreeCamera` is a root
   `IPlayerControlObject`, not a soldier; it appeared in the first `VehicleList` with an occupied
   seat. The module now excludes `ICameraObject` roots everywhere (list, both `EnterVehicle` forms,
   `TickState`'s in-vehicle test). v0's nearest-vehicle search had the same hole.
5. **`VehicleList` reports pooled vehicle objects.** The engine reuses vehicle objects: a cleared
   wreck, and an object waiting to be respawned, sit at position 0/0/0 with **no armour component**
   (health -1, flags 0, so not "destroyed") until the spawner places them again (Z10s 1819 and 2065
   went from 0/0/0 back to the pad). During a level load all four helis are briefly in that state.
   Not filtered, because "no armour" is also a legitimate state in the contract. Proposal: either
   the contract says "health -1 at 0/0/0 is a parked object; ignore it", or the module skips
   entries without an armour component (and the contract drops the -1 case for vehicles).
6. **Vehicle team is 0 for every empty heli** on dalian, including the ones on each base's pad, so
   `team` does not tell the sidecar whose heli it is; only position does.
7. **The fallback needed one more engine call than the contract named.** `getGameStatus()` is
   appended so that the timers run only while the engine would spawn (status 1, respawn delay
   over). Without it, the end of every round would look like a blocked spawn.
8. **Home group:** "the first `findSpawnGroup` result" requires an active group, so a blocked pad
   at `CreateBot` would make another group home. Implemented: home = the lowest-id usable on-foot
   group, active or not, which equals `findSpawnGroup` whenever nothing is blocked. "Usable" (not a
   squad group, not human-only, at least one point) also defines when `CreateBot` gets `Error` 6.
9. **Mixed groups** (some enter-on-spawn points) are ordered with the enter-on-spawn groups, since
   the engine picks a random active point.
10. **"Every group tried":** a group that is inactive when its turn comes counts as tried, so on
    dalian `SpawnBlocked` needs one fallback period plus `spawnBlockedSeconds`.
11. **`RestartRound` rate limit** counts successful restarts only; a refusal does not start the
    60 s window.
12. **Buttons and latest-wins:** buttons travel with the axes, so a one-tick press in a `SetInput`
    that is superseded within the same tick is lost, and a held button must be resent every tick.
    `PIToggleCameraMode` cycles every tick it is held.
13. **No exit-vehicle button exists** under the contract's rule: the only exit key is `PIUse`,
    whose enter half reaches the AI `BFfindEntryPoint` path. Seat change (F1-F8) is in the table and
    stays inside the vehicle.
14. **Look axes in a vehicle** are ignored unless `PIMouseLook` is held, which zeroes the cyclic;
    `PIMouseLook` is therefore not in the table, and the pilot's look axes do nothing.
15. **The spec's "bits not in the table are logged once" cannot trigger**: the proposed table uses
    all 32 bits.
16. **Simultaneous spawns can stack:** after a restart several bots spawned on the same pad point
    in one tick (engine behaviour; see scenario b).

## What is left

- **Enemy-perception and client paths:** no game client joined; Q4 and Q9 are unchanged.
- **Look pitch sign** and the gunner-seat look slots (per-vehicle `.tweak` content) are unmeasured.
- **Flying a fallback-spawned bot** was not tested; only its seating and survival.
- **Pooled-object entries** in `VehicleList` (contract point 5) await a decision.
- **Q5, Q8**, the Windows build and the Docker host are as in module-v0.md.

## Remaining frontiers relevant to this change

- No `[FRONTIER]` line was printed in any run (counts above).
- `ServerGameLogic::restartMap` calls `AutoRecorder::roundFinished`, marked `[FRONTIER] 0x80A05C0`,
  only when `getAutoRecorder()` is non-null; in this tree it returns 0 (`GameServer.cpp:1928-1931`),
  so `restartRound` does not reach it. It ran four times without a frontier
  line.
- `SpawnGroup::updateStateMask/getNetUpdate/setNetUpdate` are `[FRONTIER]`; `listSpawnGroups` does
  not call them.
