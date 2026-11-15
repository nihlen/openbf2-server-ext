# openbf2-server-ext agent instructions

Read [README.md](README.md) and the contract of the channel you are changing before you change anything:
[docs/protocol.md](docs/protocol.md) for the bot channel, [docs/admin-channel.md](docs/admin-channel.md) for the
admin channel. Settings are in [docs/configuration.md](docs/configuration.md).

## The contract

- `docs/protocol.md` and `docs/admin-channel.md` are the only definitions of the two protocols. Code follows the
  contract, not the other way round, and clients never redefine a message locally.
- If a contract is wrong or incomplete, stop and report what is missing. Do not work around it in code, and do not
  edit it unless the maintainer asked you to.
- Every engine fact a contract relies on cites an OpenBF2 `file:line`. Verify it in the current source before you
  build on it. The items under "Known limitations" are unverified by definition.
- In the current development phase, a wire change that does not touch the engine keeps the protocol version
  (protocol.md "Versioning").
- A change to a contract comes with the matching change to the reference client in `tools/` and to the unit tests.

## Working with OpenBF2

- The module builds against the `serverext/r1` branch of github.com/nihlen/OpenBF2. OpenBF2's `AGENTS.md` governs any
  change in that repo: read it in full before you touch it.
- Engine changes are kept as small as possible and marked `[CHOSEN, not recovered]`. Its `docs/server-extension.md`
  describes the engine side of this module.
- Prefer typed virtual calls on engine interfaces. Do not patch raw addresses or hard-code vtable offsets. The engine
  exports no symbols, so anything that is not a virtual call or inline header code is out of reach.

## Runtime rules

- Main-thread code never blocks: no socket or disk I/O, no waiting on a client. See the threading rules in
  protocol.md.
- A malformed or hostile client must never crash or stall the server. New parsing code gets a hostile-input test.
- Every test server and every module endpoint uses an explicit, unused port. Set `sv.serverPort` and `botListen`
  together, and never use the default game port 16567.
- Run bots only on servers you control.

## Build and test

- `./build-module.sh [openbf2-checkout] [build-dir]` builds the module and runs the unit tests. Use your own build
  directory.
- `tools/testclient/serverext_client.py` and `tools/adminclient/adminclient.py` test the two channels against a
  running server. A protocol change must pass them before a client depends on it.
- The release zip is built from commits by `release/build.sh`, not from the working tree.
- A release is not in use until bf2-docker pins it. After making one, say so and name what has to change there
  (README, "Release build").
