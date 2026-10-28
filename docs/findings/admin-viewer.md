# Admin channel: viewer additions (2026-10-28)

Approved by the author on 2026-10-28 for a 3D viewer in BF2WebAdmin. Contract:
`docs/admin-channel-draft.md` (`PlayerState` view mode, `AimState`/`AimStream`, `WeaponFire`, `Weapons`,
`Subscribe` streams 5, 18 and 19). Admin protocol version stays 1. No engine change.

## What was added

| Item | Wire | Engine source |
|---|---|---|
| Camera view mode | `u8` at the end of `PlayerState` (1 inside .. 7 extern trace, 0 none, 255 dead or no camera) | `IPlayer::getCamera()->getViewMode()`; the server applies every player's camera keys (`ServerGameLogic.cpp:1550-1579`, from `GameServer.cpp:3788`) |
| Aim | stream 5, `0x1115 AimStream`: per living player the camera object's and the slot-0 weapon's absolute transforms, 50 bytes | the two transforms `FireComp::getProjectileOrigin` fires along (`FireComp.cpp:807-865`) |
| Gunfire | event stream 18, `0x1217 WeaponFire`: one event per shooter, weapon and projectile template per tick, with the round count, the first launch's origin and velocity; optional template filters | the projectile observer (`FireComp.cpp:1120-1129`), every server launch including bullets and flares |
| Weapons | event stream 19, `0x1218 Weapons`: a player's weapons in slots 0-2, on change, on `Subscribe` and after every `Snapshot` | `IPlayerControlObject::getActiveWeapon(0..2)` polled per tick by object identity |

## Setup

| Item | Value |
|---|---|
| OpenBF2 | `serverext/r1` at `db3a07f`, built into `~/bf2-linux-viewer` (`testenv/build-linux.sh`), `bf2` md5 `fd117ea18c958bb7691004a4bd6c1351` |
| Module | this change on `10790ce`, built into `~/serverext-build/viewmode`, `libserverext.so` md5 `5072491d732fba549b48be457d4c30ca` |
| Server | WSL, netsky `2v2-nocheck` content (`prepare-content.sh`) in `~/serverext-viewer/srv`, `dalian_2_v_2` `gpm_cq` 16; game 16583/udp, GameSpy 29915, RCON 127.0.0.1:4735, bot channel 127.0.0.1:16716, admin dial-out to 127.0.0.1:4316 (TLS, pinned); `mm_webadmin` not loaded |
| WebAdmin | `tools/adminclient/adminclient.py --scenario viewer` (players 30 Hz, aim 30 Hz, vehicles 5 Hz, fire and weapons events); logs `~/serverext-viewer/logs/` |
| Bots | `tools/testclient/serverext_client.py --scenario viewer`: a pilot and a gunner in one Z-10 hovering 60 m over the sea; the pilot cycles views and fires rockets and flares, the gunner sweeps its turret, fires its gun, cycles views and toggles to the TV launcher and back |

Ports 16567, 16569, 16571, 16700 and 16701 and the localtest were not used; no container was touched
beyond `prepare-content.sh`'s temporary one.

## Results

Unit tests: `serverext_tests` and `serverext_admin_tests` pass; the golden file has 64 frames (new:
`aim_stream`, `ev_weapon_fire`, `ev_weapons`, `cmd_subscribe_fire`). BF2WebAdmin's codec tests (232) pass
against the same file.

Second live run (`admin-viewer2.log`, `bot-viewer2.log`): **15/15 checks pass**.

| Check | Result |
|---|---|
| AimStream | 3,351 frames in 111 s (30.3 Hz), living players only, both bots flags 3 (camera and weapon) |
| View mode | pilot 1, 3, 4, 5; gunner 1, 2 (the cycle key); 255 while dead |
| Weapons | every player reported after `Subscribe` and again after `RequestSnapshot`; gunner `AHE_Z10_Gun` -> `ahe_z10_HJ8Launcher_TV` -> gun; pilot `ahe_z10_S8Launcher` + `ahe_z10_FlareLauncher`; on foot `chrif_type95`; empty when dead |
| WeaponFire | 96 events: gun 76 (one round per tick at most), rockets 8, flares 12 |
| Gun: launch direction (velocity less the root's) vs aim | weapon axis median 0.9°, max 1.3°; camera axis the same (the gunner camera rides the gun) |
| Rockets vs aim | camera axis 0.3°, weapon axis 3.8°: they fire in camera DOF, and the pilot camera is pitched about 4° from the airframe (protocol.md, `TickState` note) |
| Flares | ejected sideways at about 10 m/s own speed, about 90° from either axis, as the launcher's barrels point |
| Resync | `RequestSnapshot` mid-run; 0 sequence gaps |
| Fuzz | `adminclient.py --fuzz` against the same server: pass, a clean link answers Ping afterwards |

The first run lost its heli (the script held the nose down for the whole run and flew into the ground); its
rocket numbers matched (camera 0.0°, weapon 4.0°).

## Cost

Module stats lines (1,800-tick windows) during the runs:

| Window | Admin channel's own cost | Whole handler |
|---|---|---|
| Run 1, crew firing | p50 20 µs, p99 40 µs, max 690 µs | p50 130, p99 250, max 1,186 µs |
| Run 2, crew firing | p50 20 µs, p99 40 µs, max 102 µs | p50 130, p99 240, max 615 µs |
| Idle windows | p50 10-20 µs, p99 40 µs | p50 100-110, p99 180-190 µs |

0 ticks over 20 ms; 0 `[FRONTIER]`, abort or segfault in the server log. Phase 1 measured the admin
channel at p99 30-40 µs and v3 the whole handler at p99 180-250 µs, so the additions are within the noise.

Bandwidth with 2 players (link average, including dead and boarding time): aim 30 Hz 1.6-2.6 KB/s
(13 + 50 bytes per player per frame: 3.6 KB/s with both alive), player stream 30 Hz 1.8-3.0 KB/s,
`WeaponFire` 140 B/s averaged over a minute with 8 s of gunfire (about 80 bytes an event, about 0.8 KB/s
while a gun fires), `Weapons` 3 B/s. For 64 players the aim stream at 30 Hz is about 96 KB/s, so a
viewer should pick 10 Hz or less unless it needs every tick.

## Open

- A soldier's weapon transform on the server is unverified (no bot fired on foot here). Soldiers fire from
  the camera object, so the contract tells a viewer to use the camera for them.
- A human client is unexercised, as for the rest of the admin channel.
- BF2WebAdmin decodes the new messages but does not subscribe to them or use them yet.
