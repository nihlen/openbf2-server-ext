# openbf2-server-ext protocol

Status: **v0, approved by the author on 2026-10-22, implemented** (`docs/findings/module-v0.md`). The open questions
are still open; answers are collected in `docs/findings/` and folded in here by the coordinator.
Change it only through the process in [Changing this contract](#changing-this-contract).

This is the single contract between three pieces:

| Piece | Repo | Role |
|---|---|---|
| Engine extension module | `openbf2-server-ext` (this repo) | Runs inside the OpenBF2 dedicated server. Owns the socket, the per-tick snapshot, the input queue and the watchdog. |
| Engine changes | `OpenBF2`, local branch | The few things the module cannot do from outside (see [Engine requirements](#engine-requirements)). |
| Bot sidecar | `bf2-bots` | Separate process on the same host. Reads state, decides, sends inputs. |
| Admin client | `BF2WebAdmin` | Central. Uses the admin channel (v1, not specified yet). |

v0 covers only what a single bot needs to hover: create a bot, put it in a heli, stream its state,
and accept the four flight axes. Everything else is listed under [Later](#later).

## Loading

- Built as `libserverext.so` (Linux x86-64), and later `serverext.dll` (Windows x86 and x64). The
  strict `bf2_w64ded` link is broken at OpenBF2 HEAD (VoIP `.def`), so x64 Windows cannot be
  validated yet.
- `bf2` exports no symbols. The module can only make typed virtual calls on engine interfaces and
  use inline header code. Non-virtual engine functions such as `world::getRootParent` are out of
  reach; walk `IObject::getParent()` instead.
- Loaded by this line in `mods/bf2/openbf2.con`, which `GameServer::init` runs before Python and
  ModManager initialise (`GameServer.cpp:262-279`). The netsky content has no such file, so create it:

  ```
  openbf2.addModDll serverext
  ```

- The module file goes in `bin/amd-64/` beside `bf2`, because `dlopen` searches `$ORIGIN` and
  `LD_LIBRARY_PATH`, not the working directory (`OpenBF2Console.cpp:110-139`).
- Exports `initDll(IClassManager*)`. The loader unloads the module only if `initDll` returns false
  (`OpenBF2Console.cpp:239-242`). There is no unload path after that, so a loaded module lives as
  long as the process.
- Settings come from `serverext.cfg` in the server's working directory:

  | Key | Default | Meaning |
  |---|---|---|
  | `botListen` | `127.0.0.1:16700` | Bot channel endpoint. Must never be 16567. Keep it on loopback, except inside a container, where it binds `0.0.0.0` and Docker publishes it as `127.0.0.1:16700` only. |
  | `watchdogTicks` | `6` | Ticks without input before a bot's input is neutralised (6 = 200 ms). 1 to 3000. |

  `key value` or `key = value`; `#` and `//` start comments. A missing file means defaults. A bad
  value makes `initDll` return false, so the module refuses to load instead of listening somewhere
  unexpected.

## Threading rules

These rules are part of the contract, because breaking them causes server hitches or crashes.

1. The server runs a single-threaded loop with a fixed 30 Hz simulation tick
   (`TickCalculator(10, 30.0f)`, `OpenBF2/Code/BF2/Game/Main/BF2Engine.cpp:418`).
2. The module's socket I/O runs on its own thread. That thread never calls into the engine or Python.
   It only moves bytes into and out of queues.
3. Engine work happens on the main thread, once per tick:
   - drain the inbound queue and apply commands and inputs;
   - build one state snapshot and push it to the outbound queue.
4. No blocking calls on the main thread: no socket writes, no disk I/O, no waiting on the sidecar.
   If the sidecar is slow, the server keeps ticking and the watchdog applies.

### The per-tick hook (Q1, answered)

No engine change is needed. Details and evidence: `docs/findings/engine-q1-q4.md`, Q1.

- In `initDll`, get the `"EventManager"` singleton. Register an `IEventListener` on category 7, which
  nothing in the tree uses, then post one `postEventNextUpdate(7, …)`. The handler does the tick's
  work and posts the next event.
- The handler runs exactly once per simulation tick, inside `GameServer::simulateFrame`, after
  `gameLogic->update` and before `simulatePlayersUpdate` (`GameServer.cpp:2522-2546`).
- Inputs written in the handler take effect in the same tick. The snapshot taken there is the end of
  the previous tick, and `TickState` is numbered accordingly. The module keeps its own tick counter.
- Do not use `FrameEventManager`: it runs once per frame, which can be 0 to 10 ticks, and after the
  network send.
- Category 7 is `[CHOSEN]` by the module. Whether unrecovered retail code uses it is unverified (Q8).
  If it turns out to be taken, the fallback is a one-line hook in `simulateFrame` (R0).

## Transport and framing

- TCP on `botListen`. One sidecar connection at a time in v0. A second connection is accepted and
  closed at once, with no frame.
- Every message is a frame:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | `u32` | payload length in bytes (excludes this header) |
  | 4 | `u16` | message type |
  | 6 | `u16` | reserved, 0 |
  | 8 | bytes | payload |

- All integers and floats are little-endian. Floats are IEEE-754 `f32` unless stated. Strings are
  `u16` byte length followed by UTF-8 bytes, no terminator.
- Maximum frame payload: 64 KiB. A larger length closes the connection.
- Any framing or parse error (bad length, nonzero reserved field, wrong payload length, anything
  before `Hello`, a second `Hello`) is `Error` 2, then close. An unknown type, including one sent in
  the wrong direction, is `Error` 3, then close. Per-request errors (codes 4 to 7) keep the
  connection. The module must survive malformed input without affecting the server.

## Conventions

These match what `bf2-bots` already uses (`bf2-bots/src/Bf2Bots.Game/LocalState.cs`) so the
controller code is reused unchanged:

- World axes: x east, y up, z north. Metres, metres per second.
- Angles in degrees: yaw 0 = north, 90 = east; pitch positive = nose down; roll positive = bank left.
- Angular velocity: rad/s in world axes; positive about y turns the nose east.
- These are already the engine's own conventions (Q2, answered; `docs/findings/engine-q1-q4.md`).
  The module reads the root vehicle object (walk `getParent()` to the top), not the seat. Its
  transform `m` is from `IObject::getAbsoluteTransformation()` (row-major, rows right/up/forward/position),
  and its physics node is from `queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`.
  `queryInterface(IID_IPhysicsNode)` returns null; verified at runtime (`docs/findings/r1-smoke.md`):

  | Field | Source |
  |---|---|
  | x, y, z | `m[12], m[13], m[14]` |
  | yaw | `atan2(m[8], m[10])`, degrees |
  | pitch | `-asin(clamp(m[9], -1, 1))`, degrees |
  | roll | `atan2(m[1], m[5])`, degrees |
  | velocity | `getPositionalSpeed()`, unchanged |
  | angular velocity | `getRotationalSpeed()`, unchanged |

- This is the body attitude. The bf2-bots physics port logs the pilot camera, which is pitched 4°
  nose down, so the sidecar must not apply that camera correction to `TickState`.

## Messages

### Handshake

The sidecar connects and sends `Hello` first. The module answers `Welcome` or `Error` and closes.

**`0x0001 Hello`** (sidecar → module)

| Type | Field |
|---|---|
| `u16` | protocol version, `0` for this draft |
| string | client name, for logs |

**`0x0002 Welcome`** (module → sidecar)

| Type | Field |
|---|---|
| `u16` | protocol version |
| `f32` | tick rate in Hz (30) |
| `u32` | current tick number |
| string | map name |
| string | game mode |

A version mismatch is an `Error` followed by a close. There is no negotiation in v0.

### Bots

**`0x0101 CreateBot`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id, echoed in the reply |
| string | name; the module does not add a prefix, so the sidecar sends `[BOT] ...` itself |
| `u8` | team, 1 or 2 |

Reply: `BotCreated` or `Error`. Team other than 1 or 2, or an empty name, is `Error` 7. The name has
control bytes replaced and is capped at 64 bytes. `Error` 6 means no spawn group is available right
now, for example during a level load or briefly after a bot was destroyed; the sidecar retries
after a couple of seconds.

The module creates the player (R1), then sets its team, a valid kit and its team's on-foot spawn
group (`IServerExtension::findSpawnGroup`, R1). The engine spawns it on the pad on a later tick,
once a round is running; no human needs to be present. `TickState`'s alive flag shows when that has happened. Never use the in-heli
enter-on-spawn group as the home group; in v1 it is only a fallback (see Blocked spawns).

A bot that dies, including at the end of every round, stays a bot: the engine respawns it on the
pad without a `BotRemoved`. `TickState` shows it alive and on foot, and the sidecar sends
`EnterVehicle` again. Only a map change (unload) removes it.

**`0x0102 BotCreated`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id |

**`0x0103 EnterVehicle`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (must be a bot created on this connection) |
| `u32` | vehicle id: a networkable id as in `TickState`, or 0 for the nearest vehicle within 30 m whose seat is free |
| `u8` | seat index, 0 = pilot |

Reply: `Ack` or `Error`. The bot must be alive and on foot (`Error` 6 otherwise). Vehicle and seat
problems are `Error` 5. "Vehicle" means any root object with a seat that is not a soldier. The
search for id 0 walks every registered object and costs about 4 ms on that tick, so the sidecar
should not send it every tick. The module checks that the seat is not
occupied and that the vehicle's team is 0 or the bot's, because `IGame::enterVehicle` checks
neither, then calls `IGame::enterVehicle(player, seat, false)` (`Game/IGame.h:126`). On
`dalian_2_v_2` the pad spawn points are about 14 m from the base's two helis.

**`0x0104 RemoveBot`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id |

Reply: `Ack` or `Error`. The module also removes every bot it created when the sidecar
disconnects. On a map change the engine destroys every AI-flagged player itself
(`GameServer.cpp:1416-1429`). The module then drops their ids without destroying them again, and
the sidecar recreates them.

**`0x0105 Ack`** (module → sidecar): `u32` request id.

**`0x0106 BotRemoved`** (module → sidecar), sent whenever one of this connection's bots stops existing.

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | reason: 1 `RemoveBot`, 2 map change, 3 vanished without an unload (no known admin path causes it) |

### Input

**`0x0201 SetInput`** (sidecar → module)

| Type | Field | Engine slot |
|---|---|---|
| `u8` | player id | |
| `u32` | the tick number this input was computed from | |
| `f32` | cyclic pitch, -1..1 | `PIPitch` = 1 |
| `f32` | cyclic roll, -1..1 | `PIRoll` = 2 |
| `f32` | collective, -1..1 | `PIThrottle` = 3 |
| `f32` | pedals, -1..1 | `PIYaw` = 0 |
| `u32` | button bitmask, 0 in v0 (reserved for fire, alt fire, seat and camera keys) | |

- Slot numbers are from `OpenBF2/Code/BF2/IO/Input/Input.h:198`. The axis assignment matches the
  ControlMap the bots use today (`c_PIPitch`, `c_PIRoll`, `c_PIThrottle`, `c_PIYaw`).
- The module writes these values into the bot's `io::PlayerInput`, which the server reads for AI
  players every tick (`OpenBF2/Code/BF2/Game/GameServer/GameServer.cpp:3363`).
- The module must also set the slots' bits in `PlayerInput::mappingFlags`, using the same mask
  `PlayerAction::get` produces (`PlayerAction.cpp:172-203`). The AI buffer starts with no bits set
  (`Player.cpp:4052`), and vehicle code reads an unmapped slot as 0.
- Latest wins: if several `SetInput` frames for one bot arrive within a tick, only the last is used.
  Every tick the module zeroes all 64 slots, sets the mask and writes the four axes. A nonzero
  button bitmask is ignored and logged once in v0.
- Values outside -1..1 are clamped. NaN or infinity is rejected with an `Error` and the input is
  not applied.
- After clamping, the module quantizes each axis exactly as a human's input is quantized on the way
  to the server: `convertAxisToInternalResolution`, then `short(int(v * 100))`, then `/ 100`
  (`PlayerAction.cpp:27-112`). That truncates twice, so for example 0.57 arrives as 0.56. The
  bf2-bots physics port uses the same chain.
- Q4, mostly answered: from the server's `PlayerInput` on, bot and human input run the same code.
  Still open: whether the client's ControlMap stage (deadzone, scale, inversion) matches the bots'
  fixed profile. Settle it with a scripted manoeuvre on both paths.

### State

**`0x0301 TickState`** (module → sidecar), sent once per simulation tick.

| Type | Field |
|---|---|
| `u32` | tick number: the module's tick N sends the state at the end of tick N-1, numbered N-1. `Welcome` carries N. `SetInput` echoes the `TickState` number it was computed from. |
| `f64` | server simulation time, seconds |
| `u8` | number of entries N |
| N × entry | one per bot created on this connection, then one per other player in a vehicle |

Entry:

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | flags: bit 0 alive, bit 1 is a bot from this connection, bit 2 in a vehicle |
| `u8` | team |
| `u8` | seat index, 255 if on foot |
| `u32` | root object's networkable id (vehicle if in one, else soldier) |
| 3 × `f32` | position x, y, z |
| 3 × `f32` | yaw, pitch, roll |
| 3 × `f32` | velocity x, y, z |
| 3 × `f32` | angular velocity x, y, z |
| `f32` | vehicle health, 0..1, or -1 if on foot or the vehicle has no armour component |

- A bot of this connection that exists but is dead still has an entry, with the alive flag clear,
  position 0/0/0 and health -1. Ignore anything but its flags.
- Enemy entries are raw. The bot's perception filter still decides what the bot may know; the
  sidecar must apply it before anything reaches the decision layer.
- If the sidecar falls behind, the module drops old `TickState` frames rather than queueing them,
  and the gap is visible in the tick numbers.

### Errors

**`0xFFFF Error`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | request id, 0 if none |
| `u16` | code |
| string | message for logs |

Codes: 1 version mismatch, 2 malformed frame, 3 unknown message type, 4 unknown or foreign player,
5 vehicle or seat unavailable, 6 engine refused, 7 invalid value.

## Watchdog

- If a bot gets no `SetInput` for `watchdogTicks` consecutive ticks, the module neutralises its
  input and logs it once.
- Neutral in v0: in a vehicle, cyclic and pedals 0 and collective held at its last value so the
  heli does not drop. On foot, every axis 0. On death the stored axes are zeroed, so a respawned bot
  stands still until the sidecar sends input.
- Q5, answered (`docs/findings/q5-q9.md`): holding collective is safe only from a hover. On the Z-10 it
  crashed the heli within about 10 s from a full-collective climb, a steep descent and a 32° banked
  turn, because zero cyclic holds the current attitude. Proposed, not approved: in a heli, level the
  attitude (P-D on pitch and roll, pedals damp yaw rate) and hold vertical speed at 0 (PI on
  collective). All five tested states survived with it. It is a module change with no wire change.
- On disconnect, bots are neutralised immediately and removed after the watchdog period, without
  `BotRemoved`.

## Map-change detection

`GameServer::unload` posts `(ECHud, 3)` before destroying AI players. The module listens on `ECHud`
for it and ignores every other id. On the server only the AIDLL listens on that category, so
those events are now dispatched to the module instead of dropped. If a bot disappears with an
unload seen, `BotRemoved` reason is 2; with no unload for two ticks, reason 3.

## Engine requirements

Changes needed in the `OpenBF2` local branch. Keep them as small as possible; everything else
belongs in the module. Each is non-retail behaviour, marked `[CHOSEN, not recovered]`. OpenBF2's
`AGENTS.md` wants a linked issue discussing the approach before such a change. The author has
decided this work stays local with no GitHub issues, so these sections are the design record. An
issue is needed only before anything goes upstream.

**R0 (fallback only). Tick hook.** Not needed unless EventManager category 7 turns out to be taken
(Q8). It would be one hook in `GameServer::simulateFrame`, between `processEvents` and
`simulatePlayersUpdate` (`GameServer.cpp:2530-2531`), registered through the R1 interface.

**R1. Create a bot player without the AIDLL.** `IGameServer::createPlayer(name, true)` refuses when
no AI library is loaded (`GameServer.cpp:371`; the function starts at `:365`), and the rebuilt
server never loads one. With `false` the new player becomes the server's local human. Registering
a fake `"AI"` singleton is rejected: `g_aiInstance` is referenced about 50 times in 23 engine files
outside `AI/`, and Python's `getIsAIGame()` would report true. Design:

- A new header-only interface, `IServerExtension`, with `virtual int createInputPlayer(const
  std::string& name) = 0` returning the player id or -1. `GameServer` implements it and registers
  it as the singleton `"OpenBF2ServerExtension"` before `openbf2.con` runs. The module reaches it with
  `getSingleton` and typed virtual calls. No retail vtable changes.
- The code after the gate in `createPlayer` (`:378-425`) moves into a private helper.
  `createPlayer(name, true)` keeps its gate and calls the helper, so retail behaviour is unchanged;
  `createInputPlayer` calls the helper without the gate.
- `virtual int findSpawnGroup(int team, bool enterOnSpawn) = 0` returns the lowest id of an active
  spawn group that team can use whose spawn points all match `enterOnSpawn` (counting
  `getAIEnterOnSpawn` as enter-on-spawn), or -1. It is built from
  `SpawnManager::getGroupsForTeam` and `ISpawnGroup::getActive/getSpawnPoints`. It is needed because
  `spawnManager` is not a registered singleton, and `ISpawnPoint` exposes neither team nor control
  point. Walking spawn points needs a distance heuristic, which failed on `daqing_2_v_2`. Append it
  after `createInputPlayer`; the interface is append-only.
- The same interface carries anything else the module later needs from `gameLogic` or
  `spawnManager`, since neither is a registered singleton.
- Implemented and tested on OpenBF2 branch `serverext/r1`, uncommitted (`docs/findings/r1-smoke.md`,
  `docs/findings/module-v0.md`).

**R1b. Null guards for the AI couriers.** These are required, because R1 alone can crash the server.
An AI-flagged player reaches three courier pointers that only the AIDLL fills. Each guard behaves
as if no vehicle or target was found:

| Site | Trigger |
|---|---|
| `PlayerControlObjectHelpers.cpp:327-329` | Enter-on-spawn points (the 2v2 in-heli spawn) and the bot pressing `PIUse` |
| `GameServer.cpp:3340` | An enemy missile lock on a vehicle the bot is in. The module cannot prevent this. |
| `ServerGameLogic.cpp:3411-3415` | A defibrillator used on a non-soldier. Not reachable in v0. |

**R2. Getting the bot into a heli seat.** No engine change is needed beyond R1/R1b; the route is
in `CreateBot` and `EnterVehicle` above. The AI/human spawn flags (`ServerGameLogic.cpp:644-657`) and
the UseAI first-wave refusal (`:1930-1938`, `gpm_coop` only) do not block a bot on the 2v2 layer.
A human teammate pressing E prefers an AI-occupied seat and can evict the bot
(`PlayerControlObjectHelpers.cpp:456-466`), as with retail bots.

**R3 (later). Stepped time** for training: advance N ticks on command without the wall-clock wait
and the 10-tick catch-up cap. Not part of v0.

### How these bots differ from retail AIDLL bots

- They spawn without any human present: `m_roundGoing` is set before the begin-round wait for
  `sv.numPlayersNeededToStart` humans (`ServerGameLogic.cpp:423`, `:452`). But with only bots the
  round never reaches Running: no round timer, no tickets, no scoring, no stats, and no Python
  handlers, timers or triggers except `alwaysCall` ones. Bots never count as humans, and in this
  tree `ServerSettings::getNumPlayersNeededToStart` returns a hard-coded 2
  (`ServerSettings.cpp:333-342`, a `[CHOSEN, not recovered]` floor), so setting it to 0 has no
  effect. Two connected humans are needed for a real round.
- `getIsAIGame()` is 0. Python sees `ai=1`, `remote=1`, `connected=0`, profile 0, address `N/A`,
  as for a retail bot. Q9, answered in `docs/findings/q5-q9.md`:
  - No bot-specific Python exception in any ModManager module.
  - Python `PlayerConnect` runs inside `createInputPlayer`, before the module sets the team, so
    mm_autobalance and mm_webadmin first see the bot on team 2. No `PlayerChangeTeams` follows.
  - A crash (death with no attacker) raises in `mm_webadmin.onPlayerKilled` and is never reported.
    That is not bot-specific.
  - Kick and ban do nothing to a bot. A ModManager ban writes a permanent ban on address `N/A`.
    So `BotRemoved` reason 3 cannot come from an admin.
  - An admin team switch (`bf2cc switchplayer`, `!sw`, BF2WebAdmin `team`) is followed: the module
    re-resolves the bot's spawn groups for the new team and checks `EnterVehicle` against the live
    team (fixed in `b912c22`). `TickState`'s team is authoritative; the sidecar must handle a change.
  - After a kill, the alive flag stays set for `sv.manDownTime` (2 s), because the soldier is
    man-down.
- GameSpy reports `AIPlayer=1` per bot but `hasBots=0`.
- They get human weapon deviation and none of the AIDLL's squad, heal or ammo notifications.

## Testing

Setup, scripts and evidence: `docs/findings/test-env.md` and `testenv/`. Verified on 2026-10-22 up
to an empty server ticking on `dalian_2_v_2` for over 5 minutes, in both WSL and Docker, with no
`[FRONTIER]` hits. No client has joined yet.

### Map and content

- Test on `dalian_2_v_2` (`gpm_cq`, layer 16) with the production content from
  `cr.nihlen.net/library/bf2-netsky:2v2-nocheck`.
- Bot behaviour, spawn points and heli physics are only meaningful on that map and content.
  A different map proves the protocol, not the bot.
- Changes from production: `sv.internet 0`, `sv.punkBuster 0`, `mm_webadmin` pointed at an unused
  port (`127.0.0.1:4303`), and the server started with `start.sh`, never `start_bf2hub.sh`, which
  preloads the retail `libbf2hub.so`.
- Bots spawn and fly with no human, but the round only runs with two connected humans (see "How
  these bots differ"). Anything that needs round scoring or tickets needs two game clients.

### Where to run the server

- **WSL (default):** `testenv/build-linux.sh`, `prepare-content.sh`, `run-wsl.sh`. One build
  serves both `bf2` and `libserverext.so`. The bot channel on WSL loopback is reachable from
  Windows at `127.0.0.1` through TCP localhost forwarding.
- **Docker (production shape), verified for v1** (`docs/findings/docker-v1.md`): `testenv/build-bullseye.sh`
  builds `bf2`, `libdice_py.so` and the module with Debian 11's toolchain; `run-docker.sh build` bakes
  the module, `openbf2.con` and `serverext.cfg` into the image. In Docker the module logs to
  `docker logs` (stderr); `/volume/bf2.log` holds only the status screen and is deleted with the
  container. Image and container
  `serverext-testenv`, FROM the netsky image. The image has glibc 2.31, so `bf2`, `libdice_py.so`
  and `libserverext.so` must be built in Debian 11; the WSL build will not run there. Set
  `PUBLISH_IP=127.0.0.1` to keep it off the LAN.
- **A Windows client joining (Q6, answered):** WSL forwards TCP only, not UDP. Join the WSL server
  at the VM's address (`wsl -d Ubuntu hostname -I`) on 16571, or the Docker server at
  `127.0.0.1:16571`. Do not switch WSL to mirrored networking: that restarts Docker Desktop.

### Ports

- Port 16567 is reserved for the author's sessions.
- The BF2WebAdmin local test environment uses `16569/udp`, gamespy `29901/udp`, RCON
  `127.0.0.1:4721` and WebAdmin `4300`. Do not reuse those either.
- This project: game `16571/udp`, gamespy `29903/udp`, RCON `127.0.0.1:4723`, bot channel an unused
  port in `127.0.0.1:16700-16709`. On the author's machine 16700 is held by a stale `wslrelay`, so
  WSL runs use 16701 and Docker uses 16702. The server also binds VoIP UDP 55124 and 55126, so two WSL test servers at
  once collide.

### Checks

- This repo contains a small test client that speaks the protocol (connect, create a bot, enter the
  heli, send fixed inputs, print states). It must pass against a running server before either C#
  repo depends on a change.
- The end-to-end check: start the local OpenBF2 server with the module on `dalian_2_v_2`, start
  the bot sidecar, and confirm a bot spawns, enters the heli and holds a hover for 60 seconds.

## v1 (approved and implemented 2026-10-22)

Implemented and tested: `docs/findings/module-v1.md` (OpenBF2 `540fa88`, module `64a7bb3`).
Changes on top of v0. The protocol version becomes 1, and the module and sidecar move together:
the module accepts only version 1. Sources: the bf2-bots sidecar's findings
(`bf2-bots/docs/serverext-backend.md`) and the author's guidance on blocked spawns.

### Vehicles

**`0x0302 VehicleList`** (module → sidecar), sent on the first tick after `Welcome` and then once a
second (every 30 ticks), after that tick's `TickState`.

| Type | Field |
|---|---|
| `u32` | tick number, as in `TickState` |
| `u16` | number of entries N |
| N × entry | one per vehicle on the map: a root object with seats that is not a soldier |

Entry:

| Type | Field |
|---|---|
| `u32` | networkable id, as in `TickState` and `EnterVehicle` |
| string | template name, for example `ahe_ah1z` |
| `u8` | team, 0 if neutral or empty |
| `u8` | flags: bit 0 destroyed (hit points at or below 0) |
| 3 × `f32` | position x, y, z |
| `f32` | yaw, degrees |
| `f32` | health, 0..1, or -1 with no armour component |
| `u8` | number of seats |
| `u32` | bitmask of occupied seats, bit 0 = seat 0 (pilot) |

- "Vehicle" excludes cameras: every player's free camera is a root seat object too. This applies to
  `VehicleList`, both forms of `EnterVehicle` and the in-vehicle flag in `TickState`.
- The engine pools vehicle objects. A cleared wreck, or a heli waiting for its spawner, sits at
  0/0/0 with no armour component (health -1, not flagged destroyed) until it is placed again. All
  helis are in that state briefly during a level load. The sidecar ignores entries with health -1.
  Each crash and respawn can leave another pooled entry, and a base's heli can come back under a
  different id, so an id is only good for one pick.
- Damaged helis are not destroyed and can be entered. A minimum health is sidecar policy.
- Team is 0 for every empty heli, including those on a base's pad, so only position tells whose
  heli it is.
- This is the sidecar's only source for a vehicle's type and for empty vehicles. Enemy vehicles
  are raw, like enemy `TickState` entries, and go through the perception filter.
- The module keeps its own vehicle set: a full walk at load, then 500 registered objects per tick,
  so new vehicles appear within about a second. Per-tick cost is p50 70-80 µs and p99 110-150 µs;
  only ticks that create a bot reach 2-3 ms.

### Entering vehicles

- Both forms of `EnterVehicle` skip destroyed vehicles: id 0 never picks one, and an explicit id is
  `Error` 5.
- Both forms require the bot within `enterRange` metres of the vehicle (`serverext.cfg`, default
  30). A bot that spawned elsewhere walks there first (see On foot).

### Blocked spawns

A wreck, or a healthy heli out of place, can block a spawn group's points. Usually another of
the team's points is free, and the bot can spawn there and walk to the heli. If every point is
blocked, the round has to be restarted.

- `CreateBot` succeeds whenever the team has any spawn group, active or not. `Error` 6 now only
  means the team has none, as during a level load.
- Tested on `dalian_2_v_2`: with the pad full, a bot falls back to the in-heli point after 5 s and is
  seated in the nearest free seat through the R1b-guarded path. With every group blocked,
  `SpawnBlocked` follows and `RestartRound` clears it.
- Timers run only while the round is playing and the bot's respawn delay is over
  (`IServerExtension::getGameStatus`), so an end-of-round wait never counts as blocked.
- After a round restart, the engine can spawn two bots on one pad point in the same tick. That is
  retail behaviour.
- The module owns the fallback. If a bot waiting to spawn has not spawned within
  `spawnFallbackSeconds` (default 5) of its group becoming its target, the module moves it to the
  team's next active group, nearest to the original group's points first, and cycles through them.
  Groups without enter-on-spawn come first. The team's enter-on-spawn groups come last: on
  `dalian_2_v_2` each base has only the pad group and the in-heli point, so the in-heli point is
  the only fallback there. For an AI-flagged bot, that point takes the R1b-guarded
  `BFfindEntryPoint` path and puts it in the nearest free seat within 15 m, as a human spawning
  there would be. That path is untested. The v1 module worker must exercise it first. If it
  crashes or misbehaves, enter-on-spawn groups stay excluded and the worker reports why. After a successful spawn the bot's target goes back to its home group
  (the first `findSpawnGroup` result), so the next death tries the pad first.
- **`0x0107 SpawnBlocked`** (module → sidecar): `u8` player id, `u16` seconds waiting. Sent when a
  bot has not spawned for `spawnBlockedSeconds` (default 15) with every group tried, then every 10 s
  while still waiting.
- **`0x0108 RestartRound`** (sidecar → module): `u32` request id. Restarts the round as
  `admin.restartMap` does. It is allowed only if `serverext.cfg` has `allowRoundRestart 1` (default
  0), and at most once per 60 s. Otherwise it is `Error` 6. Reply: `Ack`. The sidecar sends it only
  after `SpawnBlocked`, never on its own initiative.

### On foot

- `SetInput` has two more `f32` fields after pedals: look yaw (`PIMouseLookX` = 4) and look pitch
  (`PIMouseLookY` = 5), -1..1, validated, clamped and quantized like the other axes. The message is
  33 bytes.
- On foot (Q10, answered, `module-v1.md`):

  | Field | Soldier effect | Sign (measured) |
  |---|---|---|
  | collective | walk forward and back | + forward |
  | pedals | strafe | + right |
  | cyclic pitch, roll | nothing | |
  | look yaw | turn, about 5° per tick per unit (150°/s at 1.0) | + right (yaw increases) |
  | look pitch | look up and down, clamped to ±85° | not measured |

- Standing still, the first 40° of a turn moves only the head, and the body follows slowly. Turn
  while creeping forward to point the body.
- In a pilot seat the look axes are ignored.
- Quantization is the contract's choice for human parity. Bot input bypasses the engine's wire
  codec (`GameServer.cpp:3512`).

### Buttons

`SetInput`'s bitmask is live. A set bit presses the button for the one tick in which that
`SetInput` is applied; a tick without a new `SetInput` presses nothing. Q11 is answered, with
evidence in `module-v1.md`.

| Bit | Button | Notes |
|---|---|---|
| 0 | fire (`PIFire`) | held while set |
| 1 | alt fire | |
| 2 | flares | |
| 3 | jump (`PIAction`) | on foot |
| 4 | sprint | |
| 5 | crouch | while held |
| 6 | prone | toggles |
| 7 | reload | |
| 8 | cycle camera view | cycles every tick while held, so send it for one tick |
| 9-12 | camera views 1-4 | 10-12 are idempotent. 9 is not: from view 1 it selects the nose cam when the server allows it. To reach view 1 from any view, press 10, then 9. |
| 13-21 | weapon select 1-9 | gas mask and night vision flip every tick while held |
| 22 | next weapon | |
| 23 | fire mode | |
| 24-31 | change to seat 1-8 (F1-F8) | never into an occupied seat |

- There is no exit-vehicle button. The only exit key is `PIUse`, whose enter half takes the
  untested AI courier path, so it stays excluded. `PIMouseLook` is excluded too: holding it in a
  pilot seat zeroes the cyclic.

### Engine

- `IServerExtension` gained, appended: `listSpawnGroups(team, SpawnGroupInfo*, capacity)`,
  `restartRound()` (what `admin.restartMap` runs) and `getGameStatus()`. OpenBF2 branch
  `serverext/r1`, commit `540fa88`.

## v2 (approved 2026-10-22, not implemented yet)

Approved by the author on 2026-10-22, all items. Sources: the v1 sidecar (`bf2-bots/docs/serverext-backend.md`),
the play mission and `docs/findings/q5-q9.md` (man-down flag). The damage event uses the same engine
damage observer (R5) as the admin channel draft.

1. Each own bot's camera view mode in `TickState`, so the sidecar can verify a view change. Today
   it is visible only in the module's log, and latest-wins can drop a one-tick press.
2. `enterRange` in `Welcome`, so the sidecar doesn't have to be told it.
3. More than one connection, or a way to hand bots between connections. Test setups that need a
   second client currently run everything on one connection.
4. Weapon state per own bot in `TickState`: the selected weapon, rounds loaded, magazines and
   reloading. Today the sidecar counts its own shots against the tweak files.
5. Projectiles near own bots, at least missiles in flight: position, velocity, owner team and
   template. These are raw and must go through the perception filter. They are needed for dodging,
   for enemy TV-missile timing, and for a gunner bot that guides a TV missile.
6. Spotting: a spot button or message, and the team's spotted state of enemies.
7. Round start: chat lines for this connection, or a round-state field. The 2v2 rules start on the
   gunners' "rdy"/"go".
8. Damage events (victim, attacker, weapon, amount), filtered to what a player would see (hit
   markers, kill messages).
9. Teleport, for test drills on test servers only, behind a `serverext.cfg` switch.
10. A man-down bit in `TickState` entry flags: the alive flag stays set for `sv.manDownTime` after a kill.

Items 4-8 come from the play mission (`bf2-bots/docs/serverext-missions.md`). Items 4 and 5 unblock TV
fights and a gunner bot. Item 8 overlaps the admin channel's events (`docs/admin-channel-draft.md`).

## Later

- v1 admin channel for BF2WebAdmin: the existing `mm_webadmin` events and commands, carried by the
  module instead of Python. Separate endpoint and authentication, because it is not loopback.
- R3 stepped time and a `Step` message.
- Hooks that can veto or change actions (damage, team selection).

## Open questions

| Id | Question | Status |
|---|---|---|
| Q1 | Which engine callback runs the module's per-tick work | Answered: EventManager category 7 ([Threading rules](#threading-rules)) |
| Q2 | Engine native rotation convention and the conversion | Answered ([Conventions](#conventions)) |
| Q3 | How to spawn a bot into a heli seat; AI flags on the 2v2 spawn points | Answered: pad spawn, then `EnterVehicle`; needs R1b |
| Q4 | Does writing raw axes match the client ControlMap path, including signs | Server side answered (mapping bits, quantization); client ControlMap stage open |
| Q5 | Safest neutral input for the watchdog | Answered: held collective is unsafe outside a hover; level-and-hold proposed ([Watchdog](#watchdog)) |
| Q6 | Can a Windows client join a server in WSL (UDP forwarding) | Answered: use the WSL VM's IP, or Docker |
| Q7 | Does `sv.numPlayersNeededToStart 0` start a round with only bots | Answered: bots spawn either way, but the round never runs without two humans; this tree floors the setting at 2 |
| Q8 | Does unrecovered retail code use EventManager category 7 | Open; needs IDA/Ghidra. If yes, use R0 |
| Q9 | How the netsky and `mm_webadmin` Python treat `ai=1`, `connected=0` players | Answered (see "How these bots differ") |
| Q10 | Which slots move, strafe and turn a soldier, and the look axes' range | Answered ([On foot](#on-foot)); look pitch sign unmeasured |
| Q11 | Button bit table | Answered ([Buttons](#buttons)) |

Still unexercised at runtime: the three R1b guards, the enter-on-spawn path for a bot, yaw and roll
input and any client connection. The `[FRONTIER]` comment at `SpawnPoint.cpp:211-214` is stale: the bodies below it are
implemented.

## Changing this contract

1. A worker that finds the contract wrong stops and reports instead of working around it.
2. The coordinator or the author edits this file. Until the first implementation exists, answers to
   the open questions and wire changes keep version 0; after that, every wire change bumps it.
3. Every affected repo gets the change in a new brief. Repos never redefine messages locally.
