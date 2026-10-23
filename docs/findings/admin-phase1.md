# Admin channel, phase 1: byte layouts, engine changes and results

Status: done on 2026-10-23 by a worker thread; committed locally in both repos (OpenBF2 `873d1b1`). The spec is `docs/admin-channel-draft.md`
(draft 2, approved 2026-10-22 with decisions B1-B5). The byte layouts below were written before the
module code, as the brief requires. `docs/admin-channel-draft.md` and `docs/protocol.md` were not
edited. Paths without a repo prefix are OpenBF2 paths under `Code/BF2/` in the `serverext-r1`
worktree.

## Byte layouts (admin protocol version 1)

### Framing and conventions

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

### Sequence numbers and resync

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

### Session (`0x10xx`)

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

### State (`0x11xx`)

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
| `u8` | stream: 1 players, 2 vehicles, 3 projectiles, 4 game; event streams 16 `Damage`, 17 `ProjectileFired`/`ProjectileHit` |
| `u8` | rate, Hz, 1..30 (ignored for 16 and 17) |
| `u8` | filters F |
| F × string | template names, compared without case. Streams 3 and 17 need at least one; the others take none |

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

### Events (`0x12xx`, module → WebAdmin)

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

### Commands (`0x13xx`, WebAdmin → module) and replies (`0x14xx`)

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

### Command semantics

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

## Configuration (`serverext.cfg`)

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

## Engine changes (OpenBF2 `serverext/r1`)

All `[CHOSEN, not recovered]`, citing `docs/admin-channel-draft.md` (the approved design record; there
is no GitHub issue). `IServerExtension` stays append-only and a separate adapter object, so no retail
vtable or layout changes. No IDA/Ghidra session was available to this worker. None of the changes
recovers or alters a retail body: R4 is one added call at the top of a recovered function, and the
appended methods run what the matching console command or Python wrapper runs, read from the
recovered source.

| Addition | Where | Why (no typed path from the module) |
|---|---|---|
| R4: `ScriptEventArg`, `IScriptEventListener`, `setScriptEventListener` | `Game/GameServer/IServerExtension.h`; the hook and decoder in `Game/Python/PythonHost.cpp` (`notifyScriptEventListener`, called first in `PythonHost::handleEvent` after its two early returns); the slot `serverExtensionHooks::scriptEventListener()` in `ServerExtensionHooks.h`, defined in `PythonHost.cpp` | Script events exist as typed values only at their 51 call sites; the dispatcher sees Python tuples |
| `getServerInfo` (`sv.serverName`, ports, max players, time limit, rounds, internet, ranked) | `GameServer.cpp` adapter | `serverSettings` is not a registered singleton; `IGameServer::getName` is the network listener's name and is empty on this build |
| `getRoundInfo` (tickets, winner, victory type, round time, rounds played) | adapter; two inline read accessors added to `ServerGameLogic.h` (`getServerExtRoundStartTick`, `getServerExtRoundsPlayed`) | `gameLogic` and `scoreManager` are not reachable; the round-start tick is a protected field |
| `getTeamName` | adapter | `IGameLogic::getTeamName`, `gameLogic` not reachable |
| `getCdKeyHash` | adapter | `gcd_getkeyhash` is an `extern "C"` function inside the executable |
| `getDisconnectReason` | adapter | `GameServer::m_disconnectTypes` is private; it still holds the reason while the network pass destroys the player |
| `getMapList`, `runMapListEntry` | adapter | `mapList` and `ServerGameLogic::setNextLevel/runNextLevel` are not reachable |
| `sendGameLogicEvent`, `sendHudEvent`, `sendMedalEvent`, `setRank` | adapter | they construct engine `GameEvent` subclasses (`PostRemoteEvent`, `MedalEvent`, `RankEvent`) and call the non-virtual `PlayerScoreData::setRank` |
| `killPlayer` | adapter | `IGameLogic::killPlayer` on the unreachable `gameLogic` |

Everything else the commands need has a typed path and no engine change: the console
(`MainConsole` singleton, `Console::executeLine`), chat (`IGameServer::sayAll/sayTeam/say` with the
engine's channel 14 for one player), kick (`IGameServer::kickPlayer`), objects
(`ObjectTemplateManager::getTemplate`, `IGame::createObjectOnAllClients`,
`ObjectManager::getAllObjectsFromGrid/getObjectFromGrid/destroyObject`), armour, transforms and
physics (`IArmorGLComp`, `IObject`, `IPhysicsNode`), score fields (`PlayerScoreData`, same build) and
team (`IPlayer::setTeam`). Ban runs `admin.banPlayer`/`admin.banPlayerKey` through the console,
because `BanList` is not reachable.

### R4 in detail

- **Site.** `PythonHost::handleEvent(ScriptEvent, PyObject*)`, after `if (!m_initialized) return;` and
  `if (m_byte01) return;`, before the handler loop. With no listener the cost is one load and branch
  per script event.
- **Decoder.** It reads the argument tuple without changing it: ints (Python bools are ints), floats,
  strings (borrowed bytes; Python 2.3's `PyString_AsStringAndSize` with an `int` length), tuples
  (flattened into the same array, up to three levels and 64 entries; the kill's assists are a tuple of
  `(player, score)` tuples), object wrappers (the PyCObject `token` attribute, resolved through
  `tokenControlBlock` to the weak pointer's target, null when the object is gone) and player wrappers
  (an int `index` attribute, resolved with `playerManager->getPlayerFromId`). Each attribute lookup's
  reference is released (unlike `parse_arg`'s shipped leak) and every Python error it causes is
  cleared.
- **Not decoded:** anything else (None). Nothing is delivered while Python is not initialised or under
  the `m_byte01` mute, as for Python itself.
- **Contract for the listener:** read only, no Python, console, object creation or deletion, damage
  or kills inside the callback (documented in `IServerExtension.h`).

## CD-key hash (WebAdmin keys on it)

**OpenBF2 does not compute a CD-key hash itself, and on a LAN server there is none.** The hash is the
client's: `gcd_authenticate_user` stores the first 32 characters of the client's CD-key response as
`client->hkey` (`External/GameSpy/gcdkey/gcdkeys.c:216-275`) and `gcd_getkeyhash(g_gameID, id)` returns
it (`:396-414`). Both `admin.listPlayers` (`GameServer.cpp:2903`) and `admin.banPlayerKey`
(`BanList.cpp:337`) read it that way, and so does the new `getCdKeyHash`.

- `gcd_authenticate_user` runs only on the internet branch: `GameSpyCDKeyAuthenticator::connectPlayer`
  calls it when `m_internetServer` is set, and on a LAN server (`sv.internet 0`) authorises the client
  directly (`Game/MatchMaking/Server/MatchMakingServer.cpp:87-98`).
- It also needs the GameSpy cd-key SDK initialised: `gcd_init_qr2` (`GameSpyBackend.cpp:205-207`)
  returns early unless the GameSpy availability check succeeded (`gcdkeys.c:172-176`,
  `__GSIACResult == GSIACAvailable`). Production servers reach a GameSpy replacement (BF2Hub); whether
  the availability check and the key exchange succeed there on OpenBF2 is **unverified** (no client
  joined; the test servers run `sv.internet 0`).
- With no product registered, `gcd_getkeyhash` returns `""` (`gassert` is compiled out without
  `GUSE_ASSERTS`), so the call is safe on a LAN server.
- **Consequences, implemented:** `PlayerInfo.hash` is empty for bots, for every player on a LAN server
  and whenever the SDK holds none. `Ban` by CD key is refused (`Error` 6) when the hash is empty,
  because `admin.banPlayerKey` would otherwise ban the empty key, and `challengeResponse` compares
  every joining client's hash against the key list (`GameServer.cpp:2948-2950`), so it would refuse
  every keyless player.
- **For phase 2:** WebAdmin cannot key players on the hash on a LAN or test server. It needs a fallback
  key (profile id or name) or an internet test server with a real client to verify the hash first.

## Module design

Files: `src/AdminProtocol.h/.cpp` (codec, text conversion), `src/AdminCrypto.h/.cpp` (HMAC-SHA256,
random, SHA-256 via libcrypto), `src/AdminLink.h/.cpp` (the main/I-O queue), `src/AdminSession.h/.cpp`
(handshake and parsing on the I/O thread), `src/AdminIo.h`, `src/AdminIo_posix.cpp` (dial-out, backoff,
TLS), `src/Admin.h/.cpp` (engine side), `src/Rotation.h`, `src/EngineUtil.h` (helpers moved out of
`Module.cpp` so both channels share them). Changed: `src/Module.cpp` (hosts the admin channel and
notifies it of bot-channel bots), `src/Config.h/.cpp` (admin keys), `src/Protocol.h/.cpp`
(`i16`/`i32`/`u64`/`f64`/bytes and a frame limit per decoder), `CMakeLists.txt` (OpenSSL),
`tests/AdminTests.cpp`, `tests/fixtures/admin-v1-golden.txt`, `tools/adminclient/adminclient.py`,
`README.md`.

- **Threads.** The admin channel has its own I/O thread, not the bot channel's, so a blocking name
  lookup (`getaddrinfo`) or a TLS handshake can never delay a sidecar. It never calls the engine. The
  game thread only reads engine state and queues. The per-tick work runs from the bot channel's tick
  handler (`Module::handleEvent`, after the bot work), so both channels share one EventManager
  category-7 event.
- **TLS.** OpenSSL with memory BIOs: the I/O thread owns the non-blocking socket, feeds received bytes
  to the read BIO and sends the write BIO's bytes with `send(MSG_NOSIGNAL)`, so a closed peer can never
  raise SIGPIPE in the server. TLS 1.3 only (minimum and maximum), `SSL_VERIFY_PEER` with a callback
  that pins the leaf's SPKI (`i2d_X509_PUBKEY`, SHA-256) and ignores the CA chain. A mismatch fails the
  handshake before the module sends anything. The code builds against OpenSSL 1.1.1 and 3.x (Debian 11
  ships 1.1.1; only 3.0.2 in WSL was built and run).
- **Backoff.** 1 s after an authenticated link closes. After a failed attempt (refused, TLS, wrong
  proof, timeout) the delay doubles from 1 s up to 60 s; each delay has ±20% jitter.
- **Ordering.** Replies, snapshots and events share one ordered queue, so a `Snapshot` and the events
  around it are never reordered. Commands run at most 64 per tick.
- **Tracking.** Per player the module keeps the team and score fields it last reported (for
  `TeamChange` and `ScoreChange`) and the kick and ban notices it saw (for the disconnect reason).
  Control points come from one walk of the registered objects after each level load, repeated every
  10 s while none is found. Projectiles of subscribed templates are tracked from the launch observer,
  as the bot channel does, at most 256.
- **Load line.** `admin channel: protocol v1, dial 127.0.0.1:16710, server id '127.0.0.1:16573', TLS 1.3
  (pinned), identity 1, console 1, objects 1, queue 4194304 bytes; R4 1, status hook 1, unload hook 1,
  damage 1, projectile 1`. A missing singleton, a held R4 slot or an I/O start failure refuses the
  module's load before the bot channel listens.
- **Stats.** Once a minute the module logs the admin session, events sent and dropped, commands, bytes
  per second per category (events, replies, each stream) and the admin channel's own per-tick cost.

## Draft points that proved wrong, ambiguous or dangerous

`docs/admin-channel-draft.md` was not edited. Each item names the reading implemented.

1. **CD-key hash.** It does not exist on a LAN server and is the client's own hash, not OpenBF2's (see
   the CD-key section). WebAdmin needs a fallback player key for test servers.
2. **Rotation convention.** No conflict: the bot channel's yaw, pitch and roll equal Python's
   `getRotation`, so the inventory's rotation gap needs no conversion.
3. **Zero hit points on an occupied object abort the server.** `IArmorGLComp::setHitPoints(0)` on a
   vehicle with a player in it makes the armour evict and destroy the occupant's soldier, and
   `ObjectManager::destroyObject` deliberately aborts on a soldier that still has its kit
   (`Scene/Object/ObjectManager.cpp:1853-1854`). Found at runtime under gdb with the first `KillPlayer`
   bit 0, which zeroed the vehicle before the kill (`ArmorGLComp::updateStatus` → message 24 →
   `PlayerControlObject::handleMessage` → `Game::destroyObject` → abort). Python's `setDamage(0)` reaches
   the same path; `mm_webadmin` used 1. Implemented: `SetHealth` refuses a result of 0 hit points on a
   soldier or an occupied vehicle (`Error` 6), and `KillPlayer` bit 0 kills the player first and zeroes
   the vehicle only when it is then empty (another occupant: `Error` 6, nothing done). The vehicle
   becomes a wreck (`VehicleStream` health 0, destroyed flag); no `VehicleDestroyed` follows, because
   `setHitPoints` bypasses the damage path.
4. **`VehicleDestroyed` for soldiers.** Script event 24 fires for every dying player control object,
   soldiers included. The module reports vehicles only.
5. **Control point team at the event.** Event 25 fires before the conquest script sets the new team,
   so `ControlPoint` is reported at the module's next tick with the team after the script ran.
6. **`Spawn` into a vehicle.** Script event 23 fires for every spawn, because `spawnPlayer` enters the
   soldier first; a spawn into a vehicle is `Spawn` then `EnterVehicle`. Event 23 also fires when a
   living player returns to its soldier; the module filters that with the alive flag.
7. **Disconnect reason.** The script event carries none. The engine keeps it in `m_disconnectTypes`
   while the network pass destroys the player, so a getter was appended. Result 13 ("refused or
   dropped") is reported as "left" (`[INFERRED]`: a client that quits is dropped with 13; not exercised,
   no client). Bots removed by the bot channel have no reason (0, unknown).
8. **Kick and ban of bots** are refused (`Error` 6), as the draft allows. A console `admin.kickPlayer`
   on a bot still raises the engine's kick notice (a `Kick` event) with no disconnect, because a bot has
   no connection.
9. **Ban by CD key with no hash** would ban the empty key and lock out every keyless player; refused.
10. **`Say` to one player** uses the engine's Player channel (14), which the draft did not name. It
    comes back as `Chat` channel 14 with the player id in the flags.
11. **Team score** ("tickets and score for both teams"): the engine has no team score besides tickets.
    `GameState` carries tickets, winner and victory type; a player's team score (`rplScore`) is in
    `PlayerScore`.
12. **Round time** is -1 until the round reaches Running; with bots only it never does (protocol.md,
    "How these bots differ").
13. **Frame size.** Module → WebAdmin frames may reach 1 MiB (`Snapshot`, `ObjectList`,
    `ConsoleResult`); WebAdmin → module keeps the bot channel's 64 KiB.
14. **Optional player ids** are `i16` with -1, not the bot channel's presence bits.
15. **Identity without TLS.** The draft says IPs and hashes are sent by default "with TLS". The default
    is 1 with TLS and 0 without; `adminSendIdentity` overrides it.
16. **Damage volume.** `Damage` is opt-in (event stream 16): every hit is one event (199 in a short bot
    fight). So are `ProjectileFired` and `ProjectileHit` (stream 17, template filter).
17. **Spawned objects without a networkable.** `createObjectOnAllClients` returns before telling any
    client when the object has no networkable; `ObjectSpawned` then has networkable id 0 and the object
    exists on the server only. Heli templates have one (`ahe_ah1z` spawned as net ids 2065-2069).
18. **`SetObjectActive` needs the console.** It sets the console's active object for the raw `Console`
    lines that follow, so with `adminAllowConsole 0` it is of no use. It is not gated itself.
19. **Capabilities.** Implemented switches: `adminAllowConsole` (default 1, B5) and `adminAllowObjects`
    (default 1, since the raw console can spawn and delete anyway). Policies are phase 3 and have no
    switch yet.
20. **An overloaded link is closed, not resynchronised.** Event drops start at `adminQueueBytes` and the
    link closes at four times that. With the kernel's loopback socket buffers absorbing several MB, the
    stall test reached the close (88 events dropped first) before WebAdmin could read a gap; the
    in-session gap path was exercised by dropping an event in the client.

## Results

### Setup

| Item | Value |
|---|---|
| OpenBF2 | worktree `serverext-r1`, branch `serverext/r1`, `74e18d9` plus this change, committed as `873d1b1`; built into `~/bf2-linux-serverext-admin` (`testenv/build-linux.sh`), `bf2` md5 `06b42122496f2ffbdcf54bc557d3e33b` |
| Module | `~/serverext-build/admin`; final `libserverext.so` md5 `97842422cb60efd74d17c758040f5a3e` (OpenSSL 3.0.2, WSL Ubuntu 22.04) |
| Content and run dir | netsky `2v2-nocheck` via `prepare-content.sh` into `~/serverext-admin/srv`; `mm_webadmin` not loaded; VoIP ports moved to 55323-55325 so a second WSL server does not collide |
| Server | WSL, `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`, `dalian_2_v_2` `gpm_cq` 16 (one run switched to `daqing_2_v_2`); game 16573/udp, GameSpy 29905/udp, RCON 127.0.0.1:4725, bot channel 127.0.0.1:16703, admin dial-out to 127.0.0.1:16710; `allowTeleport 1`, `allowRoundRestart 1`, `sv.internet 0` |
| WebAdmin | `tools/adminclient/adminclient.py` in WSL (Python 3.10.6), TLS 1.3 with a self-signed P-256 certificate, `adminCertSha256` its pin; logs `~/serverext-admin/logs/*.log` |
| Bots | `tools/testclient/serverext_client.py` (`--scenario damage --crash` and `soak2`) on the bot channel |

Ports 16567, 16569, 16571, 16700, 16701, 29901, 29903, 4300, 4721 and 4723 were not used; no
container was created or touched; the BF2WebAdmin localtest was not started. Ten server processes
ran (one per rebuild or configuration), each stopped with SIGTERM, except the one that aborted
(below).

### Unit tests

`serverext_admin_tests`: 12,823 checks, 0 failures; the bot channel's `serverext_tests` still pass
(621,268 checks). Covered: text conversion both ways (every 1252 byte, invalid UTF-8 forms), HMAC
(RFC 4231 case 2), SHA-256, the proof transcript, rotation round trips (2,000 random attitudes) and
conventions, every structure, every module message and every event field by field, every WebAdmin
message parsed exactly, short by every byte and one byte long, 36 bad-value cases (`Error` 7 without
closing), the handshake (right key; wrong key → `Error` 8 and no `Auth`; commands before `Challenge`;
unknown type; WebAdmin's `Error`; a second `Challenge`; oversize; nonzero reserved; byte-by-byte
input), 3,000 fuzz sessions, the link queue (order, latest-wins slots, event drops beyond the size,
overload at four times, bounded commands), the configuration keys and their dependencies, the golden
frames, and the I/O thread against a local OpenSSL TLS 1.3 server: right pin (authenticated), **wrong
pin (handshake aborted, no Hello, peer pin logged)**, wrong key over TLS (refused), plain loopback and
reconnect after the server restarts.

`tests/fixtures/admin-v1-golden.txt` (60 frames) is written by `adminclient.py --write-golden`; the
C++ encoder reproduces all 38 module messages byte for byte and parses the 22 WebAdmin messages to
the sample values. The C# codec can share the same file.

### Runtime

| Item | Result | Log |
|---|---|---|
| TLS handshake | **Pass**: `admin: TLS up (TLSv1.3, TLS_AES_256_GCM_SHA384), certificate pin matches`; Hello → Challenge → Auth → Welcome (1,012 B, 34 map-list entries, teams CH/US) → Snapshot | `listen1`, every run |
| Wrong pin | **Pass 2/2**: the module aborts TLS (`tlsv1 alert unknown ca` at WebAdmin) and logs `WebAdmin's certificate does not match adminCertSha256 (peer SPKI sha256 31ddde8d...)`; nothing sent | `wrongpin1` |
| Wrong key | **Pass 2/2**: `Error 8 WebAdmin's proof does not match adminKey` three times, backoff 16.8 s then 29.4 s | `wrongkey1` |
| Snapshot | **Pass**: players, 4 vehicles (pooled entries at 0/0/0 with health -1 during load, as the bot channel reports), 2 control points; `RequestSnapshot` with its request id | `listen1`, `commands2` |
| Events | **Pass** for GameState, RoundStart, RoundEnd, PlayerConnect, PlayerDisconnect, Spawn, Death, TeamChange, ScoreChange, Kill (rockets; the crash as kind 5), Damage (199 in the damage scenario), EnterVehicle, ExitVehicle, VehicleDestroyed, Chat (channels 12, 13, 14, with `§` and `é` intact), ProjectileFired, ProjectileHit (with the damaged victim's id), Kick, Bot (created, removed, team change). **Not exercised:** Revive (needs a medic), ControlPoint (no capture without humans: the round never runs), Ban (the engine bans connected humans only), human chat channels 0 and 1, a real disconnect reason (bots have none) | `events1`, `commands2`, `extras3`, `map1` |
| Commands | **Pass 41/41** plus **7/7** extras: Ping, RequestSnapshot, Console (`sv.serverName`, `maplist.list`; a newline is `Error` 7), Say (all, team, one player, unknown player `Error` 4), Teleport (position +50 m within 0.2 m; rotation by networkable id; out of world `Error` 7; unknown console id `Error` 5), SetHealth (0.5 on an empty heli; 0 on an occupied one `Error` 6), SetScore → ScoreChange, Rank → ScoreChange rank 4, Medal, GameEvent 0, HudEvent 0, Kick and Ban of a bot `Error` 6, ListObjects (console id equals the vehicle stream's), SetObjectActive then `object.templateName`, SpawnObject (`ahe_ah1z` at the requested place and yaw 90.0) and DeleteObject, deleting a player's soldier `Error` 6, SetTeam with kill → TeamChange and Bot events, KillPlayer → Death, on a dead player `Error` 6, KillPlayer with vehicle destroy (wreck, health 0), Map to a map not in the rotation `Error` 5, Map next, Subscribe and Unsubscribe | `commands2`, `extras3`, `map1` |
| Sequence gap | **Pass 5/5**: a lost event (dropped in the client) is a gap; `RequestSnapshot` brings `lastSeq` 2 and the next event continues | `resync1` |
| Dropped link | **Pass**: WebAdmin closed the link mid-stream; the module redialled after 1.2 s; session 2 → 3, Snapshot `lastSeq` 0, events numbered from 1 | `resync1` |
| Overloaded link | **Pass**: with `adminQueueBytes 65536` and a stalled reader, the module dropped 88 events, then closed at four times the size (`WebAdmin is not reading`) and redialled in 1.0 s; the server kept ticking | `stall2` |
| Fuzz | **Pass 24/24** rounds: each malformed frame ended the link with the right `Error` (2 oversize, 2 reserved, 3 unknown type, 2 second Challenge, 2 malformed), the module redialled every time, and a clean link answered Ping afterwards | `fuzz1` |
| Reconnect after the listener restarts | **Pass**: between scenarios the module retried with growing delays (`connect: Connection refused`, 4.6 s, 7.6 s ... 60 s ±20%) and authenticated as soon as a listener was back | server log |
| Map change | **Pass 4/4**: Map next → RoundEnd, GameState 1 → 2 → 3 → 1, RoundStart on `daqing_2_v_2` 25 s later; the link stayed up; Snapshot after the change has 38 vehicles | `map1` |
| 10-minute soak | **Pass** (below) | `soak1` |

### 10-minute soak

Two bots hovering (`soak2`, spotting and firing rockets), one admin link subscribed to players at
30 Hz, vehicles at 5 Hz, projectiles at 10 Hz (three rocket and missile templates), game at 1 Hz,
damage events and projectile events; a WebAdmin Ping every 25 s.

| Check | Result |
|---|---|
| Link | up for 601 s, 0 sequence gaps, 24/24 Pings answered by the game thread |
| Player stream | 18,029 frames, 30.0 Hz |
| Events | 48 (spawns, entries, damage, projectile launches and hits, bots), 0 dropped |
| Per-tick cost, whole handler (bot and admin channel), soak windows of 1,800 ticks | p50 100 µs, p99 140-170 µs, max 388-449 µs; run max 1,095 µs (the level load); 0 ticks over 20 ms |
| Admin channel's own per-tick cost | p50 20 µs, p99 30-40 µs, max 47-320 µs |
| Bandwidth with 2 players and 4 vehicles | players 30 Hz 3.5-4.0 KB/s; vehicles 5 Hz 1.9-2.0 KB/s; projectile stream 10 Hz 0.23 KB/s (empty frames); game 1 Hz 44 B/s; events 3-20 B/s |
| Server | alive at the end, RSS 148 MB; 0 `[FRONTIER]`, 0 abort, 0 segfault in the soak process's log |

The bot channel alone measured p50 100-110 µs and p99 160-180 µs in v2's soak; the admin channel adds
about 20-40 µs, well under the 1 ms budget. The 64-player estimate in the draft (about 2.7 KB a frame)
matches the encoding: 13 + 56 bytes per player, so 64 players at 30 Hz are about 108 KB/s.

### The one abort

The first `KillPlayer` with bit 0 aborted the server (no log line; reproduced under gdb, backtrace in
"Draft points", 3). It was the module's command, not an engine regression: zeroing an occupied
vehicle's hit points. Fixed in the module before the final runs (`extras3` passed the same case);
every later server log has 0 abort.

## What is left

- **Client paths:** Revive, ControlPoint, human chat (0, 1), Ban, a real kick and its disconnect
  reason, timed-out and left reasons, and the CD-key hash all need one or more game clients, and the
  hash needs an internet server.
- **Docker:** not run for the admin channel. The Debian 11 image has OpenSSL 1.1.1; the code is
  written for 1.1.1 and 3.x but was built and run only against 3.0.2. The image needs `libssl1.1`
  (present in Debian 11's base) and the bullseye build needs `libssl-dev`.
- **Phase 2:** the C# codec (the golden file is ready), the listener with .NET's `SslStream` TLS 1.3,
  and a fallback player key while the CD-key hash is unverified.

## Remaining frontiers relevant to this change

- No `[FRONTIER]` line was printed in any run.
- R4 adds no frontier. The appended methods call only recovered bodies; `PlayerScoreData::setRank`
  (no longer a frontier, `PlayerScoreData.cpp:66`) and `ServerGameLogic::runNextLevel` were reached by
  `Rank` and `Map`.
- `ObjectManager::destroyObject`'s shipped abort on a soldier with a kit is retail behaviour and stays;
  the module avoids it.
