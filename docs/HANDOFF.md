# HANDOFF

## Status (updated in M4 — read "Milestone 4" at the bottom first, then "Milestone 3")
M0 (audit/docs), M1A (CI workflow) and M2A (controller fixes) are merged to `master` (PR #1, #2, #3). On a real PICO 4 the
menu and controller clicks work after M2A (owner report). M2B (PR #4, cinematic skip / pointer) is **not merged**: its
cinematic fix did not pass physical validation; the existing cinematic can finish by itself and saved games load, so
cinematic skipping is no longer a blocker. M3 (this branch) branches from `master`, not from M2B.
The M0 and M1A sections below are kept as history and are partly stale.

## Start point
- Upstream state: `master` @ `4c714b9163b0de94a856ac896c6cfa56a283de74` ("Fix Tesla and a Quest Permission Issue")
- Work branch: `pico4-development`

## Verified from source (not built)
- Root `Projects/Android`; Gradle wrapper 8.2.1; AGP 8.2.1
- compileSdk 32 / minSdk 29 / targetSdk 32; Java source/target 1.8 (JDK 17 recommended for AGP 8.2; not pinned)
- NDK 25.1.8937393; ndk-build; arm64-v8a only; c++_shared
- OpenXR loader 1.1.60 from Maven (needs network)
- Modules: gl4es, qagame, ui, cgame, rtcw_client
- Manifest already has PICO launcher category (`com.picovr.intent.category.LAUNCHER`), `pvr.app.type`, `pxr.controller`, and Quest `supportedDevices`
- `OpenXrInput.c` already has a `bytedance/pico4_controller` profile; `TBXR_Common.c` detects "pico" runtime name

## Prerequisites (not installed here)
JDK 17, Android SDK Platform 32 + build-tools, NDK 25.1.8937393, network for Maven (google(), mavenCentral()).
Retail RTCW pk3 files are required at runtime in `/sdcard/RTCWQuest/Main` (not in repo; not needed to compile).

## Known issues / unknowns
- `Projects/Android/local.properties` is committed with a Windows `sdk.dir`; must be overridden in CI/cloud.
- `README.md` says `ndk.dir` in local.properties; build.gradle uses `ndkVersion` instead.
- `support-compat:28.0.0` is an old support-library dependency (not AndroidX).
- Manifest `supportedDevices` lists Quest only; effect on PICO 4 UNVERIFIED (likely ignored).
- `fileTree(libs)` includes `haptic_service.aar`; its role on PICO unverified.
- (Stale since M1A) ~~No `.github/` directory, no CI.~~ `.github/workflows/android-debug.yml` exists.
- (Stale since M1B) ~~Whether it builds at all: UNVERIFIED.~~ VERIFIED: the Actions workflow built the APK twice.

## M0 files created
`CLAUDE.md`, `docs/HANDOFF.md`, `docs/PICO4_BUILD_AUDIT.md` (no other files modified).

## Git
Commit hash: see `git log -1` on `pico4-development` (single M0 docs commit).

## Next
M1: reproducible debug APK build (GitHub Actions or a cloud session with SDK/NDK), fixing only what blocks compilation.

---

## Milestone 1A — CI build infrastructure
- Branch: `pico4-m1-build` (from master after PR #1 merge `81a5e16`). PR/commit: see the M1A pull request on GitHub.
- Files: added `.github/workflows/android-debug.yml`, `docs/PICO4_BUILD_ZH.md`; updated this file. No source/Gradle/manifest changes.
- Workflow: `workflow_dispatch` only; ubuntu-24.04; 45 min timeout; `contents: read`; JDK 17 (Temurin), SDK platform 32, build-tools 34.0.0, NDK 25.1.8937393 via sdkmanager; CI-only `local.properties`; `./gradlew assembleDebug` (runs existing `extractOpenXrLoader`, `packResources`, ndk-build); then APK validation, SHA-256, 2-day artifact `RTCWQuest-PICO4-Debug-ARM64`.
- Build-tools 34.0.0 is the AGP 8.2.1 default (assumed; not pinned in repo).
- **Compilation executed: NO.** Workflow only statically checked (YAML parses). Not run in Actions yet.

### Confirmed from source in M1A
- Java loads `openxr_loader` (try block) then `rtcw_client` (`GLES3JNIActivity.java:56,60`). The manifest `android.app.lib_name=rtcwquest` is NativeActivity metadata; the activity is a plain `Activity`, so it appears unused (resolves the M0 question; runtime effect not tested).
- Shared libs expected in APK `lib/arm64-v8a/`: `librtcw_client`, `libgl4es`, `libqagame`, `libcgame`, `libui`, `libopenxr_loader`, `libc++_shared` (all modules are `BUILD_SHARED_LIBRARY`; STL c++_shared).
- `gradlew` is committed as mode 644; CI runs `chmod +x`.
- `local.properties` is tracked (Windows path); CI overwrites it in the workspace only.
- `z_vr_assets.pk3` is generated (not tracked) by `packResources`; the validator checks for `assets/z_vr_assets.pk3`.

### Unverified risks
`APP_ALLOW_MISSING_DEPS=true`, `USE_ASAN=1` (no .mk uses it), support-compat 28.0.0, debug keystore, Quest-only `supportedDevices`. First real build may reveal more.

### Next session
Owner merges PR and runs workflow (Actions > Android Debug APK > Run workflow). Then M1B: read logs, fix only compile/packaging errors, update this file.

---

## Milestone 1B — build verification (observed on GitHub, 2026-10)
- [Run #1](https://github.com/IvanChen-ctrl/RTCWQuest/actions/runs/39833618907) (2026-10-22) and
  [run #2](https://github.com/IvanChen-ctrl/RTCWQuest/actions/runs/40127569323) (2026-10-24): workflow `Android Debug APK`,
  `workflow_dispatch` on `master` @ `2500dd2`, both **conclusion: success**. VERIFIED (read from the Actions API).
- Which of the two runs produced the APK that was installed on the headset is **not verified**. Artifacts expire
  after 2 days. M2A's test plan records the SHA-256 so future tests are traceable.

## Milestone 2A — controller button diagnosis and repair

### Confirmed observations (first real PICO 4 test, Windows Feedback Bridge v0.2)
- Original PICO 4 (A8110, Android 10, firmware 5.13.7), RTCWQuest 1.5.0 (versionCode 53), debug APK from Actions.
- App runs, renders the RTCW main menu, no crash in the ~10 s capture.
- Right controller moves the menu cursor; hovering menu text triggers haptics.
- Tested physical buttons on both controllers do not activate or confirm any menu item.
- Existing logs contain `Menu_HandleMouseMove` / `Item_SetFocus` but no explicit button-action logging, so absence of
  button logs is NOT proof that no events were generated. Android screenshots were black (video was fine).

### Traced in source (VERIFIED by reading code; not run on hardware)
Path: OpenXR profile -> `xrSuggestInteractionProfileBindings` -> `xrSyncActions` -> `xrGetActionState*` ->
`ovrInputStateTrackedRemote.Buttons` (`OpenXrInput.c`) -> `handleTrackedControllerButton` / `VrInputDefault.c` menu
branch -> `Sys_QueEvent(SE_KEY, K_MOUSE1)` -> `CL_KeyEvent` -> `UI_KEY_EVENT` -> `Menu_HandleKey`.
- Cursor and haptics only need the pose and haptic bindings. Button input needs the trigger/face-button bindings.
  So "pose + haptics work, buttons do not" is consistent with the pose/haptic bindings being accepted while the
  button bindings are not the ones the runtime used.
- The menu click logic itself is plausible: with default settings the right hand drives the cursor and
  `domButton1` (A) or the trigger produces `K_MOUSE1`; the engine path after `Sys_QueEvent` is stock Quake 3.

### Defects found in source
1. **`XR_BD_controller_interaction` was never enabled** (`TBXR_Common.c`: only the GLES and Android create-instance
   extensions). The PICO 4 profile `/interaction_profiles/bytedance/pico4_controller` is defined by that extension, so
   suggesting bindings for it without the extension is rejected by the runtime. The `XrResult` of every
   `xrSuggestInteractionProfileBindings` call was discarded, so this failure was silent.
2. **`khr/simple_controller` fallback was unusable.** Its only button (`select/click`) was bound to
   `triggerTouchAction`, which no code reads. A runtime that falls back to that profile therefore produced no usable
   button at all while still giving poses and haptics — exactly the reported symptom.

### Hypothesis (NOT confirmed)
Because defect 1 rejects the PICO 4 profile, the PICO runtime most likely bound a fallback profile (possibly
`khr/simple_controller`), whose select click was unreachable (defect 2). This fits the symptoms but the bound profile
was never logged. The pre-existing line `VR controllers found: <path>` (logcat tag `RTCW`, printed by
`TBXR_CheckControllers`) may already be in the captured logcat and would confirm or refute this: grep for it.

### What M2A changed (all additive; Quest paths unchanged)
- `TBXR_Common.c`: query instance extensions and enable `XR_BD_controller_interaction` only if the runtime advertises
  it (Quest does not). Required extensions are unchanged.
- `OpenXrInput.c`:
  - New `selectAction` bound to `khr/simple_controller` select/click (replaces the unread `triggerTouchAction` binding);
    folded into the trigger by `VrInput_EffectiveTrigger`.
  - Every `xrSuggestInteractionProfileBindings` result is now logged; if the full PICO 4 set is rejected it retries once
    without the optional bindings (`squeeze/click`, `thumbstick/touch`).
  - Button bitmask construction moved into the pure, unit-tested `VrInputLogic.h` (same bit values, same thresholds,
    compile-time `_Static_assert` against `xrButton_*`); thumbstick and per-hand reading unified in `TBXR_ReadHand`.
  - Logs the bound profile per hand, `xrSyncActions` / `xrAttachSessionActionSets` results and action errors.
- `VrInputCommon.c` / `VrInputDefault.c`: `handleTrackedControllerButton` uses the shared edge function; the two
  `K_MOUSE1` calls per hand are now `handleMenuClick` (same events, same order). Menu hand choice uses
  `VrInput_MenuUsesOffhand` (same condition). Both log generated key events.
- `cl_keys.c`: one-line hook at the top of `CL_KeyEvent` reporting delivery of MOUSE1/ESCAPE/SPACE.
- New: `VrInputLogic.h`, `VrInputDiag.c/.h`, `rtcw/Android_client.mk` entry, `tests/vr_input`,
  `.github/workflows/input-host-tests.yml`, `docs/INPUT_DIAGNOSTICS.md`, `docs/PICO4_INPUT_TEST_PLAN.md`.
- Diagnostics: logcat tag `RTCWInput`, state changes only, rate limited, cvar `vr_input_diag` (default 1, 0 = off).
  Format and events: `docs/INPUT_DIAGNOSTICS.md`.

### What was actually tested
- VERIFIED (host): `make -C tests/vr_input test` — 98 checks, 0 failures (button edges, thresholds, per-hand masks,
  select-as-trigger, menu hand selection, MOUSE1 press/release generation, diagnostics change-detection, rate limiting,
  line format). Mutation check: deliberately breaking the select fold, the threshold and the menu-hand condition
  each made the tests fail.
- VERIFIED (static): the edited C files were syntax-checked with host clang against stub Android/GL headers; no errors
  in the edited code. This is NOT the NDK build.
- Android build of this branch: see the pull request (CI run link). Build success is not evidence of PICO 4 behaviour.
- **NOT tested:** anything on a PICO 4 or Quest. The controller problem is **not** known to be fixed.

### Next hardware verification
Follow `docs/PICO4_INPUT_TEST_PLAN.md`: build via the existing workflow, install, run the 3-minute procedure while
capturing `adb logcat -s RTCWInput:I RTCW:I`, fill in the evidence table, and send the log back. If clicks still fail,
the first missing event in the trace identifies the broken link (`docs/INPUT_DIAGNOSTICS.md`).

### Open questions
- Which profile did the runtime actually bind in the first test (`VR controllers found:` line)?
- Do right-hand clicks via A/B/trigger and left Menu (Escape) all work once the PICO 4 profile is accepted?
- `TBXR_CheckControllers` maps unknown profiles (including `khr/simple_controller`) to Touch semantics; harmless for
  buttons now, left unchanged.

---

## Milestone 3 — B-button jump, God Mode, AI mercenary companion

Branch `pico4-m3-gameplay-mercenary-mvp`, from `master` @ `ceca712`. Full description, controls, limits and the headset
checklist: `docs/M3_GAMEPLAY_MERCENARY.md`. **Nothing is verified on a headset.**

### What exists in code
- **Jump:** `VrInput_JumpStep` (`VrInputLogic.h`) replaces the raw K_SPACE event in `VrInputDefault.c`. Normal gameplay now
  sends `+moveup`/`-moveup` (no dependency on a SPACE binding); camera sequences keep K_SPACE (stock skip); mounted gun
  keeps `+activate`; the release always matches the press and is sent even if a menu/backpack zone becomes active.
  The reason B did nothing on the headset is **not proven** (missing `SPACE -> +moveup` binding vs. dropped release; both
  fixed). New log line `ev=jump route=... down=...`.
- **God Mode:** cvar `vr_godmode` (default 0, not archived); toggle on the notebook "Private bucket list" page
  (`z_vr_assets/ui/cheats.menu`) and console `vr_godmode 0|1`. `G_Damage` checks `G_VrGodProtects()` next to `FL_GODMODE`
  (`g_vr.c`, `g_combat.c`); single-player human only; `DAMAGE_NO_PROTECTION` and tramcar crush still kill.
- **Mercenary:** `g_merc.c` + `g_vrlogic.h`. Stock AICast character (American, Partisan fallback) spawned through
  `AIChar_spawn`; follow = stock leader logic (`cs->leaderNum`), hold = stock wait goal, combat = stock AI. Supervisor
  state machine FOLLOW/HOLD/ENGAGE/RETURN/DEAD/DISMISSED, validated spawn, duplicate prevention, bounded RETURN with
  give-up + cooldown (no teleport), friendly-fire guard in `G_Damage`. Console `merc hire|follow|hold|assist|type|dismiss|status`
  (also notebook buttons). Use-key on the mercenary toggles follow/hold.
- **Save/load:** the companion (and a pending spawn) is skipped by `G_SaveGame` (`g_save.c`) because `G_LoadGame` aborts on
  a saved client the new level did not create. The wish is the cvar `vr_merc`; level change / load re-hires a new one.

### Tests (host, no SDK)
`make -C tests/vr_input test` -> 139 checks, 0 failures (98 from M2A + 41). `make -C tests/vr_gameplay test` -> 544 checks,
0 failures. Mutation checks (12 + 3) each make a test fail. Both run in `.github/workflows/input-host-tests.yml`.
Edited game files were syntax-checked with host gcc against the real headers (`-include stddef.h -include stdint.h
-DGAMEDLL`); the Actions Android build is the real compile check (see the pull request for the run).

### M3 hotfixes (reported from the PICO 4)
1. `Failed to load animation scripts for model american` (fatal) -> preflight before spawn (`a8d5a1a`).
2. `NO ALLIED SOLDIER MODEL FOUND` (both American and Partisan rejected). Root cause not determinable without the owner's files; found in
   source that my preflight was stricter than the loader (hard-coded `default` skin names, ignored the `default.script` rule). Now: skin names
   discovered via `trap_FS_GetFileList`, script rule = the loader's, script must define AI states, candidates are (behaviour, visual) pairs
   (american, partisan, infantryss, eliteguard; others only via `merc type <model>`), exact missing paths logged (`VR mercenary: model ...
   MISSING: ...`), `merc models` full report, refusal if nothing qualifies. Offline: `tools/inventory_rtcw_models.py` (tested on synthetic pk3)
   and `tools/Inventory-RtcwModels.ps1` (port, **not executed**). Tests: `tests/vr_gameplay` 544 checks.

### M3 hotfix 2B: sleep/resume (owner report: black screen / freeze / launcher after standby)
Root cause not proven. Source defects found and fixed: asserts in the session-state handler (abort), LOSS_PENDING/EXITING/instance loss ignored,
busy-wait loops, unbounded `xrWaitSwapchainImage`, ignored frame-call results, `destroyed` never set + a second engine thread on relaunch,
JNI global-ref leak, wake lock held while paused, `exit(1)` on session creation failure, local-space leak. New `VrLifecycleLogic.h` (pure, 94 host checks,
12 mutations killed), logging tag `RTCWLifecycle`, hardware plan in `docs/M3_GAMEPLAY_MERCENARY.md` section 3b. **Unverified on hardware.**
Removed: the `OpenXRTestWakeLock` (kept `FLAG_KEEP_SCREEN_ON`).

### Not done / unknown
- Whether the Partisan model is complete on the owner's install (probed at runtime; see `merc models` / logcat).
- Whether the notebook corner button that opens the "Private bucket list" page is reachable in VR (`visible 2`, unverified).
- Whether the use key on the mercenary reaches the replaced activate hook (assumed from `AICast_Activate`).
- No FPS or `gm:` measurements; method in the M3 doc (`com_speeds 1`).
- One companion only; not persisted; no recovery teleport; HOLD does not leash an engaged chase.
- Multiplayer/LAN co-op not started (out of scope).

### Next session
Owner builds the APK (Actions -> Android Debug APK -> this branch), runs the checklist in the M3 doc and returns the
logcat (`adb logcat -s RTCWInput:I RTCW:I`). If B still does not jump, compare `ev=btn name=B` with `ev=jump`. If the
mercenary does not appear, `merc status` and the `VR AI spawn ...` / `VR mercenary: ...` lines in the `RTCW` tag show where.

---

## Milestone 4 — tactical mercenary AI, M5 LAN co-op research

Branch `pico4-m4-tactical-ai`, **stacked on** `pico4-m3-gameplay-mercenary-mvp` (M3 PR #5 still open; the M4 PR targets the
M3 branch, not `master`). Full description, state table, commands, limits and the 20-point headset checklist:
`docs/M4_TACTICAL_AI.md`. M5 research: `docs/M5_PICO4_LAN_COOP_FEASIBILITY.md`. **Nothing is verified on a headset.**

### What exists in code
- `g_vrtactics.h` (new, pure): command validation, friend/foe classification, threat score, target selection with
  commitment + hysteresis, damage windows, friendly-fire line test, formation slots, cover / flank / regroup entry and
  exit rules, reason codes.
- `g_merc.c`: tactical context from the companion's own sight memory (250 ms), target applied to the stock AI,
  hold-fire when the player/an ally is in the line of fire, formation correction, designation by fire press
  (`merc attack`), cover (stock hide pos + local samples, executed by `AIFunc_BattleTakeCoverStart`), bounded flank
  (goal entity + stock chase), regroup/retreat (stock chase of the player), stances, `RTCWTactical` diagnostics.
- `g_vrlogic.h`: `VRMERC_COVER/FLANK/REGROUP` appended (M3 numbering unchanged); the M3 machine leaves them alone.
- UI: `z_vr_assets/ui/mercenary.menu` (10 buttons), link on the cheats page, registered in `ui/ingame.txt`.
- Console: `merc attack [now] | cover | regroup | stance [defensive|aggressive]` added to the M3 commands.
  Cvars `vr_merc_stance`, `vr_merc_diag`.
- Savegames: the companion's temporary goal entities are now skipped like the companion.

### Tests
`make -C tests/vr_tactics test` -> 2625 checks, 0 failures (new). `vr_gameplay` 544, `vr_input` 139, `vr_lifecycle` 94, unchanged
and passing. 40 deliberate mutations of the pure logic all fail a test. All four run in
`.github/workflows/input-host-tests.yml`. `g_merc.c`, `g_combat.c`, `g_main.c`, `g_cmds.c`, `g_save.c` pass `gcc -fsyntax-only`
against the real engine headers (zero warnings in `g_merc.c`). The Android build result is in the pull request.

### Not done / unknown
- No engine run of any M4 behaviour; all thresholds are first guesses. The stock AI interaction (calling
  `AIFunc_BattleTakeCoverStart` / `AIFunc_ChaseGoalStart` from the supervisor) is the same pattern the stock use-order uses
  but is unproven for these states.
- Companion still not persisted; one companion; no splash-weapon friendly-fire handling; no reload-under-pressure trigger.
- M5: research only, no multiplayer code. Key finding: the VR pose reaches the game module through a process-local global
  (`gVR`, 100 uses), not through the protocol, so co-op needs a per-client pose path first. Recommended prototype host:
  a PC dedicated server (Option B).

### Next session
Owner builds the APK from the M4 branch, runs the checklist (M4 doc section 12) and returns `adb logcat -s RTCWTactical:I RTCW:I`.
If a behaviour is wrong, the `state`/`cover`/`flank`/`nav_failure` lines show which decision fired. Then: tune thresholds,
or start M5.0 (host build of the server) if the owner approves.

---

## M4 hardware-feedback round — crouch, following, squad (same branch, PR #6)

Owner's first PICO 4 test of M4: mercenary spawns, God Mode works; A did not crouch, only one mercenary, too slow, got lost,
tactics weak. Details, causes, controls, diagnostics and the 21-point hardware plan: **`docs/M4_SQUAD_AND_MOVEMENT.md`**.
**Everything is source-level and host-tested only; none of it is confirmed on the headset.**

- **Crouch (read from source):** `vr_irl_crouch_enabled` defaults to 1 and `PM_CheckDuck`'s IRL branch returned before reading
  `upmove < 0`, so `+movedown` did nothing; the press/release were also both gated by the backpack zone. Fixed with
  `VrCrouchLogic.h` (shared by game/cgame) + `VrInput_DuckStep` (always-released latch) + `ev=crouch` in `RTCWInput`.
- **Slow / lost (read from source):** stock AICast caps speed at speedScale 1.2 x run-animation speed (about 73-88% of the
  player), and the leader chase stops at < 240 straight-line units without line of sight. Fixed (companion only) with
  `g_vrmove.h`: path-distance NEAR/RUN/CATCHUP with hysteresis, a speed-floor hook in `ai_cast_think.c`, a bounded recovery
  ladder (repath -> player's trail -> wait). No teleport. `vr_merc_move 0` restores the M3 behaviour.
- **Squad:** `squad[4]` independent soldier records (unique AI names `vr_merc_1..4`), `vr_merc_squad` limit (default 2),
  one recruit at a time, per-soldier/`all` addressing and a notebook selector, roles + single file in corridors, spread of
  fire, wounded/no-ammo cover. `g_vrsquad.h` is the pure part.
- **Tests:** `tests/vr_input` 53608, `vr_gameplay` 544, `vr_lifecycle` 94, `vr_tactics` 3042, `vr_squad` 653, all passing.
  Mutation checks on the new pure logic caught all but equivalent mutants.
- **Unknown until the headset test:** whether A now crouches, whether the speed floor looks right (foot sliding?), whether
  CATCHUP/recovery keeps soldiers with the player through doors, how three soldiers perform, and the frame times.
- **Not done:** teleport recovery (deliberately), companion persistence, per-soldier stance, splash-weapon friendly fire.

Next session: owner builds the APK from the M4 branch, runs the plan, returns `adb logcat -s RTCWInput:I RTCWMerc:I
RTCWTactical:I RTCWLifecycle:I RTCW:I` plus the frame-time table.

