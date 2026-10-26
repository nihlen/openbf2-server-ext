# openbf2-server-ext agent instructions

Read [README.md](README.md) and [docs/protocol.md](docs/protocol.md) before changing anything.

## The whole system

`C:\Data\Projects\openbf2-server-ext\docs\system.md` is the guide to how this module, OpenBF2, bf2-bots, BF2WebAdmin
and the BF2WebAdmin localtest fit together. It covers the repos and branches, how to build each piece, how to deploy
to the localtest and test there, the ports, the logs and the working rules. Read it before you build, deploy or test
anything outside this repo's unit tests.

## The contract

- `docs/protocol.md` is the only definition of the protocol shared by this module, `bf2-bots` and
  `BF2WebAdmin`. The admin channel's contract is `docs/admin-channel-draft.md`. Code follows the contract, not the
  other way round.
- If the contract is wrong or incomplete, stop and report what is missing. Do not work around it in
  code and do not edit it unless the author or the coordinating thread asked you to.
- Every engine fact the contract relies on cites an OpenBF2 `file:line`. Verify it in the current
  source before building on it; the open questions in the contract are unverified by definition.
- In the current development phase, a wire change that does not touch the engine keeps the protocol version.

## Working with OpenBF2

- OpenBF2's `AGENTS.md` governs any change inside `C:\Data\Projects\OpenBF2`. Read it in full before
  touching that repo. Work on its `serverext/r1` branch, checked out in that folder; `main` is upstream's and stays free of these changes. Changes there are local only, kept as small as
  possible, and marked `[CHOSEN, not recovered]`. Its `docs/server-extension.md` describes the engine side of this
  module.
- Prefer typed virtual calls on engine interfaces. Do not patch raw addresses or hard-code vtable
  offsets.
- Build into an agent-owned output directory (`BF2_BUILD_OUT` for OpenBF2 builds); never reuse or
  delete another agent's build directory.
- The Q12 engine fix is deferred by the author. Do not implement it.

## Runtime rules

- Port 16567 is reserved for the author's own sessions. Every test server and every module
  endpoint uses an explicit, unused port; set `sv.serverPort` and `botListen` together. The port map is in
  `docs/system.md`.
- Test on `dalian_2_v_2` with the netsky server content. Use an OpenBF2 build in WSL (`testenv/`) or the BF2WebAdmin
  localtest's OpenBF2 variant, which is this module's test environment.
- In the localtest, touch only its own containers (`bf2wa-localtest-*`), never other containers. Before you stop,
  recreate or restart any of them, run `play-2v2.ps1 -Mode Status`. If a play session runs, leave it alone and use a
  separate test instance (`test-instance.ps1`).
- Do not restart WSL or kill `wslrelay`.
- Bots run only on the author's own servers.
- Main-thread code never blocks: no socket or disk I/O, no waiting on a client. See the threading
  rules in the contract.
- A malformed or hostile client must never crash or stall the server.

## Git

- Local commits are allowed, with clear messages and explicit paths. The localtest builds from commits.
- Do not push, create remotes or open issues or pull requests.
