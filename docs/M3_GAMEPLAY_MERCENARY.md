# M3 — B-button jump, God Mode and AI mercenary companion

Branch `pico4-m3-gameplay-mercenary-mvp`, based on `master` @ `ceca712` (M2A merged).
**Nothing here has been run on a PICO 4 or Quest.** "Implemented" below means: present in source, logic covered by
host tests, compiled by CI (see the pull request for the run). It does not mean "works on the headset".

## 0. Branch and dependencies

- Base is `origin/master` `ceca712`, which contains the M2A controller fixes (PR #3). Verified with `git log`.
- PR #4 (M2B, cinematic skip and pointer work) and its branch `pico4-m2b-cinematic-menu` (`52d19b0`) are untouched and
  **not imported**. Cinematic skipping is not a goal of M3. No M2B change is needed for any M3 feature.
- Jump in a cutscene keeps its previous behaviour (see 1.3), so M3 neither fixes nor worsens cinematic skipping.

## 1. Task A — B-button jump

### 1.1 What was traced (read from source)

```
PICO OpenXR b/click (OpenXrInput.c: BAction -> sFace2 -> raw.face2)
 -> VrInput_ComposeButtons: VRI_BTN_B (right hand) / VRI_BTN_Y (left hand)      [VrInputLogic.h, host tested]
 -> HandleInput_Default: primaryButton2 = domButton2 (B) or, with vr_switch_sticks, offButton2 (Y)
 -> jump block (gameplay branch only; the screen-layer branch handles B as Escape)
 -> was: Sys_QueEvent(SE_KEY, K_SPACE) -> CL_KeyEvent -> keys[K_SPACE].binding -> "+moveup"
 -> CL_KeyMove: up += movespeed * kb[KB_UP]  -> PM_CheckJump (upmove >= 10)
```

Findings:

| Question | Answer (source) |
|---|---|
| Is B down/up received? | The M2A trace (`ev=btn hand=R name=B`) already logs it; the bit mapping is host tested. |
| Right physical controller? | Yes: B is bound on the **right** hand for the PICO 4, Touch and Index profiles. With `vr_switch_sticks 1` the primary/secondary button groups swap (existing design, unchanged): jump becomes **left Y**, crouch **left X**, and the notebook moves to **right B**. |
| Suppressed by backpack/mounted gun? | Backpack: the press was dropped while `canUseBackpack` or `backpackitemactive == 2` (intended). Mounted gun: `+activate` instead (intended). |
| Is K_SPACE bound to `+moveup`? | **Nothing in the repository binds it.** The binding comes from the retail `default.cfg` / the user's `wolfconfig.cfg`, which are not in the repo. Every other VR action (crouch, use, kick, attack, reload) issues its `+/-` command directly; the jump was the odd one out. |
| Consumed by menu/cinematic transitions? | In a camera sequence `CL_KeyEvent` turns SPACE into `cameraInterrupt` (skip). In menus the screen-layer branch runs instead of the jump block. |
| Legitimate "cannot jump" states? | `PM_CheckJump`: 500 ms after the last jump, `PMF_RESPAWNED`, jump still held (`PMF_JUMP_HELD`), not on the ground. |
| Stuck input? | **Yes, a defect:** the release was gated by the same condition as the press. Press B, then move the hand into the backpack zone (or open a menu) before releasing, and `-moveup` / the SPACE release was never sent. |

**Root cause is not proven.** Static reading cannot show why B did nothing on the headset. The two source-level
candidates are the missing `SPACE -> +moveup` binding and the dropped release. Both are removed by the change below.
If B still does not jump, the `RTCWInput` log tells which link is missing (section 1.4).

### 1.2 Change

`VrInput_JumpStep()` (`VrInputLogic.h`, pure, host tested) is called every frame from `VrInputDefault.c`:

| Situation | Press | Release |
|---|---|---|
| Normal gameplay | `+moveup` | `-moveup` |
| Mounted gun | `+activate` (as before) | `-activate` |
| In-engine camera sequence (`cl.cameraMode`) | `K_SPACE` key event (as before, stock "skip") | `K_SPACE` up |
| Menus / blocked (backpack zone, backpack weapon select) | nothing | releases a jump that was already held |

- The route is latched at press time, so a release always matches its press.
- A press that starts while blocked is never replayed when the block ends with B still held (only a press edge can
  start a jump).
- Holding B sends exactly one `+moveup`; there is no repeat and no stuck state.
- Moving does not matter: the step takes no stick input.
- Crouch (`A`), attack, reload, notebook (`Y`), run, use, kick, menu clicks and escape are untouched.
- B in menus is still Escape; it is not remapped to jump.

### 1.3 Cutscenes

Unchanged on purpose: during a camera sequence B still queues `K_SPACE`, which `CL_KeyEvent` converts to
`cameraInterrupt`, exactly as before. M3 does not claim to fix the unreliable skip that M2B investigated.

### 1.4 Reading the log (`adb logcat -s RTCWInput:I RTCW:I`)

```
ev=btn hand=R name=B down=1 ...        B reached the app
ev=jump route=moveup down=1            jump logic fired        (new)
ev=jump route=moveup down=0            released                (new)
```
- `btn B` but no `ev=jump`: blocked (`canUseBackpack`), menu open, or a hand/scheme mismatch (check `vr_switch_sticks`).
- `ev=jump` present but no jump: the game refused it (`PM_CheckJump` conditions above) or the player is in a state
  that blocks movement.
- No `btn B`: OpenXR binding problem (see `INPUT_DIAGNOSTICS.md`).

## 2. Task B — God Mode

### 2.1 Use

- **VR:** notebook (left **Y**, `notebook` command) -> the existing "Private bucket list" page -> **"> God mode: No/Yes"**.
  This page already held the heal/armor/ammo cheats; I added the toggle there. I did not verify on hardware how the
  page-corner button of the notebook is revealed (`notebook.menu` `button_cheats`, `visible 2`).
- **Console fallback:** `vr_godmode 1` / `vr_godmode 0`. A cvar can also be pre-set without a keyboard by adding
  `+set vr_godmode 1` to `/sdcard/RTCWQuest/commandline.txt` (read at launch by `GLES3JNIActivity`).
- Default **off**, not archived: every launch starts off.
- Not B and no other gameplay button is used.

### 2.2 Behaviour

- `G_Damage` checks `G_VrGodProtects()` right after the stock `FL_GODMODE` check. No second damage system, no
  per-frame health rewrite. Decision logic: `VrGod_Protects()` in `g_vrlogic.h`.
- Protects only the local single-player human: `g_gametype == GT_SINGLE_PLAYER`, client 0, not an AI. In any other
  game type the cvar is forced back to 0 and never has an effect.
- Switching on restores health to the maximum once (`ent->health` and `STAT_HEALTH`, so the HUD matches) and shows
  "God Mode: ON" as a centre print. Switching off only reports; normal damage resumes with the next hit.
- Enemy AI still sees and attacks the player normally (no notarget). No ammo, noclip or other cheat is touched.

| Damage source | Result with God Mode on | Basis |
|---|---|---|
| Bullets, melee, explosions, flames | blocked | all go through `G_Damage` with flags 0 |
| Falling | blocked | `g_active.c:933`, flags 0 |
| Water / lava | blocked | `g_active.c:312,335`, `DAMAGE_NO_ARMOR` / 0 |
| `trigger_hurt` | blocked, except with spawnflag 8 | `g_trigger.c:506` |
| Telefrag, `trigger_hurt` with spawnflag 8 | **still kills** | `DAMAGE_NO_PROTECTION`, same as the stock flag |
| Tramcar crush | **still kills** | stock code clears the god flag first; kept by passing `DAMAGE_NO_PROTECTION` |
| `kill` console command / suicide | still works | calls `player_die` directly |
| Scripted death/mission failure that does not go through `G_Damage` | not blocked (not assessed per map) | |

- **Save / load / level change / respawn:** the cvar lives in the engine, outside the savegame and outside
  `ClientSpawn` (which resets `ent->flags`), so it persists across all four. This is why a cvar was used instead of the
  `FL_GODMODE` flag.
- Mission scripts are not disabled.

## 3. Task C — AI mercenary companion (MVP)

### 3.1 What is reused from RTCW

| Need | Reused |
|---|---|
| Character, weapons, animations, sounds | `AICHAR_AMERICAN` (Thompson, pineapple grenades, `american/default`, allied team); `AICHAR_PARTISAN` as fallback type. Retail assets only, nothing added or redistributed. |
| Spawning | `AIChar_spawn` from a runtime entity filled like an `ai_american` map entity |
| Following, doors, obstacle avoidance | the stock leader logic: `cs->leaderNum = player` (what using a friendly NPC does in the original game); chase/idle/battle functions keep within 256 units of the leader |
| Holding a position | the stock "wait goal" entity + `AIFunc_ChaseGoalStart` (used by the original "stay" order) |
| Combat, targeting, visibility, weapon handling | stock AICast sight and fight code (time-sliced, already budgeted) |
| Navigation | AAS/botlib via the stock AI; the supervisor only queries `trap_AAS_*` for spawn validation |
| Removal | `trap_DropClient`, as the engine does for all Cast AI at level end |

No new AI framework, navigation code, threads, network access or LLM is used. Everything runs offline in the game module.

### 3.2 Controls

All of these go through the console command `merc ...` (forwarded to the game like `give`), which the VR buttons run.

| Action | VR | Console |
|---|---|---|
| Hire (free, one at a time) | notebook -> Private bucket list -> "> Hire a mercenary" | `merc hire` |
| Follow me | "> Mercenary: follow me" | `merc follow` |
| Hold position | "> Mercenary: hold position" | `merc hold` |
| Assist in combat on/off | "> Mercenary: assist in combat on/off" | `merc assist [0\|1]` |
| Model for the next hire (automatic if unset or unusable) | "> Mercenary: model (cycle, next hire)" | `merc type [<model>\|auto]`, `merc models` |
| Dismiss | "> Dismiss the mercenary" | `merc dismiss` |
| Status | - | `merc status` |
| Toggle follow/hold in the world | look at the mercenary and press **use** (dominant stick click, `+activate`; assumed to reach the stock activate hook, unverified) | - |

No existing button (B, trigger, grip, sticks) was repurposed. The menu actions take effect when the menu is closed
(the game is paused while a menu is open).

### 3.3 State machine (`VrMerc_NextState`, `g_vrlogic.h`)

```
 hire -> FOLLOW <-> HOLD         (orders)
 FOLLOW/HOLD/RETURN --target seen, assist on--> ENGAGE
 ENGAGE --target gone 2.5 s--> FOLLOW or HOLD (by order), or RETURN_TO_PLAYER if too far
 FOLLOW --> RETURN_TO_PLAYER if > 640 units from the player
 HOLD   --> RETURN_TO_PLAYER if > 512 units from the post (returns to the post)
 RETURN --arrived (320 / 96 units)--> FOLLOW / HOLD
 RETURN --no 48-unit progress for 10 s--> HOLD where it stands (gave up), no RETURN for 20 s
 any   --health 0--> DEAD        any --dismiss--> DISMISSED      (terminal until the next hire)
```

- The supervisor runs every 100 ms and does nothing at all when no companion exists or is wanted.
- **No teleport.** An unreachable destination ends with the companion holding position and a 20 s cooldown, not with
  endless movement or repeated path searches. An explicit follow/hold command ends the cooldown.
- Target safety: a stock-AI target that is the player, a companion, an ally or a neutral NPC is cleared.
  `VrMerc_IsValidHostile` is host tested.
- Friendly fire: `G_Damage` ignores damage from the companion to the player, allies and neutrals, and damage from the
  player to the companion (VR aiming accidents). Enemy fire can still kill the companion. Use dismiss to remove it.
- Assist off uses the stock `noattack` mechanism; the companion then follows/holds but does not fire.

### 3.4 Spawning and duplicates

Candidates (24, deterministic, behind the player first, 112-176 units out) must pass: straight unobstructed walk from
the player; a floor within 96 units with normal.z >= 0.7 and within 64 units of the player's height; free hull
(solids, other characters); not in water/lava/slime/nodrop; inside an AAS area with reachability and a route to the
player's area. Hire is refused (with a centre-print reason) in a cutscene, while a level/savegame is loading, in
non-single-player game types, without `bot_enable`, without AAS data, without a free client slot, or if a companion
exists or is being spawned. A pending spawn that does not appear within 5 s is cancelled and retried (at most 6 times).

### 3.5 Save, load and level change — supported behaviour

- The companion and its pending spawn request are **never written to a savegame**. Reason: the loader aborts with
  `client mis-match in savegame` for a saved client slot that the freshly started level did not create.
- The wish for a companion is the cvar `vr_merc` (not archived). After a level change, a load or the player's death
  (which reloads), the supervisor hires a **new** one near the player once the level has settled (4 s, no cutscene,
  not loading). The old one is gone; its health, ammo and orders are not carried over.
- After death or dismissal `vr_merc` is cleared; nothing is re-hired until the next "Hire".
- Quitting and relaunching starts without a companion.
- Not claimed: that this works on every campaign map. Maps without AAS data, or with scripted sequences that hold
  the player, are refused or retried; this is untested.

### 3.6 Performance

Reuses AICast scheduling. Added work: one supervisor tick per 100 ms with a handful of distance computations; spawn
validation runs once per hire (at most about 100 traces); no per-frame scans, no allocations, no threads, bounded
logging (state changes only). An extra Cast AI character costs what any extra enemy costs.

No frame-rate numbers exist yet. **Profiling method** (needs a PICO 4):

1. Add `+set com_speeds 1` to `/sdcard/RTCWQuest/commandline.txt` (or `com_speeds 1` in the console).
2. `adb logcat -s RTCW:I | grep "frame:"`. Each line shows `gm:` (game module incl. AI, ms) and `sv:`/`cl:`/`rf:`/`bk:`.
3. Stand in the same spot in the same level for 60 s without a companion, then hire one and repeat. Compare the
   median of `gm:` and `all:` and note the headset's displayed FPS (`cg_drawFPS 1`).
4. Record the numbers in the evidence table in section 6. Do not use host numbers.

## 3a. Hotfix 1 and 2: mercenary model compatibility ("Failed to load animation scripts", then "no allied soldier model found")

### Hotfix 1 (`a8d5a1a`): the fatal error
`G_Error` in `g_client.c` fires when the server-side `G_ParseAnimationFiles()` cannot open
`models/players/<model>/wolfanim.cfg`, or `wolfanim.script` (or, for a cfg with `version` <= 1, `models/players/default.script`).
The cgame twin is `#if 0`. `<model>` is the part of the character's skin string before `/` (`american/default`).
A preflight was added so the loader is never called with an incomplete model.

### Hotfix 2: why the preflight then rejected both American and Partisan (headset report: "NO ALLIED SOLDIER MODEL FOUND")
**Confirmed from source** (not from the headset logs, which I do not have):

| Question | Answer |
|---|---|
| What the preflight demanded | `wolfanim.cfg`, `wolfanim.script` **always**, `body_default.skin` + `body.mds` (or four legacy lower/upper files), `head_default.skin`. |
| What the engine really demands | cfg; script **or** (cfg `version` <= 1 and `models/players/default.script`); the body skin for the skin name it is given; a head skin for the head name it is given. Skin names are not required to be `default`. |
| Too strict? | **Yes, in two ways (my defect):** (1) the skin names were hard-coded to `default` although they are only the stock defaults of map entities; a model whose skins are named differently (mission packs, mods) was rejected although it loads; (2) the script rule ignored the legacy `default.script` path. Whether either is the reason on your install is **not known** until the log is read. |
| Why it was not obvious | The first version printed only flags. It did not print which path was missing or which skin names exist. |

**Not determinable without your files:** which exact resource is missing for each candidate, and whether it is expected from the base game or from a
mod. The runtime now reports exactly that (below), and `tools/` can do the same offline.

Per candidate (what the code expects; "exact missing resource" comes from the log/inventory):

| Character | Runtime model (from `aiDefaults` skin) | Config / script | Mesh and skins | Source expected |
|---|---|---|---|---|
| American (`AICHAR_AMERICAN`, Thompson) | `american` | `models/players/american/wolfanim.cfg` + `.script` | `body_<skin>.skin`+`body.mds` or `lower_/upper_`; `head_<skin>.skin` | base game or a mod; unverified |
| Partisan (`AICHAR_PARTISAN`, Thompson) | `partisan` | same pattern | same | same |
| Soldier (`AICHAR_SOLDIER`, MP40) | `infantryss` | same pattern | same | base game (used by retail enemies) |
| Elite Guard (`AICHAR_ELITEGUARD`) | `eliteguard` | same pattern | same | base game (retail enemies) |

### Dynamic discovery (what the code does now)
- **Virtual file system, not paths:** every probe goes through the engine's file system (all pk3 files, `fs_game` mod folder, mission packs),
  so search order and overrides are the engine's own (`files.c`: pk3 files in a folder sorted alphabetically with `sp_` as `zz`, later wins;
  mod folders before base). A model split across two pk3 files (animation in one, skins in another) is complete.
- **Skins are discovered:** `trap_FS_GetFileList("models/players/<m>", ".skin")` -> `body_*`, `lower_*`/`upper_*`, `head_*`; `default` is preferred,
  otherwise the first one. The spawn entity gets `aiSkin = <model>/<bodySkin>` and `aihSkin = <headSkin>`.
- **Script is checked for AI states:** the script text must define `STATE RELAXED`, `ALERT` and `COMBAT` (the AI uses `aiState`); a
  player-style script (e.g. BJ's) that only loads would leave the AI in its bind pose, so it is rejected with that reason.
- **Behaviour and visual are separate:** a candidate is (`AICHAR_*` behaviour: weapons, attributes, sounds) + (model/skins). Faction is the spawn
  entity's `aiTeam = AITEAM_ALLIES`, never the model. Preference order: american, partisan, infantryss (German soldier model, allied faction),
  eliteguard. A model that is not in that list (a mission-pack character, `bj2`, ...) is **never chosen automatically**; it can be forced with
  `merc type <model>` and is then used with the American behaviour only if it passes every check.
- **Hostile-looking visuals** are only reached after the allied-looking ones fail. Their targeting is team-based, so allies and the player are never
  targets; companion damage to the player/allies is blocked and player shots do not hurt it (same code as before). Collision uses the AI's own
  bounding box (`aiDefaults`), not the model. What cannot be verified on the host, and must be checked on the headset: that the model really
  animates (walk/run/idle/fire) with the chosen weapon.
- **Fail safely:** if nothing passes, the hire is refused with the exact missing resources in logcat (`VR mercenary: model ... MISSING: ...`) and
  the message "no compatible soldier model (see log, merc models)". Nothing is spawned, no animation error is suppressed, no placeholder.
- **Reports:** `merc models` (console, or run once from the menu "model" button then read logcat) prints every native pairing and every other
  model directory with a verdict.

### Offline inventory of your install (nothing extracted or uploaded)
- Windows: `powershell -ExecutionPolicy Bypass -File .\tools\Inventory-RtcwModels.ps1 -Path D:\RTCW\Main, D:\RTCW\<mission pack folder>`
- Anywhere with Python 3: `python tools/inventory_rtcw_models.py <Main> [<mod folder> ...]` (this one I ran against synthetic pk3 files: precedence,
  cross-pk3 merging, mod override and the AI-state check behave as described; the PowerShell script is a port that I could not execute).
- The report lists every model folder with COMPLETE/INCOMPLETE, the exact missing paths and which pk3 provides each file.

### What to verify on the PICO 4
1. Install the APK, `adb logcat -c; adb logcat -s RTCW:I RTCWInput:I RTCWLifecycle:I > run.log`, start a level, notebook -> cheats page ->
   "Mercenary: model" (cycles) / "Hire a mercenary".
2. Expect in `run.log`: one `VR mercenary: model '<m>' (...)` line per candidate with `MISSING:` details, then `spawning ... with model ... skin ... head ...`.
3. A visible, animated soldier appears behind you. Walk, stop, hold, follow, fire at an enemy: animations must play (no T-pose/sliding).
4. If it still says "no compatible soldier model", send `run.log` and the inventory report: the exact missing paths decide the next step.
5. Run `merc models` once and send the log as well (shows non-native model folders that qualify).

## 4. Tests

```
make -C tests/vr_input test       # 139 checks (98 existing + 41 new for jump/diagnostics)
make -C tests/vr_gameplay test    # 544 checks: God Mode, hire rules, spawn candidates, friend/foe, state machine,
                                  #             stall watchdog, auto re-hire policy
```
Both also run in `.github/workflows/input-host-tests.yml` (pull requests touching the files).
These are deterministic host tests of the pure logic that the game and VR layer call. They do not run the engine,
AAS, traces or OpenXR. Deliberate mutations (12 in `g_vrlogic.h`, 3 in `VrInputLogic.h`) each make a test fail.

Covered: B press/release/hold and routes; God Mode on/off, single-player only, AI never protected, no-protection
damage; hire refusals and duplicate prevention (five rapid commands -> one request); spawn candidates; friend versus
enemy; friendly-fire policy; every state transition; dead/dismissed terminal states; stall watchdog, give-up and
cooldown over a two-minute simulation; auto re-hire after a load without duplicates.

Not covered by host tests: anything that needs the engine (actual traces, AAS routing, the AI's own behaviour, the UI
menu, savegame files). The edited game files were syntax-checked with host gcc against the real headers; the Android
build is the compile check.

## 5. Known unsupported / unverified

- No hardware evidence for any feature.
- On the owner's headset the American model failed to load and then both candidates failed the first preflight (section 3a). The
  game now discovers the files and skins it can use; if nothing qualifies it refuses and prints the exact missing paths.
- One companion only (`VRMERC_MAX_COMPANIONS`); multiplayer and LAN co-op are out of scope.
- Companion state is not persisted (3.5). A savegame made with a companion loads without one, then a new one is hired.
- No voice or gesture commands; no equipment/economy; no recovery teleport.
- HOLD does not stop the stock AI from chasing a target beyond the leash while engaged; it returns afterwards.
- Hire/dismiss from the menu only act after the menu closes.
- Cinematic skipping remains as on `master` (not part of M3).
- `vr_godmode`/`vr_merc*` are plain cvars: anyone with console access can set them; single-player gating is enforced in
  the game module.

## 6. PICO 4 acceptance checklist (to be filled by the owner)

APK: workflow `Android Debug APK` on this branch, artifact `RTCWQuest-PICO4-Debug-ARM64`. Record commit and SHA-256.
Capture `adb logcat -s RTCWInput:I RTCW:I`.

| # | Test | Expected | Result / evidence |
|---|---|---|---|
| 1 | Walk, then press **B** | player jumps; `ev=jump route=moveup down=1/0` | |
| 2 | Jump while moving forward and strafing | jumps, no stuck input after release | |
| 3 | Hold B for 3 s | one jump, no auto-repeat, nothing stuck | |
| 4 | Press B in a menu | acts as Escape, no jump | |
| 5 | Crouch (A), shoot (trigger), reload (grip tap), notebook (Y) | unchanged | |
| 6 | `vr_switch_sticks 1`: press left **Y**, left **X**, right **B** | jump, crouch, notebook (jump no longer on right B) | |
| 7 | God Mode on (notebook page), get shot | health shows 100 and stays; enemies still shoot | |
| 8 | Fall from a height; stand in fire/water if available | no damage | |
| 9 | God Mode off, get shot | health drops normally | |
| 10 | God Mode on, save, load, change level, die-and-reload | still on after each | |
| 11 | Hire a mercenary in a level with enemies | appears behind you, within a few seconds | |
| 12 | Walk away, through a door | follows, does not block the door | |
| 13 | Hold position, walk away | stays; engages enemies that come near | |
| 14 | Follow me again | rejoins | |
| 15 | Fight a group | mercenary shoots enemies, never you or friendly NPCs | |
| 16 | Shoot toward the mercenary | no damage to it | |
| 17 | Assist off | does not fire; assist on: fires again | |
| 18 | Dismiss | disappears; hire again works; no duplicate | |
| 19 | Hire twice quickly | only one mercenary | |
| 20 | Hire, save, quit app, relaunch, load | no crash; no mercenary until hired again | |
| 21 | Hire, save, load in session | no crash; a new mercenary appears after a few seconds, not two | |
| 22 | Hire, change level | a new mercenary appears near you in the next level | |
| 23 | 10+ minutes of play with the mercenary | no crash, no freeze, no runaway log | |
| 24 | `com_speeds` comparison (3.6) | record `gm:` and FPS with / without | |
