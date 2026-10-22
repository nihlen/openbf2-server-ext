# Admin channel findings: A1, A4 and the R4 design

Source investigation only. No source was changed, nothing was built, and no server was started.

- **OpenBF2 source:** worktree `C:\Users\Alex\.t3\worktrees\OpenBF2\serverext-r1`, branch `serverext/r1`
  (head `540fa88`). Every engine `file:line` below is relative to `Code/BF2/`.
- **BF2WebAdmin:** `C:\Data\Projects\DotNet\BF2WebAdmin`. C# paths are relative to
  `src/BF2WebAdmin.Server/`. `mm_webadmin.py` is
  `examples/docker-compose/bf2/server-1/server/admin/modules/mm_webadmin.py` (v0.5); `mm_utils.py`
  is next to the `modules` directory.
- **Retail `bf2` Python package:** read from a stray copy at `/tmp/inst/x/python/bf2/` (WSL temp).
  Its origin was not checked. It is used only for `Player.index`, `getVehicle`, `getRootParent` and
  `GameLogic.send*` wrappers.
- **Binaries:** IDA and Ghidra were not used. Every engine claim rests on the recovered source,
  including the PE/ELF notes in it. Points that need binary evidence or a runtime test are marked
  **unverified**.
- **Confidence:** **H** means read directly in source. **M** means inferred from source that was
  read, with no runtime check. **U** means unverified.

## Summary

| Question | Answer |
|---|---|
| A1 | All 13 retail script events that `mm_webadmin` uses go through one dispatch function, `PythonHost::handleEvent(ScriptEvent, PyObject*)` (`Game/Python/PythonHost.cpp:799`). By the time they get there, their arguments are already Python tuples. The typed C++ values exist only at the 51 call sites. They carry what `mm_webadmin` sends, with three exceptions: the CD-key hash, team names and tickets need lookups that no module-reachable interface offers. Game state is not one of the 42 events. It comes from `EventManager` category 0, id 1. |
| R4 | `[CHOSEN, not recovered]`. Add one observer call at the top of `PythonHost::handleEvent(ScriptEvent, PyObject*)`, before the Python handler loop. An engine-side decoder in `PythonHost.cpp` turns the tuple back into a typed argument array (player, object, int, float, string, nested tuple). Two things are appended to `IServerExtension`: `setScriptEventListener` and the `IScriptEventListener` type. The cost is one branch per event when no listener is set. Most events can also be had without R4 through `EventManager`. Only the reviver identity has no other source. |
| Commands | `rcon`/`rconresponse` need the console. `"MainConsole"` is a registered singleton, and `Console::executeLine(cmd, out, …)` returns the output. `pm`, `position`, `rotation`, `damage`, `team` and `score` have typed paths. `gameevent`, `hudevent`, `medal` and `rank` need new appended `IServerExtension` methods, because they construct engine `GameEvent` subclasses or call non-virtual engine code. `timerinterval` is internal to the module. |
| A4 | Yes for the game modules. They never see the wire: they use `IGameServer` state, Mediator events and `IGameWriter` methods. A second backend changes `SocketServer`/`Program.cs` (DI wiring) and adds a reader adapter and an `IGameWriter`. Four text-protocol assumptions leak into modules, and the backend must keep them: `GetRconResponseAsync` results joined with `\b`, `SendRcon` raw console strings run in strict order, `RootVehicleId` equal to the console grid id, and Windows-1252 text. |

---

## A1. Engine origin of each mm_webadmin event

### The dispatch path

- **Script-event table.** It has 42 names, mapped to ids 0..41 (`PythonHost.cpp:1879-1904`). The
  draft's `:1882` falls inside it. `registerHandler` stores each callable in
  `m_vector28[event]` (`:1948-1958`), and the vector is sized to 42 (`:2153`). **H**
- **One dispatcher.** `PythonHost::handleEvent(ScriptEvent, PyObject*)` (`:799-820`) is the only
  walker of `m_vector28`. A grep finds no other reader besides registration, resize and clear
  (`:805`, `:1948`, `:2153`, `:2217`). It returns early when the host is not initialized or the
  `m_byte01` mute is set (`:801-804`). It runs a handler when the handler was registered
  `alwaysCall`, or when the game is playing and the begin-round state is Running (`:808-811`). Python
  exceptions are cleared (`:813-817`). **H**
- **Call sites.** There are 51 `pythonHost()->handleEvent(...)` calls in 19 `.cpp` files. All of
  them build the tuple first with a typed `python::buildArgs(...)` overload
  (`PythonHost.h:121-153`), which converts `IPlayer*` and `IObject*` to Python wrappers
  (`PythonHost.cpp:203-297`). Typed values therefore exist only at the call sites, not in the
  dispatcher. **H**
- **Events with no caller.** Ids 33 (`ConsoleSendCommand`), 35 (`ServerReady`), 36 (`BeginRound`)
  and 37 (`EndRound`) have no caller in current source. Whether retail ever fires them is
  **unverified** (needs IDA/Ghidra).
- **Game state is not a script event.** `host.registerGameStatusHandler` appends to `m_list14`
  (`PythonHost.cpp:1124-1144`). That list is walked by the virtual `IEventListener::handleEvent`
  (`:849-904`), which is registered on `EventManager` category 0 (`:2184`). The event is posted by
  `ServerGameLogic::setGameStatus` as `ECGameLogic` id 1 with an `int` payload, possibly delayed
  (`ServerGameLogic.cpp:311-318`). **H**

### Event table

The "Py id" column is the script-event id. "Args at the call site" lists the C++ types passed to
`buildArgs`.

| mm_webadmin message | Py event (id) | Engine origin (file:line) | Args at the call site | What mm_webadmin sends | Extra lookups and the typed interface for each | Conf. |
|---|---|---|---|---|---|---|
| `serverInfo` | none; sent on socket connect | — | — | `sv.serverName`, `sv.serverPort`, `sv.gameSpyPort` (via rcon), the `maplist.list` regex (gpm_cq only), `maxPlayers` | Console `executeLine` for the `sv.*` values and `maplist.list`. `sv.maxPlayers` is a console getter (`Game/GameServer/ServerSettingsConsole.cpp:347`). `IGameServer::getName` (`Game/IGameServer.h:96`). | H |
| `gameStatePlaying/EndGame/PreGame/Paused/Restart/NotConnected` | game-status list, not a script event | `ServerGameLogic.cpp:311-318` posts `ECGameLogic` 1 → `PythonHost.cpp:849` | `int` status (`Game/IGameLogic.h:30-35`: 1 Playing, 2 EndGame, 3 PreGame, 4 Paused, 5 Restart, 6 Disconnected) | team 1/2 names, map name, max players; tickets at EndGame | Map name: `IGame::getCurrentMapName` (`Game/IGame.h:153`). Team names: `IGameLogic::getTeamName` (`IGameLogic.h:85`). Tickets: `ServerGameLogic::getNumberOfTickets`, non-virtual (`ServerGameLogic.cpp:2729`). `gameLogic` is not a registered singleton, and the `GameLogic` console object has no getter for current tickets or team names (`Game/GameLogic/GameLogicConsole.cpp:159-241`). **This needs appended `IServerExtension` methods** (`getTeamName`, `getTickets`). | H |
| `controlPointCapture`, `controlPointNeutralised` | `ControlPointChangedOwner` (25) | `Game/Objects/Game/ControlPoint.cpp:294-296` (captured, 1) and `:307-309` (neutralised, 0), in `ControlPoint::handleUpdate`, only while status is Playing (`:283`) | `IObject*` (the CP), `int` 1/0 | `cp_getParam('flag')`, `cp_getParam('team')`, `templateName` | The CP's team and flag come from `IControlPoint::getTeam/getFlag` through `queryInterface(IID_IControlPoint)` (`Game/Objects/IControlPoint.h:23,36`), the same path Python uses (`Game/Python/py/Objects.cpp:859-873`). The template name is `IObject::getTemplate()->getName()` (`Game/Objects/IObject.h:151`). Everything is reachable. | H |
| `ticketStatus` | none (the timer is commented out, `mm_webadmin.py:418-419`) | — | — | — | Dead in v0.5. | H |
| `playerConnect` | `PlayerConnect` (14) | Remote: `GameServer.cpp:1307-1308` (`handleClientInfo`). Local/bot: `GameServer.cpp:543-544` (`createPlayerUngated`) | `IPlayer*` | index, name, profileId, IP, CD-key hash, team | Name, team and profile id: `IPlayer::getName/getTeam/getProfileId` (`Game/Objects/IPlayer.h:46,97,212`). IP: `IPlayer::getPeerAddress` (`IPlayer.h:195`), set just before the event (`GameServer.cpp:1303-1305`), or `IGameServer::getPlayerConnectionIp` (`IGameServer.h:59`). **CD-key hash:** `mm_utils.get_cd_key_hash` parses `admin.listPlayers` (`mm_utils.py:352-377`). The engine reads it with `gcd_getkeyhash(gamespy::g_gameID, id)`, an `extern "C"` function inside the executable (`GameServer.cpp:152`, `:2856-2858`). A module cannot call it. Use the console `admin.listPlayers` (`GameServer.cpp:2836`) or **an appended `IServerExtension::getCdKeyHash`**. Whether GameSpy CD-key auth produces a hash on OpenBF2 at all is **unverified**. | H (hash: U) |
| `playerSpawn` | `PlayerSpawn` (23) | `GameServer.cpp:3831-3835`, in `GameServer::enterVehicle` when the entered object is a Soldier | `IPlayer*`, `IObject*` (the soldier) | position and rotation of the root | Read from the `IObject*` (`getAbsolutePosition` slot 27, `getAbsoluteTransformation` slot 33; `IObject.h:80,86`). The root is found through the inline `getParent()` (`IObject.h:157`). | H |
| `playerChangeTeam` | `PlayerChangeTeams` (7) | `ServerGameLogic.cpp:1872-1874`, in `selectTeam` only (the client's team request) | `IPlayer*`, `bool` (`m_shouldUpdateTicketSystem`) | index, new team | `IPlayer::getTeam`. Event 7 does **not** fire for `Player::setTeam` calls from Python or admin (`PythonHost.cpp:1310-1314`). `mm_webadmin` calls `onPlayerChangeTeams` itself after `team` (`mm_webadmin.py:263`). | H |
| `playerScore` | `PlayerScore` (28) | Only `pmgr_setScore` (`PythonHost.cpp:1497-1502`), when Python writes `score` with score events enabled | `IPlayer*`, `int` delta | score, rplScore, kills, deaths | Read from `IPlayer::getScoreData()` (`IPlayer.h:132`) → `PlayerScoreData` fields (`Scene/Object/PlayerScoreData.h:49-57`). This is a **Python-originated** event: it fires only when game-mode scripts update the score. | H |
| `playerRevived` | `PlayerRevived` (2) | `ServerGameLogic.cpp:3962-3967`, in `ServerGameLogic::resurrect` (`:3846`) | `IPlayer*` revivee, `IPlayer*` medic | medic index, revivee index | None needed. | H |
| `playerKilled`, `playerTeamkilled`, `playerKilledSelf` | `PlayerKilled` (1) | Main: `GameLogic/GameLogicDamage.cpp:458-461` (`serverGiveDamage`). Suicide: `ServerGameLogic.cpp:178-180` (local) and `GameServer.cpp:1390-1394` (network case 12) | `IPlayer*` victim, `IPlayer*` attacker, `IObject*` weapon (null when `vehicleKill`, `:457`), `map<IPlayer*,int>` assists, `IObject*` vehicle. Suicide: `(p, p, 0, 0, 0)` | attacker and victim index, the two positions, `weapon.templateName` or `"roadkill"` | The weapon template is on the `IObject*`. **The fifth argument is the attacker's vehicle, not the victim's soldier.** It is `serverGiveDamage`'s `vehicle` parameter (`GameLogicDamage.cpp:183`), filled from the source PCO or the armor's last-hit vehicle (`ServerGameLogic.cpp:2939-2946`). `mm_webadmin` calls it `victimSoldierObject` and sends its root position as the victim position (`mm_webadmin.py:580-593`). The victim's position must come from `victim->getVehicle()`. A roadkill is `weapon == 0` with `vehicle != 0` (`GameLogicDamage.cpp:457`, `ServerGameLogic.cpp:2964-2965`). The driver is the attacker, and the root of `vehicle` is the vehicle. | H (what the 5th arg holds for infantry kills: U) |
| `playerDeath` | `PlayerDeath` (3) | `ServerGameLogic.cpp:3550-3553`, in `ServerGameLogic::killPlayer` (`:3471`), when `a` is set | `IPlayer*`, `IObject*` (`player->getVehicle()` captured before the kill, `:3515`) | index, position | Read from the object. | H |
| `playerDisconnect` | `PlayerDisconnect` (15) | `GameServer.cpp:3989-3991`, in `GameServer::destroyPlayer` (`:3949`) | `IPlayer*` | index | None needed. | H |
| `enterVehicle` | `EnterVehicle` (21) | `GameServer.cpp:3838-3843`, in `enterVehicle` for non-Soldier objects | `IPlayer*`, `IObject*` (the entered object), `bool` child-soldier active | index, root vehicle **id**, root template, entered template | Root: walk `getParent()`. Id: `IObject::getObjectId()` (`IObject.h:155`) is the console grid id. `object.active id<N>` resolves through `ObjectManager::getObjectFromGrid` (`Scene/Object/ObjectTemplateUtil.cpp:76-85`, `:98`). That replaces `findId()` with no console round trips. | H (id = grid key: M) |
| `exitVehicle` | `ExitVehicle` (22) | `GameServer.cpp:3925-3932`, in `exitVehicle` when `fireScript` is set and the object is not a Soldier | `IPlayer*`, `IObject*` (the **old** vehicle) | index, `-1`, `"Unknown"`, template | The real old vehicle is available, so its root id and template can be sent. The placeholders are `mm_webadmin`'s own choice (`:620`). | H |
| `vehicleDestroyed` | `VehicleDestroyed` (24) | `GameLogicDamage.cpp:437-445`, when the owner is a PCO, `dies` is set and the armor was not already damaged | `IObject*` owner, `IPlayer*` attacker | `-1`, template | Id and root come from the object. | H |
| `chatServer`, `chatPlayer` | `ChatMessage` (39) | `GameServer.cpp:4107-4109`, in `handleStringReceivedEvent` (`:4063`). It covers both network chat and `game.sayAll/sayTeam` (`GameServer.cpp:4537-4570` post the same `StringReceivedEvent`) | `int` sender (−1 on channels 12/13), `std::string` text, `std::string` channel name, `int` flags | channel, flags, index, text (with `stripmessage`) | None needed. | H |
| `playerPositionUpdate` | none; a 4 Hz Python timer | — | — | index, root position/rotation of `getVehicle()`, ping | The module's own tick snapshot: `IPlayer::getVehicle` (`IPlayer.h:55`). Retail `Player.getVehicle` returns the default vehicle when the child soldier is active (`PythonHost.cpp:1236-1246`). Ping: `IPlayer::getPing` (`IPlayer.h:129`). Rotation: Python returns the negated, reversed ZXY angles of the root's absolute matrix (`Game/Python/py/Objects.cpp:611-628`). The module must apply the same convention. | H |
| `projectilePositionUpdate` | none; the same timer | — | — | `findId(proj)`, template, position, rotation (only `agm114_hellfire_tv`) | A grid walk with `ObjectManager::getAllObjectsFromGrid` (`Scene/Object/ObjectManager.h:88`), the same walk `omgr_getObjectsOfType` uses (`py/Objects.cpp:515-536`). The grid key is the id. | H |
| `response` | none; the reply to `rconresponse` | — | — | code, output with `\n` → `\b` | Console. | H |
| raw rcon output, `ERROR: …` | none | — | — | untyped lines | `GameReader` ignores them (`GameReader.cs:110-117`). They can be dropped. | H |

The draft counts 24 events. The code emits 28 typed messages, plus the raw rcon and `ERROR` lines.
`ticketStatus` is dead.

### Surprises found while tracing A1

1. **`gameStatePaused` never reaches Python, and neither does the status that follows it.** The
   game-status sweep has a latch. A transition into code 4 sets it and returns. The next transition
   clears it and returns. Neither notifies (`PythonHost.cpp:863-877`). As recovered, `mm_webadmin`'s
   `gameStatePaused` is unreachable, and the `Playing` after an unpause is lost. A module listening
   to `ECGameLogic` 1 directly sees every status. **H** in source. Whether retail behaves the same
   way rests on the recovery comment, not on a check made here.
2. **Every `playerKilled` sends the wrong "victim position"**, because the fifth argument is the
   attacker's vehicle (row above). **H** for the argument. What it holds for an infantry kill is
   **U**.
3. **Suicides probably never produce `playerKilledSelf`.** The suicide paths pass `None` as the
   fifth argument. `getPositionString(None)` → `bf2.objectManager.getRootParent(None)` →
   `None.getParent()` raises. The handler's exception is cleared silently (`PythonHost.cpp:813-817`).
   **M** (inferred from the Python library copy; not run).
4. **PlayerScore is driven by scripts.** A module that writes `PlayerScoreData` directly does not
   raise event 28, so game-mode scripts are not told. That is acceptable for the admin channel,
   which only reports scores.

---

## R4. Hook design

### Facts the design rests on

- **One dispatch site, but untyped arguments.** All 42 script events funnel through
  `PythonHost::handleEvent(ScriptEvent, PyObject*)` (`PythonHost.cpp:799`). The typed C++ values
  are gone by then. A hook there can deliver typed values only if it decodes the Python wrappers
  again. **H**
- **Decoding is possible inside the engine.** `IObject` wrappers carry a CObject token that
  `parse_arg(PyObject*, IObject*&)` turns back into the object (`py/Objects.cpp:99-124`). Player
  wrappers are instances of the host's Player class, built from the player id
  (`PythonHost.cpp:230-262`). The retail class stores that id as `self.index`
  (`/tmp/inst/x/python/bf2/PlayerManager.py:60`). Ints, floats and strings are plain CPython
  objects. The assists argument is a tuple of `(player, int)` tuples (`PythonHost.cpp:269-282`). **H**
  (the `index` attribute: M, from the library copy)
- **Game state is not on this path.** It is on `EventManager` category 0 (see above).

### Proposed R4 (`[CHOSEN, not recovered]`)

The two declarations below are appended to `IServerExtension`, under its append-only rule:

```cpp
struct ScriptEventArg                 // plain data; valid only during the callback
{
    enum Kind { None = 0, Int, Float, String, Player, Object, Tuple };
    int kind;
    int i;                            // Int value; Player: the player id
    double f;                         // Float value
    const char* s; int sLength;       // String: borrowed from the Python string
    world::IPlayer* player;           // Player: getPlayerFromId(index), may be null
    world::IObject* object;           // Object: null if the wrapper's object is gone
    int first, count;                 // Tuple: a slice of the same array (assists)
};
class IScriptEventListener
{
public:
    virtual void onScriptEvent(int eventId, const ScriptEventArg* args, int argc) = 0;
protected:
    ~IScriptEventListener() {}
};
// appended after getGameStatus():
virtual bool setScriptEventListener(IScriptEventListener* listener) = 0;   // null clears; one listener
```

- **Engine side.** All of it is in `PythonHost.cpp`, plus one forwarding line in
  `GameServer::ServerExtension` (`GameServer.cpp:198`):
  - Add a file-static listener pointer and a `python::setScriptEventListener` setter.
  - In `handleEvent(ScriptEvent, PyObject*)`, after the two early returns (`:801-804`) and
    **before** the handler loop: `if (listener && args) notifyListener(eventId, args);`.
  - The decoder fills a fixed stack array of 32 entries; it counts and truncates on overflow.
  - It classifies each item as `None`, `PyInt`, `PyFloat`, `PyString`, `PyTuple` (one nested
    level), an instance of `pyHostPlayerClass()` (read `index`, resolve through `playerManager`), or
    an object found by `parse_arg`. Any decode error is cleared with `PyErr_Clear`, and the item
    becomes `None`.
  - Retail Python behaviour does not change.
- **Why before Python:**
  - The causal order is kept. Score events nested inside a kill's handlers come after the kill.
  - The objects are still alive. A Python handler can `object.delete` them.
  - The Python gate (`alwaysCall` or Playing state) does not apply, which matches `mm_webadmin`'s
    `alwaysCall=1` registrations.
  - An "after Python" or veto variant belongs with the later `IGameLogic` hooks, not here.
- **Coverage.** One site covers the 38 script ids that have a caller in source. The remaining 4
  (33, 35-37) have no caller. Game state is not covered; take it from `EventManager`
  `ECGameLogic` 1.
- **What still gates it:**
  - Nothing is delivered when Python is not initialized: `buildArgs` returns an empty `PyRef`
    (`PythonHost.cpp:301-302`), and `handleEvent` returns early.
  - Nothing is delivered under the `m_byte01` mute, whose producer is marked `[UNRESOLVED]` (`:848`; the field comment at `:138` calls it initialize's cached argument).
  - Normal servers always initialize Python.
- **Threading.** All the call sites traced are on the simulation thread, inside event dispatch or
  game logic. The same is true of Python, which runs without a GIL handoff here. **M**. The
  persistence responses (18-20) complete from HTTP requests polled by the server; their thread was
  not traced (**U**). They do not matter to the admin channel.
- **Reentrancy.** Dispatch nests: `pmgr_setScore` raises 28 inside 1's handlers, and
  `EventManager::postEvent` dispatches synchronously even when nested (`Core/EventManager.cpp:150-169`).
  The listener must only copy the values into the module's bounded queue. It must not call Python,
  run console commands, or create or delete engine objects inside the callback. Pointers must not
  be kept past the call.
- **Cost:**
  - No listener: one load and branch per script event.
  - With a listener: O(argc) type checks per event. Each player argument adds one
    `PyObject_IsInstance` and one `index` attribute lookup. Each object adds one `parse_arg`.
  - Script events run at tens per second at most, and each already costs one full Python call per
    handler. Positions are not events; they come from the module's per-tick snapshot.
- **Alternative B: typed per-call-site notifies.** Add a `serverExtNotify(id, typed…)` next to each
  `buildArgs`. That is 17 sites in 6 files for the 13 events the admin channel uses, or 51 sites in
  19 files for all of them.
  - For: real C++ types, no Python decode, and it works with Python down.
  - Against: it touches many recovered files and enlarges review, and a newly recovered call site
    can be missed silently. Not recommended as the first version.

### What the module can already get without R4

`EventManager` dispatch is synchronous on the main thread (`Core/EventManager.cpp:127-169`). The
module already registers listeners on it (`openbf2-server-ext/src/Module.cpp:335-340`).

| Need | EventManager source | Payload | Gap | Conf. |
|---|---|---|---|---|
| Game status | `ECGameLogic` 1, `ServerGameLogic.cpp:317` | `int*` status (possibly delayed) | none, and it avoids the Pause latch | H |
| Player created | `ECGame` 4, `GameServer.cpp:530`, `:1298` | `IPlayer*` | fires before `setPeerAddress` (`:1303-1305`); read the IP through `IGameServer::getPlayerConnectionIp` or on the next tick | H |
| Player removed | `ECGame` 2, `GameServer.cpp:3997` | `IPlayer*` | none | H |
| Spawned | `ECGame` 6, `ServerGameLogic.cpp:2132` (`spawnPlayer`) | `IPlayer*` | the soldier position at that moment is untested; read it on the next tick | M |
| Enter vehicle | `ECGame` 9, `GameServer.cpp:3815` | `IPlayer*` (`getVehicle()` is the new object) | none; fires for soldiers too | H |
| Exit vehicle | `ECGame` 11, `GameServer.cpp:3919` | `IPlayer*` | the old vehicle is not in the payload; the module tracks the last vehicle per player | H |
| Kill | `ECGameLogic` 3, `GameLogicDamage.cpp:467` | `KilledByData*`: killer and victim ids, killer-alive flag, killer vehicle template id, weapon template id (`Game/GameEvents/KilledByEvent.h:16-30`) | no assists; suicides via `killPlayer` alone are not reported here | H |
| Death | `ECGame` 8, `GameLogic.cpp:3061` | `int[2]` {player id, flag} | none | H |
| Team change (any cause) | `ECGameLogic` 8, `GameLogic/Player.cpp:5132` | `int[2]` {player id, new team}; posted before `m_team` is stored | broader than event 7, which is better for the admin channel | H |
| Chat | `ECNetwork`, node id 11 (`GameServer.cpp:1380-1381`, `:4566-4567`) | `StringReceivedEvent*` (`Game/Common/StringManager.h:46`) | engine-class layout, valid in a same-build module only | M |
| CP capture, score, vehicle destroyed | none | — | poll `IControlPoint`, `PlayerScoreData` and armor status per tick | M |
| Revive (medic identity) | none | — | **R4 or alternative B only** | H |

**Verdict.** R4 is not strictly required. Everything except the reviver's identity can be built from
`EventManager` and per-tick polling. R4 is still the cheaper route: it is one engine site, and it
gives the same events with the same arguments as retail scripts see, without a reconstruction layer
in the module.

---

## Commands

Module-reachable singletons, all through `getSingleton`:

- Already used by the module (`src/Module.cpp:298-303`): `EventManager`, `OpenBF2ServerExtension`,
  `PlayerManager`, `ObjectManager`, `Game` (→ `IGameServer`), `System`.
- `"MainConsole"` is registered by `Core/ModuleRegistry.cpp:542` and fetched the same way in
  `Core/DllModule.cpp:79`.

| Command | mm_webadmin implementation | Typed engine path for the module | Conf. |
|---|---|---|---|
| `rcon` | `host.rcon_invoke` (`mm_webadmin.py:155-161`) | **Needs the console.** `rcon_invoke` itself is `mainConsole->executeLine(cmd + "\n", out, false×5)` (`PythonHost.cpp:1766-1782`). From the module: `getSingleton("MainConsole")` → `Console::executeLine(const std::string&, std::string& out, bool×5)` (`IO/Console/Console.h:41`, virtual), on the main thread, outside any event callback. The output is in `out`. The console access level used by `rcon_invoke` is whatever is current; that matters for privileged commands and is **U**. ModManager commands (`iga …`) are not engine console commands. BF2WebAdmin sends those over the classic rcon port through `RconClient` (`Abstractions/BaseModule.cs:84-88`), which this change does not affect. | H |
| `rconresponse` | as `rcon`, reply `response\t<code>\t<out with \n→\b>` | same | H |
| `pm` | `host.rcon_feedback(p.index, msg)` (`:173-177`) | `rcon_feedback` posts `ECNetwork` 19 with a local `{int connectionId; std::string text}` (`PythonHost.cpp:1787-1803`), and `GameServer` sends it as a data block (`GameServer.cpp:1439-1453`). The module can post the same event through `EventManager::postEvent(ECNetwork, 19, &req, 0)`, but the struct layout is local to that file (`[INFERRED]` there). Cleaner: **an appended `IServerExtension::sendConsoleFeedback(playerOrConnId, text)`**. `mm_webadmin` passes a player index where the parameter is a connection id; whether the two are equal is **U**. | M |
| `position` | `getRootParent(p.getVehicle()).setPosition` (`:703-707`) | Walk to the root with `IObject::getParent` (inline, `IObject.h:157`), then `IObject::setAbsolutePosition` (slot 28, `IObject.h:81`). That is the same call as `object_setPosition` (`py/Objects.cpp:593-607`). | H |
| `rotation` | `root.setRotation` (`:710-714`) | `getAbsoluteTransformation` / `setAbsoluteTransformation` (slots 33/35) on a root, `setRelativeTransformation` (slot 59) on a child. The angles use the negated, reversed ZXY convention (`py/Objects.cpp:632-655`). `setRotationZXY` is engine code, so the module needs its own matrix math. | H |
| `damage` | `root.setDamage(d)`; below 10 also the soldier, plus `findId` + `object.delete` ×2 (`:723-738`) | `IObject::getArmorComponent()` (`IObject.h:161`) → `IArmorGLComp::setHitPoints` (`Game/Objects/GameLogicComp/IArmorGLComp.h:45`), the same call as `object_setDamage` (`py/Objects.cpp:706-722`). For the delete, the console with the real grid id (`object.active id<N>` + `object.delete`); no `findId`. | H |
| `rank` | `sendRankEvent` + `p.score.rank = n` (`:747-756`) | **Needs a new appended method.** `sgl_sendRankEvent` builds an engine `RankEvent` (`ServerGameLogic.cpp:2627-2641`), and `PlayerScoreData::setRank` is non-virtual and touches the nametag manager (`Scene/Object/PlayerScoreData.cpp:66-72`). Proposed: `IServerExtension::setRank(playerId, rank, bool sendEvent)`. | H |
| `medal` | `sendMedalEvent(p, …)` (`:741-744`; always fails: `p` is undefined) | **Needs a new method.** It builds an engine `MedalEvent` (`ServerGameLogic.cpp:2610-2624`). Proposed: `IServerExtension::sendMedalEvent(playerId, medal, value)`. | H |
| `gameevent` | `bf2.gameLogic.sendGameEvent` → `host.sgl_sendGameLogicEvent` | **Needs a new method.** It builds `PostRemoteEvent(ECGameLogic, …)`; event ids above 24 are refused (`ServerGameLogic.cpp:2540-2559`). Proposed: `IServerExtension::sendGameLogicEvent(playerId, event, data)`. | H |
| `hudevent` | `sgl_sendHudEvent` | **Needs a new method.** It builds `PostRemoteEvent(ECHud, …)`; ids above 78 are refused (`ServerGameLogic.cpp:2644-2662`). Proposed: `IServerExtension::sendHudEvent(playerId, event, data)`. | H |
| `score` | `p.score.rplScore/kills/deaths/score = …` → `pmgr_setScore` | Write `IPlayer::getScoreData()` fields (`PlayerScoreData.h:49-54`): `m_score`, `m_field04` (rplScore), `m_field14` (kills), `m_field10` (deaths), the same mapping as `PythonHost.cpp:1474-1505`. This relies on the class layout, which holds only in a same-build module. It does not raise event 28 (see A1). | M |
| `team` | `p.setTeam(t)` → `pmgr_p_set` → `IPlayer::setTeam((t!=1)+1, false)` (`PythonHost.cpp:1310-1314`), then kill and emit `playerChangeTeam` | `IPlayer::setTeam(int, bool)` (virtual, `IPlayer.h:96`), then `damage 1`. `ECGameLogic` 8 reports the change. | H |
| `timerinterval` | restarts the Python timer | Internal to the module (snapshot rate). | H |

Construction of `GameEvent` subclasses stays in the engine. A module can call
`IGame::sendGameEvent*` (`Game/IGame.h:116-121`), but it cannot instantiate `RankEvent`, `MedalEvent`
or `PostRemoteEvent`, because their vtables and bodies live in the executable. Hence the four small
appended wrappers. Two more should be appended at the same time for A1: `getTeamName`/`getTickets`
and `getCdKeyHash`.

---

## A4. A second backend in BF2WebAdmin

### The seam

- **Modules.** They depend on `IGameServer` (state, `GameWriter`, `GameReader`), on Mediator events
  (`IHandleEventAsync<…>`, for example `Modules/BF2/Heli2v2Module.cs:25-50`) and on `IGameWriter`
  methods. None of them parses event lines.
- **`IGameReader`** (`Abstractions/IGameReader.cs:3-9`) is string-only: `QueueMessage(string)` plus
  recording. `GameReader` splits on tab, routes `response` to `SetRconResponse`, and maps the other
  names to `IEventHandler` calls with string-to-int/Position parsing (`GameReader.cs:85-118`,
  `:172-221`). `EventHandler` turns those into `IGameServer` updates (`EventHandler.cs`).
- **`IGameWriter`** (`Abstractions/IGameWriter.cs:6-26`) has typed methods: `SendTeleport`,
  `SendRank`, `SendScore` and the rest. It also has two text ones, `SendRcon(params string[])` and
  `GetRconResponseAsync(string)`. `GameServer.SetReconnectedAsync(IGameWriter, …)` swaps the writer
  (`Entities/GameServer.cs:95-110`), so a different writer is supported.
- **`SocketServer`** is concrete. It owns the TCP listener, the private-IP check, the line reader
  (`SocketServer.cs:190-297`), and server identification from the first `serverInfo` line
  (`:514-566`). The DI registration is a single `ISocketServer` (`Program.cs:96-100`), which
  controllers, the hub and the message bus consume.

### What a native backend implements

1. **A listener or session** for the native framing, HMAC challenge and bounded queues. Either it is
   a second listener inside `SocketServer`, or a composite `ISocketServer` merges both registries so
   `GetGameServer` still sees every server. `Program.cs` and `SocketServer` change; modules do not.
2. **A reader adapter.** The least change is to turn each native event into the same tab-separated
   string and call `GameReader.QueueMessage`. That keeps `GameReader`, `EventHandler`, recording and
   modules untouched. The adapter must reproduce `mm_webadmin`'s shaping:
   - the `playerKilledSelf` / `playerTeamkilled` / `playerKilled` split, with `"roadkill"`;
   - positions as `x/h/y`, parsed by `Position.Parse`;
   - `serverInfo` first, with the map list as `name|size,…` (gpm_cq only today);
   - chat channel names and the `stripmessage` cleanup.

   A later step could call `IEventHandler` directly with typed values. That needs `GameReader`'s
   handler table to be shareable; it is not needed for v1.
3. **A `NativeGameWriter : IGameWriter`.** Map each method to a native command. `GetRconResponseAsync`
   correlates by request id. `SendText` keeps building `game.sayall "…"` through rcon
   (`GameWriter.cs:289-307`).

### Text-protocol assumptions that modules depend on

| Assumption | Where | What the backend must do |
|---|---|---|
| `GetRconResponseAsync` returns lines joined with `\b` | `Heli2v2Module.cs:1668-1670` splits on `'\b'` and matches `ID ([0-9]+)` against `object.listObjectsOfTemplate` output | Join console output lines with `\b` (or keep the exact `mm_webadmin` transform, `\n`→`\b` minus the trailing newline). The typed "list objects of template" query can come later as a new writer method, and Heli2v2 changes only if someone chooses to. |
| `SendRcon` then `GetRconResponseAsync` share console state, in order | `Heli2v2Module.cs:888-891` (`ObjectTemplate.activeSafe …`, then a getter); `:1677-1680` and `BF2Module.cs:381-405` (`object.active id<N>`, then `object.delete`); `RconScript.InitServer` | Run `rcon` and `rconresponse` from one FIFO queue on the main thread, never reordered or interleaved, as the single-socket `mm_webadmin` does today. |
| `RootVehicleId` equals the console object id | `BF2Module.cs:381-405`, `ChopperMayhemModule.cs:102-115`, `Heli2v2Module.cs:574-594` use `object.active id{RootVehicleId}`; `GameServer.GetVehicle` keys vehicles on it (`Entities/GameServer.cs:337-356`) | Send the grid id (`IObject::getObjectId`). It is better than today: `findId` returns −1 on a miss, and every −1 vehicle then collapses into one `Vehicle`. Exit and destroy ids become real; `EventHandler` ignores them (`EventHandler.cs:192-203`). |
| Windows-1252 text | `GameWriter.cs:39-42`; `SendText` adds `§C1001` colour codes | Native UTF-8 on the wire; the module converts to 1252 before the console or chat, and the backend decodes to `string`. |
| Rcon output lines and `ERROR:` lines are ignored | `GameReader.cs:110-117` | They can be dropped. |
| Position precision `%.1f` | `mm_webadmin.py:759-766` | `f32` is finer. No module was found that relies on the rounding (only a grep was done; **U** for subtle replay/compare logic in Heli2v2's path recording). |
| Heartbeat via `GetRconResponseAsync("physics.gravity")` | `GameWriter.cs:322-338` | Works unchanged through the console path. |

**A4 verdict.** No game module needs to change. The work is a new session and listener (or a
composite `ISocketServer`), a string-producing reader adapter and an `IGameWriter`. The backend must
keep `\b`-joined responses, strict rcon ordering and grid ids for vehicles.

---

## Unverified items

- Whether retail fires script events 33, 35, 36 and 37 at all (needs IDA/Ghidra).
- Whether retail has the same Pause latch as the recovered `PythonHost.cpp:863-877` (the recovery
  comment says so; not re-checked against the binary).
- What `PlayerKilled`'s fifth argument holds for infantry kills (the projectile path into
  `ServerGameLogic.cpp:2900-2965` was not traced in full).
- That `Player.index` is the attribute R4's decoder should read. It was read from an unattributed
  copy of the retail `bf2` package.
- Whether `gcd_getkeyhash` returns a hash on OpenBF2 servers.
- Whether `rcon_feedback`'s connection id equals the player index `mm_webadmin` passes.
- The console access level when the module calls `executeLine`, and which commands it gates.
- The thread that completes persistence responses (events 18-20).
- `ECGame` 6 (spawn): whether the soldier is already positioned when it fires.
- Category choice and runtime: none of the above was run. The module is Linux-only today
  (`openbf2-server-ext/CMakeLists.txt:29`).
