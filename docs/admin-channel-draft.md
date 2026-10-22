# Admin channel: proposal (draft 2)

Status: **draft, not approved.** It replaces draft 1, which kept `mm_webadmin.py`'s text protocol.
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

**Encryption**
- Player IPs and CD-key hashes are personal data. Either:
  - use TLS 1.3 with a pinned WebAdmin certificate, or
  - accept HMAC alone on a private network or WireGuard link that already protects the traffic (B1).
- Until that's settled, IPs and hashes are sent only with `adminSendIdentity 1`.

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
| Player | id, name, team, squad, bot flag, profile id, IP and CD-key hash (only with `adminSendIdentity`), ping, score, kills, deaths, team kills, alive or man-down or dead, vehicle id and seat |
| Vehicle | networkable id, template, team, position, rotation, velocity, health, destroyed, occupied seats |
| Control point | id, template, team, capture state |
| Projectile | id, template, owner player, position, velocity (only templates WebAdmin subscribes to, e.g. TV missiles) |
| Game | state (all six, including Paused), tickets for both teams, round time |

**`Subscribe(stream, rateHz, filter)`**
- The streams are player states, vehicles, projectiles (filtered by template) and tickets.
- Each is a plain snapshot of the subscribed entities at the chosen rate (B3).

### Events (sequence-numbered)

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
| `Teleport` | a player's root vehicle or any object id: position and rotation |
| `SetHealth`, `KillPlayer` | |
| `SetTeam` | optionally killing the player |
| `SetScore` | score, kills, deaths |
| `Rank`, `Medal` | |
| `GameEvent`, `HudEvent` | including the 0 values today's Python skips |
| `Kick`, `Ban` | For a bot these map to `RemoveBot`, or are refused, never to the address `N/A` ban |
| `Map` | restart, next, or set map and mode |
| `ListObjects` | template filter; returns ids, templates, positions, rotations and health. Replaces `listObjectsOfTemplate` parsing |
| `SpawnObject`, `DeleteObject` | template, position, rotation; for map modules. Can be turned off |
| `Subscribe`, `Unsubscribe` | streams |
| `SetPolicy`, `ClearPolicy` | phase 3 |

### Policies (phase 3)

| Policy | Effect |
|---|---|
| Damage | zones with no damage (base protection), multipliers per weapon or vehicle, friendly-fire rules |
| Team | refuse a team switch that would stack teams |
| Spawn | per team, group or vehicle |

The module reports every action it takes as an event. Policies need engine veto hooks (R6), each
designed and approved on its own.

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
- **Game modules change where they relied on the text protocol.** Heli2v2's `listObjectsOfTemplate`
  parsing becomes `ListObjects`. `RootVehicleId` becomes the networkable id. Rcon `\b` parsing and
  ordered raw console strings become typed commands where one exists.
- **The `mm_webadmin` backend stays** for retail and BF2Hub servers, unless the author retires
  them (B2).

## Phases

1. **Engine and module.**
   - R4, R5 (shared with bot v2) and the getters.
   - The admin channel's session, authentication, snapshot, events, streams and the core commands.
   - A Python reference client with a fuzz mode, as for the bot channel.
   - Verified on the WSL and Docker test servers.
2. **BF2WebAdmin.**
   - The C# codec and backend, and the module refactors.
   - Tested end to end with the netsky 2v2 setup. That needs the BF2WebAdmin local test
     environment recreated, which waits for the author's go-ahead.
3. **Policies and veto hooks.**
4. **TLS,** if B1 chooses it.

## Open questions

| Id | Question | Recommendation |
|---|---|---|
| B1 | Is TLS needed in phase 1, or do the deployments already run over a private network? | TLS if any game server reaches WebAdmin over the internet |
| B2 | Keep the `mm_webadmin` backend for retail and BF2Hub servers? | Keep it until every server runs OpenBF2 |
| B3 | Stream encoding: plain snapshots at the chosen rate, or deltas? | Plain. 64 players at 30 Hz is about 80 KB/s; add deltas only if that is too much |
| B4 | Should WebAdmin create and remove bots (for example an in-game `!bot` command), through the bot channel's sidecar? | Later, after the bot work settles |
| B5 | Is raw console allowed by default? | Yes, but it can be turned off per server |
