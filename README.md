# openbf2-server-ext

A native module for the OpenBF2 dedicated server that gives out-of-process tools a stable, typed
interface to the running game: per-tick state, bot players driven by external input, and later the
admin events and commands that BF2WebAdmin uses today through `mm_webadmin.py`.

Status: protocol v0 implemented for Linux x86-64 (`libserverext.so`) and tested on a WSL server;
see [docs/findings/module-v0.md](docs/findings/module-v0.md). Start with the contract in
[docs/protocol.md](docs/protocol.md).

## Layout and build

| Path | Contents |
|---|---|
| `src/` | The module. `Protocol`, `Config`, `Channel` and `Session` are engine-free; `Module.cpp` is the engine side; `IoThread_posix.cpp` is the socket thread. |
| `tests/` | Unit tests for the engine-free layer (framing, parsing, hostile input, config, queues). |
| `tools/testclient/` | `serverext_client.py`, a Python 3 stdlib test client: hover flight and `--fuzz`. |
| `testenv/` | WSL and Docker test-server scripts; `deploy-module.sh` installs the module into a run directory. |
| `probe/` | The superseded `[TEMP]` R1 smoke probe, kept as the evidence behind `docs/findings/r1-smoke.md`. |

Build in WSL (needs the tools from `testenv/tools-linux.sh` and an OpenBF2 checkout carrying the
R1 engine changes, by default the `serverext-r1` worktree; override with `OPENBF2_SRC`):

```
./build-module.sh [openbf2-checkout] [build-dir]
```

That configures the `linux-x64` preset, builds `libserverext.so` and runs the unit tests. A Windows
build (`serverext.dll`) is not implemented yet.

## Related repos

| Repo | Local path | Relationship |
|---|---|---|
| OpenBF2 | `C:\Data\Projects\OpenBF2` | The server this module loads into. A local branch carries the engine changes the module needs. |
| bf2-bots | `C:\Data\Projects\bf2-bots` | The bot sidecar, a client of the bot channel. |
| BF2WebAdmin | `C:\Data\Projects\DotNet\BF2WebAdmin` | Future client of the admin channel. |
| BF2WebAdmin-localtest | `C:\Data\Projects\DotNet\BF2WebAdmin-localtest` | Existing local `dalian_2_v_2` setup on the retail netsky server; a reference for content and ports, not a host for this module. |
| OpenBF2-BF2Hub | `C:\Data\Projects\_ext\OpenBF2-BF2Hub` | Reference for a module's layout, presets and loading. |

## License

The module compiles against OpenBF2 headers, so expect GPLv3, as OpenBF2-BF2Hub is. Clients talk to
it over a socket and are not linked to it. Confirm before publishing.
