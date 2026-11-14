# openbf2-server-ext

A native module for the OpenBF2 dedicated server that gives out-of-process tools a stable, typed
interface to the running game: per-tick state, bot players driven by external input, and later the
admin events and commands that BF2WebAdmin uses today through `mm_webadmin.py`.

Status: bot channel protocol v3 implemented for Linux x86-64 (`libserverext.so`) and tested on a WSL server;
see [docs/findings/module-v3.md](docs/findings/module-v3.md) (module-v2.md, module-v1.md and module-v0.md for earlier versions). Start with
the contract in [docs/protocol.md](docs/protocol.md). The admin channel (phase 1 of
[docs/admin-channel-draft.md](docs/admin-channel-draft.md)) is implemented beside it; its byte layouts, configuration and
results are in [docs/findings/admin-phase1.md](docs/findings/admin-phase1.md); the viewer additions (view mode, aim, gunfire,
weapons) in [docs/findings/admin-viewer.md](docs/findings/admin-viewer.md). What a TV missile can hit on a helicopter, and
which parts the server tests at a lag-compensated (rewound) pose: [docs/findings/tv-hitbox.md](docs/findings/tv-hitbox.md).
Scripts: WebAdmin pushes JavaScript over the admin channel and the module runs it every tick in an embedded
QuickJS-NG, with a small `game` API and memory and CPU-time limits (the contract's "Scripts" section; how it was built,
tested and what it costs: [docs/findings/scripts.md](docs/findings/scripts.md)). `adminAllowScripts 0` in
`serverext.cfg` turns them off.

## Layout and build

| Path | Contents |
|---|---|
| `src/` | The module. `Protocol`, `Config`, `Channel` and `Session` are engine-free; `Module.cpp` is the engine side; `IoThread_posix.cpp` is the socket thread. The admin channel: `AdminProtocol`, `AdminCrypto`, `AdminLink` and `AdminSession` (engine-free), `AdminIo_posix.cpp` (its dial-out and TLS thread) and `Admin.cpp` (its engine side). `Script` is the admin channel's script host (engine-free: QuickJS runtimes, the `script` and `game` objects, the limits); `Admin.cpp` gives it the game. |
| `third_party/quickjs-ng/` | QuickJS-NG v0.18.0, the scripts' JavaScript engine: the library's files with one line changed (its `README.md`), built as a static library and linked into `libserverext.so`. Its `README.md` has the source, the checksum and how to update. |
| `tests/` | Unit tests for the engine-free layer (framing, parsing, hostile input, config, queues); `AdminTests.cpp` for the admin channel, TLS included; `ScriptTests.cpp` for the script host against a mock world; `fixtures/admin-v1-golden.txt`, the golden frames shared with the C# codec. |
| `tools/testclient/` | `serverext_client.py`, a Python 3 stdlib test client: hover flight, `--fuzz`, the v1 scenarios (`--scenario` walk, buttons, blockpad, allblocked, wreck) and the v2 scenarios (view, weapons, damage, spot, round, teleport, multiconn, soak2; the watchdog neutral is q5) and the v3 scenarios (burst, tv, soak3). `tvscan.py`: scripted TV-missile passes for the hit/miss replay tests (docs/findings/tv-hitbox.md). |
| `tools/adminclient/` | `adminclient.py`, the reference WebAdmin for the admin channel: a TLS listener (it creates the certificate and prints its pin), the HMAC handshake, snapshot, subscriptions, every command, scenarios and `--fuzz`; `--write-golden` writes the fixtures. For scripts: `--scenario script` and `scriptends` (the checks), and `--scenario runscript --script-file x.js` to run one script on a test server and see its events, errors and CPU time. |
| `testenv/` | WSL and Docker test-server scripts; `deploy-module.sh` installs the module into a run directory. |
| `probe/` | The superseded `[TEMP]` R1 smoke probe, kept as the evidence behind `docs/findings/r1-smoke.md`. |

Build in WSL. It needs the tools from `testenv/tools-linux.sh` and an OpenBF2 checkout carrying the
`IServerExtension` engine changes. By default that is `C:\Data\Projects\OpenBF2`, branch `main`; override it with
the first argument or `OPENBF2_SRC`:

```
./build-module.sh [openbf2-checkout] [build-dir]
```

That configures the `linux-x64` preset, builds `libserverext.so` and runs the unit tests. The admin
channel needs OpenSSL 1.1.1 or 3.x (`libssl-dev` to build; `libssl3` or `libssl1.1` at run time, linked
dynamically). QuickJS-NG is C, so the build needs a C compiler beside the C++ one (`gcc` comes with `g++`); it adds
no shared library. A Windows build (`serverext.dll`) is not implemented yet.

## Release build

`release/build.sh` builds the zip the production game-server images unpack into `bin/amd-64` (bf2-docker
`images/netsky-openbf2-*`): `openbf2-serverext-<openbf2>-<module>-<bf2hub>.zip` with `bf2` and `libdice_py.so`
(OpenBF2 with the `IServerExtension` commits), `libserverext.so` (this repo), `libbf2hub.so`
([OpenBF2-BF2Hub](https://github.com/B2BF/OpenBF2-BF2Hub)) and a `source.txt` with the three commits, the builder and
per binary its sha256, newest `GLIBC_` version and `NEEDED` libraries. It runs in Git Bash with Docker Desktop or on
Linux, takes the sources as `git archive` of commits, builds in the docker volume `serverext-release-build`
(incremental) with the image from `release/Dockerfile`, and runs the module's unit tests:

```
release/build.sh                                         # HEAD of ../OpenBF2, this repo and ../_ext/OpenBF2-BF2Hub
OPENBF2_REV=824d4dc MODULE_REV=c14b561 BF2HUB_REV=7e37854 CLEAN=1 release/build.sh
```

Output in `release/out/`: the zip, its `.sha512` (the line for bf2-docker's `assets.sha512`), a `-debug.tar.gz` with
the unstripped binaries, and the build log. The builder is Ubuntu 22.04, the target of OpenBF2's own Linux CI (GCC 11,
static libstdc++), with glibc 2.35 (Debian trixie has 2.41) and OpenSSL 3.0.2 (the module links `libssl.so.3`).
`.github/workflows/release.yml` runs the same script on GitHub Actions; it needs this repo and the OpenBF2 fork's
branch on GitHub.

## Related repos

How they fit together, and how to build, deploy and test across them: [docs/system.md](docs/system.md)
(`C:\Data\Projects\openbf2-server-ext\docs\system.md`).

| Repo | Local path | Relationship |
|---|---|---|
| OpenBF2 | `C:\Data\Projects\OpenBF2` | The server this module loads into. Local commits on branch `serverext/r1` carry the engine changes the module needs (`docs/server-extension.md` there). |
| bf2-bots | `C:\Data\Projects\bf2-bots` | The bot sidecar, a client of the bot channel. |
| BF2WebAdmin | `C:\Data\Projects\DotNet\BF2WebAdmin` | The admin channel's client: branch `admin-channel` (not merged into `master`) replaces `mm_webadmin.py` with it. |
| BF2WebAdmin-localtest | `C:\Data\Projects\DotNet\BF2WebAdmin-localtest` | The local test environment. Its OpenBF2 variant runs this module on `dalian_2_v_2` with WebAdmin and the bots attached. |
| OpenBF2-BF2Hub | `C:\Data\Projects\_ext\OpenBF2-BF2Hub` | Reference for a module's layout, presets and loading. |

## License

The module compiles against OpenBF2 headers, so expect GPLv3, as OpenBF2-BF2Hub is. Clients talk to
it over a socket and are not linked to it. Confirm before publishing.
