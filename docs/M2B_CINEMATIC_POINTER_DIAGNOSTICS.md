# M2B: cinematic skip overflow, menu pointer alignment, input transitions

Milestone M2B, branch `pico4-m2b-cinematic-menu` (from `master` @ `ceca712`, which contains the merged M2A / PR #3).
Status of every statement below is marked **CONFIRMED** (read in this repository's source, or reproduced by the host
tests), **ASSUMED** (reasoned, not observed), or **NOT VERIFIED ON HARDWARE**. Nothing here has been run on a PICO 4
yet; the owner's physical test is the acceptance step.

## 1. P0: `Client command overflow`

### 1.1 Where the error comes from (CONFIRMED)

`CL_AddReliableCommand` (`rtcw/src/client/cl_main.c`) raises `Com_Error( ERR_DROP, "Client command overflow" )` when
`clc.reliableSequence - clc.reliableAcknowledge > MAX_RELIABLE_COMMANDS` (256, `qcommon.h`). That is the only place the
string exists. `reliableAcknowledge` is only updated when a server message arrives (`CL_ParseServerMessage`,
`cl_parse.c`). So an overflow needs **more than 256 unacknowledged commands**, i.e. a long period without server
messages while commands keep being queued.

### 1.2 Who queues commands every frame (CONFIRMED)

During a camera sequence `CG_CalcViewValues` (`cgame/cg_view.c`, `cg.cameraMode`) calls
`trap_SendClientCommand( "setCameraOrigin x y z" )` on **every rendered frame**. This goes through
`CG_SENDCLIENTCOMMAND` -> `CL_AddReliableCommand`. It is the only per-frame reliable command in the cutscene path.
Normal playback stays harmless: snapshots arrive at the server rate (`sv_fps`, 20 by default in stock RTCW) and
acknowledge the queue, so only a handful of entries are outstanding (ASSUMED from the rates; the new `cam_hb` log
measures it).

### 1.3 What stops the acknowledgments (CONFIRMED in source)

When `cl_paused && sv_paused` (single player pause):

* `CL_SendCmd` (`cl_input.c`) returns before sending any packet;
* `SV_Frame` (`server/sv_main.c`, `SV_CheckPaused`) returns before sending snapshots;
* `CL_SetCGameTime` (`cl_cgame.c`) stops advancing time but cgame still renders (`SCR_DrawScreenField` calls
  `CL_CGameRendering` for `CA_ACTIVE` unless a *full-screen* menu is up; `z_vr_assets/ui/notebook.menu` has
  `fullscreen 0`), so `setCameraOrigin` is still queued once per frame.

Nothing is acknowledged, so the queue grows by one per frame: **256 frames, about 2.8 s at 90 fps** (3.6 s at 72 fps),
then the error. The existing comment in `CL_CheckUserinfo` ("don't overflow the reliable command buffer when paused")
shows the engine already knows this failure class.

### 1.4 What pauses the game during a cutscene (CONFIRMED in source)

* `UI_SET_ACTIVE_MENU( UIMENU_NOTEBOOK )` sets `cl_paused 1` (`ui/ui_main.c`).
* The VR layer reaches it with the console command `notebook` (`VrInputDefault.c`, left Y with the default scheme),
  handled by `IN_Notebook` (`cl_input.c`). `IN_Notebook` only checked `cls.state == CA_ACTIVE`.
* Stock RTCW cannot do this during a camera: `CL_KeyEvent` eats every key while `cl.cameraMode` ("pausing not allowed in
  camera mode"). The VR layer bypasses `CL_KeyEvent` by issuing the console command directly.
* During a cutscene the VR layer is in its gameplay branch, not the menu branch: `VR_CalculateScreenLayer`
  (`RTCWVR_SurfaceView.c`) is false for `CA_ACTIVE` without a menu, so left X/Y, grips, sticks etc. are live while the
  cutscene plays. A player looking for "the skip button" can therefore press Y (notebook) instead of Menu/B.

### 1.5 What is NOT proven

* Which button the owner actually pressed in `ScreenRecording_2026.10.25-06.06.44.mp4`. The recording shows the error at
  about 16 s; it cannot show the pause. The `notebook` path is the only **source-verified** way to pause during a camera
  that the controller reaches; other unknown pause paths are possible.
* The hypotheses from the task list were checked as follows:

| # | Hypothesis | Result |
|---|---|---|
| 1 | Repeated skip commands from a held button | **Not supported.** `handleTrackedControllerButton`, `handleMenuClick` and the B-button code all generate events on press/release **edges** only (`VrInput_ButtonEdge`, host-tested: a held button yields 0 events/frame). The VR layer produces no key auto-repeat. A repeat stream could only come from a keyboard; it is bounded anyway now (see 1.6). |
| 2 | Several press edges from one physical press | Not supported by the edge logic. The M2A capture shows one `down`/`up` pair per pull. |
| 3 | Input forwarded during loading | Not supported. The M2A capture shows an extra `MOUSE1 up` delivered at map start without a controller event (`catchers=0x0 route=game`); it comes from the UI calling `Key_ClearStates` (`UI_KEY_CLEARSTATES`, `cl_ui.c`), which only emits releases. `cameraInterrupt` requires `cl.cameraMode`, which is cleared by `CL_ClearState` when the new gamestate arrives (`cl_parse.c`). |
| 4 | Commands queued faster than processed | **Yes, in the paused state** (1.3): one per frame vs zero acknowledgments. Not the case in normal play. |
| 5 | Commands not acknowledged during a transition | **Yes while paused** (1.3). Map loading blocks the whole frame loop, so it queues nothing. |
| 6 | Completion handler entered several times | `CG_STOPCAMERA` and `CG_StopCamera` are idempotent; duplicate `stopCamera` is harmless. Not a source of volume. |
| 7 | Stale controller state across states | Real but separate: gameplay actions can be left held across a mode switch. Fixed in 2.5; not the overflow cause. |
| 8 | Another defect | The pause path above. |

### 1.6 Corrections (what was implemented)

| Layer | Change | Why |
|---|---|---|
| Root cause | `IN_Notebook` / `IN_Help` do nothing while `cl.cameraMode` (`VrCin_PauseMenuAllowed`). | Restores stock behaviour: no pausing menu inside a camera sequence. |
| Defence in depth | `CL_AddCgameReliableCommand` (cgame `trap_SendClientCommand`) does not queue `setCameraOrigin` while the client is paused or the backlog is >= 64 (`VRCIN_SUPERSEDE_HIGH_WATER`). | That command only carries the newest value; the next frame sends a newer one. Any other pause path can no longer fill the queue with it. |
| Skip request | `CL_KeyEvent` queues `cameraInterrupt` only if the previous one has been acknowledged (`VrCin_CameraKey` + `vrCinGate_t`). The gate resets on `CG_STARTCAMERA`/`CG_STOPCAMERA`. | One deliberate skip = one request, however many presses/repeats arrive before the server acknowledges. |
| Diagnostics | `RTCWCinematic` log, section 3. | Next failure identifies the real cause. |

Not changed: `MAX_RELIABLE_COMMANDS`, the error text and its `ERR_DROP`, the cinematic content, any timing. All other
reliable commands are queued exactly as before; a stall with any other command stream still overflows (host test
`testStallWithoutPauseStillErrors`).

`setCameraOrigin` comment in `cg_view.c` says it must be sent every frame so a loadgame into the same camera position
still shows the right entities. That holds: dropped commands are only those issued while paused or backlogged, and the
first frame after the backlog clears sends one again.

## 2. P1: menu pointer, transitions

### 2.1 Pointer pipeline (CONFIRMED, as read)

```
OpenXR aim pose (StageSpace, OpenXrInput.c)
  -> QuatToYawPitchRoll -> vr.dominanthandangles / vr.offhandangles   (VrInputDefault.c)
  -> interactWithTouchScreen(menuYaw, angles)                         (VrInputCommon.c)
       u = -sin(ctrlYaw - menuYaw) + 0.5     v = (ctrlPitch - 15) / 90 + 0.5   (unchanged mapping)
  -> PortableMouseAbs(u, v): absx = u*640, absy = v*480               (in_android.c)
  -> UI_MOUSE_EVENT_ABS -> _UI_MouseEventAbs -> uiInfo.uiDC.cursorx/y (ui_main.c)
  -> the same cursorx/y is drawn AND used by Display_MouseMove for hit testing
```

The menu panel is an `XrCompositionLayerQuad` in `StageSpace` (`TBXR_submitFrame`): centre = current head position plus
`VR_GetScreenLayerDistance()` along `vr.hmdorientation_snap[YAW]`, facing that yaw, 6.0 x 5.5 m.

The failure modes from the task:

* **C (pointer correct but hit test displaced): excluded by construction.** The drawn cursor and the hit test read the same
  `uiDC.cursorx/y`.
* **A / B (panel or rendered pointer wrong):** the cursor is drawn by the UI at the 640x480 position above, so B reduces
  to "the position computed from the controller direction is wrong".
* **E (scaling inconsistent): not a source of intermittency.** The mapping is the same constant formula every frame; it is
  an angular approximation, not a geometric ray/plane intersection, so a constant calibration error always exists. It is
  deliberately left unchanged (the owner reports correct pointing when initialisation succeeds).
* **D (stale or wrong reference):** found, see 2.2.

### 2.2 Defect found: two different reference yaws (CONFIRMED in source)

* The panel is placed using `vr.hmdorientation_snap[YAW]`, captured by `VR_SetHMDPosition`.
* The cursor was computed against a different variable: `static float menuYaw = 0` in `HandleInput_Default`, assigned
  **only in the non-menu branch** (`menuYaw = vr.hmdorientation[YAW]`).

When the app starts directly in the menu (always) the non-menu branch has never run, so `menuYaw` stays 0 while the panel
faces whatever yaw the headset had after startup in `StageSpace`. Pointing at the middle of the panel then gives
`u = 0.5 - sin(yaw)`; for a 25 degree start yaw that is 0.08 instead of 0.5 (host test
`testReferenceYawConsistency`). The error depends on how the user faces at launch, so it is intermittent, and it
disappears after a restart if the next start happens to be near 0 degrees, or after one pass through the game (the
static is then refreshed). This matches the report. **It is the leading, source-verified explanation; it is NOT confirmed
on hardware.**

Fix: both input handlers (`VrInputDefault.c`, `VrInputWeaponAlign.c`) now pass `vr.hmdorientation_snap[YAW]`, the exact
value the panel uses.

### 2.3 Further robustness changes (CONFIRMED defects in the code path, effect on hardware ASSUMED)

* **Snap from an unusable HMD pose** (`TBXR_GetHMDOrientation`). `loc.pose` was applied without checking it; an unfilled
  `XrSpaceLocation` holds an all-zero quaternion, which yields yaw 0. The original code re-snapped only during the first
  100 `VR_SetHMDPosition` calls or on a mode toggle, so a pose that became valid after frame 100 left the panel frozen at
  yaw 0. Now the snap decision (`VrSnap_Update`, `VrPointerLogic.h`) never snaps from an unusable pose and restarts the
  100-frame warm-up when the pose becomes usable. A pose is unusable if its quaternion is not unit length/finite, or the
  runtime did not set `XR_SPACE_LOCATION_ORIENTATION_VALID_BIT` (after 300 frames of "unit quaternion but flag missing"
  the data is trusted anyway, in case a runtime does not set the flag).
* **Recenter.** `TBXR_Recenter` recreates the reference spaces on `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` but
  never re-snapped the panel. It now requests a snap. `VR_SetHMDOrientation` is called before `VR_SetHMDPosition` so the
  snap uses the current frame's yaw (relevant right after a recenter, when the yaw jumps).
* **Controller pose.** A non-unit/zero/non-finite controller quaternion is replaced by the last usable orientation
  (`VrPointer_HoldPose`); non-finite angles never reach `PortableMouseAbs`.
* `hmdposition_snap` / `hmdorientation_snap` are only read by the panel placement and the cursor (grep in the repository);
  the extra snaps do not touch gameplay.

### 2.4 By-design behaviour that can look like misalignment (CONFIRMED, not changed)

Pulling the trigger of the **other** controller switches the menu cursor to that hand (`vr.menu_right_handed`, initialised
`qtrue` in `RTCWVR_InitOnce`, not persisted). The M2A capture shows exactly this (`ev=menuhand cursor=L`, then
`cursor=R`). If the owner tried both triggers during a session and then pointed with the original hand, the cursor would
follow the other controller, and a restart restores the default. `RTCWPointer ev=hand` now logs the active hand so the two
causes can be told apart.

### 2.5 Input state across mode changes (P1, section 5 of the task)

Gameplay actions (`+attack`, `+kick`, ...) and keys (`K_SPACE`) are released by a later button edge. If the layer switches
between the press and the release (gameplay <-> menu/loading), the other branch handles the release and the action stays
down. Now: `sendButtonAction` and the key helper `VrInput_QueueKey` record what this layer holds
(`VrTransitionLogic.h`); `VrInput_ModeTransition` releases **only those** when `RTCWVR_useScreenLayer()` changes
(`-name` commands and key-up events). Nothing else is cleared: movement, the engine's own key state and held buttons the
layer did not start are untouched. The later physical release is a no-op. Logged as `RTCWInput ev=release`.

Not changed (listed so it is not mistaken for a fix): reaction to focus loss/resume and controller disconnection rely on
`xrSyncActions` reporting actions as inactive, which `TBXR_ReadHand` already turns into released buttons; no change was
made or tested.

## 3. Diagnostics

Same line format as `RTCWInput` (`v1 t=<ms> ev=<name> key=value ...`, `t` = engine millisecond clock, values contain no
spaces), so the Windows Feedback Bridge and `docs/INPUT_DIAGNOSTICS.md` parsers apply unchanged. Source:
`RTCWVR/VrM2bDiag.c/.h`.

| Tag | Cvar | Values |
|---|---|---|
| `RTCWCinematic` | `vr_cin_diag` | `1` (default) state changes, `0` off |
| `RTCWPointer` | `vr_pointer_diag` | `1` (default) changes + a sample every 2 s in menus, `2` a sample every 250 ms in menus, `0` off |

Enable/disable at runtime from the console: `vr_cin_diag 0`, `vr_pointer_diag 2` (not archived; every launch starts at 1).
Each tag has its own token bucket (burst 30, 10 lines/s); lost lines are reported as `ev=drop n=<count>`.
`ev=overflow` is always logged (priority Error, ignores the cvar and the limiter).

Collect: `adb logcat -s RTCWCinematic:I RTCWPointer:I RTCWInput:I RTCW:I` (`RTCWCinematic:E` is included in `:I`).

### RTCWCinematic events

| `ev` | Fields | When |
|---|---|---|
| `state` | `cls`, `cam`, `paused`, `catchers`, `map`, `seq`, `ack` | `cls.state`, camera mode, pause or key catcher changed (loading, cutscene, gameplay boundaries) |
| `cam_start` | `map`, `cls`, `seq`, `ack` | cgame started the primary camera |
| `cam_stop` | `map`, `interrupts`, `suppressed`, `superseded`, `max_outstanding`, `seq`, `ack` | camera ended; counters for this camera |
| `cam_hb` | `seq`, `ack`, `outstanding`, `paused`, `superseded_total` | every 2 s while a camera runs |
| `interrupt` | `key`, `result` (`sent`/`suppressed_unacked`), `seq`, `ack`, `outstanding`, `paused` | a skip key (ESC/SPACE/ENTER) was pressed in camera mode |
| `supersede_start` / `supersede_end` | `kind`, `paused`, `seq`, `ack`, `outstanding` / `dropped`, `why`, `total` | the new guard started/stopped dropping `setCameraOrigin` |
| `queue` | `outstanding`, `level`, `seq`, `ack`, `paused`, `cam`, `cls` | backlog crossed 32, 64, 128, 192, 240 (upward only) |
| `pause_refused` | `what`, `cam`, `cls` | `notebook`/`help` refused during a camera |
| `overflow` | `seq`, `ack`, `outstanding`, `cls`, `cam`, `paused`, `catchers`, `map`, `packet_age_ms`, `kinds` | immediately before the unchanged `Client command overflow` error. `kinds` = unacknowledged commands by **name only**, e.g. `setCameraOrigin:250,cameraInterrupt:3` (no arguments, no payloads) |

### RTCWPointer events

| `ev` | Fields | When |
|---|---|---|
| `snap` | `reason` (`pose_valid`, `screen_toggle`, `recenter`), `hmdYaw`, `quadYaw`, `pos`, `screen` | the menu placement was captured (warm-up frames are not logged) |
| `pose` | `who` (`HMD`/`L`/`R`), `usable`, `flags` (hex `XrSpaceLocationFlags`) | usability of a pose changed |
| `recenter` | `hmdYawBefore` | reference spaces were recreated |
| `hand` | `hand` | the hand driving the menu cursor changed |
| `ptr` | `ctrlYaw`, `ctrlPitch`, `refYaw`, `hmdYaw`, `u`, `v`, `ui` (`x,y` in 640x480) | rate-limited sample while the menu shows |
| `offaxis` | `state`, `hmdYaw`, `refYaw`, `dyaw` | the panel is more than 45 degrees away from where the user looks (state change) |
| `reject` | `reason`, angles | non-finite input was ignored (once per process) |

How to read it: `ptr` with `ctrlYaw` close to `refYaw` and `pitch` near 15 should give `u~0.5, v~0.5, ui~320,240`. If the
user reports the pointer is off while aiming at the panel centre, compare `refYaw` with `ctrlYaw` (reference problem),
look for `snap`/`pose usable=0` lines before it (initialisation), and for `ev=hand` (hand switch).

`RTCWInput ev=release reason=to_screen|to_game actions=<n> keys=<n>` reports section 2.5.

## 4. Tests

`make -C tests/vr_m2b test` (also runs `tests/vr_input`). Host-only, no Android tooling. The production headers
(`VrCinematicLogic.h`, `VrPointerLogic.h`, `VrTransitionLogic.h`, `VrInputLogic.h`) and `VrM2bDiag.c` are compiled
directly; the queue model in `test_cinematic.c` mirrors `CL_AddReliableCommand` / `CL_AddCgameReliableCommand` /
the camera block of `CL_KeyEvent` and calls the production decision functions.

Covered: natural completion, single skip, press-and-hold (300 repeated downs), rapid presses, input during/after the
transition, repeated completion notifications, queue boundary (257 allowed, 258 errors for non-droppable commands), the
paused-camera overflow reproduced **before** and absent **after** the guard, key release state, entering gameplay;
pointer initial calculation, reference-yaw consistency (regression for 2.2), non-finite input, viewport conversion
(identical truncation), quaternion/pose validity, invalid-to-valid and recenter and reopen snap decisions, diagnostics
rate limiting; M2A regression (PICO trigger, left/right independence, MOUSE1 press/release, hand selection) and the held
input release logic. Mutation checks (breaking each guard) make the corresponding tests fail.

Not testable without hardware: haptics on hover, OpenXR pose validity flags from the PICO runtime, real frame rates,
the actual button the owner pressed, the rendering of the quad, Quest behaviour (Quest paths were not modified; only the
shared engine logic above changed).

## 5. Known limitations

* The overflow root cause (pause via the notebook button) is source-proven as a mechanism but not proven to be what
  happened in the recording. The `overflow` log line removes the doubt next time.
* The pointer fix targets the reference-yaw defect; if misalignment persists after it, `RTCWPointer` lines decide between
  hand switch, tracking and mapping calibration (the mapping is an angular approximation and was not changed).
* The by-design hand switch (2.4) is untouched.
* Cutscene gameplay controls other than the notebook/help menus stay live (weapon switching etc. as before); only the
  paused menus are refused.
* The mode-transition release uses `RTCWVR_useScreenLayer()`; a menu opened without a layer change does not trigger it.

## 6. Physical PICO 4 regression steps

Build with the existing workflow (Actions -> Android Debug APK -> Run workflow, branch `pico4-m2b-cinematic-menu`), note
the commit and APK SHA-256, install with `adb install -r`, start the capture:

```
adb logcat -c
adb logcat -s RTCWCinematic:I RTCWPointer:I RTCWInput:I RTCW:I > m2b.log
```

1. Cold start 5 times. For each: point at three menu entries (top left, centre, bottom right); note whether the cursor
   sits where the controller points. Record result per start. Do not pull the left trigger unless testing the hand switch.
2. Pull the left trigger once, then point with the left controller (expected: cursor follows the left hand; `ev=hand hand=L`).
   Pull the right trigger (expected: back to the right hand).
3. Start a new game. Let the opening cutscene finish naturally. Expected: playable scene, no error.
4. Start again. Skip with one short press of left **Menu**, then (new start) with **B**. Expected: one skip, gameplay starts.
5. Start again. Hold **B** for 5 s during the cutscene. Expected: one skip, no error.
6. Start again. Press **Y** (notebook) during the cutscene. Expected: nothing opens; log shows `ev=pause_refused what=notebook`.
7. Start again. Mash Menu/B/Menu/B quickly. Expected: skip, no error; `ev=interrupt result=suppressed_unacked` lines allowed.
8. In gameplay: hold the right trigger (fire) and press Menu to open the menu, release the trigger, close the menu.
   Expected: no stuck firing; `ev=release reason=to_screen actions=1` in the log.
9. Gameplay controls as in `docs/PICO4_INPUT_TEST_PLAN.md` section 5 (fire, jump, Y notebook outside cutscenes, movement,
   quicksave gesture).
10. Recenter the view while in the menu (PICO system recenter gesture; the exact gesture is device-specific). The menu
    should reappear in front of you; if the runtime reports the change the log shows `ev=recenter` and `ev=snap reason=recenter`.
    If no such line appears, the runtime did not send a reference-space change event: record that.

If an error or misalignment still occurs: stop the capture right after it, keep the whole file (do not trim), add the
video time of the event, the APK SHA-256 and commit, and note which buttons were pressed. For the overflow, the `ev=overflow`
line (`kinds=`, `paused=`, `cam=`) is the decisive evidence; for the pointer, the last `ev=ptr`, `ev=snap`, `ev=pose` and
`ev=hand` lines before the report.
