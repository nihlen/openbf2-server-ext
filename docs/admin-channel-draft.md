# Admin channel: draft for the author

Status: **draft, not approved.** Nothing implements it. When it is approved, it moves into
`protocol.md` as its own section. Until then, no worker builds against it.

## Goal

Carry what BF2WebAdmin gets today from `mm_webadmin.py`, natively, through the module. BF2WebAdmin
gets one new backend that maps the native messages onto its existing `GameReader`/`GameWriter`
events, so its game modules (`Heli2v2Module` and the rest) do not change. `mm_webadmin.py` stays
supported for retail and BF2Hub servers. The native channel is for OpenBF2 servers only.

## What exists today

Inventory of `mm_webadmin.py` v0.5 and the C# side. The deployed netsky copies are byte-identical
to the repo's `examples/docker-compose/bf2/server-1/server/admin/modules/mm_webadmin.py`.

- **Transport:** the game server dials out over TCP to WebAdmin on port 4300. Messages are
  tab-separated lines in Windows-1252; commands are space-separated. WebAdmin re-triggers the
  connection through ModManager rcon (`wa connect`/`wa connectprivate`).
- **24 events:** game state, control points, player connect, spawn, team, score, revive, kill and
  death, disconnect, vehicle enter, exit and destroy, chat, rcon responses, and a position stream
  at 4 Hz with TV-missile positions.
- **13 commands:** rcon, rconresponse, pm, position, rotation, damage, rank, medal, gameevent,
  hudevent, score, team, timerinterval. Kick, ban, chat and map changes all go through `rcon`.

Problems the native channel should fix:

1. **No authentication.** Any private IP is accepted, and a connected peer can run any console
   command. Player IPs and CD-key hashes travel in clear text.
2. **Blocking work on the game thread:**
   - connect, with a 5 s timeout, and `gethostbyname`;
   - `findId()`, which calls `object.listObjectsOfTemplate` plus two rcon calls per match, and
     runs on every vehicle entry and every missile update.
3. **No backpressure.** Every queue on both sides is unbounded.
4. **Bugs:** `medal` always fails; roadkills are never reported; `gameevent`/`hudevent` with a 0
   argument are skipped; exit and destroy send placeholder ids; and a partial line over 128 bytes
   drops the connection.

## Proposed design

**Connection**
- The module dials out to `adminConnect host:port` (`serverext.cfg`), as today. That keeps the
  central WebAdmin behind its existing firewall rules.
- Connect, DNS and reconnect with backoff all run on the I/O thread. The main thread never blocks.
- No rcon re-trigger is needed: the module reconnects by itself. After each connect it replays the
  current state (game status, players, vehicles), as `sendReconnectMessage` does today.

**Authentication**
- A pre-shared key in `serverext.cfg` (`adminKey`), never on the wire.
- WebAdmin sends a random challenge, and the module answers with HMAC-SHA256(key, challenge ‖
  server id). Nothing else is accepted before that.
- TLS is optional and later. Without it, the stream is still readable on the network, so player
  IPs and CD-key hashes are sent only when the setting `adminSendIdentity 1` is on.

**Wire format**
- The same framing as the bot channel: a binary header and a length limit.
- Message types are in their own range (0x1000 and up), with their own version number.
- UTF-8 strings. Positions are `f32`, not `"%.1f"` strings.
- Bounded queues: if WebAdmin falls behind, position frames are dropped (latest wins), and events
  are kept up to a limit that, when reached, closes the connection with a reason.

**Events**
- The same 24 as today, with real ids where today's messages send placeholders, and the roadkill
  case fixed.
- Positions come from the module's own per-tick snapshot, at a rate WebAdmin chooses (up to 30 Hz).
  They cover players, vehicles and projectiles by template, with no `findId()`.

**Commands**
- The same 13 as today, made with typed engine calls where an interface exists: position,
  rotation, damage, team, score, and the game and HUD events.
- `rcon` still runs console commands, through the console singleton, so kick, ban, map changes and
  the RconScripts keep working.
- New: `RconResponse` is correlated by request id with no 10 000 wrap, and gets typed queries such
  as "list objects of template" with object ids, replacing `findId()`.

**Where the events come from: one engine change, R4**
- Most events reach the server's scripts only through the Python host's event dispatch, the 42
  retail events at `PythonHost.cpp:1882`. The module cannot see them without an engine hook.
- Proposed R4 (`[CHOSEN, not recovered]`): one listener interface on `IServerExtension`, called
  from the Python host's dispatch with the event id and typed arguments, before or after Python
  sees them. One engine site covers all 42 events. Retail Python behaviour is unchanged.
- Rejected alternatives:
  - injecting a C extension into CPython 2.3 the way BF2Hub does, because that ABI is fragile;
  - hooking engine internals by address, because it breaks on OpenBF2 updates.

**Later, not in the first version**
- Hooks that can veto or change an action, such as damage, team selection or spawn: base
  protection that cancels damage, refusing team stacking. These need their own engine hooks on
  `IGameLogic`, and their own approval.

## Open questions

| Id | Question |
|---|---|
| A1 | Does the Python host's dispatch carry everything each event needs (weapon template, root vehicle, CP team), or do some events need extra lookups? |
| A2 | Should the module dial out (as proposed) or listen for WebAdmin? Dialling out matches today's deployment. |
| A3 | Is HMAC with a pre-shared key enough, or is TLS needed in the first version? |
| A4 | Does the existing `GameReader`/`GameWriter` interface fit a second backend without changes to the game modules? |
| A5 | What happens when `mm_webadmin.py` and the native channel are both active? Proposal: the module refuses to start the admin channel if `mm_webadmin` is loaded, so events are never sent twice. |

## Order of work, once approved

1. Investigation only, no code: A1, A4, and the R4 design against the current source.
2. R4 in OpenBF2 (branch `serverext/r1`), plus the module's admin channel with a test client and a
   fuzz mode.
3. A BF2WebAdmin backend, tested against a local BF2WebAdmin with the netsky 2v2 setup. The
   BF2WebAdmin local test containers no longer exist, so that environment must be recreated
   first. That is the author's environment, so it waits for the author's go-ahead.
