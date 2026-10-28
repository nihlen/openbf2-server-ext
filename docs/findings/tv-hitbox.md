# TV missile hitboxes: what a missile can hit, and what is rewound (2026-10-28)

Why a TV missile that visibly passes through a helicopter can miss, and why one that passes behind it can hit. This
is the server side of the hit/miss visualization planned for a 3D viewer in BF2WebAdmin (the client's "red" pose
next to the server's "blue" hitbox). Everything here was read in OpenBF2 `serverext/r1` and measured on a WSL
server with scripted bots; the engine paths are cited as `file:line` under `C:\Data\Projects\OpenBF2\Code\BF2\`.
Background: bf2-bots `physics/HITREG.md` and `docs/hit-registration.md`, which measured the same rewind against a
retail server with a real client.

## How the server decides a hit

- The server simulates at 30 Hz. Per tick (`GameServer::simulateFrame`, `Game/GameServer/GameServer.cpp`): objects
  update, players' vehicles integrate, other physics nodes (projectiles) integrate, then world collision, then the
  players' vehicle collisions, then every node's latency history is sampled (`PhysicsNodeManager::postFrameUpdate`,
  `Physics/PhysicsNodeManager.cpp:147-162`).
- A projectile is a point. Each tick its path, from `position - dt * velocity` to `position`, is swept against every
  candidate object's projectile collision mesh (LOD 0) (`Physics/PointResponsePhysics.cpp:86-173`). Of all contacts in
  the tick, the one farthest back along the path wins (`PointResponsePhysics::doResponse`).
- **Lag compensation (rewind).** An object with a latency history is tested at its pose `L` seconds ago, `L` taken
  from the shooter (`Physics/PointResponsePhysics.cpp:128-139`, `Physics/LatencyCompensation.cpp:82-108`):
  `L = ping x 0.5 + the interpolation time the shooter's client reports`, at most 0.3 s; 1/30 s or less rewinds
  nothing. Bots are local players and get no rewind; `vars.set phy-latencyComp-debug <s>` forces one value for every
  shooter (used for the tests below).
- **Who has a history.** Only networked, mobile objects **without a parent** get one at creation
  (`Scene/Object/Object.cpp:619-625`): vehicle roots and soldiers. Soldiers keep their own 16-tick history of
  position, rotation and pose (`Physics/SoldierPhysicsNode.cpp:404-447`), also while seated. Child parts of a
  vehicle have none and are tested where they are now.
- The missile does not need to touch the visible model: hull hits are against the collision mesh, and after a direct
  hit the missile stops at the contact point and stays there until its time to live runs out (so the admin
  channel's `ProjectileHit` comes up to 3.5 s after the impact; the impact tick is the `Damage` event's).
- Damage goes to the nearest armour up the parent chain, so a hit on a child part (a rotor, a dummy missile) is
  reported as damage to the helicopter, and `Damage` cannot tell the parts apart. The projectile-collision observer
  (below) can.

## What a TV missile can hit

From the content (netsky 2v2 `objects_server.zip`, `ahe_ah1z`/`ahe_z10` `.con` and `.tweak`) and confirmed by
logging every server hit in an instrumented engine copy (AH-1Z target).

| Part | AH-1Z | Z-10 | Tested at | Seen hit |
|---|---|---|---|---|
| Hull (root collision mesh, geometry 0; canopy glass is material 1) | yes | yes | rewound by `L` | yes |
| Pilot, gunner (soldiers, collision skeleton) | yes | yes | rewound by `L` (their own history and pose) | pilot yes; gunner not tested |
| Main rotor (`*_mainrotor`, own collision mesh: the blades, not a disc) | yes | yes | now | yes |
| Tail rotor (`*_tailrotor`) | yes | yes | now | not tested |
| Dummy missiles on the stub-wing rails (`agm114_hellfiredummy`, Hellfire collision mesh) | 8 | none | now | yes |
| Nose sensor turret (`*_Sensors*`, geometry parts 6-7 / 9) | 2 parts | 1 part | now | yes |
| Chin gun turret, barrel (`*_gunturret`, `*_barrel*`) | yes | yes | now | not tested |
| Landing-gear dampers (`AHE_AH1Z_Damper0-3`, one face each) | yes | none | now | no (practically unhittable) |
| Wheels (`AHE_Z10_LWheel`, `RWheel`, `TailWheel`) | none | yes | now | not tested |
| `AHE_Z10_Tail` (geometry part 10, about 3 x 0.2 x 0.6 m: the horizontal stabiliser; the tail boom is in the hull) | none | yes | now | not tested |

- Not hittable although they have collision physics: the rocket and flare launchers and the rotor engines
  (`*_HydraLauncher`, `ahe_z10_S8Launcher`, `*_FlareLauncher`, `*_Rotor`, `*_TailEngine`): no collision geometry.
- AH-1Z positions (body frame, x right, y up, z forward, metres): dummy missiles at x +-1.29 and +-1.64, y -0.32 and
  -0.63, z 0.3, each 1.75 m long (`ahe_ah1z.tweak:129-150`); main rotor mount 0/1.50/0, blades 14 m across and
  y +0.24..+0.98 above the mount; tail rotor 0.54/1.44/-9.03; sensors near 0/0/4.4; pilot around z +1.9.
- **Consequence:** the server hitbox is split. The hull and the crew trail the drawn helicopter by about
  `target speed x L`; the rotors, dummy missiles, sensors and gun stay where the helicopter is now.

### Why the crew can be hit through the hull

Both use the same rewind, so a missile coming from outside meets the hull first. The exception: the sweep covers
only the missile's own movement within a tick, not the hull's movement between ticks. When the missile's tip ends a
tick just outside the hull and the target moves on (0.5-1.2 m per tick in the scan), the tip lies inside the next
tick's rewound hull without having crossed a face. That tick's sweep starts inside the fuselage, and the pilot's
skeleton comes before any hull face. The soldier then takes the full damage and the helicopter none ("shot out").
In all 7 such hits of the scan the sweep started inside the cockpit, the skeleton contact came at 1-24 % of the
sweep and the first hull face at 9-34 %. Fast closing speeds (head-on, fast slides) make it more likely.

## Measurements

WSL server, OpenBF2 `serverext/r1` `db3a07f` (working tree: plus another session's uncommitted
`tv-aim-from-missile` change in `LPTargetComp.cpp`, default 0 = retail, so inactive), module `93467b1`, netsky
`2v2-nocheck` content, `dalian_2_v_2`, ports 16587/29919/4739, bot channel 16720, admin channel 4320; `stepMode 1`.

1. **Bot crews against each other** (bf2-bots sidecar `be8d449`, 744 TV flights at forced `L` 0, 0.10, 0.15, 0.25 and
   0.30 s): the lag that best predicts the server's direct hits equals the forced `L` every time (714/744 flights
   agree with "the path crosses the hull rewound by `L`").
2. **Hitbox scan** (1568 flights): a scripted AH-1Z target flying repeatable passes (nose-on sideways slides at 16 and
   29 m/s, broadside at 34 and 49 m/s, head-on at 35 m/s) and a hovering Z-10 gunner shooting each pass with a random
   aim lag `tau` (aimed at the intercept moved back by `tau x velocity`); forced `L` 0, 0.15 and 0.30 s. For the test the
   TV missile's damage was set to 1 and its explosion damage to 0 (the target survives, every direct hit still gives
   a `Damage` event) and the launcher's magazines to infinite (`ObjectTemplate.ammo.nrOfMags -1`).

Direct-hit rate by where the shot was aimed:

| Target | L | aimed at the current position | aimed L behind |
|---|---|---|---|
| broadside 34 m/s | 0.15 s | 3/31 | 35/35 |
| broadside 49 m/s | 0.15 s | 0/28 | 29/29 |
| broadside 49 m/s | 0.30 s | 0/15 | 18/18 |
| nose-on slide 16 m/s | 0.15 s | 2/33 | 44/48 |
| nose-on slide 29 m/s | 0.15 s | 1/32 | 26/33 |
| nose-on slide 29 m/s | 0.30 s | 0/19 | 14/15 |
| head-on 35 m/s | either | about 100 % | about 100 % |

Crossing targets: the hit window is centred on `L` and is as wide as the target's size over its speed (about 0.3 s
broadside at 49 m/s, about 0.1 s nose-on at 29 m/s). Head-on the trail lies along the line of sight and does not
cause misses.

All 1568 flights against the full model (hull and crew rewound, the rest now):

| Crossed in the model | Server direct hit | No hit |
|---|---|---|
| rewound hull | 631 | 15 (grazes at the band edge) |
| Hellfire dummy (now) | 17 | 3 (grazes) |
| main rotor blade slab (now) | 24 | 11 (blades spin: a crossing is a chance) |
| nose sensors (now) | 1 | 1 |
| nothing | 2 (contact point on the rewound hull; the path test missed them) | 863 |

All 675 direct hits are explained; 7 of them damaged the pilot instead of the helicopter (above).

The parts were identified with a temporary log of every hit in `PointResponsePhysics::checkObjectVsObject` and
`GenericProjectile::handleCollision` in a private copy of the source (`~/openbf2-tvdiag`, never in the shared tree):
37 slide hits named `agm114_hellfiredummy` and 6 `AHE_AH1Z_Sensors1/2`, 18 head-on hits `ahe_ah1z_mainrotor`, all
tested at the current pose.

Not tested: a Z-10 target, the gunner soldier, the tail rotor and gun turret, targets faster than 49 m/s, a human
client (the client's drawn pose stays a model), retail (the code is the recovered retail code).

## The projectile-collision observer (engine)

`IServerExtension::addProjectileCollisionObserver` (OpenBF2 `serverext/r1` `0a58ec0`, `[CHOSEN, not recovered]`,
see its `docs/server-extension.md`) reports every collision the server hands to a projectile, from
`GenericProjectile::handleCollision`, server only: the projectile, its shooter, the object part touched (`null` for
terrain), the impact point, normal, relative speed, material, the shooter's current latency and whether the part
was tested rewound. Validated by an MSVC syntax check and the Linux build; it has not run, because nothing in the
module uses it yet (the same call site was exercised by the temporary log above); reporting it on the admin channel is a contract change still
to be designed and approved.

## For the viewer

- The server ("blue") hitbox is the hull and crew placed where the target was `L` ago, plus the child parts above
  at the target's current pose. `L` needs the shooter's ping and reported interpolation time
  (`IPlayer::getInterpolationTime`), which the admin channel does not carry yet.
- The client's drawn pose ("red"): both the missile and the target are ghosts the gunner's client draws with the same
  interpolation delay (both `BasicInfo`, `PMLinear`), so relative to the missile the target appears at its current
  server pose. A model, not measured with a human client.
- Which part a missile hit: exact from the observer; from data alone, the contact point (the stopped missile's
  position) classified against the parts above, ambiguous when `L` is under about 0.1 s.

## Tools (scratch, not in the repo)

- WSL `~/serverext-tvhitbox/tools/`: `tvrecord.py` (admin-channel recorder: heli and TV streams at 30 Hz,
  `--sparse`, forced-lag blocks, `--setup` console lines), `tvscan.py` (the scripted scan, a stepping protocol v4 client
  on `serverext_client.py`); recordings in `~/serverext-tvhitbox/logs/`.
- `C:\Data\Projects\DotNet\BF2WebAdmin-localtest\scratch\tvhitbox\`: `tvanalyze.py` (lag fit), `tvscananalyze.py`
  (hit bands, hit rate by aim lag), `tvcontact.py`, `tvtimeline.py`, `tvparts.py` (the full part model),
  `tvsoldier.py`, `tvsoldier2.py` (crew hits); collision meshes from bf2-bots `data-pipeline` and `meshes\`.
