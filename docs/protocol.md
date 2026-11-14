# Bot channel protocol

The bot channel lets an external process create bot players on an OpenBF2 dedicated server, read the game state every
simulation tick and send each bot's input. This document is its contract: the wire format, the behaviour behind every
message, and the engine facts the module relies on. The current protocol version is **4**.

| Piece | Where | Role |
|---|---|---|
| The module | this repo, `libserverext.so` | Runs inside the server process. Owns the socket, the per-tick snapshot, the input queue and the watchdog. |
| Engine additions | the `serverext/r1` branch of [nihlen/OpenBF2](https://github.com/nihlen/OpenBF2/tree/serverext/r1) | The few things a module cannot do from outside ([Engine requirements](#engine-requirements)). |
| The sidecar | any client of this protocol | A separate process, normally on the same host. Reads state, decides, sends inputs. |

The module's second channel, for server administration, is described in [admin-channel.md](admin-channel.md). Every
setting named here is listed in [configuration.md](configuration.md).

Engine citations such as `GameServer.cpp:371` are paths and lines in the OpenBF2 source under `Code/BF2/`.

## Contents

- [Loading](#loading)
- [Threading rules](#threading-rules)
- [Transport and framing](#transport-and-framing)
- [Conventions](#conventions)
- [Messages](#messages): [handshake](#handshake), [bots](#bots), [input](#input), [state](#state),
  [events](#events), [stepping](#stepping), [errors](#errors)
- [Spawning](#spawning)
- [Watchdog](#watchdog)
- [Map changes](#map-changes)
- [Stepped time](#stepped-time)
- [TV-guided missiles](#tv-guided-missiles)
- [Notes for client authors](#notes-for-client-authors)
- [Engine requirements](#engine-requirements)
- [How these bots differ from retail AI bots](#how-these-bots-differ-from-retail-ai-bots)
- [Known limitations](#known-limitations)
- [Versioning](#versioning)

## Loading

- The module is built as `libserverext.so` for Linux x86-64. There is no Windows build.
- `bf2` exports no symbols. The module can only make typed virtual calls on engine interfaces and use inline header
  code. Non-virtual engine functions such as `world::getRootParent` are out of reach; the module walks
  `IObject::getParent()` instead.
- The module is loaded by this line in `mods/bf2/openbf2.con`, which `GameServer::init` runs before Python initialises
  (`GameServer.cpp:262-279`):

  ```
  openbf2.addModDll serverext
  ```

- The file goes in `bin/amd-64/` beside `bf2`, because `dlopen` searches `$ORIGIN` and `LD_LIBRARY_PATH`, not the
  working directory (`OpenBF2Console.cpp:110-139`).
- It exports `initDll(IClassManager*)`. The loader unloads the module only if `initDll` returns false
  (`OpenBF2Console.cpp:239-242`). There is no unload path after that, so a loaded module lives as long as the process.
- Settings come from `serverext.cfg` in the server's working directory ([configuration.md](configuration.md)). A
  missing file means defaults. A bad value makes `initDll` return false, so the module refuses to load instead of
  listening somewhere unexpected.

## Threading rules

These rules are part of the contract, because breaking them causes server hitches or crashes.

1. The server runs a single-threaded loop with a fixed 30 Hz simulation tick (`TickCalculator(10, 30.0f)`,
   `Game/Main/BF2Engine.cpp:418`).
2. The module's socket I/O runs on its own thread. That thread never calls into the engine or Python. It only moves
   bytes into and out of queues.
3. Engine work happens on the main thread, once per tick: drain the inbound queue and apply commands and inputs, then
   build one state snapshot and push it to the outbound queue.
4. The main thread never blocks: no socket writes, no disk I/O, no waiting on the sidecar. If the sidecar is slow,
   the server keeps ticking and the [watchdog](#watchdog) applies. The one exception is [stepped time](#stepped-time),
   where the tick source may wait up to 2 ms per frame in place of the engine's frame sleep.

### The per-tick hook

The module needs no engine change to run once per tick.

- In `initDll` it gets the `"EventManager"` singleton, registers an `IEventListener` on event category 7, which
  nothing in the OpenBF2 tree uses, and posts one `postEventNextUpdate(7, …)`. The handler does the tick's work and
  posts the next event.
- The handler runs exactly once per simulation tick, inside `GameServer::simulateFrame`, after `gameLogic->update` and
  before `simulatePlayersUpdate` (`GameServer.cpp:2522-2546`).
- Inputs written in the handler take effect in the same tick. The snapshot taken there is the end of the previous
  tick, and `TickState` is numbered accordingly. The module keeps its own tick counter.
- `FrameEventManager` is not used: it runs once per frame, which can be 0 to 10 ticks, and after the network send.

## Transport and framing

- TCP on `botListen`. The channel has **no authentication**: keep it on loopback. Inside a container it can bind
  `0.0.0.0` when Docker publishes the port on `127.0.0.1` only.
- Up to `maxConnections` connections at once (default 4). A connection beyond that is accepted and closed at once,
  with no frame.
- Each connection has its own handshake, request ids, bots, state stream and events. A bot belongs to the connection
  that created it: only that connection can drive, enter, teleport, spot with or remove it. Bots cannot be handed
  between connections.
- Every message is a frame:

  | Offset | Type | Field |
  |---|---|---|
  | 0 | `u32` | payload length in bytes (excludes this header) |
  | 4 | `u16` | message type |
  | 6 | `u16` | reserved, 0 |
  | 8 | bytes | payload |

- All integers and floats are little-endian. Floats are IEEE-754 `f32` unless stated. A string is a `u16` byte length
  followed by UTF-8 bytes, with no terminator.
- The maximum frame payload is 64 KiB. A larger length closes the connection.
- Any framing or parse error (bad length, nonzero reserved field, wrong payload length, anything before `Hello`, a
  second `Hello`) is `Error` 2, then close. An unknown type, including one sent in the wrong direction, is `Error` 3,
  then close. Per-request errors (codes 4 to 7) keep the connection.
- A malformed or hostile client must never crash or stall the server. `tools/testclient/serverext_client.py --fuzz`
  exercises this.

## Conventions

- World axes: x east, y up, z north. Metres, metres per second.
- Angles are in degrees: yaw 0 is north and 90 is east; positive pitch is nose down; positive roll is a bank to the
  left.
- Angular velocity is in rad/s about the world axes; positive about y turns the nose east.
- These are the engine's own conventions. The module reads the root vehicle object (it walks `getParent()` to the
  top), not the seat. The transform `m` is `IObject::getAbsoluteTransformation()` (row-major; rows right, up, forward,
  position) and the physics node comes from `queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`
  (`queryInterface(IID_IPhysicsNode)` returns null):

  | Field | Source |
  |---|---|
  | x, y, z | `m[12], m[13], m[14]` |
  | yaw | `atan2(m[8], m[10])`, degrees |
  | pitch | `-asin(clamp(m[9], -1, 1))`, degrees |
  | roll | `atan2(m[1], m[5])`, degrees |
  | velocity | `getPositionalSpeed()`, unchanged |
  | angular velocity | `getRotationalSpeed()`, unchanged |

- This is the body attitude, not a camera's. A helicopter's pilot camera is pitched a few degrees nose down relative
  to it.
- **Player ids.** No id value means "none": the engine gives bots ids counting down from 255, so the first bot is
  255. Every optional player id has a presence bit in the message's flags.
- **Object ids** are the engine's networkable ids of root objects (a vehicle, or a soldier on foot).
- **Tick numbers.** The module's tick N sends the state at the end of tick N-1, numbered N-1. `Welcome` carries N.
  `SetInput` echoes the `TickState` number it was computed from.
- **Raw state.** The module applies no perception filter. Enemy positions, vehicles and projectiles are sent as the
  server knows them; deciding what a bot may fairly know is the sidecar's job.

## Messages

| Id | Name | Direction | Reply |
|---|---|---|---|
| `0x0001` | [`Hello`](#handshake) | sidecar → module | `Welcome` or `Error` |
| `0x0002` | [`Welcome`](#handshake) | module → sidecar | |
| `0x0101` | [`CreateBot`](#bots) | sidecar → module | `BotCreated` or `Error` |
| `0x0102` | `BotCreated` | module → sidecar | |
| `0x0103` | [`EnterVehicle`](#bots) | sidecar → module | `Ack` or `Error` |
| `0x0104` | [`RemoveBot`](#bots) | sidecar → module | `Ack` or `Error` |
| `0x0105` | `Ack` | module → sidecar | |
| `0x0106` | `BotRemoved` | module → sidecar | |
| `0x0107` | [`SpawnBlocked`](#spawning) | module → sidecar | |
| `0x0108` | [`RestartRound`](#spawning) | sidecar → module | `Ack` or `Error` |
| `0x0109` | [`Teleport`](#bots) | sidecar → module | `Ack` or `Error` |
| `0x010A` | [`Spot`](#bots) | sidecar → module | `Ack` or `Error` |
| `0x010B` | [`Say`](#bots) | sidecar → module | `Ack` or `Error` |
| `0x010C` | [`Step`](#stepping) | sidecar → module | none |
| `0x0201` | [`SetInput`](#input) | sidecar → module | none (`Error` on a bad value) |
| `0x0202` | [`SetTargetDirection`](#input) | sidecar → module | none (`Error` on a refusal) |
| `0x0301` | [`TickState`](#state) | module → sidecar | |
| `0x0302` | [`VehicleList`](#state) | module → sidecar | |
| `0x0303` | [`ProjectileList`](#state) | module → sidecar | |
| `0x0304` | [`Damage`](#events) | module → sidecar | |
| `0x0305` | [`Kill`](#events) | module → sidecar | |
| `0x0306` | [`Chat`](#events) | module → sidecar | |
| `0x0307` | [`RoundState`](#state) | module → sidecar | |
| `0xFFFF` | [`Error`](#errors) | module → sidecar | |

Requests carry a `u32` request id that the reply echoes. Each parser requires the payload to be exactly the message,
with no trailing bytes.

### Handshake

The sidecar connects and sends `Hello` first. The module answers `Welcome`, or `Error` 1 and closes.

**`0x0001 Hello`** (sidecar → module)

| Type | Field |
|---|---|
| `u16` | protocol version: 3 or 4 |
| string | client name, for logs |

**`0x0002 Welcome`** (module → sidecar)

| Type | Field |
|---|---|
| `u16` | protocol version: the client's |
| `f32` | tick rate in Hz (30) |
| `u32` | current tick number |
| string | map name |
| string | game mode |
| `f32` | `enterRange`, metres |
| `f32` | `projectileRange`, metres |
| `u8` | features: bit 0 `allowTeleport`, bit 1 `allowRoundRestart`, bit 2 stepping available (`stepMode 1`) |

There is no negotiation. Version 4 adds only `Step`; a version 3 client gets everything else unchanged, and a `Step`
from one is `Error` 3. Any other version is `Error` 1 and close.

### Bots

**`0x0101 CreateBot`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| string | name. The module adds no prefix, so a sidecar that wants one, such as `[BOT]`, sends it. |
| `u8` | team, 1 or 2 |

Reply: `BotCreated` or `Error`. A team other than 1 or 2, or an empty name, is `Error` 7. Control bytes in the name
are replaced and the name is capped at 64 bytes. `Error` 6 means the team has no spawn group at all, as during a level
load; retry after a couple of seconds.

The module creates the player, then sets its team, a valid kit and its team's on-foot spawn group. The engine spawns it
on a later tick, with no human present. `TickState`'s alive flag shows when that has happened. See
[Spawning](#spawning) for what happens when the spawn points are blocked.

A bot that dies, including at the end of every round, stays a bot: the engine respawns it without a `BotRemoved`.
`TickState` shows it alive and on foot, and the sidecar sends `EnterVehicle` again. Only a map change removes it.

**`0x0102 BotCreated`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id |

**`0x0103 EnterVehicle`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| `u32` | vehicle id: a networkable id as in `TickState` and `VehicleList`, or 0 for the nearest vehicle with that seat free |
| `u8` | seat index, 0 = pilot |

Reply: `Ack` or `Error`.

- The bot must be alive and on foot (`Error` 6 otherwise) and within `enterRange` metres of the vehicle (default 30).
  A bot that spawned elsewhere walks there first.
- Vehicle and seat problems are `Error` 5. A "vehicle" is any root object with a seat that is not a soldier and not a
  camera (every player's free camera is a root seat object too).
- Destroyed vehicles are skipped: id 0 never picks one, and an explicit id is `Error` 5. Damaged vehicles can be
  entered; a minimum health is the sidecar's policy.
- The module checks that the seat is free and that the vehicle's team is 0 or the bot's, because
  `IGame::enterVehicle` checks neither, then calls `IGame::enterVehicle(player, seat, false)` (`Game/IGame.h:126`).
- The search for id 0 walks the vehicle set, so do not send it every tick.

**`0x0104 RemoveBot`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id |

Reply: `Ack` or `Error`. The module also removes every bot of a connection when that connection closes: they are
neutralised at once and removed after the watchdog period, without `BotRemoved`.

**`0x0105 Ack`** (module → sidecar): `u32` request id.

**`0x0106 BotRemoved`** (module → sidecar), sent whenever one of this connection's bots stops existing.

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | reason: 1 `RemoveBot`, 2 map change, 3 vanished without a map change (no known admin path causes it) |

**`0x0109 Teleport`** (sidecar → module), for drills on test servers.

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| 3 × `f32` | position |
| `f32` | yaw, degrees |

Reply: `Ack` or `Error`. `Error` 6 unless `allowTeleport 1` (default 0), and when the bot is not alive. Non-finite or
out-of-world values (|x| or |z| above 8192, y outside -1000..10000) are `Error` 7. The module moves the bot's root
object (its vehicle when seated) with `setAbsoluteTransformation` to a level attitude at that yaw, then resets its
physics node (speeds zero) and wakes it. On foot the yaw does not stick: a soldier's facing comes from its own look
state, so turn it with look yaw.

**`0x010A Spot`** (sidecar → module)

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| `u32` | target: the networkable id of a root object |

Reply: `Ack` or `Error`. The bot must be alive and not man-down (`Error` 6). The target must exist and be an enemy: a
root object whose team, or whose occupant's team, is the other team (`Error` 5 otherwise). At most one spot per bot
per second (`Error` 6). The module sends what a human's spot sends: `IGame::sendSpottedMessage(bot, target, x/z,
type)` and the "spotted" radio line to the team. The type is the target template's
`IPlayerControlObjectTemplate::getVehicleType`, or 0 for a soldier. The module does not check that the bot can see
the target.

**`0x010B Say`** (sidecar → module): the bot sends a chat line as itself.

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | player id (a bot of this connection) |
| `u8` | channel: 0 Global or 1 Team |
| string | text |

Reply: `Ack` or `Error`. `Error` 4: the player is not a live bot of this connection. `Error` 7: another channel, or a
text that is empty, longer than 100 bytes or not printable ASCII (0x20-0x7E). `Error` 6: a second `Say` from the same
bot within a second. The bot can be in any state, and the engine's own rules apply: a dead or man-down sender reaches
only dead receivers (`GameServer::playerMessageEvent`), and the sender's spam counter is used
(`Player::getIsThisASpam`).

The module posts the line as the event a client's chat becomes (`StringManager::addStringDataChunk`,
`Game/Common/StringManager.cpp:169-207`): a `StringReceivedEvent` (event 11 on `ECNetwork`) with the bot's player id
as the sender, type 2 (the only type a client may send), target 0 and flags 0. It is posted with
`EventManager::postEventNextUpdate`, so the engine dispatches it at the next update, outside the module's tick
handler. `GameServer::handleStringReceivedEvent` (`Game/GameServer/GameServer.cpp:4289-4334`) then handles it as it
handles a client's line:

- `playerMessageEvent` relays it to every started connection.
- The Python chat event runs with `(sender id, text, channel, 0)`, so server scripts and the admin channel's
  `ChatMessage` see it as that player's chat.
- The module's own `Chat` forwarding sees it like any other line.

`GameServer::say` and `sayAll` cannot do this: they always use the local human player as the sender, and a dedicated
server has none (`GameServer.cpp:4778-4793`).

### Input

**`0x0201 SetInput`** (sidecar → module), 33 bytes.

| Type | Field | Engine slot |
|---|---|---|
| `u8` | player id | |
| `u32` | the tick number this input was computed from | |
| `f32` | cyclic pitch, -1..1 | `PIPitch` = 1 |
| `f32` | cyclic roll, -1..1 | `PIRoll` = 2 |
| `f32` | collective, -1..1 | `PIThrottle` = 3 |
| `f32` | pedals, -1..1 | `PIYaw` = 0 |
| `f32` | look yaw, -1..1 | `PIMouseLookX` = 4 |
| `f32` | look pitch, -1..1 | `PIMouseLookY` = 5 |
| `u32` | button bitmask | |

- Slot numbers are from `IO/Input/Input.h:198`. The module writes the values into the bot's `io::PlayerInput`, which
  the server reads for AI players every tick (`Game/GameServer/GameServer.cpp:3363`). From there on, bot and human
  input run the same code.
- The module also sets the slots' bits in `PlayerInput::mappingFlags`, using the mask `PlayerAction::get` produces
  (`PlayerAction.cpp:172-203`). The AI buffer starts with no bits set (`Player.cpp:4052`), and vehicle code reads an
  unmapped slot as 0.
- **Axes are latest-wins, buttons are ORed.** When several `SetInput`s for one bot arrive before a tick applies them,
  the axes are the last one's and the buttons are the OR of all of them, so a one-tick press is never lost. A release
  cannot cancel a press in the same tick.
- A set button bit presses the button for the one tick in which that `SetInput` is applied. A tick without a new
  `SetInput` presses nothing; the axes are held.
- Values outside -1..1 are clamped. NaN or infinity is rejected with `Error` 7 (request id 0) and the input is not
  applied.
- After clamping, the module quantizes each axis exactly as a human's input is quantized on the way to the server:
  `convertAxisToInternalResolution`, then `short(int(v * 100))`, then `/ 100` (`PlayerAction.cpp:27-112`). That
  truncates twice, so 0.57 arrives as 0.56. Bot input bypasses the engine's wire codec (`GameServer.cpp:3512`), so the
  module applies this itself to keep bots and humans on the same input resolution.

On foot, the axes mean:

| Field | Soldier effect | Sign |
|---|---|---|
| collective | walk forward and back | + forward |
| pedals | strafe | + right |
| cyclic pitch, roll | nothing | |
| look yaw | turn, about 5° per tick per unit (150°/s at 1.0) | + right (yaw increases) |
| look pitch | look up and down, clamped to ±85° | not measured |

Standing still, the first 40° of a turn moves only the head, and the body follows slowly. Turn while creeping forward
to point the body. In a pilot seat the look axes are ignored.

Buttons:

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
| 9-12 | camera views 1-4 | 10-12 are idempotent. 9 is not: from view 1 it selects the nose camera when the server allows it. To reach view 1 from any view, press 10, then 9. |
| 13-21 | weapon select 1-9 | gas mask and night vision flip every tick while held |
| 22 | next weapon | |
| 23 | fire mode | |
| 24-31 | change to seat 1-8 (F1-F8) | never into an occupied seat |

There is no exit-vehicle button. The only exit key is `PIUse`, whose enter half takes an untested AI code path
([R1b](#engine-requirements)), so it is excluded. `PIMouseLook` is excluded too: holding it in a pilot seat zeroes the
cyclic.

**`0x0202 SetTargetDirection`** (sidecar → module), 29 bytes: guide the bot's own TV-guided missile.

| Type | Field |
|---|---|
| `u8` | player id (a bot of this connection) |
| `u32` | the tick number this direction was computed from, for logs |
| 3 × `f32` | start: the eye point, metres (a human client sends its camera position) |
| 3 × `f32` | end: a point on the line of sight beyond start |

It is a control stream like `SetInput`, not a request: there is no reply on success.

- **Latest wins.** The module applies at most one per bot per tick, the last that arrived.
- **Refusals** are `Error` with request id 0, at most one per bot per tick:
  - 4: not a bot of this connection;
  - 7: a non-finite value, a point outside the world (as `Teleport`), or start and end closer than 0.1 m;
  - 6: the bot is not alive or is man-down, or none of its active weapons (`getActiveWeapon(0..2)`) is TV-guided
    (`IWeaponObject::getTargetSystemType() == TSTVGuided`, 4), or that weapon has no target object.
- **What it does:** `IWeaponObject::setTargetDirection(start, end)` on that weapon, the virtual the client's
  `TargetDirectionEvent::executeServer` calls (`GameEvents/TargetDirectionEvent.cpp:95-130`, `IWeaponObject.h:140`).
- It works with no missile in flight (it places the target the next launch flies to) and with one (the missile turns
  to the new target, within its follow rates).
- **Wire-guided weapons are refused**: on the server their target follows the player's camera every tick
  (`LPTargetComp.cpp:271-298`), which would overwrite a direction at once.

Where the target lands, and how to launch, is in [TV-guided missiles](#tv-guided-missiles).

### State

`TickState`, `VehicleList` and `ProjectileList` are latest-wins: if the sidecar falls behind, the module drops old
frames rather than queueing them, and the gap is visible in the tick numbers.

**`0x0301 TickState`** (module → sidecar), sent once per simulation tick.

| Type | Field |
|---|---|
| `u32` | tick number |
| `f64` | server simulation time, seconds |
| `u8` | number of entries N |
| N × entry | one per bot created on this connection, then one per other player in a vehicle |

Entry:

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | flags: bit 0 alive, bit 1 a bot of this connection, bit 2 in a vehicle, bit 3 man-down, bit 4 spotted |
| `u8` | team |
| `u8` | seat index, 255 if on foot |
| `u32` | root object's networkable id (the vehicle if in one, else the soldier) |
| 3 × `f32` | position x, y, z |
| 3 × `f32` | yaw, pitch, roll |
| 3 × `f32` | velocity x, y, z |
| 3 × `f32` | angular velocity x, y, z |
| `f32` | vehicle health, 0..1, or -1 if on foot or the vehicle has no armour component |
| `u8` | camera view mode of an own bot: 1 inside, 2 nose, 3 chase, 4 front chase, 5 fly-by, 6 trace, 7 extern trace, 0 none; 255 for other players and for a dead own bot |
| `u8` | weapon count W: 0 for other players, else the bot's active weapon slots that hold a weapon (at most 3) |
| W × weapon | |

Weapon:

| Type | Field |
|---|---|
| `u8` | slot: 0 fire (`PIFire`), 1 alt fire, 2 flares |
| string | weapon template name |
| `i16` | rounds in the loaded magazine (`IWeaponObject::getAmmoInMag(0)`), -1 for no ammo component or infinite |
| `i16` | magazines left including the loaded one (`getCurrentNrOfMags`), -1 when unknown |
| `u8` | flags: bit 0 reloading (`isReloading`); other bits 0 |
| `f32` | reload time left as a fraction 0..1 (`getReloadTimeLeftInPercent`), 0 when not reloading |
| `f32` | heat 0..1 (`getHeat`), -1 when the weapon does not use heat |

- Other players, including other connections' bots, appear only while in a vehicle.
- A bot of this connection that exists but is dead still has an entry, with the alive flag clear, position 0/0/0 and
  health -1. Ignore everything but its flags.
- **Man-down** (bit 3): after a kill the alive flag stays set for `sv.manDownTime`, because the soldier is man-down
  (`IPlayer::getIsManDown`).
- **Spotted** (bit 4): the entry's enemies spotted this player's root object within the last 12 s, the lifetime of the
  client's minimap marker (`Menu/Bf2MenuMapNode.cpp:842`). The server keeps no spotted state; the module keeps its own
  table.
- The weapons come from the seat's or soldier's `IPlayerControlObject::getActiveWeapon(0..2)`. A seat with two weapons
  on one fire input shows only the active one.

**`0x0302 VehicleList`** (module → sidecar), sent on the first tick after `Welcome` and then once a second (every 30
ticks), after that tick's `TickState`.

| Type | Field |
|---|---|
| `u32` | tick number, as in `TickState` |
| `u16` | number of entries N |
| N × entry | one per vehicle on the map |

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

- This is the sidecar's only source for a vehicle's type and for empty vehicles.
- The engine pools vehicle objects. A cleared wreck, or a vehicle waiting for its spawner, sits at 0/0/0 with no
  armour component (health -1, not flagged destroyed) until it is placed again. Ignore entries with health -1.
- A vehicle can come back under a different id after a respawn, so an id is only good for one pick.
- Team is 0 for every empty vehicle, including those on a base's pad, so only position tells whose it is.
- The module keeps its own vehicle set: a full walk at load, then 500 registered objects per tick, so a new vehicle
  appears within about a second.

**`0x0303 ProjectileList`** (module → sidecar), sent after `TickState` on every tick on which the connection has a
projectile to report, and once with N = 0 after the last one goes.

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
| `u8` | flags: bit 0 guided (has a seek component), bit 1 fired by a bot of this connection, bit 2 the owner id is valid |
| `u32` | networkable id of the guided target's root object, 0 if none |
| 3 × `f32` | position |
| 3 × `f32` | velocity |
| `f32` | time to live, seconds |
| `u32` | launch number: counts up from 1 for every launch the module tracks since it loaded, never reused |

- **Which projectiles:** those with a networkable id, a seek component or a detonation component
  (`IID_IProjectileDetonationComp`): missiles, rockets, grenades and explosive cannon shells. Ordinary bullets have
  none of the three on a dedicated server (`GenericProjectile.cpp:1357-1359`) and are not reported. Filter by template
  name for the ones you care about.
- A projectile is reported to a connection while it is active and within `projectileRange` metres (default 1000) of
  one of that connection's live bots, or was fired by one of them. The module tracks at most 256 at once.
- **The projectile id is reused across flights**, because the engine pools projectile objects. Use the launch number
  to tell flights apart.

**`0x0307 RoundState`** (module → sidecar), sent after `Welcome` and whenever a field other than the time changes.

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | game status (`IGameLogic::getGameStatus`: 1 playing, 2 end game, 3 pre game, ...), 255 if none |
| `u8` | begin-round state: 0 announce, 1 waiting for players, 2 pre-round countdown, 3 running, 255 if none |
| `f32` | seconds to the next state change, -1 if none |

### Events

`Damage`, `Kill` and `Chat` are control frames: they are never dropped while the connection lives.

**`0x0304 Damage`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | tick: the number of the first `TickState` whose state includes this damage. It can be off by one, because damage is applied both before and after the module's handler within a server tick. |
| `u8` | flags: bit 0 the victim is an own bot, the vehicle an own bot is in, or an own bot this damage killed; bit 1 the attacker is an own bot; bit 2 the victim's hit points reached 0; bit 3 this damage killed a player; bit 4 the victim player id is valid; bit 5 the attacker id is valid |
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
| 3 × `f32` | position the engine passed |

- A connection gets a `Damage` only when bit 0 or bit 1 is set: damage to its own bots and damage they deal. This is
  what a player sees as a health bar and hit markers. Amounts are raw hit points.
- The position is the projectile's start, the explosion centre, or 0/0/0 for a bleed. It is not the hit point.
- `Damage` keeps coming after a kill: a destroyed vehicle burns, with the killer still as attacker. Ignore events with
  hit points before at or below 0.
- A rocket hit is two events in one tick: the projectile and its splash, with the same source template.
- The source is the engine's damage observer, shared with the admin channel ([Engine requirements](#engine-requirements)).

**`0x0305 Kill`** (module → sidecar): every kill on the server, to every connection. This is the kill feed every
player sees.

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | victim player id |
| `u8` | victim team |
| `u8` | attacker player id, valid only with flag bit 3 (none for a crash) |
| `u8` | attacker team, 0 if none |
| string | weapon template name, empty if none |
| `u8` | flags: bit 0 the victim is an own bot, bit 1 the attacker is an own bot, bit 2 team kill, bit 3 the attacker id is valid |

**`0x0306 Chat`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | tick |
| `u8` | flags: bit 0 the sender is a player (else the server) |
| `u8` | sender player id, valid with flag bit 0 |
| `u8` | sender team, 0 if none |
| `u8` | channel, the engine's number: 0 Global, 1 Team, 12 ServerMessage, 13 ServerTeamMessage (`GameServer::handleStringReceivedEvent`) |
| string | text |

- The source is the module's `ECNetwork` listener for `StringReceivedEvent` (id 11), the event `GameServer` relays.
- Channels 0 and 12 (`game.sayAll`) go to every connection. Channel 1 goes only to connections with a bot on the
  sender's team. Channel 13 (`game.sayTeam`) goes only to connections with a bot on the team the event's flags carry,
  with that team in the team field. Every other channel (squad, commander, ...) is not forwarded.
- The text is converted from the engine's Windows-1252 to UTF-8 (`§`, byte 0xA7, arrives as U+00A7). Control bytes
  become `?`. It is cut to at most 255 bytes of UTF-8, never inside a character.

### Stepping

**`0x010C Step`** (sidecar → module), version 4 only.

| Type | Field |
|---|---|
| `u32` | ticks granted: added to the budget (capped at 300); 0 releases stepping |

No reply. `Error` 6 and close when `stepMode` is 0 or another connection is stepping. A wrong length is `Error` 2.
See [Stepped time](#stepped-time).

### Errors

**`0xFFFF Error`** (module → sidecar)

| Type | Field |
|---|---|
| `u32` | request id, 0 if none |
| `u16` | code |
| string | message for logs, at most 1024 bytes |

| Code | Meaning | Connection |
|---|---|---|
| 1 | version mismatch | closed |
| 2 | malformed frame | closed |
| 3 | unknown message type | closed |
| 4 | unknown player, or a player of another connection | kept |
| 5 | vehicle, seat or target unavailable | kept |
| 6 | refused: the engine, a switch in `serverext.cfg` or a rate limit said no | kept (closed for `Step`) |
| 7 | invalid value | kept |

## Spawning

A bot's home is its team's on-foot spawn group: the lowest id of an active spawn group the team can use whose spawn
points are not enter-on-spawn points (`IServerExtension::findSpawnGroup`). The engine spawns it there once the bot's
respawn delay is over.

A wreck, or a vehicle out of place, can block a group's spawn points. The module owns the fallback:

- If a bot waiting to spawn has not spawned within `spawnFallbackSeconds` (default 5) of a group becoming its target,
  the module moves it to the team's next active group, nearest to the home group's points first, and cycles through
  them. Groups without enter-on-spawn points come first; enter-on-spawn groups come last.
- An enter-on-spawn point puts the bot in the nearest free seat within 15 m, as a human spawning there would be
  (`BFfindEntryPoint`, behind the [R1b](#engine-requirements) guard).
- After a successful spawn the bot's target goes back to its home group, so the next death tries it first.
- Timers run only while the round is playing and the bot's respawn delay is over
  (`IServerExtension::getGameStatus`), so an end-of-round wait never counts as blocked.

**`0x0107 SpawnBlocked`** (module → sidecar): `u8` player id, `u16` seconds waiting. Sent when a bot has not spawned
for `spawnBlockedSeconds` (default 15) with every group tried, then every 10 s while it is still waiting.

**`0x0108 RestartRound`** (sidecar → module): `u32` request id. Reply: `Ack` or `Error`. It restarts the round as
`admin.restartMap` does, which clears the wrecks. It is allowed only with `allowRoundRestart 1` (default 0) and at
most once per 60 s across all connections; otherwise it is `Error` 6. A sidecar should send it only after
`SpawnBlocked`.

After a round restart the engine can spawn two bots on one spawn point in the same tick. That is retail behaviour.

## Watchdog

- If a bot gets no `SetInput` for `watchdogTicks` consecutive ticks (default 6, which is 200 ms), the module
  neutralises its input and logs it once.
- **In the pilot seat of a helicopter** (seat 0 of a vehicle whose template's category is `VCHelicopter`) the module
  flies level-and-hold on every tick after the watchdog trips: it levels pitch and roll, damps the yaw rate with the
  pedals and holds vertical speed at 0 with the collective (`src/Neutral.h`). It holds height, not position: expect
  6 to 10 m/s of drift. Holding the last collective with a centred cyclic is not safe: outside a hover it crashes the
  helicopter within seconds, because a centred cyclic keeps the current attitude.
- **In other vehicles and seats** cyclic and pedals go to 0 and the collective is held at its last value.
- **On foot** every axis is 0.
- On death the stored axes are zeroed, so a respawned bot stands still until the sidecar sends input.
- On disconnect a connection's bots are neutralised at once and removed after the watchdog period.
- The watchdog counts ticks, not seconds, so it behaves the same under [stepped time](#stepped-time).

## Map changes

On a map change the engine destroys every AI-flagged player itself (`GameServer.cpp:1416-1429`). `GameServer::unload`
posts `(ECHud, 3)` before it does. The module listens on `ECHud` for that event and ignores every other id, drops its
bots' ids without destroying them again, and sends `BotRemoved` with reason 2. The sidecar creates its bots again
after the load. A bot that disappears with no unload seen for two ticks is reported with reason 3.

On a dedicated server nothing else listens on `ECHud`, so those events are dispatched to the module instead of
dropped.

## Stepped time

Stepped time lets a test server run faster than real time, in lockstep with a sidecar. It needs the engine's tick
source ([R3](#engine-requirements)) and protocol version 4.

- `stepMode 1` in `serverext.cfg` (default 0) installs the module's tick source at load. **Never set it on a server
  humans play on**: the game then runs only as fast as a stepping sidecar allows.
- `Welcome`'s features bit 2 says stepping is available.
- Without a stepping connection the module grants ticks at 30 Hz from a steady clock (at most 10 a frame; a longer
  backlog is dropped, as `TickCalculator` does), so the server runs in real time.
- A connection's first `Step` makes it the stepping connection. From then on the server runs a tick only when that
  connection has granted it. That lasts until `Step` 0 or until the connection closes; then real time resumes at
  once.
- **Lockstep:** the sidecar grants one tick after it has sent its inputs for the last `TickState`. The I/O thread
  queues those inputs before it counts the grant (one connection, one thread, in order), so every input sent before a
  `Step` is applied in the tick that `Step` grants. Granting more than one tick ahead trades that guarantee for speed.
- `Step` is handled on the I/O thread, because the main thread does not tick while the budget is empty. With an empty
  budget the tick source waits up to 2 ms for a grant, and the I/O thread wakes it as soon as a `Step` arrives.
- Everything driven by ticks or by the engine's exact time runs at the stepped rate: physics, projectiles, round time,
  Python timers. Network timeouts, the end-of-round wait for clients' acknowledgements (`GameServer.cpp:1137-1146`)
  and the ban list stay on the wall clock. A stepped server is for bot tests without human clients.

## TV-guided missiles

A bot gunner flies a TV-guided missile with `SetTargetDirection`. What follows was measured on the attack helicopters
of the stock game (`ahe_ah1z`, `ahe_z10`).

**Launching.**

- The TV launcher is not selectable with weapon select or next weapon. One tick of **alt fire**, the seat's weapon
  toggle, switches the gunner seat to it, and **fire** launches.
- Sequence: toggle, wait for `TickState` to show the launcher in slot 0, send `SetTargetDirection`, then fire. A
  direction sent before the launch holds. Stream directions only while the launcher is active; otherwise every one is
  `Error` 6. A direction in the same tick as the toggle is refused.
- Without a direction the missile flies straight along the airframe.
- The gunner's view mode goes to 3 at launch for a few seconds (`useProjectileCamera`).
- `ProjectileList`'s target field is 0 for a TV missile.
- An empty launcher shows 0 rounds and 1 magazine, not reloading.

**Where the target lands.** `LPTargetComp::setTargetDirection(start, end)` on the server goes straight to
`updateTargetObject(start, end)` (`LPTargetComp.cpp:348-361`, `:374-497`), which:

- casts a ray from `start` through `end` out to the weapon's maximum distance, first against the terrain heightmap,
  then against objects, ignoring the weapon's own vehicle and the missile;
- stops at water: the ray is two-sided (`LPTargetComp.cpp:99-106`) and `HeightmapCluster` tests a two-sided ray
  against the sea level (`Physics/HeightmapCluster.cpp:555-570`);
- ignores an object on the weapon's own team (`:460-467`), so steering at a teammate marks the ground behind it;
- places the target on the object hit, else on the terrain or water hit, else at the maximum target distance along
  the ray.

**Turn rate.** The missile turns towards the target within its template's follow rates, so aim early: a late, large
retarget misses.

**One limitation** affects two gunners at once; see [Known limitations](#known-limitations).

## Notes for client authors

- **Death can be invisible.** With `sv.spawnTime` at or below `sv.manDownTime`, the alive flag never drops. Treat
  "the man-down bit dropped and the bot jumped to a spawn point" as a death.
- **A spotted soldier on foot is not visible** in `TickState`, which lists other players only in vehicles.
- **Rocket pods fire once per press.** Pulse the trigger, for example 3 ticks on and 3 off.
- **Reload time left reads 0 on the first reloading tick.**
- **With only bots on the server, `RoundState` stays at begin-round 1** (waiting for players). See
  [How these bots differ](#how-these-bots-differ-from-retail-ai-bots).
- **Server chat is channels 12 and 13; human chat is 0 and 1.**
- **`TickState`'s team is authoritative.** An admin can switch a bot's team; the module then re-resolves the bot's
  spawn groups and checks `EnterVehicle` against the new team.
- **A human teammate can evict a bot from its seat.** A human pressing the enter key prefers an AI-occupied seat
  (`PlayerControlObjectHelpers.cpp:456-466`), as with retail bots.

## Engine requirements

The module needs a few additions to the engine. They are non-retail behaviour, marked `[CHOSEN, not recovered]` in the
OpenBF2 source, kept as small as possible, and live on the `serverext/r1` branch of
[nihlen/OpenBF2](https://github.com/nihlen/OpenBF2/tree/serverext/r1). That branch's `docs/server-extension.md`
describes the engine side in full. The identifiers below are the ones the code comments use.

**`IServerExtension`.** A header-only interface that `GameServer` implements and registers as the singleton
`"OpenBF2ServerExtension"` before `openbf2.con` runs. The module reaches it with `getSingleton` and typed virtual
calls. The interface is append-only, and no retail vtable changes. It carries whatever the module needs from
`gameLogic` and `spawnManager`, neither of which is a registered singleton.

**R1. Create a bot player without the AI library.** `IGameServer::createPlayer(name, true)` refuses when no AI library
is loaded (`GameServer.cpp:371`), and the rebuilt server never loads one. With `false` the new player becomes the
server's local human. Registering a fake `"AI"` singleton is not an option: the engine references the AI instance in
many places outside `AI/`, and Python's `getIsAIGame()` would report true.

- `createInputPlayer(name)` returns the player id or -1. The code after the gate in `createPlayer` moved into a
  private helper that both functions call, so retail behaviour is unchanged.
- `findSpawnGroup(team, enterOnSpawn)` returns the lowest id of an active spawn group that team can use whose spawn
  points all match `enterOnSpawn`, or -1. `ISpawnPoint` exposes neither team nor control point, so the module cannot
  work this out itself.
- `listSpawnGroups(team, SpawnGroupInfo*, capacity)`, `restartRound()` (what `admin.restartMap` runs) and
  `getGameStatus()` serve [Spawning](#spawning).

**R1b. Null guards for the AI couriers.** These are required, because R1 alone can crash the server. An AI-flagged
player reaches three pointers that only the AI library fills. Each guard behaves as if no vehicle or target was found:

| Site | Trigger |
|---|---|
| `PlayerControlObjectHelpers.cpp:327-329` | An enter-on-spawn point, and a bot pressing `PIUse` |
| `GameServer.cpp:3340` | An enemy missile lock on a vehicle the bot is in. The module cannot prevent this. |
| `ServerGameLogic.cpp:3411-3415` | A defibrillator used on a non-soldier |

**R2. Getting a bot into a seat** needs nothing beyond R1 and R1b: the route is `CreateBot` and `EnterVehicle`. The
AI/human spawn flags (`ServerGameLogic.cpp:644-657`) and the first-wave refusal (`:1930-1938`, `gpm_coop` only) do not
block a bot on a conquest layer.

**R3. Stepped time.** `setTickSource(ITickSource*)`, where `ITickSource` is a header-only interface with `int
takeTicks(int maxTicks)`. Null, the default, leaves the retail frame loop unchanged.

- With a source set, each frame of `BF2Engine::mainLoop` asks it for the ticks to run (at most 10, the retail
  per-frame cap), advances the exact clock by exactly that many ticks with `System::advanceExactTime` (the demo
  recorder's fixed-step path, `Core/System.cpp:270`) instead of the wall clock, passes that count to `game->update`
  and skips the 30 Hz frame limiter. A frame with no ticks sleeps 1 ms.
- The source is set once, in `initDll`, and never cleared while the server runs: switching back to the wall clock
  would leave `System`'s wall reference behind the stepped clock.

**Observers.** The damage observer behind `Damage` and `Kill`, and the other hooks the admin channel uses, are
described in [admin-channel.md](admin-channel.md).

## How these bots differ from retail AI bots

- **They spawn without any human present**: `m_roundGoing` is set before the begin-round wait for humans
  (`ServerGameLogic.cpp:423`, `:452`). But with only bots the round never reaches Running: no round timer, no tickets,
  no scoring, no stats, and no Python handlers, timers or triggers except `alwaysCall` ones. Bots never count as
  humans, and in OpenBF2 `ServerSettings::getNumPlayersNeededToStart` returns at least 2
  (`ServerSettings.cpp:333-342`). **Two connected humans are needed for a real round.**
- `getIsAIGame()` is 0. Python sees a bot as `ai=1`, `remote=1`, `connected=0`, profile 0, address `N/A`, as it sees a
  retail bot.
- Python's `PlayerConnect` runs inside `createInputPlayer`, before the module sets the team, so a script first sees
  the bot on team 2. No `PlayerChangeTeams` follows.
- A kick or a ban does nothing to a bot.
- GameSpy reports `AIPlayer=1` per bot but `hasBots=0`.
- They get human weapon deviation and none of the AI library's squad, heal or ammo notifications.

## Known limitations

- **Linux x86-64 only.** There is no Windows build of the module.
- **A TV launch can be refused while another launcher's TV missile is in flight.** `ToggleCameraComp::requestFire`
  (`ToggleCameraComp.cpp:199-205`) holds a weak pointer to its last projectile that is not invalidated when the
  missile returns to the object cache. The cache hands the same object to the other launcher, so the stale pointer
  resolves to a missile in flight and the launch is refused while the other gunner is in its projectile camera. A bot
  avoids it by pressing alt fire twice after each launch (to the gun and back to the launcher): that clears the
  pointer and leaves the projectile camera, while the missile keeps following `SetTargetDirection`. Whether the
  retail server behaves the same is not known.
- **The client's control mapping is not reproduced.** From the server's `PlayerInput` on, bot and human input run the
  same code. A human's input also passes the client's ControlMap (deadzone, scale, inversion) before it reaches the
  server; whether that stage matches a bot's direct axis values is not measured.
- **Event category 7.** The per-tick hook uses a category nothing in the OpenBF2 tree uses. Whether unrecovered
  retail code uses it is not verified. The fallback is a one-line hook in `GameServer::simulateFrame`, between
  `processEvents` and `simulatePlayersUpdate` (`GameServer.cpp:2530-2531`), registered through `IServerExtension`.
- **Not every R1b guard has run on a server.** The enter-on-spawn guard is exercised by the spawn fallback. The
  missile-lock and defibrillator guards are in place but have not been triggered deliberately. The sign of look pitch
  is not measured.
- **Bot chat depends on a layout copy.** The module posts a `StringReceivedEvent` built from a copy of the engine
  struct, checked field by field with `static_assert` against `Game/Common/StringManager.h`. A layout change there
  fails the module build; a change in how the engine handles the event would not.

## Versioning

- The version in `Hello` and `Welcome` changes when a wire change needs an engine change. The module accepts the
  current version and the versions it lists as compatible (now 3 and 4).
- While the project is in its development phase, a wire addition that needs no engine change keeps the version. `Say`
  and `ProjectileList`'s launch number were added that way.
- This document is the only definition of the protocol. The code follows it: a difference between them is a bug in
  one of the two, to be settled here first. Clients never redefine a message locally.
- `tools/testclient/serverext_client.py` speaks the whole protocol (Python 3, standard library only) and has a
  scenario per feature. It must pass against a running server before a client depends on a change.
