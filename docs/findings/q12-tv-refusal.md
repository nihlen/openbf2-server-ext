# Q12: why a TV launch is refused while another launcher's TV missile flies

Status: investigated on 2026-10-23 by a worker thread. No OpenBF2 source was changed: the traces lived
only in a scratch copy of the tree. `docs/protocol.md` was not edited. Nothing was pushed.

## Answer

- **`LPTargetComp` is not the cause.** Its `m_currentProjectile` is cleared every time, and its
  `requestFire` passed every press that reached it. The refusing component is the launcher's **zoom
  component, `ToggleCameraComp`**: `GenericFireArm::requestFire` asks the zoom component before the
  target component, so `LPTargetComp` is never consulted for a refused press.
- **Cause.** `ToggleCameraComp::m_projectile` is a `WeakPtr<IProjectileObject>`. It lives on the
  control block of the projectile's `IProjectileObject` face, which is a second block next to
  `IObject`'s. The object cache invalidates only the `IObject` block when it pools a projectile, so the
  face's weak pointer survives. When the pool hands the same object to the other crew's launcher, the
  stale pointer resolves to the other crew's missile. `ToggleCameraComp::requestFire` then refuses
  because that missile is in a projectile camera, the other gunner's. The sidecar's suspicion was a
  weak pointer that survives pool reuse, and that is what happens, but in `ToggleCameraComp`, not
  `LPTargetComp`.
- **Retail: not established.** No IDA or Ghidra connection was available (no MCP tool, no running
  IDA/Ghidra process). So this document makes **no claim** about what retail does. The functions that
  would have to be read are listed under [Retail comparison](#retail-comparison).
- **Proposed fix** (not implemented): drop the `IProjectileObject` face's weak block when a projectile
  is pooled. That is a recovery correction only if retail is shown to do it. Otherwise it needs the
  intentional-change workflow. See [Proposed fix](#proposed-fix).

## Setup

| Item | Value |
|---|---|
| OpenBF2 | `serverext-r1` at `873d1b1`, exported with `git archive` into a scratch tree `~/q12-src` (WSL). The worktree was never edited, and `git status` is clean. |
| Trace | opt-in `[TEMP]` `[Q12]` lines to stderr (`BF2_Q12_TRACE=1`), only in the scratch tree. The patch is kept at `~/serverext-q12/q12-trace.patch`: `Core/Q12Trace.h` (new), `IObject.cpp`, `GenericProjectile.cpp`, `GenericFireArm.cpp`, `LPTargetComp.cpp`, `ToggleCameraComp.cpp`, `ObjectCache.cpp`. They only log, and the logging calls only const accessors (`isInProjectileCamera` and `fireAllowed` have no side effects). |
| Builds | `~/bf2-linux-serverext-q12` (`testenv/build-linux.sh`, run 3's `bf2` md5 `8f27e40605b0f4e21e33a5d51699827a`); module `~/serverext-build/q12` from this repo's master (`build-module.sh`, unit tests pass, `libserverext.so` md5 `c50b9fb6b14c90ffcc56f6dfbb326fce`) |
| Content | netsky `2v2-nocheck`, fresh run dir `~/serverext-q12/srv` (`prepare-content.sh`), module deployed with `BOT_LISTEN=127.0.0.1:16701` |
| Server | WSL, `start.sh` → `bf2` → `BF2::init` → `BF2Engine::startGame` → `GameServer`, `dalian_2_v_2` (`gpm_cq` 16). Game 16571/udp, gamespy 29903/udp, RCON 127.0.0.1:4723, bot channel 127.0.0.1:16701. No other port was used, and no container was created or touched. Three server processes ran, each stopped with SIGTERM. The server is left stopped. |
| Client | `tools/testclient/serverext_client.py --port 16701 --scenario q12` (new), Python 3.10.6 in WSL |
| Logs | `~/serverext-q12/`: `q12-run{1,2,3}.log` (client), `bf2-run{1,2,3}.log` (server console), `q12-run3.full.trace` (the `[Q12]` lines of run 3) |

## Reproduction

`--scenario q12` (new) creates two crews on one connection. Crew A is a pilot and gunner bot in team
1's Z-10 (gunner's launcher `ahe_z10_HJ8Launcher_TV`). Crew B is the same in team 2's AH-1Z
(`AHE_AH1Z_CoGunner_HellFireLauncherTV`). Each gunner toggles to the TV launcher. Both pilots hover 60 m
above their pads, nose seaward, and every missile is unguided onto the sea. The scenario has four
cases:

1. A fires. B fires 15 ticks later, while A's missile flies.
2. The same with B first.
3. A fires. B fires 30 ticks after A's missile has ended.
4. A and B press in the same TickState.

A refused press is retried once after the other missile has ended.

The result was the same in all three runs (run 1 without the component trace; runs 2 and 3 with it).
Run 3:

| Case | Result |
|---|---|
| 1 A, then B in flight | A launched 1852; **B launched 1851** (B had never fired) |
| 2 B, then A in flight | B launched 1852; **A pressed at 1849: no launch**; A's retry after B's missile ended launched 1852 |
| 3 B after A's ended | A launched 1852, B launched 1852 |
| 4 A and B in one tick | A launched 1852; **B: no launch** |

So the refusal is deterministic and needs no race. A launcher is refused while **the object it last
fired** flies for the other crew. Case 1 passes only because B had never fired: B's stale pointer was
still null, and B got a second object (1851) because 1852 was out of the pool. Every later shot is 1852,
because the pool returns its highest-keyed entry (below). This matches the soak's "every TV missile is
1852" and its "both crews pressed within a tick and the other's launched".

### Trace (run 3, abridged)

Pointers: A's launcher `…21cc0`, B's `…e2d50`. The pooled projectile 1852 is `…51ed0`, and its
`IProjectileObject` face is `…52120`. The face's weak block is **`…50f30`**.

```
tick 1525 ToggleCameraComp::requestFire launcher=…21cc0 m_projectile{ctrl=(nil)}                 fireAllowed=1   (A's first shot)
tick 1525 GenericFireArm::requestFire  launcher=…21cc0 accepted=1 refusedBy=none
tick 1525 ToggleCameraComp::postFire   launcher=…21cc0 m_projectile{ctrl=…50f30 target=…52120}   (A holds 1852's face block)
...  A's missile ends: LPTargetComp::projectileWasReset clears A's m_currentProjectile
assignNewObjectID obj=…51ed0 weakCtrl=…19090 (invalidated)                                    (the IObject block only)
cache push obj=…51ed0 key=1852 projectileFaceWeakCtrl=…50f30 target=…52120 (NOT invalidated)
tick 1833 GenericFireArm::requestFire  launcher=…e2d50 accepted=1                                (B, case 2)
cache pop obj=…51ed0                                                                           (B gets 1852)
tick 1833 ToggleCameraComp::postFire   launcher=…e2d50 m_projectile{ctrl=…50f30}                 (both launchers now hold …50f30)
tick 1851 ToggleCameraComp::requestFire launcher=…21cc0 m_projectile{ctrl=…50f30 target=…51ed0} projLauncher=…e2d50 projNet=1852 inProjCam=1 fireAllowed=1
tick 1851 GenericFireArm::requestFire  launcher=…21cc0 accepted=0 refusedBy=zoom              (A refused: B's missile, B's camera)
tick 1971 ToggleCameraComp::requestFire launcher=…21cc0 m_projectile{ctrl=…50f30 target=…51ed0} projLauncher=…e2d50 inProjCam=0
tick 1971 LPTargetComp::requestFire ... decision=pass cur{ctrl=(nil)}                           (retry passes once 1852 is back in the pool)
...
tick 2643 GenericFireArm::requestFire  launcher=…21cc0 accepted=1                                (case 4: A launches 1852 first)
tick 2643 ToggleCameraComp::requestFire launcher=…e2d50 m_projectile{ctrl=…50f30 target=…51ed0} projLauncher=…21cc0 inProjCam=1
tick 2643 GenericFireArm::requestFire  launcher=…e2d50 accepted=0 refusedBy=zoom
```

`LPTargetComp` in the same run: every `LPTargetComp::requestFire` that ran logged `decision=pass` with
`cur{ctrl=(nil)}`. Every launch was followed by `projectileWasReset` on the launching weapon, which
cleared its `m_currentProjectile`. That pointer's `IObject` block was also invalidated at every cache
push.

## Cause in source (OpenBF2 `873d1b1`)

1. **The refusing vote.** `GenericFireArm::requestFire` (`Game/Objects/Weapons/GenericFireArm.cpp:1209-1233`)
   asks the ammo, fire, zoom, target and animation components in that order. Once the zoom vote fails,
   the target vote (`LPTargetComp`) is skipped.
2. **The zoom component.** `ToggleCameraComp::requestFire`
   (`Game/Objects/Weapons/ZoomComponents/ToggleCameraComp.cpp:199-205`) returns false while
   `m_projectile` resolves to a projectile that `isInProjectileCamera()`. `postFire` (`:188-197`) sets
   `m_projectile` through `getProjectileWeakPtr` (`:44-60`), which uses the face's block
   `IProjectileObject::m_weakControl` (`Game/Objects/Projectiles/IProjectileObject.h:38`). Only
   `reset()` (`:207-210`) clears it, reached through `ZoomComp::disable` (`ZoomComp.cpp:57`) on a weapon
   switch or exit. `ToggleCameraComp` has no `projectileWasReset` (the `ZoomComp` default at
   `ZoomComp.cpp:55` is empty), so nothing clears it when the missile ends.
3. **"In projectile camera" asks the projectile's current launcher.**
   `GenericProjectile::isInProjectileCamera` (`Game/Objects/Projectiles/GenericProjectile.cpp:1469-1477`)
   compares the active camera of the PCO that owns the projectile's **current** launcher weapon with the
   projectile camera. After reuse, that launcher is the other crew's. The camera is switched on at launch
   (`postInit` → `useProjectileCamera` → `enableProjectileCamera`, `:1425-1463`, the template's
   `useProjectileCamera`). `resetProjectile` switches it off at the end of the flight (`:529`). So the
   refusal lasts exactly as long as the other crew's missile flies (about 4 s).
4. **Pooling invalidates only the `IObject` block.** When a missile ends, `ObjectManager::destroyObjects`
   (`Scene/Object/ObjectManager.cpp:2140-2167`) pushes it into the cache, because it was created through
   the cache. `ObjectCache::pushObject` (`Scene/Object/ObjectCache.cpp:28-56`) calls
   `IObject::internal_assignNewObjectID` (`Game/Objects/IObject.cpp:90-108`), which drops only
   `IObject::m_weakControl` (`IObject.h:257`, +0x7C). The face's block is dropped only by
   `~IProjectileObject` (`IProjectileObject.h:27-35`), and a pooled object is never destroyed.
   `GenericProjectile::disableCachedObject` (`GenericProjectile.cpp:435-461`) and `resetProjectile`
   (from `:474`) don't touch it either.
5. **Why it is always the same object.** `ObjectCache::popObject` with id 0 takes the rightmost entry
   (`ObjectCache.cpp:63-95`, `--it` at `:83`). The pool is keyed by networkable id, and an ended missile
   goes back under the same key, so 1852 is popped every time unless it is in flight. The stale face
   pointer therefore almost always points at the missile in flight.

Not the cause:

- **`LPTargetComp::m_currentProjectile`** (`LPTargetComp.cpp:312-315`) is a `WeakPtr<IObject>` on the
  `IObject` block. `projectileWasReset` (`:187-210`), reached from `resetProjectile` →
  `getLauncherWeapon()->projectileWasReset` (`GenericProjectile.cpp:520-527`), clears it at the end of
  every flight. `internal_assignNewObjectID` invalidates it as well. The guard at `:322-336` is correct
  as reconstructed, and it was never the refusing vote.
- **A per-template pool of size one.** The pool held 16 pre-cached `agm114_hellfire_tv` objects
  (1837-1852), and case 1 popped a second one.
- **The cached flag 0x20.** It was 0 on every live missile in the trace.

Side effect, not exercised: `ToggleCameraComp::toggle` (from `:126`) and `getChangeCameraId` (`:212-222`)
read the same stale pointer. While the other crew's missile flies, a gunner's toggle press would take
the "leave projectile camera" arm and set its own PCO's camera to `m_prevCameraId`. Other
`WeakPtr<IProjectileObject>` holders, such as `GenericProjectile::m_associatedTracerWeak`, use the same
face block. Tracers are client-side, so this was not examined further.

## Retail comparison

**Not done.** No IDA or Ghidra instance with `bf2_w32ded.exe` or the Linux ELF was connected or
reachable from this session: no disassembler MCP tool was available, and no IDA, Ghidra or Java process
was running. AGENTS.md makes that a blocker for any claim about recovered behaviour. Nothing above says
what retail does, and the observation that retail 2v2 play has two TV missiles in the air at once is
anecdotal, not evidence. Retail may still differ at any of the points below.

To settle Q12, read these functions in the PE and the ELF (the ELF ranges come from the tree's own
comments):

| Function | What to look for |
|---|---|
| `ObjectCache::pushObject` | anything besides `disableCachedObject` and `internal_assignNewObjectID` that drops a weak block |
| `IObject::internal_assignNewObjectID` (slot 85) and any `GenericProjectile` / `SimpleObject` / `Bundle` override of slot 85 | whether a projectile override also drops the `IProjectileObject` face's block (+0x04 of the face) |
| `GenericProjectile::disableCachedObject` (ELF `0x8280400`, 274 B) and `GenericProjectile::resetProjectile` (ELF `0x8283240`, 735 B) | a write of 0 into the face's `m_weakControl->target`, or a release of the face's block |
| the `IProjectileObject` weak-pointer producer (the "74-byte" non-virtual, `GenericProjectile.cpp:258-260`) and every xref to the face's +0x04 field | every site that creates, nulls or drops the face block |
| `ToggleCameraComp::postFire`, `requestFire`, `reset` and its vtable (ELF `0x829D780..0x829EA8A`) | whether `postFire` really takes the face's weak pointer and not `IObject::getWeakPtr`; whether `ToggleCameraComp` overrides `projectileWasReset` (the `ZoomComp` slot) |
| `GenericProjectile::isInProjectileCamera` (PE `0x5B2E60`) | that it tests the projectile's current launcher, as reconstructed |
| `ObjectManager::createObject` / `ObjectCache::popObject`, and the `GSUseObjectCache` default | that retail's dedicated server pools projectiles at all, and pops the rightmost entry |

If retail drops the face block somewhere in that path (or clears `m_projectile` on `projectileWasReset`),
the reconstruction is missing it and the fix is a recovery correction. If retail matches the
reconstruction everywhere, retail has the same defect, and fixing it is an intentional change.

## Proposed fix

Not implemented. One change, in `GenericProjectile`'s pooling path. Mirror what
`IObject::internal_assignNewObjectID` does for the `IObject` block: when a projectile goes into the
object cache, set the `IProjectileObject` face's `m_weakControl->target` to 0, drop the owner's
reference (delete the block when it was the last one) and null `m_weakControl`. The next
`getProjectileWeakPtr` then allocates a fresh block for the next flight, and every older
`WeakPtr<IProjectileObject>` resolves to null. `ToggleCameraComp`'s pointer and the tracer links stop
following a pooled object into its next flight. A launcher whose missile has ended is then refused
only by its own state, as `LPTargetComp` already is.

- **Where.** It depends on the retail reading above. If retail does it, put it where retail does (a
  `GenericProjectile` slot-85 override, `disableCachedObject` or `resetProjectile`). If not, the narrowest
  intentional site is a `GenericProjectile::internal_assignNewObjectID` override that calls the `IObject`
  one and then drops the face block, so it runs exactly when the cache reassigns ids.
- **Contracts.** No ABI, protocol or file-format change: the face block's layout is unchanged, and
  only its lifetime ends earlier for pooled objects. A new slot-85 override does not add a vtable slot.
- **Classification.** A recovery correction only with the retail evidence above. Otherwise it is a
  deliberate departure from retail behaviour. It then needs an issue that discusses this approach before
  implementation, and the code must mark it `[CHOSEN, not recovered]` with the issue linked (AGENTS.md,
  "Intentional fixes and improvements").
- **Rejected alternatives.**
  - Overriding `ToggleCameraComp::projectileWasReset` to call `reset()` fixes only this holder, and it
    adds a vtable override that retail may not have.
  - Making `requestFire` compare `getLauncherWeapon()` with `firearm()` changes the recovered body of a
    shipped function to hide the stale pointer instead of ending it.
- **Validation for the fix.** `--scenario q12` should pass 18/18 (cases 2 and 4 launch). Repeat the
  three 10-minute crew soaks from bf2-bots `b8babda` with 0 refused presses. Check that a single
  launcher is still refused while its own missile flies (`disableFireWhenActiveMissile`,
  `ToggleCameraComp` in-camera). Run the MSVC, `msvc-x64` and Linux gates for the touched TU.

## For the sidecar and the contract

- Until a fix lands, the sidecar's workaround stands: press again. More precisely, a press is refused
  while the object this launcher last fired (in practice 1852) is in flight for the other crew. It
  launches as soon as that missile ends. A human gunner on this server sees the same.
- Contract point 1 of bf2-bots `serverext-backend.md` names `LPTargetComp::requestFire`. The refusing
  vote is `ToggleCameraComp::requestFire`, and `protocol.md` "Sidecar on v3" should be corrected the
  same way when Q12 is closed.
