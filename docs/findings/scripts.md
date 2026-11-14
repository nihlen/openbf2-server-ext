# Admin channel: scripts (2026-11-14)

Approved by the author on 2026-11-14. Contract: `docs/admin-channel-draft.md` "Scripts" (capability bit 3,
`StartScript`, `StopScript`, `ScriptMessage`, `ListScripts`, `ScriptList`, `ScriptFinished`, `ScriptEvent`,
`adminAllowScripts`). The admin protocol version stays 1. No engine change. This is the module side; BF2WebAdmin's
side (the TypeScript sources, their build and the C# runner) is in that repo.

## What was added

| Item | Where |
|---|---|
| QuickJS-NG v0.18.0, unmodified | `third_party/quickjs-ng/` (its `README.md` has the source, the sha256 and how to update); the static library `serverext_qjs` |
| The script host | `src/Script.h`, `src/Script.cpp`: engine-free, one QuickJS runtime and context per script, the `script` and `game` objects, the life cycle, the limits. It reaches the game only through `script::World` |
| The engine side | `src/Admin.cpp`: the four commands, `script::World` on the engine (the bodies of the `Teleport` and `Say` commands, `IPhysicsNode::setPositionalSpeed` and `setRotationalSpeed` with `setIsAwake`), the scripts' tick after the tick's commands, level unload, link down, capability bit 3, a `scripts ...` part in the stats line |
| Codec and config | `src/AdminProtocol.*` (types `0x1318`-`0x131B`, `0x1405`, `0x121C`, `0x121D`, `kCapScripts`), `src/Config.*` (`adminAllowScripts`, default 1) |
| Reference client | `tools/adminclient/adminclient.py`: the codec, seven golden frames, `--scenario script` (the commands, every end reason, the limits; with a bot-channel bot also the host API against the player stream), `--scenario scriptends` (level unload, closed link), `--scenario runscript --script-file x.js` (runs one script on a test server and prints its events and cost) |
| Unit tests | `tests/ScriptTests.cpp` (`serverext_script_tests`, the host against a mock world), `tests/AdminTests.cpp` (parsing, bad values, golden frames, config) |

How the pieces of the contract were built that it leaves open:

- **No `eval`.** `JS_Eval` needs the parser, and a context that has the parser also has a working `eval` and
  `Function` constructor. So the source is compiled in a second context of the same runtime (base objects and the
  parser, nothing else), written out with `JS_WriteObject`, read into the script's context with `JS_ReadObject` and
  run there with `JS_EvalFunction`; the parser's context is freed before that. The script's context is
  `JS_NewContextRaw` plus `JS_AddIntrinsicBaseObjects`, `JSON`, `MapSet`, `TypedArrays` and `Promise`. The bytecode
  never leaves the process, so reading it is safe.
- **The time budget** is the runtime's interrupt handler, which compares `CLOCK_THREAD_CPUTIME_ID` with the call's
  deadline. QuickJS calls it once per 10000 polls (loop back-edges and calls of script code, and some loops inside its
  built-ins). An interruption cannot be caught by the script. After every call the host also compares the time the call
  took with its budget, and the sum over the last 30 ticks with 30 ms.
- **Unhandled rejections**: the runtime's rejection tracker remembers a rejected promise until a handler is attached;
  what is left when the call's promise jobs have run ends the script (reason 3).
- **Order**: what a script emits is queued in the host and handed to the admin channel after the command's reply, so
  `Ok` always comes first and `ScriptFinished` last.

## Setup

| Item | Value |
|---|---|
| OpenBF2 | `serverext/r1` working tree on `824d4dc` (with the author's uncommitted R7 changes, which the module's `f7de037` needs), built into `~/bf2-linux-scripts` (`testenv/build-linux.sh`), `bf2` md5 `de9c5acb3baa04352a1f943af230a591` |
| Module | this change on `1d3f5a7`, built into `~/serverext-build/scripts`, `libserverext.so` md5 `1ee08066c6a8e31b3d1d623145515adf` |
| Server | WSL, the netsky `2v2-nocheck` content copied from an earlier run directory into `~/serverext-scripts/srv` (the image is not on this PC any more), `dalian_2_v_2` `gpm_cq` 16; game 16585/udp, GameSpy 29909, RCON 127.0.0.1:4729, bot channel 127.0.0.1:16735, admin dial-out to 127.0.0.1:16736 (`adminTls 0`, loopback) |
| WebAdmin | `tools/adminclient/adminclient.py --no-tls --port 16736`; logs `~/serverext-scripts/logs/final-*.log` |
| Bots | `tools/testclient/serverext_client.py --hover-height 60` (a Z-10 hovering 60 m over its pad, the pilot holding height, attitude and heading), and a bot that spawns and stands (`~/serverext-scripts/soldier.py`, the test client's `Client` class) |

Ports 16567, 16569, 16571 and the localtest were not used; no container was touched. The server was stopped
afterwards.

## Results

Unit tests: `serverext_tests`, `serverext_admin_tests` and `serverext_script_tests` (524 checks) pass. The golden
file has 80 frames (new: `cmd_start_script`, `cmd_stop_script`, `cmd_script_message`, `cmd_list_scripts`,
`script_list`, `ev_script_finished`, `ev_script_event`). The script tests also pass built with
`-fsanitize=address,undefined` and QuickJS's asserts on (no leak, no assert), except one whose result depends on
speed (under the sanitizer the time budget ends the memory-filling loop before the memory limit does).

Live, the final build: `script` without a bot **32/32**, with the helicopter **43/43**, with the soldier **39/39**;
`scriptends` **9/9**; `--fuzz` **3/3**; the server ran on and its log has no abort.

| Check | Result |
|---|---|
| Capability | `Welcome` caps 11 (console, objects, scripts) |
| A counting script | `Ok`; one `ScriptEvent` per tick on consecutive ticks; the handler's tick is `game.tick`; `finish({ticks: 5})` gives `ScriptFinished` reason 1 with that result after the stop handler's event; 7 calls |
| The world | `script.api` 1, `game.map`, `game.tickRate` 30, `game.players()` equal the snapshot; `script.log` is `ScriptEvent` kind 2; `game.say` is a `Chat` event from the server with `§` and `é` intact |
| `ListScripts`, `ScriptMessage`, `StopScript` | the running script is listed; the message handler gets the parsed JSON; `StopScript` gives the stop handler's event and reason 2; unknown instances are `Error` 5, a message that is not JSON `Error` 7 |
| Errors | a throwing handler: reason 3 with `Error: boom` and the frames `at boom (throws:2:13)`; an endless loop: reason 4 after 5.0 ms, and the server answers `Ping`; a syntax error: `Error` 7 `SyntaxError: ... at syntax:2:9`; throwing top-level code, arguments that are not JSON, flags 1, instance 0, a bad name: `Error` 7; API 2: `Error` 5; a used instance and a ninth script: `Error` 6 |
| The language | no `Date`, `RegExp`, `Proxy`, timers, `console`, `std`, `os`; `eval` and `new Function` throw a `TypeError` |
| `game.player` | in 10 ticks equal to the player stream frame of the same tick: team, alive, seat, `rootId`, rotation, and position and velocity to the last bit. Helicopter: `seated` true, `seat` 0, `vehicle` `ahe_z10`; soldier: `seated` false, `seat` -1, `vehicle` `ch_light_soldier` |
| `game.teleport` | helicopter and soldier 40 m up: at the target in the next stream frame, the yaw kept |
| `game.setVelocity` once | helicopter `[0, 15, 0]`: vertical speed 15.0 in that tick's frame, then 14.3, 13.6, ... 9.0 nine ticks later, 3.6 m higher. Soldier `[0, 8, 0]`: 8.0, 7.9, 7.3, ... 3.5, 1.8 m higher |
| `game.setVelocity` every tick for 30 ticks | helicopter `[0, 0, 6]`: moved 5.78 m north; soldier `[0, 0, 3]`: 2.52 m (it stands on the ground) |
| A soft ceiling | 15 m below a helicopter hovering at 213.9 m whose pilot keeps holding 213.9: `setVelocity` with a downward speed of twice the overshoot (at most 30 m/s) whenever it is above. From second 8 to 12 it stays between 199.0 and 199.1 m (ceiling 198.9); 350 pushes in 12 s |
| `game.setAngularVelocity` | 1 rad/s every tick for 15 ticks (28.6 degrees) on a level helicopter facing north: about world y yaw +28.8; about world x pitch +28.3 (nose down); about world z roll +28.2 (bank left); the other two angles within 0.5 degrees. On a soldier: nothing (and a teleport's rotation does not stay: its yaw is back at its look yaw in the next tick) |
| Level unload | `Map` next: both scripts end with reason 5; the stop handler ran first, still counted the player and its chat line `script stop: level unload` reached the bot client; `ListScripts` empty on the new map, and a script starts there |
| Closed link | the listener closes the socket with a script running: the module logs `ending 1 scripts with the link`, the stop handler's `script stop: link closed` reaches the bot client, the next link has session + 1 and no scripts, and the instance can be used again |
| Fuzz | 24 rounds of hostile frames, the new types included: the module answers with `Error` and dials back; a clean link answers `Ping` |

## Cost

Measured on this PC (WSL 2), `RelWithDebInfo` (`-O2`).

| What | Cost |
|---|---|
| `libserverext.so`, stripped as the release build strips it (`strip --strip-debug`) | 879,440 → 2,114,736 bytes (+1.24 MB); fully stripped 730,440 → 1,877,712 (+1.15 MB). No new shared library: `libssl.so.3`, `libcrypto.so.3`, `libm.so.6`, `libc.so.6` as before; no QuickJS symbol is exported |
| Memory | about 175 KiB per script before it allocates anything of its own; at most 4 MiB each |
| Starting a script | about 100 µs for ten lines of source in a unit-test loop (the runtime, both contexts, compiling, the top-level code), about 30 µs to stop it. Not measured as a whole on the live server, where the first use is colder; the top-level code alone took 15 to 60 µs there |
| An idle script (an empty tick handler) | about 1 µs of CPU per tick (eight of them: 0.96 to 1.07 µs each) |
| A handler that calls `game.players()` and loops over the result | about 12 µs per tick with one player |
| `game.player(id)` | about 12 µs for the first in a tick, about 1 µs for each further call (cold caches make the first use in a tick the expensive one) |
| The ceiling script (one `game.player`, some arithmetic, one `game.setVelocity`) | 21.6 µs per tick, 35 µs the longest |
| The host API in a hot loop (unit test, mock world) | `game.player` 0.6 µs, `game.players()` 0.1 µs, `game.setVelocity` 0.06 µs, `game.teleport` 0.08 µs, `game.tick` 0.03 µs |
| Module stats line with scripts running | `admin cost (us) p50 10 p99 200` during the test scenarios (the p99 is the test scripts' start-ups and errors); max 5.3 ms in the period with the endless-loop test |

For comparison the admin channel without scripts costs 20 to 40 µs per tick at p99 (`docs/admin-channel-draft.md`).

## Limits of the limits

- **The time budget stops mistakes, not attacks.** It is checked where QuickJS polls: an endless `for (;;) {}` ends
  after 5.0 ms and an endless `indexOf` over a sparse array of 2^32 elements after 5.1 ms, but a loop whose every
  round is one long built-in call is only checked every 10000 rounds: 23 ms around `JSON.stringify` of 40 numbers,
  5.2 ms around a 4 KiB `fill`. A script written to stall (a loop around a sort of a large array) can hold the main
  thread for seconds before it is ended. Fixing that needs a smaller poll interval inside `quickjs.c`
  (`JS_INTERRUPT_COUNTER_INIT`, not configurable from outside), that is a patched QuickJS, which this change does
  not do. Scripts come only from WebAdmin over the authenticated link, the same trust the `Console` command has.
- **Wall time is not counted**, only the main thread's CPU time, so a descheduled server thread does not end a script
  and a script cannot block: there is nothing in the API that waits.
- **`log` writes to the module's log on the main thread**, at most 16 lines per script and tick.

## Not tested

- What a client sees: there was no human client. A body moved by `setVelocity` or `teleport` is predicted by its own
  pilot's client, so that pilot may see corrections, as with the `Teleport` command.
- More than one player in a script on a live server (the unit tests cover it against the mock world), and a full
  server.
- The release and localtest image builds were not run. They build this repo with the same preset and run `ctest`,
  and their builders have `gcc` (it comes with `g++`), so they need no change; `git archive` carries `third_party/`.
  The localtest image cannot be built from commits at the moment for another reason: the module's R7 commands need
  OpenBF2's R7 methods, which are still uncommitted in the OpenBF2 checkout.
- A Windows build (there is none yet; QuickJS-NG builds with MSVC, `Script.cpp` uses `clock_gettime`).
