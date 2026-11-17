# Admin channel

The admin channel is a typed, binary protocol between the server extension module (`libserverext.so`, running inside
an OpenBF2 dedicated server) and an admin client. [BF2WebAdmin](https://github.com/nihlen/BF2WebAdmin) is the client it
was built for; there the channel replaces the text protocol of `mm_webadmin.py`. This document is the reference for
the protocol: a client can be implemented from it alone.

The channel gives a client:

- a full snapshot of the server on every connect, then sequence-numbered events;
- state streams (players, vehicles, projectiles, game state, aim) at a rate the client picks, up to 30 Hz;
- typed commands with typed replies (chat, teleport, health, teams, score, kick, ban, maps, objects, votes, HUD
  events);
- scripts: JavaScript the client pushes and the module runs inside the server tick;
- the server's health every 10 s: how regularly its ticks run, what a tick costs, what the host does to it, and
  how each player's connection carries packets.

The bot channel, the module's other protocol, is described in [protocol.md](protocol.md). The two share framing and
codec style and nothing else: they use separate connections, versions and type ranges.

## Contents

- [Design](#design)
- [Transport and security](#transport-and-security)
- [Wire format](#wire-format)
- [Session](#session)
- [State](#state)
- [Events](#events)
- [Hit/miss replay](#hitmiss-replay)
- [Commands and replies](#commands-and-replies)
- [Command semantics](#command-semantics)
- [Client presentation commands](#client-presentation-commands)
- [Client resync](#client-resync)
- [Server votes](#server-votes)
- [Scripts](#scripts)
- [Seat preferences](#seat-preferences)
- [Server health](#server-health)
- [Engine interface](#engine-interface)
- [Cost](#cost)
- [Testing a client](#testing-a-client)
- [Not implemented](#not-implemented)
- [Known limitations](#known-limitations)

## Design

1. **Typed and id-based.** Players are referred to by player id; vehicles, objects and projectiles by networkable id
   or console id. Nothing travels as text to be parsed.
2. **State, not only events.** The module builds a world snapshot every tick. The client subscribes to the parts it
   wants at the rate it wants. Events carry full context: ids, templates, positions, vehicles.
3. **Nothing blocks the game thread.** The game thread only reads engine state and queues frames. All socket and TLS
   work happens on the admin channel's own I/O thread, which never calls the engine.
4. **Resynchronise by snapshot.** Every link starts with a full snapshot. Events are numbered; a client that sees a
   gap asks for a new snapshot. The connection is never silently left inconsistent.
5. **Plain streams.** A stream frame is a full copy of the subscribed entities, not a delta. Diffing would cost
   little CPU but needs a baseline per subscriber, field masks, entity lifetimes and a full frame after every gap, in
   both codecs. A moving player's position and rotation change every frame anyway. See [Cost](#cost) for the sizes.
6. **One protocol family.** Framing, error handling and codec style are the bot channel's. Golden frames are shared
   by the C++ codec and client codecs ([Testing a client](#testing-a-client)).

## Transport and security

### Connection

- **The module dials out.** It connects to `adminConnect host:port` from `serverext.cfg`; the client listens. One
  client can serve many game servers, and a game server needs no inbound port for administration.
- The admin channel is off unless `adminConnect` is set.
- **Reconnect.** After an authenticated link closes, the module retries after 1 s. After a failed attempt (refused,
  TLS failure, wrong proof, timeout) the delay doubles from 1 s up to 60 s. Every delay has ±20% jitter. A changed
  key or pin on the client side therefore takes effect within about 60 s.
- **Handshake timeout.** The whole handshake, TLS included, must finish within 10 s of the TCP connect.

### Encryption

- TLS 1.3 only, in the module, with OpenSSL 1.1.1 or 3.x.
- The client presents a certificate; a self-signed one is enough. The module pins it: `adminCertSha256` is the
  SHA-256 of the certificate's SubjectPublicKeyInfo. The module checks the leaf's public key against the pin and
  ignores the CA chain, host names and expiry. Rotating the client's key means updating the pin on every server.
- A pin mismatch fails the TLS handshake before the module sends anything.
- `adminTls 0` sends in the clear. The module accepts it only when `adminConnect` is a loopback address (`127.x.x.x`
  or `localhost`). It exists for tests.

### Authentication

- Mutual HMAC-SHA256 challenge-response with a key per server (`adminKey`). Both sides prove they hold the key
  ([Session](#session)).
- The client proves the key first. The module sends no server information to a peer that has not proven it.
- The server's identity is its configured server id (`adminServerId`), not its source address.
- The proofs are not bound to the TLS session. The pinned certificate is what prevents a relay; the HMAC proves the
  key.

### What a server allows

Whoever holds the key and the client's certificate key controls the server through this channel. `serverext.cfg` can
narrow that per server, so a leaked key cannot do more than the server allows:

| Key | Effect when 0 |
|---|---|
| `adminAllowConsole` | `Console` is answered with `Error` 9. Without it the client cannot run arbitrary console lines |
| `adminAllowObjects` | `SpawnObject` and `DeleteObject` are answered with `Error` 9 |
| `adminAllowScripts` | `StartScript` is answered with `Error` 9 and capability bit 3 is clear |
| `adminSendIdentity` | players' IP addresses and CD-key hashes are withheld (empty strings) |

All four default to 1, except that `adminSendIdentity` defaults to 0 without TLS. `Welcome` reports the result in its
capability bits. The raw console can do most of what the other switches guard, so turning off objects or scripts
without turning off the console narrows little.

Every key, with its default and range, is in [configuration.md](configuration.md#admin-channel). A bad value in
`serverext.cfg` makes the module refuse to load.

## Wire format

Admin protocol version 1.

### Framing

Every message is a frame, with the bot channel's header:

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | payload length in bytes (excludes this header) |
| 4 | `u16` | message type |
| 6 | `u16` | reserved, 0 |
| 8 | bytes | payload |

- All integers and floats are little-endian. Floats are IEEE-754.
- A string is a `u16` byte length followed by UTF-8 bytes, with no terminator.
- **Frame size.** A client → module payload is at most 64 KiB. A module → client payload is at most 1 MiB, because
  `Snapshot`, `ObjectList` and `ConsoleResult` can be large.
- **Type range.** Admin types are `0x1001`-`0x15FF`. `0xFFFF Error` is shared with the bot channel and has the same
  layout. Nothing else on this connection is a bot-channel message.
- Every payload must be exactly the message: a short or long payload is a framing error.

### Types used in the layouts

- **Player ids** are `u8` where a player must exist and `i16` where none is possible (`-1` = none). Bots created
  through the bot channel get ids counting down from 255, so no `u8` value can mean "none".
- **Networkable id:** `u32`, 0 = none. The engine's id is 16 bits.
- **Console id:** the object's grid id, the `N` in the console's `object.active idN` (`IObject::getObjectId`,
  resolved by `ObjectManager::getObjectFromGrid`, `Scene/Object/ObjectTemplateUtil.cpp:76-85`), as `u32` with
  `0xFFFFFFFF` = none. Static map objects can have a console id and no networkable id.
- **`ObjectRef`** (commands that act on an object):

  | Type | Field |
  |---|---|
  | `u8` | kind: 1 a player's root object (its vehicle when seated, else its soldier), 2 networkable id, 3 console id |
  | `u32` | id: the player id for kind 1 |

- **`Vec3`** is 3 × `f32` x, y, z: metres, or m/s for velocities. World axes are x east, y up, z north.
- **`Rot`** is 3 × `f32` yaw, pitch, roll in degrees, the bot channel's convention
  ([protocol.md](protocol.md#conventions)): yaw 0 = north and 90 = east, pitch positive = nose down, roll positive =
  bank left. It equals Python's `getRotation`: with `m` the object's absolute transform, the angles are
  `(atan2(m8, m10), -asin(m9), atan2(m1, m5))` (`Game/Python/py/Objects.cpp:611-628`, `Math/Mat4.cpp:234-264`).
  Setting a rotation uses the exact inverse, `setRotationZXY(-roll, -pitch, -yaw)` (`Math/Mat4.cpp:167-232`).
- **Text** is UTF-8 on the wire and Windows-1252 in the engine. The module converts both ways, so `§` (U+00A7, BF2's
  colour-code prefix) is byte 0xA7 in the engine and colour codes keep working. Inbound text that is not valid UTF-8
  or contains control characters is `Error` 7. A character with no Windows-1252 byte becomes `?`. In outbound text,
  control bytes become `?`.

### Order

- The game thread runs the client's commands in arrival order, at most 64 per tick; the rest run on the next tick.
  It queues each reply when the command has run.
- Replies, snapshots and events share one ordered queue and go out in the order they were queued.
- Stream frames are the exception: latest wins. An unsent stream frame is replaced by the next one, and stream
  frames are written after the queued frames of each flush.

### Sequence numbers and resync

- Every event carries a `u32` sequence number. Numbers count per link session: the first event after the `Snapshot`
  that follows `Welcome` is 1, and each later event on that session is the previous plus one.
- **`Snapshot.lastSeq`** is the number of the last event whose effect the snapshot already includes. The client
  drops events with a number at or below it and applies the rest.
- **Gaps.** The module never blocks on a slow client. When its outbound queue exceeds `adminQueueBytes` (default
  4 MiB), it drops new events instead of queueing them. Their numbers are used up, so the client sees the gap and
  sends `RequestSnapshot`. Replies and snapshots are never dropped.
- **Overload.** If the queue reaches four times `adminQueueBytes`, the module closes the link and reconnects.
- **Reconnect.** A new link gets a new session number in `Welcome`, a full `Snapshot` and numbering from 1.
  Subscriptions and scripts do not survive a reconnect.

### Errors

**`0xFFFF Error`** (either direction)

| Type | Field |
|---|---|
| `u32` | request id, 0 if none |
| `u16` | code |
| string | message, for logs |

| Code | Meaning |
|---|---|
| 1 | version mismatch |
| 2 | malformed frame |
| 3 | unknown message type |
| 4 | unknown player |
| 5 | unknown object, or unavailable |
| 6 | refused by the engine or by a rule |
| 7 | invalid value |
| 8 | authentication failed |
| 9 | disabled by this server's `serverext.cfg` |

- A framing or parse error (a length over the limit, a nonzero reserved field, a payload that is not exactly the
  message) is `Error` 2, then close.
- An unknown type, or a type sent in the wrong direction, is `Error` 3, then close.
- Per-request errors (4 to 7 and 9) carry the request id and keep the connection. A value check that fails while
  parsing a command (a range, a flag combination, bad text) is `Error` 7 for that request, answered in order with
  the other commands.

### Versions and capabilities

The protocol version is 1 and is not negotiated. Later additions do not change it: they are new message types, new
streams, or fields with a capability bit in `Welcome`. A module answers a type it does not know with `Error` 3 and
closes the link, so:

- the client sends the script commands, `SeatPreference`, `SetCollision` and `ResyncClient` only when the matching
  capability bit is set, and subscribes to stream 21 only when capability bit 6 is set;
- commands without a capability bit (`StartVote`, `CancelVote`, `HudEventRaw`, `RoundClock`, `SquadOrder`) and the
  current structure layouts need a module that has them. Module and client are deployed together.

## Session

1. The module connects, completes TLS and sends `Hello`.
2. The client answers `Challenge`.
3. The module checks the client's proof, then sends `Auth`, `Welcome` and `Snapshot` without waiting.
4. The client checks the module's proof before it uses anything after it.

**`0x1001 Hello`** (module → client)

| Type | Field |
|---|---|
| `u16` | admin protocol version, 1 |
| string | server id (`adminServerId`, 1-64 printable ASCII bytes without spaces) |
| 32 bytes | module nonce `Nm` (random, from OpenSSL `RAND_bytes`) |
| string | module build, for logs (for example `libserverext admin 1`) |

**`0x1002 Challenge`** (client → module)

| Type | Field |
|---|---|
| 32 bytes | client nonce `Nw` (random) |
| 32 bytes | client proof `Pw` |

**`0x1003 Auth`** (module → client)

| Type | Field |
|---|---|
| 32 bytes | module proof `Pm` |

Proofs, with `K` the raw key (`adminKey`, 16-64 bytes, written as hex in the configuration) and `id` the server id
bytes:

```
T(label) = label || 0x00 || u16le(len(id)) || id || Nm || Nw
Pw = HMAC-SHA256(K, T("OBF2-ADMIN-1 webadmin"))
Pm = HMAC-SHA256(K, T("OBF2-ADMIN-1 server"))
```

- The labels are ASCII. Different labels mean neither side's proof can be replayed as the other's.
- Compare proofs in constant time (OpenSSL `CRYPTO_memcmp`; .NET `CryptographicOperations.FixedTimeEquals`).
- A version other than 1 in `Hello`: the client sends `Error` 1 and closes.
- An unknown server id, or a server with no key: the client sends `Error` 8 and closes.
- A bad `Pw`: the module sends `Error` 8, closes and backs off as after any failed attempt.
- A bad `Pm`: the client sends `Error` 8 and closes.

**`0x1004 Welcome`** (module → client)

| Type | Field |
|---|---|
| `u16` | admin protocol version, 1 |
| `u32` | session number: 1 for the first link since the server process started, then +1 per link |
| `u32` | current module tick |
| `f32` | tick rate, Hz (30) |
| string | server name (`sv.serverName`) |
| `u16` | game port (`sv.serverPort`) |
| `u16` | GameSpy query port (`sv.gameSpyPort`) |
| `u8` | maximum players (`sv.maxPlayers`) |
| `u8` | capabilities, below |
| string | current map name |
| string | current game mode |
| `u16` | current map size (`IGame::getCurrentMapSize`) |
| `u16` | current map-list index, `0xFFFF` if none |
| `u16` | map-list entries M (at most 256) |
| M × entry | string name, string game mode, `u16` size: the server's rotation (`maplist.list`), every mode |
| string | team 1 name |
| string | team 2 name |
| `GameState` | [State](#state) |

Capability bits:

| Bit | Meaning |
|---|---|
| 0 | `Console` is allowed (`adminAllowConsole`) |
| 1 | `SpawnObject` and `DeleteObject` are allowed (`adminAllowObjects`) |
| 2 | IP addresses and CD-key hashes are sent (`adminSendIdentity`) |
| 3 | scripts can be run ([Scripts](#scripts)) |
| 4 | the module accepts `SeatPreference` ([Seat preferences](#seat-preferences)) |
| 5 | the module accepts `SetCollision` ([Command semantics](#command-semantics)) |
| 6 | the module has stream 21, `ServerHealth` and `PlayerConnections` ([Server health](#server-health)) |
| 7 | the module accepts `ResyncClient` ([Client resync](#client-resync)) |

### Keepalive

Either side may send `Ping`; the other answers `Pong`.

| Type | Message | Fields |
|---|---|---|
| `0x1501` | Ping | `u32` request id, `u64` the sender's clock |
| `0x1502` | Pong | `u32` request id, `u64` the echoed clock |

- The client's `Ping` is answered by the game thread, in order with the commands, so a `Pong` also shows that the
  server is ticking.
- The module sends `Ping` after 15 s without receiving anything and closes the link after 45 s without receiving
  anything.

## State

### Shared structures

**`GameState`** (in `Welcome`, `Snapshot`, `GameStream` and the `GameState` event)

| Type | Field |
|---|---|
| `u8` | game status (`IGameLogic::getGameStatus`): 1 playing, 2 end game, 3 pre game, 4 paused, 5 restart, 6 not connected; 0 none |
| `u8` | begin-round state: 0 announce, 1 waiting for players, 2 pre-round countdown, 3 running; 255 none |
| `f32` | seconds to the next state change, -1 if none |
| 2 × `i32` | tickets of teams 1 and 2, -1 if unknown |
| `f32` | round time: seconds since the round started, -1 if not started |
| `f32` | time limit, seconds (`sv.timeLimit`), 0 if none |
| `u8` | winner of the last round, 0 if none |
| `u8` | victory type of the last round, 0 if none |

The engine has no team score besides tickets. A player's team score is in `PlayerScore`. The round time stays -1
until the round reaches the running state.

**`PlayerState`** (per player, in `PlayerInfo` and `PlayerStream`; 58 bytes)

| Type | Field |
|---|---|
| `u8` | team |
| `u8` | squad, 0 if none |
| `u8` | life: 0 dead, 1 alive, 2 man-down (alive and down; `IPlayer::getIsManDown`) |
| `u8` | flags: bit 0 seated in a vehicle, bit 1 commander, bit 2 squad leader |
| `u16` | ping, ms |
| `u32` | root object's networkable id (its vehicle when seated, else its soldier), 0 if none |
| `u32` | root object's console id, `0xFFFFFFFF` if none |
| `u8` | seat (the PCO id), 255 on foot or dead |
| `Vec3` | root position (0/0/0 when dead) |
| `Rot` | root rotation |
| `Vec3` | root velocity (its physics node) |
| `f32` | soldier health 0..1 (its armour), -1 if none |
| `u8` | camera view mode: 1 inside, 2 nose, 3 chase, 4 front chase, 5 fly-by, 6 trace, 7 extern trace, 0 none; 255 when dead or without a camera |
| `u16` | lag compensation, ms: how far back the server tests this player's projectiles against other vehicles and soldiers (0 to 300; 33 or less rewinds nothing). See [Hit/miss replay](#hitmiss-replay) |

- The view mode is the server's copy (`IPlayer::getCamera()->getViewMode()`). The server applies each player's
  camera keys to its own camera object (`ServerGameLogic.cpp:1550-1579`, called for every player from
  `GameServer.cpp:3788`), and projectile cameras set it too (`ToggleCameraComp.cpp:116-160`). A key press the server
  never received is not reflected.
- The lag is the engine's own value, `LatencyCompensation::getLatency(player)`: ping × 0.5 plus the interpolation
  time the player's client reports, capped by `phy-latencyComp-max` (0.3 s), or the forced `phy-latencyComp-debug`.
  Bots are local players and get 0. The module rounds it to whole milliseconds.

**`PlayerScore`** (in `PlayerInfo` and `ScoreChange`)

| Type | Field |
|---|---|
| `i32` | score |
| `i32` | team score |
| `i32` | kills |
| `i32` | deaths |
| `i32` | team kills |
| `i16` | rank, -1 if none |

The field mapping is that of Python's score setter (`Game/Python/PythonHost.cpp:1474-1511`).

**`PlayerInfo`** (per player, in `Snapshot` and `PlayerConnect`)

| Type | Field |
|---|---|
| `u8` | player id |
| string | name |
| `u8` | flags: bit 0 AI-flagged (any bot), bit 1 created through the module's bot channel, bit 2 connected (`IPlayer::getIsConnected`), bit 3 the IP and hash below are sent |
| `i32` | profile id |
| string | IP address, empty if withheld or none (bots) |
| string | CD-key hash, empty if withheld or none |
| `PlayerScore` | |
| `PlayerState` | |

The CD-key hash is the client's own: the first 32 characters of its GameSpy CD-key response
(`External/GameSpy/gcdkey/gcdkeys.c:216-275`, `:396-414`). The engine holds one only on an internet server
(`sv.internet 1`) with a working GameSpy CD-key backend (`Game/MatchMaking/Server/MatchMakingServer.cpp:87-98`). It
is empty for bots and for every player on a LAN server. A client that keys players on the hash needs another key
there.

**`VehicleState`**

| Type | Field |
|---|---|
| `u32` | networkable id |
| `u32` | console id |
| string | template name |
| `u8` | team, 0 if neutral or empty |
| `u8` | flags: bit 0 destroyed (hit points at or below 0) |
| `Vec3` | position |
| `Rot` | rotation |
| `Vec3` | velocity |
| `f32` | health 0..1, -1 with no armour component |
| `u8` | seats |
| `u32` | occupied seats, bit 0 = seat 0 |

The set is the bot channel's vehicle set ([protocol.md](protocol.md)): every root object with seats that is not a
soldier or a camera. The engine pools vehicle objects, so the set includes pooled entries at 0/0/0 with health -1 (a
cleared wreck, or a vehicle waiting for its spawner).

**`ControlPointState`**

| Type | Field |
|---|---|
| `u32` | console id |
| `u32` | networkable id, 0 if none |
| string | template name |
| `u8` | team (`IControlPoint::getTeam`) |
| `u8` | flag team (`IControlPoint::getFlag`) |
| `f32` | take-over counter 0..1 (`getTakeOverCounter`) |
| `u8` | capturing state (`getCapturingState`) |
| `Vec3` | position |

**`ProjectileState`**

| Type | Field |
|---|---|
| `u32` | id: the networkable id, or the engine object id plus 0x10000 when it has none |
| string | template name |
| `i16` | owner player, -1 if none |
| `u8` | owner team |
| `u8` | flags: bit 0 guided (it has a seek component) |
| `u32` | networkable id of the guided target's root, 0 if none |
| `Vec3` | position |
| `Rot` | rotation |
| `Vec3` | velocity |
| `f32` | time to live, seconds |

- Only projectiles whose template is in a projectile subscription (stream 3 or 17) are tracked, at most 256 at once.
- The engine pools projectile objects, so an id is reused across flights.

**`AimState`** (per living player, in `AimStream`; 50 bytes)

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | flags: bit 0 the camera fields are set, bit 1 the weapon fields are set |
| `Vec3` | camera position: the absolute transform of the player's camera object, 0/0/0 without bit 0 |
| `Rot` | camera rotation, the same matrix |
| `Vec3` | weapon position: the absolute transform of the active weapon in slot 0 of the player's seat (`IPlayerControlObject::getActiveWeapon(0)`; on foot the soldier's), 0/0/0 without bit 1 |
| `Rot` | weapon rotation, the same matrix |

- These are the two transforms the server fires along (`FireComp::getProjectileOrigin`,
  `Game/Objects/Weapons/FireComponents/FireComp.cpp:807-865`): the weapon's own, or the camera object's when the
  weapon fires in camera DOF (`fire.fireInCameraDof`) or the shooter is a soldier. The template's
  `projectileStartPosition` offset is added to it (`:874-885`) and is not included here.
- The camera is the server's copy of the view: the eye in views 1 and 2, the external camera in views 3 to 7.
- Soldiers fire from the camera object. Use the camera for a soldier's aim.

**`WeaponSlot`** (in the `Weapons` event): `u8` slot (0-2), string weapon template.

### Snapshot

**`0x1101 Snapshot`** (module → client): sent after `Welcome`, and in reply to `RequestSnapshot`.

| Type | Field |
|---|---|
| `u32` | request id: 0 after `Welcome`, else `RequestSnapshot`'s |
| `u32` | `lastSeq` ([Sequence numbers and resync](#sequence-numbers-and-resync)) |
| `u32` | tick |
| `f64` | server time, seconds |
| `GameState` | |
| `u8` | players P, then P × `PlayerInfo` |
| `u16` | vehicles V, then V × `VehicleState` |
| `u16` | control points C, then C × `ControlPointState` |
| `u16` | projectiles R, then R × `ProjectileState` (the subscribed templates only) |

**`0x1102 RequestSnapshot`** (client → module): `u32` request id. Reply: `Snapshot`.

### Subscriptions

**`0x1103 Subscribe`** (client → module). Reply: `Ok`, or `Error` 7.

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | stream, below |
| `u8` | rate, Hz, 1..30 (ignored for streams 16 to 21) |
| `u8` | filters F (at most 64) |
| F × string | template names, 1-256 bytes each, compared without case |

| Stream | Content | Filters |
|---|---|---|
| 1 | `PlayerStream` | none |
| 2 | `VehicleStream` | none |
| 3 | `ProjectileStream` | at least one projectile template |
| 4 | `GameStream` | none |
| 5 | `AimStream` | none |
| 16 | `Damage` events | none |
| 17 | `ProjectileFired` and `ProjectileHit` events | at least one projectile template |
| 18 | `WeaponFire` events | any number. None: every launch. Else: launches whose weapon or projectile template matches one |
| 19 | `Weapons` events | none |
| 20 | `ProjectileCollision` and `ProjectileTrace` events | at least one projectile template |
| 21 | `ServerHealth` and `PlayerConnections` events | none |

- Streams 1 to 5 are state streams with a rate. The module sends a frame every `round(30 / rate)` ticks.
- Streams 16 to 20 switch on event types that are off by default because of their volume. Every hit is one
  `Damage` event.
- Stream 21 switches on `ServerHealth` and `PlayerConnections`, one of each per 10 s. It is off by default so that a
  client which does not know the events never receives them.
- A new `Subscribe` for a stream replaces the old one.
- Subscribing to stream 19 reports every player's weapons at once.

**`0x1104 Unsubscribe`** (client → module): `u32` request id, `u8` stream. Reply: `Ok`, or `Error` 7 for an unknown
stream.

**Stream frames** (module → client, latest wins):

| Type | Payload |
|---|---|
| `0x1111 PlayerStream` | `u32` tick, `f64` time, `u8` N, N × (`u8` player id, `PlayerState`) |
| `0x1112 VehicleStream` | `u32` tick, `f64` time, `u16` N, N × `VehicleState` |
| `0x1113 ProjectileStream` | `u32` tick, `f64` time, `u16` N, N × `ProjectileState` |
| `0x1114 GameStream` | `u32` tick, `f64` time, `GameState` |
| `0x1115 AimStream` | `u32` tick, `f64` time, `u8` N, N × `AimState` (living players only) |

## Events

Events are `0x12xx`, module → client. Every event starts with a `u32` sequence number and a `u32` tick. The fields
below follow them.

Most events come from the engine's script events, the same events Python handlers receive. The module observes them
before Python's handlers run and reads the objects during the callback. "Script event N" in the source column is the
engine's script event with that id.

| Type | Event | Fields after seq and tick | Source |
|---|---|---|---|
| `0x1201` | GameState | `GameState`, `u8` previous status | every game status change, Paused included (`ServerGameLogic.cpp:311-318`), and begin-round state changes, polled per tick |
| `0x1202` | RoundStart | string map, string mode, 2 × `i32` tickets | the status becomes 1 (playing) from anything except 4 (paused) |
| `0x1203` | RoundEnd | 2 × `i32` tickets, `u8` winner (0 none), `u8` victory type | the status becomes 2 (end game); the engine sets the winner first (`ServerGameLogic.cpp:883-897`) |
| `0x1204` | PlayerConnect | `PlayerInfo` | script event 14 |
| `0x1205` | PlayerDisconnect | `u8` player, `u8` reason, `u16` engine reason (`0xFFFF` if none) | script event 15 and the engine's pending disconnect type |
| `0x1206` | Spawn | `u8` player, `u8` team, `u32` root networkable id, `u32` root console id, string root template, `Vec3` position, `Rot` rotation | script event 23 while the player is not yet alive |
| `0x1207` | Death | `u8` player, `u8` team, `u32` networkable id, string template of the object the player was in, `Vec3` its position | script event 3 |
| `0x1208` | Revive | `u8` revived player, `i16` medic | script event 2 |
| `0x1209` | TeamChange | `u8` player, `u8` old team, `u8` new team, `u8` flags (bit 0 bot-channel bot) | the module compares every player's team each tick |
| `0x120A` | ScoreChange | `u8` player, `PlayerScore` | the module compares every player's score fields each tick |
| `0x120B` | Kill | below | script event 1 |
| `0x120C` | Damage | below; only while subscribed (stream 16) | the engine's damage observer |
| `0x120D` | EnterVehicle | `u8` player, `u8` seat, `u8` flags (bit 0 the child soldier is active), `u32` root networkable id, `u32` root console id, string root template, string seat template | script event 21, non-soldier objects only |
| `0x120E` | ExitVehicle | the same fields, for the vehicle left | script event 22 |
| `0x120F` | VehicleDestroyed | `u32` networkable id, `u32` console id, string template, `i16` attacker, `Vec3` position | script event 24, vehicles only |
| `0x1210` | ControlPoint | `u8` kind (1 captured, 0 neutralised), `ControlPointState` | script event 25, reported at the module's next tick |
| `0x1211` | Chat | `i16` sender (-1 the server), `u8` sender team (for channel 13 the target team), `u8` channel, string channel name, `u32` engine flags, `u8` text flags (bit 0 a dead prefix was removed), string text | script event 39 |
| `0x1212` | ProjectileFired | `ProjectileState` | the engine's projectile launch observer; subscribed templates only (stream 17) |
| `0x1213` | ProjectileHit | `u32` id, string template, `i16` owner, `u8` flags (bit 0 expired: its time to live ran out; bit 1 it damaged something), `u32` networkable id of the first object it damaged (0 none), `Vec3` last position | a tracked projectile is no longer in flight (stream 17) |
| `0x1214` | Kick | `u8` player, string name | script event 17: every kick, ban and vote kick (`BanList::alertOnKicked`) |
| `0x1215` | Ban | `u8` player, string name, `u8` ban type (0 timed, 1 permanent, 2 round), `u32` expiry (Unix time; 0 permanent) | script event 16 (`BanList::alertOnBanned`) |
| `0x1216` | Bot | `u8` player, `u8` kind (1 created, 2 removed, 3 team change), `u8` team, `u32` bot-channel connection | the module's bot channel |
| `0x1217` | WeaponFire | below; only while subscribed (stream 18) | the projectile launch observer |
| `0x1218` | Weapons | `u8` player, `u8` W, W × `WeaponSlot` | only while subscribed (stream 19) |
| `0x1219` | ProjectileCollision | [Hit/miss replay](#hitmiss-replay); only while subscribed (stream 20) | the engine's projectile-collision observer |
| `0x121A` | ProjectileTrace | [Hit/miss replay](#hitmiss-replay); only while subscribed (stream 20) | the module's tick hook and the projectile observers |
| `0x121B` | VoteResult | `u32` request id of the `StartVote`, string question, `u8` outcome (1 passed, 2 failed, 3 cancelled by `CancelVote`, 4 cancelled by a level unload), `u16` yes, `u16` no, `u16` voters | [Server votes](#server-votes) |
| `0x121C` | ScriptFinished | `u32` instance, string name, `u8` reason, string result, `u32` calls, `u32` CPU time of all calls in microseconds, `u32` CPU time of the longest call in microseconds | [Scripts](#scripts) |
| `0x121D` | ScriptEvent | `u32` instance, `u8` kind (1 data: JSON text, 2 a log line), string payload (at most 8192 bytes) | [Scripts](#scripts) |
| `0x121E` | ServerHealth | [Server health](#server-health); only while subscribed (stream 21) | the module's tick hook, the main thread's clocks and the host's `/proc` |
| `0x121F` | PlayerConnections | [Server health](#server-health); only while subscribed (stream 21) | the engine's per-connection statistics |

Notes on single events:

- **Order.** For one victim, `Kill` comes before `Death`: the engine raises script event 1 before it kills the player
  (`GameLogicDamage.cpp:484-507`).
- **Spawn.** The engine enters the soldier before it marks the player alive (`ServerGameLogic.cpp:2061-2062`), so a
  spawn straight into a vehicle is `Spawn` (the soldier) followed by `EnterVehicle`. Script event 23 also fires when
  a living player returns to its soldier; the module filters that out.
- **PlayerDisconnect reason:** 0 unknown, 1 left, 2 kicked, 3 timed out, 4 banned, 5 other. Kicked and banned come
  from the kick and ban notices the module saw for that player. A bot removed through the bot channel has reason 0.
- **TeamChange and ScoreChange** are detected by the module, so they report a change from any source: the player, an
  admin, autobalance or a script.
- **VehicleDestroyed.** The engine raises script event 24 for every dying player control object, soldiers included.
  The module reports vehicles only.
- **ControlPoint.** The game-mode script sets the new team in its own handler, after the module's observer. The
  module therefore reports the event one tick later, with the team after the script ran. The kind is the engine's
  argument.
- **Chat.** The channel is the engine's number (`GameServer::handleStringReceivedEvent`). The names, by number from
  0: `Global`, `Team`, `Commander`, `Squad`, `SquadLeader`, `SquadId`, `LocalTeamMedic`, `LocalGroundVehicle`,
  `LocalAirVehicle`, `Local`, `LocalTeam`, `Vehicle`, `ServerMessage`, `ServerTeamMessage`, `Player`, `LocalRevive`,
  `LocalRevive`, `TeamAndLocalEnemies`, `SquadAndLocalEnemies`, `Invalid`. The module removes the client's channel
  prefixes (`HUD_TEXT_CHAT_TEAM`, `HUD_TEXT_CHAT_SQUAD`, `HUD_TEXT_CHAT_COMMANDER`) and dead prefixes (`*DEAD*` and
  its variants) from the text; text flag bit 0 tells that a dead prefix was there. The text is at most 1024 bytes. A
  `Say` to one player comes back as channel 14 with the player id in the engine flags.
- **Kick.** A console `admin.kickPlayer` on a bot raises the engine's kick notice, so a `Kick` event, with no
  disconnect: a bot has no connection.
- **Weapons** lists the weapons in the active slots 0 to 2 of the player's seat (on foot the soldier's); W is 0 when
  the player is dead. The module polls per tick and sends the event when a player's set changes, for every player
  on `Subscribe`, and for every player after every `Snapshot`.
- **Bot** lets the client tell bot-channel bots apart from other players.

**`Kill`**

| Type | Field |
|---|---|
| `u8` | victim |
| `i16` | attacker, -1 if none |
| `u8` | kind: 1 kill, 2 team kill, 3 suicide (attacker = victim), 4 roadkill (no weapon, an attacker vehicle), 5 no attacker (crash, fall, out of bounds) |
| string | weapon template, empty if none |
| `u8` | victim team |
| `u32` | victim root networkable id |
| string | victim root template |
| `Vec3` | victim root position (the victim's own vehicle) |
| `u8` | attacker team, 0 if none |
| `u32` | attacker vehicle networkable id: the root of the script event's vehicle argument, else the attacker's root; 0 if none |
| string | that vehicle's template |
| `Vec3` | attacker root position |
| `u8` | assists A, then A × (`u8` player, `i32` assist score) |

**`Damage`**

| Type | Field |
|---|---|
| `i16` | victim player: the soldier's player, or for a vehicle its first occupant, or the occupant this damage killed; -1 if none |
| `u32` | victim root networkable id |
| `u32` | victim root console id |
| string | victim template (the object whose armour took the damage) |
| `i16` | attacker, -1 if none |
| string | weapon template |
| string | projectile (source) template |
| `f32` | amount |
| `f32` | hit points before |
| `f32` | hit points after |
| `f32` | maximum hit points |
| `Vec3` | position the engine passed |
| `u8` | flags: bit 0 the victim's hit points reached 0, bit 1 this damage killed a player |

- The position is the projectile's start, the explosion centre, or 0/0/0 for a bleed. It is not the hit point; the
  hit point is in `ProjectileCollision`.
- Damage is booked to the nearest armour up the parent chain, so a hit on a rotor is a `Damage` to the helicopter.
- A destroyed vehicle keeps burning: `Damage` events continue after the one that set bit 0, with the killer still as
  attacker.
- A rocket hit is usually two events in one tick, the projectile and its splash, with the same source template.

**`WeaponFire`** (one per shooter, weapon and projectile template per tick in which it launched)

| Type | Field |
|---|---|
| `i16` | shooter player, -1 if none |
| `u32` | networkable id of the launching vehicle's root, 0 if none |
| string | weapon template (the projectile's launcher), empty if none |
| string | projectile template |
| `u8` | rounds: projectiles this weapon launched in the tick, batches (shotguns) included, at most 255 |
| `Vec3` | origin: the first of them at launch |
| `Vec3` | velocity: the first one's at launch, the muzzle velocity plus the vehicle's (`FireComp.cpp:941-956`) |

- The source is the projectile observer in `FireComp::setupProjectile` (`FireComp.cpp:1120-1129`): every projectile
  the server launches, bullets and flares included. Tracer copies are made after it and are not counted
  (`:1131-1160`).
- The event's tick is the tick of the launch. The module sends it with its next tick's events.
- At most 512 shooter-and-weapon aggregates per tick; more are dropped and counted in the module's stats line.

## Hit/miss replay

Event stream 20 lets a viewer replay a projectile's whole flight, with each target drawn where it is and where the
server tested it, and name the part a hit touched. `Subscribe` takes projectile templates as filters, at least one.
It carries two events, and it builds on the lag field of `PlayerState`.

### How the server decides a hit

- A projectile is a point. Each tick its path, from `position - dt × velocity` to `position`, is swept against every
  candidate object's collision mesh (`Physics/PointResponsePhysics.cpp:86-173`).
- **Lag compensation.** An object with a latency history is tested at its pose `L` seconds ago, with `L` the
  shooter's lag (`Physics/PointResponsePhysics.cpp:128-139`, `Physics/LatencyCompensation.cpp:82-108`). A lag of
  1/30 s or less rewinds nothing.
- **Only parentless networked objects and soldiers have a history** (`Scene/Object/Object.cpp:619-625`,
  `Physics/SoldierPhysicsNode.cpp:404-447`): vehicle hulls and soldiers, seated crew included. Child parts of a
  vehicle (rotors, dummy missiles on the rails, sensors, gun turrets, wheels) have none and are tested where they
  are now.
- So the server's hitbox of a moving vehicle is split: the hull and the crew trail the vehicle by about
  `speed × L`, and the child parts stay where the vehicle is now.
- The latency history advances at the end of each tick (`Physics/PhysicsNodeManager.cpp:147-162`).

### `0x1219 ProjectileCollision`

One event per collision the server hands to a subscribed projectile. At most 64 per tick; the rest are dropped
before they get a sequence number (so they leave no gap) and are counted in the module's stats line.

| Type | Field |
|---|---|
| `u32` | projectile id, as `ProjectileState`'s |
| string | projectile template |
| `i16` | shooter player, -1 if none |
| `u8` | kind: 0 terrain or water, 1 an object |
| `u8` | flags: bit 0 the part is the root (a vehicle's hull), bit 1 the part is a soldier, bit 2 the part was tested at its rewound pose |
| `u32` | root networkable id, 0 if none |
| `u32` | root console id, `0xFFFFFFFF` if none |
| string | root template, empty for kind 0 |
| string | part template: the object touched (the root's own for a hull hit; for example `agm114_hellfiredummy`, `ahe_ah1z_mainrotor`, `us_light_soldier`), empty for kind 0 |
| `i16` | part player: the soldier's player id when bit 1, else -1 |
| `Vec3` | impact point |
| `Vec3` | contact normal |
| `Vec3` | relative speed |
| `u16` | collision material index |
| `f32` | latency, seconds: the lag compensation the engine used for this shooter |
| `Vec3` | part position as tested: rewound when bit 2, else where it is now |
| `Rot` | part rotation as tested |
| `Vec3` | root position now (0/0/0 for kind 0) |
| `Rot` | root rotation now |

- The source is `GenericProjectile::handleCollision`, on the main thread inside the collision response.
- With bit 2 the tested pose is `IPhysicsNode::getLatencyCompensatedTransformation(latency)` of the part, read in
  the callback. The history has not advanced yet, so this is the transform the collision used. Without bit 2 it is
  the part's absolute transform, so a rotor hit shows the blade that was hit.
- A projectile can collide more than once (ricochet, penetration). A guided missile's direct hit is its first
  kind 1 event. The missile then stays at the impact point until its time to live runs out, and `ProjectileHit`
  follows only then. A miss usually ends in a kind 0 event.
- `Damage` cannot tell parts apart. Match a `Damage` to a collision by tick and projectile template.
- The tick is numbered as `Damage`'s: the state the colliding tick started from. That is the `ProjectileTrace`
  sample whose tested poses that tick's sweep used.

### `0x121A ProjectileTrace`

The whole flight of one subscribed projectile, sent once when it ends. It is an event, not a stream frame: it is
never replaced by a newer one.

| Type | Field |
|---|---|
| `u32` | projectile id, as `ProjectileState`'s |
| string | projectile template |
| `i16` | shooter player, -1 if none |
| `u32` | first tick: the tick of sample 0 (samples are consecutive ticks) |
| `u8` | flags: bit 0 the trace was cut at the sample limit, bit 1 it ended at a collision (else the projectile vanished or expired) |
| `u8` | targets T (at most 8), then T × (`u32` root networkable id, string template) |
| `u16` | samples S (at most 300), then S × sample |

Each sample is 28 + T × 48 bytes:

| Type | Field |
|---|---|
| `Vec3` | projectile position |
| `Vec3` | projectile velocity |
| `f32` | the shooter's lag compensation this tick, seconds |
| T × target | `Vec3` position now, `Rot` rotation now, `Vec3` position as tested, `Rot` rotation as tested |

- **Timing.** Sample `k` carries the tick label `first + k`: the state at the end of that tick, taken in the
  module's tick hook before the next tick is simulated. "As tested" is the rewound transform of each target's
  physics node at that moment, which is exactly the pose the projectile's sweep in the next tick is tested against.
  So the sweep from sample `k` to sample `k + 1` meets the target at sample `k`'s tested pose, and a
  `ProjectileCollision` in that sweep carries tick `first + k`.
- **Without a rewind** (no history, or a lag of 1/30 s or less) the sweep meets the target where the tick leaves
  it, because targets move before collision. Sample `k`'s tested pose is then sample `k + 1`'s current pose; the
  last sample keeps its own.
- **Targets.** Every vehicle in the vehicle set, except the shooter's own root, that comes within 150 m of the
  projectile during the flight; at most 8, nearest first. A target that leaves the set keeps its last pose.
  Vehicles within 1000 m of the launch are sampled from the first sample on, so a target that comes close late
  still has its whole flight.
- **Soldiers are not traced.** A seated crew member is rewound with its own history but sits where the rewound hull
  carries it. A hit on one is in `ProjectileCollision`.
- **Child parts** are not traced either. A viewer draws them at the target's current pose from their fixed offsets
  in the vehicle's template. The rotor's blade angle is unknown, so a viewer draws the rotor as a disc and shows
  the blade only at a `ProjectileCollision` on it.
- **End.** Sampling stops 15 ticks after the projectile's first collision with an object or the terrain (so the
  target can be seen moving on), when the projectile vanishes, or at 300 samples (10 s).
- **Limits.** At most 64 flights are traced at once; more are counted in the stats line and not traced.
- **Size and cost.** A 3.5 s missile flight with one target is about 8 KB, sent once. The main thread reads one
  transform per candidate per tick while a subscribed projectile flies, and nothing otherwise.

## Commands and replies

Commands are `0x13xx`, client → module. Every command starts with a `u32` request id; the reply echoes it. Success
is `0x1401 Ok` (`u32` request id) unless a typed reply is listed. Failure is `Error` with the request id. What each
command does, and its errors, is in [Command semantics](#command-semantics).

| Type | Command | Fields after the request id | Reply |
|---|---|---|---|
| `0x1301` | Console | string line (one line, 1-4096 bytes) | `ConsoleResult` |
| `0x1302` | Say | `u8` target (0 all, 1 team, 2 one player), `u8` team or player id, string text (1-1024 bytes) | `Ok` |
| `0x1303` | Teleport | `ObjectRef`, `u8` flags (bit 0 set position, bit 1 set rotation; at least one), `Vec3` position, `Rot` rotation | `Ok` |
| `0x1304` | SetHealth | `ObjectRef`, `u8` flags (bit 0 the value is a fraction of the maximum), `f32` value | `Ok` |
| `0x1305` | KillPlayer | `u8` player, `u8` flags (bit 0 also set the player's vehicle's hit points to 0) | `Ok` |
| `0x1306` | SetTeam | `u8` player, `u8` team (1 or 2), `u8` flags (bit 0 kill the player after the switch) | `Ok` |
| `0x1307` | SetScore | `u8` player, `u8` mask (bit 0 score, 1 team score, 2 kills, 3 deaths, 4 team kills; at least one), 5 × `i32` values in that order (all present) | `Ok` |
| `0x1308` | Rank | `u8` player, `u8` rank (0-21), `u8` flags (bit 0 send the rank-up event) | `Ok` |
| `0x1309` | Medal | `u8` player, `i32` medal, `i32` value | `Ok` |
| `0x130A` | GameEvent | `u8` player, `u8` event (0-24), `i32` data | `Ok` |
| `0x130B` | HudEvent | `u8` player, `u8` event (0-78), `i32` data | `Ok` |
| `0x130C` | Kick | `u8` player, string reason (at most 256 bytes; shown to the player) | `Ok` |
| `0x130D` | Ban | `u8` player, `u8` by (0 address, 1 CD key), `u8` duration (0 permanent, 1 this round, 2 seconds), `u32` seconds (1 to 2147483647 with duration 2), string reason (at most 256 bytes; logged) | `Ok` |
| `0x130E` | Map | `u8` action (1 restart the round, 2 run the next map, 3 run a map-list index, 4 run a map by name and mode), `u16` index, string name, string mode, `u16` size (0 any) | `Ok` |
| `0x130F` | ListObjects | string template name (exact, without case), `u16` maximum entries (1-4096) | `ObjectList` |
| `0x1310` | SetObjectActive | `ObjectRef` (kind 2 or 3) | `Ok` |
| `0x1311` | SpawnObject | string template name, `Vec3` position, `Rot` rotation, `u8` team (0-3) | `ObjectSpawned` |
| `0x1312` | DeleteObject | `ObjectRef` (kind 2 or 3) | `Ok` |
| `0x1313` | StartVote | string question (1-96 bytes), `u16` seconds (5-600), `u8` team (0 everyone, 1 or 2), `u8` percent (1-100), `u8` minimum ballots (0-64) | `Ok` |
| `0x1314` | CancelVote | nothing | `Ok` |
| `0x1315` | HudEventRaw | `i16` player (-1 every playing client), `u8` HUD event, `u8` length N (0-52), N bytes payload | `Ok` |
| `0x1316` | RoundClock | `i16` player (-1 every connected client), `f32` seconds from now (0-86400) | `Ok` |
| `0x1317` | SquadOrder | `u8` team (1 or 2), `u8` squad (1-255), `u8` order id, `u8` flags (bit 0 a commander order), `u32` target networkable id (0 none, at most 65535), 3 × `Vec3` points | `Ok` |
| `0x1318` | StartScript | `u32` instance, `u8` script API version the script needs, `u8` flags (0), string name, string arguments, string source | `Ok` |
| `0x1319` | StopScript | `u32` instance | `Ok` |
| `0x131A` | ScriptMessage | `u32` instance, string message (JSON text, at most 8192 bytes) | `Ok` |
| `0x131B` | ListScripts | nothing | `ScriptList` |
| `0x131C` | SeatPreference | `u8` player, `u8` seat (0 first, 1 second, 255 none) | `Ok` |
| `0x131D` | SetCollision | `ObjectRef`, `u8` flags (bit 0 collision on; 0 turns it off; other bits must be 0) | `Ok` |
| `0x131E` | ResyncClient | `i16` player (-1 every connected human client) | `Ok` |

Template names, map names and game modes are at most 256 bytes. Unknown flag bits are `Error` 7 in every command.

**`0x1402 ConsoleResult`**: `u32` request id, `u8` flags (bit 0 the output was truncated at 256 KiB), `u16` line count
L, L × string. The output of the console line, split at `\n`, without the final empty line.

**`0x1403 ObjectList`**: `u32` request id, `u8` flags (bit 0 more objects matched than were returned), `u16` N,
N × entry:

| Type | Field |
|---|---|
| `u32` | console id |
| `u32` | networkable id, 0 if none |
| string | template name |
| `Vec3` | position |
| `Rot` | rotation (the absolute matrix) |
| `f32` | health 0..1, -1 without armour |
| `u8` | flags: bit 0 the object has a parent |

**`0x1404 ObjectSpawned`**: `u32` request id, `u32` console id, `u32` networkable id (0: the object has no
networkable and exists on the server only).

**`0x1405 ScriptList`**: `u32` request id, `u8` N, N × (`u32` instance, string name): the scripts that run now, in
start order.

## Command semantics

Common rules:

- A command that names a player answers `Error` 4 when no such player exists.
- An `ObjectRef` of kind 2 or 3 that resolves to no object is `Error` 5. Kind 1 is `Error` 4 for no such player and
  `Error` 6 for a dead player, who has no root object.
- Positions must be finite and inside the world: x and z within ±8192, y from -1000 to 10000.

- **Console**: runs the line on the engine's main console and returns its output, as Python's `host.rcon_invoke`
  does (`Game/Python/PythonHost.cpp:1766-1783`). `Error` 9 with `adminAllowConsole 0`.
- **Say**: `IGameServer::sayAll` (channel 12), `sayTeam(team)` (channel 13), or the engine's Player channel (14) for
  one player, which only that player's connection receives (`GameServer::adminToPlayerEvent`).
- **Teleport**: acts on the target's root object. It sets the absolute transform, keeping the current position or
  rotation if only one is given, then resets the physics node (speeds zero) and wakes it. On foot a soldier's yaw
  does not stick: its facing comes from its own look direction.
- **SetHealth**: sets the hit points of the target's armour (`IArmorGLComp::setHitPoints`, Python's `setDamage`,
  `Game/Python/py/Objects.cpp:706-722`). The value is hit points (0 to 1e6), or a fraction 0..1 with flag bit 0. It
  bypasses the damage path, so no damage, kill or destruction is reported from it. An object without armour is
  `Error` 5. A result of 0 hit points on a soldier or an occupied vehicle is `Error` 6: the engine would evict and
  destroy the occupant's soldier, and it aborts when a soldier that still has its kit is destroyed
  (`Scene/Object/ObjectManager.cpp:1853-1854`). Use `KillPlayer`.
- **KillPlayer**: the engine's own kill, the call a crash makes, so `Death` is raised and no `Kill`. A dead player
  is `Error` 6. With bit 0 and the player seated, the module kills the player and then sets the emptied vehicle's
  hit points to 0, which makes it a wreck (`VehicleState` health 0, destroyed flag, no `VehicleDestroyed`). If
  another occupant is in the vehicle, the command is `Error` 6 and nothing is done. Nothing is deleted.
- **SetTeam**: `IPlayer::setTeam(team, false)`, then `KillPlayer` if bit 0. `TeamChange` follows from the module's
  own detection.
- **SetScore**: writes the score fields directly, as Python's score setter does. It raises no script event;
  `ScoreChange` follows from the module's own detection.
- **Rank, Medal, GameEvent, HudEvent**: what Python's `sendRankEvent` (plus setting the rank), `sendMedalEvent`,
  `sendGameLogicEvent` and `sendHudEvent` do (`ServerGameLogic.cpp:2540-2662`). A value of 0 is sent like any other.
  `Error` 6 when the engine refuses.
- **Kick**: `IGameServer::kickPlayer` with the text as the reason.
- **Ban**: the module runs `admin.banPlayer` or `admin.banPlayerKey` with `perm`, `round` or the seconds. It is not
  a raw console line from the client, so it works with `adminAllowConsole 0`. A ban by CD key of a player without a
  hash is `Error` 6: it would ban the empty key and lock out every player without one. The console resolves its
  argument by name before id, so the ban is `Error` 6 while another player's name equals this player's id.
- **Kick and Ban refuse bots** (`Error` 6). A bot has no connection, and a ban would land on the address `N/A`.
  Bot-channel bots are removed through the bot channel. A player that is not connected is `Error` 6.
- **Map**: action 1 is `admin.restartMap`; 2 is `admin.runNextLevel`; 3 and 4 set the next level to the map-list
  entry and run it. Action 4 looks the entry up by name, mode and size and needs a name and a mode. A map that is
  not in the rotation is `Error` 5.
- **ListObjects**: every grid object whose template is the named one, the walk of the console's
  `object.listObjectsOfTemplate` (`Scene/Object/ToolObject/ObjectTemplateAdm.cpp:263-277`). It returns the console
  id beside the networkable id, because static map objects may have no networkable id.
- **SetObjectActive**: runs `object.active idN` with the object's console id, so the console's object commands that
  follow act on it. It only helps raw `Console` lines, so it is of no use with `adminAllowConsole 0`.
- **SpawnObject**: creates the template at the position with `IGame::createObjectOnAllClients`, the engine's own
  object-spawner path (`ServerGameLogic.cpp:947-951`). An unknown template is `Error` 5. `Error` 9 with
  `adminAllowObjects 0`. An object whose template has no networkable is created on the server only and no client
  sees it; `ObjectSpawned` then has networkable id 0. Vehicle templates have one.
- **DeleteObject**: `ObjectManager::destroyObject`, what the console's `object.delete` runs. The module refuses
  (`Error` 6) a soldier, a camera, or a vehicle with a player in it. `Error` 9 with `adminAllowObjects 0`. A client
  removes the object only if its own copy has a networkable; a copy without one stays until that client is
  resynced or the round restarts ([Client resync](#client-resync)).
- **StartVote, CancelVote**: [Server votes](#server-votes). `StartVote` is `Error` 6 when a vote is running or no
  human player can vote, and `Error` 5 when the module cannot listen for ballots. `CancelVote` is `Error` 6 when no
  vote is running.
- **HudEventRaw, RoundClock, SquadOrder**: [Client presentation commands](#client-presentation-commands).
- **ResyncClient**: [Client resync](#client-resync). Sent only when capability bit 7 is set.
- **StartScript, StopScript, ScriptMessage, ListScripts**: [Scripts](#scripts).
- **SeatPreference**: stores the player's preference in the module, by player id. Nothing else happens until the
  player and a teammate are dead with the same spawn group, when the module may move the player's spawn tick
  ([Seat preferences](#seat-preferences)). A seat value other than 0, 1 or 255 is `Error` 7 whether or not the
  player exists. Setting the value a player already has is `Ok`. Sent only when capability bit 4 is set.
- **SetCollision**: turns collision off or on for the target's root object (as for `Teleport`) and every
  descendant. Sent only when capability bit 5 is set.
  - The module walks the tree as the engine's `world::updateFlagsRecursive` does
    (`Scene/Object/ObjectUtil.cpp:172-177`) and changes `OFHasCollision` with `IObject::updateFlags`
    (`Game/Objects/IObject.h:76`, `:194`). The engine adds a newly set object to the response physics and removes a
    newly cleared one (`Scene/Object/Object.cpp:858-867`).
  - Off clears the bit on every object of the tree that has it, and the module remembers those objects. On sets it
    again only on remembered objects that still lack it, so parts that never had collision are never given any.
  - Sending the state an object already has changes nothing and is `Ok`.
  - The module forgets an object when on is sent for its tree, when its root is destroyed, and at a level unload. A
    vehicle respawned from the object cache gets its creation flags back (`Object.cpp:689-695`, `:746`).
  - Errors beyond the common ones: 5 the object is in the object cache; 6 when 16384 objects are already without
    collision.
  - Nothing is sent to clients: the flag is the server's.
  - A vehicle without collision falls through whatever it stands on. Turning collision on while a vehicle is inside
    other geometry wrecks it: the engine destroys it within a few ticks.

## Client presentation commands

Three commands drive HUD features the retail client already has, with no client mod: map markers, notices, a round
clock and squad orders. Each calls one method the module's engine interface adds
([Engine interface](#engine-interface)). They have no capability bit.

### HudEventRaw

Sends the HUD event with its payload to one player, or to every playing client for -1. Python's `sendHudEvent`
carries only a 4-byte payload; this carries up to 52 bytes. The payload is zero-padded and always sent as 52 bytes,
so a handler that reads past what the caller gave sees zeros.

Only events whose client handlers read the payload as plain data are allowed. Any other id is `Error` 7:

| Id | Effect | Payload |
|---|---|---|
| 43 | Spotted marker at x/z, one team | `u16` spotter, `u16` target, `u8` team at +4, `f32` x at +8, `f32` z at +12, `i32` vehicle type at +16 |
| 42 | Artillery circle and heads-up | `Vec3`, `u8` active at +12 |
| 40, 41 | UAV marker, satellite sweep | position |
| 74, 83 | Vehicle-drop and supply-drop marker and "incoming" voice-over | position |
| 64, 65 | Control point captured, lost | `i32` control point id, `i32` player id |
| 44 | Missile-lock warning | 16 bytes |
| 51 | The client sends this radio message itself | 32-byte name, channel at +0x20 |
| 66, 70, 71 | Help hint, ticket voice-over | `i32` |

- The payloads are the ones the engine's own senders use (`CommanderToolsManager.cpp`, `SupplyDropManager.cpp`,
  `GameServer.cpp`).
- Event 42 may touch commander-only state. Events 64 and 65 do nothing unless both teams can capture the point.
- Events 7, 12, 17, 38 and 73 are never allowed: their handlers treat the payload as a pointer.
- `Error` 6 when the engine refuses.

### RoundClock

Sends a begin-round event with the server's current begin-round state and a state-change time of now plus the given
seconds, to one player or to all (-1). Only the client's clock and banner move. The server's own begin-round event
at the next phase change overwrites it. `Error` 6 when the engine refuses.

### SquadOrder

Issues a squad order, or a commander order with flag bit 0, through the engine's squad manager, so server squad
state stays consistent and members get the retail HUD notices. Order ids: 0 artillery, 1 UAV, 2 vehicle drop,
3 supply, 5 attack, 6 defend, 7 repair, 8 destroy, 9 mines, 10 attack control point, anything else move. A squad
exists while it has members; a squad without members is `Error` 6.

## Client resync

**Draft: not yet approved by the maintainer.**

`ResyncClient` makes a client drop every dynamic object it holds and build them again from the server, without a
round restart. Nobody dies on the server, no vehicle respawns and no score changes.

### Why it exists

A client can hold an object the server cannot remove:

- The server creates an object on clients with a create event that carries a template id and a networkable id
  (`GameServer::createObjectOnAllClients`). The client creates the object from its own template. If that template
  has no networkable on the client, the object exists there without a networkable id
  (`Game/GameClient/GameClient.cpp:1819-1821`).
- This is how a static object is shown to unmodified clients: its template gets a networkable on the server only
  (console lines, `networkableInfo.createNewInfo` and `ObjectTemplate.setNetworkableInfo`).
- The server's destroy event names the networkable id. The client finds no object for it and does nothing
  (`GameClient.cpp:1909-1917`). No other client event addresses an object without a networkable id.
- The client destroys such an object in one place: its round reset, which destroys every root object that was not
  created at map load (`GameLogic::resetAllObjects`, `Game/GameLogic/GameLogic.cpp:2000-2075`).

### What the module does

In the tick of the command, for each client it resyncs, through `IServerExtension::resyncClient`
([Engine interface](#engine-interface)):

1. It sends the event a round restart sends first (`PostRemoteEvent` ECGameLogic 6,
   `Game/GameLogic/ServerGameLogic.cpp:342`). The client runs its round reset
   (`Game/GameLogic/ClientGameLogic.cpp:623-627`, `GameLogic.cpp:2080`): it kills every player locally, destroys
   every dynamic object, clears its squads and sets its clock to 0. The server does not run it.
2. It marks every dynamic object as not yet created for this connection and resets the connection's ghosts, as a
   round restart does (`GameServer::resetConnections`). From here the server sends this client events only, no
   object or control state (`GameServer::processGameStateAndSendPackets` requires the connection's reset latch).
3. It sends the object part of the join database (`ServerConnection::clientSendObjectDatabase`) without the static
   ghosts: a create event for every dynamic networked object with a base priority of 0 or more (vehicles, soldiers,
   kits, projectiles), then for each living player the events that seat it in its soldier, kit and vehicle, then
   the sticky projectiles.
4. It sends "this player spawned" for every living player (ECNetwork 9, as `ServerGameLogic.cpp:2127-2131`) and the
   squad state.
5. It sends the game status. When the client has acknowledged that event, the server sets the reset latch and
   sends object and control state again. Event blocks are confirmed in order
   (`Game/Common/GameEventManager.cpp:430-455`), so the client has every event up to here by then.

In the ticks that follow, through `IServerExtension::resyncClientStatics`:

6. It sends the create events of the static ghosts (objects with a negative base priority: the spawned static
   objects), 4 per tick and client, in networkable-id order, up to the highest id in use when the resync began. An
   object created later was announced to the client when it was created.

The static ghosts come last and paced because a connection's stream managers share each packet without a budget
(`ClientConnection::transmitMsgs`, `Game/Common/ClientConnection.cpp`): about 18 create events fill a 640-byte
packet, and while they do, the client gets no state for its own vehicle. At 4 a tick they take about 190 bytes of
each packet (the server sends a client 20 packets a second).

The client takes its tick from the server again with the first control state it receives
(`GameClient.cpp:1564`).

An object the server has deleted is not in the replay, so the client's copy is gone after the resync. To move or
remove objects that clients hold without a networkable: delete them on the server (`DeleteObject`, or the console),
create the new ones, then send `ResyncClient` once.

### Rules

- A player that does not exist is `Error` 4.
- `Error` 6 for one player when the engine refuses: the player has no connection (a bot), the connection is not
  playing yet, or it has not acknowledged an earlier reset (a `ResyncClient` or a round restart). With -1 the module
  resyncs every connected human client it can and answers `Ok`; the log has the counts.
- `Ok` means the resync has begun. Nothing is reported when the client has acknowledged it or when its static
  objects are sent; the module's log has a line for the latter.
- A `ResyncClient` for a client whose static objects are still being sent starts over: the client has dropped those
  it had got.
- A level unload ends the sending of static objects.
- A round restart while a resync waits for its acknowledgement: the acknowledgement of the resync sets the reset
  latch before the client has confirmed the restart.

### What a player sees

Measured on 2026-11-17 with one retail client (a pilot or gunner in an AH-1Z, and on foot) on a test server with
`dalian_2_v_2`, about 60 resyncs, 3 to 600 spawned static objects. The round was in "waiting for players" throughout
(one human), so a running round is not covered.

- **No crash, no disconnect**, also with 30 resyncs half a second apart.
- **The server's state does not change**: the player stays alive, in its seat and vehicle.
- **The player's own vehicle.** The view stays in the cockpit. For one frame the HUD speed and the rotor read 0.
  The client has no server state for its vehicle for about one round trip plus 60 ms: under 0.1 s at a ping of
  16 ms, about 0.2 s at 100 ms, about 0.3 s at 190 ms with 3% packet loss. A hovering heli moved less than 0.5 m
  (0.9 m at 190 ms); a heli cruising at 30 m/s stayed on its course.
- **HUD.** The repair and rearm icons show for under a second. A kill line for the player's own name appears in
  the kill list. The spawn banner can flash.
- **Static objects** are gone at once and come back at 120 a second in the order they were created: 150 took 1.3 s,
  600 took 5 s. Until an object is back the client does not collide with it, and the server does.
- **Other vehicles** keep their course. In one of two cases a heli 200 m away stood still for 0.9 s; otherwise the
  stall was 0.1 s.
- **A TV missile in flight is lost to its gunner.** The gunner's view returns to the heli and the missile flies on
  unguided. The next shot is normal.
- **On foot** the view jumps up about 2.6 m and settles within 0.3 s, and the weapon is drawn again.

Not tested: more than one human client, a running round (tickets, scores, commander, a squad order on screen), a
player who is dead or in the spawn menu, passengers, parachutes, and what a player hears.

## Server votes

A yes/no vote on any question, run by the module with no engine change. One vote at a time.

- **How players answer.** With PgUp and PgDn. With no HUD question pending, the retail client turns them into the
  team radio messages `roger_that` and `negative` (`HudQuestionManager.cpp`, `replyWithoutQuestion`), which reach
  the module as radio events.
- **Commands and event.** `0x1313 StartVote`, `0x1314 CancelVote` and the event `0x121B VoteResult`.
- **Voters.** Every human player (not AI-flagged) on a voting team: team 1 or 2, or only the given team for a team
  vote. Re-checked every tick. A voter who leaves, or leaves the team of a team vote, loses the ballot. A voter who
  joins mid-vote gets the vote line at once.
- **Ballots.** A player's latest answer while the vote runs counts. Changing it is allowed.
- **Result.** The vote ends when its time is up or every voter has voted. It passes when at least `minimum ballots`
  (at least 1) were cast and yes ≥ `percent` % of them. A level unload cancels the vote (outcome 4).
- **Lines.**
  - The question, to every voter: `VOTE: <text> (PgUp yes / PgDn no, <s> s left)`.
  - A voter's own line, on the tick after the ballot: `<text> - you voted YES` (or `NO`).
  - The result, to everyone, or to the team of a team vote: `VOTE PASSED: <text> (yes Y, no N)`,
    `VOTE FAILED: …` or `VOTE CANCELLED: <text>`. A level unload cancels without a line.

### Keeping the question on screen

The lines go to the client's game-message rows, the box the kill feed and server messages share, which player chat
cannot push out. That box has 5 rows and clears 20 s after its last new line
(`Menu/BF2HudManagerInit.cpp:123,132`); a sixth line pushes the oldest out. The module therefore keeps, per voter,
the number of rows added since its vote line and the time of the last one. It posts the line again, with the
current time left, when the count reaches 5 or when 20.5 s plus the voter's ping have passed since the last row.

Rows it counts:

- every chat line of type 0 or 1, which includes every server message, whether it comes from Python, the console
  or the admin channel: channel 14 counts for its player, channel 13 for its team, the rest for everyone. The
  module's own vote lines are not counted; its "you voted" lines are;
- the server's own game-info radio lines;
- a kill-feed line on each kill with a killer, "player disconnected", and a control point captured or lost.

Not counted: a few client-local lines (squad invites and requests, "Autoaccept disabled"), a new squad leader and a
team-kill kick or ban notice. Those can push the question out early; it returns at the next counted push-out or
quiet clear. Long lines wrap into extra rows at a width the server does not know, so the question is limited to 96
bytes and every line counts as one row.

### Costs and constraints

- Each answer is also relayed as team radio ("Roger that", "Negative").
- A pending squad invite, order or team-kill question takes the key press first.
- A player who just spammed radio may have the press dropped by the client's radio throttle.
  `sv.radioSpamInterval` can be relaxed during the vote.
- "PgUp/PgDn" is literal text; a player with other key bindings presses those.
- The radio message ids are constants in `src/Vote.h`: 112 and 113, the two messages' positions in the `bf2` mod's
  `Common/Sound/VoiceMessages*.con` (the engine numbers messages in load order,
  `Game/GameLogic/VoiceMessages.cpp:102`). A mod with another message list needs other ids.

## Scripts

Game logic that must run inside a tick, changed without restarting the game server. The client pushes JavaScript
source over the admin channel and the module runs it on the main thread in an embedded JavaScript engine. A script
sees the world through a small host API and never touches the engine or the network itself. BF2WebAdmin writes its
scripts in TypeScript and sends the compiled JavaScript.

### Engine and language

QuickJS-NG (MIT), vendored at a pinned release under `third_party/quickjs-ng` and linked statically into
`libserverext.so`. Each script gets its own runtime and context. Code runs in strict mode as one global script.

| Available | Not available |
|---|---|
| the base objects, `JSON`, `Map` and `Set`, typed arrays, promises, `queueMicrotask`, `BigInt` | `eval` and the `Function` constructor (the names exist and throw a `TypeError`, as do the constructors of async and generator functions), `Date`, regular expressions, `Proxy`, `WeakRef`, modules, `console`, timers, `performance`, QuickJS's `std` and `os` |

- A regular expression literal is a `SyntaxError` when the script is started. The string methods that need one
  (`match`, `matchAll`, `search`) throw a `TypeError`; `split`, `replace`, `replaceAll` and `includes` with strings
  work.
- Precompiled bytecode is never accepted from the client: QuickJS does not verify it.

### Availability

`Welcome` capability bit 3 is set when `adminAllowScripts` is 1. The client sends the script commands only then.
`StartScript` names the script API version the script needs. This document describes version 1; a later version
only adds to the API.

### Commands

- **`StartScript`**
  - instance: the client's choice, not 0 and not a running script's;
  - API version: 1 or more;
  - flags: 0;
  - name: 1-64 printable ASCII bytes;
  - arguments: JSON text, or empty for none;
  - source: JavaScript, 1-60000 bytes of UTF-8.

  `Ok` means the source's top-level code ran without an error. Errors: 9 with `adminAllowScripts 0`; 5 when the
  module's script API is older than the version asked for; 6 when 8 scripts run or the instance is in use; 7 for a
  bad instance, version, name or flags, arguments that are not JSON, source that does not compile, or top-level
  code that throws or runs over its time budget. The message is the script error with its line. A failed
  `StartScript` leaves nothing running and sends nothing the script emitted.
- **`StopScript`**: `Ok`, then `ScriptFinished` with reason 2. `Error` 5 for no such script.
- **`ScriptMessage`**: calls the script's message handlers with the parsed JSON, at once, in order with the other
  commands. `Ok` once they have run. `Error` 5 for no such script, `Error` 7 when the message is not JSON.
- **`ListScripts`**: `ScriptList`.

### Life cycle

1. `StartScript` creates the runtime, sets `script.args` and runs the top-level code, which registers its handlers.
2. Every module tick, after the client's commands of that tick have run, each script's tick handlers are called,
   scripts in start order. They run before the module reads the state for that tick's events and stream frames, so
   a stream frame shows what the scripts of the same tick did.
3. A script ends for one of these reasons, reported in `ScriptFinished`:

   | Reason | Cause | Stop handlers run | `ScriptFinished` result |
   |---|---|---|---|
   | 1 | the script called `script.finish(result)`; it ends when the handler that called it returns | yes, with `finished` | the JSON text of `finish`'s argument, empty without one |
   | 2 | `StopScript` | yes, with `stopped` | empty |
   | 3 | a handler threw, a promise was rejected and not handled, or the script ran out of memory | no | the error message with its stack |
   | 4 | the script went over its time budget | no | the error message with its stack |
   | 5 | the level unloaded | yes, with `level unload` | empty |

   When the link to the client closes or the module shuts down, the stop handlers run with `link closed` and no
   event is sent.
4. Scripts do not outlive their link. After every `Welcome` the client starts the scripts it wants again. After an
   event gap (a lost `ScriptFinished`) the client sends `ListScripts` with its `RequestSnapshot` and ends its record
   of every script that is not listed.

Details:

- **Order.** What a script emits in its top-level code or in a message handler is sent after the reply (`Ok`) to
  that `StartScript` or `ScriptMessage`. A script's `ScriptFinished` comes after its last `ScriptEvent`, the stop
  handlers' included.
- **Handlers.** A handler set during a tick runs from that tick on. At most 64 handlers of a kind; more are
  ignored. After `finish`, the handlers that have not run yet in that call are skipped, and an error thrown after
  it does not change the reason.
- **Stop handlers** let a script undo what it did. In one, `finish` and new handlers are ignored; `emit`, `log` and
  `game` work. It has 5 ms; one that runs over is cut off and logged. An error in a stop handler is logged and
  ignored. At a level unload and a closing link the stop handlers still see the players, and a chat line they send
  reaches the clients.
- **Out of memory** cannot be caught by the script: it always ends it.
- **Text.** Every string the module sends for a script is valid UTF-8: a lone surrogate becomes `?`. The error
  text of reasons 3 and 4 is cut at 8192 bytes.

### Limits

| Limit | Value |
|---|---|
| Scripts at once | 8 |
| Memory per script | 4 MiB (about 175 KiB is used before the script allocates anything) |
| Stack per script | 256 KiB |
| CPU time, top-level code | 50 ms |
| CPU time, any other call | 5 ms |
| CPU time, summed over any 30 consecutive ticks | 30 ms (1 ms per tick on average; the top-level code is not counted) |
| `ScriptEvent`s per script per tick | 16: `emit` beyond that throws a `RangeError`, `log` lines beyond that are dropped and counted |
| `emit` payload, `ScriptMessage` message | 8192 bytes |
| `finish` result | 60000 bytes of JSON; more throws a `RangeError` |

- A *call* is one entry into the script: its top-level code, all tick handlers of one tick, all message handlers of
  one `ScriptMessage`, or all stop handlers, each with the promise jobs they leave. `ScriptFinished` counts these.
- CPU time is the main thread's (`CLOCK_THREAD_CPUTIME_ID`), so a descheduled server thread is not counted. Wall
  time is not counted, and nothing in the API waits.
- The engine's interrupt handler ends a call that runs over. An endless `for (;;) {}` ends after about 5 ms. A call
  that came back by itself after more than its budget ends the script too (reason 4).
- **The budget is a guard against mistakes, not against a hostile script.** QuickJS checks for an interruption at
  loop back-edges and calls of script code, once per 100 of them in this build, and in some loops of its built-ins.
  One built-in call is never interrupted; the memory limit bounds what it can cost. Sorting, joining and writing as
  JSON 20000 numbers in one tick takes about 5.6 ms. So a script can cost one tick some milliseconds too many, and
  is then ended. Whoever can start a script holds the admin key.
- `ScriptFinished` carries the script's call count and CPU time. The module's stats line shows the scripts' cost.

### Host API, version 1

Two frozen global objects, `script` and `game`.

- Player ids are the wire's (0-255). A player id must be an integer number, else a `TypeError`; one outside 0-255
  is no player.
- A player's *root object* is its vehicle when seated, else its soldier, as `ObjectRef` kind 1. A dead player has
  none.
- A `Vec3` is `[x, y, z]` in metres (m/s for velocities) and a `Rot` is `[yaw, pitch, roll]` in degrees: the
  wire's conventions. Either is anything with numbers at `[0]`, `[1]` and `[2]`: an array or a typed array.
- A wrong argument type throws a `TypeError`. A non-finite number in a position, rotation or speed throws a
  `RangeError`.

| Member | Meaning |
|---|---|
| `script.api` | 1 |
| `script.name`, `script.args` | the name and the parsed arguments of `StartScript` (`null` without arguments) |
| `script.onTick(fn)` | `fn(tick)` every module tick; `tick` is `game.tick` |
| `script.onMessage(fn)` | `fn(message)` for every `ScriptMessage`, with the parsed JSON |
| `script.onStop(fn)` | `fn(reason)` before the script ends, with the reason as text |
| `script.finish(result?)` | ends the script; `result` must be JSON-serialisable |
| `script.emit(data)` | sends `ScriptEvent` kind 1 with `JSON.stringify(data)` |
| `script.log(...parts)` | writes the parts, joined with spaces, to the module's log and sends `ScriptEvent` kind 2 (cut at 8192 bytes) |
| `game.tick`, `game.tickRate`, `game.time` | the module's tick number (the one that tick's events and stream frames carry), 30, and the engine's time in seconds |
| `game.map` | the current map's name |
| `game.players()` | the ids of all players, as an array |
| `game.player(id)` | `null` for no such player, else `{ id, name, team, alive, bot, seated, seat, rootId, vehicle, position, rotation, velocity }` |
| `game.teleport(id, position, rotation)` | the `Teleport` command on the player's root object. Either argument may be `null` to keep it; both `null` throws a `TypeError`. A position resets the speeds. A position outside the world throws a `RangeError`. Returns `false` when the player has no root object |
| `game.setVelocity(id, velocity)` | sets the root object's speed in world axes and wakes the body. More than 1000 m/s on an axis throws a `RangeError`. Returns `false` without a root object or a physics node |
| `game.setAngularVelocity(id, velocity)` | sets the root object's rotational speed in rad/s about the world axes. More than 100 rad/s on an axis throws a `RangeError`. Returns as `setVelocity` |
| `game.say(text, player?)` | the `Say` command: to everyone, or to one player. Control characters are removed and the text is cut at 1024 bytes; an empty text says nothing |

In `game.player`'s result, `bot` is the AI flag, `seat` the seat id or -1, `rootId` the root object's networkable id
(0 without one; the players of one vehicle share it), `vehicle` the root object's template (empty when dead), and
`position`, `rotation` and `velocity` the root object's (`null` when dead). The values equal the `PlayerStream`
frame of the same tick.

Behaviour of the setters:

- Promises work: their jobs run after each handler, inside its time budget. The API has no timers; a script counts
  ticks.
- `game.setVelocity` sets the speed the next stream frame shows. The game's own forces act on it from the next tick
  (gravity, a rotor, ground friction), so a speed that is to be held is set every tick.
- `game.setAngularVelocity` follows the bot channel's convention for angular velocity
  ([protocol.md](protocol.md#conventions)): on a level helicopter facing north, a positive value about world y
  turns the nose east, about world x pitches the nose down, about world z banks left.
- Neither `game.setAngularVelocity` nor a `game.teleport` rotation turns a soldier: a soldier's facing is its own
  look direction.
- The setters use `IPhysicsNode::setPositionalSpeed` and `setRotationalSpeed` (`Physics/IPhysicsNode.h:50-51`).

## Seat preferences

Some spawn points put a player straight into a vehicle (`setEnterOnSpawn 1`). Two teammates who die together and
both have such a point selected are spawned in the same spawn pass, and the engine seats them in player-list order:
the first gets seat 0 (a helicopter's pilot), the second the next free seat (its gunner). With `SeatPreference` a
player chooses. One player's preference is enough; the teammate needs none.

There is no engine change. The module only moves a dead player's spawn tick, through `IPlayer`'s typed virtual
calls.

### What the engine does

- **Tick count.** The game logic's tick count advances once per update, at its top
  (`Game/GameLogic/ServerGameLogic.cpp:418`), and the same update then runs the spawn pass (`:542`).
- **Spawn pass.** `uPlayingSpawning` (`ServerGameLogic.cpp:633-689`) runs its player walk on every third call only
  (`:661-665`, `:688`). It walks the players in list order and spawns every dead player whose spawn tick is not
  after the tick count (`:674`) and that has a spawn group (`:685-686`). A man-down player is killed first in the
  same walk (`:677-684`).
- **Spawn tick.** A kill sets the spawn tick to the tick count plus the spawn time × 30 (`:3479`, `:3511-3513`).
  Nothing else moves it while the player is dead, except a round start (`:750`), a respawn request (`:1901`) and a
  revive (`:3965`).
- **Seat.** For an enter-on-spawn point the engine looks for a seat (`:2109-2113`, `:1736-1737`). The sweep walks
  the vehicle's seats in ascending order and takes the first unoccupied one
  (`Game/Objects/Game/PlayerControlObjectHelpers.cpp:441-457`).
- **Interfaces.** `IPlayer::getIsAlive`, `getSpawnAtTick`, `setSpawnAtTick`, `getTimeToSpawn` and `getSpawnGroup`
  are virtual (`Game/Objects/IPlayer.h:65`, `:98-100`, `:125`). `setSpawnAtTick` only stores the value
  (`Game/GameLogic/Player.cpp:1537-1540`). The value is in the player's state sync to the clients
  (`Player.cpp:3244`), so a client's spawn countdown follows a moved tick.
- **Where the module runs.** The module's tick handler runs after the game logic's update in
  `GameServer::simulateFrame` (`Game/GameServer/GameServer.cpp:2995-3001`). So it runs after this tick's spawn
  pass, with the tick count that pass saw; the next pass sees that count plus 1 or more.

### The rule

The rule is `seat::plan` in `src/SeatOrder.h`. It runs every module tick while any preference is set, with or
without a link.

Rank a first-seat preference 0, none 1, a second-seat preference 2. Let *now* be the tick count. For every two
players X and Y that are both dead (a man-down player is alive), on the same team, with the same spawn group above
0, and rank(X) > rank(Y):

```
base = max(Y.spawnAtTick, now + 1)
if X.spawnAtTick <= base + 2: X.spawnAtTick = base + 3
```

- **Why it is enough.** Y is spawned by the first pass at or after base, which is at most base + 2 because passes
  are three ticks apart. X is spawned by a pass at or after base + 3. So X is in a later pass than Y whatever the
  pass phase; Y takes the first free seat and X the next.
- **Why now + 1.** The module runs after the pass of tick *now*, so a Y that is due but still dead is spawned by a
  pass after *now*.
- **No cycles.** Ranks are a strict order. The module settles rank 1 players, then rank 2 players against all lower
  ranks, and one call is a fixed point. While Y is due and waiting for the pass (at most three ticks), X is moved
  on every tick to stay three ahead; that costs X nothing, since it could not join Y's pass anyway.
- **Equal ranks** (both first, both second, both none) are left to the engine's order.
- **A player who died first still waits.** The rule does not ask when X died: a player who wants the second seat
  waits for a dead teammate with a lower rank in the same group, up to that teammate's full spawn time. One-sided
  preferences need this: the teammate without a preference must not take the first seat by being due first. A
  teammate who picks another spawn group, or whose group is cleared, releases X at once.
- **The tick count.** No engine interface returns it. The module derives it from a dead player still waiting:
  `getTimeToSpawn` is (spawn tick − tick count) / 30 s, clamped at 0 (`Player.cpp:1567-1577`). When every such
  player is already due, the module sets the first one's tick 65536 ticks past the largest, reads `getTimeToSpawn`,
  and puts the tick back at once, on the main thread with nothing running in between.
- **Cost.** Nothing while no preference is set. Otherwise one walk of the player list and a pairwise check of the
  dead players in a spawn group per tick.
- **Log.** Each new wait is logged once, with both players, the old and new spawn tick and the engine tick.

### Lifetime and scope

- A preference is kept by player id in the module: across admin links, level loads and team changes. The client
  need not resend it after a reconnect, though doing so is harmless.
- It is dropped when a player with that id connects or disconnects, because ids are reused.
- `Welcome` capability bit 4 is always set by a module that has the command.
- Only the first two seats can be asked for. A third teammate without a preference is ordered like the others.
- Man-down players are not ordered: they are alive when the module looks, and the pass that ends their man-down
  state can spawn them at once (`ServerGameLogic.cpp:677-686`).
- The rule does not look at the spawn point itself. In a group without an enter-on-spawn point it only delays the
  later player by a pass or two.

## Server health

Two events describe how well the server runs. `ServerHealth` is about the server process: whether its ticks come on
time, how much of a tick the main thread works, and whether the host takes the CPU away. `PlayerConnections` is about
the network between the server and each player: packets per second in both directions and packets lost.

- `Welcome` capability bit 6 is set by a module that has both events. The client subscribes to stream 21 only then.
- While subscribed, the module sends one `ServerHealth` per window of 300 ticks (10 s at 30 Hz), followed by one
  `PlayerConnections` for the same window with the next sequence number and the same tick. The windows run from the
  server's first tick whether or not a client is subscribed, so the first `ServerHealth` after a `Subscribe` covers a
  full window. The events' tick is the tick that completed the window.
- The module writes the same figures to its log once a minute, as a `health:` line over a window of 1800 ticks, with
  or without an admin link.

### How the engine times its ticks

The figures only make sense against how the engine runs.

- The simulation tick is 1/30 s (`TickCalculator(10, 30.0f)`, `Game/Main/BF2Engine.cpp:419`). Each frame the engine
  runs the ticks that have become due on its clock, up to 10. When more than 10 are due it runs 10 and drops the
  rest, so the game falls behind real time (`IO/TickCalculator.cpp:20-52`).
- A dedicated server sleeps at the end of each frame until 1/30 s minus `server-sleep-overhead-time` has passed
  since the frame began (`Game/Main/BF2Engine.cpp:1949-1972`). The variable's default is 5 ms (`:1567-1568`), so a
  frame takes 28.3 ms and the server shows about 35 frames per second.
- Frames are therefore shorter than ticks. In normal running five ticks in six follow the one before by one frame
  (28.3 ms) and the sixth by two (56.7 ms), because the frame between them had no tick due.
- When a frame starts late, the ticks that became due meanwhile run in that one frame, directly after each other.

### `0x121E ServerHealth`

After the sequence number and the tick:

| Type | Field |
|---|---|
| `u16` | ticks in the window (300) |
| `u32` | real time the window took, microseconds |
| `u8` | flags, below |
| `u32` | tick interval, median, microseconds |
| `u32` | tick interval, 99th percentile |
| `u32` | tick interval, longest |
| `u16` | late ticks: intervals above 66 667 microseconds (two ticks) |
| `u16` | short ticks: intervals below 16 667 microseconds (half a tick) |
| `u32` | main-thread CPU time per tick, median, microseconds |
| `u32` | main-thread CPU time per tick, 99th percentile |
| `u32` | main-thread CPU time per tick, largest |
| `u32` | the module's tick handler, 99th percentile, microseconds |
| `u32` | the module's tick handler, longest |
| `u32` | involuntary context switches of the main thread in the window |
| `u32` | major page faults of the main thread in the window |
| `u16` | the host's CPUs |
| `u16` | host CPU busy, hundredths of a percent of all CPUs |
| `u16` | host CPU steal, hundredths of a percent |
| `u16` | host CPU I/O wait, hundredths of a percent |
| `u32` | the server process's resident memory, KiB |

| Flag bit | Meaning |
|---|---|
| 0 | stepped time is on (`stepMode 1`): the tick figures do not describe a real-time server |
| 1 | the host figures are valid |
| 2 | a level was unloaded during the window: the load that followed is in the tick figures |

- **Tick interval.** The real time between the start of the module's tick handler on one tick and on the next, from a
  steady clock. On a healthy server the median is one frame, the 99th percentile and the longest are two frames, and
  there are no late and no short ticks.
- **Late ticks** count the intervals in which the simulation stood still for more than two ticks. The limit is above
  the two-frame interval of normal running.
- **Short ticks** count the ticks that ran in the same frame as the one before: a frame started so late that two
  ticks were due. With the default frame length a frame more than 38 ms late always causes one, and a frame 5 to
  38 ms late does when a tick was about to become due.
- **Ticks lost.** The window always has 300 ticks. When the engine dropped ticks, the window's real time is longer
  than 10 s by the time they stood for: `real time × 30 − ticks`, within one tick.
- **CPU time per tick** is the main thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`) between the same two points:
  everything the main thread did for one tick, the engine's work and the module's. A tick has 33 333 microseconds;
  the nearer the 99th percentile is to that, the less room the server has.
- **The module's tick handler** is the module's own part of that, the bot channel and the admin channel together.
- **Involuntary context switches** count how often the kernel took the main thread off a CPU while it still wanted
  to run. **Major page faults** count how often it waited for a page to be read from disk: the host is short of
  memory. Both come from `getrusage(RUSAGE_THREAD)`.
- **Host figures** are the differences of `/proc/stat` over the window: busy is user, nice, system and interrupt
  time, steal is time the hypervisor gave to other machines, I/O wait is idle time with disk I/O outstanding. In a
  container they describe the whole machine the container runs on. The resident memory is `/proc/self/statm`. They
  are read once a second by a thread of their own, so a window's figures cover its 10 s to within a second. Flag bit
  1 is clear, and the three percentages are 0, when `/proc/stat` could not be read or no new sample fell in the
  window.
- **A level load** stops the ticks for as long as it takes. The window it falls in has flag bit 2 and one very long
  interval; a client should leave such a window out of its statistics.
- Nothing in the event is read from the engine, and the main thread makes no file or socket call for it: two clock
  reads per tick and one `getrusage` per window.

### `0x121F PlayerConnections`

After the sequence number and the tick:

| Type | Field |
|---|---|
| `u8` | N, players with a reading in the window |
| N × entry | below, 40 bytes each |

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | readings the figures are from, 1 or 2 in a full window |
| `u16` | ping at the last reading, ms |
| `u16` | lowest ping of the readings |
| `u16` | highest ping of the readings |
| `f32` | packets per second the server sent to the player |
| `f32` | packets per second the server received from the player |
| `f32` | bytes per second sent |
| `f32` | bytes per second received |
| `f32` | packets per second lost on the way to the player, mean of the readings |
| `f32` | the same, the highest reading |
| `f32` | packets per second lost on the way from the player, mean of the readings |
| `f32` | the same, the highest reading |

- **Source.** `IGameServer::getConnectionStatus` (`Game/IGameServer.h:52`), which returns the network layer's record
  for a connection (`ConnectionStatus`, `IO/Network/ProtocolLayer/NetServer.h:28-37`; `NetServer.cpp:177-191`). A
  connection's id is its player's id (`Game/GameServer/GameServer.cpp:3915`). No engine change is involved.
- **What a reading covers.** The network layer keeps 10 records per connection, each over at least 500 ms, and the
  record it returns is their average (`IO/Network/ProtocolLayer/SWConnection.cpp:487-535`, `:540-566`): about the last
  5 s. The module reads every connection every 150 ticks, so the two readings of a window cover its 10 s. Records
  that are not filled yet count as zero, so the figures of a player's first 5 s are too low.
- **Who is in it.** Connected human players. A bot has no connection and no entry. A player who joined during the
  window has one reading; a player who left during it has none.
- **Packets sent and received** are datagrams of the game's own protocol. A client sends 10 or 30 a second and is
  sent 4 to 20, depending on the connection type it chose, so the two rates also show that choice.
- **Lost on the way to the player** is what the player's acknowledgements report missing. **Lost on the way from the
  player** is gaps in the sequence numbers the server received; a packet that arrives out of order is counted as lost.
  Divide by the packet rate of the same direction for a loss ratio.
- **Ping** is the round trip of the ping request the server sends each connection every 3 s
  (`NetServer.cpp:434-435`, `:1340-1357`), averaged over the same 10 records. `PlayerState`'s ping is the same
  measurement as the player object holds it.
- **Values.** A rate that is not a finite number of 0 or more is sent as 0, and a ping above 65 535 as 65 535.

## Engine interface

The OpenBF2 engine exports no symbols, so the module uses only typed virtual calls on engine interfaces. What it
cannot reach that way is added to `IServerExtension`
(`Code/BF2/Game/GameServer/IServerExtension.h` on the OpenBF2 `serverext/r1` branch). The interface is append-only
and a separate adapter object, so no retail vtable or layout changes. Every addition is non-retail behaviour, marked
`[CHOSEN, not recovered]` in the engine source. The bot channel's additions (R1 to R3) are in
[protocol.md](protocol.md).

| Id | Addition | Used for |
|---|---|---|
| R4 | Script-event observer: `setScriptEventListener`, `IScriptEventListener`, `ScriptEventArg` | every event that comes from a script event |
| R5 | Damage observer: `addDamageObserver` | `Damage`; shared with the bot channel |
| | Projectile observers: `addProjectileObserver`, `getProjectileInfo`, `addProjectileCollisionObserver` | `ProjectileFired`, `WeaponFire`, the projectile streams, the hit/miss replay |
| | Getters: `getServerInfo`, `getRoundInfo`, `getTeamName`, `getCdKeyHash`, `getDisconnectReason`, `getMapList`, `getGameStatus`, `getBeginRoundState`, `getTimeToNextStateChange`, `getLagCompensation` | `Welcome`, `GameState`, `PlayerInfo`, `PlayerState`, `PlayerDisconnect` |
| | Actions: `runMapListEntry`, `restartRound`, `sendGameLogicEvent`, `sendHudEvent`, `sendMedalEvent`, `setRank`, `killPlayer` | `Map`, `GameEvent`, `HudEvent`, `Medal`, `Rank`, `KillPlayer` |
| R7 | Client presentation: `sendHudEventRaw`, `sendRoundClock`, `issueSquadOrder` | `HudEventRaw`, `RoundClock`, `SquadOrder` |
| R8 | Client resync: `resyncClient`, `resyncClientStatics` | `ResyncClient` (draft) |

**R4, the script-event observer.**

- **Site.** One call at the top of `PythonHost::handleEvent`, after its two early returns and before the handler
  loop. With no listener the cost is one load and one branch per script event.
- **Decoder.** The engine decodes the event's Python argument tuple to typed values without changing it: ints,
  floats, strings, nested tuples (flattened, up to three levels and 64 entries), object wrappers (resolved to the
  object, null when it is gone) and player wrappers. Anything else is delivered as none.
- Nothing is delivered while Python is not initialised or muted, as for Python's own handlers.
- **Contract for the listener:** read only. No Python, console, object creation or deletion, damage or kills inside
  the callback.

**R7.** The three methods run on the main thread and live only in the extension's own files. `sendHudEventRaw`
refuses an id outside the whitelist and a size above 52. The module checks the same whitelist when it parses
`HudEventRaw`. A module that calls these methods must never be loaded into an engine built without them, as with
every appended method.

**R8, the client resync** (draft). `resyncClient` and `resyncClientStatics` are in the extension's own code. They
use what the retail server already has: the reset event and connection reset of a round restart, and the join
database. Two things are new:

- `ServerConnection::clientSendDatabase` is split in two. Its first part, the object database, is now
  `clientSendObjectDatabase`, which `clientSendDatabase` calls first; a join sends what it sent before. For a
  resync it can send the part without the static ghosts, or a slice of the static ghosts by networkable id.
- The last event of the replay is a `PostRemoteEvent` subclass that sets the connection's reset latch when the
  client acknowledges it (`GameServer::confirmReset`). On the wire it is an ordinary game-status event.

Everything else the commands need has a typed path and no engine change: the console, chat, kick, object templates
and the object manager, armour, transforms and physics, score fields and teams. `Ban` runs through the console,
because the ban list is not reachable.

## Cost

Measured on a 30 Hz server with bots:

- **Main thread.** The admin channel adds 20 to 40 µs per tick at p99. An idle script costs about 1 µs per tick; a
  script that reads one player and sets one velocity about 20 µs.
- **`PlayerStream`.** 59 bytes per player per frame: about 114 KB/s for 64 players at 30 Hz, about 8 KB/s for 4
  players. Pick a low rate (1 to 5 Hz) where the client does not animate.
- **`AimStream`.** 50 bytes per living player per frame: about 96 KB/s for 64 players at 30 Hz.
- **`WeaponFire`.** About 80 bytes per event, about 0.8 KB/s while one gun fires.
- **Scripts.** QuickJS-NG adds about 1.2 MB to `libserverext.so` and no shared library.
- **`ServerHealth`.** 79 bytes per 10 s. Measuring costs two clock reads per tick, well under a microsecond.
- **`PlayerConnections`.** 17 bytes plus 40 per human player per 10 s. One call per player every 5 s; the engine
  gathers the figures whether they are read or not.

If stream bandwidth becomes a problem, the cheap step is a stream encoding that skips entities unchanged since the
last frame, with a full frame every second. That is a new stream encoding, not a protocol redesign.

## Testing a client

- **Golden frames.** `tests/fixtures/admin-v1-golden.txt` holds one encoded frame per message type, one per line as
  `name type-hex frame-hex`. The module's unit tests (`tests/AdminTests.cpp`) check the C++ codec against it. A
  client codec should decode and re-encode the same file byte for byte; BF2WebAdmin's codec tests do.
- **Reference client.** `tools/adminclient/adminclient.py` (Python 3, standard library) is a complete client: a TLS
  listener that creates its certificate and prints its pin, the handshake, the snapshot, subscriptions, every
  command, test scenarios and a fuzz mode (`--fuzz`) that sends hostile frames. `--write-golden` writes the golden
  file. Point a test server's `adminConnect` at it to see the protocol from the client's side.
- **Hostile input.** A malformed or hostile client must never crash or stall the server. The unit tests cover
  every truncation of every command; the fuzz mode covers a live link.

## Not implemented

- **Policies and veto hooks.** Rules the module would evaluate inside the tick (no-damage zones, damage
  multipliers, refusing a team switch, spawn rules) need engine veto hooks. There are no messages for them.
- **Bot management from the admin channel.** Bots are created and driven through the bot channel only. The `Bot`
  event reports them.
- **Delta-encoded streams.** See [Cost](#cost).
- **More presentation events.** Raw game-logic events (unknown subtypes abort the client), kill-feed events,
  commander drops and a control point's take-over counter.
- **A Windows build.** The module builds for Linux x86-64 only.

## Known limitations

- **No version negotiation.** Module and client are deployed together. A command without a capability bit closes
  the link on a module that does not know it.
- **The automated tests use bots and the reference client.** Paths that need game clients are outside them:
  `Revive`, `ControlPoint`, a real kick or ban and its disconnect reason, the timed-out reason, and the CD-key
  hash.
- **Disconnect reason "left" is inferred.** The engine drops a client that quits with its generic "refused or
  dropped" result, which the module reports as left.
- **No CD-key hash on a LAN server.** `PlayerInfo`'s hash is empty there and a ban by key is refused.
- **Zero hit points on an occupied object would abort the server.** `SetHealth` refuses it; `KillPlayer` with the
  vehicle bit avoids it.
- **`SetHealth` reports nothing.** It bypasses the damage path, so no `Damage`, `Kill` or `VehicleDestroyed`
  follows.
- **Spawned objects are often invisible.** A template without a networkable exists on the server only. Map changes
  that clients must see need console lines, and so `adminAllowConsole 1`.
- **`ResyncClient` is tested with one retail client and no running round** ([What a player sees](#what-a-player-sees)).
  A TV missile in flight is lost to its gunner.
- **A soldier cannot be turned.** `Teleport`'s rotation, and a script's, do not stick on a soldier.
- **Teleported and script-moved bodies are predicted by their pilot's client.** That pilot may see corrections.
  What a client sees of a vehicle without collision on the server is unverified.
- **`AimState` for soldiers.** Whether a soldier's weapon transform follows its aim pitch on the server is
  unverified. Use the camera.
- **An overloaded link is closed, not resynchronised.** Event drops start at `adminQueueBytes` and the link closes
  at four times that. With large socket buffers the close can come before the client reads a gap.
- **The HMAC proofs are not bound to the TLS session.** The certificate pin prevents a relay.
- **Presentation commands are read from the reconstructed client code.** Verify an event on a retail client before
  relying on it.
- **Votes need the `bf2` mod's voice message list**, and their ballots depend on the client's key handling
  ([Costs and constraints](#costs-and-constraints)).
- **The script time budget stops mistakes, not attacks** ([Limits](#limits)).
- **Seat preferences cover the first two seats** and not man-down players
  ([Lifetime and scope](#lifetime-and-scope)).
- **`PlayerConnections` is read while the engine's network thread writes it.** `NetServer::getConnectionStatus`
  looks the connection up without the network layer's lock (`NetServer.cpp:160-161`), as the engine's own send path
  does for every connection every frame (`:879-889`). A reading can therefore mix two moments' figures. The module
  adds two such lookups per player per 10 s to the engine's own.
- **`PlayerConnections` has no real-client test.** The automated tests have no game client, so only the empty event
  and the codec are covered; the figures of a real connection are unverified.
- **No measure of late input.** Whether a player's input or steering reached the server in time for its tick is not
  in either event; it would need an engine addition.
