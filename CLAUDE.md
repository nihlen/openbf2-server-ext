# openbf2-server-ext agent instructions

Read [README.md](README.md) and [docs/protocol.md](docs/protocol.md) before changing anything.

## The contract

- `docs/protocol.md` is the only definition of the protocol shared by this module, `bf2-bots` and
  `BF2WebAdmin`. Code follows the contract, not the other way round.
- If the contract is wrong or incomplete, stop and report what is missing. Do not work around it in
  code and do not edit it unless the author or the coordinating thread asked you to.
- Every engine fact the contract relies on cites an OpenBF2 `file:line`. Verify it in the current
  source before building on it; the open questions in the contract are unverified by definition.

## Working with OpenBF2

- OpenBF2's `AGENTS.md` governs any change inside `C:\Data\Projects\OpenBF2`. Read it in full before
  touching that repo. Changes there are local-branch only, kept as small as possible, and marked
  `[CHOSEN, not recovered]`.
- Prefer typed virtual calls on engine interfaces. Do not patch raw addresses or hard-code vtable
  offsets.
- Build into an agent-owned output directory (`BF2_BUILD_OUT` for OpenBF2 builds); never reuse or
  delete another agent's build directory.

## Runtime rules

- Port 16567 is reserved for the author's own sessions. Every test server and every module
  endpoint uses an explicit, unused port; set `sv.serverPort` and `botListen` together. The
  BF2WebAdmin local test already uses 16569, 29901 and 4721; see "Testing" in the contract.
- Test on `dalian_2_v_2` with the netsky server content, running an OpenBF2 build in WSL or Docker
  as described in the contract. Never stop or modify the BF2WebAdmin local test containers.
- Main-thread code never blocks: no socket or disk I/O, no waiting on a client. See the threading
  rules in the contract.
- A malformed or hostile client must never crash or stall the server.

## Git

- Do not commit, push, create remotes or open issues or pull requests unless the author asks.
