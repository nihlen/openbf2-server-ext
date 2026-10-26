# How the pieces fit: OpenBF2, the server extension, the bots and WebAdmin

This is the one guide to the whole system: which repo holds what, how data flows, how to build each piece, how the
local test environment (the "localtest") is assembled from them, and how to rebuild, redeploy and test after a change
in any one of them. It links to the per-repo documents for the details. If something here disagrees with the code or
the scripts, the code wins; fix this file.

State as of 2026-10-26.

## Repos and checkouts

Work in the default checkouts below. Older worktrees under `C:\Users\Alex\.t3\worktrees\` and the branches
`serverext/r1` (OpenBF2) and `serverext-sidecar` (bf2-bots) are history: their commits are in the branches below.
Findings files still name them as the record of where a result was made; do not work there.

| Repo | Path | Branch to work on | What it holds | Instructions |
|---|---|---|---|---|
| OpenBF2 | `C:\Data\Projects\OpenBF2` | `serverext/r1` (checked out; local only, never pushed, **not merged into `main`**, which stays equal to `origin/main`): the 4 commits below and the docs commit `2a2c77e` on top of `ccf3099` | The engine (BF2 1.50 reconstruction). The server-extension additions `IServerExtension`, its observer hooks and the R1b guards are `[CHOSEN, not recovered]` local commits `9e1cda6`, `540fa88`, `74e18d9`, `873d1b1` | `AGENTS.md` (governs everything there); `C:\Data\Projects\OpenBF2\docs\server-extension.md` |
| openbf2-server-ext | `C:\Data\Projects\openbf2-server-ext` | `master` (no remote) | The native module `libserverext.so`: bot channel and admin channel; the contracts; test clients; findings | `CLAUDE.md`, [README.md](../README.md), [protocol.md](protocol.md) |
| bf2-bots | `C:\Data\Projects\bf2-bots` | `main` | The bot sidecar (`bf2bots serverext`), plus the older memory-reader/vJoy agent | `CLAUDE.md`; `C:\Data\Projects\bf2-bots\docs\serverext-backend.md`, `C:\Data\Projects\bf2-bots\docs\serverext-missions.md` |
| BF2WebAdmin | `C:\Data\Projects\DotNet\BF2WebAdmin` | `admin-channel` (checked out; **not merged into `master`**, stays separate for now) | The web admin. On `admin-channel` its game-server backend is the module's admin channel instead of `mm_webadmin.py`. `master` is still the `mm_webadmin` version | `CLAUDE.md`; `C:\Data\Projects\DotNet\BF2WebAdmin\docs\admin-channel.md` |
| BF2WebAdmin-localtest | `C:\Data\Projects\DotNet\BF2WebAdmin-localtest` | not a git repo | Docker compose, scripts and data for a local SQL Server, a BF2 server on `dalian_2_v_2` and WebAdmin; the OpenBF2 variant is this system's test environment | its `C:\Data\Projects\DotNet\BF2WebAdmin-localtest\README.md` |

The author may have uncommitted work in any of them (BF2WebAdmin has some now: `BF2WebAdmin.sln`, `RconScript.cs`,
`BF2WebAdmin.AppHost/`). Never stage, commit, revert or modify files you did not change; commit with explicit paths.

## Data flow

```
 OpenBF2 dedicated server (bf2, in the localtest container)
   mods/bf2/openbf2.con: openbf2.addModDll serverext     (runs in GameServer::init, before Python)
   └─ libserverext.so  ── fetches the IServerExtension singleton "OpenBF2ServerExtension", hooks the tick (30 Hz)
        ├─ bot channel: TCP listener, botListen (0.0.0.0:16705 in the container, published on 127.0.0.1:16705)
        │      ▲  plain TCP, protocol v3 (docs/protocol.md), no authentication: loopback only
        │      └── bf2-bots sidecar  bf2bots.exe serverext --endpoint 127.0.0.1:16705  (Windows host)
        │          or tools/testclient/serverext_client.py
        └─ admin channel: dials OUT to adminConnect (host.docker.internal:4310) over TLS 1.3, certificate pinned
               │  HMAC handshake with adminKey, admin protocol v1 (docs/admin-channel-draft.md, findings/admin-phase1.md)
               ▼
           BF2WebAdmin (admin-channel branch) AdminListener on 4310 (Windows host) ── SQL Server (bf2 database)
           web UI http://localhost:5050 ── tools\hubclient uses the same SignalR hub
 ModManager rcon (mm_rcon, container 4711, published 127.0.0.1:4721) is still there: tools\rcon.py, WebAdmin's
 web rcon box, kick/ban through iga.
```

- The module runs inside the server process. Its socket I/O runs on its own threads; engine work happens once per
  tick on the main thread (protocol.md "Threading rules"). The engine exports no symbols, so the module uses only
  typed virtual calls on engine interfaces and `IServerExtension`.
- The sidecar creates bots (connection-less AI-flagged players) through the bot channel, reads `TickState` and the
  other state messages, and sends inputs, `Say` (chat), `Spot`, `Teleport`.
- WebAdmin never connects to the game server: the module dials WebAdmin. The server row (`Server.ServerId` =
  `adminServerId`, `Server.AdminKey` = `adminKey`) must match `serverext.cfg`; `adminCertSha256` is the pin of
  WebAdmin's certificate (`BF2WebAdmin.Server.exe --admin-pin`).
- Bots are also players to WebAdmin: their chat (`Say`) reaches WebAdmin's Heli2v2 module as a player's chat, which is
  how rounds start with bots (bf2-bots serverext-missions.md "Round flow").

## Build

All builds use agent-owned output directories. Nothing builds on a Windows bind mount.

| Piece | Where | Command | Output | Notes |
|---|---|---|---|---|
| Module (dev loop) | WSL Ubuntu | `cd /mnt/c/Data/Projects/openbf2-server-ext && ./build-module.sh [openbf2-checkout] [build-dir]` | `<build-dir>/libserverext.so`, ctest run (`serverext_tests`, `serverext_admin_tests`) | Default OpenBF2 `/mnt/c/Data/Projects/OpenBF2`, build dir `~/serverext-build/linux-x64`; use your own build dir, e.g. `~/serverext-build/<task>`. Needs `testenv/tools-linux.sh` once. Builds the working tree. Output needs glibc 2.35: runs in WSL, **not** in the localtest container |
| Engine (dev loop) | WSL Ubuntu | `testenv/build-linux.sh /mnt/c/Data/Projects/OpenBF2 ~/bf2-linux-<task>` | `bf2`, `libdice_py.so` | For a WSL test server (`testenv/`). OpenBF2's own gates (MSVC syntax checks etc.) are in its `AGENTS.md` "Validation" |
| Engine + module for the localtest | Git Bash on Windows, Docker Desktop | `bash C:/Data/Projects/DotNet/BF2WebAdmin-localtest/openbf2/build.sh` | image `bf2wa-localtest-openbf2:latest` | Builds both in `debian:bullseye` (glibc 2.31, OpenSSL 1.1.1) from **commits**, `git archive` of `HEAD` of both default checkouts. Uncommitted changes are left out (it says so): commit first. `OPENBF2_REV=`/`MODULE_REV=` pin other commits, `IMAGE=` another tag. Prints the commits and records them (below). OpenBF2 builds incrementally in the docker volume `bf2wa-openbf2-build`; first build takes long |
| Sidecar | Windows | `cd C:\Data\Projects\bf2-bots; dotnet build Bf2Bots.sln` (dev); for the localtest `publish-sidecar.ps1` (below) | `src\Bf2Bots.Cli\bin\...\bf2bots.exe` | .NET 10 SDK (`global.json`), warnings are errors |
| WebAdmin | Windows | `cd C:\Data\Projects\DotNet\BF2WebAdmin; dotnet build src\BF2WebAdmin.Server\BF2WebAdmin.Server.csproj`; for the localtest `setup.ps1` publishes it | `app\` in the localtest | .NET 9 projects. Build projects, not `BF2WebAdmin.sln`: the author's uncommitted sln lists the untracked AppHost |

Order: an engine interface change (`IServerExtension.h`) needs the engine, then the module rebuilt against it; the
module, sidecar and WebAdmin are otherwise independent. The localtest image always builds engine and module together.

Which source is running:

- BF2 server image: `docker image inspect bf2wa-localtest-openbf2:latest --format '{{json .Config.Labels}}'`
  (`serverext.openbf2.commit`, `serverext.module.commit`), or in a running container
  `docker exec bf2wa-localtest-openbf2 cat /home/bf2/srv/serverext-source.txt` (in Git Bash prefix
  `MSYS_NO_PATHCONV=1`). `openbf2\out\source.txt` records the last build of any tag (its `image` line names the tag),
  not necessarily `:latest`. Images built before 2026-10-26 have no record: the current `:latest` (2026-10-24) is
  OpenBF2 `873d1b1` + module `100d53e`.
- Sidecar: `sidecar\source.txt` (written by `publish-sidecar.ps1`; the installed `sidecar\` of 2026-10-24 predates it
  and is bf2-bots `468c3a9`).
- WebAdmin: `app\source.txt` (written by `setup.ps1`: the commit and the uncommitted paths it published). The current
  `app\` (2026-10-23) predates it: it is `admin-channel` from that day, commit unrecorded.

## The localtest

`C:\Data\Projects\DotNet\BF2WebAdmin-localtest`, details in its README. The OpenBF2 variant is assembled like this:

| Part | Made from | By | Runs as |
|---|---|---|---|
| BF2 server | netsky content image `cr.nihlen.net/library/bf2-netsky:2v2-nocheck` with its `bf2`/`libdice_py.so` replaced by OpenBF2's, plus `libserverext.so`, `openbf2.con`, `serverext.cfg` (`botListen 0.0.0.0:16705`, `watchdogTicks 6`) | `openbf2\build.sh` + `openbf2\Dockerfile` | container `bf2wa-localtest-openbf2` (compose `docker-compose.openbf2.yml`, profile `openbf2`); admin keys appended to `serverext.cfg` at the container's first start from `SERVEREXT_CFG_EXTRA` (`.env`'s `ADMIN_KEY`, `ADMIN_CERT_SHA256`) |
| SQL Server | `mcr.microsoft.com/mssql/server:2022-latest`, database `bf2` from `init-db.sql` | `setup.ps1` | container `bf2wa-localtest-mssql` |
| WebAdmin | `..\BF2WebAdmin` **working tree** (branch `admin-channel`) | `setup.ps1 -OpenBF2` (publishes into `app\`, writes `app\Configuration\appsecrets.json`, recreates the BF2 container with the current pin) | `app\BF2WebAdmin.Server.exe` on the host, started by `play-2v2.ps1` (background, pid in `logs\play\webadmin.pid`) or `start-webadmin.ps1` (foreground) |
| Bots | bf2-bots commit (default `HEAD` of `main`) | `publish-sidecar.ps1` (into `sidecar\`, or staged in `sidecar.next\` while bots run) | `sidecar\bf2bots.exe`, started by `play-2v2.ps1` |
| Map data for the bots | `C:\Data\Projects\bf2-bots\data\maps\dalian_2_v_2` (generated, git-ignored, only in that checkout) | bf2-bots `data-pipeline` | read by the sidecar (`--map-dir`) |

`play-2v2.ps1 -Mode Pilot|Gunner|Status|Stop` is the author's play session (a human and a bot against a bot crew).
`-Mode Status` shows what runs. A Start recreates the BF2 container when `:latest` changed, installs a staged sidecar,
starts WebAdmin if nothing listens on 4310, replaces the bots and restarts the round.

### Rebuild and redeploy after a change

First run `powershell -ExecutionPolicy Bypass -File .\play-2v2.ps1 -Mode Status` in the localtest (safe: it starts
and stops nothing; it only deletes a pid file whose process is gone; `test-instance.ps1 -Mode Status` likewise). If the BF2
server, WebAdmin or the bots run, the author may be playing: do not restart, recreate or stop anything of the play
server; test on a separate test instance (next section) and leave the deploy to the author's next Stop/Start. This
PC's execution policy is `AllSigned`: run every localtest script with `powershell -ExecutionPolicy Bypass -File`.

| Changed | Unit tests | Build | Deploy to the play server (only with no session running) | Or test on a test instance |
|---|---|---|---|---|
| Engine (OpenBF2) | OpenBF2 `AGENTS.md` gates; a WSL build | commit in OpenBF2, then `IMAGE=bf2wa-localtest-openbf2:<task> bash openbf2/build.sh` (or `:latest` to deploy) | build `:latest`; the next `play-2v2.ps1 -Mode Pilot` (or Gunner) recreates the container. (`setup.ps1 -OpenBF2` also recreates it) | `test-instance.ps1 -Mode Start -Name <task> -Image bf2wa-localtest-openbf2:<task>` |
| Module | `./build-module.sh "" ~/serverext-build/<task>` in WSL | commit in openbf2-server-ext, then `openbf2/build.sh` as above | as for the engine | as for the engine |
| Sidecar (bf2-bots) | `dotnet test Bf2Bots.sln` | commit in bf2-bots | `publish-sidecar.ps1` (installs into `sidecar\` if no bots run, else stages `sidecar.next\`); the next play Start uses it | `publish-sidecar.ps1 -Out scratch\sidecar-<task>`, run that `bf2bots.exe` against the instance's bot port |
| WebAdmin | `dotnet test test\BF2WebAdmin.Tests\BF2WebAdmin.Tests.csproj` | `dotnet build` the project | `setup.ps1 -OpenBF2` (publishes the working tree into `app\`, **recreates the BF2 container**); then `play-2v2.ps1` Start (or restart WebAdmin by hand) | `dotnet publish ...\BF2WebAdmin.Server.csproj -c Release -o scratch\test-<task>\app`, then `test-instance.ps1 -Mode Start -Name <task>` (the instance runs that publish) |

Notes:

- `setup.ps1` publishes into `app\`, which the play WebAdmin runs from, and recreates the BF2 container: only with no
  session.
- `play-2v2.ps1` Start without `-NoRoundRestart` restarts the round; Pilot/Gunner always replace the bots.
- A staged `sidecar.next\` (none at the moment) is installed by the next `play-2v2.ps1` Start or Stop; the previous
  sidecar goes to `sidecar.old\`.
- `openbf2/build.sh` uses shared state whatever `IMAGE` is: `openbf2\out\`, the docker volume `bf2wa-openbf2-build`
  and a builder container of that name. Run one build at a time: `docker ps --filter name=bf2wa-openbf2-build` must be
  empty first. Remove your own tag when done (`docker image rm bf2wa-localtest-openbf2:<task>`).
- `build-module.sh` builds the working tree, `openbf2/build.sh` only commits: after the WSL tests pass, commit (only
  your files, explicit paths), then build the image.
- Do not pipe the output of `play-2v2.ps1` or `test-instance.ps1` Start: the background WebAdmin inherits the pipe and
  the pipeline only ends when it exits.

### A separate test instance

`test-instance.ps1` runs a second OpenBF2 server, a second WebAdmin and a second database beside the play server, so
a test never disturbs a session. This is how the bot-chat work was verified on 2026-10-24 (database `bf2test`).

```powershell
cd C:\Data\Projects\DotNet\BF2WebAdmin-localtest
powershell -ExecutionPolicy Bypass -File .\test-instance.ps1 -Mode Start -Name <task> [-Image bf2wa-localtest-openbf2:<task>] [-App <WebAdmin publish dir>] [-NoWebAdmin]
powershell -ExecutionPolicy Bypass -File .\test-instance.ps1 -Mode Status -Name <task>
powershell -ExecutionPolicy Bypass -File .\test-instance.ps1 -Mode Stop -Name <task>
```

- Without `-Name` the name is `test`. Container `bf2wa-localtest-openbf2-<task>` (recreated on every Start), game `127.0.0.1:16571/udp`, GameSpy 29903,
  rcon `127.0.0.1:4723`, bot channel `127.0.0.1:16706`, admin channel dialled to `host.docker.internal:4312`.
- WebAdmin from `scratch\test-<task>\app` (copied once from `app\` when missing, so it can be older than `app\`: delete
  it to copy again; or publish your build there, or pass `-App`), web UI http://localhost:5052,
  database `bf2test` (created from `init-db.sql` with server row `127.0.0.1:16571`, the localtest group's
  `ServerModule` and `ServerPlayerAuth` rows copied from `bf2`), log `scratch\test-<task>\webadmin.log`.
- It shares `bf2wa-localtest-mssql` with the play server: Start starts it if needed, Stop never stops it. If it was
  stopped before you started, stop it yourself afterwards (`docker stop bf2wa-localtest-mssql`) unless a play session
  started meanwhile.
- Use `-Name <task>` (with Start, Status and Stop) so your instance gets its own container and `scratch\test-<task>\`
  directory and does not overwrite an earlier instance's app and logs. Two instances at once also need other ports
  (the port parameters); ports must not be 16567 or the play server's. Do not run it together with a WSL test server on
  its default ports (both use 16571/29903/4723).
- Start prints the `ServerModule` rows it copied into the database; `Heli2v2Module` must be among them for Heli2v2
  tests. If it is missing, `bf2` has no such row either: enable it there (WebAdmin UI) or insert it into the test
  database.
- Bots: run your own copy, `publish-sidecar.ps1 -Out scratch\sidecar-<task>`, not the play session's `sidecar\`: a
  process running from `sidecar\` would block a staged `sidecar.next\` from being installed.
- Clients: `$env:RCON_PORT=4723; python -I tools\rcon.py "<cmd>"`;
  `$env:BF2WA_URL='http://localhost:5052'; $env:BF2WA_SERVER_ID='127.0.0.1:16571'; dotnet run --project tools\hubclient -- --watch 30`;
  the sidecar with `--endpoint 127.0.0.1:16706`; `serverext_client.py --port 16706`.
- A sidecar run like the play session's (two all-bot crews; add `--human-seat 0|1 --test-stand-in <s>` for a mixed
  crew):

  ```powershell
  cd C:\Data\Projects\DotNet\BF2WebAdmin-localtest\scratch\sidecar-<task>   # publish-sidecar.ps1 -Out here
  .\bf2bots.exe serverext --endpoint 127.0.0.1:16706 --team 2 --also-team 1 --also-vehicle ahe_z10 --gunners `
      --name "[BOT] bot" --mission config\flights\serverext-play-2v2.json --repeat-mission `
      --map-dir C:\Data\Projects\bf2-bots\data\maps\dalian_2_v_2 --flights-dir <task log dir>\flights > <task log dir>\sidecar.log
  ```

  The flags are those `play-2v2.ps1` builds (see its source); bf2-bots serverext-backend.md "How to run" lists all.
  Stop it with Ctrl+C or `Stop-Process` (its bots are removed when the connection drops).
- Verified 2026-10-26: Start, the module authenticated to the instance's WebAdmin, rcon and hubclient reached it, a
  `serverext_client.py` bot held a 20 s hover, Stop.

### Other test servers

`openbf2-server-ext/testenv/` runs the OpenBF2 server directly in WSL (`build-linux.sh`, `prepare-content.sh`,
`deploy-module.sh`, `run-wsl.sh`) or in its own Docker image `serverext-testenv` (`run-docker.sh`), with no WebAdmin.
That is the fastest loop for module work with a WSL build; see [findings/test-env.md](findings/test-env.md) and
protocol.md "Testing". Their run dirs are agent-owned (`~/serverext-<task>/srv`).

## Ports

| Port | What | Notes |
|---|---|---|
| 16567 (and 29900, 4711 on the host) | **Reserved for the author's own sessions** | Never use, never stop what holds it |
| 16569/udp (all interfaces) | play server game port | LAN players join here |
| 29901/udp | play server GameSpy query | |
| 127.0.0.1:4721 | play server rcon (container 4711) | `tools\rcon.py` |
| 127.0.0.1:16705 | play server bot channel (container 16705) | sidecar, test client |
| 4310 (0.0.0.0) | play WebAdmin admin-channel listener (TLS) | the module dials `host.docker.internal:4310` |
| 5050 | play WebAdmin web UI | user `admin`, password `WEBADMIN_ADMIN_PASSWORD` in `.env` |
| 127.0.0.1:14333 | SQL Server (shared) | databases `bf2` (play), `bf2test` (test instance) |
| 4300 | `mm_webadmin` text socket | only the retail variant (`setup.ps1` without `-OpenBF2`) and BF2WebAdmin `master` |
| 16571/udp, 29903/udp, 127.0.0.1:4723, 127.0.0.1:16706, 4312, 5052 | test instance defaults | `test-instance.ps1` |
| 16571, 29903, 4723, bot 16701 (WSL) / 16702 (Docker) | `testenv/` servers | 16700 is held by a stale `wslrelay`: leave it |
| 16703, 16710, 16573, 29905, 4725 | used by the admin-channel phase-1 WSL runs | |
| 55124, 55126/udp | VoIP inside each server | two WSL servers at once collide; containers do not |

## Which test proves what

| Test | Proves | Does not prove |
|---|---|---|
| Module unit tests (`ctest` in the build dir: `serverext_tests`, `serverext_admin_tests`) | framing, parsing, hostile input, config, queues, admin codec against the golden fixtures, TLS against a local OpenSSL server | anything engine-side |
| `tools/testclient/serverext_client.py --port <bot port> [--scenario ...]` (Python 3, stdlib) | the bot channel against a running server: hover (default), walk, buttons, blockpad, wreck, view, weapons, damage, spot, round, teleport, multiconn, soak2/3, burst, tv, q12, `--fuzz`. Must pass before a C# repo depends on a module change (protocol.md "Checks") | bot behaviour |
| `tools/adminclient/adminclient.py` | the admin channel without WebAdmin: a reference TLS listener (own certificate, `--print-pin`), handshake, snapshot, every command, scenarios, `--fuzz`; `--write-golden` writes the fixtures shared with C#. Point a WSL server's `adminConnect` at it (findings/admin-phase1.md "Results") | WebAdmin's behaviour |
| bf2-bots `dotnet test Bf2Bots.sln` (313 tests on 2026-10-26) | sidecar codec against golden bytes from the module, controller, missions, round flow logic | a live server |
| Sidecar against a server (`bf2bots serverext ...`) | bots spawn, board, fly, fight, type rdy/go; logs `round flow:`, `SAY` lines | a human in the loop |
| Sidecar `--test-stand-in <s>` with `--human-seat 0|1` | a mixed crew without a human: a bot sits in the human's seat and leaves it once after s seconds, and types for its crew | how a real human plays |
| WebAdmin `dotnet test test\BF2WebAdmin.Tests\BF2WebAdmin.Tests.csproj` (220 tests on 2026-10-26) | the C# admin codec (golden file), handshake, TLS pin, session ordering/resync, Heli2v2 | a live module |
| `tools\hubclient` (localtest) | WebAdmin from the web UI's side: `--watch`, chat commands, `--rcon`, `--get-server`, `--set-admin-key` | the Blazor pages themselves |
| Test instance (`test-instance.ps1`) | the whole chain on a real server and a real WebAdmin with its own DB, without touching the play session; Heli2v2 rounds stored in `bf2test` | a human client |
| A play session (`play-2v2.ps1`) | the author plays | for the author only; agents check Status and leave it alone |

Nothing has been tested with a human client on an OpenBF2 server with the module yet (see "Open issues").

## Logs

| What | Where |
|---|---|
| Module (`[serverext]` lines), play server | `docker logs bf2wa-localtest-openbf2`; inside the container `/volume/bf2.log` (status screen only), `/volume/modmanager.log` |
| Module, test instance | `docker logs bf2wa-localtest-openbf2-<name>` |
| WebAdmin, play session | `logs\play\webadmin.log`, `webadmin.err.log` (started by play-2v2.ps1); `start-webadmin.ps1` logs to its console |
| WebAdmin, test instance | `scratch\test-<name>\webadmin.log` |
| Sidecar, play session | `logs\play\sidecar-<mode>-<time>.log` (+ `.err.log`), flight logs under `logs\play\flights\` |
| Localtest image build | `openbf2\out\build.log`, `openbf2\out\image-build.log`, `openbf2\out\source.txt` |
| WSL test servers | `<run-dir>/bf2.log` (`testenv/run-wsl.sh log`) |
| Older runs | `scratch\` in the localtest (by date and topic), WSL `~/serverext-*` |

Do not delete the author's data in the localtest: `secrets\`, `.env`, `app\`, `logs\play\`. Put scratch output under
`scratch\`.

## Working rules

- **Port 16567 is reserved** for the author's sessions. Every server and endpoint you start uses an explicit, unused
  port; set the game port and `botListen` together.
- **Containers:** touch only the localtest's own containers (`bf2wa-localtest-*`) and containers you created for
  your task; never any other container on this PC. Before stopping, recreating or restarting a localtest container,
  WebAdmin or the bots, run `play-2v2.ps1 -Mode Status`: if anything runs, the author may be playing; use a test
  instance instead.
- **WSL:** do not restart WSL, do not run `wsl --shutdown`, do not kill `wslrelay`, do not switch WSL networking
  (that restarts Docker Desktop).
- **Git:** local commits are allowed (and needed: `build.sh` and `publish-sidecar.ps1` build commits). No pushes, no
  remotes, no pull requests, no issues, in any repo.
- **Bots** run only on the author's own servers (the localtest, testenv, the author's test VM), never on a remote
  server the author does not control. Bot names start with `[BOT]`.
- **OpenBF2:** its `AGENTS.md` governs any change there: read it in full, no pull requests, engine changes are
  `[CHOSEN, not recovered]`, kept small, local-branch only. See OpenBF2 docs/server-extension.md.
- **Protocol changes:** the contracts are `docs/protocol.md` (bot channel) and `docs/admin-channel-draft.md` (admin
  channel). Code follows them; a worker who finds them wrong stops and reports. In the current development phase a
  wire change that does not touch the engine keeps the protocol version (the author's rule, as for `0x010B Say` and
  the launch number; protocol.md "Changing this contract" states the same rule).
- **Q12 engine fix is deferred** by the author (2026-10-23): do not fix it; the sidecar's workaround stays.
- Main-thread module code never blocks; a malformed or hostile client must never crash or stall the server.

## Open issues

Collected from the findings and per-repo docs on 2026-10-26; the source of each is named.

- **Q12:** the engine refuses a TV launch while another launcher's TV missile flies in the gunner's projectile camera
  (stale `IProjectileObject` weak block). Engine fix deferred; the sidecar's gunner leaves the camera after each launch.
  A human gunner is not covered by that workaround. findings/q12-tv-refusal.md; protocol.md Q12; bf2-bots
  serverext-backend.md "Q12 workaround".
- **A round never runs with one human:** this tree floors `sv.numPlayersNeededToStart` at 2 and bots don't count, so
  without two connected humans the round stays in "waiting for players": no Python event handlers, tickets or
  scoreboard. WebAdmin (whose observer runs before that gate), Heli2v2 rounds, damage and kills work. protocol.md Q7;
  localtest README.
- **Bot chat relies on a layout copy of `StringReceivedEvent`** (`BotChatEvent` in `src/Module.cpp`,
  `static_assert`ed field by field against OpenBF2 `Game/Common/StringManager.h`, because the engine exports no
  symbols). A layout change in that header fails the module build; an engine change in how the event is handled
  (its virtuals, a new field use) would not. A human client seeing the bot's line is unverified. findings/bot-chat.md.
- **`Server.ServerId` is `nvarchar(20)`**, one short of `255.255.255.255:65535`, and it is now `adminServerId`.
  BF2WebAdmin admin-channel.md item 5.
- **The admin key is visible in WebAdmin's server edit form** to Administrator-role users (HMAC needs the raw key).
  admin-channel.md item 6.
- **Kick and ban still go through rcon** (ModManager `iga`) from the web UI and Discord; the typed commands are only
  behind the `.kick`/`.ban` chat commands. admin-channel.md item 7.
- **Heli2v2 and bot respawns:** a bot respawned with RemoveBot/CreateBot is a new player to Heli2v2; a pending round
  can then hold 5 players and Heli2v2 (exactly 4) would not start it. Not seen yet. bf2-bots serverext-missions.md
  "Round flow".
- **No human client has joined** an OpenBF2 server with the module: human chat channels, revive, a real kick/ban and
  its disconnect reason, in-game `.o`, the client ControlMap stage (Q4) are unexercised. findings/admin-phase1.md "What
  is left"; admin-channel.md "Not exercised".
- **CD-key hashes** exist only on an internet server; on a LAN or test server WebAdmin keys players `name:<name>`
  (decision B6). findings/admin-phase1.md "CD-key hash".
- **Many WebAdmin features need `adminAllowConsole 1`** (map mods, noclip, TV settings, `InitServer`). admin-channel.md
  item 2.
- **A changed admin key takes up to 60 s** (the module's reconnect backoff). admin-channel.md item 3.
- **Teleport rotation is not held** by a hovering bot's heading hold. admin-channel.md item 4.
- **The bot channel has no authentication:** keep it on loopback. findings/docker-v1.md.
- **Production deployment is blocked:** the engine changes are local, unpushed commits; bf2hub and PunkBuster with
  OpenBF2 are untested; shutdown is SIGKILL. findings/docker-v1.md "What blocks deploying".
- **Linux only:** no Windows `serverext.dll` yet; the OpenBF2 `msvc-x64` strict link fails on VoIP symbols (not caused
  by this work). README; findings/r1-smoke.md.
- **Q8** (does unrecovered retail code use EventManager category 7) needs IDA/Ghidra; the three R1b guards, the
  enter-on-spawn path for a bot, and yaw/roll input have never run. protocol.md "Open questions".
- **Stale statements in older docs:** BF2WebAdmin admin-channel.md's "Problems found" still lists bot-channel `Chat`
  breaking `§` (fixed by the v3 text conversion); findings/docker-v1.md item 6 says the admin channel is missing.
  The later documents win.
