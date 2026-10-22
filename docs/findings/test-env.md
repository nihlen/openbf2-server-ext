# Test environment: OpenBF2 server on the netsky `dalian_2_v_2` content

Status: verified on 2026-10-21/22 (UTC) by a worker thread. Both hosting options from the contract
work up to an empty server ticking on `dalian_2_v_2`. **Chosen: WSL** for the edit-build-run loop,
with Docker kept as the production-shaped check. No OpenBF2 source was changed. No client has
joined yet; that is the author's step (see [Joining with the game client](#joining-with-the-game-client)).

Helper scripts are in `testenv/`. All of them take ports and paths from arguments or environment and
refuse 16567, 16569, 29900, 29901, 4711 and 4721.

| Script | Purpose |
|---|---|
| `tools-linux.sh` | Installs cmake 3.30.5, ninja 1.12.1 and the libncurses-dev headers into `~/tools`. No root needed. |
| `build-linux.sh` | Builds OpenBF2 `linux-x64` (`bf2`, `libdice_py.so`) in WSL Ubuntu. Output needs glibc 2.35. |
| `build-bullseye.sh` | Builds the same targets in a throwaway `debian:bullseye` container. Output needs glibc 2.29, so it runs on the netsky image. |
| `prepare-content.sh` | Extracts the netsky content into a run directory, fills the `{{...}}` placeholders and swaps in the OpenBF2 binaries. |
| `run-wsl.sh` | Runs the WSL server: `start`, `stop`, `status` or `log`. |
| `Dockerfile`, `run-docker.sh` | Builds the `serverext-testenv` image (FROM the netsky image) and runs it: `build`, `start`, `stop`, `status`, `logs` or `rm`. |
| `soak-check.sh`, `soak-docker.sh` | Sample a running server once a minute. |
| `udp-probe.ps1` | Sends a GameSpy query from Windows to a server's query port. |

## Versions and inputs

| Item | Value |
|---|---|
| OpenBF2 | worktree `C:\Users\Alex\.t3\worktrees\OpenBF2\serverext-test-env`, branch `serverext/test-env`, commit `ccf30993258bfb9638db4aa7115c8a4c1a456e55`, clean |
| Content image | `cr.nihlen.net/library/bf2-netsky:2v2-nocheck`, image id `sha256:62c880556ef2…`, repo digest `sha256:49660f21e9ed…`, created 2026-01-03. Debian 11.11, glibc 2.31. |
| WSL | WSL 3.0.3.0, kernel 6.18.54.1, distro `Ubuntu` 22.04.1, g++ 11.4.0, NAT networking (no `.wslconfig`) |
| Bullseye build | `debian:bullseye`, g++ 10.2.1, glibc 2.31, apt from `archive.debian.org` |
| Docker | Docker Desktop, engine 29.0.1 |
| Build dirs | `~/bf2-linux-serverext-testenv` (WSL build), `~/bf2-linux-serverext-testenv-bullseye` (bullseye build) |
| Run dir | `~/serverext-testenv/srv` (WSL). Logs from this work are in `~/serverext-testenv/` (`bf2-run1.log`, `bf2-probe-run.log`, `soak-wsl.log`, `soak-docker.log`). |
| Image / container | `serverext-testenv:latest` / `serverext-testenv` |

Both builds link strictly, and `dice_py_check_run` passed in both ("zlib roundtrip and loopback UDP
send/receive ok … python runtime check complete").

## Reproduce

Run these from WSL. Set the two paths once:

```bash
T=/mnt/c/Data/Projects/openbf2-server-ext/testenv
SRC=/mnt/c/Users/Alex/.t3/worktrees/OpenBF2/serverext-test-env
sed -i 's/\r$//' $T/*.sh          # only if a Windows checkout gave the scripts CRLF endings
```

### Option A: WSL (chosen)

```bash
bash $T/tools-linux.sh                                   # once; or: sudo apt install cmake ninja-build libncurses-dev
bash $T/build-linux.sh $SRC ~/bf2-linux-serverext-testenv
bash $T/prepare-content.sh ~/serverext-testenv/srv ~/bf2-linux-serverext-testenv   # prints the RCON password
bash $T/run-wsl.sh start                                 # then: status | log | stop
bash $T/soak-check.sh 6                                  # optional 6-minute sample
```

`prepare-content.sh` recreates the run directory from the image on every run, so re-running it
discards local edits there. To refresh only the binaries after a rebuild, copy
`~/bf2-linux-serverext-testenv/bin_linux/{bf2,libdice_py.so}` into `~/serverext-testenv/srv/bin/amd-64/`.

### Option B: Docker

```bash
bash $T/build-bullseye.sh $SRC                           # a full build takes about 3 minutes on this machine
bash $T/run-docker.sh build                              # serverext-testenv:latest, FROM the netsky image
bash $T/run-docker.sh start                              # then: status | logs | stop | rm
bash $T/soak-docker.sh 6
```

`start` uses `docker run --name serverext-testenv`. After a `stop`, run `rm` before the next `start`.
`docker stop` ends in SIGKILL after 15 s (exit 137) because the image's PID 1 is `sh -c ./run.sh`,
which does not forward SIGTERM. The production image has the same limitation.

### Changes applied to the production content

Both options make these changes and nothing else:

- The netsky placeholders are filled as follows: `sv.serverPort 16571`, `sv.gameSpyPort 29903`,
  RCON 4723 on 127.0.0.1, `sv.maxPlayers 16`, and server name `serverext test`.
- `sv.internet 0` and `sv.punkBuster 0`. A private test server must never register with a master
  server.
- `mm_webadmin.serverHost/serverPort` are set to `127.0.0.1:4303`, where nothing listens. That keeps
  the test server out of the BF2WebAdmin local test, which listens on 4300. ModManager logs one
  `Error: WebAdmin failed to connect to server socket 127.0.0.1:4303 ((111, 'Connection refused'))`
  and carries on.
- The server is started with `start.sh` instead of `start_bf2hub.sh`. The latter `LD_PRELOAD`s the
  retail `libbf2hub.so`, which is built for the retail binary.
- In the WSL run directory the retail `bf2` and `libdice_py.so` are kept in `bin/retail-amd-64/`
  for reference.

## Stage results

Every run used the real `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`
path with no arguments, so the server read the image's `mods/bf2/settings/{serversettings,maplist}.con`.
No `BF2_*` harness variables were set. No client connected.

| Stage | WSL (run 1, `run-wsl.sh`) | Docker (`serverext-testenv`) |
|---|---|---|
| Process start | Pass | Pass |
| Python and mod init | Pass. ModManager v2.2 initialised 6 modules, including `mm_rcon`, `mm_webadmin` and `mm_bf2cc`. RCON listens on 127.0.0.1:4723. | Pass. The same modules initialised and RCON listens on container port 4711, published as 127.0.0.1:4723. |
| Map load of `dalian_2_v_2` | Pass. Status went from `[pregame]` to `[playing]` within about 5 s. | Pass, same as WSL |
| Socket on 16571 | Pass. `ss`: `bf2` owns UDP 0.0.0.0:16571 and 0.0.0.0:29903. The GameSpy reply reports `hostport=16571`. | Pass. Both ports bound per `/proc/net/udp`, published on 0.0.0.0. |
| Ticking ≥ 5 min | Pass for **9 min 39 s**: 114 status-screen redraws, steady "Average FPS:35", 6 s CPU, RSS about 142 MB | Pass for **6 min 50 s**: redraws rose by about 12 per minute (10 → 81), "Average FPS:35–36", RSS about 138 MB |
| `[FRONTIER]`, abort or segfault lines | 0 | 0 in `bf2.log` and 0 in `docker logs` |
| Stop and immediate restart | SIGTERM stops the server within 10 s. The next start rebinds 16571 and 29903 at once (runs 2, 3 and the foreground probe run). | Not measured, since each start makes a new container |

Log excerpts (ANSI codes stripped; the ncurses status screen writes to the same stream):

```
Could not find platform independent libraries <prefix>          # stderr, both options; Python 2.3 notice, harmless
Could not find platform dependent libraries <exec_prefix>
Consider setting $PYTHONHOME to <prefix>[:<exec_prefix>]
[TEMP][ANIMSWEEP] ECCore 3 posted by GameServer::loadPath (path=Levels/dalian_2_v_2/)
[TEMP][ANIMSWEEP] handleEvent #1: 172 templates registered, 172 re-read their script.
[TEMP][ANIMSWEEP] postLoad #1: 3327 registered, 0 swept, 3327 to load.
Battlefield 2 Dedicated Server v1.5.3153-802.0(x86-64) "serverext test" Average FPS:35 [d:0, o:9]
IP: 0.0.0.0 Port: 16571 (LAN)  Map: dalian_2_v_2  Game mode: gpm_cq/16  Mod: bf2
Players: 0/16 (0 r) (0 connecting)  Round: 1/99  Status: [playing]
```

```
# modmanager.log
Info: Starting ModManager Rcon v8.5 on 127.0.0.1:4723
Info: ModManager WebAdmin started
Info: Initialised 6 modules
Info: Validating maplist...
Info: No maplist changes required
```

```
# soak-check.sh, WSL run 1 (UTC)
23:28:12 09:39 00:00:06 142204 udp16571=1 fps_redraws=114 last=35 mapname=Dalian_2_v_2 gametype=gpm_cq gamemode=openplaying numplayers=0 hostport=16571
frontier/abort lines in bf2.log: 0
# soak-docker.sh
23:31:19 etime/cpu/rss=00:49 00:00:02 142648 udp_sockets=2 fps_redraws/last=10 Average FPS:35
23:37:20 etime/cpu/rss=06:50 00:00:04 137920 udp_sockets=2 fps_redraws/last=81 Average FPS:36
frontier/abort lines: bf2.log=0 stderr=0
```

The server also binds UDP 55124 and 55126 for VoIP (`sv.voipEnabled 1` with a local VoIP server).
Two WSL test servers at once would collide on those ports. Docker does not publish them.

What this does not show: a client connecting, joining, spawning or playing; a map change or reload;
RCON commands beyond the login banner; the bot channel; and loading any module.

## `openbf2.addModDll`: which file and line

- **File:** `mods/bf2/openbf2.con` in the server root. In WSL that is `~/serverext-testenv/srv/mods/bf2/openbf2.con`.
  In Docker it is `/home/bf2/tmp/srv/mods/bf2/openbf2.con` in the image, which `run.sh` moves to
  `/home/bf2/srv/mods/bf2/`. The netsky content does not have this file; create it.
- **Line:** `openbf2.addModDll serverext`
- **Where the module file goes:** `bin/amd-64/libserverext.so`, beside `bf2`. The name has no slash,
  so `dlopen` searches the executable's `RUNPATH $ORIGIN` and `start.sh`'s
  `LD_LIBRARY_PATH=bin/amd-64`, not the working directory. The loader tries `libserverext.so` first,
  then `serverext.so` (`Code/BF2/Game/Main/OpenBF2Console.cpp:110-139`). For Docker the module must also be built by
  `build-bullseye.sh`'s toolchain (glibc ≤ 2.31). Add it to the image with
  `COPY libserverext.so /home/bf2/tmp/srv/bin/amd-64/` plus a `COPY` of `openbf2.con`.
- **Runs before Python init:** `GameServer::init` runs `<GSModDirectory>/openbf2.con` through
  `runFullAccess` at `Code/BF2/Game/GameServer/GameServer.cpp:262-276`. It then calls
  `python::pythonHost()->initialize` at `:279` and `initializeAdminMod` (ModManager) at `:306`.
  `addModDll` is `openBf2AddModDll` at `Code/BF2/Game/Main/OpenBF2Console.cpp:184`, and `initDll` is
  called at `:239`. A missing file is not an error.
- **Checked at runtime without loading anything:** for one run, `openbf2.con` held
  `openbf2.addModDll serverext_probe_missing`, a name with no matching file. During `[pregame]`,
  before the map load and before ModManager came up, the console showed
  `openbf2.addModDll: could not load 'serverext_probe_missing' (tried libserverext_… error: serverext_probe_missing.so: cannot open shared object file: No such fi…`
  (truncated by the status screen), and the server carried on to `[playing]`. The probe file was
  then removed, and the run directory is back to the unmodified content.
- **Why not `ServerSettings.con`:** in this content, `serversettings.con` is a template that
  `run.sh` fills from `ENV_*` and moves to the `/volume/settings` volume. `openbf2.con` is also
  what the author's existing `bf2-netsky:openbf2-2v2-nocheck` image uses: it contains
  `openbf2.addModDll "bf2hub"` and runs `start.sh`.

## Q6: can a Windows client reach the server?

All probes ran from Windows against my own test servers only.

| Path | UDP 29903 (GameSpy query from Windows) | TCP (RCON banner) |
|---|---|---|
| WSL server, Windows → `127.0.0.1` | **Fails**: ICMP port unreachable ("forcibly closed by the remote host") | **Works**, even though the WSL listener is bound to `127.0.0.1` |
| WSL server, Windows → WSL VM IP (`172.22.252.155` at test time) | **Works**: 1065-byte reply with `hostport=16571` | not tested |
| Docker, Windows → `127.0.0.1` | **Works**: 1074-byte reply | **Works** (127.0.0.1:4723 → 4711) |
| Docker, Windows → LAN IP `192.168.1.157` | **Works** | n/a (RCON is published on 127.0.0.1 only) |

Answer:

- **WSL in NAT mode:** localhost forwarding carries TCP only. A Windows client can still join over
  UDP by targeting the WSL VM's address. Get it with `wsl -d Ubuntu hostname -I`. It changes when
  WSL restarts. No firewall rule or mirrored networking was needed.
- **Mirrored networking was not tried and is not recommended.** It needs a `.wslconfig` change and
  `wsl --shutdown`, which also restarts Docker Desktop's WSL backend and every container on it.
- **Docker published UDP works on 127.0.0.1.** By default it is also reachable from the LAN, as the
  BF2WebAdmin local test is. Set `PUBLISH_IP=127.0.0.1` to keep it local.
- **The game port 16571 itself was not probed.** It answers only the client handshake. The GameSpy
  round trip shows that the UDP path between Windows and that host works in both directions.
- **For the bot sidecar on Windows:** TCP to `127.0.0.1:<botListen port>` reaches a WSL module that
  binds loopback, as the contract requires. Docker is different; see the next section.

### Joining with the game client

Use a retail 1.50 client. The server is "nocheck", so a modded client is fine. The in-game LAN
browser will not list the WSL server, because the VM is on a different subnet. Join directly:

```
# WSL: start it, then read the address
wsl -d Ubuntu -- bash /mnt/c/Data/Projects/openbf2-server-ext/testenv/run-wsl.sh start
wsl -d Ubuntu hostname -I                               # e.g. 172.22.252.155
"<BF2 dir>\BF2.exe" +restart 1 +joinServer 172.22.252.155 +port 16571 +playerName <name>

# Docker
"<BF2 dir>\BF2.exe" +restart 1 +joinServer 127.0.0.1 +port 16571 +playerName <name>
```

`+joinServer`, `+port`, `+playerName` and `+restart` are the engine's own switches
(`Code/BF2/Game/Main/BF2Engine.cpp:773-791`). Report the stages separately: connect, loading
screen, join, spawn, and play in a heli.

## Windows `bf2_w32ded` as a fallback

Not built, because the Linux path works. Assessment:

- **Prerequisites present:** Visual Studio 2022 and 18 with the x86/x64 VC tools (found by `vswhere`),
  and `Code/Build/build.cmd`.
- **Prerequisite missing:** the Python runtime has not been built
  (`Code/BF2/External/python-embedded/OBJ/python/release` does not exist), so
  `build_python.cmd x86` is needed first.
- **Diagnostic link only:** `bf2_w32ded` is a `/FORCE` link, so unresolved calls can fail only when
  they run. `bf2_w64ded` (`msvc-x64`) is a strict link and the better Windows fallback.
- **Content:** the netsky tree is a Linux server layout (`bin/amd-64`, `pb_amd-64`, `start*.sh`).
  `mods/` and `admin/` are platform-neutral, so the content root should work, but this is untested.
- **Verdict:** usable for compiling the module on Windows (`serverext.dll`), but a weaker runtime
  host than either Linux option. It gives no production parity and does not avoid the client-join
  question.

## Why WSL over Docker

Both passed every stage. WSL is the better day-to-day host for this project:

1. **Bot channel:** the contract says `botListen` defaults to `127.0.0.1:16700`, "loopback only in
   v0". In WSL that works as written, because Windows reaches a WSL loopback TCP listener through
   localhost forwarding (shown with RCON). In Docker, the container's 127.0.0.1 cannot be reached
   from the host. The module would have to bind `0.0.0.0` inside the container, published as
   `127.0.0.1:16700:16700`, which contradicts the contract as worded.
2. **One build:** the WSL build serves both `bf2` and `libserverext.so`. Docker needs a second,
   glibc-2.31 toolchain (`build-bullseye.sh`) for both the server and the module.
3. **Diagnostics:** `[FRONTIER]` output goes to stderr. In WSL it lands in `bf2.log`, and core
   dumps and `gdb` are available directly. In Docker, stderr goes to `docker logs` and the status
   screen to `/volume/bf2.log`.

Docker remains the better check of production shape, and the easier host for the author's client:
it joins at `127.0.0.1:16571` with no IP lookup. Run it before trusting any behaviour that depends
on the image (Debian 11 runtime, `run.sh`, volumes).

## Corrections to the contract

These cover "Testing" and the parts of "Loading" that the tests touch.

1. **The WSL build cannot replace the binary in the netsky image.** "Server binary replaced by the
   OpenBF2 Linux build" does not work with the WSL Ubuntu 22.04 build: it needs glibc 2.35 and the
   image has 2.31. Build in Debian 11 (`build-bullseye.sh`), and build `libserverext.so` the same
   way for Docker. Also, the "server binary" is a pair: `bf2` and `libdice_py.so` must be replaced
   together.
2. **Use `start.sh`, not `start_bf2hub.sh`.** Both the WSL and Docker setups must avoid the
   image's `start_bf2hub.sh`, which `LD_PRELOAD`s the retail `libbf2hub.so` into OpenBF2.
3. **Loading:** for this content the file is `mods/bf2/openbf2.con`, and the module goes in
   `bin/amd-64/` (dlopen search path), not the working directory. `serverext.cfg` "in the server's
   working directory" is `~/serverext-testenv/srv/` in WSL and `/home/bf2/srv/` in Docker.
4. **Q6 premise:** the contract's guess is half right. Localhost forwarding is TCP-only, but UDP
   does not need mirrored networking: the WSL VM IP works directly. Recommend against mirrored,
   because switching restarts Docker Desktop.
5. **`botListen` in Docker:** loopback-only is unreachable from the host when the module runs in a
   container. Either test the bot channel only in WSL, or let the contract allow a configurable
   bind address with Docker publishing it as `127.0.0.1:16700`. This is the coordinator's call.
6. **Ports the contract does not list:** VoIP UDP 55124 and 55126 are bound by the server.
   `mm_webadmin` is an outbound connection that by default would target the BF2WebAdmin local
   test's 4300; redirect it (4303 here).
7. **Required config deviations:** `sv.internet 0` and `sv.punkBuster 0`. Otherwise the test server
   would try to register publicly and use PunkBuster.
8. **Docker publishes on all interfaces by default.** UDP on 0.0.0.0 is reachable from the LAN.
   Use `PUBLISH_IP=127.0.0.1` when that is not wanted.

## Gotchas met

- **`wsl.exe -- bash -c '…'` expands `$` before bash does.** It goes through a shell first:
  `wsl -- bash -c 'false; echo $?'` prints `0`. Put logic in script files, or write `\$`.
- **The background server needs time to detach.** If `wsl.exe` returns before it does, the server
  is killed with the session. `run-wsl.sh` uses `nohup setsid` and checks the process after 3 s.
- **`apt-get` in `debian:bullseye` now 404s on `deb.debian.org/debian-security`.** Use
  `archive.debian.org`, as `build-bullseye.sh` does.
- **No sudo in WSL.** cmake and ninja are tarballs in `~/tools`. The ncurses headers come from
  `apt-get download libncurses-dev` plus `dpkg -x`, linked against the runtime
  `libncursesw.so.6` and `libtinfo.so.6` through `CURSES_*` cache overrides.
- **An OpenBF2-based netsky image already exists:**
  `cr-ts.nihlen.net/library/bf2-netsky:openbf2-2v2-nocheck`, from 2026-10-19. Its base is Debian
  trixie (`php8.4-fpm`), it has its own `bf2` from 2026-10-14 and a rebuilt `libbf2hub.so` loaded
  through `openbf2.con`, and it runs `start.sh`. It shows the author's intended layout. It is not
  the `2v2-nocheck` production image and was not run here.

## State left behind

- **No server is running.** The WSL server was stopped with SIGTERM. The `serverext-testenv`
  container was stopped and removed; the `serverext-testenv:latest` image remains.
- **To start again:** `bash $T/run-wsl.sh start`, or `bash $T/run-docker.sh start` (run
  `bash $T/run-docker.sh rm` first if an old container exists).
- **The BF2WebAdmin local test containers (`bf2wa-localtest-bf2`, `bf2wa-localtest-mssql`) no longer
  exist, and I did not remove them.** `docker events` shows a compose-style
  `kill → stop → destroy → network destroy` of the whole `bf2wa-localtest` project at 23:16:32Z. My
  only Docker action near then was removing my own `serverext-testenv-extract` container at
  23:16:13Z. Unnamed containers I did not create (`fervent_benz`, and `vibrant_morse`, which was
  `docker export`-ed) appear at 23:17. Someone else is using Docker concurrently. I left it as found.
