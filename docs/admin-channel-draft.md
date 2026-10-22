# Admin channel: proposal (draft 2)

Status: **approved by the author on 2026-10-22**, with the decisions in [Decisions](#decisions). Byte
layouts are written in phase 1 and reviewed before the C# side starts. It replaces draft 1, which kept
`mm_webadmin.py`'s text protocol.
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
3. **Policies and veto hooks.**

TLS is part of phase 1 (B1).

## Decisions

| Id | Question | Decision (2026-10-22) |
|---|---|---|
| B1 | TLS | Needed: servers reach WebAdmin over the internet. TLS 1.3 in the module with OpenSSL and a pinned self-signed WebAdmin certificate, in phase 1 |
| B2 | Keep the `mm_webadmin` backend | No. It is replaced; the author runs an old WebAdmin image meanwhile |
| B3 | Plain snapshots or deltas | Plain, at a per-stream rate. Deltas can come later without changing events or commands (below) |
| B4 | Bot management from WebAdmin | Maybe, later. The channel must not rule it out; no messages yet |
| B5 | Raw console by default | Yes, with a per-server switch to turn it off |

**Why plain streams (B3).** CPU is not the cost: diffing 64 players against the last frame takes
microseconds on the I/O thread. The cost is correctness on both sides: a baseline per subscriber,
field masks, entities appearing and disappearing, a full frame after every gap, and the same logic
again in the C# codec and the golden tests. Field deltas also save less than they seem, because every
moving player's position and rotation changes every frame. A full 64-player frame is about 2.7 KB, so
30 Hz is about 80 KB/s, and a 2v2 server with 4 players is about 6 KB/s. If that becomes a problem,
the cheap step is to skip entities unchanged since the last frame, with a full frame every second; it
is a new stream encoding, not a protocol redesign.
