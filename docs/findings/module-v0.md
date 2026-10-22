# Module v0: `libserverext.so`, `findSpawnGroup` and the test client

Status: done on 2026-10-22 (UTC) by a worker thread. Nothing is committed in either repo. The
OpenBF2 changes are uncommitted in the worktree `C:\Users\Alex\.t3\worktrees\OpenBF2\serverext-r1`
(branch `serverext/r1`), on top of the R1/R1b changes from `r1-smoke.md`. The test server is stopped.

The design record for the OpenBF2 change is `docs/protocol.md` "Engine requirements" R1, which
stands in for the linked issue OpenBF2's `AGENTS.md` asks for. IDA and Ghidra were not used. No
recovered body was written or changed; `findSpawnGroup` is an intentional addition built only from
existing typed, recovered calls.

## Summary

| Stage | Result |
|---|---|
| Part 1: `IServerExtension::findSpawnGroup` | Added and used at runtime: team 1 gets group 3 (Z10 pad), team 2 gets group 1 (AH1Z pad) on `dalian_2_v_2`. MSVC x86 full gate `ALL OK`; Linux build pass; msvc-x64 strict link fails with the **same 6 VoIP symbols** as HEAD (diffed). |
| Module load | `openbf2.addModDll: loaded 'libserverext.so' (initDll ok)`; tick hook and unload hook registered; listener on `127.0.0.1:16700`. |
| Welcome | Pass: map `dalian_2_v_2`, mode `gpm_cq`, 30 Hz. |
| Bot created, spawned, in heli | Pass: created on tick N, alive on N+1 or N+2, seated in the nearest Z10 pilot seat by `EnterVehicle(0, seat 0)` about 2 s later. |
| 60 s hover from Windows | **Pass** through WSL localhost forwarding: `HOVER HELD 60 s; max altitude error 2.99 m; horizontal drift 8.4 m; health 1.00`. |
| Watchdog | Pass: tripped once, 6 ticks after the last `SetInput`; cyclic and pedals 0, collective held; the Z10 kept station. |
| Disconnect cleanup | Pass: bots neutralised at once and destroyed 6 ticks later, also while airborne. |
| Reconnect | Pass: 400+ connections on one server process; each new sidecar gets its own Welcome and bots. |
| Map change | Pass: `BotRemoved` reason 2 on the tick after the unload, then recreate, spawn and re-enter. |
| Fuzz | Pass: 29 cases (26 + 3 that were a test-client bug, see [Fuzz](#fuzz)); server still at 30.5 ticks/s afterwards. One real module bug found and fixed. |
| 10-minute soak | **Pass**: one process, 12 min uptime, the bot hovering for 600 s continuously under the test client (20,101 `TickState`s, 0 tick gaps); 0 `[FRONTIER]`, 0 abort, 0 segfault lines; RSS flat at about 143 MB. |
| Per-tick cost | p50 20 µs, p99 40 to 60 µs with one flying bot. A tick with `EnterVehicle(0)` costs 3 to 5 ms (the nearest-vehicle walk). |
| Unit tests | `serverext_tests`: 620,271 checks, 0 failures, no engine needed. |

**Found during testing, not a module defect:** WSL's `wslrelay` stopped forwarding Windows
`127.0.0.1:16700` after a flood test that ended in a server-side close. RCON on 4723 kept working.
The fuzz and soak stages therefore ran from a client inside WSL. See
[WSL relay wedge](#wsl-relay-wedge-on-16700).

## Part 1: OpenBF2 change

`IServerExtension.h` gains, after `createInputPlayer` (append-only):

```cpp
virtual int findSpawnGroup(int team, bool enterOnSpawn) = 0;
```

`GameServer.cpp`: the adapter declares it, and the body sits right after `extern IBase* spawnManager;`
(it needs that declaration), plus `#include "../Objects/ISpawnPoint.h"`. Both carry
`[CHOSEN, not recovered]` and cite protocol.md R1. The body:

1. Returns -1 if the adapter is withdrawn or `spawnManager` is null.
2. Walks `SpawnManager::getGroupsForTeam(team)` (same team and `m_hasSpawnPoints`, in map-key order),
   then `getSpawnGroup(id)` and `getActive(true)`. This is the sequence `createAndSpawnAIPlayer`
   already uses, with `isAI = true` because the bot is AI-flagged.
3. Requires **every** spawn point of the group to match `enterOnSpawn`, where a point counts as
   enter-on-spawn when `getEnterOnSpawn() || getAIEnterOnSpawn()`. That is the test
   `ServerGameLogic::spawnPlayer` applies to an AI player (`ServerGameLogic.cpp:2110-2111`).
4. Returns the first (lowest) matching id, or -1.

Validation:

| Gate | Command / output dir | Result |
|---|---|---|
| MSVC x86, one TU | `Code\Tests\syntaxcheck.cmd Code\BF2\Game\GameServer\GameServer.cpp` | Pass |
| MSVC x86 full syntax gate | `Code\Tests\syntaxcheck.cmd` | `ALL OK`, exit 0 |
| Linux x86-64 | `testenv/build-linux.sh`, out `~/bf2-linux-serverext-v0` (fresh) | Pass, 0 errors, `python runtime check complete` |
| msvc-x64 strict link | `%TEMP%\serverext-v0\x64.cmd` (own Python build and `x64build`) | `LNK1120: 6 unresolved externals`, all `BF2VoipServer::CBF2VoipServer::*`. The sorted symbol set is **identical** to the HEAD baseline from `r1-smoke.md` (`x64-baseline.log`). 0 compile errors. |

An isolated x86 full build was not rerun; the change adds no `.cpp` and touches one TU.

## Part 2: the module

### Files

| File | Lines | Role |
|---|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | 78, 22 | `serverext_core` (static, engine-free), `serverext` (MODULE, `libserverext.so`), `serverext_tests`. `OPENBF2_SRC` is a cache variable; the default is this worktree (`/mnt/c/...` on Linux, `C:/...` on Windows). Windows configure stops with a clear message for now. |
| `build-module.sh` | 18 | Configure, build and test in WSL. |
| `src/Protocol.h/.cpp` | 198, 318 | Wire format: `Writer`, `Reader` (sticky overrun), `FrameDecoder`, exact-length parsers, encoders, axis conditioning. |
| `src/Config.h/.cpp` | 31, 146 | `serverext.cfg` parser. |
| `src/Channel.h/.cpp` | 89, 141 | The only shared state between the threads. |
| `src/Session.h/.cpp` | 45, 100 | Per-connection rules on the I/O thread: framing, Hello first, parsing. |
| `src/IoThread.h`, `src/IoThread_posix.cpp` | 22, 300 | Socket thread. A Windows port adds `IoThread_win32.cpp`. |
| `src/Module.cpp` | 884 | `initDll`, the tick handler and all engine work. |
| `tests/TestMain.cpp` | 517 | Unit tests. |
| `testenv/deploy-module.sh` | 26 | Writes `openbf2.con`, `serverext.cfg` and copies the module into a run dir. |
| `tools/testclient/serverext_client.py` | 581 | Test client. |

`probe/` is kept unchanged. It is superseded by the module, but `r1-smoke.md` cites it as its
evidence and this repo is untracked, so deleting it would be unrecoverable. It can go once the
author has committed or archived it.

The module links with `--no-undefined`, `-static-libstdc++` and `-static-libgcc`, like the probe.
Its only `NEEDED` entries are `libm`, `libc` and `ld-linux-x86-64`; the undefined dynamic symbols
are the weak TM-clone, `__cxa_pure_virtual` and `__gmon_start__` ones. `initDll` is the only export.

### Threading and ownership

- **Main thread** (`Module.cpp`): everything that touches the engine, inside the category-7 event.
  It re-posts the event first, so an early return cannot break the chain. Per tick it:
  1. drains the inbound queue with one `deque::swap` under the lock;
  2. applies commands (Hello, CreateBot, EnterVehicle, RemoveBot) and stores the latest `SetInput`
     per bot;
  3. for each bot: checks that it still exists, then runs the alive/death transitions, re-sets a
     cleared spawn group (every 5 s while dead), runs the watchdog and writes the bot's
     `io::PlayerInput`;
  4. builds `TickState` into a reused buffer and swaps it into the channel.
  No socket call, no disk I/O and no blocking. The only syscall is a one-byte non-blocking pipe
  write that wakes the I/O thread. Logging goes into a bounded queue that the I/O thread writes to
  stderr.
- **I/O thread** (`IoThread_posix.cpp`): `poll` on the wake pipe, the listener and the client, with
  a 100 ms timeout so logs are flushed. It reads up to 1 MiB per wake, feeds `Session`, and writes
  with `MSG_NOSIGNAL`. It never calls into the engine or Python. Started detached from `initDll`
  after the listener is bound synchronously, so a bad endpoint fails `initDll`.
- **Channel** (`Channel.cpp`), one mutex, short holds only:
  - inbound commands, bounded at 4096; a full queue closes the connection with Error 6;
  - outbound control frames (Welcome, BotCreated, Ack, BotRemoved, Error), never dropped while the
    connection lives, bounded at 8 MiB, above which the slow sidecar is disconnected;
  - one slot for the latest `TickState`; an unsent one is replaced, so a slow reader sees gaps;
  - logs, bounded at 10,000 lines with a dropped count.
  Every frame is tagged with the connection number, and anything for an older connection is dropped.
- **Lifetimes:** the module object, the channel and the I/O thread are heap objects that are never
  freed. The loader has no unload path, and a joinable `std::thread` destroyed at exit would call
  `std::terminate`.
- **Bots** are held as `(id, IPlayer*)`. The pointer is only compared with
  `PlayerManager::getPlayerFromId(id)`, never dereferenced first, so an engine-destroyed bot is
  detected without a use-after-free.

### Protocol behaviour, item by item

- **`serverext.cfg`:** `key value` or `key = value`, `#` and `//` comments. `botListen` must be an
  IPv4 address and port, never 16567; `watchdogTicks` is 1 to 3000. A bad value or a missing file
  is logged; a bad value makes `initDll` return false, so the module refuses to load visibly instead
  of listening somewhere unexpected.
- **Tick hook:** category 7 as in Q1. It fired at 30 Hz through the whole run (0 `TickState` gaps in
  5,401 frames over 180 s).
- **Framing:** a payload above 65,536 bytes, or a nonzero reserved field, is Error 2 and close.
  Parsers require the exact length, so trailing bytes are malformed.
- **Hello/Welcome:** the I/O thread enforces Hello first and the version, so Error 1 and 2 need no
  main-thread round trip. Welcome comes from the main thread: protocol 0, 30.0 Hz, the module's
  current tick, `getCurrentMapName()` and `getCurrentMapGameMode()`.
- **CreateBot:** team must be 1 or 2 (Error 7); the name has control bytes replaced, is capped at
  64 bytes (the engine truncates further) and must not be empty (Error 7).
  `findSpawnGroup(team, false)` runs **before** the player is created, so a refusal (Error 6)
  leaves no orphan player. Then `createInputPlayer`, `setTeam(team, false)`, `setKit(0)` and
  `setSpawnGroup(group)`.
- **EnterVehicle:** the bot must be this connection's (Error 4) and alive on foot (Error 6). A nonzero
  id must be at most 65535 and resolve through `IGame::getObjectFromNetworkableId`; it is walked to
  its root. 0 walks `ObjectManager::getAllRegisteredObjects()` for root objects within 30 m. A
  vehicle is a root with `IPlayerControlObject` that is not `ISoldier`. The checks are the seat's
  existence and `!isOccupied()`, and the vehicle team being 0 or the bot's. On failure it is Error 5.
  Then `IGame::enterVehicle(player, seat, false)`; if the player's vehicle is not the seat afterwards,
  Error 6.
- **RemoveBot:** `IGameServer::destroyPlayer`, then `Ack`, then `BotRemoved` reason 1. It worked
  for bots on foot and for bots flying a Z10; no abort.
- **BotRemoved:** reason 1 as above. Reason 2 when the bot vanished and a level unload was seen
  (below). Reason 3 when it vanished with no unload for two ticks.
- **Unload detection:** `GameServer::unload` posts `(ECHud, 3)` before destroying AI players. The
  module registers a second listener on `ECHud` for that. On the server only the AIDLL's listener
  uses that category, so the effect is that ECHud events are now dispatched to the module, which
  ignores everything but id 3, instead of being dropped. This was seen at startup (tick 0) and twice
  per `admin.runNextLevel`. The one-tick grace covers the case where the post is queued and lands
  after the handler.
- **SetInput:** an unknown or foreign player is Error 4. Each axis is rejected if NaN or infinite
  (Error 7, input not applied), then clamped and quantized (see
  [contract points](#contract-points-that-proved-wrong-or-ambiguous)). Latest wins within a tick.
  Nonzero buttons are ignored and logged once. Every tick the module zeroes the 64 inputs, sets
  `mappingFlags` to the `PlayerAction::get` mask, and writes PIPitch, PIRoll, PIThrottle and PIYaw.
- **TickState:** this connection's live bots first, then every other player whose vehicle is a PCO
  under a non-soldier root. Position and angles come from the root's absolute transformation with
  the contract's formulas. Velocities come from `queryComponent(IID_IPhysicsNode, IID_IPhysicsNode)`,
  health is `getHitPoints / getMaxHitPoints` of the root's `IArmorGLComp` component, clamped to 0..1.
  The root id is `getNetworkable()->m_networkableId`. Time is `System::getExactTime()`.
- **Watchdog:** after `watchdogTicks` ticks without `SetInput`, cyclic and pedals go to 0 and
  collective is held; logged once until input resumes.
- **Disconnect:** the bots are neutralised immediately and destroyed after `watchdogTicks` ticks,
  without `BotRemoved`, since there is no one to tell.

## Part 3: test client

`tools/testclient/serverext_client.py`, Python 3 standard library only. Why Python: it runs
unchanged on Windows and in WSL with no build step and no dependency on the module's code, so it
checks the wire format independently of the C++ encoder. `struct` covers the format exactly.

- Normal mode: Hello, CreateBot (team 1), wait for alive, `EnterVehicle(0, seat 0)`, 5 s spin-up,
  then a climb and hover. Collective is a PID on height above the pad (target 15 m).
  Station keeping is velocity to attitude to cyclic, plus heading hold on pedals. It sends one
  `SetInput` per `TickState` and prints the bot's state every `--print-every` ticks. It re-enters
  after a death and recreates the bot after `BotRemoved`.
- `--blips` adds short pedal, roll and pitch pulses during the hover, for sign checks.
- `--stop-sending-after`, `--exit-after`, `--no-remove` drive the watchdog and disconnect stages.
- `--fuzz` runs the hostile cases listed below and then checks that the server still ticks at 30 Hz.

## Runtime validation

Setup:

- Run dir `~/serverext-v0/srv`, made by `testenv/prepare-content.sh` from
  `cr.nihlen.net/library/bf2-netsky:2v2-nocheck` (image id `62c880556ef2`), with `bf2` and
  `libdice_py.so` from `~/bf2-linux-serverext-v0` (md5 `a778ce7b…` for `bf2`, checked) and
  `testenv/deploy-module.sh`. Content settings unchanged otherwise, including
  `sv.numPlayersNeededToStart 2`.
- Ports: game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723, bot channel 127.0.0.1:16700. VoIP
  55124/55126 as usual. No other server was running; no container was created or touched.
- Path: `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`, real loads of
  `dalian_2_v_2` (`gpm_cq` 16). No `BF2_*` harness variables, no game client connected.
- Three server processes, all stopped with SIGTERM:
  1. module build 1: load through the map change, plus the first fuzz run, about 15 minutes;
  2. module build 2 (the fuzz fix): about 3 minutes, during which the relay wedge was found;
  3. module build 2: fuzz from WSL and the soak, 12 minutes.

  The hover from Windows ran on build 1. Build 2 differs only in `Channel::setConnection`.

### Load and Welcome

```
[serverext] loaded: protocol v0, bot channel 127.0.0.1:16700, watchdog 6 ticks; tick hook 1 (event 1), unload hook 1, System 0x63002c9b8210
[serverext] tick 0: level unload
openbf2.addModDll: loaded 'libserverext.so' (initDll ok)
[serverext] connection 1 from 127.0.0.1:47662
[serverext] tick 797: Hello from 'serverext testclient' on connection 1; Welcome map 'dalian_2_v_2' mode 'gpm_cq'
```

### Bot created, spawned, in the heli

```
[serverext] tick 798: created bot 255 '[BOT] test' team 1 kit 0 spawn group 3 (ai 1 connected 0)
[serverext] tick 799: bot 255 spawned
[serverext] tick 858: bot 255 entered 'ahe_z10' (net id 1819) seat 0
```

Client side (`TickState`, flags 7 = alive, own bot, in vehicle):

```
tick 857 spinup cmd c+0.00 | id 255 fl 7 team 1 seat 0 root 1819 pos -218.06 153.89 159.06 ypr 163.7 0.6 -0.0 vel 0.00 0.00 0.00 hp 1.00
```

### 60 s hover from Windows

The client ran on Windows against `127.0.0.1:16700` through WSL localhost forwarding.

- Run 1, altitude hold only: altitude held, but the Z10 trims about 4° nose down, so it flew forward
  at 15 m/s, crashed and was respawned. That exercised the death, respawn and re-enter path four
  times.
- Run 2, `--blips`, gave the input signs below.
- Run 4, with station keeping and velocity integral:

```
08:42:17 Ack req 2
08:42:30 hover reached at height 12.0 m above the pad
08:43:30 HOVER HELD 60 s; max altitude error 2.99 m; horizontal drift 8.4 m; health 1.00
08:43:30 Ack 03000000
08:43:30 BotRemoved ff01
```

Measured input response on the Z10 in hover. Each is consistent with the Q2 conventions; yaw and
roll were unexercised before.

| Input | Response |
|---|---|
| pedals +0.3 | ω_y rises to +0.08 rad/s within 0.4 s; yaw increases (nose right) |
| roll +0.2 | roll angle decreases (bank right); ω about the body forward axis matches |
| pitch +0.2 | pitch angle increases (nose down) |

The hover settles at collective about 0.33 and pitch about -2°.

### Watchdog

```
08:44:38 STOP SENDING SetInput now (watchdog test); still reading state
[serverext] tick 17829: bot 255 watchdog: no SetInput for 6 ticks; cyclic and pedals 0, collective 0.33
```

Over the following 10 s the Z10 stayed within 0.2 m horizontally and climbed 0.4 m, because the
held collective was slightly above trim. That is one data point for Q5, not an answer.

### Disconnect cleanup and reconnect

```
[serverext] connection 5 closed after 1204 frames: the sidecar closed the connection
[serverext] tick 18123: sidecar connection 5 gone; 1 bots neutralised, removal in 6 ticks
[serverext] tick 18129: bot 255 removed after its sidecar disconnected
[serverext] connection 6 from 127.0.0.1:33386
[serverext] tick 18615: Hello from 'serverext testclient' on connection 6; Welcome ...
```

The bot was airborne when it was destroyed. Server process 3 accepted more than 400 connections.

### Map change

`exec admin.nextLevel 0`, then `exec admin.runNextLevel` over RCON while the bot flew:

```
[serverext] tick 19397: bot 255 died                       (end of round)
[serverext] tick 20146: level unload
[serverext] tick 20146: level unload
[serverext] tick 20147: bot 255 no longer exists (level unload); not destroying it again
[serverext] tick 20218: created bot 255 '[BOT] test' team 1 kit 0 spawn group 3 (ai 1 connected 0)
[serverext] tick 20278: bot 255 entered 'ahe_z10' (net id 1819) seat 0
```

Client: `BotRemoved player 255 reason 2`, `BotCreated req 4`, `Ack req 5`.

Transient refusal seen once: the first `CreateBot` after a previous bot had been destroyed got
`Error 6 "no active on-foot spawn group"`; a retry 2 s later succeeded. `SpawnGroup::getActive(true)`
asks each point's `getActive`, which evidently went false for a moment, probably a point
cooldown or occupancy (not traced). The sidecar must retry `CreateBot` on Error 6.

### Fuzz

Run from WSL. Every case below is a separate connection unless noted.

```
PASS garbage before Hello: Error code 2, closed
PASS SetInput before Hello: Error code 2, closed
PASS Hello version 1: Error code 1, closed
PASS unknown type 0x7777: Error code 3, closed
PASS module-only type Welcome: Error code 3, closed
PASS oversize length 65537: Error code 2, closed
PASS length 0xFFFFFFFF: Error code 2, closed
PASS reserved != 0: Error code 2, closed
PASS short SetInput: Error code 2, closed
PASS long CreateBot string length: Error code 2, closed
PASS Hello twice: Error code 2, closed
PASS max-size unknown frame: Error code 3, closed
PASS truncated frame then hang-up: sent
PASS 300 random-noise connections: 300 ended with Error+close
PASS 100 connect-and-close
PASS second connection: closed by the server
PASS SetInput unknown player: Error code 4 ..., connection kept
PASS CreateBot team 3: Error code 7 'team must be 1 or 2', connection kept
PASS CreateBot empty name: Error code 7 'name is empty', connection kept
PASS EnterVehicle foreign player: Error code 4 ..., connection kept
PASS RemoveBot foreign player: Error code 4 ..., connection kept
PASS SetInput NaN: Error code 7 'SetInput axis is NaN or infinite; input not applied', connection kept
PASS SetInput +inf: Error code 7 ..., connection kept
     EnterVehicle id 70000: Error code 5 'vehicle id out of range', connection kept
     EnterVehicle id 65535: Error code 5 'no object with that networkable id', connection kept
     EnterVehicle seat 200: Error code 5 'no vehicle with that seat free within 30 m', connection kept
PASS 20000-frame SetInput burst sent
PASS Hello right after a flood, then slow reader 5 s
PASS server still ticking: 93 TickStates in 3 s, 30.5 ticks/s
```

The three unmarked lines were printed as FAIL because the client expected `None` as the code; the
server's answers are correct and the client now expects 5. Also logged: out-of-range axes clamped,
`buttons 0xFFFFFFFF ignored ... (logged once)`, and the bot of the fuzz connection removed 6 ticks
after the connection closed.

**Bug found and fixed.** In the first fuzz run, the 20,000-frame burst filled the command queue and
was closed as designed. The next connection's Hello, arriving before the main thread's next drain,
was refused with Error 6 because the old connection's 4,096 stale commands still counted.
`Channel::setConnection` now drops the old connection's commands (lifecycle events are kept), and a
unit test covers it. The second fuzz run shows the fix.

### WSL relay wedge on 16700

After the first fuzz run, which ran from Windows, Windows connections to `127.0.0.1:16700` were
accepted by `wslrelay.exe` but never reached the server. There was no `connection` log line, and
`Get-NetTCPConnection` showed the relay holding `CloseWait` sockets. RCON on `127.0.0.1:4723` still
worked through the same relay, and clients inside WSL reached 16700 normally. The relay kept
listening on 16700 more than two minutes after the server process had exited.

The likely trigger is the module closing a connection while the relay still had buffered data for it:
the 680 KB burst, then `shutdown`/`close` with unread data, which sends an RST. This is not proven.
I did not restart WSL or kill `wslrelay` (shared with Docker Desktop and the author's
environment), so **the relay may still be wedged for 16700**. That matters for a Windows-hosted
sidecar: if the module closes it with unread data, it may not be able to reconnect through WSL
forwarding until WSL restarts. The Docker host does not use `wslrelay`.

### Soak

Server process 3, client inside WSL (the relay was wedged), started after the fuzz run on the
same process:

```
08:55:43 Welcome: protocol 0, 30 Hz, tick 1306, map 'dalian_2_v_2', mode 'gpm_cq'
08:55:45 Ack req 2
08:55:58 hover reached at height 12.0 m above the pad
09:05:58 HOVER HELD 600 s; max altitude error 2.99 m; horizontal drift 8.5 m; health 1.00
09:06:53 exit after 670.0 s; 20101 TickStates, 0 tick gaps
09:06:53 Ack 03000000
09:06:53 BotRemoved ff01
```

Last state line: `pos -214.37 168.89 146.26 ypr 163.6 -4.0 -0.0 vel -0.00 -0.01 -0.01`, collective
0.33. Server log over the whole process (`bf2.log`, 12:02 uptime): `FRONTIER 0 abort 0 segv 0`; the
same for process 1 (`bf2-run1.log`). RSS 144,076 KB at 12 s of uptime and 143,176 KB at 12 min.
After SIGTERM no `bf2` process remained and 16571, 29903, 4723 and 16700 were unbound.

Logs: `~/serverext-v0/bf2-run1.log`, `~/serverext-v0/srv/bf2.log` (process 3) in WSL; client logs
`%TEMP%\serverext-v0un*.log` on Windows.

### Per-tick cost

Measured inside the handler with `steady_clock`, from just before the re-post to the end of the
`TickState` encode. The I/O thread's work is not included. The module logs a 10 µs-bucket histogram
every 1,800 ticks.

Values are bucket upper edges (10 µs resolution).

| Window | p50 | p99 | max | Notes |
|---|---|---|---|---|
| Soak, 1,800-tick windows with 1 bot hovering | 20 to 30 µs | 40 to 50 µs | 54 to 358 µs | No `EnterVehicle` in the window |
| Soak, whole run (19,800 ticks) | 20 µs | 50 µs | 3,458 µs | The max is the `EnterVehicle(0)` tick |
| Process 1, whole run (16,200 ticks), 1 bot, 5 sidecar sessions | 20 µs | 50 µs | 4,753 µs | Each window with an `EnterVehicle(0)` has a 3 to 5 ms max |
| Idle, no bot | 10 µs | 30 µs | 234 to 669 µs | |

No tick in any run took over 20 ms. The per-tick work with one bot is two player lookups, one
transform, two component queries, the player-list walk and a 73-byte `TickState`. The outlier is the
nearest-vehicle search over `getAllRegisteredObjects()` (about 15,000 objects).

## Contract points that proved wrong or ambiguous

`docs/protocol.md` was not edited. Each item names the reading I implemented.

1. **SetInput quantization formula versus "as a human's input is quantized".** The contract gives
   `float(short(int(v * 100))) / 100`, but the human path quantizes twice: the client stores
   `short(int(convertAxisToInternalResolution(v) * 100))` (`PlayerAction::setAxis`) after
   `convertAxisToInternalResolution` already truncated to hundredths, and the server divides by 100.
   The float round trip loses another hundredth for some values; 0.57 becomes 0.56. The unit test
   finds 4,023 of 200,000 random inputs where the two differ. **Implemented: the two-step human
   chain**, because the contract's stated intent and its citation (`PlayerAction.cpp:27-112`) are
   human parity. The bf2-bots physics port should use the same chain.
2. **Malformed frames: close or keep?** Only "unknown type closes" is stated. **Implemented: every
   framing or parse error is Error 2, then close**, because after a bad length the stream cannot be
   trusted. Per-request semantic errors (codes 4 to 7) keep the connection.
3. **Message types sent in the wrong direction** (for example a sidecar sending `Welcome`) are
   treated as unknown, Error 3 and close.
4. **The second connection is "refused"**, with no frame defined. It is accepted and closed at once,
   with no Error frame, and logged.
5. **The bot "must be alive and on foot"** for `EnterVehicle`, with no code assigned. **Used: 6
   (engine refused).** Vehicle and seat problems are 5.
6. **Watchdog neutral on foot.** "Collective held" only makes sense in a vehicle; on foot a held
   PIThrottle would keep the soldier walking. **On foot, every axis goes to 0.** Also, on death the
   stored axes are zeroed, so a respawned bot stands still until the sidecar sends input.
7. **Buttons are "reserved, 0 in v0"**, but a nonzero value has no defined handling. **Ignored, and
   logged once.** Rejecting would drop the axes too.
8. **TickState tick numbering.** "The snapshot taken there is the end of the previous tick, and
   `TickState` is numbered accordingly." **Implemented: the handler's own tick N sends `TickState`
   N-1, and `Welcome` carries N.** A sidecar should echo the `TickState` tick in `SetInput`.
9. **Vehicle health for a vehicle with no armour component** is undefined; it is sent as -1, the
   same as on foot. Every heli tested has armour.
10. **CreateBot needs a loaded level.** During a load, or briefly after a bot was destroyed,
    `findSpawnGroup` can return -1 and `CreateBot` gets Error 6. The contract does not say the
    sidecar must retry; it must.
11. **`findSpawnGroup` matching.** "Spawn points match `enterOnSpawn`" does not say all or any.
    **Implemented: all points**, counting `getAIEnterOnSpawn` as enter-on-spawn for the AI-flagged
    bot, so an on-foot request can never land on an in-vehicle point.
12. **Unload detection** needs a listener on `ECHud` (above). That is not in the contract. It is a
    module-side choice with no engine change, but it makes `ECHud` events dispatch on the server
    instead of being dropped.
13. **The "nearest vehicle" search** considers every root `IPlayerControlObject` that is not a
    soldier, not only helicopters, as the contract says "vehicle". On `dalian_2_v_2` the pad helis
    were the only candidates within 30 m.
14. **`serverext.cfg` errors.** The contract gives defaults only. A bad value makes the module
    refuse to load instead of silently falling back.

## What is left

- **Windows build** (`serverext.dll`): needs `IoThread_win32.cpp` and a non-pipe wake in `Channel`.
  x64 validation is still blocked by the VoIP `.def` link failure at HEAD.
- **Docker host** (glibc 2.31): the module must be built in Debian 11 (`build-bullseye.sh`) and
  needs `libpthread` there. Not run.
- **Q5**: whether held collective is a safe neutral. One watchdog run held station; a bot in a climb
  or turn was not tested.
- **Q8**: category 7 in unrecovered retail code, still unverified (no IDA or Ghidra).
- **R1b sites, enter-on-spawn and `PIUse`:** still not exercised. Buttons are ignored, so `PIUse`
  stays unreachable from the protocol.
- **A game client joining** while bots fly, and the scoreboard and Python view of the bots (Q9).
- **`EnterVehicle(0)` cost:** about 4 ms on the tick it runs, because it walks about 15,000
  registered objects. That is fine for occasional requests; a spatial query or a cached vehicle
  list would be needed if a sidecar issues it often.
- **The WSL relay wedge** needs a controlled reproduction on a spare port.
- **Q4 client ControlMap stage** remains open; the module matches the server-side human path only.

## Remaining frontiers relevant to this change

- No `[FRONTIER]` line was printed in any run (counts below).
- `[UNRESOLVED]` markers in `SpawnManager.h` concern its private fields and the `ISpawnManager` base;
  `findSpawnGroup` uses only the public virtuals `getGroupsForTeam` and `getSpawnGroup` and
  `ISpawnGroup`/`ISpawnPoint` virtuals, all of which `createAndSpawnAIPlayer` or `spawnPlayer`
  already call.
- The `SpawnPoint.cpp:211-214` `[FRONTIER]` comment is still stale, as `r1-smoke.md` says; the point
  bodies ran.
