# HANDOFF

## Status (updated in M2B — read "Milestone 2B" at the bottom first)
M0 (audit/docs), M1A (CI workflow) and M2A (controller input, PR #3) are merged to `master` (latest `ceca712`).
Physical PICO 4 test of the M2A build (owner, 2026-10-24/25): the app runs, the menu renders, the right controller moves
the cursor with hover haptics, **trigger clicks now work**, menu items can be selected, the opening cinematic plays and
**finishes naturally into the first-person scene**. Open problems: skipping the cinematic can end in
`Error: Client command overflow`, and the menu pointer is intermittently misaligned. M2B (branch
`pico4-m2b-cinematic-menu`) addresses both; it is NOT verified on hardware. The M0 and M1A sections below are history
and partly stale.

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

## Milestone 2B — cinematic skip overflow, menu pointer, input transitions

Full write-up: `docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md` (read it; this section is a summary).

### Confirmed on hardware before M2B (owner, PICO 4 A8110, Android 10, firmware 5.13.7, RTCWQuest 1.5.0 / 53)
- M2A is merged (PR #3). Menu pointing, hover haptics, controller clicking, menu selection work.
- The opening cinematic runs; letting it finish enters the first-person scene (recordings 05.55.27 and 05.58.48).
- **Reproduced:** `Error: Client command overflow` at about 16 s in recording 06.06.44 after trying to skip. The log
  capture supplied (Feedback Bridge v0.3, 21:54 UTC) predates the error, so the internal state at the failure is unknown.
- **Intermittent:** the menu pointer sometimes does not line up with the menu; restarting restores it.

### Findings (source-level; details and evidence in the M2B document)
- The error string exists only in `CL_AddReliableCommand` (`cl_main.c`): more than 256 unacknowledged reliable commands.
- During a camera sequence cgame queues one `setCameraOrigin` per frame. If the game is paused (`cl_paused && sv_paused`)
  nothing is sent or acknowledged, so the queue overflows after about 256 frames (~3 s).
- The VR layer is in its gameplay branch during cutscenes and its left-Y `notebook` command reaches `IN_Notebook`, which
  pauses the game with no camera check (stock RTCW cannot, because `CL_KeyEvent` eats keys in camera mode).
  **This mechanism is source-proven; that it is what happened in the recording is ASSUMED.**
- Pointer: the cursor was computed against a static `menuYaw` (0 until the first non-menu frame) while the panel is placed
  at `vr.hmdorientation_snap[YAW]`. Pointing at the panel centre therefore gives a wrong cursor whenever the launch yaw is
  not about 0. Source-proven defect; hardware effect ASSUMED.
- Also seen: pulling the *other* controller's trigger in the menu intentionally switches the cursor hand (not a bug,
  restart resets it); HMD pose could be frozen in as the menu direction while invalid; recenter never re-placed the menu;
  a gameplay action pressed before a mode switch could stay held.

### What M2B changed (small, reversible; Quest paths unchanged)
- `cl_input.c`: `IN_Notebook`/`IN_Help` refuse while `cl.cameraMode`.
- `cl_main.c` / `cl_cgame.c`: cgame's `trap_SendClientCommand` goes through `CL_AddCgameReliableCommand`, which does not
  queue the superseded per-frame `setCameraOrigin` while paused or when 64+ commands are unacknowledged. Other commands and
  the overflow error are unchanged. `MAX_RELIABLE_COMMANDS` unchanged.
- `cl_keys.c`: `cameraInterrupt` (skip) is queued once until acknowledged (`VrCin_CameraKey`).
- Pointer: both input handlers use `vr.hmdorientation_snap[YAW]` as the reference; snap decision moved to
  `VrPointerLogic.h` (no snap from an unusable pose, re-snap on recenter and when the pose returns); controller/HMD
  non-unit quaternions are held/ignored; non-finite angles never reach the cursor. Mapping formula unchanged.
- Input transitions: the VR layer remembers the `+actions` and keys it holds and releases only those when the screen
  layer mode changes (`VrTransitionLogic.h`, `VrInput_ModeTransition`).
- New files: `VrCinematicLogic.h`, `VrPointerLogic.h`, `VrTransitionLogic.h`, `VrM2bDiag.c/.h`, `tests/vr_m2b/`,
  `docs/M2B_CINEMATIC_POINTER_DIAGNOSTICS.md`; `Android_client.mk` lists `VrM2bDiag.c`; `input-host-tests.yml` runs
  the new host tests.
- Diagnostics: logcat tags `RTCWCinematic` (cvar `vr_cin_diag`) and `RTCWPointer` (cvar `vr_pointer_diag`), same line
  format as `RTCWInput`; `ev=overflow` is always logged with the backlog by command **name**.

### Tests performed
- VERIFIED (host): `make -C tests/vr_m2b test` — 1449 checks, 0 failures; it also runs `tests/vr_input` (98 checks, 0
  failures). Mutation checks (disabling each guard/snap rule) fail the tests. The old behaviour (overflow ~257 frames after
  a pause during a camera) is reproduced by the model and absent with the guard.
- VERIFIED (static): the edited engine and VR files pass `gcc -fsyntax-only` with stub Android/EGL/JNI headers (not the
  NDK build).
- Android APK build of this branch: see the pull request (workflow run link and result). Build success is not evidence of
  PICO 4 behaviour.
- **NOT tested:** anything on a PICO 4 or Quest. Neither defect is known to be fixed.

### Remaining hardware verification
Follow section 6 of the M2B document (5 cold starts, hand switch, natural completion, skip by Menu and B, hold B, Y during
the cutscene, rapid presses, fire-then-menu, gameplay regression, recenter) with the capture
`adb logcat -s RTCWCinematic:I RTCWPointer:I RTCWInput:I RTCW:I` and record date, build/commit and APK SHA-256 in `docs/`.

### Open questions
- Which button was pressed when the overflow occurred? (`ev=overflow kinds= paused=` and `ev=interrupt`/`pause_refused`.)
- Does the PICO runtime send `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` on recenter (`ev=recenter`)?
- Are `XR_SPACE_LOCATION_ORIENTATION_VALID_BIT` flags reliable on the PICO 4 (`ev=pose who=... flags=`)?
- Is the remaining pointer error, if any, the angular mapping (not changed) rather than the reference or hand switch?
