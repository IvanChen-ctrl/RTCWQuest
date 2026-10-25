# M4 — Tactical mercenary AI

Branch `pico4-m4-tactical-ai`, stacked on `pico4-m3-gameplay-mercenary-mvp` (M3, PR #5, still open when M4 was built).
**Nothing in this milestone has been run on a PICO 4, a Quest or in a game engine.** "Implemented" means: present in
source, decision logic covered by host tests, compiled by the Android workflow (see the pull request for the run).
Whether the companion actually behaves as described is exactly what the hardware checklist in section 10 is for.

Related: `docs/M3_GAMEPLAY_MERCENARY.md` (baseline), `docs/M5_PICO4_LAN_COOP_FEASIBILITY.md` (research only),
**`docs/M4_SQUAD_AND_MOVEMENT.md`** (the hardware-feedback round: A-button crouch, adaptive following and recovery,
the multi-soldier squad, formation, the 21-point hardware plan). Where this document says "the companion" in the singular,
read "each soldier": the logic is per soldier since that round, and the squad default is 2.

## 1. Summary

A tactical decision layer sits above the stock AICast code. It decides **what** the companion should do (which enemy,
whether to take cover, flank or regroup, where to stand) and then calls the stock functions for **how** (movement and
navigation through AAS, aiming, firing, animation).

| Behaviour | Implemented how |
|---|---|
| Follow with spacing | Stock leader logic (as in M3) + a formation correction: not on the player, not in front of the weapon |
| Target choice | Threat score per known enemy, commitment time and hysteresis, applied to the stock AI target |
| Attack My Target | Notebook button arms it; the next fire press designates what the weapon points at |
| Avoid friendly fire | Active hold-fire when the player or an ally is on the line of fire (plus the M3 immunity) |
| Cover | Engine hide position (AAS route table) + local samples, validated; executed by the stock take-cover function |
| Flank | Situational side move with line-of-fire, detour and fire-line checks; bounded goal-entity walk |
| Regroup / retreat | Stock chase of the player, command-driven or low-health/isolation driven, with time limit and cooldown |
| Stances | Defensive (default) and Aggressive change scoring, cover and flank thresholds |
| Diagnostics | Logcat tag `RTCWTactical`, state changes and decisions only |

A squad of up to 4 independent soldiers (default 2) since the hardware-feedback round; this document's behaviour description is per soldier. Everything runs in the game module, offline, with no threads, no allocation and no
network.

## 2. Baseline verification (before adding tactics)

Traced by reading the code; **not run**. "Effective" means the command ends in a stock AI call or game action.

| Path | Code | Effective? |
|---|---|---|
| Hire | `merc hire` -> `VrMerc_TryHire` -> `VrMerc_RequestSpawn` -> stock `AIChar_spawn` -> `VrMerc_PollSpawn`/`Adopt` | Yes: a real Cast AI character is created |
| Follow | `cs->leaderNum = player` (stock leader logic: idle/battle/chase keep within 256 units of the leader and walk back to 240) | Yes, stock |
| Hold | `VrMerc_AnchorAt`: wait-goal entity + `AIFunc_ChaseGoalStart` (what the original "stay" order does) | Yes, stock |
| Enemy acquisition | Stock `AICast_ScanForEnemies` in `AIFunc_Idle/ChaseGoal/ChaseGoalIdle/Battle*` (also scans while holding) | Yes, stock |
| Firing | Stock `AICast_ProcessAttack`; `assist off` = stock `scriptNoAttackTime` | Yes, stock |
| Dismissal | `VrMerc_Finish` -> `trap_DropClient` | Yes |
| Death | supervisor sees `health <= 0` -> `VrMerc_Finish(DEAD)` | Yes |
| Save/load | companion skipped by `G_SaveGame`; `vr_merc` cvar re-hires after the level reloads | Yes (by design: not persisted) |
| Level change | `VrMerc_Init` clears all state per level; the cvar re-hires | Yes |

Findings:

- No command was found that only changed a variable without reaching the AI. The `ENGAGE` state is observational by
  design (the stock AI does the fighting; the state only drives the return logic).
- Gaps closed in M4: the companion's temporary goal entities (hold post) could be written into savegames (now skipped);
  there was no spacing/formation, no active friendly-fire avoidance, and no target policy beyond the stock one.
  Every **new** M4 movement is guarded against mission scripts (`VrMerc_ScriptBusy`); the M3 `RETURN`/`HOLD` entry actions
  are not guarded and were left as they were.
- Not changed because it needs the real game to judge: whether the stock follow distance (64-256 units) feels right,
  and whether `AIFunc_ChaseGoalStart` called from the supervisor interacts cleanly with every stock state. The stock
  "use" order does the same call, which is why it was kept.

## 3. Architecture

```
g_merc.c  (engine glue)                      g_vrtactics.h / g_vrlogic.h (pure, host tested)
-------------------------------------        ----------------------------------------------
VrMerc_Frame (every server frame, idle       VrTac_ValidateCommand
  unless a companion/spawn/wish exists)      VrTac_ClassifyTarget / ValidateDesignation
  - designation poll (fire press)            VrTac_ThreatScore / SelectTarget (hysteresis)
VrMerc_Supervise (every 100 ms)              VrTac_FriendInLineOfFire
  - VrMerc_Perceive      (every 250 ms) --->  VrTac_RepositionReason / FormationSlot
  - VrMerc_AvoidFriendlyFire                  VrTac_CoverReject/Score/WantCover/CoverExit
  - VrMerc_NextState (M3 base machine) --->   VrTac_FlankPoint/Reject/WantFlank/FlankExit
  - VrMerc_Tactics -> maneuver steps          VrTac_WantRegroup/RegroupExit
        calls stock AIFunc_* / AICast_*       VrMerc_NextState (M3, extended)
```

Rules the glue follows:

- It only changes the AI through functions the original game already calls from outside the think loop
  (`AIFunc_ChaseGoalStart`, `AIFunc_BattleTakeCoverStart`, `AIFunc_IdleStart`, `AIFunc_DefaultStart`,
  `AICast_StateChange`, `cs->enemyNum`, `scriptNoAttackTime`).
- It never moves a character directly and never teleports.
- It never acts while a mission script is controlling the character (`castScriptEventIndex >= 0` or a script
  `nomove`).

## 4. Tactical context (what the companion knows)

Rebuilt at most every 250 ms (`VRTAC_PERCEPTION_MS`), kept in a static struct (`mtx`):

| Item | Source |
|---|---|
| Player position, aim ray | `g_entities[0]`; hand ray from `CalcMuzzlePointForHandActivate` when `gVR` exists, else the view angles |
| Companion position, health, weapon, range | its entity, `cs->weaponNum`, a weapon-range table |
| Current order, stance, state, time in state | supervisor |
| Hostile candidates (max 8, nearest kept) | entities of Cast AI **with a sighting in the companion's own sight memory** (`cs->vislist[i]`: `real_visible_timestamp` / `visible_timestamp`), or the one the player pointed at. Never-detected enemies are skipped. |
| Visibility, time since seen | the same memory (`visibleNow` = seen within 700 ms; entries older than 6 s are dropped) |
| Whether the enemy targets the player/companion | the enemy's own AI target, read **only while the companion currently sees it** (approximates "it is aiming at X") |
| Recent damage to player/companion | `VrMerc_NoteDamage`, called from `G_Damage` (ring of 8 hits, windows of 3-4 s) |
| Last known enemy position | `vislist[].visible_pos` |
| Friendly NPC positions (max 8, within 1500 units) | allied and neutral Cast AI |
| Line of fire to the committed target | `AICast_CheckAttack` once per scan |
| Navigation | AAS area and `trap_AAS_AreaTravelTimeToGoalArea` for each candidate position |
| Obstacles/cover geometry | one `trap_Trace` box test and floor snap per candidate; `AICast_VisibleFromPos` for concealment |

Limits: the "enemy targets X" reading and "neutral/ally" classification rely on the enemy's AI state being observable
while visible; this is an approximation of what a human would see, not a perfect model.

## 5. State machine

M3 states are unchanged (values `NONE..DISMISSED` keep their numbers). Three maneuver states were appended.
The suggested `DEFEND_PLAYER` and `RETREAT` states were **not** added as separate states because they would be
redundant: defending the player is the Defensive stance plus "Cover me" (scoring), and a retreat is `REGROUP` entered
with reason `low_health`.

| State | Entry | In-game action | Exit | Failure fallback | Cooldown | Cleanup |
|---|---|---|---|---|---|---|
| `FOLLOW` | order follow; return from any state | stock leader following; formation correction (see 6.1) | order hold; far -> `RETURN`; target -> `ENGAGE` | - | - | formation goal freed |
| `HOLD` | order hold; `RETURN` gave up | wait goal + stock chase | order follow; leash exceeded -> `RETURN` | stay put | 20 s after a failed return | wait goal freed |
| `ENGAGE` | assist on and a valid hostile target | stock combat; tactical target applied | 2.5 s without target | - | - | - |
| `RETURN` | too far from player/post (M3) | stock chase of the player/post | arrived (320 units of the player / 96 of the post) | 10 s without progress -> `HOLD`, no teleport | 20 s | - |
| `SEEK_COVER` | sustained fire (>= 3 hits in 4 s), heavy damage (>= 30% in 4 s), or a defensive HOLD with a visible target within 1300 | `AIFunc_BattleTakeCoverStart` toward a validated cover position (or crouch in place) | stayed >= 2.5 s, calm 3 s -> done; 7 s timeout; target gone 2.5 s; script | no usable cover -> keep fighting with the stock behaviour; stalled 2.5 s -> counted failure | 6 s after failure, 8 s after success; one search per 2.5 s | `man` cleared, AI handed back |
| `FLANK` | no clear shot for 1.5 s (aggressive) / 3 s (defensive) **or** aggressive and 5 s on the same target; never under HOLD or "cover me"; health >= 50%; target 250-1100 away | goal entity at a validated side position; stock chase while the stock AI keeps firing; 3.5 s firing dwell on arrival | arrived + dwell; 9 s timeout; target dead/invalid; health < 40%; stalled 3 s; script | candidate rejected -> counted failure, 20 s cooldown | 14 s; 3 failures stop it (reset after 60 s) | goal entity freed |
| `REGROUP` | `merc regroup`; or health < 35% under fire (HOLD: < 20%); or > 1100 units from the player under fire | stock chase of the player | within 260 units; 14 s timeout; stalled 4 s; script | gives up, state returns to the order | 20 s between automatic regroups | - |
| `DEAD` | health <= 0 | cleanup | terminal until the next hire | - | - | maneuver, hold goal, fire hold, designation cleared |
| `DISMISSED` | `merc dismiss` | `trap_DropClient` | terminal until the next hire | - | - | same as DEAD |

Other rules:

- A maneuver is never started from another maneuver; a new order cancels it (`ORDER_CHANGED`).
- HOLD is respected: no flanking, no isolation regroup. The only exceptions are an explicit `merc regroup` and a
  near-death retreat (< 20% health under fire), which converts the order to follow (otherwise the companion would walk
  straight back into the same fire).
- After any maneuver the state is `ENGAGE` if a target remains and assist is on, otherwise the standing order's state.
- A consistency guard recovers a maneuver state that lost its maneuver record (`ev=recover`).

## 6. Behaviours

### 6.1 Follow and formation

The stock leader logic walks the companion to within 240 units of the player and stops at 64. M4 adds, only in
`FOLLOW` with no target and no script, checked every 750 ms:

- **too close** (< 56 units horizontally) or **in front of the weapon** (inside about 24 degrees of the aim line,
  56-420 units away) -> move to a slot **behind and to the side** of the player (130 back, 90 sideways, the side the
  companion is already on first, then the other).
- The slot must pass the navigation check (floor, solid, liquid, AAS reachable, travel <= 6 s). If neither slot does
  the companion stays put and does not retry for 4 s.
- Bounded: 5 s maximum, 4 s cooldown, progress watchdog, cancelled by any target or script.
- Aim direction in VR is the dominant hand ray (`gVR`); if the game runs without VR data the view angle is used.
  *Whether the hand ray is the right "weapon direction" for every weapon is ASSUMED.*

### 6.2 Target selection

Score per candidate (`VrTac_ThreatScore`): base 100, nearer is higher (up to +60), currently visible +40 (otherwise
decays with the time since seen), targets the player +45 (+25 more in Defensive or "cover me"), targets the companion
+35, hurt the player within 3 s +30, hurt the companion within 3 s +25, in weapon range +15 (otherwise -25), aggressive
stance: nearby bonus, "cover me": enemies near the player gain, **designated +200**. Stale sightings (> 6 s) are dropped.

`VrTac_SelectTarget` keeps the committed target for at least 2.5 s unless a newcomer is better by +90 points
(emergency, for example a designation); after that a newcomer must beat it by 30% and by 12 points. The result is
applied to the stock AI (`cs->enemyNum`) when the stock AI has no valid target, when the tactical choice just changed,
or (re-asserted at most every 1.5 s) while a designation is active. The stock acquisition continues to work; this layer
biases and overrides it, it does not replace it.

Never targeted (`VrTac_ClassifyTarget`): the player, the companion, allies, neutral NPCs (civilians, story characters),
dead characters, anything that is not a Cast AI character (props, doors).

### 6.3 Friendly-fire avoidance

Every 100 ms the segment from the companion to its target is tested against the player (radius 44) and up to 8 nearby
allied/neutral characters (radius 36). If one is in between, the companion's attack is held for 450 ms through the stock
`scriptNoAttackTime` (only our own hold is released, never a script's) and `ev=ff_avoid` is logged at most every 2 s.
The M3 damage immunity remains as a safety net. Splash weapons (grenades) are **not** specially handled.

### 6.4 Cover

Triggered by `VrTac_WantCover` (table in section 5). The search (max once per 2.5 s) builds up to 10 candidates:
the engine's own `AICast_GetTakeCoverPos` result (a crouch-in-place or an AAS route-table hiding spot) and 8 ring
samples 250 units around the companion starting away from the threat. Each is validated by `VrTac_CoverReject`:
not solid/liquid, AAS-reachable within 7 s, **concealed** from the enemy's last known position
(`AICast_VisibleFromPos`), 48-640 units away, not more than 64 units closer to the threat, not within 56 units of the
player or 70 units of the player's line to the threat, and within 384 units of the post when holding. The best score
wins and the stock `AIFunc_BattleTakeCoverStart` walks there (and keeps fighting on the way, as the stock AI does).
No usable spot: nothing changes and the companion keeps fighting; the search is retried after 6 s.

*Concealment uses one stock visibility ray from the enemy's last known position; it is not a full cover analysis.*

### 6.5 Flank

See the table in section 5. Candidates: two angles (70 and 100 degrees) on both sides of the enemy at 300-600 units.
Each is checked for floor/AAS reachability, a detour limit (travel time <= 2x the straight-line time + 1.5 s and <= 7 s),
distance to the enemy >= 200, **not crossing the player's line of fire** (9 samples along the walk and the destination
against the player-to-enemy line, margin 80), and an open straight line from the destination to the enemy. If none
qualifies the flank is cancelled (`ev=flank_cancelled`) and the cooldown starts.

### 6.6 Regroup and retreat

`merc regroup` (or the notebook button) ends HOLD, cancels a running maneuver and sends the companion to the player. It
is the same stock chase the M3 `RETURN` used, but forced, bounded and usable during combat. Automatic triggers are low
health under fire and isolation under fire, with a 20 s cooldown, so retreat/engage cannot loop faster than that.

### 6.7 Stances

| | Defensive (default) | Aggressive |
|---|---|---|
| Target scoring | player threats weighted higher | nearby enemies weighted higher |
| Cover | sustained fire is enough | only with heavy damage or health < 50% |
| HOLD | cover when exposed | keeps shooting |
| Flank | waits 3 s without a clear shot | 1.5 s without a clear shot, or a 5 s stalemate |

Both stances use the same friendly-fire, navigation and cooldown rules. Aggressive does **not** chase enemies beyond the
stock leash; its extra reach is the bounded flank. *Whether this is aggressive enough in play is a tuning question for
the hardware test.*

### 6.8 Attack My Target

1. Notebook -> Private bucket list -> **Mercenary orders >>** -> **Attack my target** (or console `merc attack`).
   This only **arms** the order for 15 s ("Mercenary: aim at an enemy and fire").
2. Close the notebook. Point the weapon at a hostile and pull the trigger.
3. The first fire press while armed traces from the dominant hand (`MASK_SHOT`, 12-unit box, 2400 range). The hit must
   be a living hostile Cast AI character in range: not the player, the companion, an ally, a civilian, a prop or the
   world. A reason is shown otherwise (for example "that is a friend").
4. On success the target is told to the companion's sight memory (as its "last known position"), gets +200 score and is
   re-asserted for 30 s. It expires when the target dies or is invalid; the companion then returns to normal
   behaviour. Designation state is cleared on dismissal, death and level change.

Moving the controller alone never issues an order; `merc attack now` (console, for testing) designates immediately.

### 6.9 Cover Me

`merc cover`: ends HOLD (follow), comes to the player if more than 400 units away, and for 20 s weights threats near the
player higher and disables flanking. It is a bias, not a scripted action; the companion still fights with the stock AI.

## 7. Commands

| Action | Notebook (Private bucket list -> Mercenary orders >>) | Console |
|---|---|---|
| Follow Me | `> Follow me` | `merc follow` |
| Hold Position | `> Hold position` | `merc hold` |
| Attack My Target | `> Attack my target` (then aim and fire) | `merc attack` (arm) / `merc attack now` |
| Cover Me | `> Cover me` | `merc cover` |
| Regroup | `> Regroup` | `merc regroup` |
| Defensive Stance | `> Defensive stance` | `merc stance defensive` |
| Aggressive Stance | `> Aggressive stance` | `merc stance aggressive` (`merc stance` toggles) |
| Assist On / Off | `> Assist on / off` | `merc assist [0\|1]` |
| Dismiss | `> Dismiss` | `merc dismiss` |
| Hire | `> Hire a mercenary` | `merc hire` |
| Status | - | `merc status` |

Cvars: `vr_merc_stance` (0/1, not archived), `vr_merc_diag` (0/1/2, default 1), plus the M3 `vr_merc` (now the number of
soldiers to keep), `vr_merc_assist`, `vr_merc_model`; squad round: `vr_merc_squad` (limit, default 2), `vr_merc_sel` (0 = all,
1-4), `vr_merc_move` (adaptive following, default 1). The orders accept `all` or a soldier number (`merc hold 2`). The existing buttons, God Mode and everything else on the cheats page are unchanged;
a link was added at the bottom right. New file: `z_vr_assets/ui/mercenary.menu` (registered in `ui/ingame.txt`).
As in M3, menu commands take effect after the notebook closes (the game is paused while it is open). Whether the cheats
page corner button is reachable in VR is still unverified.

## 8. Navigation limitations

- Everything depends on AAS data. A map without it refuses the hire (`no navigation data`).
- All movement uses stock routing; the tactical layer only chooses destinations. Unreachable or blocked destinations
  are caught by the AAS reachability test and by a progress watchdog (no movement gain for 2.5-4 s) and end in a logged
  `ev=nav_failure`. There is no teleport and no recovery jump.
- The companion cannot follow through script-driven transitions (elevators, tramcars, scripted doors) beyond what the
  stock AI does; scripted moments take priority over every tactical decision.
- Cover quality is a heuristic (see 6.4). Flanking needs open space; in corridors it will usually find no candidate.
- Splash weapons, grenades and the companion's own explosives are not part of the friendly-fire line test.

## 9. Performance and budgets

| Budget | Value |
|---|---|
| Idle cost (no companion, no wish) | one integer test per server frame, as in M3 |
| Supervisor tick | 100 ms |
| Perception scan | 250 ms; one loop over the client slots + one `AICast_CheckAttack` |
| Cover search | at most once per 2.5 s and only while under pressure; <= 10 candidates (about 3 traces + 1 AAS query each) |
| Flank search | at most once per 2 s after the cooldown; 4 candidates |
| Formation check | 750 ms; navigation check only when the companion is in the way |
| Friendly-fire test | 100 ms; pure arithmetic |
| Memory | static structs only; no allocation; no threads |
| Logging | events only; burst limit 40 lines/s |

**No frame-time or FPS measurement exists.** To compare with and without the companion on the headset:

1. Same map, same spot, same weapon. Console: `com_speeds 1` (stock output includes `gm:` game time per frame) or an
   external frame-time overlay.
2. Run 60 s with `merc dismiss` (no companion), note `gm:` and the headset's frame-time/CPU level; then `merc hire`,
   60 s in `FOLLOW` standing still, 60 s in a firefight.
3. Repeat with `vr_merc_diag 0` to exclude logging. `merc status` shows `supervised` and `perceptions` counters.

## 10. Diagnostics

`adb logcat -s RTCWTactical:I RTCWMerc:I` (add `RTCW:I RTCWInput:I` to correlate). Recruitment, lifecycle and movement events use the tag `RTCWMerc` (see `M4_SQUAD_AND_MOVEMENT.md`); decisions use `RTCWTactical`. Format, one event per line:

```
v1 t=<level.time ms> ev=<name> id=<soldier 1-4, 0 = squad> key=value key=value ...
```

`t` is the game clock (it stops while the game is paused). Reason values are the stable tokens of
`VrTac_ReasonName` (for example `sustained_fire`, `cover_toward_enemy`, `flank_crosses_fire`, `nav_stall`).
`vr_merc_diag`: 0 off, 1 events, 2 adds rejections.

| `ev` | Fields | When |
|---|---|---|
| `merc_hired` (tag `RTCWMerc`) | `ent name squad character stance` | a soldier joined |
| `order` / `order_rejected` | `cmd ...` / `cmd reason` | a command was accepted / refused |
| `state` | `from to reason after_ms` | every state change |
| `target` / `target_lost` / `target_rejected` | `ent prev score dist visible designated candidates` / ... / `ent reason` | tactical target changes; a designation was refused |
| `designate`, `designate_disarmed`, `designation_end` | `ent dist` / `reason` / `target reason` | designation lifecycle |
| `ff_avoid` | `target blocker` | fire held because a friend was in the line (max every 2 s) |
| `reposition` | `reason side travel` | formation move started |
| `cover` / `cover_none` / `cover_rejected` (v2) | `trigger kind dist travel score candidates target` / `candidates last_reject trigger` / `cand reason dist` | cover chosen / not found / candidate refused |
| `flank` / `flank_cancelled` / `flank_rejected` (v2) | `trigger target travel dist_to_enemy no_shot_ms` / `reason fails` / `reason` | flank lifecycle |
| `regroup` | `trigger dist_to_player health` | regroup/retreat started |
| `maneuver_end` | `kind reason took_ms` | any maneuver ended |
| `nav_failure` | `maneuver dist ...` | a destination could not be reached (stall/timeout/return give-up) |
| `stance`, `cover_me_end`, `recover`, `merc_died`/`merc_dismissed`, `drop` | ... | misc |

The Windows Feedback Bridge only needs to capture this tag; nothing in that project was changed.

## 11. Automated tests

| Suite | Checks | Covers |
|---|---|---|
| `tests/vr_tactics` (new) | 3042 | command validity, friend/foe classification (exhaustive table), designation range, damage windows, threat scoring, target selection and hysteresis (noise and switch simulations), friendly-fire geometry, formation slots and settling, cover reject/score/pick/want/exit, flank geometry/reject/want/exit/cooldown/failure limit, regroup/retreat/HOLD exception/cooldown, loop bounds, maneuver states in the base machine |
| `tests/vr_squad` (hardware-feedback round) | 653 | adaptive follow modes and hysteresis, speed floor, stuck-recovery ladder, breadcrumb trail, squad limit/identity/addressing, formation roles, target distribution, recruitment sequences |
| `tests/vr_gameplay` | 544 | M3 mercenary and God Mode logic, unchanged |
| `tests/vr_input` | 53608 | controller input, plus (hardware-feedback round) the crouch button state machine, the crouch height rule and their pipeline from the raw hand input |
| `tests/vr_lifecycle` | 94 | OpenXR/Android lifecycle, unchanged |

Run: `make -C tests/vr_tactics test` (also `vr_gameplay`, `vr_input`, `vr_lifecycle`); all four run in
`.github/workflows/input-host-tests.yml`.

Mutation checks: 40 deliberate edits (16 in the foundation, 24 in cover/flank/regroup) of `g_vrtactics.h`/`g_vrlogic.h` (for example removing the neutral-NPC rejection, the
hold leash, the flank cooldown, the line-of-fire radius, the commitment time) each made at least one test fail.

What the tests do **not** prove: anything in `g_merc.c` (traces, AAS, the stock AI calls, the UI, savegames, entity
cleanup) because it needs the engine. Those parts were compiled against the real engine headers
(`gcc -fsyntax-only`, zero warnings in `g_merc.c`) and by the Android workflow, and nothing more. The "save/load",
"cleanup" and "invalid entity reference" requirements are covered by design (state cleared in `VrMerc_Finish` and
`VrMerc_Init`, saved-entity skip) and by the M3 session-reset tests, not by an engine-level test.

## 12. Hardware acceptance checklist (owner)

Build: Actions -> Android Debug APK -> Run workflow -> branch `pico4-m4-tactical-ai`; compare the artifact's SHA-256 with
the run summary. Capture: `adb logcat -s RTCWTactical:I RTCW:I RTCWInput:I > m4.log`. Record each result in a copy of
the table in `docs/PICO4_INPUT_TEST_PLAN.md` style (date, build SHA, result, log excerpt).

| # | Test | Do | Pass looks like | Log |
|---|---|---|---|---|
| 1 | Hiring | Notebook -> bucket list -> Hire | "Mercenary: hired", soldier appears near you | `merc_hired` |
| 2 | Model/animation | Walk, stop, turn | Visible, animated, not T-posed | - |
| 3 | Corridor following | Walk through a corridor and a door | Stays within a few metres, arrives after you | `state`, no `nav_failure` |
| 4 | Obstacles | Walk around crates/pillars | Does not get stuck; if it does, `nav_failure` | `nav_failure` |
| 5 | Hold | `Hold position`, walk away 20 m | Stays; returns when you give Follow | `order cmd=hold` |
| 6 | Designate | `Attack my target`, close notebook, aim at a guard, fire | Message, companion engages that guard | `designate`, `target designated=1` |
| 7 | Firing | Combat | Companion shoots visible enemies | `state to=ENGAGE` |
| 8 | Friendly fire | Stand between companion and an enemy | Companion stops shooting while you are in the line; you take no damage | `ff_avoid` |
| 9 | Cover | Let two enemies shoot at it | Moves to/crouches behind cover, then returns | `cover`, `maneuver_end` |
| 10 | Flank | Aggressive stance, an enemy behind a corner at 400-900 units | One short side move, then fires; not repeated within 14 s | `flank`, `flank_cancelled` |
| 11 | Regroup | Send it away (Hold), then `Regroup` | Comes to you | `regroup trigger=command` |
| 12 | Stances | Toggle Defensive/Aggressive | Message each time; behaviour differs mildly | `stance` |
| 13 | Dismiss | `Dismiss` | Soldier disappears, no error | `cleanup reason=dismissed` |
| 14 | Save/load | Quicksave, load | No crash; new soldiers appear (not the same ones) | `merc_hired` |
| 15 | God Mode | Toggle, take hits | Unchanged from M3 | - |
| 16 | B-button jump | Jump | Jumps | `RTCWInput ev=jump` |
| 17 | Sleep/resume | Remove headset, wait, wear it | Resumes (M3 hotfix 2B) | `RTCWLifecycle` |
| 18 | Ten minutes | Play a real section with the companion | No crash, no stuck state, no mission-script break | - |
| 19 | Frame time | Procedure in section 9 | Record numbers with and without | - |
| 20 | Regressions | Menu clicks, triggers, haptics, a scripted scene with the companion present | As before M4 | - |

Please report anything where the log contradicts the observed behaviour; that is the most useful result.

## 13. Known issues and not implemented

- Not run in the engine at all. Tuning values (distances, thresholds, cooldowns) are first guesses.
- Soldiers are not persisted across save/load (re-hired up to the remembered count; orders, health and ammo reset). Unchanged from M3.
- Splash/explosive weapons are not part of friendly-fire avoidance; reloading under pressure is not a cover trigger
  (not reliably observable); no voice/gesture commands; no per-weapon tactics.
- "Enemy targets X" is read from the enemy's AI target while visible, which is an approximation.
- The aggressive stance does not extend the stock leash.
- The notebook corner button reachability and the use-key hook (M3) are still unverified.
- Flanking cannot find a candidate in tight spaces; the cancellation is logged, not announced.
- Level-time stamps stop while paused, so timestamps in the log do not advance in the notebook.

## 14. Next milestones

1. Owner hardware test of this checklist; tune from the logs.
2. Companion persistence across save/load (needs an entity/cast-state save path).
3. Multiple companions (the state is one struct; it would become an array keyed by slot).
4. M5 per `docs/M5_PICO4_LAN_COOP_FEASIBILITY.md`: start with the host-build spike, then per-client VR pose.

## 15. Source files changed

`g_merc.c`, `g_vrtactics.h` (new), `g_vrlogic.h` (states appended, maneuver rule), `g_combat.c` (one call),
`g_local.h`, `g_main.c` (declarations and cvars), `z_vr_assets/ui/mercenary.menu` (new), `ui/cheats.menu`
(link), `ui/ingame.txt` (registration), `tests/vr_tactics/` (new), `.github/workflows/input-host-tests.yml`,
`.gitignore`, docs.
