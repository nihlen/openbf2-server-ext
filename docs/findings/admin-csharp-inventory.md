# Admin channel: BF2WebAdmin C# inventory

Read-only investigation, 2026-10-22, for phase 2 of `docs/admin-channel-draft.md` (draft 2, approved).
Nothing in BF2WebAdmin was changed, built or run, and no container was touched.

- **BF2WebAdmin:** `C:\Data\Projects\DotNet\BF2WebAdmin` working tree (branch head `a869c5c`, with the
  author's uncommitted edits to `BF2WebAdmin.sln` and `Constants/RconScript.cs`). C# paths are relative to
  `src/BF2WebAdmin.Server/` unless they start with another project name. `mm_webadmin.py` is
  `examples/docker-compose/bf2/server-1/server/admin/modules/mm_webadmin.py`.
- **Bot-channel C# codec for comparison:** `C:\Users\Alex\.t3\worktrees\bf2-bots\serverext-sidecar`,
  `src/Bf2Bots.ServerExt/Protocol.cs` (514 lines: `MessageType`, records, `Protocol.Encode/Decode`, `FrameDecoder`)
  and `tests/Bf2Bots.ServerExt.Tests/ProtocolTests.cs` (golden hex frames from the Python reference client).
- Read means read in source. **Guess** marks anything inferred and not checked.

## 1. The current backend

### Pipeline

```
TCP 4300 ─ SocketServer ─ line ─ GameReader ─ IEventHandler ─ EventHandler ─ IGameServer (GameServer) ─ Mediator ─ modules
modules ─ IGameWriter (GameWriter) ─ text command line ─ same socket ─ mm_webadmin.py
```

| Part | Where | What it does |
|---|---|---|
| DI wiring | `Program.cs:96-100`, `BF2WebAdminService.cs:24-40` | One singleton `ISocketServer`, constructed from `ServerSettings` (`IpAddress`, `Port`, `GameServers` from config); a `BackgroundService` calls `StartAsync` |
| Listener | `SocketServer.cs:125-188` | `TcpListener(IPAddress.Any, port)`. Accepts a client only if its IPv4 is in the server list **or is any private IP** (`:139-148`) |
| Line reader | `SocketServer.cs:190-297` | `StreamReader.ReadLineAsync` per connection (default UTF-8 decoding, `:204`). The first line must be `serverInfo` (`:236-242`); every line goes to `GameReader.QueueMessage` (`:244-247`). On disconnect it waits 1 min and re-triggers the game server (`:286-296`) |
| Reconnect trigger | `SocketServer.cs:110-123`, `:569-653` | For every configured server, opens classic rcon (`RconClient`) and sends `wa connectprivate <port>` or `wa connect <ip> <port>` until `mm_webadmin` dials back. Polly retry 1 min, then 10 min |
| Server registry | `SocketServer.cs:34`, `:41-43`, `:655-695` | `ConcurrentDictionary<string, GameServerConnectionContext>` keyed `"<publicIp>:<gamePort>"`. Add/remove from the UI clears the cache and restarts the reconnect loop |
| Line parser | `GameReader.cs:50-118`, table `:172-216` | Unbounded channel; splits on `\t`; `response` goes to `SetRconResponse` (`:92-95`); 27 names map to `IEventHandler` calls with `int.Parse`, `Position.Parse`, `Rotation.Parse` (`:219-221`); unknown lines (raw rcon output, `ERROR:`) are dropped (`:110-117`) |
| Recording | `GameReader.cs:120-170`; started by `Modules/BF2/WebModule.cs:386-396` | Writes the raw text lines, plus a synthesized state header (`:149-157`), to `./Data/gameevents-*.txt`. Nothing in the repo replays them (searched) |
| Event dispatch | `EventHandler.cs:8-231`; interface `Abstractions/IEventHandler.cs` | Turns typed arguments into `IGameServer` mutations. CP capture/neutralise, ticket status and revive throw `NotImplementedException` (`:72-88`, `:137-140`; the reader swallows it, `SocketServer.cs:261-264`). `vehicleDestroyed` is ignored (`:198-203`). Enter-vehicle drops soldier templates (`:184-185`) |
| Domain model | `Entities/GameServer.cs`, `Abstractions/IGameServer.cs`; entities in `BF2WebAdmin.Common/Entities/Game/` | Lists of maps, teams, players, vehicles, projectiles; chat and event ring buffers. Every mutator publishes a record from `GameEvents.cs:6-25` on the per-server `Mediator`, which modules consume via `IHandleEventAsync<T>` |
| Command sender | `GameWriter.cs`; interface `Abstractions/IGameWriter.cs` | Unbounded channel, one writer task; Windows-1252 encoding + `\n` (`:39-42`, `:94-103`). Typed methods are formatted as text commands (`:180-281`); `SendRcon` (`:119-123`); `GetRconResponseAsync` with `response<N>` codes wrapping at 10 000, 5 s timeout (`:131-155`); `SendText` builds `game.sayall "[§C1001netsky§C1001] …"` (`:289-307`); `SendHeartbeat` = `rconresponse physics.gravity` (`:322-338`) |
| Classic rcon, outside the socket | `RconClient.cs`; `Abstractions/BaseModule.cs:84-88`; `Modules/BF2/BF2Module.cs:509-525`; `Hubs/ServerHub.cs:184-185` | ModManager rcon on `RconPort` with `RconPassword`: web/Discord `!` commands become `iga kick/ban/warn`, `map …` (`BaseModule.cs:58-70`); the in-game admin list is resynced on every map change (`iga listAdmins/delAdmin/addAdmin`). Not part of the 4300 protocol |

### Identification and authentication today

- **No authentication.** Any private-range peer is accepted (`SocketServer.cs:141-142`), and the first
  `serverInfo` line is trusted.
- **Identity = connecting IP + game port.** `GetGameServerFromInitialMessageAsync` (`SocketServer.cs:514-567`)
  takes the game port from `serverInfo` field 3, matches a `ServerInfo` by exact IP, or any IP if the peer is
  private (container setups), then keys the server as `"<publicIp>:<gamePort>"`, substituting the configured
  WebAdmin IP when the peer is private (`:543-544`). `GameServer.Id` is recomputed the same way
  (`Entities/GameServer.cs:28`).
- **Server list:** `Server` table rows (via `IServerSettingsRepository`) merged with `ServerSettings:GameServers`
  from config, cached 5 min (`SocketServer.cs:63-100`). `Server` columns: `ServerId` (PK, `nvarchar(20)`,
  value `"<ip>:<gamePort>"`), `ServerGroup`, `IpAddress`, `GamePort`, `QueryPort`, `RconPort`, `RconPassword`,
  Discord fields (`BF2WebAdmin.Data/Entities/ServerSetting.cs:6-19`; local test row
  `BF2WebAdmin-localtest/init-db.sql:76-113`). `BF2WebAdmin.Data/Schema.sql:130-137` is stale (two columns);
  the EF migration `20230125001531_UpdateServer` and `SqlServerSettingsRepository.cs` hold the real shape.
- `ServerGroup` selects the enabled modules (`ServerModule`) and the player auth table (`ServerPlayerAuth`,
  keyed by **CD-key hash**).

### How the domain model is fed

| Domain item | Fed by (text event → `GameServer` method) | Notes |
|---|---|---|
| Server name, ports, max players, map list | `serverInfo` → `UpdateServerInfoAsync`, `UpdateMapsAsync` (`EventHandler.cs:20-33`) | `UpdateServerInfoAsync` also creates the `ModManager` (`GameServer.cs:113-153`). Map list is `name|size,…`, gpm_cq only (`mm_webadmin.py:779-786`) |
| Game state, current map, teams | `gameState*` → `UpdateGameStateAsync`; `gameStatePlaying` → `UpdateMapAsync` (`GameServer.cs:189-202`) | Teams are created only here (ids 1/2 + names). `UpdateMapAsync` resends `RconScript.InitServer` on every map |
| Players | `playerConnect` → `AddPlayerAsync` (`EventHandler.cs:105-117`) | `IPAddress.Parse(ip)`, `Hash`, team lookup by id. After 1 min uptime it **reassigns the team itself** and sends `team` (mm_autobalancer workaround, `GameServer.cs:204-213`) |
| Position, rotation, ping | `playerPositionUpdate` at 4 Hz → `UpdatePlayerAsync` (`GameServer.cs:220-237`) | The position of the player's current vehicle root (`mm_webadmin.py:539-542`) |
| Alive/dead | `playerSpawn` → `SetPlayerSpawnAsync`; `playerKilled*`/`playerDeath` → `SetPlayerKillAsync`/`SetPlayerDeathAsync` (`GameServer.cs:299-323`) | Suicide is guessed from `IsAlive` (`:318-319`, "always thinks it's suicide"). A spawn straight into a vehicle is synthesized from `enterVehicle` (`:247-253`) |
| Vehicles | `enterVehicle` → `GetVehicle` + `UpdatePlayerVehicleAsync` (`GameServer.cs:239-259`, `:337-358`) | Keyed on `RootVehicleId` (`findId` console id, −1 on a miss, so misses collapse into one `Vehicle`). Exit clears; destroy ignored. `_vehicles` is never pruned |
| Team | `playerChangeTeam` → `UpdatePlayerTeamAsync` | Plus `FixTeamMismatchAsync`, which re-queries GameSpy to repair teams (`GameServer.cs:398-421`, used by `Heli2v2Module.cs:1132`) |
| Score | `playerScore` → `UpdatePlayerScoreAsync` | total, team (rplScore), kills, deaths |
| Projectiles | `projectilePositionUpdate` (TV missiles only) → `GetProjectile`, `UpdateProjectileAsync` | Owner is **guessed**: nearest player in an `ahe_` vehicle (`GameServer.cs:376-396`) |
| Chat | `chatServer`/`chatPlayer` → `AddMessageAsync` → `ModManager.HandleChatMessageAsync` (dot-commands) | Channel is the engine's channel name string |
| Reconnect | `SetReconnectedAsync` clears everything and swaps the writer (`GameServer.cs:95-111`); `mm_webadmin` replays state (`mm_webadmin.py:134-142`) | |
| Unknown player id | `GetPlayer` returns `new Player()` instead of null (`GameServer.cs:360-364`) | Masks out-of-order or unknown ids |

## 2. Features that depend on text-protocol details

"Draft message" names refer to draft 2. **Gap** means the draft as written does not carry what the feature needs.

| Feature (file:line) | Text-protocol detail it relies on | Replacement in draft 2 | Gap? |
|---|---|---|---|
| Heli2v2 `.nofences` (`Modules/BF2/Heli2v2Module.cs:1645-1687`) | `GetRconResponseAsync("object.listObjectsOfTemplate …")`, split on `'\b'`, regex `ID ([0-9]+)`, then `object.active id<N>` + `object.delete` per match | `ListObjects` + `DeleteObject` | **Gap.** Fences and lamp posts are static map objects. Draft 2 addresses objects by networkable id; static objects very likely have none (**guess**; the local test README notes spawned objects have no networkable). `ListObjects`/`DeleteObject` must accept the console (grid) id, or the module must document what happens to objects without a networkable |
| Heli2v2 noclip (`Heli2v2Module.cs:568-606`, `RconScript.cs:287-305`) | `object.active id{RootVehicleId}` + `object.hasCollision` + four `object.armor.hpLost*` | none typed; raw `Console` | **Gap.** `RootVehicleId` becomes a networkable id (draft 2, "The C# side"), but the console resolves `id<N>` through the grid (findings `admin-channel-a1-a4.md` row `enterVehicle`; `q5-q9.md:124`: console id 29328 vs networkable 1819). Either the Vehicle entity carries the console id too, or typed commands are needed (`SetCollision`, armor properties) |
| Freeze/unfreeze (`Modules/BF2/BF2Module.cs:370-410`); ChopperMayhem NASA (`Modules/BF2/ChopperMayhemModule.cs:95-120`, currently a no-op `object.active`) | `object.active id{RootVehicleId}` + `object.setIsDisabledRecursive` | none typed | **Gap**, same id problem |
| Heli2v2 TV-missile get/set (`Heli2v2Module.cs:883-975`), MG on/off, flares, artillery, damage tweaks (`RconScript.cs`), ChopperMayhem template edits (`ChopperMayhemModule.cs:198-230`), gravity (`BF2Module.cs:199-209`), `InitServer` on connect and map change (`GameServer.cs:91`, `:110`, `:198`; `Heli2v2Module.cs:624`, `:1090`) | Raw `ObjectTemplate.*`/`physics.*` strings; `activeSafe` then a getter via `GetRconResponseAsync` relies on **FIFO across fire-and-forget and request/response** | `Console` ("runs in order") | No id dependency. Needs one ordered queue for every `Console` command, whether or not WebAdmin awaits the result, and a result as lines (not `\b`) |
| Object spawning: `BaseModule.SpawnObject` (`Abstractions/BaseModule.cs:23-37`), `MapModule.SpawnObject` (`Modules/BF2/MapModule.cs:137-155`), ChopperMayhem towers, `.crash` (`BF2Module.cs:436-459`) | `RconScript.AddObject` (`RconScript.cs:255-285`): ObjectSpawner template + `object.absolutePosition x/h/y` + `object.rotation y/p/r` from `Position/Rotation.ToString()` (3 decimals, invariant culture) | `SpawnObject` (template, position, rotation) | Partly. `.clear`/`.undo` use `admin.restartmap` because deletes don't reach clients (`MapModule.cs:159-189`; local test README). `DeleteObject` has the same engine limitation; nothing in WebAdmin can switch to it for spawned objects (**guess**: unless the module's spawn path creates networkables) |
| Kill/switch (`BaseModule.cs:39-56`; `ChopperMayhemModule.cs:271`; `Heli2v2` pad `:627-660`) | `damage <id> 1`: in `mm_webadmin` this sets the root's damage, and below 10 also the soldier's and **deletes the root vehicle** (`mm_webadmin.py:723-738`); only if alive. `team` kills via `damage 1` and emits `playerChangeTeam` (`:254-263`) | `KillPlayer`, `SetHealth`, `SetTeam(kill)` | **Gap in semantics.** Draft 2 does not say whether `KillPlayer` deletes the vehicle, or whether `SetHealth` below 10 kills. Heli2v2 switching relies on today's behaviour (**guess** for how much) |
| Teleport/rotate (`Heli2v2Module.cs:649-658`, `:860-861`, `:1385-1398`; `Modules/BF2/JailModule.cs:70`, `:100`; `ChopperMayhemModule.cs:298-299`; `BF2Module.cs:113-121`, `:185-195`, `:338-355`; `MapModule.cs:188`) | `position <idx> x h y` and `rotation <idx> y p r` as **separate** commands on the player's root vehicle | `Teleport` (position and rotation) | **Gap:** most callers send only one of the two. `Teleport` needs optional position and optional rotation (or WebAdmin re-sends the last streamed value, which is stale) |
| Rotation values (stream → `Player.Rotation` → `SpawnObject`/`SendRotate`, `MapModule.cs:113-128`, `BF2Module.cs:113-121`) | Python's yaw/pitch/roll (negated, reversed ZXY, degrees; findings A1 row `playerPositionUpdate`) | float32 rotation in streams and commands | **Gap (spec):** the draft must fix the convention as Python's yaw/pitch/roll, the same for streams, `Teleport` and `SpawnObject` |
| Positions everywhere: `Position.Parse("x/h/y")` (`GameReader.cs:220-221`) | `%.1f` strings | float32 | None. Constants parsed in modules (`Heli2v2Module.cs:142-143`) and DB JSON (`:1071`) are storage, not wire |
| **Per-player position** (Heli2v2 path recording `:1362-1380`, stalk, anti-NASA `:1385-1398`, playback `:860`; ChopperMayhem portals and safe-area check `:262-300`; `MapModule` `.o` spawns at the player; `BF2Module` snapshots; WebModule live map) | `playerPositionUpdate`: per player, root of current vehicle, rotation, ping | Player state has **no position**; Vehicle stream has position | **Gap.** Add root position/rotation (and root networkable id) to the player state, as the bot channel's TickState does (`protocol.md:274`), or define that infantry soldiers appear in the Vehicle stream |
| Projectile owner (`GameServer.cs:376-396`) | Nearest `ahe_` player guess | Projectile `owner player` | None; removes the guess |
| `RootVehicleId` as vehicle key (`GameServer.cs:337-358`; DTO `BF2WebAdmin.Shared/Communication/DTOs/VehicleDto.cs:7`) | `findId` console id | Networkable id | None for keying. See the console-id gaps above |
| Kill classification (`Heli2v2Module.cs:455-530`, `:1318-1332`; `GameServer.cs:316-323`; `ChopperMayhemModule.cs:262-264` "victimPosition bugged") | Event name split (`playerKilled`/`Teamkilled`/`KilledSelf`), `"roadkill"` weapon, **kill before death** ordering, crash inferred from a `playerDeath` with no prior kill, wrong victim position | `Kill` with kind, both positions, both vehicles | Draft fixes the content. **Gap (spec):** state the order of `Kill` and `Death` for one victim; Heli2v2 merges both into one round record and relies on `KillerWeapon = weapon ?? previous` |
| Spawn into a vehicle (`GameServer.cs:247-253`) | No `playerSpawn` when spawning straight into a vehicle | `Spawn` | **Check:** script event 23 fires only for soldiers (findings A1); `ECGame` 6 fires on every `spawnPlayer`. The draft should say `Spawn` covers vehicle spawns |
| Team changes (`GameServer.cs:204-213`, `:398-421`) | Missed autobalance team changes, fixed by a local guess and a GameSpy query | `TeamChange` from any source | None; both workarounds can go |
| Score (`EventHandler.cs:131-135`; `ChopperMayhemModule.cs:276-277`; `MapModule.cs:166-175`; `BF2Module.cs:160-176`) | `playerScore` and `score` carry **team score (rplScore)** | Player has score, kills, deaths, team kills; `SetScore` has score, kills, deaths | **Gap:** add team score (rplScore) to Player state, `ScoreChange` and `SetScore`. `Score.Team` in WebAdmin is team score, not team kills |
| Chat (`Heli2v2Module.cs:1106` requires `Channel == "Global"`; `BF2WebAdmin.Common/Entities/Game/Message.cs:16-24`) | Channel name strings (`Global`, `Team`, `Squad`, `ServerMessage`, `ServerTeamMessage`), flags as a string, `stripmessage` removal of `HUD_TEXT_CHAT_*`/`*DEAD*` (`mm_webadmin.py:788-797`) | `Chat` (channel, player, text, flags) | **Spec:** say whether `channel` is the engine string or an enum, and where the prefix stripping happens |
| Text encoding (`GameWriter.cs:39-42`; `SocketServer.cs:204`; `Message.cs:16`) | Sent as 1252, but **received as UTF-8**, so 0xA7 (`§`) arrives as U+FFFD and `Message.Text` maps `�` back to `§` | UTF-8 both ways; module converts at the engine | None. The module must map U+00A7 to 0xA7 so `§C1001` colour codes in `SendText`/modules keep working, and `Message.cs:16` can drop the `�` hack |
| Map list (`EventHandler.cs:20-33`; `GameServer.cs:189-201`) | `name|size`, gpm_cq only; `UpdateMapAsync` throws on a map not in the list (`Map.Index` on null, `:200`) | `Welcome`: map list for every mode | **Check:** entries need mode and size (`Map.Size`, `MapDto`) |
| Startup order (`SocketServer.cs:236-242`; `EventHandler.cs:114`) | `serverInfo` first, then game state with team names, then players | `Hello`…`Welcome`, then `Snapshot` | None, if `Welcome` carries team names before the `Snapshot` players |
| Rcon response correlation (`GameWriter.cs:131-173`) | `response<N>` codes, 10 000 wrap, 5 s timeout | Request id + typed result | None |
| Heartbeat (`BF2Module.cs:574-595`, `GameWriter.cs:322-338`) | `rconresponse physics.gravity` every 25 s | none | **Minor gap:** no keepalive/ping message in the draft. A `Console` round trip works, but a protocol ping is cleaner |
| Event recording (`GameReader.cs:120-170`, `WebModule.cs:386-396`) | Records the text lines | none | Internal: record decoded frames or domain events instead |
| Player identity (`BF2Module.cs` auth and country lookup; `ServerPlayerAuth`; Heli2v2 team hashes and `MatchRoundPlayer.PlayerHash`) | `playerConnect` carries IP and CD-key hash | Player IP and hash only with `adminSendIdentity` | **Requirement:** WebAdmin cannot work without the hash (it is the player key for auth, matches and the DB). WebAdmin servers need `adminSendIdentity 1`, and the hash must exist on OpenBF2 (unverified, findings A1). Bots need a defined empty hash/IP; `IPAddress.Parse` throws on `"N/A"` today (`EventHandler.cs:112`) |

## 3. Coverage differences

### Used by WebAdmin today, not covered by draft 2

1. Per-player root position/rotation in the player stream (section 2).
2. Team score (rplScore) in state, `ScoreChange` and `SetScore`.
3. Console (grid) ids for vehicles and static objects, or typed replacements for collision, armor
   `hpLost*` and `setIsDisabledRecursive`.
4. `Teleport` with position only or rotation only.
5. Exact `KillPlayer`/`SetHealth` semantics (vehicle deletion below 10 hit points).
6. A keepalive/ping (today: the 25 s `physics.gravity` heartbeat).
7. Mode and size per map in the map list.
8. Ordering between `Kill` and `Death`, and ordering of typed commands relative to `Console` (for example
   `MapModule` restarts the map then rewrites scores; `SwitchPlayer` sets the team then kills). **Suggestion:**
   all commands execute on the game thread in arrival order.
9. Out of the channel and untouched by the draft: ModManager rcon on `RconPort` (`iga` kick/ban/warn/admin
   sync, `map`). It keeps working only while ModManager and its rcon run on the OpenBF2 server. Kick, ban and
   map could later move to the draft's typed `Kick`/`Ban`/`Map`; the `iga` admin list cannot.
10. `pm` (`rcon_feedback`): in `IGameWriter` but never called. Not needed.

### In draft 2, not used by anything in WebAdmin yet

- Control points (handlers throw `NotImplementedException`, `EventHandler.cs:72-80`), Revive (`:137-140`),
  tickets and round time (`ticketStatus` dead; `EndGame` tickets ignored, `:49-52`), `RoundStart`/`RoundEnd`
  (Heli2v2 has its own round notion), Paused (enum exists, never delivered today).
- Vehicle health, velocity, destroyed and seats; `VehicleDestroyed`; `ExitVehicle` ids (ignored today).
- `Damage`, `ProjectileFired`, `ProjectileHit`; projectile velocity.
- Squad, team kills, profile id (only used by `FixTeamMismatchAsync`), disconnect reason.
- Typed `Kick`, `Ban`, `Map` (WebAdmin uses `iga` and `admin.restartmap`), `Medal`, `HudEvent` (no callers),
  `Rank` (one caller, `BF2Module.cs:111`), `Say` to a team or one player (only `sayall` today).
- `Bot` events; Kick/Ban notices; `RequestSnapshot` (new by design).
- Policies (phase 3). A Damage zone policy would replace ChopperMayhem's after-the-fact baserape punishment
  (`ChopperMayhemModule.cs:260-282`) and is the likely first user.

## 4. Suggested shape for the new backend

### Seam

Modules already depend only on `IGameServer` state, Mediator records (`GameEvents.cs`) and `IGameWriter`
(confirmed in A4; `test/BF2WebAdmin.Tests/Heli2v2ModuleTests.cs:36-48` substitutes exactly these). Keep that
as the module-facing side and put the seam below it:

- **Read side, unchanged for modules:** `IGameServer` properties and the `GameEvents.cs` records.
- **Write side for the backend:** move `GameServer`'s mutators (`UpdateServerInfoAsync` … `AddMessageAsync`,
  `GetVehicle`, `GetProjectile`, `SetReconnectedAsync`, `SetRconResponse`) out of `IGameServer` into an
  internal `IGameStateSink` that `GameServer` implements. The backend is the only caller. Add
  `ApplySnapshotAsync(Snapshot)`, which replaces the clear-and-replay in `SetReconnectedAsync`.
- **Commands:** keep `IGameWriter`'s typed methods (signatures stay, so module edits stay small), implemented by
  a new `AdminGameWriter`. Replace `SendRcon`/`GetRconResponseAsync` with `Console(string)` and
  `Task<IReadOnlyList<string>> ConsoleAsync(string)`, and add `ListObjectsAsync`, `SpawnObject`,
  `DeleteObject`, `Subscribe` (replacing `SendTimerInterval`/`CurrentTrackerInterval`).
- **Delete:** `SocketServer.cs` (keep the `ISocketServer` interface, or rename it `IGameServerRegistry`, since
  controllers, `ServerHub` and modules use it), `GameReader.cs`, `IGameReader.cs`, `EventHandler.cs`,
  `IEventHandler.cs`, `GameWriter.cs`, the `wa connect` reconnect loop, the private-IP acceptance, the team
  workarounds (`GameServer.cs:204-213`, `:398-421`) and the projectile-owner guess.

### Proposed files (new folder `AdminChannel/`)

| File | Role |
|---|---|
| `AdminProtocol.cs` | Message types, records, `Encode`/`Decode`, `FrameDecoder`. Same style as `Bf2Bots.ServerExt/Protocol.cs` (u32 length, u16 type, u16 reserved, little-endian, u16-length UTF-8 strings), admin type range and version. A copy, not a shared package: the two repos have no common library today |
| `AdminListener.cs` | `BackgroundService`: `TcpListener` on the admin port; per client, `SslStream.AuthenticateAsServerAsync` with `SslServerAuthenticationOptions { ServerCertificate, EnabledSslProtocols = Tls13, ClientCertificateRequired = false }`, a handshake timeout, then an `AdminSession` |
| `AdminHandshake.cs` | `Hello`/`Challenge`/`Auth`/`Welcome`: look up the `Server` row by the claimed server id, verify the module's HMAC and prove its own, using `HMACSHA256` and `CryptographicOperations.FixedTimeEquals`. Unknown id, no key or a bad MAC closes the connection. Exact transcript bytes come from the phase-1 layouts |
| `AdminSession.cs` | Read loop → decode → `AdminStateApplier`; bounded outbound `Channel`; request-id → `TaskCompletionSource` with timeouts; event sequence check → `RequestSnapshot` on a gap; disconnect → `SocketState.Disconnected` |
| `AdminStateApplier.cs` | Maps `Snapshot`, stream frames and events onto `IGameStateSink` (the role `EventHandler` has today) |
| `AdminGameWriter.cs` | `IGameWriter` over `AdminSession` |
| `AdminServerRegistry.cs` | The `ISocketServer` replacement: registry keyed by server id, `GetDisconnectedServersAsync` by id |
| `test/BF2WebAdmin.Tests/AdminProtocolTests.cs` | Golden frames from the module's Python reference client, as `ProtocolTests.cs` does for the bot channel; malformed-frame rejection; handshake tests with fixed nonces |

### TLS

- Config section, for example `ServerSettings:AdminChannel { Port, CertificatePath, CertificatePassword }`
  (names are a suggestion). Load with `X509CertificateLoader`/`X509Certificate2` (net9.0, which the server
  project targets).
- At startup, log the pin each server needs:
  `Convert.ToHexString(SHA256.HashData(cert.PublicKey.ExportSubjectPublicKeyInfo()))`.
  The same value goes into `adminCertSha256` in each server's `serverext.cfg`. WebAdmin stores no pin.
- Generate the self-signed certificate once (ECDSA P-256 is enough; `openssl req -x509` or
  `CertificateRequest.CreateSelfSigned`). Expiry does not matter to a pinned SPKI, but give it a long
  lifetime anyway.
- TLS 1.3 server support in .NET depends on the OS TLS stack: OpenSSL on Linux (the Docker deployment),
  Windows 11 / Server 2022 or newer on Windows. **Guess:** fine for both deployments, not checked.
- .NET's `SslStream` has no public TLS exporter API, so the HMAC transcript cannot be bound to the TLS session.
  With a pinned certificate and TLS 1.3 that is acceptable; note it in the layouts.
- `adminTls 0` (loopback tests): the listener needs a matching switch, for example a second plain port bound
  to loopback only.

### Server configuration and the DB

- **Server id = the existing `Server.ServerId` value** (`"<ip>:<gamePort>"`, for example
  `127.0.0.1:16569` in the local test). Configuring that string as the module's server id keeps every
  existing key valid: SignalR groups (`WebModule.cs:39`), log directories (`LogModule.cs:30`),
  `Match.ServerId` rows, `ServerHub` and the controllers. `GameServer.Id` returns the configured id instead of
  recomputing it from the socket.
- **`adminKey`:** add a nullable `AdminKey` column to `Server` (raw 32-byte key, hex or base64). HMAC needs the
  raw key, so it cannot be hashed; ASP.NET Data Protection could encrypt it at rest (option, **guess**). A
  server with no key cannot connect. Touches `ServerSetting.cs`, `SqlServerSettingsRepository.cs`,
  `ServerSettingsRepository.cs`, a new EF migration, `Schema.sql`, `ServerDataDto`,
  `BF2WebAdmin.Blazor/Shared/ServerEditForm.razor`, `ServerHub` add/update, and the local test
  `init-db.sql`.
- `IpAddress`, `QueryPort`, `RconPort`, `RconPassword` stay: GameSpy queries, classic rcon (`iga`) and the
  "disconnected servers" list still use them. `ServerSettings:GameServers` from config would need the key too,
  or be dropped (**suggestion:** drop it; DB only).
- `ServerId` is `nvarchar(20)`, which is one character short of `255.255.255.255:65535`. That is a latent bug
  today, not a blocker.

## 5. Rough size (guess)

| | Files | Lines (guess) |
|---|---|---|
| New: `AdminChannel/` (protocol, listener, handshake, session, applier, writer, registry) | 7 | 1 800 – 2 800 (the codec alone is 2-3× the bot codec's 514 lines, because draft 2 has about four times as many messages) |
| New: tests (golden frames, handshake, applier) | 2 – 3 | 500 – 800 |
| New: EF migration | 2 | generated |
| Deleted | `SocketServer.cs`, `GameReader.cs`, `EventHandler.cs`, `GameWriter.cs`, `IGameReader.cs`, `IEventHandler.cs` | about −1 500 |
| Modified: core | `IGameServer.cs`, `Entities/GameServer.cs`, `IGameWriter.cs`, `Program.cs`, `BF2WebAdminService.cs`, `GameEvents.cs` (kill kind) | moderate |
| Modified: modules | `BaseModule.cs`, `BF2Module.cs`, `Heli2v2Module.cs`, `ChopperMayhemModule.cs`, `MapModule.cs`, `WebModule.cs` (recording) | small to moderate; Heli2v2 is the largest (nofences, noclip, TV-missile getters, kill handling, `SendTimerInterval`) |
| Modified: entities and data | `Message.cs`, `Player.cs`/`Score`, `Vehicle.cs`, `ServerSetting.cs`, two repositories, `Schema.sql`, `ServerDataDto.cs`, `ServerEditForm.razor`, `ServerHub.cs`, `Controllers/ServerController.cs` (if the registry is renamed) | small |
| Modified: tests | `test/BF2WebAdmin.Tests/Heli2v2ModuleTests.cs` | small |

About 25 – 30 files touched and 10 – 12 new, roughly 2 500 – 3 500 new lines and 1 500 removed. Outside the
repo: the local test env's `init-db.sql` (AdminKey) and its BF2 container (OpenBF2 + module instead of
`mm_webadmin.py`), as phase 2 of the draft already says.
