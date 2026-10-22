# Module v2: engine observers, layouts and results

Status: done on 2026-10-23 by a worker thread; committed locally in both repos. The byte layouts below were written before the
module code, as the brief requires. `docs/protocol.md` and `docs/admin-channel-draft.md` were not
edited. Paths without a repo prefix are OpenBF2 paths under `Code/BF2/` in the `serverext-r1`
worktree. The spec is `docs/protocol.md` "v2 (approved 2026-10-22)", items 1-11.

## Engine changes (OpenBF2 `serverext/r1`)

All `[CHOSEN, not recovered]`, citing protocol.md v2 (and admin-channel-draft.md R5 for the damage
observer). `IServerExtension` stays append-only and is still a separate adapter object, so no
retail vtable or layout changes. No IDA/Ghidra session was available to this worker; none of the
changes recovers or alters a retail body. Each hook is an added read-only call at a recovered site
whose source was read; the retail statements around it are unchanged.

| Addition | Where | Why it is needed (no typed path from the module) |
|---|---|---|
| R5 damage observer: `DamageInfo`, `IDamageObserver`, `addDamageObserver`/`removeDamageObserver` | call in `GameLogicDamage::serverGiveDamage`, around `armor->damage` (`Game/GameLogic/GameLogicDamage.cpp`) | Damage is a call chain inside the engine; nothing posts an event a module could hear |
| Projectile launch observer: `IProjectileObserver`, `addProjectileObserver`/`removeProjectileObserver` | call in `FireComp`'s projectile launch, after `m_lastFiredProjectileId` is set (`Game/Objects/Weapons/FireComponents/FireComp.cpp`) | Projectiles are plain registered objects with no list and no creation event; a full walk of the ~15,300 registered objects every tick would cost about 1.5 ms (v1 measured 500 objects at about 50 µs) |
| `ProjectileInfo` + `getProjectileInfo(IObject*, ProjectileInfo&)` | `GameServer.cpp` adapter | `IProjectileObject` has no recovered virtual surface; owner, team, launcher, target and activity are only on the concrete `GenericProjectile` |
| Spot observer: `SpotInfo`, `ISpotObserver`, `addSpotObserver`/`removeSpotObserver` | call at the top of `SpottedEvent::executeServer` (`Game/GameEvents/SpottedEvent.cpp`) | A human's spot runs `executeServer` directly from the client connection, never through EventManager; the server keeps no spotted state |
| `getBeginRoundState()`, `getTimeToNextStateChange()` | `GameServer.cpp` adapter, forwarding to `IGameLogic` | `gameLogic` is not reachable from a module |

Engine-internal: `Game/GameServer/ServerExtensionHooks.h` holds a small `ObserverTable<T>` (4
borrowed pointers, notified in order). Each table is defined beside its only notify site, so every
image that links the site links the table.

### R5, the damage observer (shared with the admin channel)

- **Site:** the end of `GameLogicDamage::serverGiveDamage`. `armor->damage(` has exactly one caller
  in the tree, and every `IGameLogic::giveDamage` path ends there: projectile hits, explosions,
  collisions, fall damage, bleeds, water, upside-down, out of bounds, `damageWhenLost`, crushes and
  `killPlayer`. The hook reads `getHitPoints()` just before `armor->damage(damage, perpetrator)` and
  calls the observers just after it, before `fullDestroy`.
- **Record:** victim object (the armour owner: a soldier or a vehicle), credited attacker after the
  team filter, source object (projectile or crusher), weapon object, attacker vehicle, the
  post-modifier amount, hit points before and after, maximum hit points, the caller's position and
  direction, material, and the players this call killed (those `PlayerKilled` was raised for, up to
  16).
- **Read-only:** the observer gets a const record; there is no veto (that is R6, phase 3).
- **Not observed:** hit points set outside that path: Python `setDamage`, network replication,
  `fullDestroy` writes, heals and revives, and `ServerGameLogic::giveDamage`'s early returns (game
  status not 1, damage under 0.1).
- **Position caveat:** a projectile hit passes the projectile's start position, an explosion its
  centre, a bleed 0/0/0. The armour's last-hit place is not the same thing.
- **Cost:** one empty-table test per damage call when nothing is registered.
- **Admin channel use:** the same record carries what the admin draft's `Damage` and `Kill` events
  need (victim player or object, attacker, weapon template via `weapon->getTemplate()`, amount,
  health before and after, position, killed players). Filtering is per consumer.

## Byte layouts (protocol version 2)

Framing, strings, error codes and the v1 messages are unchanged unless listed. The module accepts
only version 2; a v1 `Hello` is `Error` 1 and close.

Optional player ids carry a presence bit in the message's flags. No id value can mean "none",
because the engine gives bots ids counting down from 255 (the first bot is 255). The first draft
of these layouts used 255 for "none" and was changed before the runs below.

### Connections (item 3)

- Up to `maxConnections` (serverext.cfg, 1..8, default 4) sidecar connections at once. Another
  connection beyond that is accepted and closed at once, with no frame, as in v1.
- Each connection has its own `Hello`, request ids, bots, `TickState`, `VehicleList` and events. A bot
  belongs to the connection that created it: only that connection can drive, enter, teleport, spot
  with or remove it, and it is removed when that connection closes (after the watchdog period, as in
  v1).
- **Handover is out of scope in v2.** It adds a second owner state and an authority question
  (which connection's `SetInput` wins during the hand), and the motivating case (a test needing a
  second client) is covered by several connections. Other connections' bots appear in `TickState`
  like any other player (flag bit 1 clear), and only when in a vehicle, as in v1.
- `RestartRound`'s 60 s limit is server-wide.

### `0x0002 Welcome`

| Type | Field |
|---|---|
| `u16` | protocol version, 2 |
| `f32` | tick rate in Hz |
| `u32` | current tick number |
| string | map name |
| string | game mode |
| `f32` | `enterRange`, metres (item 2) |
| `f32` | `projectileRange`, metres |
| `u8` | features: bit 0 `allowTeleport`, bit 1 `allowRoundRestart` |

### `0x0301 TickState`

Header unchanged (`u32` tick, `f64` time, `u8` N). Each entry is the v1 entry (60 bytes) followed by:

| Type | Field |
|---|---|
| `u8` | camera view mode of an own bot (item 1): 1 inside, 2 nose, 3 chase, 4 front chase, 5 fly-by, 6 trace, 7 extern trace, 0 none; 255 for other players or a dead own bot |
| `u8` | weapon count W: 0 for other players, else the bot's active weapon slots that hold a weapon (at most 3) |
| W × weapon | item 4 |

New entry flag bits: bit 3 **man-down** (item 10: alive flag set, `IPlayer::getIsManDown`), bit 4
**spotted**: the entry's team's enemies spotted this player's root object within the last 12 s
(item 6; 12 s is the client's minimap marker lifetime, `Menu/Bf2MenuMapNode.cpp:842`).

Weapon:

| Type | Field |
|---|---|
| `u8` | slot: 0 fire (`PIFire`), 1 alt fire, 2 flares (`PlayerControlObject::addWeapon` by fire input) |
| string | weapon template name |
| `i16` | rounds in the loaded magazine (`IWeaponObject::getAmmoInMag(0)`), -1 for no ammo component or infinite |
| `i16` | magazines left including the loaded one (`getCurrentNrOfMags`), -1 when unknown |
| `u8` | flags: bit 0 reloading (`isReloading`); other bits 0 |
| `f32` | reload time left as a fraction 0..1 (`getReloadTimeLeftInPercent`), 0 when not reloading |
| `f32` | heat 0..1 (`getHeat`), -1 when the weapon does not use heat |

The weapons come from the seat's (or soldier's) `IPlayerControlObject::getActiveWeapon(0..2)`, then
`IWeaponObject` (`IID_IWeaponObject`): no engine change.

### `0x0303 ProjectileList` (module → sidecar, item 5)

Sent after `TickState` on every tick on which the connection has a projectile to report, and once
with N = 0 after the last one goes. Latest wins, like `TickState`.

| Type | Field |
|---|---|
| `u32` | tick, as in `TickState` |
| `u8` | N (at most 64) |
| N × entry | |

Entry:

| Type | Field |
|---|---|
| `u32` | projectile id: its networkable id, or the engine object id plus 0x10000 when it has none |
| string | projectile template name |
| `u8` | owner player id, valid only with flag bit 2 |
| `u8` | owner team |
| `u8` | flags: bit 0 guided (has a seek component), bit 1 fired by an own bot of this connection, bit 2 the owner id is valid |
| `u32` | networkable id of the guided target's root object, 0 if none |
| 3 × `f32` | position |
| 3 × `f32` | velocity |
| `f32` | time to live, seconds |

Which projectiles: those with a networkable id, a seek component or a detonation component
(`IID_IProjectileDetonationComp`): missiles, rockets, grenades and explosive cannon shells. Ordinary
bullets have none of the three on the dedicated server (no networkable: `GenericProjectile.cpp:1357-1359`)
and are not reported. Measured on dalian: the Hydra and S-8 rockets have no networkable id and no
seek component, only the detonation component, so a networkable-or-guided filter (the first
implementation) reported no rockets at all; the AH-1Z gunner's 20 mm `AHE_AH1Z_Gun_Projectile`
detonates and is reported. The module tracks at most 256 projectiles at once. A projectile is reported to a connection while it is active and within
`projectileRange` (default 1000 m, 10..5000) of one of that connection's live bots, or was fired by
one of them. The module applies **no perception filter**: entries are raw, like enemy `TickState`
entries, and the sidecar's perception filter decides what the bot may know.

### `0x0304 Damage` (module → sidecar, item 8)

A control frame, never dropped while the connection lives.

| Type | Field |
|---|---|
| `u32` | tick: the number of the first `TickState` whose state includes this damage (it can be off by one, because damage is applied both before and after the module's handler within a server tick) |
| `u8` | flags: bit 0 the victim is an own bot or the vehicle an own bot is in (or an own bot this damage killed), bit 1 the attacker is an own bot, bit 2 the victim's hit points reached 0, bit 3 this damage killed a player, bit 4 the victim player id is valid, bit 5 the attacker id is valid |
| `u8` | victim player id (bit 4): the soldier's player, or for a vehicle the first occupant, or the occupant this damage killed |
| `u32` | victim root object's networkable id, 0 if none |
| string | victim template name |
| `u8` | attacker player id (bit 5) |
| string | weapon template name, empty if none |
| string | projectile (source) template name, empty if none |
| `f32` | amount (hit points) |
| `f32` | hit points before |
| `f32` | hit points after |
| `f32` | maximum hit points |
| 3 × `f32` | position the engine passed (see the R5 position caveat) |

**Module filter ("what a player would see"):** a connection gets a `Damage` only when bit 0 or bit 1
is set: damage to its own bots (the health bar) and damage they deal (hit markers). Amounts are raw
hit points; any further narrowing (a hit marker without numbers) is the sidecar's.

### `0x0305 Kill` (module → sidecar, item 8)

Every kill on the server, to every connection: the kill feed every player sees.

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | victim player id |
| `u8` | victim team |
| `u8` | attacker player id, valid only with flag bit 3 (none for a crash) |
| `u8` | attacker team, 0 if none |
| string | weapon template name, empty if none |
| `u8` | flags: bit 0 victim is an own bot, bit 1 attacker is an own bot, bit 2 team kill, bit 3 the attacker id is valid |

### `0x0306 Chat` (module → sidecar, item 7)

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | flags: bit 0 the sender is a player (else the server) |
| `u8` | sender player id, valid with flag bit 0 |
| `u8` | sender team, 0 if none |
| `u8` | channel, the engine's number: 0 Global, 1 Team, 12 ServerMessage, 13 ServerTeamMessage, ... (`GameServer::handleStringReceivedEvent`) |
| string | text, control bytes replaced, at most 255 bytes |

Source: the module's `ECNetwork` listener for `StringReceivedEvent` (id 11), the same event
`GameServer` relays; no engine change. Filter: channels 0 (Global) and 12 (ServerMessage,
`game.sayAll`) go to every connection; channel 1 (Team) only to connections with a bot on the
sender's team, and channel 13 (ServerTeamMessage, `game.sayTeam`) only to connections with a bot on
the team the event's flags carry (`GameServer::sayTeam` puts it there), with that team in the team
field. Every other channel (squad, commander, local, ...) is not forwarded.

### `0x0307 RoundState` (module → sidecar, item 7)

Sent after `Welcome` and whenever a field other than the time changes.

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | game status (`IGameLogic::getGameStatus`: 1 playing, 2 end game, 3 pre game, ...), 255 if none |
| `u8` | begin-round state: 0 announce, 1 waiting for players, 2 pre-round countdown, 3 running, 255 if none |
| `f32` | seconds to the next state change, -1 if none |

### `0x0109 Teleport` (sidecar → module, item 9)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| 3 × `f32` | position |
| `f32` | yaw, degrees |

Reply `Ack` or `Error`. `Error` 6 unless `allowTeleport 1` (default 0). The bot must be alive
(`Error` 6). Non-finite or out-of-world (|x|, |z| > 8192, y outside -1000..10000) values are `Error`
7. The module moves the bot's root object (its vehicle when seated) with
`setAbsoluteTransformation` to a level attitude at that yaw, then resets its physics node (speeds
zero) and wakes it. On foot the yaw does not stick: the soldier's facing comes from its own look
state, so a sidecar turns it with look yaw.

### `0x010A Spot` (sidecar → module, item 6)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| `u32` | target: networkable id of a root object, as `TickState` and `VehicleList` give |

Reply `Ack` or `Error`. The bot must be alive and not man-down (`Error` 6); the target must exist
(`Error` 5) and be an enemy: a root object whose team, or whose occupant's team, is the other team
(`Error` 5). At most one spot per bot per second (`Error` 6). The module sends what a human's Q +
click sends: `IGame::sendSpottedMessage(bot, target, x/z, type)` (type: the target template's `IPlayerControlObjectTemplate::getVehicleType`, 0 for a soldier; a human client passes what its commo rose chose, which the module cannot know) and the "spotted" radio
line to the team. It does not check that the bot can see the target: that is the sidecar's
perception filter.

### Watchdog (item 11)

No wire change. In a vehicle, after `watchdogTicks` without `SetInput`, the module flies the
level-and-hold law of `docs/findings/q5-q9.md` every tick (`src/Neutral.h`): cyclic pitch
`-0.05·pitch - 0.05·pitch rate`, cyclic roll `0.05·roll + 0.05·roll rate`, pedals
`-0.03·yaw rate` (degrees, degrees/s), collective `ci - 0.03·vy` with `ci -= 0.03·vy·dt`, starting
from the last collective. The law is for helicopters only: the module applies it when the bot is in
seat 0 of a vehicle whose template's vehicle category is `VCHelicopter` (3,
`Game/Objects/Game/ModuleWorldObjTemplGame.cpp`; `ObjectTemplate.vehicleCategory VCHelicopter` in the
heli tweaks), read with the typed virtual `IPlayerControlObjectTemplate::getVehicleCategory`, so no
engine change. A first attempt read the HUD vehicle type through an appended engine method; the
dedicated server has no VehicleHud template components, the test always failed, and the first
watchdog run fell back to v1's neutral and lost the heli in the climb exactly as in Q5. That method
was removed. Other vehicles and other seats keep v1's neutral (cyclic and
pedals 0, collective held); on foot every axis stays 0. The law runs every tick until a `SetInput`
arrives, the bot leaves the heli or dies (then all axes 0), or, after a disconnect, the bot is
removed.

## Module design notes

Files changed in this repo: `src/Protocol.h/.cpp`, `src/Channel.h/.cpp`, `src/Session.cpp`,
`src/Config.h/.cpp`, `src/IoThread_posix.cpp`, `src/Module.cpp`, `src/Neutral.h` (new),
`tests/TestMain.cpp`, `tools/testclient/serverext_client.py`, `testenv/deploy-module.sh` (the v2
keys `MAX_CONNECTIONS`, `ALLOW_TELEPORT`, `PROJECTILE_RANGE`), `README.md`.

- **Version:** `kVersion = 2`; a v1 or v3 `Hello` is `Error` 1 and close.
- **serverext.cfg:** `maxConnections` (1..8, default 4), `allowTeleport` (`0`/`1`, default 0),
  `projectileRange` (10..5000 m, default 1000). A bad value refuses the load, as before.
- **Connections:** `Channel` keeps one outbound stream per open connection: a control queue plus three
  latest-wins slots, `TickState`, `ProjectileList` and `VehicleList`, written in that order after the
  control frames. The I/O thread polls up to `maxConnections` clients. The main thread keeps a
  `Conn` per connection (ready, next `VehicleList`, last `RoundState`, whether the last
  `ProjectileList` had entries). Each bot keeps its connection; commands for another connection's
  bot are `Error` 4. `RestartRound`'s 60 s window is server-wide.
- **TickState:** the in-vehicle players are filled once per tick and shared; each ready connection
  gets its own bots first (with view mode and weapons), then those players minus its own bots. The
  encoder drops entries from the end before the payload would pass 64 KiB.
- **Observers:** `Module` is the `IDamageObserver`, `IProjectileObserver` and `ISpotObserver`,
  registered in `initDll` (`damage 1, projectile 1, spot 1 observers` in the load line). The
  callbacks only encode and queue frames or update module tables. Chat comes from an `ECNetwork`
  listener (id 11, `StringReceivedEvent`), registered after `GameServer`'s, so it sees the raw sender.
- **Projectiles:** launches are filtered (networkable, seek or detonation component), then tracked
  by object id. Every tick an entry is kept only while `ObjectManager::getRegisteredObject(id)`
  still returns the same pointer, the object is not flagged cached (0x20) and `getProjectileInfo`
  says active. Position comes from `getAbsolutePosition`, velocity from the physics node. A cached
  projectile gets a new object id, so a recycled object is never taken for the old one.
- **Spotted state:** a table of spotted root networkable ids with an expiry per spotting team (12 s),
  pruned every 12 s. `TickState` bit 4 is set while any other team's expiry is in the future.
- **Watchdog:** `neutralise` picks v1's neutral or level-and-hold when it trips; `flyNeutral` runs
  the law each tick after that, also during the removal countdown after a disconnect.
- **Observability:** the module logs man-down changes (with time left), kills, spots, teleports,
  projectile launches (the first 20 and every 200th), skipped launches (the first 20), chat (the
  first 50 and every 100th), round-state changes, and the launch count in the per-minute cost line.

Unit tests: 621,268 checks, 0 failures. New: the Teleport and Spot parsers (exact, short, long,
empty, non-finite), the v2 module-only types refused from a client (Error 3), Teleport and Spot as
Session commands and before `Hello` (Error 2), the `Welcome` v2 fields, `TickState` v2 entries
field by field (an own bot with two weapons, another player), the 64 KiB cut with long weapon names,
`ProjectileList` (fields, empty, the 64-entry cap), `Damage`, `Kill`, `Chat` (control bytes, the
255-byte cut), `RoundState`, the multi-connection `Channel` (slot order, independent connections,
a close drops only that connection's frames and commands, overflow per connection) and the
level-and-hold law (signs, rates, the yaw seam, the integrator, clamps, non-finite input). The fuzz
sessions now include Teleport and Spot. The module builds without warnings.

## Runtime validation

| Item | Value |
|---|---|
| OpenBF2 | worktree `serverext-r1`, branch `serverext/r1` at `540fa88` plus this change, committed after the runs as `74e18d9`, built into `~/bf2-linux-serverext-v2` (`testenv/build-linux.sh`). Final `bf2` md5 `80a5b28c7a995e03b3cb302ef1cf91b1`, `libdice_py.so` `2abb1d07936e1e4747d543d464b4c600` |
| Module | `~/serverext-build/v2` (`build-module.sh`); final `libserverext.so` md5 `be151ea0732ebd650e1cc68ce7eb971e` |
| Content | netsky `2v2-nocheck`, fresh run dir `~/serverext-v2/srv` (`prepare-content.sh`) |
| Server | WSL, `testenv/run-wsl.sh`, `dalian_2_v_2` (`gpm_cq` 16), path `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`. Game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723, bot 127.0.0.1:16701; `allowRoundRestart 1`, `allowTeleport 1`, `maxConnections 4`. Ports 16700, 16567, 16569, 29901, 4721 and 4300 were not used; no container of anyone else was touched |
| Client | `tools/testclient/serverext_client.py` in WSL (Python 3.10.6); logs in `~/serverext-v2/*.log`, server logs in `~/serverext-v2/srv/bf2*.log` (one per server process) |

Seven server processes ran, one per module or engine rebuild, each stopped with SIGTERM. Every
scenario was rerun on the last one after the final engine change (`*-f.log`); the results below are
from that process unless a row says otherwise.

| Item | Result | Run |
|---|---|---|
| 1 view mode | **Pass 12/12**: TickState's view mode follows every camera key as Q11's table says (1 → 2 → 3, then mode keys 1/2/3/4 → 1/3/4/5, mode 1 → 1) | `view-f` |
| 2 enterRange in Welcome | **Pass**: `enterRange 30.0 m, projectileRange 1000 m, features 0x03`, equal to serverext.cfg | `view-f` |
| 3 connections | **Pass 13/13**: two connections each own one bot; the other connection's `SetInput`, `EnterVehicle`, `RemoveBot` and `Spot` with it are `Error` 4; the 5th connection (`maxConnections 4`) is closed without a frame; closing A removes A's bot 6 ticks later and B's stays; a new connection gets its own bot | `multiconn-f` |
| 4 weapon state | **Pass**: on foot `usrif_m4` 30/5; pilot `AHE_AH1Z_HydraLauncher` 8 rounds, 5 mags and `AHE_AH1Z_FlareLauncher` (slot 2) 6/1; rounds drop per shot; the reload is reported with `reload_left` falling from 1.00 to 0.00 over about 8 s (241 ticks), mags 5 → 4; gunner `AHE_AH1Z_Gun` 900 with heat | `weapons-f`, `damage-f` |
| 5 projectiles | **Pass for rockets and cannon shells; guided missiles not exercised.** 8 own Hydra rockets were reported (template, owner 255, team 2, about 105 m/s, TTL 9.97 s), and the list went empty afterwards; the gunner's 20 mm shells are reported. The AH-1Z gunner bot never launched its TV missile (see What is left) | `weapons-f` |
| 6 spotting | **Pass 10/10**: A spots B: Ack, B's entry has bit 4 within 0.08 s, cleared after 12.0 s; a second spot within 1 s is `Error` 6, a teammate `Error` 5, an unknown id `Error` 5; A is not flagged | `spot-f` |
| 7 round state and chat | **Pass 6/6**: `RoundState` after `Welcome` (status 1, begin-round 1: bots never start a round); `game.sayAll "rdy"` → `Chat` channel 12; `game.sayTeam 1 "go"` → channel 13, team 1; `sayTeam 2` is not sent to a client with no team-2 bot; after `RestartRound` (3,0) → (1,0) → (1,1). A map change gave (1,1) → (2,1) → (1,0) → (1,1). Human chat (channels 0 and 1) was not exercised: no game client | `round-f`, `mapchange` |
| 8 damage and kills | **Pass 18/18**: rifle at B's occupied AH-1Z: `Damage` flags 0x33, victim 254, attacker 255, `chrif_type95`, 875.0 → 873.8 of 875.0, and the last `after` equals TickState's health; rockets at C on foot: `Damage` 0x33, `ahe_z10_S8Launcher` / `ahe_z10_S8Launcher_Projectile`, 100.0 → 50.9; `Kill` attacker 252 team 1 with `ahe_z10_S8Launcher`, flags 0x0b; a crash: `Kill` with no attacker, and `Damage` without an attacker but with the victim-own flag | `damage-f` |
| 9 teleport | **Pass 11/11**: on foot to 0.8 m of the target; NaN, infinite yaw, x 9000 and y -2000 are `Error` 7; a foreign player `Error` 4; a seated heli to +120 m at yaw 131: yaw 130.5, pitch -0.5, roll 0.0, horizontal speed 0.2 m/s, still seated (it then falls from rest). `allowTeleport 0` → `Error` 6 is covered by the client (`--expect-teleport-refused`, also in `--fuzz`) but was not run: every server ran with 1 | `teleport-f` |
| 10 man-down bit | **Pass**: C's bit 3 was set with the alive flag for 2.1 s (`sv.manDownTime 2`), then it cleared and C was 664 m away at its spawn. Module log: `bot 253 man-down 1 (time left 2.0 s, time to spawn 2.0 s)`, 60 ticks later `man-down 0` | `damage-f` |
| 11 watchdog neutral | **Pass, all five states survive** (below) | `q5-module-2` |
| Fuzz v2 | **Pass 61/61**, server still at 29.9 ticks/s | `fuzz-1` |
| Map change | **Pass**: `level unload`, `BotRemoved` reason 2, recreate, re-enter, hover | `mapchange` |
| 10-minute soak | **Pass** (below) | `soak2` |

Log excerpts (module log, final process):

```
loaded: protocol v2, bot channel 127.0.0.1:16701 (max 4 connections), watchdog 6 ticks, enterRange 30.0 m, ... allowTeleport 1, projectileRange 1000 m; tick hook 1 (event 1), unload hook 1, chat hook 1, damage 1, projectile 1, spot 1 observers
refused a connection from 127.0.0.1: 4 already open (maxConnections)
tick 1493: sidecar connection 2 gone; 1 bots neutralised, removal in 6 ticks
tick 1499: bot 255 removed after its sidecar disconnected
tick 4261: bot 255 spotted 'us_light_soldier' (net id 1794, team 2)
tick 4262: object 1794 spotted by team 1 (connection -1, spotter 1666)
tick 10471: kill: player 253 (team 2) by 252 with 'ahe_z10_S8Launcher'
tick 11017: kill: player 252 (team 1) by -1 with ''
tick 1702: bot 255 watchdog: no SetInput for 6 ticks; heli level-and-hold neutral
tick 16254: level unload
tick 16255: bot 255 no longer exists (level unload); not destroying it again
```

### Watchdog (item 11): the module's neutral in flight

`--scenario q5 --q5-neutral module --q5-att 0.06,0.08,0.005 --q5-vel 1.0,0.15`, Z-10 on team 1. The
module logged `watchdog: no SetInput for 6 ticks; heli level-and-hold neutral` at each cut.

| State | 10 s after the cut | 20 s after the cut | q5-q9.md, emulated `hover`, 20 s |
|---|---|---|---|
| hover | +0.4 m, 5.9 m/s | +0.1 m, 8.3 m/s | -11.1 m, 0.0 m/s |
| climb (full collective) | +21.6 m, pitch ≤ 5.0°, 6.7 m/s | +21.3 m, 8.8 m/s | +21.9 m, then holds |
| descent (steep) | -11.4 m, 7.3 m/s | -17.0 m, 8.4 m/s | -16.7 m |
| forward 29 m/s | +14.4 m, pitch ≤ 8.5°, 9.3 m/s | +15.7 m, 8.0 m/s | +15.7 m, 8.3 m/s |
| banked turn 32° | +13.5 m, roll 31.7° levelled, 9.5 m/s | +14.6 m, 8.8 m/s | +13.7 m (10 s) |

Health stayed 1.00. The first run (`q5-module-1`, before the vehicle-category fix) used v1's neutral
by mistake and lost the heli in the climb at 10.4 s, as Q5 did. The 6-10 m/s drift that q5-q9.md
predicted for this law remains: it levels and holds height, but not position.

### 10-minute soak

`--scenario soak2 --soak-seconds 600`: one connection, with A (team 1, Z-10) and B (team 2, AH-1Z)
hovering 30 m over their pads. Every 30 s A spots B, every 45 s both toggle the camera, and every 60 s
A fires one rocket seaward. All v2 state was computed every tick: weapons, view modes, the spotted
table, projectile tracking, round state, and the damage, projectile and spot observers.

| Check | Result |
|---|---|
| TickStates | 18,001 in 600 s, 0 gaps |
| Bots | 0 deaths, 2 boardings (the starts); 19 spots acked, 0 refused; 10 `Damage` events reached the client (the soak client does not break them down) |
| Server process | alive, `udp16571` bound, RSS 144,184 → 144,192 KB over 11 checks |
| Log lines | 0 `[FRONTIER]`, 0 abort, 0 segfault in each of the 7 server logs of the run dir |
| Handler cost, soak windows (1,800 ticks each) | p50 100-110 µs, p99 160-180 µs, max 218-594 µs |
| Handler cost, whole last process (37,800 ticks) | p50 110 µs, p99 170 µs, max 8,559 µs (a fuzz window), 0 ticks over 20 ms |

v1 measured p50 70-80 µs and p99 110-150 µs. The extra 30 µs is the per-tick weapon, view and
projectile work and the two-bot `TickState`. p99 stays far below the 1 ms budget. Ticks over 1 ms
create bots or absorb the fuzz flood, as in v1.

## Contract points that proved wrong or ambiguous

`docs/protocol.md` was not edited. Each item names the reading implemented.

1. **No player id value can mean "none".** Bots get ids counting down from 255, so the first bot is
   255. Every optional player id (projectile owner, damage victim and attacker, kill attacker, chat
   sender) has a presence bit instead.
2. **"Projectiles, at least missiles in flight" needs a different test from "networkable or
   guided".** The Hydra and S-8 rockets have neither a networkable id nor a seek component on the
   server. The module reports projectiles with a detonation component too. The AH-1Z gunner's 20 mm
   shells detonate, so they are reported as well; a sidecar that wants only missiles filters by
   template.
3. **Guided missiles were not exercised.** In the AH-1Z gunner seat, weapon select 2 left the
   seat's active slots at the gun, and alt fire launched nothing. The guided flag is the seek
   component, as the engine's `initComponents` reads it, but no guided projectile was seen at
   runtime. **Guiding** a TV missile needs a new message: the server steers it from the client's
   `TargetDirectionEvent`, which calls the virtual `IWeaponObject::setTargetDirection`
   (`GameEvents/TargetDirectionEvent.cpp:95-130`), so a `SetTargetDirection` message needs no engine
   change. Not designed here.
4. **The server keeps no spotted state.** Spotting is a relay to the spotting team's HUDs, which
   show a 12 s marker. The module keeps its own table from the engine's spot observer: its own
   bots' spots and, through the same hook, humans' (not exercised: no client). Because `TickState`
   lists other players only while they are in a vehicle, a spotted enemy soldier on foot is not
   visible to the sidecar unless it is one of its own bots.
5. **The alive flag may never clear on death.** With `sv.spawnTime` at or below `sv.manDownTime`
   (2 and 2 in the netsky content), the engine kills a man-down soldier and respawns it in the same
   tick (`ServerGameLogic::uPlayingSpawning`, pass 2), so `TickState` shows man-down for 2 s, then
   alive at the spawn point, never alive 0. The sidecar should take "man-down bit dropped and the
   bot jumped to a spawn point" as a death. The module's own death handling (zeroing the stored
   axes, resetting the spawn fallback) runs only when the alive flag drops, so it is skipped for
   these deaths. That is harmless for the fallback (the bot respawned), but the last `SetInput`
   axes stay stored until the sidecar sends new input or the watchdog trips. The q5-q9.md reading
   "the alive flag stays set for `sv.manDownTime`" is right, but the death that follows can be
   invisible.
6. **Damage position is not the hit point.** The engine passes the projectile's start position for
   a projectile hit, the explosion centre for splash and 0/0/0 for bleeds. `Damage` carries it raw.
   Damage set outside `serverGiveDamage` is not observed (Python `setDamage`, replication,
   `fullDestroy`, heals).
7. **"Filtered to what a player would see" (item 8)** is implemented as involvement: `Damage` only
   when the connection's bot is the victim (or sits in it, or was killed by it) or the attacker;
   `Kill` for every kill (the kill feed). Amounts and hit points are raw; a sidecar wanting a bare
   hit marker drops them.
8. **On-foot rifle fire at soldiers still did not register.** 30 rounds at 0.5, 0.9 and 5 m, facing
   error under 2°, never hit another bot; the same rifle hit an occupied heli at 15 m every time,
   and rockets killed a soldier. That matches q5-q9.md section 6 and is not root-caused (not
   module-related: no damage reached the engine's damage path).
9. **Teleport on foot does not keep the yaw.** The soldier's facing comes from its own look state
   (`Soldier::updateTransformation`), so the sidecar turns it with look yaw. A seated heli takes the
   yaw. Teleport zeroes the speeds (`IPhysicsNode::reset(true)`); a heli then falls from rest.
10. **The Hydra and S-8 pods fire once per press.** Holding fire launches one rocket; a burst needs
    the trigger pulsed (3 ticks on, 3 off fired the whole pod).
11. **`reload_left` reads 0 on the first reloading tick**, then 1.0 and falling. The sidecar should
    use the peak, or treat the first tick as unknown.
12. **"The selected weapon"** is what `getActiveWeapon(0..2)` returns per fire input. A seat with
    several weapons on one input (the gunner's gun and TV missile) shows only the active one.
13. **Round state with bots only** is begin-round 1 (waiting for players) forever, as Q7 says. The
    2v2 "rdy"/"go" convention needs human chat, which arrives as `Chat` channel 0 or 1 (not
    exercised). Server chat (`game.sayAll`, `game.sayTeam`) is channels 12 and 13.
14. **Additions the contract did not ask for:** `Welcome` also carries `projectileRange` and a
    feature byte (teleport, round restart allowed), so a sidecar knows whether `Teleport` and
    `RestartRound` can succeed.
15. **Handover of bots between connections is out of scope**: see Connections.
16. **Telling a heli from other vehicles** uses the template's vehicle category (`VCHelicopter`),
    not the HUD vehicle type, which the dedicated server never loads.

## What is left

- **Guided missiles:** launching a TV or wire missile from a gunner bot, and a `SetTargetDirection`
  message to guide it (contract point 3).
- **Client paths:** human spots, human chat (channels 0 and 1) and the "rdy"/"go" start; no game
  client joined.
- **`allowTeleport 0`** refusal was not run against a server (the code path is a one-line check).
- **Docker:** see below.
- The admin channel can build on R5 as it is: the record carries everything its `Damage` and
  `Kill` events need (victim object and player, attacker, weapon via `weapon->getTemplate()`,
  amount, hit points before and after, position, killed players).

## Remaining frontiers relevant to this change

- No `[FRONTIER]` line was printed in any run.
- `serverGiveDamage` calls `BFCollisionHandler::handleProjectileDamage`, a `[FRONTIER]` that aborts,
  only when an AI collision handler is registered; without the AIDLL it is not. The R5 call is after
  it and does not change that.
- `SpottedEvent::executeServer` posts the AI's `event_enemySpotted` (a `[FRONTIER]` abort) and a
  radio message reaches `GameServer::sendRadioMessageToAI` (`[FRONTIER]`) only in a bots game
  (`aiInstance()->isInitialized()`), which this server never is. `Spot` exercised both paths without
  reaching them.
- `IProjectileObject` has no recovered virtual surface ("full virtual declarations are intentionally deferred", its header);
  `getProjectileInfo` uses `GenericProjectile` through the same cast the engine itself makes.

## Docker test image

`testenv/build-bullseye.sh` into this worker's own `~/bf2-linux-serverext-v2-bullseye` (Debian 11,
glibc 2.31; module unit tests pass there too), then `run-docker.sh build` and `start` with
`TESTENV_NAME=serverext-v2-testenv` (a new image and container, so the earlier workers'
`serverext-testenv` image was not touched), `PUBLISH_IP=127.0.0.1`, bot channel 127.0.0.1:16702,
`SERVEREXT_CFG_EXTRA="allowTeleport 1;allowRoundRestart 1"`. `bf2` md5
`87f78d9b9b02f6bf618cfb6570103e01`, `libserverext.so` `da858a44a571944d83ffc9b31ffc5b3b`. Client:
Windows Python 3.10.6 (`py -3`), logs `%TEMP%\serverext-v2\docker-*.log`. The WSL server was
stopped first.

| Check | Result |
|---|---|
| Load | `loaded: protocol v2, bot channel 0.0.0.0:16702 (max 4 connections), ... allowRoundRestart 1, allowTeleport 1, projectileRange 1000 m; ... damage 1, projectile 1, spot 1 observers` |
| `view` | Pass 12/12 |
| `spot` | Pass 10/10 |
| `damage` (without `--crash`) | Pass 16/16 |
| `--fuzz` | Pass 61/61 |
| 60 s hover | `HOVER HELD 60 s; max altitude error 2.99 m; horizontal drift 9.0 m; health 1.00` |
| Container log (1,714 lines) | 0 `[FRONTIER]`, 0 abort, 0 segfault; cost p50 120-130 µs, p99 180-260 µs |

The container was stopped and removed afterwards; the image `serverext-v2-testenv` is left in place.

## Gates (OpenBF2)

| Gate | Result |
|---|---|
| MSVC x86, the four touched TUs one by one (`syntaxcheck.cmd <file>`) | Pass |
| MSVC x86 full syntax gate (`syntaxcheck.cmd`) | `ALL OK`, exit 0, rerun after the last engine edit |
| Linux x86-64 (`testenv/build-linux.sh`, own `~/bf2-linux-serverext-v2`) | Pass, `python runtime check complete`; also in Debian 11 (`build-bullseye.sh`) |
| msvc-x64 strict link (`%TEMP%\serverext-v2\x64.cmd`, own `x64build`) | `LNK1120: 6 unresolved externals`, all `BF2VoipServer::CBF2VoipServer::*` (constructor, destructor, `CreateInstance`, `Initialize`, `Shutdown`, `Update`), rerun after the last engine edit; no compile errors |

## Git

- OpenBF2 worktree `serverext-r1`, branch `serverext/r1`: commit `74e18d9` on top of `540fa88` with the
  engine changes above.
- openbf2-server-ext, branch `master`: one commit with the module, tests, test client, deploy
  script, README and this file.
- Nothing was pushed; no remotes, issues or pull requests.
