# Admin channel (approved draft 2, implemented)

Status: **approved by the author on 2026-10-22**, with the decisions in [Decisions](#decisions).
**Phase 1 implemented 2026-10-23** (OpenBF2 `873d1b1`, module `f62ae97`..`0aab582`,
`docs/findings/admin-phase1.md`). The wire format below was approved by the author on 2026-10-23;
phase 2 (C#) builds against it. **Phase 2 implemented 2026-10-23** in BF2WebAdmin, branch
`admin-channel` (`8973560`, `f3c4801`, `626a780`, its `docs/admin-channel.md`), tested end to end in
the BF2WebAdmin localtest with an OpenBF2 container (OpenBF2 `873d1b1`, module `0aab582`, Debian 11
with OpenSSL 1.1.1w) and bots. It replaces draft 1, which kept
`mm_webadmin.py`'s text protocol.
**Viewer additions approved by the author on 2026-10-28** (for a 3D viewer in WebAdmin): the view mode in
`PlayerState`, the aim stream (5), and the `WeaponFire` (18) and `Weapons` (19) event streams. The admin
protocol version stays 1; module and WebAdmin are deployed together. Implemented and tested on a WSL server
with a firing bot crew: `docs/findings/admin-viewer.md`.
**Hit/miss replay approved by the author on 2026-10-28** (for the viewer): the lag in `PlayerState`, and the
`ProjectileCollision` and `ProjectileTrace` events on stream 20 (the section under `WeaponFire` below). Engine getter
`IServerExtension::getLagCompensation` and the projectile-collision observer (OpenBF2 `serverext/r1` `0a58ec0`,
`1ca26b8`). Version stays 1. Implemented and tested on a WSL server with scripted bots: `docs/findings/tv-hitbox.md`.
The author has decided that the BF2WebAdmin C# side may change as much as needed, and that the
author keeps deployments from running `mm_webadmin.py` and the native channel at the same time.
Evidence for the engine side: `docs/findings/admin-channel-a1-a4.md`. Inventory of today's
protocol and its problems: draft 1, in git history (`5fc2a6d`).

## Principles

1. **Typed and id-based.**
   - Players are referred to by player id. Vehicles, objects and projectiles are referred to by
     networkable id.
   - Nothing travels as text to be parsed. That ends console grid ids, `findId()`, `\b`-joined
     rcon output, `"%.1f"` position strings and Windows-1252.
2. **State, not only events.**
   - The module already builds a per-tick world snapshot for the bot channel. WebAdmin subscribes
     to the parts it wants, at the rate it wants, up to 30 Hz.
   - Events carry full context: ids, templates, positions, vehicles.
3. **Nothing blocks the game thread.** The game thread only reads and queues; all I/O happens on
   the I/O thread. The per-tick cost is measured and stays under the bot channel's budget
   (p99 < 1 ms).
4. **Resynchronise by snapshot.**
   - On every connect, the module sends a full snapshot, then events with sequence numbers.
   - WebAdmin detects a gap from the numbers and asks for a new snapshot.
   - The connection is never silently left inconsistent.
5. **Decisions inside a tick run in the module.**
   - Vetoing damage or refusing a team switch must be decided within the tick, so it can't wait
     for a network round trip to WebAdmin.
   - Instead, WebAdmin pushes **policies** (rules). The module evaluates them locally and reports
     what it did.
6. **One protocol family.**
   - The same framing, error handling and codec style as the bot channel, with its own version.
   - Golden-byte tests are shared by the C++ and C# codecs, as they are today for the bot channel.

## Transport and security

**Connection**
- The module dials out to WebAdmin (`adminConnect host:port` in `serverext.cfg`). One central
  WebAdmin serves many game servers, as today.
- Connecting and reconnecting with backoff happen on the I/O thread. The old rcon-triggered
  reconnect (`wa connect`) is gone.

**Authentication**
- Mutual HMAC-SHA256 challenge-response with a key per server (`adminKey`). Both sides prove they
  hold the key.
- The server's identity is a configured server id, not its source IP.

**Encryption** (B1: the game servers reach WebAdmin over the internet)
- TLS 1.3, in the module, with OpenSSL (the distro's `libssl3`, linked dynamically; the Docker image
  installs it). The module's I/O thread drives a non-blocking `SSL` object on the existing socket.
- WebAdmin presents a dedicated self-signed certificate on the game-server port, generated once.
  .NET wraps the accepted socket in `SslStream`.
- Each server pins it: `adminCertSha256 <hex>` in `serverext.cfg`, the SHA-256 of the certificate's
  public key (SPKI). No CA store, hostnames or renewals; rotating the key means updating the pin.
- The HMAC challenge runs inside TLS and still authenticates the server to WebAdmin.
- `adminTls 0` exists only for loopback tests. With TLS, IPs and CD-key hashes are sent by default;
  `adminSendIdentity 0` withholds them.

**Capabilities**
- `serverext.cfg` can turn off powerful commands per server: raw console, object spawn and delete,
  and policies. A leaked key then can't do more than the server allows.

## Messages (outline; byte layouts once approved)

### Session

| Message | Content |
|---|---|
| `Hello` / `Challenge` / `Auth` / `Welcome` | Version, server id and nonces, then the server info: name, ports, max players, the map list for every mode, current map and mode, team names, tickets and game state |
| `Snapshot` | Every player, vehicle, control point and tracked projectile, plus game state and tickets (fields below) |
| `RequestSnapshot` | WebAdmin to module, after a sequence gap or at any time |

### State in the snapshot and the streams

| Entity | Fields |
|---|---|
| Player | id, name, team, squad, bot flag, profile id, IP and CD-key hash (sent unless `adminSendIdentity 0`; WebAdmin keys auth and match records on the hash), ping, score, kills, deaths, team kills, alive or man-down or dead, vehicle id and seat, position and rotation (of the root vehicle when seated) |
| Vehicle | networkable id, template, team, position, rotation, velocity, health, destroyed, occupied seats |
| Control point | id, template, team, capture state |
| Projectile | id, template, owner player, position, velocity (only templates WebAdmin subscribes to, e.g. TV missiles) |
| Game | state (all six, including Paused), tickets and score for both teams, round time |

**`Subscribe(stream, rateHz, filter)`**
- The streams are player states, vehicles, projectiles (filtered by template) and tickets.
- Each is a plain snapshot of the subscribed entities at the chosen rate (B3). Positions and
  rotations are float32; WebAdmin picks low rates (1-5 Hz) where it doesn't animate.

### Events (sequence-numbered)

For one victim, `Kill` comes before `Death`. `Spawn` also fires for a spawn straight into a vehicle.

| Event | Notes |
|---|---|
| GameState, RoundStart, RoundEnd | including Paused, which Python never sees; RoundEnd carries tickets and the winner |
| PlayerConnect, PlayerDisconnect | with the reason (left, kicked, timed out) |
| Spawn, Death, Revive | Revive carries the medic and the revived player |
| TeamChange | from any source: player, admin or autobalance. The module detects the change itself, because the engine doesn't always raise an event |
| ScoreChange | detected by the module from any source |
| Kill | attacker, victim, weapon template, both positions, both vehicles, and the kind: kill, team kill, suicide, roadkill or crash (no attacker). Fixes today's missing crash reports and the wrong victim position |
| Damage | victim (player or object), attacker, weapon or projectile template, amount, health before and after, hit position. Needs R5 |
| EnterVehicle, ExitVehicle, VehicleDestroyed | with real ids and the seat, and the attacker where known |
| ControlPoint | captured or neutralised, the new team |
| Chat | channel, player, text, flags |
| ProjectileFired, ProjectileHit | for subscribed templates |
| Kick, Ban | notices |
| Bot | created, removed and team changes for bot-channel bots, so WebAdmin can tell them apart |

### Commands (each with a request id and a typed result)

| Command | Notes |
|---|---|
| `Console` | executes a line and returns its output. Runs in order. Can be turned off |
| `Say` | to all, a team or one player |
| `Teleport` | a player's root vehicle or any object id: position, rotation, or both |
| `SetHealth`, `KillPlayer` | `KillPlayer` states whether the player's vehicle is destroyed too (today's `damage 1` does) |
| `SetTeam` | optionally killing the player |
| `SetScore` | score, kills, deaths |
| `Rank`, `Medal` | |
| `GameEvent`, `HudEvent` | including the 0 values today's Python skips |
| `Kick`, `Ban` | For a bot these map to `RemoveBot`, or are refused, never to the address `N/A` ban |
| `Map` | restart, next, or set map and mode |
| `ListObjects` | template filter; returns ids, templates, positions, rotations and health. Replaces `listObjectsOfTemplate` parsing. Returns the console id beside the networkable id, because static map objects (fences) may have no networkable id |
| `SetObjectActive` | by console or networkable id; replaces `object.active id<N>` for Freeze, Heli2v2 noclip and `.nofences` |
| `SpawnObject`, `DeleteObject` | template, position, rotation; for map modules. Can be turned off |
| `Subscribe`, `Unsubscribe` | streams |
| `Ping` | keepalive either way, so a dead link is noticed |
| `SetPolicy`, `ClearPolicy` | phase 3 |

### Policies (phase 3)

| Policy | Effect |
|---|---|
| Damage | zones with no damage (base protection), multipliers per weapon or vehicle, friendly-fire rules |
| Team | refuse a team switch that would stack teams |
| Spawn | per team, group or vehicle |

The module reports every action it takes as an event. Policies need engine veto hooks (R6), each
designed and approved on its own.

### Conventions

- Rotations use the bot channel's convention (`docs/protocol.md`, Conventions).
- Text is UTF-8. `§` (BF2's colour-code prefix) maps to byte 0xA7 at the engine boundary, so colour
  codes keep working.
- The `Welcome` map list carries each map's mode and size.
- WebAdmin's server id is its existing `Server.ServerId` value; the key is a new `AdminKey` column.

## Engine changes (OpenBF2 branch `serverext/r1`, each `[CHOSEN, not recovered]`)

| Id | Change | Notes |
|---|---|---|
| R4 | Script-event observer | One call at the top of `PythonHost::handleEvent`, with an engine-side decoder to typed arguments. Covers every retail script event in one place |
| R5 | Damage observer | Read-only, at the point where damage is applied. **The bot channel's approved v2 damage event needs the same hook**, so it is built once for both |
| — | Appended `IServerExtension` methods | CD-key hash, team names, tickets, round time, game and HUD events, medal, rank, player message, and object spawn and delete if no typed path exists |
| R6 | Veto hooks | Phase 3 only |

## The C# side

- **A new backend in BF2WebAdmin:**
  - a listener with the new protocol and authentication;
  - a C# codec with the shared golden-byte tests;
  - events and state fed straight into its domain model, with no text lines.
- The survey of what changes is `docs/findings/admin-csharp-inventory.md`: roughly 25-30 files
  touched, 10-12 new, and the old socket/parser/writer removed.
- **Game modules change where they relied on the text protocol.** Heli2v2's `listObjectsOfTemplate`
  parsing becomes `ListObjects`. `RootVehicleId` becomes the networkable id. Rcon `\b` parsing and
  ordered raw console strings become typed commands where one exists.
- **The `mm_webadmin` backend is replaced, not kept** (B2). Until the new backend is ready, the
  author runs an old WebAdmin Docker image for the existing servers. The author makes sure no
  server runs `mm_webadmin.py` and the native channel at the same time.

## Phases

1. **Engine and module.**
   - R4, R5 (shared with bot v2) and the getters.
   - The admin channel's session, authentication and TLS, snapshot, events, streams and the core
     commands. The byte layouts go into this document for review before phase 2.
   - A Python reference client with a fuzz mode, as for the bot channel.
   - Verify that OpenBF2 computes a CD-key hash for each player; WebAdmin depends on it.
   - Verified on the WSL and Docker test servers.
2. **BF2WebAdmin.**
   - The C# codec and backend, and the module refactors.
   - Tested end to end with the netsky 2v2 setup in the BF2WebAdmin local test environment
     (`C:\Data\Projects\DotNet\BF2WebAdmin-localtest`; workers may start it with its scripts).
     Its BF2 container must run OpenBF2 with the module instead of `mm_webadmin.py`.
3. **Policies and veto hooks.** Not started.

TLS is part of phase 1 (B1).

## Wire format (admin protocol version 1, phase 1)

Copied from `docs/findings/admin-phase1.md`, where it was written before the code.

#### Framing and conventions

- **Transport.** TCP from the module to WebAdmin (`adminConnect host:port`), inside TLS 1.3 unless
  `adminTls 0`. The module dials; WebAdmin listens.
- **Frame.** The bot channel's header: `u32` payload length, `u16` type, `u16` reserved (0), then
  the payload. Little-endian. Strings are `u16` byte length plus UTF-8, no terminator.
- **Frame size.** WebAdmin → module payloads are at most 64 KiB, as on the bot channel. Module →
  WebAdmin payloads may be up to 1 MiB, because `Snapshot`, `ObjectList` and `ConsoleResult` can be
  large. A larger length, or a nonzero reserved field, is a framing error: `Error` 2 and close.
- **Its own version and type range.** The admin channel is version 1 of its own protocol. Its types
  are `0x1001`-`0x15FF`; only `0xFFFF Error` is shared with the bot channel, with the same layout.
  Nothing on this connection is a bot-channel message.
- **Errors.** As on the bot channel: framing or parse error `Error` 2 and close; an unknown type, or
  a type sent in the wrong direction, `Error` 3 and close; per-request errors keep the connection.
- **Player ids** are `u8` where a player must exist and `i16` where none is possible (`-1` = none).
  The bot channel's presence bits are not reused here: bots count down from 255, and an `i16` cannot
  be misread.
- **Object ids.** A networkable id is `u32` (the engine's id is 16 bits; 0 = none). A console id is
  the object's grid id, the `N` in `object.active idN` (`IObject::getObjectId`, resolved by
  `ObjectManager::getObjectFromGrid`, `Scene/Object/ObjectTemplateUtil.cpp:76-85`), as `u32` with
  `0xFFFFFFFF` = none. Static map objects can have a console id and no networkable id.
- **`ObjectRef`** (commands that act on an object):

  | Type | Field |
  |---|---|
  | `u8` | kind: 1 a player's root object (its vehicle when seated, else its soldier), 2 networkable id, 3 console id |
  | `u32` | id: the player id for kind 1 |

- **Vectors.** `Vec3` is 3 × `f32` x, y, z (metres; m/s for velocities). `Rot` is 3 × `f32` yaw,
  pitch, roll in degrees, the bot channel's convention (`docs/protocol.md`, Conventions). It is
  the same as Python's `getRotation`: Python returns the negated, reversed ZXY angles
  (`Game/Python/py/Objects.cpp:611-628`), and `getRotationZXY` (`Math/Mat4.cpp:234-264`) gives
  z = atan2(-m1, m5), x = asin(m9), y = atan2(-m8, m10), so Python's (yaw, pitch, roll) is
  (atan2(m8, m10), -asin(m9), atan2(m1, m5)), which is the bot channel's formula. The draft's
  rotation gap in `admin-csharp-inventory.md` section 2 therefore needs no conversion. Setting a
  rotation uses the exact inverse, `setRotationZXY(-roll, -pitch, -yaw)` (`Math/Mat4.cpp:167-232`),
  which is also what `IGame::createObjectOnAllClients` does with its rotation argument
  (`Game/GameServer/GameServer.cpp:2327`).
- **Text** is UTF-8 on the wire and Windows-1252 in the engine. The module converts both ways, so
  `§` (U+00A7) is byte 0xA7 in the engine and colour codes keep working. Inbound text that is not
  valid UTF-8 is `Error` 7; a character with no 1252 byte becomes `?`.
- **Order.** The game thread runs WebAdmin's commands in arrival order, at most 64 per tick (the rest
  on the next tick), and queues each reply when the command has run. Everything the module sends
  (replies, `Snapshot`, events) goes out in the order it was queued, except stream frames
  (latest wins, written after the queued frames of each flush).

#### Sequence numbers and resync

- Every event carries a `u32` sequence number. Numbers count per link session: the first event after
  the `Snapshot` that follows `Welcome` is 1, and each event sent on that session is the previous
  plus one.
- **`Snapshot.lastSeq`** is the number of the last event whose effect the snapshot already includes.
  WebAdmin drops events with a number at or below it and applies the rest.
- **Gaps.** The module never blocks on a slow WebAdmin. When its outbound queue exceeds
  `adminQueueBytes` (default 4 MiB), it drops new events instead of queueing them; their numbers are
  used up, so WebAdmin sees the gap and sends `RequestSnapshot`. Replies and snapshots are never
  dropped; if the queue reaches four times `adminQueueBytes` the module closes the link and
  reconnects.
- **Reconnect.** A new link gets a new session number in `Welcome`, a full `Snapshot` and numbering
  from 1. Subscriptions do not survive a reconnect.

#### Session (`0x10xx`)

The module connects, completes TLS (pinned, below), and sends `Hello`. WebAdmin answers `Challenge`.
The module checks WebAdmin's proof, then sends `Auth`, `Welcome` and `Snapshot` without waiting.
WebAdmin checks the module's proof before it uses anything after it; on a bad proof it sends `Error`
8 and closes. The whole handshake, TLS included, must finish within 10 s of the TCP connect.

**`0x1001 Hello`** (module → WebAdmin)

| Type | Field |
|---|---|
| `u16` | admin protocol version, 1 |
| string | server id (`adminServerId`, 1-64 bytes; WebAdmin's `Server.ServerId`) |
| 32 bytes | module nonce `Nm` (random, from OpenSSL `RAND_bytes`) |
| string | module build, for logs (for example `libserverext admin 1`) |

**`0x1002 Challenge`** (WebAdmin → module)

| Type | Field |
|---|---|
| 32 bytes | WebAdmin nonce `Nw` (random) |
| 32 bytes | WebAdmin proof `Pw` |

**`0x1003 Auth`** (module → WebAdmin)

| Type | Field |
|---|---|
| 32 bytes | module proof `Pm` |

Proofs, with `K` the raw key (`adminKey` as hex, 16-64 bytes) and `id` the server id bytes:

```
T(label) = label || 0x00 || u16le(len(id)) || id || Nm || Nw
Pw = HMAC-SHA256(K, T("OBF2-ADMIN-1 webadmin"))
Pm = HMAC-SHA256(K, T("OBF2-ADMIN-1 server"))
```

- The labels are ASCII. Different labels mean neither side's proof can be replayed as the other's.
- WebAdmin proves the key first, so the module sends nothing that depends on the key, and no server
  information, to a peer that has not proven it.
- Compare proofs in constant time (`CRYPTO_memcmp`; .NET `CryptographicOperations.FixedTimeEquals`).
- **Not bound to the TLS session.** .NET's `SslStream` has no TLS exporter, so the transcript does
  not include TLS keying material. The pinned certificate is what prevents a relay; the HMAC proves
  the key.
- A version other than 1 in `Hello`: WebAdmin sends `Error` 1 and closes. An unknown server id or a
  server with no key: `Error` 8 and close. A bad `Pw`: the module sends `Error` 8, closes and backs
  off as after any failed attempt.

**`0x1004 Welcome`** (module → WebAdmin)

| Type | Field |
|---|---|
| `u16` | admin protocol version, 1 |
| `u32` | session number: 1 for the first link since the server process started, then +1 per link |
| `u32` | current module tick |
| `f32` | tick rate, Hz (30) |
| string | server name (`IGameServer::getName`) |
| `u16` | game port (`sv.serverPort`) |
| `u16` | GameSpy query port (`sv.gameSpyPort`) |
| `u8` | maximum players (`sv.maxPlayers`) |
| `u8` | capabilities: bit 0 `Console` allowed, bit 1 `SpawnObject`/`DeleteObject` allowed, bit 2 IPs and CD-key hashes are sent |
| string | current map name |
| string | current game mode |
| `u16` | current map size (`IGame::getCurrentMapSize`) |
| `u16` | current map-list index, `0xFFFF` if none |
| `u16` | map-list entries M |
| M × entry | string name, string game mode, `u16` size: the server's rotation (`maplist.list`), every mode |
| string | team 1 name (`IGameLogic::getTeamName(1)`) |
| string | team 2 name |
| `GameState` | below |

#### State (`0x11xx`)

**`GameState`** (in `Welcome`, `Snapshot`, `GameStream` and the `GameState` event)

| Type | Field |
|---|---|
| `u8` | game status (`IGameLogic::getGameStatus`): 1 playing, 2 end game, 3 pre game, 4 paused, 5 restart, 6 not connected; 0 none |
| `u8` | begin-round state: 0 announce, 1 waiting for players, 2 pre-round countdown, 3 running; 255 none |
| `f32` | seconds to the next state change, -1 if none |
| 2 × `i32` | tickets of teams 1 and 2, -1 if unknown |
| `f32` | round time: seconds since the round started (`ServerGameLogic` round-start tick), -1 if not started |
| `f32` | time limit, seconds (`sv.timeLimit`), 0 if none |
| `u8` | winner of the last round, 0 if none |
| `u8` | victory type of the last round, 0 if none |

**`PlayerState`** (per player, in `Snapshot` and `PlayerStream`)

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
| `u8` | camera view mode (`IPlayer::getCamera()->getViewMode()`, `ICameraObject` +0x1C): 1 inside, 2 nose, 3 chase, 4 front chase, 5 fly-by, 6 trace, 7 extern trace, 0 none (the bot channel's values, `docs/protocol.md` v2 `TickState`); 255 when dead or without a camera |
| `u16` | lag compensation, ms: how far back the server tests this player's projectiles against vehicles and soldiers (`IServerExtension::getLagCompensation`; see "Hit/miss replay" A below) |

The view mode is the server's copy: `ServerGameLogic::checkPlayerTriggers` applies each player's camera keys to
the server's camera object (`ServerGameLogic.cpp:1550-1579`, called for every player from
`GameServer.cpp:3788`), and projectile cameras set it too (`ToggleCameraComp.cpp:116-160`). A camera key press
the server never received is not reflected. Added 2026-10-28 with the author's approval; the version stays 1.

**`PlayerScore`** (in `PlayerInfo` and `ScoreChange`)

| Type | Field |
|---|---|
| `i32` | score (`PlayerScoreData::m_score`) |
| `i32` | team score (`rplScore`, `m_field04`) |
| `i32` | kills (`m_field14`) |
| `i32` | deaths (`m_field10`) |
| `i32` | team kills (`TKs`, `m_field18`) |
| `i16` | rank (`m_rank`), -1 if none |

The field mapping is Python's `pmgr_setScore` (`Game/Python/PythonHost.cpp:1474-1511`).

**`PlayerInfo`** (per player, in `Snapshot` and `PlayerConnect`)

| Type | Field |
|---|---|
| `u8` | player id |
| string | name |
| `u8` | flags: bit 0 AI-flagged (any bot), bit 1 created by the module's bot channel, bit 2 connected (`IPlayer::getIsConnected`), bit 3 IP and hash below are sent |
| `i32` | profile id |
| string | IP address, empty if withheld or none (bots) |
| string | CD-key hash, empty if withheld or none (see the CD-key answer below) |
| `PlayerScore` | |
| `PlayerState` | |

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
| `f32` | health 0..1, -1 with no armour component (a pooled wreck) |
| `u8` | seats |
| `u32` | occupied seats, bit 0 = seat 0 |

The set is the bot channel's vehicle set (`docs/protocol.md` v1 "Vehicles"): every root object with
seats that is not a soldier or a camera, including pooled entries at 0/0/0 with health -1.

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
| `u32` | id: networkable id, or the engine object id plus 0x10000 without one (as the bot channel) |
| string | template name |
| `i16` | owner player, -1 if none |
| `u8` | owner team |
| `u8` | flags: bit 0 guided (seek component) |
| `u32` | networkable id of the guided target's root, 0 if none |
| `Vec3` | position |
| `Rot` | rotation |
| `Vec3` | velocity |
| `f32` | time to live, seconds |

Only projectiles whose template is in a projectile subscription are tracked (`Subscribe` stream 3 or
17), at most 256 at once.

**`AimState`** (per living player, in `AimStream`; added 2026-10-28 for a 3D viewer)

| Type | Field |
|---|---|
| `u8` | player id |
| `u8` | flags: bit 0 the camera fields are set, bit 1 the weapon fields are set |
| `Vec3` | camera position: the absolute transform of the player's camera object (`IPlayer::getCamera()` as `IObject`), 0/0/0 without bit 0 |
| `Rot` | camera rotation, the same matrix |
| `Vec3` | weapon position: the absolute transform of the active weapon in slot 0 of the player's seat (`IPlayerControlObject::getActiveWeapon(0)`; on foot the soldier's), 0/0/0 without bit 1 |
| `Rot` | weapon rotation, the same matrix |

- These are the two transforms the server fires along (`FireComp::getProjectileOrigin`,
  `Game/Objects/Weapons/FireComponents/FireComp.cpp:807-865`): the weapon's own, or the camera object's
  when the weapon fires in camera DOF (`fire.fireInCameraDof`) or the shooter is a soldier. The template's
  `projectileStartPosition` offset is added to it (`:874-885`) and is not included here.
- The camera is the server's copy of the view (see the view mode under `PlayerState`): the eye in views 1 and 2, the
  external camera in views 3-7.
- Open: whether a soldier's weapon transform follows its aim pitch on the server. Soldiers fire from the camera
  object, so a viewer should use the camera for a soldier's aim.

**`WeaponSlot`** (in `Weapons`): `u8` slot (0-2), string weapon template.

**`0x1101 Snapshot`** (module → WebAdmin): after `Welcome`, and in reply to `RequestSnapshot`.

| Type | Field |
|---|---|
| `u32` | request id: 0 after `Welcome`, else `RequestSnapshot`'s |
| `u32` | `lastSeq` (above) |
| `u32` | tick |
| `f64` | server time, seconds |
| `GameState` | |
| `u8` | players P, then P × `PlayerInfo` |
| `u16` | vehicles V, then V × `VehicleState` |
| `u16` | control points C, then C × `ControlPointState` |
| `u16` | projectiles R, then R × `ProjectileState` (the subscribed templates only) |

**`0x1102 RequestSnapshot`** (WebAdmin → module): `u32` request id. Reply: `Snapshot`.

**`0x1103 Subscribe`** (WebAdmin → module). Reply `Ok` or `Error` 7.

| Type | Field |
|---|---|
| `u32` | request id |
| `u8` | stream: 1 players, 2 vehicles, 3 projectiles, 4 game, 5 aim; event streams 16 `Damage`, 17 `ProjectileFired`/`ProjectileHit`, 18 `WeaponFire`, 19 `Weapons`, 20 `ProjectileCollision`/`ProjectileTrace` |
| `u8` | rate, Hz, 1..30 (ignored for 16-20) |
| `u8` | filters F |
| F × string | template names, compared without case. Streams 3, 17 and 20 need at least one; stream 18 takes any number (none: every launch; else launches whose weapon or projectile template matches one); the others take none |

Subscribing to stream 19 reports every player's weapons at once (below).

A new `Subscribe` for a stream replaces the old one. The module sends a stream frame every
`round(30 / rate)` ticks.

**`0x1104 Unsubscribe`** (WebAdmin → module): `u32` request id, `u8` stream. Reply `Ok`.

**Stream frames** (module → WebAdmin; latest wins: an unsent frame is replaced by the next one):

| Type | Payload |
|---|---|
| `0x1111 PlayerStream` | `u32` tick, `f64` time, `u8` N, N × (`u8` player id, `PlayerState`) |
| `0x1112 VehicleStream` | `u32` tick, `f64` time, `u16` N, N × `VehicleState` |
| `0x1113 ProjectileStream` | `u32` tick, `f64` time, `u16` N, N × `ProjectileState` |
| `0x1114 GameStream` | `u32` tick, `f64` time, `GameState` |
| `0x1115 AimStream` | `u32` tick, `f64` time, `u8` N, N × `AimState` (living players only) |

#### Events (`0x12xx`, module → WebAdmin)

Every event starts with `u32` sequence number and `u32` tick. The fields below follow them. Sources:
"R4" is the script-event observer (engine change below) with its script-event id; the module
reads the objects during the callback, before Python's handlers run.

| Type | Event | Fields after seq and tick | Source |
|---|---|---|---|
| `0x1201` | GameState | `GameState`, `u8` previous status | `ECGameLogic` id 1 (every status change, Paused included, `ServerGameLogic.cpp:311-318`), and begin-round state changes polled per tick |
| `0x1202` | RoundStart | string map, string mode, 2 × `i32` tickets | status becomes 1 (playing) from anything except 4 (paused) |
| `0x1203` | RoundEnd | 2 × `i32` tickets, `u8` winner (0 none), `u8` victory type | status becomes 2 (end game); `endGame` sets the winner first (`ServerGameLogic.cpp:883-897`) |
| `0x1204` | PlayerConnect | `PlayerInfo` | R4 14 |
| `0x1205` | PlayerDisconnect | `u8` player, `u8` reason: 0 unknown, 1 left, 2 kicked, 3 timed out, 4 banned, 5 other; `u16` engine reason, `0xFFFF` if none | R4 15 plus the pending disconnect type (engine getter below) |
| `0x1206` | Spawn | `u8` player, `u8` team, `u32` root networkable id, `u32` root console id, string root template, `Vec3` position, `Rot` rotation | R4 23 while the player is not yet alive (`spawnPlayer` enters the soldier before `setIsAlive(true)`, `ServerGameLogic.cpp:2061-2062`). A spawn straight into a vehicle is `Spawn` (soldier) then `EnterVehicle` |
| `0x1207` | Death | `u8` player, `u8` team, `u32` networkable id, string template of the object the player was in, `Vec3` its position | R4 3 |
| `0x1208` | Revive | `u8` revived player, `i16` medic | R4 2 |
| `0x1209` | TeamChange | `u8` player, `u8` old team, `u8` new team, `u8` flags: bit 0 bot-channel bot | the module compares every player's team each tick (any source) |
| `0x120A` | ScoreChange | `u8` player, `PlayerScore` | the module compares every player's score fields each tick (any source) |
| `0x120B` | Kill | see below | R4 1 |
| `0x120C` | Damage | see below; only while subscribed (stream 16) | R5 |
| `0x120D` | EnterVehicle | `u8` player, `u8` seat, `u8` flags (bit 0 the child soldier is active), `u32` root networkable id, `u32` root console id, string root template, string seat template | R4 21 (non-soldier objects only) |
| `0x120E` | ExitVehicle | the same fields, for the vehicle left | R4 22 |
| `0x120F` | VehicleDestroyed | `u32` networkable id, `u32` console id, string template, `i16` attacker, `Vec3` position | R4 24, vehicles only (the engine also raises it for dying soldiers) |
| `0x1210` | ControlPoint | `u8` kind (1 captured, 0 neutralised: the engine's argument), `ControlPointState` (the team after the game-mode script ran) | R4 25, reported at the module's next tick (the conquest script sets the new team in its Python handler, after R4) |
| `0x1211` | Chat | `i16` sender (-1 the server), `u8` sender team (for channel 13 the target team), `u8` channel (the engine's number, `GameServer::handleStringReceivedEvent`), string channel name, `u32` engine flags, `u8` text flags (bit 0 a dead prefix was removed), string text | R4 39 |
| `0x1212` | ProjectileFired | `ProjectileState` | the v2 projectile observer, subscribed templates only (stream 17) |
| `0x1213` | ProjectileHit | `u32` id, string template, `i16` owner, `u8` flags (bit 0 expired: time to live ran out, bit 1 it damaged something), `u32` networkable id of the first object it damaged (0 none), `Vec3` last position | a tracked projectile no longer in flight (stream 17) |
| `0x1214` | Kick | `u8` player, string name | R4 17 (`BanList::alertOnKicked`: every kick, ban and vote kick) |
| `0x1215` | Ban | `u8` player, string name, `u8` ban type (0 timed, 1 permanent, 2 round), `u32` expiry (Unix time; 0 permanent) | R4 16 (`BanList::alertOnBanned`) |
| `0x1216` | Bot | `u8` player, `u8` kind (1 created, 2 removed, 3 team change), `u8` team, `u32` bot-channel connection | the module's bot channel |
| `0x1217` | WeaponFire | see below; only while subscribed (stream 18) | the v2 projectile observer |
| `0x1218` | Weapons | `u8` player, `u8` W, W × `WeaponSlot`: the weapons in the active slots 0-2 of the player's seat (on foot the soldier's), as the bot channel's weapon list; W = 0 when dead | only while subscribed (stream 19): polled per tick, sent when a player's set changes, and for every player on `Subscribe` and after every `Snapshot` |
| `0x1219` | ProjectileCollision | see "Hit/miss replay" B below; only while subscribed (stream 20) | the projectile-collision observer |
| `0x121A` | ProjectileTrace | see "Hit/miss replay" C below; only while subscribed (stream 20) | the module's tick hook and the projectile observer |

**`WeaponFire`** (one per shooter and weapon per tick in which it launched; added 2026-10-28):

| Type | Field |
|---|---|
| `i16` | shooter player, -1 if none |
| `u32` | networkable id of the shooter's root vehicle (the launching vehicle's root), 0 if none |
| string | weapon template (the projectile's launcher weapon), empty if none |
| string | projectile template |
| `u8` | rounds: projectiles this weapon launched in the tick, batches (shotguns) included, at most 255 |
| `Vec3` | origin: the first of them at launch |
| `Vec3` | velocity: the first one's at launch, the muzzle velocity plus the vehicle's (`FireComp.cpp:941-956`) |

- The source is the projectile observer in `FireComp::setupProjectile` (`FireComp.cpp:1120-1129`): every projectile
  the server launches, bullets and flares included (the 2v2 helis' `*_FlareLauncher` are `GenericFireArm`s with a
  `GenericProjectile`). Tracer copies are made after it and are not counted (`:1131-1160`).
- The event's tick is the tick of the launch. The module sends it with its next tick's events.

**Hit/miss replay** (approved by the author on 2026-10-28; implemented). For the 3D viewer: replay a missile's whole flight with the target drawn where the gunner saw it
("red", its current server pose) and where the server tested it ("blue", rewound by the shooter's lag), and name the
part a hit touched. Background and measurements: `docs/findings/tv-hitbox.md`. Three additions. Decided by the author
on 2026-10-28: the lag goes into `PlayerState`, and collisions are capped per tick.

**A. Lag in `PlayerState`.** One field appended at the end:

| Type | Field |
|---|---|
| `u16` | lag compensation, ms: how far back the server tests this player's projectiles against other vehicles and soldiers (0 to 300; 33 or less rewinds nothing) |

- The value is the engine's own, `LatencyCompensation::getLatency(player)`: ping x 0.5 plus the interpolation time
  the player's client reports (`IPlayer::getInterpolationTime`), capped by `phy-latencyComp-max` (0.3 s), or the
  forced `phy-latencyComp-debug`; bots are local players and get 0. The module reads it through the appended
  engine getter `IServerExtension::getLagCompensation(int playerId)` (OpenBF2 `serverext/r1` `1ca26b8`,
  `[CHOSEN, not recovered]`), rounded to whole ms.
- `PlayerState` is in `Snapshot`, `PlayerInfo` and `PlayerStream`, so every consumer can use it.

**B. `0x1219 ProjectileCollision`** (event stream 20). `Subscribe` takes projectile templates as filters, at least
one, as stream 17 does. One event per collision the server hands to a subscribed projectile, at most 64 per tick; the
rest are dropped before they get a sequence number (so no gap) and counted in the module's stats line.

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

- Source: the projectile-collision observer, OpenBF2 `serverext/r1` `0a58ec0`
  (`IServerExtension::addProjectileCollisionObserver`), called from `GenericProjectile::handleCollision` on the main
  thread inside the collision response. The module encodes and queues; no I/O, as for every event.
- The tested pose with bit 2 is `IPhysicsNode::getLatencyCompensatedTransformation(latency)` of the part, read in the
  callback (the module reaches the node with `queryComponent(IID_IPhysicsNode)`, as for velocities). The latency
  history only advances at the end of the tick, so this is the transform the collision used. Without bit 2 it is the
  part's absolute transform, so a rotor hit shows the blade that was hit.
- Only parentless objects (vehicle hulls) and soldiers are rewound; child parts (rotors, dummy missiles, sensors,
  gun turrets) are tested where they are now.
- A projectile can collide more than once (ricochet, penetration). A TV missile's direct hit is its first kind 1
  event; the missile then stays at the impact point until its time to live runs out, and `ProjectileHit` follows.
  A miss usually ends in a kind 0 event.
- `Damage` keeps its meaning: damage is booked to the nearest armour up the parent chain, so a rotor hit is a
  `Damage` to the helicopter. Match the two by tick and projectile template.
- The tick is numbered as `Damage`'s: the state the colliding tick started from, which is the `ProjectileTrace`
  sample whose tested poses that tick's sweep used.

**C. `0x121A ProjectileTrace`** (event stream 20, the same subscription). The whole flight of one subscribed
projectile, sent once when it ends, so the viewer can replay it with red and blue visible at every tick. An event,
not a stream frame: it is never replaced by a newer one (only the outbound-queue overflow rule applies).

| Type | Field |
|---|---|
| `u32` | projectile id, as `ProjectileState`'s |
| string | projectile template |
| `i16` | shooter player, -1 if none |
| `u32` | first tick: the tick of sample 0 (samples are consecutive ticks) |
| `u8` | flags: bit 0 the trace was cut at the sample limit, bit 1 it ended at a collision (else the projectile vanished or expired) |
| `u8` | targets T, then T × (`u32` root networkable id, string template) |
| `u16` | samples S, then S × sample |

Each sample:

| Type | Field |
|---|---|
| `Vec3` | projectile position |
| `Vec3` | projectile velocity |
| `f32` | the shooter's lag compensation this tick, seconds |
| T × target | `Vec3` position now, `Rot` rotation now, `Vec3` position as tested, `Rot` rotation as tested |

- **Timing.** Sample `k` carries the stream tick label `first + k`: the state at the end of that tick, taken in the
  module's tick hook before the next tick is simulated. "As tested" is `getLatencyCompensatedTransformation(lag)` of
  each target's physics node at that moment, which is exactly the pose the projectile's sweep in the next tick is
  tested against (the history advances only at the end of a tick). So the sweep from sample `k` to sample `k + 1`
  meets the target at sample `k`'s tested pose, and a `ProjectileCollision` in that sweep carries tick `first + k`.
  Without a history, or with a lag of 1/30 s or less, nothing is rewound and the sweep meets the target where the
  tick leaves it (targets move before collision): sample `k`'s tested pose is then sample `k + 1`'s current pose
  (the last sample keeps its own).
- **Targets.** Every vehicle in the vehicle set except the shooter's own root that comes within 150 m of the
  projectile during the flight, at most 8 (nearest first); a target that leaves the set keeps its last pose.
  Soldiers are not traced: a seated crew member is rewound with its own history but sits where the rewound hull
  carries it, and a hit on one is in `ProjectileCollision`.
- **Child parts** (rotors, dummy missiles, sensors, gun turrets) are drawn by the viewer at the target's current pose
  from their fixed offsets (the table in `docs/findings/tv-hitbox.md`). The rotor's blade angle is not traced, so the
  viewer draws the rotor as a disc and shows the blade only at a `ProjectileCollision` on it.
- **Candidates.** Vehicles within 1000 m of the launch are sampled from the first sample on, so a target that
  comes close late still has its whole flight. At most 64 flights are traced at once; more are counted in the stats
  line and not traced.
- **End.** Sampling stops 15 ticks after the projectile's first collision with an object or the terrain (so the
  target can be seen moving on), when it vanishes, or at 300 samples (10 s).
- **Size.** 28 + T × 48 bytes per sample: a 3.5 s TV flight with one target is about 8 KB, sent once.
- **Cost on the main thread.** One transform read per candidate per tick while a subscribed projectile flies;
  nothing otherwise.

Golden frames `ev_projectile_collision`, `ev_projectile_trace` and `cmd_subscribe_replay` in
`tests/fixtures/admin-v1-golden.txt`; `adminclient.py` decodes both and its `replay` scenario checks them against a
live server. The bot channel is unchanged. The admin protocol version stays 1 (development-phase rule).

**`Kill`** (for one victim, `Kill` precedes `Death`: the engine raises event 1 before it calls
`killPlayer`, `GameLogicDamage.cpp:484-507`):

| Type | Field |
|---|---|
| `u8` | victim |
| `i16` | attacker, -1 if none |
| `u8` | kind: 1 kill, 2 team kill, 3 suicide (attacker = victim), 4 roadkill (no weapon, an attacker vehicle), 5 no attacker (crash, fall, out of bounds) |
| string | weapon template, empty if none |
| `u8` | victim team |
| `u32` | victim root networkable id |
| string | victim root template |
| `Vec3` | victim root position (the victim's own vehicle, not the fifth script argument) |
| `u8` | attacker team, 0 if none |
| `u32` | attacker vehicle networkable id: the root of the script's fifth argument, else the attacker's root; 0 if none |
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
| `Vec3` | position the engine passed (`docs/findings/module-v2.md`, R5 position caveat) |
| `u8` | flags: bit 0 the victim's hit points reached 0, bit 1 this damage killed a player |

#### Commands (`0x13xx`, WebAdmin → module) and replies (`0x14xx`)

Every command starts with `u32` request id; the reply echoes it. Success is `0x1401 Ok` (`u32`
request id) unless a typed result is listed. Failure is `Error` with the request id.

| Type | Command | Fields after the request id | Reply |
|---|---|---|---|
| `0x1301` | Console | string line (one line, no newline, at most 4096 bytes) | `ConsoleResult`; `Error` 9 with `adminAllowConsole 0` |
| `0x1302` | Say | `u8` target (0 all, 1 team, 2 one player), `u8` team or player id, string text | `Ok` |
| `0x1303` | Teleport | `ObjectRef`, `u8` flags (bit 0 set position, bit 1 set rotation; at least one), `Vec3` position, `Rot` rotation | `Ok` |
| `0x1304` | SetHealth | `ObjectRef`, `u8` flags (bit 0 the value is a fraction of the maximum), `f32` value (hit points) | `Ok` |
| `0x1305` | KillPlayer | `u8` player, `u8` flags (bit 0 also set the player's vehicle's hit points to 0) | `Ok` |
| `0x1306` | SetTeam | `u8` player, `u8` team (1 or 2), `u8` flags (bit 0 kill the player after the switch) | `Ok` |
| `0x1307` | SetScore | `u8` player, `u8` mask (bit 0 score, 1 team score, 2 kills, 3 deaths, 4 team kills), 5 × `i32` values in that order (all present) | `Ok` |
| `0x1308` | Rank | `u8` player, `u8` rank (0-21), `u8` flags (bit 0 send the rank-up event) | `Ok` |
| `0x1309` | Medal | `u8` player, `i32` medal, `i32` value | `Ok` |
| `0x130A` | GameEvent | `u8` player, `u8` event (0-24), `i32` data | `Ok` |
| `0x130B` | HudEvent | `u8` player, `u8` event (0-78), `i32` data | `Ok` |
| `0x130C` | Kick | `u8` player, string reason (shown to the player) | `Ok` |
| `0x130D` | Ban | `u8` player, `u8` by (0 address, 1 CD key), `u8` duration (0 permanent, 1 this round, 2 seconds), `u32` seconds, string reason (logged) | `Ok` |
| `0x130E` | Map | `u8` action (1 restart the round, 2 run the next map, 3 run a map-list index, 4 run a map by name and mode), `u16` index, string name, string mode, `u16` size (0 any) | `Ok` |
| `0x130F` | ListObjects | string template name (exact, without case), `u16` maximum entries (1-4096) | `ObjectList` |
| `0x1310` | SetObjectActive | `ObjectRef` (kind 2 or 3) | `Ok` |
| `0x1311` | SpawnObject | string template name, `Vec3` position, `Rot` rotation, `u8` team | `ObjectSpawned`; `Error` 9 with `adminAllowObjects 0` |
| `0x1312` | DeleteObject | `ObjectRef` (kind 2 or 3) | `Ok`; `Error` 9 with `adminAllowObjects 0` |

**`0x1402 ConsoleResult`**: `u32` request id, `u8` flags (bit 0 output truncated at 256 KiB), `u16`
line count L, L × string. The output of `Console::executeLine` split at `\n`, without the final
empty line.

**`0x1403 ObjectList`**: `u32` request id, `u8` flags (bit 0 more objects matched than were
returned), `u16` N, N × entry:

| Type | Field |
|---|---|
| `u32` | console id |
| `u32` | networkable id, 0 if none |
| string | template name |
| `Vec3` | position |
| `Rot` | rotation (the absolute matrix) |
| `f32` | health 0..1, -1 without armour |
| `u8` | flags: bit 0 the object has a parent |

The walk is `object.listObjectsOfTemplate`'s: every grid object whose template is the named one
(`Scene/Object/ToolObject/ObjectTemplateAdm.cpp:263-277`).

**`0x1404 ObjectSpawned`**: `u32` request id, `u32` console id, `u32` networkable id (0: the object
has no networkable and exists on the server only).

**Keepalive** (either direction):

| Type | Message | Fields |
|---|---|---|
| `0x1501` | Ping | `u32` request id, `u64` the sender's clock, echoed |
| `0x1502` | Pong | `u32` request id, `u64` the echoed clock |

- WebAdmin's `Ping` is answered by the game thread, in order with the commands, so a `Pong` also
  shows the server is ticking.
- The module sends `Ping` after 15 s without receiving anything and closes the link after 45 s
  without receiving anything. WebAdmin answers with `Pong`.

**Error codes** (`0xFFFF Error`: `u32` request id, `u16` code, string message): 1 version mismatch, 2
malformed frame, 3 unknown message type, 4 unknown player, 5 unknown object or unavailable, 6
refused by the engine or by a rule, 7 invalid value, 8 authentication failed, 9 disabled by this
server's `serverext.cfg`.

#### Command semantics

- **Console**: `MainConsole`'s `executeLine(line + "\n", out, false × 5)`, as Python's
  `host.rcon_invoke` does (`Game/Python/PythonHost.cpp:1766-1783`).
- **Say**: `IGameServer::sayAll` (channel 12), `sayTeam(team)` (channel 13), or
  `say(text, channel 14 "Player", type 1, 0, player)` for one player: the engine's own Player channel,
  which only that player's connection receives (`GameServer::adminToPlayerEvent`).
- **Teleport**: the target's root object; `setAbsoluteTransformation` (keeping the current
  position or rotation if only one is set), then the physics node is reset (speeds zero) and woken,
  as the bot channel's `Teleport` does. On foot a soldier's yaw does not stick.
- **SetHealth**: `IArmorGLComp::setHitPoints` on the target's armour, Python's `setDamage`
  (`Game/Python/py/Objects.cpp:706-722`). It bypasses the damage path, so no damage, kill or
  destruction is reported from it. A result of 0 hit points on a soldier or an occupied vehicle is
  `Error` 6: it would abort the server (see "Draft points", 3).
- **KillPlayer**: `ServerGameLogic::killPlayer(player, true, true)` (engine method below; the same
  call a crash makes, so `Death` is raised; no `Kill`). With bit 0 and the player seated, the module
  then sets the emptied vehicle's hit points to 0 (a wreck); with another occupant in it the command
  is `Error` 6 and nothing is done. Unlike `mm_webadmin`'s `damage`, nothing is deleted.
- **SetTeam**: `IPlayer::setTeam(team, false)`, then `KillPlayer` if bit 0. `TeamChange` follows
  from the module's own detection.
- **SetScore**: writes the `PlayerScoreData` fields directly, as Python's score setter does; it does
  not raise script event 28 (`docs/findings/admin-channel-a1-a4.md`, A1 surprise 4).
- **Rank, Medal, GameEvent, HudEvent**: the bodies of Python's `sgl_sendRankEvent` (plus
  `PlayerScoreData::setRank`), `sgl_sendMedalEvent`, `sgl_sendGameLogicEvent` and
  `sgl_sendHudEvent` (`ServerGameLogic.cpp:2540-2662`), through appended engine methods. The 0 values
  `mm_webadmin` skipped are sent.
- **Kick**: `IGameServer::kickPlayer(id, reason 21, text)`. **Ban**: `admin.banPlayer` or
  `admin.banPlayerKey` with `perm`, `round` or the seconds, run by the module (not a raw console
  line from WebAdmin, so it works with `adminAllowConsole 0`). Both refuse AI-flagged players
  (`Error` 6): a bot cannot be kicked or banned (the ban would land on address `N/A`), and bot-channel
  bots are removed through the bot channel. A player that is not connected is `Error` 6.
- **Map**: action 1 is `admin.restartMap` (`IServerExtension::restartRound`); 2 is
  `admin.runNextLevel`; 3 and 4 set the next level to the map-list index (4 looks it up by name,
  mode and size) and run it. A map that is not in the rotation is `Error` 5.
- **SetObjectActive**: runs `object.active idN` with the object's console id, so the console's
  object commands that follow act on it (Freeze, noclip, `.nofences`).
- **SpawnObject**: `ObjectTemplateManager::getTemplate`, then
  `IGame::createObjectOnAllClients(template, position, rotation, true, 0, team)`, the engine's own
  ObjectSpawner path (`ServerGameLogic::spawnObject`, `ServerGameLogic.cpp:947-951`). An unknown
  template is `Error` 5.
- **DeleteObject**: `ObjectManager::destroyObject`, what `object.delete` runs. The module refuses
  (`Error` 6) a soldier, a camera, or a vehicle with a player in it.

### Configuration (`serverext.cfg`)

| Key | Default | Meaning |
|---|---|---|
| `adminConnect` | none (the admin channel is off) | `host:port` of WebAdmin's admin listener. Never port 16567 |
| `adminServerId` | required with `adminConnect` | the server id sent in `Hello`, 1-64 printable bytes |
| `adminKey` | required with `adminConnect` | the shared key, 32-128 hex digits (16-64 bytes) |
| `adminTls` | 1 | 0 sends in the clear and is accepted only for a loopback `adminConnect` |
| `adminCertSha256` | required with `adminTls 1` | SHA-256 of WebAdmin's certificate's SubjectPublicKeyInfo, 64 hex digits |
| `adminSendIdentity` | 1 with TLS, 0 without | send players' IPs and CD-key hashes |
| `adminAllowConsole` | 1 (B5) | allow `Console` |
| `adminAllowObjects` | 1 | allow `SpawnObject` and `DeleteObject` |
| `adminQueueBytes` | 4194304 | outbound queue size beyond which events are dropped (64 KiB to 64 MiB) |

Reconnect backoff: 1 s after a failure, doubled up to 60 s, with ±20% jitter; back to 1 s after a link
authenticates. A bad value refuses the module's load, as for the bot channel's keys.

### Implementation notes that change the outline above

- **CD-key hash:** OpenBF2 doesn't compute one; it is the client's GameSpy key response, present only
  on an internet server with a working cd-key SDK. On LAN and test servers it is empty. `Ban` by key
  with no hash is refused. WebAdmin needs a fallback player key (open question B6).
- **Zero hit points on an occupied object abort the server** (retail `destroyObject` on a soldier with
  a kit). `SetHealth` refuses 0 on a soldier or occupied vehicle; `KillPlayer` with the vehicle bit
  kills first and wrecks the vehicle only when it's empty. Nothing is deleted.
- `Spawn` into a vehicle is `Spawn` then `EnterVehicle`. `ControlPoint` is reported a tick late, with
  the team after the conquest script ran. `VehicleDestroyed` is for vehicles only.
- `Say` to one player uses the engine's Player channel (14).
- No team score exists besides tickets; a player's team score is in `PlayerScore`.
- Round time is -1 until the round runs.
- Module → WebAdmin frames may reach 1 MiB; WebAdmin → module 64 KiB. Optional player ids are `i16`
  -1 (the bot channel uses presence bits).
- `Damage` and projectile events are opt-in streams (volume: 199 events in one short bot fight).
- Spawned objects without a networkable exist on the server only (networkable id 0).
- `SetObjectActive` only helps raw `Console` lines, so it is useless with `adminAllowConsole 0`.
- An overloaded link (WebAdmin not reading) drops events beyond `adminQueueBytes` and closes at four
  times that; WebAdmin then reconnects and gets a snapshot.
- Measured: the admin channel adds 20-40 µs per tick (p99); 64 players at 30 Hz are about 108 KB/s.

### Phase 2 notes

- Map mods, noclip, freeze, gravity and the TV settings still use console lines, because a typed
  `SpawnObject` gives most templates no networkable, so clients never see them. They need
  `adminAllowConsole 1`.
- Kick and ban from the web UI and Discord went through ModManager's `iga` over classic rcon until
  2026-11-09; since then WebAdmin uses the typed `Kick`/`Ban` everywhere and the console for the ban list.
- A changed `AdminKey` takes effect at the module's next retry (up to 60 s).
- BF2WebAdmin's `ServerId` column is `nvarchar(20)`, one character short of the longest `ip:port`, and
  it is now the module's server id.
- The admin key is visible to admin users of the server edit form.
- The bot channel's `Chat` delivered `§` as U+FFFD (fixed in v3).
- Not exercised (needs a human client): Heli2v2 rounds (`rdy`/`go`), player chat, in-game `.o`, revive,
  control points, a real kick or ban. The CD-key hash needs an internet server.

## Decisions

| Id | Question | Decision (2026-10-22) |
|---|---|---|
| B1 | TLS | Needed: servers reach WebAdmin over the internet. TLS 1.3 in the module with OpenSSL and a pinned self-signed WebAdmin certificate, in phase 1 |
| B2 | Keep the `mm_webadmin` backend | No. It is replaced; the author runs an old WebAdmin image meanwhile |
| B3 | Plain snapshots or deltas | Plain, at a per-stream rate. Deltas can come later without changing events or commands (below) |
| B4 | Bot management from WebAdmin | Maybe, later. The channel must not rule it out; no messages yet |
| B5 | Raw console by default | Yes, with a per-server switch to turn it off |
| B6 | Player key when the CD-key hash is empty (LAN and test servers, and possibly BF2Hub on OpenBF2) | Decided 2026-10-23: the hash when present; otherwise the player name, stored as a distinct name key and accepted only on servers marked as test servers |

**Why plain streams (B3).** CPU is not the cost: diffing 64 players against the last frame takes
microseconds on the I/O thread. The cost is correctness on both sides: a baseline per subscriber,
field masks, entities appearing and disappearing, a full frame after every gap, and the same logic
again in the C# codec and the golden tests. Field deltas also save less than they seem, because every
moving player's position and rotation changes every frame. A full 64-player frame is about 2.7 KB, so
30 Hz is about 80 KB/s, and a 2v2 server with 4 players is about 6 KB/s. If that becomes a problem,
the cheap step is to skip entities unchanged since the last frame, with a full frame every second; it
is a new stream encoding, not a protocol redesign.
