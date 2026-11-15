# openbf2-server-ext

A native module for the [OpenBF2](https://github.com/B2BF/OpenBF2) dedicated server. It runs inside the server
process and gives external programs a typed, versioned interface to the running game, over two channels:

- **The bot channel** lets a program create bot players, read the game state on every simulation tick and send each
  bot's input: flight axes, buttons, guided-missile aim, chat. The bots are ordinary players to the engine, with no AI
  library involved, so the program on the other end decides everything they do.
- **The admin channel** replaces RCON and Python admin scripts for server administration. The server dials out to an
  admin client over TLS and gives it a snapshot of the game, live state streams, sequence-numbered events (chat,
  kills, damage, joins, vehicles, projectiles) and typed commands (kick, ban, say, map control, teleport, object
  spawning, votes, seat preferences, and JavaScript game scripts that run on the server).
  [BF2WebAdmin](https://github.com/nihlen/BF2WebAdmin) is the client it was built for.

The module is Linux x86-64 only (`libserverext.so`). It is in use on live servers, and the protocols are still
changing; see [Versioning](docs/protocol.md#versioning).

## How it fits together

```
 OpenBF2 dedicated server (bf2)
   mods/bf2/openbf2.con:  openbf2.addModDll serverext
   └─ libserverext.so     runs once per simulation tick (30 Hz) on the main thread;
        │                 all socket I/O is on its own threads
        ├─ bot channel:   TCP listener on botListen (loopback, no authentication)
        │                   ◄── a bot controller, or tools/testclient/serverext_client.py
        └─ admin channel: dials OUT to adminConnect over TLS 1.3 (pinned certificate, HMAC handshake)
                            ──► BF2WebAdmin, or tools/adminclient/adminclient.py
```

The engine exports no symbols, so the module uses only typed virtual calls on engine interfaces. The few things it
cannot do from outside are a small set of additions to the engine, on the
[`serverext/r1` branch of nihlen/OpenBF2](https://github.com/nihlen/OpenBF2/tree/serverext/r1). The module does not
load into an OpenBF2 build without them.

## Documentation

| Document | Contents |
|---|---|
| [docs/protocol.md](docs/protocol.md) | The bot channel: wire format, every message, spawning, the watchdog, stepped time, the engine requirements. |
| [docs/admin-channel.md](docs/admin-channel.md) | The admin channel: security model, wire format, state, events, commands, votes, scripts. |
| [docs/configuration.md](docs/configuration.md) | Every `serverext.cfg` key. |

The two protocol documents are the contracts. The code follows them, and a client is written against them.

## Install

Each [release](../../releases) is a zip to unpack into the server's `bin/amd-64` directory. It holds everything that
replaces the retail binaries:

| File | From |
|---|---|
| `bf2`, `libdice_py.so` | OpenBF2, with the engine additions the module needs |
| `libserverext.so` | this repo |
| `libbf2hub.so` | [OpenBF2-BF2Hub](https://github.com/B2BF/OpenBF2-BF2Hub), for servers listed on BF2Hub |
| `source.txt` | which commits the zip was built from, the builder image, and per binary its SHA-256, newest glibc symbol version and shared libraries |

The zip is named `openbf2-serverext-<openbf2>-<module>-<bf2hub>-<backend>.zip`: the three commits, then the GameSpy
backend it reports to. Each release also carries the zip's `.sha512` and a `-debug.tar.gz` with the unstripped
binaries.

1. Install a retail BF2 1.5 Linux dedicated server. The zip replaces its binaries; the game content stays the retail
   server's.
2. Unpack the zip into `bin/amd-64`.
3. Load the modules in `mods/bf2/openbf2.con`:

   ```
   openbf2.addModDll bf2hub
   openbf2.addModDll serverext
   ```

4. Write `serverext.cfg` in the server's working directory ([docs/configuration.md](docs/configuration.md)). With no
   file, the bot channel listens on `127.0.0.1:16700` and the admin channel is off.

The binaries need glibc 2.35 or newer and OpenSSL 3 (`libssl.so.3`); Debian 12 and 13 and Ubuntu 22.04 and newer have
both. A server on CentOS 7 uses the [CentOS 7 build](#centos-7-build) instead. OpenBF2 has no PunkBuster.

[bf2-docker](https://github.com/nihlen/bf2-docker) has Docker images that do all of this (`images/netsky-openbf2-*`
on its `combined` branch) and write `serverext.cfg` from environment variables on every start.

## Build

The build needs Linux (WSL works), CMake 3.21 or newer, Ninja, GCC with C and C++ compilers, `libssl-dev`, and an
OpenBF2 checkout of the `serverext/r1` branch. `testenv/tools-linux.sh` installs CMake and Ninja into `~/tools`
without root.

```
git clone -b serverext/r1 https://github.com/nihlen/OpenBF2.git ../OpenBF2
./build-module.sh [openbf2-checkout] [build-dir]
```

That configures the `linux-x64` preset, builds `libserverext.so` and runs the unit tests. The OpenBF2 checkout
defaults to `../OpenBF2`, or `$OPENBF2_SRC`; the build directory defaults to `~/serverext-build/linux-x64`.

- The admin channel needs OpenSSL 1.1.1 or 3.x, linked dynamically.
- The scripts' JavaScript engine, QuickJS-NG, is in `third_party/` and linked statically. It adds no shared library.
- The module builds against OpenBF2's headers only. It does not link the engine.

### Release build

`release/build.sh` builds the release zip: OpenBF2, this module and OpenBF2-BF2Hub, in one Ubuntu 22.04 builder image
(`release/Dockerfile`), with the module's unit tests run inside the build. It needs Docker and runs on Linux or in Git
Bash on Windows.

```
release/build.sh                                         # HEAD of ../OpenBF2, this repo and ../_ext/OpenBF2-BF2Hub
OPENBF2_REV=<commit> MODULE_REV=<commit> BF2HUB_REV=<commit> CLEAN=1 release/build.sh
```

It builds commits (`git archive`), never the working tree, and records them in `source.txt`. The same commits give the
same zip, byte for byte. The output is in `release/out/`. Ubuntu 22.04 is the target of OpenBF2's own Linux CI (GCC 11,
static libstdc++), and its glibc 2.35 keeps the binaries loadable on current Debian and Ubuntu.

`.github/workflows/release.yml` runs the same script on GitHub Actions and publishes the result as a release. It is
started by hand (Actions, "Release zip", "Run workflow"), with the OpenBF2 and OpenBF2-BF2Hub commits as inputs. A
push does not start it. The release notes come from `release/notes.sh`: the commits since the previous release, the
commits the zip was built from, and the files with their checksums.

A release changes no server by itself. The [bf2-docker](https://github.com/nihlen/bf2-docker) images pin one zip by
URL and SHA-512, so after a release, update `assets.txt`, `assets.sha512` and `OPENBF2_ZIP` in `build.sh` in each of
its `images/netsky-openbf2-*/assets/build/` directories, then rebuild the images. The zip's URL is
`https://github.com/nihlen/openbf2-server-ext/releases/download/<name>/<name>.zip`, and its SHA-512 is the release's
`.sha512` file.

### CentOS 7 build

For servers that still run CentOS 7 or another distribution of that age, `release/centos7/build.sh` builds the same
four binaries against glibc 2.17. Its zip has `-centos7` at the end of its name and is installed like the regular
one. It is published as a separate pre-release, named `... (CentOS 7 only)`, by
`.github/workflows/release-centos7.yml` (Actions, "Release zip (CentOS 7)", "Run workflow", with the same commits as
the regular release). It is not for current distributions: its `bf2` needs the ncurses 5 libraries
(`libncursesw.so.5`, `libtinfo.so.5`, `libformw.so.5`), which Debian 12 and Ubuntu 22.04 no longer install.

- `libserverext.so` carries its own OpenSSL 3.5, linked statically, because CentOS 7's OpenSSL has no TLS 1.3. It
  gets OpenSSL fixes only through a new build.
- The builder is CentOS 7 with GCC 11.4 compiled in the image (`release/centos7/Dockerfile`), so the first build
  takes a while.
- The build is tested less than the regular one, which is what the live servers and the bf2-docker images run.

Everything it needs is in `release/centos7/` and that workflow. The regular build does not depend on it.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The module. The bot channel: `Protocol`, `Config`, `Channel` and `Session` are engine-free, `Module.cpp` is the engine side, `IoThread_posix.cpp` is the socket thread. The admin channel: `AdminProtocol`, `AdminCrypto`, `AdminLink` and `AdminSession` are engine-free, `AdminIo_posix.cpp` is its dial-out and TLS thread, `Admin.cpp` is its engine side. `Script` is the script host: QuickJS runtimes, the `game` API and the limits. |
| `tests/` | Unit tests for everything engine-free: framing, parsing, hostile input, configuration, queues; the admin channel including TLS against a local server; the script host against a mock world. `fixtures/admin-v1-golden.txt` holds the admin channel's golden frames, for testing a client codec against the same bytes. |
| `tools/testclient/` | `serverext_client.py`, a bot channel client with a scenario per feature and a `--fuzz` mode. `tvscan.py` flies scripted TV-missile passes. Python 3, standard library only. |
| `tools/adminclient/` | `adminclient.py`, a reference admin client: a TLS listener that creates its own certificate and prints its pin, the handshake, every command, scenarios and `--fuzz`. `--scenario runscript --script-file x.js` runs one script on a server and shows its events, errors and CPU time. |
| `testenv/` | Scripts that turn a BF2 server Docker image into an OpenBF2 test server with the module, in WSL or in a container. |
| `release/` | The release build. |
| `third_party/quickjs-ng/` | QuickJS-NG, with one line changed. Its `README.md` has the source, the checksum and how to update. |

## Testing

- **Unit tests** run in every build (`ctest` in the build directory). They cover the engine-free layers.
- **Against a running server**, `tools/testclient/serverext_client.py --port <bot port>` creates a bot, flies it to a
  hover and prints its state. `--scenario <name>` runs one feature's checks and `--fuzz` sends hostile input.
  `tools/adminclient/adminclient.py` does the same for the admin channel: point the server's `adminConnect` at it.
- A test server must use its own ports. Set `sv.serverPort` and `botListen` explicitly, and never share them with a
  server people play on.

## Related projects

| Project | Relationship |
|---|---|
| [OpenBF2](https://github.com/B2BF/OpenBF2) | The engine: an open-source reconstruction of the Battlefield 2 dedicated server. |
| [nihlen/OpenBF2, `serverext/r1`](https://github.com/nihlen/OpenBF2/tree/serverext/r1) | OpenBF2 with the engine additions this module needs. Its `docs/server-extension.md` describes the engine side. |
| [OpenBF2-BF2Hub](https://github.com/B2BF/OpenBF2-BF2Hub) | The BF2Hub module for OpenBF2, shipped in the release zip. |
| [BF2WebAdmin](https://github.com/nihlen/BF2WebAdmin) | The web admin that uses the admin channel. |
| [bf2-docker](https://github.com/nihlen/bf2-docker) | Docker images of BF2 servers, including OpenBF2 servers with this module. |

## License

GPL-3.0, like OpenBF2, whose headers the module is built against. See [LICENSE](LICENSE). QuickJS-NG in
`third_party/quickjs-ng/` is under the MIT license (`third_party/quickjs-ng/LICENSE`).

Clients talk to the module over a socket and are not linked to it.
