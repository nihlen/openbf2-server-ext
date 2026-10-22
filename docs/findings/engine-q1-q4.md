# Engine findings: Q1, Q2, Q3, Q4 and the R1 check

Source investigation only. No OpenBF2 source was changed, and no server was started.

- **Source:** OpenBF2 commit `ccf3099`. The investigation used the `serverext/investigate` worktree, which is at the same commit as `C:\Data\Projects\OpenBF2` `main`. Every `file:line` below is relative to the OpenBF2 root.
- **Binaries:** IDA and Ghidra were not used. Every claim rests on current recovered source, including the PE/ELF notes embedded in it. Where a point would need binary evidence, it says so.
- **Map content:** `dalian_2_v_2/server.zip` was copied out of `cr.nihlen.net/library/bf2-netsky:2v2-nocheck` with `docker create` and `docker cp`; the image was not run. It is byte-identical (MD5 `f53c24b2d9d2bba86952341b14c08ef5`) to the copy in `E:\Docker\bf2-docker-netsky`. The map list runs it as `gpm_cq 16`.

## Summary

| Question | Answer |
|---|---|
| Q1 | No engine change is needed. Register an `EventManager` listener on the unused category 7 and post a self-rescheduling `postEventNextUpdate` event. It is dispatched exactly once per 30 Hz tick, inside `GameServer::simulateFrame`, after `gameLogic->update` and before `simulatePlayersUpdate`. `FrameEventManager` is the wrong choice: it runs once per frame, not once per tick. |
| Q2 | The engine's axes, angle signs and angular-velocity sign already match the contract. Copy position, velocity and angular velocity unchanged. Compute the angles with the formulas bf2-bots already uses. |
| Q3 | On the 2v2 layer the AI/human spawn flags do not block a bot, but the AI-flagged enter-on-spawn path crashes the server without the AIDLL. Spawn the bot on foot at a non-enter-on-spawn pad point, then call `IGame::enterVehicle` directly. |
| R1 | The contract's description is correct. Proposed design: a separate `[CHOSEN]` extension interface, so retail vtables are untouched, plus null guards at three AI-courier dereferences that an AI-flagged player can otherwise reach (R1b). |
| Q4 | On the server, a raw write lands in the same `PlayerInput` slots and is read by the same code as a client's axes. It only matches the human path if the module also (a) sets the slots' mapping bits and (b) quantizes the axes to hundredths by truncation. Without (a), every consumer reads 0. The client ControlMap stage was not traced. |

---

## Q1. Per-tick callback

### Answer

There is no listener interface the engine calls once per tick. The engine's tick-level `EventManager`, however, gives exactly the needed hook without an engine change:

1. In `initDll`, get the `"EventManager"` singleton through `IClassManager::getSingleton`. Register an `IEventListener` on category 7 with `registerEventHandler(7, listener, 0)`.
2. Post one `postEventNextUpdate(7, <id>, <data>)`.
3. In `handleEvent(7, id, …)`, do the tick's work and post the next `postEventNextUpdate`.

**Why this runs exactly once per simulation tick, at the right point:**

- `GameServer::update` runs `simulateFrame` once per step: `steps` steps per frame, capped at 10 (`Game/GameServer/GameServer.cpp:2380-2398`).
- `simulateFrame` calls `gameLogic->update(dt)`, then `eventManager->processEvents(dt)`, then `simulatePlayersUpdate(dt)` and the physics/collision passes (`GameServer.cpp:2522-2546`). This is the only server caller of `eventManager->processEvents` (`GameClient.cpp:712` is the client).
- `processEvents` increments `m_updateCount`, then dispatches due nodes. It skips any node stamped with the current update count (`Core/EventManager.cpp:254-279`). `postEventNextUpdate` inserts with delay 0 and the current stamp (`EventManager.cpp:175-188`, `204-237`). So a re-post made inside the handler fires on the next update, which is the next tick. It never fires twice in one tick and never skips one.
- Category 7 has no registered handler anywhere in the tree. The registrations are listed by `rg "registerEventHandler("`. Category 10 is BF2Engine's, 11 is BF2Engine's (`Game/Main/BF2Engine.cpp:512-515`), and 4 is the client sound facade's. Without a listener, `postEvent*` drops the node (`EventManager.cpp:132-136`, `180-184`), so the listener must be registered before the first post.
- Nothing on the server drains the queue. `removePendingEvents` has no caller outside `EventManager.cpp`. `EventManager::removeEvent` has the shipped head-removal defect (`EventManager.cpp:314-317`), but it also has no engine caller; the only `removeEvent` hits are `Animation/BoneAnimation/Bundle.cpp`, a different class. The chain therefore survives map changes.

**Timing within the tick.** At the callback in tick N:

- The world state is the result of tick N−1's physics and collisions. It also includes tick N's `gameLogic->update`, which runs spawning (`ServerGameLogic.cpp:542`) and rules.
- Inputs written now are consumed by tick N's `simulatePlayersUpdate`, a few lines later. That is zero added latency.
- Network sends happen after all steps of the frame (`GameServer.cpp:2413-2419`).

The TickState snapshot is therefore "end of tick N−1". The tick number in the frame should say so. A counter of the module's own is enough; `bf2Engine->dword54` is incremented per step at `GameServer.cpp:2395`, but it is not reachable as a typed singleton.

**What else the module can reach from `initDll`.** `initDll` runs inside `GameServer::init`, through `openbf2.con` (`GameServer.cpp:289-302`).

- The `"Game"` singleton is already registered (`BF2Engine.cpp:1181-1184`). `IGameServer` comes from `game->queryInterface(IID_IGameServer)`, the same way the engine gets it at `BF2Engine.cpp:1192`.
- `EventManager`, `PlayerManager` and `ObjectManager` are registered singletons. The pattern for fetching them is `Core/DllModule.cpp:79-97`.
- `gameLogic` and `spawnManager` are **not** registered singletons; the full list of registered names comes from the `SingletonRegInfo` rows. `gameLogic` is also not created yet when `initDll` runs (`BF2Engine.cpp:1199`). The module has no typed path to either. This matters for Q3 and R1.

### Rejected candidates

- **`FrameEventManager`.** It is processed once per **frame** with the frame's duration, after `game->update` and after the network send (`BF2Engine.cpp:1771`, `1791`). A catch-up frame runs 2 to 10 ticks and still fires it only once, and it fires on frames with zero ticks.
- **`IPlayer::handleFrameUpdate`.** It is per frame (`BF2Engine.cpp:1830-1835`).

### Fallback R0, only if the event route is rejected

Add one `[CHOSEN, not recovered]` hook to `GameServer::simulateFrame`, between `eventManager->processEvents(dt)` and `simulatePlayersUpdate(dt)` (`GameServer.cpp:2530-2531`). The module would register a callback through a new extension singleton (see R1). That gives the same timing as the event route.

### Confidence and open points

- **Confidence:** high, from source.
- **Category 7:** the choice is `[CHOSEN]` by the module. It is free in the recovered tree, but category 7 is unnamed and `[UNRESOLVED]` (`Game/IEventListener.h:13-28`). An unrecovered retail component could use it; this cannot be settled without binary evidence.
- **Not tested at runtime.**

---

## Q2. Rotation and velocity conventions

### Answer

**Transform layout.** `IObject::getAbsoluteTransformation()` (`Game/Objects/IObject.h:86`) returns a row-major `Mat4` in row-vector form:

- `m[0..2]` is the body right axis in world coordinates.
- `m[4..6]` is up.
- `m[8..10]` is forward.
- `m[12..14]` is the position.

Evidence: `spawnPlayer` places "two metres ahead" as `pos + m[8..10]*2` (`Game/GameLogic/ServerGameLogic.cpp:2020-2024`), and `Mat4::rotMult` multiplies rows (`Math/Mat4.cpp:318-329`). This is the layout bf2-bots `LocalStateReader.FromMatrix` assumes.

**Euler angles.**

- The engine's own decomposition is `getRotationZXY`: z = atan2(−m1, m5), x = asin(m9), y = atan2(−m8, m10) (`Math/Mat4.cpp:234-265`).
- `.con` `Object.rotation yaw/pitch/roll` is applied as `setRotationZXY(m, −roll, −pitch, −yaw)` (`Scene/Object/ToolObject/ObjectTemplateAdm.h:39-40`).
- The engine's yaw/pitch/roll triple is therefore yaw = atan2(m8, m10), pitch = −asin(m9), roll = atan2(m1, m5), in degrees. These are exactly bf2-bots' formulas (`LocalState.cs` `FromMatrix`).

The contract's angles are the engine's native `.con` rotation triple:

- yaw 0 = north (+z), 90 = east (+x)
- pitch positive = nose down
- roll positive = right wing up, which is bank left

Use `atan2` for roll (as bf2-bots does) instead of the engine's gimbal-lock special cases.

**Velocity.** `IPhysicsNode::getPositionalSpeed()` (`Physics/IPhysicsNode.h:48`) is world-frame metres per second; the integrator damps and averages it in world axes (`Physics/PhysicsNode.cpp:872-886`). Use it unchanged.

**Angular velocity.**

- `IPhysicsNode::getRotationalSpeed()` (`IPhysicsNode.h:49`; field at physics node +0x94, the same field bf2-bots reads) is in rad/s and **world** axes.
- The integrator turns the body by `transform = transform * R(axis = ω̂, |ω|·dt·57.2958°)` (`Physics/PhysicsNode.cpp:1213-1236`). In row-vector form, that rotates each body axis about a world axis.
- I mirrored `setRotateAboutLine`, `makeOrthonormalBasis`, `setRotateZDeg` and `rotMult` (`Math/Mat4.cpp:302-369`, `Math/Vec3.cpp:47-75`) in a script. Starting level and facing north:
  - ω = +y gives yaw +10°, nose east.
  - ω = +x gives pitch +10°, nose down.
  - ω = +z gives roll +10°, bank left.
- These match the contract's sign conventions, so angular velocity needs **no conversion**.

**Where to read it.** The vehicle's physics node is `object->queryInterface(IID_IPhysicsNode)`; the object caches that result at +0x44 (`IObject.h:241`). Use the typed `queryInterface`, not `LayoutAccess`.

- Read the root vehicle object (`world::getRootParent`), not the seat.
- A dedicated server has no frame interpolation (`BF2Engine.cpp:1764-1767` is client-only), so the object transform is the simulated one.

### Conversion

| Contract field | Source |
|---|---|
| x, y, z | `m[12], m[13], m[14]` |
| yaw | `atan2(m8, m10)` in degrees |
| pitch | `−asin(clamp(m9))` in degrees |
| roll | `atan2(m1, m5)` in degrees |
| vx, vy, vz | `getPositionalSpeed()`, unchanged |
| wx, wy, wz | `getRotationalSpeed()`, unchanged |

### Handedness

The axes are x east, y up, z north. With the engine's `cross()` (`Vec3.cpp:47-52`), x × y = z. Physically the frame is left-handed, as in DirectX. Nothing in the conversion depends on this.

### Confidence and open points

- **Confidence:** high for layout, angles and the ω sign, from source plus the numeric mirror. bf2-bots' clean-room physics port independently confirms the ω field and its sign tick by tick (bf2-bots `docs/assumptions.md`).
- **Typed accessor:** `queryInterface(IID_IPhysicsNode)` returning the node is taken from the field comment at `IObject.h:241`. I did not trace the `queryInterface` body.
- **Physics port:** the bf2-bots port says the **logged** attitude is the pilot camera, pitched 4° nose down relative to the body. The module reports the **body** transform, so the sidecar must not apply the camera correction to TickState.

---

## Q3 / R2. Getting a bot into a heli seat

### The normal spawn path

1. A dead player with `spawnGroup > 0` and `spawnAtTick <= now` is spawned automatically by `ServerGameLogic::uPlayingSpawning`, every third tick (`ServerGameLogic.cpp:633-689`). Stale groups are cleared each tick with the AI flag (`:644-657`; the contract cites `:647`).
2. `spawnPlayer` (`ServerGameLogic.cpp:1921-2140`) checks, in order:
   - `m_roundGoing` and game status 1.
   - The UseAI first-wave refusal (`:1930-1938`).
   - Spawn group and team set, and team tickets > 0.
   - The group passes `getActive(isAI)`, team, squad and `getSpawnPoint(isAI)` (`:1991-1999`).
   - `getSpawnPosition(pos, isAI)` (`:2033`).
3. It creates the soldier, calls `game->enterVehicle(player, body, true)` and sets the player alive (`:2055-2062`).
4. It creates the kit. A missing kit for `(team<<8)+kit` is a fatal Debug (`:2065-2073`).
5. If the spawn point is `EnterOnSpawn`, or `AIEnterOnSpawn` for an AI player, it calls `toggleEntryPoint(player, false)` (`:2110-2114`). That calls `BFfindEntryPoint(…, player, …)` (`:1736`), then `game->enterVehicle(player, seat, false)` (`:1749`).

### What an AI-flagged player with no AIDLL hits

- **Spawn flags: not blocking on this map.**
  - Group gating is `isAI ? !onlyForHuman : !onlyForAI` (`SpawnSystem/SpawnGroup.cpp:88-107`); point gating is the same (`Game/Objects/SpawnPoint.cpp:270`).
  - The 2v2 layer sets neither flag; the defaults are false (`Game/Objects/SpawnPointTemplate.cpp:32-39`).
  - An AI player only changes the entry search radius at an enter-on-spawn point, 15 m instead of 5 m (`SpawnPoint.cpp:349-352`).
- **UseAI first-wave refusal: does not apply.** `UseAI` is false unless the game mode is `gpm_coop` or the setting is passed (`GameServer.cpp:652-662`). The map list runs `gpm_cq`.
- **Seat swap (`:1488-1530`): only runs on a human's position-select.**
  - It lets a **human** swap seats with an AI occupant. That path dereferences `aiObject` only after a null check (`:1515`), so it is safe without the AIDLL.
  - The bot never swaps: `:1488`.
  - Separately, a same-team human pressing E prefers an AI-occupied seat: `BFfindEntryPoint` `alternate` (`PlayerControlObjectHelpers.cpp:456-466`) and the evict at `PlayerControlObject.cpp:2258`. A teammate can take the bot's seat.
- **Crash 1: enter-on-spawn, which is the 2v2 heli spawn.** `BFfindEntryPoint` dereferences `ai::BFSingletonServer::m_iaiPlayerCourier` with no null check when the player is AI (`Game/Objects/Game/PlayerControlObjectHelpers.cpp:321-329`). That pointer is 0 unless the AIDLL fills it (`AI/AIInterface/SingletonServer.cpp:22`, `50-52`). So `spawnPlayer` → `toggleEntryPoint` → `BFfindEntryPoint` crashes the server. A bot pressing `PIUse` crashes it the same way (`ServerGameLogic.cpp:1340-1347`).
- **Crash 2: scatter spawn points.** For an AI player at a scatter point that is not enter-on-spawn, `getSpawnPosition(…, checkAI=true)` calls `aiPathfindingMapContains`. That dereferences `CommonSingletonServer::m_iaiPathfindingCourier` without a null check (`SpawnPoint.cpp:38-42`, `438-440`). None of dalian's 2v2 points scatter (default false, not set), so this does not apply here.
- **Round start: bots do not start a round.** With no AI library, the round starts only when `getNumberOfAliveHumanPlayers() >= sv.numPlayersNeededToStart` (`ServerGameLogic.cpp:446-466`). That count excludes AI players and requires `getIsConnected` (`Scene/Object/PlayerManager.cpp:893`). `spawnPlayer` returns early until the round is going (`:1923-1928`). On the author's test server (`numPlayersNeededToStart 1`), a bot alone never spawns. A human must be present, or the setting must be 0. A 0 setting passes `>=` in source; this is untested.

### The dalian_2_v_2 2v2 layer

From `GameModes/gpm_cq/16/GamePlayObjects.con` inside `server.zip`:

| Object | Settings |
|---|---|
| Control point `AH1Z` | id 401, team 2, at 242/147/−266 |
| Control point `Z10` | id 402, team 1, at −207/152/158 |
| Heli spawners | Two per base, all `setControlPointId` to their base. 401: `AH1Z_CH_US_AttakHeli` at 255.1/147.1/−258.7 and `Z10_CH_US_AttakHeli` at 253.8/147.1/−292.3. 402: the mirror. Each spawner has per-team templates (`setObjectTemplate 1`/`2`). |
| Spawn points `AH1Z_1..3`, `Z10_1..3` | Default group (0), offset 0/1.25/0, no enter-on-spawn, no scatter, no AI or human flags. On the pad, about 14 m from the heli. |
| Spawn points `AH1Z_4`, `Z10_4` | `setGroup 1`, `setEnterOnSpawn 1`, `setAllowSpawnCloseToVehicle 1`. Placed 4 m above the heli spawner. This is the "spawn in heli" icon. |

Spawn group ids are allocated at load per (control point, group) pair (`SpawnPoint.cpp:195-202`, `SpawnSystem/SpawnManager.cpp:110-121`). Each base therefore has an on-foot group and an in-heli group. Their numeric ids depend on load order.

### Recommended R2 route

This uses only recovered code and stays clear of the crashes.

1. Create the bot (R1). Set its team with `IPlayer::setTeam(team, false)`, a valid kit with `setKit`, and the base's **group-0** (on-foot) spawn group with `setSpawnGroup`. This is the same call sequence as `createAndSpawnAIPlayer` (`GameServer.cpp:452-454`).
2. Let `uPlayingSpawning` spawn it on the pad. Points `*_1..3` are not enter-on-spawn and not scatter, so neither crash path is reached.
3. On a later tick, check that the target seat's `IPlayerControlObject::isOccupied()` is false and that the vehicle team is 0 or the bot's team. Then call `IGame::enterVehicle(player, seatObject, false)` (typed IGame slot +0xA4, `Game/IGame.h:126`). The seat's PCO map uses PCO id keys, pilot = 0 (`ServerGameLogic.cpp:1445`, `1460-1474`).

`GameServer::enterVehicle` (`GameServer.cpp:3563-3734`) has no AI-courier use and no occupancy check, so the module must make those checks. It also sends the enter event to clients and Python event 21. This is the same pair of calls the human E key makes when on foot (`ServerGameLogic.cpp:1740-1750`).

### Gaps the module cannot close by itself

- **Spawn group id.** `spawnManager` is not a registered singleton (`Core/ModuleRegistry.cpp:1915-1927`). The module can find the group id by walking world objects for spawn points and calling `ISpawnPoint::getSpawnGroupId()`. That path is plausible but not verified.
- **Alternative spawn.** `spawnPlayer(player, &at)`, a spawn at a supplied transform with no spawn point, is non-virtual (`ServerGameLogic.h:96`), so the module cannot reach it. Exposing it would be part of the R1 extension interface.
- **Map change.** `GameServer::unload` destroys every AI-flagged player (`GameServer.cpp:1416-1429`). The module must recreate bots after a map change and must not destroy them a second time.

### Confidence

High that the flags do not block and that the enter-on-spawn and `PIUse` crashes exist, from source. The recommended route is reasoned from source and **untested**.

The comment at `SpawnPoint.cpp:211-214` still calls `getActive`, `getSpawnPosition` and the related methods `[FRONTIER]` bodies that "fault by name". The bodies are now implemented in the same file. Treat the marker as stale, and confirm with the OpenBF2 maintainers before relying on that subsystem.

---

## R1 check and proposed design

### The contract's description, checked

- `IGameServer::createPlayer(name, isAI)` refuses when `!ai::g_aiInstance` (`GameServer.cpp:369-376`; the function starts at `:365`). The AI branch:
  - picks a free id counting down from `255 − numPlayers` (`:380-384`);
  - calls `createPlayerWithValidName`, which enforces `GSMaxNrOfPlayers` and registers the networkable at id+1 (`:327-360`);
  - calls `setIsAIPlayer(true)` and `setIsRemote(true)` (`:393-394`);
  - posts ECGame event 4 (`:396`) and sends `CreatePlayerEvent` to all connected clients (`:398`);
  - fires Python event 14 (`:408-411`);
  - allocates a VoIP session, and its event goes to a missing connection (`:413-424`).
- With `isAI = false` the player becomes the local human (`:400-404`). The contract is correct.
- `Player::setIsAIPlayer(true)` deletes the ActionBuffer and allocates a zeroed `io::PlayerInput` (mapping flags 0) at `m_aiPlayerInput` (`Game/GameLogic/Player.cpp:4031-4063`). `getAIPlayerInput()` returns it (`:4024-4027`).
- `simulatePlayerUpdate` uses that buffer for AI players (`GameServer.cpp:3363-3367`). It applies it while the game status is 1, or 3 for AI players (`:3403-3415`).
- `simulatePlayersUpdate` simulates every player whose connection is null or in state 4 (`:3432-3466`). A connection-less bot is always simulated.
- `getIsAIGame()` returns `g_aiInstance != 0` (`ServerGameLogic.cpp:2820-2823`). A fake `"AI"` singleton would make it 1 and also turn on the `g_aiInstance` branches. `rg` finds 50 non-comment `g_aiInstance` references in 23 engine files outside `AI/`. The contract's "about 15 call sites" figure was not re-counted by call site.

### Proposed design

This is `[CHOSEN, not recovered]` and needs an OpenBF2 issue before it goes upstream.

**1. New interface.** A header-only abstract class in OpenBF2, for example `Game/GameServer/IServerExtension.h`:

```cpp
// [CHOSEN, not recovered] OpenBF2 server-extension surface for native modules (issue #TBD).
class IServerExtension
{
public:
    // Creates a connection-less AI-input player without requiring the AI library.
    // Returns the player id, or -1 (player limit, no free id).
    virtual int createInputPlayer(const std::string& name) = 0;
protected:
    ~IServerExtension() {}
};
```

**2. Where it lives and how the module reaches it.**

- `GameServer` implements it, either as an extra base or as a small owned object.
- `GameServer::init` (or `startGame`) registers it as the named singleton `"OpenBF2ServerExtension"`, before `openbf2.con` runs (`GameServer.cpp:289`).
- The module calls `getSingleton("OpenBF2ServerExtension")` and makes typed virtual calls.
- `IGameServer`, `IGame` and every retail vtable stay unchanged. Appending a slot to `IGameServer` would also work, but it changes an ABI-contract interface.

**3. Body.**

- Factor the code after the gate in `createPlayer` (`:378-425`) into a private helper. `createPlayer(name, true)` keeps its gate and calls the helper, so its observable behaviour is identical. `createInputPlayer` calls the same helper without the gate.
- Name the departure in a comment at the gate.
- Later, the same interface could also carry `spawnPlayerAt` (wrapping `ServerGameLogic::spawnPlayer(player, &at)`) and the R0 tick hook, if those are wanted.

**4. R1b, needed for safety.** An AI-flagged player can reach three unguarded courier dereferences. Each fix is a one-line `[CHOSEN]` null guard, behaving as if no vehicle or target was found:

| Site | Trigger |
|---|---|
| `PlayerControlObjectHelpers.cpp:327-329` (`m_iaiPlayerCourier->getRequestedVehicle`) | Enter-on-spawn, or the bot pressing `PIUse` |
| `GameServer.cpp:3340` (`m_iaiEventCourier->eventMissileLock`) | An enemy weapon's missile-lock message (`field04 != −1`) on a vehicle with an AI occupant, unless the vehicle category is filtered at `:3330-3337`. This is **outside the module's control**. |
| `ServerGameLogic.cpp:3411-3415` (`m_iaiPlayerCourier->getResurrectTarget`) | Defibrillator use on a non-soldier. Not reachable in v0. |

Without R1b, the module avoids the first site (buttons are 0 in v0, no enter-on-spawn points) and cannot avoid the second.

### Where this player differs from a retail AIDLL bot

This comes from a full audit of the `getIsAIPlayer()` sites on the dedicated path.

| Area | Difference |
|---|---|
| Crashes | The three sites in the R1b table. A retail bot has the couriers loaded, so these paths work for it. |
| Round start | Does not count toward starting a round. Retail with AI starts immediately with `startDelay` 0 (`ServerGameLogic.cpp:446-466`). The state-3 restart check does count it (`:503-505`). |
| Map change | Destroyed by `GameServer::unload` (`:1416-1429`). The AIDLL would recreate its bots; the module must. |
| Python | `getIsAIGame()` is 0, so scripts take the human-game path (`ServerGameLogic.cpp:2822`). Player attributes `ai=1`, `remote=1`, `connected=0` (`Game/Python/PythonHost.cpp:1208-1212`) are the same as a retail bot. The retail `bf2`/stats Python and the netsky/`mm_webadmin` scripts were not checked for how they treat `ai=1` players. |
| GameSpy query | Reports `AIPlayer=1` per player (`Game/MatchMaking/Server/ServerQueryCache.cpp:161`) but `hasBots=0` (`:114`). |
| Scoreboard | Clients get the AI flag through `CreatePlayerEvent` (`Game/GameEvents/CreatePlayerEvent.cpp:63`), the same as retail. Client rendering is not on the server path and was not traced. |
| AI-library extras that are absent | ECAI notifications are dropped (no listener): squad orders (`SquadManager.cpp:531-536`), heal (`ServerGameLogic.cpp:3693`), spawn and exit (`:1903-1911`, `:1682-1688`, both gated). AI weapon deviation `m_aiDeviation` stays 0, so the bot gets human deviation (`SoldierDeviationComp.cpp:216`, `SimpleDeviationComp.cpp:81`). Courier-supplied ammo targets are skipped (`ReplenishingAmmoComp.cpp:327`). `destroyPlayer` has an `[UNRESOLVED]` omitted AI remote-player drop (`GameServer.cpp:3866`). |
| Same as a retail bot | Excluded from votes, VoIP routing and the human count. Listed as "is an AI bot." (`GameServer.cpp:2723`). Kick broadcasts but does not remove it, because there is no connection. Ban is a no-op. A human entering evicts it. Sends to its id are no-ops (`GameServer.cpp:2220-2235`). |

Connection ids are byte keys, and the bot's id is chosen from 255 downward. If the bot's id ever equalled a live connection id, sends would reach that human. This is the same for retail bots. How NetServer assigns connection ids was not checked.

### Alternative worth one look, not recommended without its own audit

A connection-less **non-AI** remote player whose `ActionBuffer` the module fills with `PlayerAction`s:

- **Gains:** human input parity, including quantization. A built-in 30-tick replay-then-neutral watchdog (`GameServer.cpp:3370-3399`). None of the AI-courier crash sites.
- **Costs:** human-only code may assume such a player has a connection. That is unaudited. `ActionBuffer::pushBack` and `PlayerAction` are non-virtual engine code, so the module would need an engine entry point anyway.

### Confidence

High for the R1 facts and the crash sites, from source; my own read covered the main sites, and a subagent audit covered the rest. The design is untested.

---

## Q4. Raw axis writes compared with the client path

### Server-side human path

1. The client's `PlayerInput` is packed by `PlayerAction::set` (`Game/Common/PlayerAction.cpp:70-112`). Axes 0 to 5 are stored as `short(int(convertAxisToInternalResolution(v) * 100))`. That means truncation toward zero to hundredths, clamped to ±327.67 (`:27-58`). Only slots whose mapping bit is set are taken; an unmapped slot is stored as 0.
2. On the server, the action is taken from the player's ActionBuffer in `simulatePlayerUpdate` (`GameServer.cpp:3370-3400`). Each tick consumes one queued action. An empty queue replays the last action for up to 30 ticks, then goes neutral.
3. The action is unpacked by `PlayerAction::get`. It zeroes all inputs, then sets the mapping bit **and** the value `short/100` for axes 0 to 5 and for every button in the table (`PlayerAction.cpp:172-203`).
4. The result goes into the same `player->handleInput(*chosen, …)`, `checkPlayerTriggers` and `performHandleUpdate` calls as the AI path (`GameServer.cpp:3403-3415`).

### Comparison

From the moment the value reaches `handleInput`, both paths run identical code on the same `io::PlayerInput` slots. Sign and range are whatever the slot holds; nothing on the server re-inverts or re-scales by player type. Two things differ:

1. **Mapping bits (must fix).** Vehicle-side readers return 0 for an unmapped slot:
   - `EngineBase.cpp:45-51`
   - `PlayerControlObject.cpp:1079-1086`
   - `ForceObject.cpp:53-58`
   - `Player.cpp:1324`, `3912`
   - `GameLogic.cpp:2526`

   The AI buffer starts with `mappingFlags = 0` (`Player.cpp:4052`), and nothing on the AI path sets them. The module must set at least bits 0 to 3, and should set the same mask `PlayerAction::get` produces (axes 0 to 5 and the button table at `PlayerAction.cpp:13-18`). Otherwise the heli sees zero input whatever the module writes.
2. **Quantization (should match).** A human's axis reaches the server truncated to hundredths, and values below 0.01 become 0. That matches bf2-bots' finding that "every stick axis reaches the server as hundredths". For parity with humans and with the bf2-bots physics port, the module should apply the same transform before writing: `float(short(int(clamp(v) * 100))) / 100`. Clamp first, as the contract says, and mirror `convertAxisToInternalResolution`'s float steps.

Timing also differs. The AI buffer is read live every tick, with no action queue and no 30-tick replay. A human's input is queued one action per tick, and some arrive a tick late (bf2-bots measured about 5% on LAN). This favours the bot slightly and does not affect correctness.

### Not traced

- The client stage: ControlMap deadzone, scale, inversion and per-vehicle maps, and the client's own float rounding (open in bf2-bots). The axis-to-slot mapping is a client ControlMap matter. On the server, `PIPitch`, `PIRoll`, `PIThrottle` and `PIYaw` are only slot numbers, read by the vehicle templates' configured inputs (for example `ForceObject` `m_template->input`).
- Whether a raw write produces the same manoeuvre as the bots' fixed profile still needs the scripted comparison the contract describes. Source cannot settle that.

### Confidence

High for the server side. The client side is unverified.

---

## Contract corrections

1. **SetInput.** Writing the four slots alone does nothing: the module must also set the mapping bits in `PlayerInput::mappingFlags`, at least bits 0 to 3, preferably the full mask `PlayerAction::get` sets. It should also quantize to hundredths (Q4). The `GameServer.cpp:3363` citation is correct.
2. **R1 gate line.** The `g_aiInstance` refusal is at `GameServer.cpp:371`; the function starts at `:365`.
3. **Q1 candidates.** `FrameEventManager` is per frame, not per tick, and runs after the network send. Recommend the EventManager self-rescheduling event on category 7 instead. The snapshot it produces is the end of tick N−1, taken before tick N's player update, so label the tick accordingly.
4. **R2.** "Spawn group, then `IGame::enterVehicle`" is right only if the group is not enter-on-spawn. The 2v2 in-heli group (`AH1Z_4`/`Z10_4`, `setEnterOnSpawn 1`) crashes an AI-flagged player without the AIDLL (`PlayerControlObjectHelpers.cpp:327`). Use the pad group, then `enterVehicle`. The `ServerGameLogic.cpp:647` flags do not block on this map.
5. **R1 scope.** R1 alone is not crash-safe. Add R1b, the three courier null guards, or accept that an enemy missile-lock on the bot's vehicle can crash the server (`GameServer.cpp:3340`).
6. **Module lifetime.** "The module also removes every bot it created when … the module unloads." The loader has no unload path. `closeModule` is only called when `initDll` refuses (`OpenBF2Console.cpp:239-242`), so a loaded module stays for the life of the process.
7. **Map change.** The engine destroys every AI-flagged player itself on unload (`GameServer.cpp:1416-1429`). The module must drop its ids and recreate bots, not remove them.
8. **Testing.** A bot alone never starts a round on a server without the AIDLL. The round needs `numPlayersNeededToStart` alive, connected humans (`ServerGameLogic.cpp:446-466`; `PlayerManager.cpp:893`). The end-to-end check needs a human client, or `sv.numPlayersNeededToStart 0`, which is untested.
9. **EnterVehicle field.** "Vehicle object id" is undefined. The engine has networkable ids (u16, `io::networkManager->getObject`, `GameServer.cpp:3527-3528`) and grid ids (`objectManager->getObjectFromGrid`, `:3311`). Pick one; the module can only reach `ObjectManager` as a singleton.
10. **Engine references the module cannot reach.** `gameLogic` and `spawnManager` are not registered singletons. Any module need for them, such as game status or spawn group lookup, goes through the R1 extension interface.

## Unverified, or needing evidence I did not have

- **Category 7 in retail binaries.** Whether any unrecovered retail code uses `EventManager` category 7 needs IDA/Ghidra.
- **`IObject::queryInterface(IID_IPhysicsNode)`.** That it returns the node is from the field comment, not a traced body.
- **`sv.numPlayersNeededToStart 0`.** Whether the setting accepts 0 and starts a round with only bots.
- **Retail Python.** How the retail/netsky Python (`bf2` stats, `mm_webadmin`) treats `ai=1` or `connected=0` players.
- **Stale marker.** The `SpawnPoint.cpp:211-214` `[FRONTIER]` comment contradicts the bodies below it.
- **Runtime.** Every runtime behaviour in this document; nothing was run.
