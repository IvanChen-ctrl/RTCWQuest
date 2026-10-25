# M4 hardware-feedback round — crouch fix, adaptive following, mercenary squad

Branch `pico4-m4-tactical-ai` (PR #6). Written after the owner's first PICO 4 test of the M4 build.
**Nothing here has been run on a headset or in the game engine.** The findings below are read from the source;
"fixed" means "changed in source, covered by host tests where the logic is pure, compiled by the Android workflow".
The owner's hardware findings stay **unresolved until the new APK is tested**.

| Hardware finding | Source-level cause found | Change | Confirmed on hardware |
|---|---|---|---|
| A does not crouch | IRL-crouch branch of `PM_CheckDuck` never reads the button; gated release | `VrCrouchLogic.h`, `VrInput_DuckStep` | **No** |
| Companion too slow | speed ceiling of the stock AI (73-88% of the player's speed) | companion-only speed floor | **No** |
| Companion gets lost | stock chase uses straight-line distance, stops without line of sight | path-distance policy + recovery ladder | **No** |
| Only one mercenary | single global record | squad of up to 4 independent soldiers, default 2 | **No** |
| Tactics inadequate | decisions were not squad aware; ammo/wounds not used | claims, wounded/ammo cover, tighter "cover me" | **No** |

## 1. A-button crouch

### Traced pipeline (source)

```
PICO A -> /user/hand/right/input/a/click (bytedance/pico4_controller) -> OpenXrInput.c: face1 action
 -> VrInput_ComposeButtons -> Buttons & ovrButton_A (right hand)           [VrInputLogic.h, host tested since M2A]
 -> HandleInput_Default: primaryButton1 (domButton1 = A; with vr_switch_sticks the OFF hand's button 1 = X)
 -> sendDuck -> "+movedown"/"-movedown"  -> cl_input.c IN_DownDown -> kb[KB_DOWN] -> cmd->upmove -= 127
 -> bg_pmove.c PM_CheckDuck: `pm->cmd.upmove < 0` -> PMF_DUCKED
```

### Root cause (read from source; hardware confirmation pending)

`vr_irl_crouch_enabled` defaults to **1** (`RTCWVR_SurfaceView.c:184`). With it on, `PM_CheckDuck` takes the
"IRL crouch" branch, computes the player's height from the headset height and **returns before the line that reads
`pm->cmd.upmove < 0`**. So `+movedown` reached the usercmd but had no effect on the player's height; the only thing the A
press did in that mode was reset the calibrated standing height (`vr.maxHeight = 0`). This matches "A does not reliably
crouch" (a crouch only happened if the player physically crouched).

A second defect made it worse: both edges were sent only while the backpack zone was inactive
(`vr.backpackitemactive != 2 && !canUseBackpack`), so a press made while the hand was in the zone was lost and a
release made after the hand entered it was never sent (a stuck `+movedown`). This is the same bug class that M3 fixed
for the jump.

### Change

- `VrCrouchLogic.h` (new, pure, shared by game and cgame through `bg_pmove.c`): `VrCrouch_ComputeHeight`. The button now
  **forces the crouched height**; the headset height still drives IRL crouching when A is not held. Hold semantics are
  unchanged (the engine's `+movedown`/`-movedown`; there is no toggle in this code base).
- `VrInput_DuckStep` (in `VrInputLogic.h`, pure): latched press, a release that is **always** sent, a held crouch released
  when a menu or the backpack zone takes over, a press made while blocked never replayed. Crouch and jump are separate
  machines.
- `VrInputDefault.c`: `sendDuck` in gameplay and (blocked) in the menu branch.
- Unchanged: handedness and `vr_switch_sticks` mapping (primary button 1: dominant hand's A, or the other hand's X when
  switched), menu input, God Mode, notebook, jump (B / left Y).
- `vr_irl_crouch_enabled` is untouched (default 1). Standing height recalibration on the A edges is kept.

### Diagnostics (`adb logcat -s RTCWInput:I`)

```
v1 t=... ev=crouch action=pressed|released|forced_release|ignored button_source=A|X game_state=gameplay|backpack|menu irl=0|1
```

How to read a failure on the headset:

| What you see | Meaning |
|---|---|
| no `ev=btn ... name=A` | the OpenXR binding/profile problem of M2A, not the crouch logic |
| `ev=btn name=A` but no `ev=crouch` | the press was swallowed before the duck logic (check `vr_switch_sticks`: then the button is **X** on the left controller) |
| `ev=crouch action=ignored game_state=backpack` | the dominant hand was in the backpack zone; move the hand away from the shoulder |
| `ev=crouch action=pressed irl=1` but the character does not lower | the physical height rule or `PM_CheckDuck` still ignores it; send the log with the `vr_irl_crouch_*` values |
| `ev=crouch action=pressed irl=0` but nothing | `+movedown` is not reaching the usercmd: console `bind` conflict or `upmove` overwritten |

Note: with button crouch the **camera does not move down** in IRL-crouch mode (the headset height drives the camera);
the collision box, the AI's view of the player and the movement speed change. Whether that feels right is for the owner
to judge.

## 2. Why the mercenary was slow and got lost

All of this is read from the stock AICast code.

### 2.1 Speed

`AICast_InputToUserCommand` / the movement-input code in `ai_cast_think.c`:

- `speed = speedScale x RUNNING_SPEED`, with `speedScale` clamped to **0.25-1.2**.
- `RUNNING_SPEED` is the American's 220, **replaced every frame** by the `moveSpeed` of the current run animation
  (`AICast_Think`).
- `bi.speed` is capped at `400 x speed/300`, and the user command is `bi.speed x 127/400`; the player's `ps.speed` is the
  same 320 for both.
- Result: at the default `speedScale` 1.0 the AI moves at about **73%** of the player's top speed (about 235 vs 320
  units/s), at the 1.2 cap about **88%** (282). Nothing in the follow code ever raises `speedScale` when the companion is
  far behind; `AICast_SpeedScaleForDistance` only slows it near its goal.

### 2.2 Losing the player

- The stock chase of the leader (`AIFunc_ChaseGoal`) **stops** when the straight-line distance is below 240 and there is
  no clear line to the leader (`AICast_EndChase` + idle), and the idle state only starts a chase again beyond 256
  straight-line units. Around a corner or through a door the straight-line distance is small while the walking path is
  long, so the companion stands still and the player leaves it behind.
- Every leash and arrival check in the stock follow logic uses `Distance()` (straight line), never the AAS path.
- A player standing off the navigation mesh gives the AI `goal.areanum == 0` (no route) for the chase.

## 3. Adaptive following (`g_vrmove.h`, pure; glue in `g_merc.c`)

Applies to a soldier on the **FOLLOW** order only (HOLD, maneuvers, scripts and combat under fire keep their own control).
Cvar `vr_merc_move` (default 1; 0 restores the M3 behaviour).

| Mode | Enter | Leave | What it does |
|---|---|---|---|
| NEAR | - | - | stock behaviour plus the formation logic |
| RUN | path/straight distance > 240 while the player moves (> 320 if the player stands) | < 190 for 1.5 s | forced chase of the player at speedScale 1.2 |
| CATCHUP | > 520, or > 300 **without line of sight** (around a corner), or no route | < 360 (< 240 without LOS) after 1.5 s -> RUN; < 190 -> NEAR | forced chase, speedScale raised to `300 / runSpeed` (capped 1.6), no slow approach |

- The distance used is the **larger of the straight line and the AAS path** (`travel time x 3 units`), refreshed every
  250 ms (one trace, two AAS queries). A player off the mesh uses the last good area.
- Hysteresis: an upgrade is immediate, a downgrade needs the thresholds below and 1.5 s of dwell; a unit test shows no
  flapping under +-30 unit jitter.
- The speed floor is applied by **one hook in `ai_cast_think.c`** that only affects a soldier with a non-zero floor
  (`VrMerc_SpeedScaleFloor`); no other NPC changes speed.
- "Cover me" tightens the thresholds (x0.75). A close fight (ENGAGE within 420 units) or being shot at suspends the policy
  so combat decisions are not overridden; a soldier catching up does not take new targets unless under fire or designated.
- The M3 `RETURN` state now only serves the HOLD leash.

### Recovery ladder (bounded, no teleport)

Detected when a soldier that is supposed to be travelling (RUN/CATCHUP, farther than 200 units) moves less than 48 units in
2.5 s. One step per 2.5 s:

1. **repath** - `trap_BotResetAvoidReach` (forget the areas the AI chose to avoid) and point the chase at the player again
   (`ev=merc_repath attempt=1`);
2. **trail** - walk to the newest reachable point on the player's recent walkable path (12 breadcrumbs, one per 120 units /
   0.8 s, at most 6 AAS queries), 7 s limit (`attempt=2`);
3. **wait** - stand down for 8 s, then a fresh episode (`ev=merc_navigation_stuck attempt=3`).

A spell of good movement forgets earlier failures. After a recovery the soldier resumes its order: the FOLLOW policy simply
points at the player again.

**Not implemented, on purpose:** emergency teleport. The brief called it optional and technically unnecessary if real
navigation works; adding it would require its own validation (out of sight, not near enemies, not inside geometry, not on
mission triggers). It can be added later behind a cvar that defaults to off.

### Caveats (unverified)

- Animation: the soldier runs faster than its run animation's `moveSpeed` in CATCHUP. Whether the legs visibly slide is not
  known.
- The AAS travel time (hundredths of a second) is converted at 300 units/s; real routes with doors or jumps may differ.
- Two reasons it may still lose the player that this change does not touch: locked or script-controlled doors, and
  places with no AAS data.

## 4. The squad

### Model

`squad[4]` records, each with its own entity slot, unique AI name `vr_merc_1..4`, character (own health and ammunition),
target, tactical state, follow/recovery state, cooldowns, goal entities, damage log and tick schedule (staggered 30 ms
apart). One pointer selects "the soldier being processed"; the shared parts are only the model survey, the one recruit that
may be on the way, the "attack my target" arming and the corridor sensing. Saves skip every soldier and every soldier's goal
entities; level changes and loads clear all records and re-hire up to the remembered count (`vr_merc`, now a **number**).

### Limit

`vr_merc_squad` (default **2**, hard ceiling 4). Raise it to 3 only after the performance check in section 6.
`merc hire` at the limit prints "Mercenary: squad is full (N of M)" and logs `merc_limit_reached`. Running out of AI
character slots is reported the same way (`reason=no_slot`).

### Controls

Console (also what the notebook buttons run). `N` is 1-4.

| Order | Command | Notes |
|---|---|---|
| Hire one more | `merc hire` | one recruit at a time; each is validated separately and kept clear of the squad |
| Follow / Hold | `merc follow [all\|N]`, `merc hold [all\|N]` | |
| Regroup | `merc regroup [all\|N]` | ends HOLD, cancels a maneuver, comes to the player |
| Cover me | `merc cover [all\|N]` | 20 s: tighter following, threats near the player weigh more, no flanking |
| Attack my target | `merc attack [all\|N]` then fire; `merc attack now [all\|N]` | arms 15 s; the **next fire press** designates what the hand points at |
| Stance | `merc stance defensive\|aggressive` | squad-wide (cvar `vr_merc_stance`) |
| Assist on/off | `merc assist [0\|1]` | squad-wide |
| Dismiss | `merc dismiss [all\|N]` | no argument = the notebook selection (default all) |
| Choose who is addressed | `merc select [all\|N]` (no argument cycles) | sets `vr_merc_sel` (0 = all) |
| Report | `merc status` | one line per soldier |

Notebook: Private bucket list -> **Mercenary orders >>**: a selector ("Orders go to: All soldiers / Mercenary 1..4"), then
Follow me, Hold position, Regroup, Attack my target, Cover me, Hire a mercenary, Defensive stance, Aggressive stance,
Assist on/off, Dismiss selected, Dismiss all. No essential controller button was repurposed.

Messages are numbered ("Mercenary 2: holding position") once there is more than one soldier. The selector falls back to
"all" when the selected soldier dies or is dismissed.

### Formation and coordination

- Roles by living order: 1 behind-left (130 back, 90 left), 2 behind-right, 3 rear-centre (250 back), 4 rear-left wide.
  Approximate: a soldier only moves when it is in the player's way, within 70 units of a squad mate (only the higher number
  steps away), or - when the player has stood still for 1.2 s - more than 150 units from its slot. Moves are bounded (5 s),
  cooled down (4 s) and validated against the navigation data; a closer fallback slot is tried before giving up.
- Corridors/doorways: the player's clearance to the left and right is sampled once per second (2 traces for the whole
  squad); below 220 units total (hysteresis to 235) the squad lines up single file behind the player, 100 units apart.
- Fire is spread: a target that squad mates have already engaged loses 28 points per mate (max 2), **unless** it is
  dangerous (designated, shooting at the player, or just hurt the player). One enemy means everybody engages it.
- Every soldier keeps its own hysteresis, cover/flank/regroup cooldowns and navigation state; a group order only sets the
  common order.
- Wounded (health < 50% and hit in the last 4 s) or out-of-ammo soldiers with a visible enemy seek cover; the existing
  retreat/regroup rules (35% health, 20% in HOLD, 20 s cooldown) apply per soldier.
- Friendly fire: every soldier holds fire when the player or any ally (other soldiers included) is on its line of fire, and
  the damage policy protects the player and all mates (a soldier's own splash stays with the stock rules).

## 5. Diagnostics

Tags: `RTCWInput` (crouch/jump), `RTCWMerc` (recruitment, lifecycle, movement), `RTCWTactical` (decisions),
`RTCWLifecycle` (unchanged). Format: `v1 t=<game ms> ev=<name> id=<soldier 1-4, 0 = squad> key=value ...`; events only,
burst-limited (40 lines/s), `vr_merc_diag` 0/1/2 (default 1; 2 adds rejections).

| Tag | `ev` | When |
|---|---|---|
| RTCWMerc | `merc_hired`, `merc_died`, `merc_dismissed`, `merc_limit_reached` | lifecycle; the last one with `squad`, `limit`, `reason=squad_full\|no_slot` |
| RTCWMerc | `merc_follow_distance` | follow mode changed: `mode from straight path los player_speed` |
| RTCWMerc | `merc_catchup_started`, `merc_navigation_stuck`, `merc_repath` | movement; `attempt=1..3`, `method=reset_avoid\|trail` |
| RTCWMerc | `merc_formation_changed` | role changed (`role previous squad`) or the corridor mode flipped (`narrow width`, `id=0`) |
| RTCWTactical | `merc_formation_changed` | a formation move started (`role trigger=too_close\|in_front\|separation\|slot narrow travel`) |
| RTCWTactical | `merc_target_selected`, `merc_target_lost`, `merc_regroup`, `state`, `cover`, `flank`, `ff_avoid`, `designate`, `order`, ... | as in `M4_TACTICAL_AI.md` |

## 6. Performance and the measurement table

Per soldier: supervisor tick 100 ms (staggered), perception 250 ms (one pass over the client slots + one visibility check),
follow metrics 250 ms (1 trace + <= 2 AAS queries), friendly-fire test 100 ms (arithmetic), cover search <= every 2.5 s only
under pressure (<= 10 candidates), flank search <= every 2 s after its cooldown, recovery <= 6 AAS queries per step. Corridor
sensing: 2 traces per second for the squad. No allocation, no threads. **No frame-time numbers exist.**

Measurement procedure (same map, same spot, 60 s each, `com_speeds 1` for `gm:` or an external frame-time overlay):

| Run | Squad | Standing still | Walking with the squad | Firefight |
|---|---|---|---|---|
| A | 0 (`merc dismiss all`) | | | |
| B | 1 | | | |
| C | 2 (default) | | | |
| D | 3 (`vr_merc_squad 3`) | | | |

Write down `gm:` (game ms per frame) and the headset's own frame-time/CPU-level reading; repeat C/D with `vr_merc_diag 0`
to exclude logging. Do not raise the limit above 2 unless run C is as stable as run B.

## 7. Hardware test plan (21 points)

Build: Actions -> Android Debug APK -> Run workflow -> branch `pico4-m4-tactical-ai`; compare the artifact's SHA-256 with
the run summary. Capture: `adb logcat -s RTCWInput:I RTCWMerc:I RTCWTactical:I RTCWLifecycle:I RTCW:I > m4b.log`.

| # | Test | Pass looks like | Log |
|---|---|---|---|
| 1 | Hold A, release | crouches while held, stands on release | `RTCWInput ev=crouch action=pressed/released` |
| 2 | Press B | jumps, A unaffected; try A+B together | `ev=jump` |
| 3 | God Mode toggle and take hits | as in M3 | - |
| 4 | `merc hire` once, walk around | follows naturally | `merc_hired id=1` |
| 5 | Run away in a straight line for 10 s | catches up (runs), stays within a few metres | `merc_follow_distance mode=run/catchup` |
| 6 | Step around a corner/through a door, wait | comes round to you | `merc_catchup_started`, no `merc_navigation_stuck` |
| 7 | Put a crate/pillar in its way | goes round it; if stuck: repath, trail | `merc_navigation_stuck`, `merc_repath` |
| 8 | `merc hire` again | a second soldier appears, not on top of the first | `merc_hired id=2 squad=2` |
| 9 | `merc hire` a third time | refused with "squad is full (2 of 2)" | `merc_limit_reached` |
| 10 | Walk, stop, walk | two soldiers behind-left / behind-right, not stacked | `merc_formation_changed` |
| 11 | Corridor and doorway | single file, no blocking | `narrow=1` |
| 12 | Notebook: Follow / Hold / Regroup (All soldiers) | both obey | `order cmd=...` |
| 13 | Selector -> Mercenary 2, Hold; then All, Follow | only soldier 2 held | `order`, `id=2` |
| 14 | Enemies: do they fire without orders? | both engage legitimate enemies, not allies/civilians | `merc_target_selected` |
| 15 | Two enemies in view | the soldiers pick different ones (unless one is dangerous) | `merc_target_selected` |
| 16 | Stand in a soldier's line of fire | it stops shooting, you take no damage | `ff_avoid` |
| 17 | Get shot at | cover / reposition where available | `cover`, `cover_none` |
| 18 | Dismiss one, rehire; Dismiss all | no duplicates, no errors | `merc_dismissed`, `merc_hired` |
| 19 | Quicksave, load; change level | soldiers re-hired up to the previous count, no crash | `merc_hired` |
| 20 | Remove the headset, wait, put it back | session recovers (M3 hotfix 2B) | `RTCWLifecycle` |
| 21 | Ten minutes of combat with two soldiers; optional: third with `vr_merc_squad 3` | stable, no stuck state, no script break | whole log |

Add the frame-time table of section 6. Please report any case where the log contradicts what you saw.

## 8. Known limitations

- Everything above is unverified in the engine. Thresholds and distances are first guesses.
- The speed floor can show foot sliding; the recovery ladder cannot get through doors it is not allowed to open.
- No teleport recovery. Soldiers are not saved with the game (re-hired up to the remembered count; orders, health and ammo
  reset). The squad shares one stance and one assist setting.
- Formation roles follow living order, so a death reshuffles the others.
- The "dangerous target" exception to fire spreading uses only what the soldier can observe (the enemy's target while it is
  visible, damage to the player).
- Splash weapons are not part of the friendly-fire line test; reloading is not a cover trigger (the "no ammo" trigger
  covers an empty weapon).
- Three and four soldiers are implemented in the data structures and logic and covered by the pure tests, but they are
  **untested in the engine**; default is 2.
