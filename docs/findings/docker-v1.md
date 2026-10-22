# Module v1 on the Docker host (netsky image, Debian 11)

Status: done on 2026-10-22 by a worker thread. Until now every module run was in WSL (Ubuntu 22.04,
glibc 2.35). This run puts the v1 module, unchanged, into the production shape: the netsky
`2v2-nocheck` image (Debian 11.11, glibc 2.31), started by the image's own `run.sh`. Every client
ran on Windows against the published ports. `docs/protocol.md` and `docs/findings/test-env.md` were
not edited; corrections are listed at the end.

## Versions and inputs

| Item | Value |
|---|---|
| OpenBF2 | worktree `C:\Users\Alex\.t3\worktrees\OpenBF2\serverext-r1`, branch `serverext/r1`, commit `540fa88`, clean |
| Module | this repo at `5fc2a6d` (v1 source unchanged since `64a7bb3`) |
| Toolchain | `debian:bullseye`, g++ 10.2.1, glibc 2.31-13+deb11u14, cmake 3.30.5 (`testenv/build-bullseye.sh`) |
| Build dir | `~/bf2-linux-serverext-docker-v1` in WSL (`build/` for OpenBF2, `module/` for the module), log `~/bf2-linux-serverext-docker-v1.build.log` |
| Base image | `cr.nihlen.net/library/bf2-netsky:2v2-nocheck`, id `62c880556ef2` |
| Test image | `serverext-testenv:latest`, id `9efc428ced5a` (kept) |
| md5 | `bf2` `1fe717bb142b5c8e18db72227a65b232`, `libdice_py.so` `b9d59a84755e0174ba512baf8fbd5bd0`, `libserverext.so` `f43e04b980207eabf0bc1ecd4c042eff` |
| Ports | game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723 (container 4711), bot channel 127.0.0.1:16702 (container 0.0.0.0:16702); all published on 127.0.0.1 only (`PUBLISH_IP=127.0.0.1`) |
| Client | Windows, Python 3.10.6, `tools/testclient/serverext_client.py --port 16702`; logs in `%TEMP%\serverext-docker-v1\run*.log` |

Before starting: no WSL `bf2` process was running and no container was up. On Windows, 127.0.0.1:16700
is held by a stale `wslrelay.exe` (PID 49784). module-v1.md also left it alone, and this run did not
use 16700. No other container or image was touched. The new build retagged `serverext-testenv:latest`,
so the earlier image from the test-env work (`82c097c0a336`) is now untagged and still present.

## Build

`testenv/build-bullseye.sh` now also builds the module in the same Debian 11 container
(`MODULE_SRC`, default this repo, mounted read-only; `MODULE_SRC=` skips it), runs its unit tests and
prints its `NEEDED` entries and its newest `GLIBC_` symbol version.

| Gate | Result |
|---|---|
| OpenBF2 `bf2` + `dice_py_check_run`, strict link | Pass, `python runtime check complete` |
| Module build | Pass (`libserverext.so`, 3.0 MB with RelWithDebInfo) |
| Module unit tests (`ctest`) | 1/1 passed |
| Newest symbol version | `bf2` GLIBC_2.29, `libdice_py.so` GLIBC_2.29, `libserverext.so` GLIBC_2.17, all ≤ 2.31 |
| Module `NEEDED` | `libm.so.6`, `libpthread.so.0`, `libc.so.6`, `ld-linux-x86-64.so.2` |

- **libpthread:** in glibc 2.31 the pthread functions are still in a separate `libpthread.so.0`,
  so the module NEEDs it. It doesn't on 2.34+. The image has `/lib/x86_64-linux-gnu/libpthread.so.0`
  → `libpthread-2.31.so`, and `bf2` NEEDs it too, so nothing else is required.
  `-static-libstdc++ -static-libgcc` keep libstdc++ out of `NEEDED`, as before.
- One toolchain now builds both halves. The WSL module (g++ 11) is not used in Docker.

## Image

`testenv/Dockerfile` and `run-docker.sh build` bake in:

| File in the image (before `run.sh` moves `tmp/srv/*` to `/home/bf2/srv`) | Content |
|---|---|
| `bin/amd-64/libserverext.so` | the module, beside `bf2` (dlopen search path) |
| `mods/bf2/openbf2.con` | `openbf2.addModDll serverext` |
| `serverext.cfg` | `botListen 0.0.0.0:16702` and `watchdogTicks 6` |

- `BOT_PORT` (default 16702) sets both the baked `botListen` port and the `start` publish
  `-p 127.0.0.1:16702:16702/tcp`. The script refuses the reserved ports for it, as for the others.
- `SERVEREXT_CFG_EXTRA` (at `start`) appends `;`-separated lines to `serverext.cfg` inside the
  container on its first start. This uses one line added to the image's `run.sh`, in the
  first-start block after `chmod`. One image then serves both `allowRoundRestart` runs. The baked
  file holds only `botListen` and `watchdogTicks`, so an appended key never repeats a baked one.
- `status` also prints the deployed `openbf2.con`, the `serverext.cfg` and the module's `loaded:`
  or refusal line.
- New `testenv/rcon.py`: a stdlib ModManager RCON client (digest login), used for the map change.
  It refuses 4711 and 4721.

## Stage results

Two containers, both made by `run-docker.sh start` from the same image. **A**
(`allowRoundRestart 0`) was up 18 min 50 s and ran runs 1-9. **B** (`SERVEREXT_CFG_EXTRA="allowRoundRestart 1"`)
ran run 10. Both took the production path `run.sh` → `su bf2` → `start.sh` → `bf2` → `BF2::init` →
`BF2Engine::startGame` → `GameServer` on `dalian_2_v_2` (`gpm_cq` 16), with the content's own
settings except the documented deviations (`sv.internet 0`, `sv.punkBuster 0`, `start.sh`,
`mm_webadmin` → 127.0.0.1:4303). No game client joined. In AGENTS.md's vocabulary, this shows
production startup, socket-listener reach and the module's bot channel. It does not show
loading-screen, joined-player or playable-client reach.

| Stage | Result |
|---|---|
| Process start, Python and ModManager | Pass. 6 modules, RCON on 0.0.0.0:4711 in the container, one expected `WebAdmin failed to connect ... 127.0.0.1:4303` |
| Module load | Pass, before Python init: `loaded: protocol v1, bot channel 0.0.0.0:16702, watchdog 6 ticks, enterRange 30.0 m, spawnFallback 5 s, spawnBlocked 15 s, allowRoundRestart 0; tick hook 1 (event 1), unload hook 1`. B printed `allowRoundRestart 1` |
| Sockets | Pass. UDP 16571 and 29903 bound. A GameSpy query from Windows to 127.0.0.1:29903 got 1099 bytes (`Dalian_2_v_2`, `gpm_cq`). Windows TCP to 127.0.0.1:16702 reaches the module. RCON login from Windows works |
| 1. Normal spawn and hover (60 s) | **Pass**: `Welcome` v1 at tick 650. The bot spawned on the pad, `EnterVehicle(0)` was acked, and the hover held: `HOVER HELD 60 s; max altitude error 2.99 m; horizontal drift 8.4 m; health 1.00` |
| 2. Blocked pad and fallback (`blockpad`) | **Pass 3/3** twice. Run 2 needed 4 standing bots, because two of them were killed on the pad 12 s after run 1 left its Z10 pilotless in a hover about 25 m away (see below). Run 3, on a settled pad: `a bot fell back after 3 standing bots`, `group 3 -> 4 (enter-on-spawn)` after 152 ticks, seated in Z10 2065 seat 0, alive and seated after 10 s. Same as WSL |
| 3a. All blocked, `RestartRound` refused (A) | **Pass 4/4**: `SpawnBlocked` after 15 s and again at 25 s, `RestartRound` refused with `Error` 6 |
| 3b. All blocked, `RestartRound` allowed (B) | **Pass 6/6**: two fallback bots in the Z10's seats 0 and 1, then `tried every spawn group` at tick 671, `SpawnBlocked` at 971 and 1271, `round restarted on request (admin.restartMap)` at 1302, the second request refused (`at most one round restart per 60 s`), and all six bots respawned. As in WSL, five bots spawned on the three pad points in one tick (retail behaviour) |
| 4. Walking | **Pass 9/9**: out-of-range `EnterVehicle` refused (`Error` 5), strafe +0.6 for 1 s gave +1.32 m right, look yaw +0.5 for 1 s gave +77.5°, walked 29.0 m to AH-1Z 1797 and arrived 6.9 m away after 6.2 s, entered seat 0 |
| 5. Buttons | **Pass 6/6**: fire hit the empty Z10 (1.0000 → 0.9571), F2 moved the bot to seat 1. Camera views logged 1→2→3→1→3→4→5→1, identical to WSL |
| 6. Fuzz | **Pass 41/41**, `server still ticking: 91 TickStates in 3 s, 29.8 ticks/s` |
| 7. Map change | **Pass**: while the bot hovered, `rcon.py` sent `exec admin.nextLevel 0` and `exec admin.runNextLevel` from Windows. Client: `BotRemoved player 255 reason 2`, recreated, re-entered, `hover reached` 13 s later, 4,483 `TickState`s in 150 s with 0 tick gaps. Server: `level unload` (twice, same tick), `bot 255 no longer exists (level unload)`, vehicle set rebuilt (see the first difference below) |
| 8. 10-minute hover soak | **Pass**: `HOVER HELD 600 s; max altitude error 2.99 m; horizontal drift 8.4 m; health 1.00`, 607 `VehicleList`s in about 616 s |
| `[FRONTIER]`, abort, segfault | **0** in `docker logs` and **0** in `/volume/bf2.log`, for both containers (A: 133 KB and 99 KB, B: 7 KB and 8 KB). No core file in `/home/bf2/srv`. Nothing mentions the retail `libaidll.so` that the image keeps in `bin/amd-64` |
| Shutdown | `docker stop -t 15` ends in SIGKILL after 16 s (exit 137). That is the image's known `sh -c ./run.sh` PID 1 limitation (test-env.md), and the module adds nothing to it |

### Soak sampler and RSS

`soak-docker.sh 11`, running in parallel with run 9 (UTC times; bf2's elapsed time, CPU time and RSS):

```
17:21:25 etime/cpu/rss=07:31 00:00:10 142776 udp_sockets=2 fps_redraws/last=87 Average FPS:35
17:26:26 etime/cpu/rss=12:32 00:00:16 142776 udp_sockets=2 fps_redraws/last=144 Average FPS:36
17:32:27 etime/cpu/rss=18:33 00:00:23 142776 udp_sockets=2 fps_redraws/last=213 Average FPS:35
frontier/abort lines: bf2.log=0 stderr=0
```

RSS was **142,776 KB at every sample** over 11 minutes, and also at 18 min 50 s. The status screen
redraws grew by about 11.5 per minute, and over the whole container life the "Average FPS" lines
were 193 × 36, 23 × 35 and one each of 27 and 1 (startup). B's RSS was 143,340 KB after 76 s.

### Per-tick cost

These are the module's own stats lines (handler time, 10 µs buckets, one per 1,800 ticks). Container A,
32,400 ticks:

| Window | p50 | p99 | max |
|---|---|---|---|
| Soak, 1 bot hovering (ticks 14,400-32,400) | 90-100 µs | 140-170 µs | 214-641 µs, except one 1,346 µs and one 1,180 µs |
| Windows with `CreateBot` (scenarios, map change) | 80-90 µs | 130-230 µs | 1.6-2.8 ms |
| Whole process | 90 µs | 160 µs | 2,800 µs, 0 ticks over 20 ms |

## Differences from the WSL results (module-v1.md)

1. **The vehicle set rebuild after a map change found 0 vehicles, where WSL found 4.** The two
   `level unload` events landed in one tick (10354). The full walk at 10355 saw 15,284 registered
   objects but no vehicle among them. The 500-per-tick slice scan then added all four helis at
   tick 10385, exactly one slice cycle later, and then Z10 2065 at 10851. So right after a map change
   a `VehicleList` can be empty for up to about 1 s. That stays inside the contract's "new vehicles
   appear within about a second", and the client didn't notice, because it recreates its bot about
   2 s after `BotRemoved`. It looks like timing: whether the walk runs before the new level's objects
   qualify as vehicles. This run doesn't show why. It is not proven to be Docker-specific, and it
   could happen in WSL too.
2. **Per-tick cost is about 20 µs higher.** Soak p50 is 90-100 µs against WSL's 70-80, and p99
   140-170 µs against 110-150. Possible causes are the g++ 10 vs 11 build or the container/VM
   scheduling; both are only guesses. The maximum is unchanged, at 2.4-2.8 ms on `CreateBot` ticks, and no tick passed 20 ms.
3. **RSS:** 142,776 KB, flat, against WSL's 142,416 → 142,420 KB. Same size and same flatness.
4. **Everything behavioural matched:** spawn ticks, the 152-tick fallback, the `SpawnBlocked`
   timing, the camera view sequence, the walking numbers (77.5°, 6.2 s, 6.9 m), fuzz 41/41, and the
   hover numbers (2.99 m; 8.4 m drift against 8.3 m). Small differences: strafe 1.32 m against 1.27 m, and fire
   damage 0.9571 against "0.96".
5. **Run 2's 4-bot blockpad** came from test ordering, not from Docker. Run 1 ends by removing its
   bot in a 12 m hover, and the abandoned Z10 came down about 25 m from the pad. Twelve seconds
   later two of run 2's standing bots died on the pad and the in-heli group was briefly inactive.
   The re-run on a settled pad matched WSL. Future sequences should let the pad settle after a hover
   run, or land first.
6. **Where the logs are:** in Docker the module's `[serverext]` lines, like `[FRONTIER]`, go to
   stderr, which is `docker logs`. `/volume/bf2.log` holds only the status screen. In WSL both were
   in `bf2.log`.
7. **The bot channel binds 0.0.0.0 in the container**, published on the host's 127.0.0.1:16702
   only. That is the form the contract's "Loading" allows. WSL bound 127.0.0.1:16701.

## What is wrong or missing in protocol.md and test-env.md for Docker

Not edited here, per CLAUDE.md. The coordinator or the author decides.

1. **protocol.md "Testing → Ports"** gives the bot channel as `127.0.0.1:16700`. On this machine
   16700 is held by a stale `wslrelay`, and every module run has used its own port (WSL v1 16701,
   Docker 16702). The list should give a range or say "an unused port, e.g. 16700-16709".
2. **protocol.md "Open questions" footer:** "Still unexercised at runtime: … and the Docker host."
   The Docker host is now exercised for v1 (this file). "Testing" still says no client has joined.
   That is still true.
3. **protocol.md "Where to run the server → Docker"** is right, but it doesn't say the module is
   baked into the image or how `serverext.cfg` is set. See "Image" above.
4. **test-env.md "Option B"** must now pass the module too. With non-default build dirs:
   `BIN_DIR=<out>/build/bin_linux MODULE_DIR=<out>/module run-docker.sh build`. The defaults match
   `build-bullseye.sh`'s default output dir, so the plain commands still work.
5. **test-env.md "Corrections" 5** (`botListen` in Docker) is settled by the contract's current
   wording and works as worded.
6. **test-env.md "Changes applied to the production content"** and "`openbf2.addModDll`" now hold
   for Docker as built: `openbf2.con` is at `/home/bf2/srv/mods/bf2/openbf2.con` (it doesn't move
   with `settings`) and `serverext.cfg` is at `/home/bf2/srv/serverext.cfg`, both root-owned 0644
   and readable by the `bf2` user.
7. **`/volume` is not a Docker volume in this image** (the image config declares none). It is a
   directory inside the container, and `run-docker.sh rm` deletes it with the logs. Copy `bf2.log`
   out before removing a container. Production may mount it; that is not checked here.
8. **New Windows-side gotcha:** Git Bash rewrites container paths in `docker exec ... /volume/bf2.log`
   to `C:/Program Files/Git/volume/...`. Use `MSYS_NO_PATHCONV=1`, or run from WSL or PowerShell.
   test-env.md's `wsl.exe` `$`-expansion gotcha also hit here. Scripts in files avoid both.

## What blocks deploying to the real game-bf2 VM

This is for the author to weigh. Nothing was attempted.

1. **bf2hub and GameSpy:** production starts `start_bf2hub.sh`, which `LD_PRELOAD`s the retail
   `libbf2hub.so` built for the retail binary. This image runs `start.sh` with `sv.internet 0`. A
   public OpenBF2 server needs the rebuilt bf2hub module (the author's `openbf2-2v2-nocheck`
   image loads it through `openbf2.con`). In that setup `openbf2.con` would hold two `addModDll`
   lines, and **bf2hub and serverext loaded together are untested**.
2. **PunkBuster:** `sv.punkBuster 0` here. PunkBuster with an OpenBF2 binary is untested.
3. **The engine is a local branch:** the module needs `bf2` built from OpenBF2 `serverext/r1`
   (`540fa88`, R1/R1b plus the v1 `IServerExtension` additions, local-only and marked
   `[CHOSEN, not recovered]`). Any deployed `bf2` must carry those changes, and the module must be
   rebuilt against the same headers.
4. **No game client has joined** any OpenBF2 server with the module. Joined-player and
   playable-client reach, Q4's client stage and Q9 (`mm_webadmin` and the netsky scripts with
   `ai=1`/`connected=0` players) are all open. On a public server, real players would meet the bots.
5. **The bot channel has no authentication.** It is safe only when it is published on the VM's
   loopback (`-p 127.0.0.1:<port>:<port>`) and the sidecar runs on the same VM. Publishing it
   anywhere else exposes bot control and `RestartRound` (if enabled).
6. **The admin channel** for BF2WebAdmin is a draft and isn't implemented, so `mm_webadmin` remains
   the admin path on a deployed server.
7. **Shutdown is SIGKILL** (exit 137), as with the current production image. The module has no
   state to flush, so this blocks nothing new.

Not a blocker: glibc. Everything is built for and runs on the image's 2.31.

## State left behind

- Both test containers are stopped and removed. `serverext-testenv:latest` (`9efc428ced5a`) is kept.
- Ports 16571, 29903, 4723 and 16702 are free. No WSL server was started.
- Build output in WSL is at `~/bf2-linux-serverext-docker-v1`. Client and container logs are in
  `%TEMP%\serverext-docker-v1\` (`run01`-`run10`, `containerA-*.log`, `containerB-*.log`,
  `soak-docker.log`).
