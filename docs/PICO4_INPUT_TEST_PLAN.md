# PICO 4 controller verification plan (M2A)

Nothing in this document has been run on a headset yet. Mark each item PASS / FAIL with evidence.

## 1. Build the APK to test

1. GitHub -> **Actions** -> **Android Debug APK** -> **Run workflow**.
2. Branch: `claude/elegant-bardeen-ykx27m` (or `master` after the M2A pull request is merged).
3. Wait for the run to finish, download artifact `RTCWQuest-PICO4-Debug-ARM64` (kept 2 days).
4. Compare the SHA-256 in the run summary with the downloaded `.apk.sha256`, and note the run's commit hash.
   This also settles the APK provenance question left open after the first test.
5. Install over the previous build: `adb install -r rtcwquest-debug.apk`.

## 2. Capture logs

```
adb logcat -c
adb logcat -s RTCWInput:I RTCW:I > m2a_input.log
```

Keep the capture running through the whole procedure below (Feedback Bridge capture is equivalent if it records
logcat). Format of the lines: `docs/INPUT_DIAGNOSTICS.md`.

## 3. Procedure (about 3 minutes)

1. Launch the game and wait for the main menu.
2. Point the right controller at a menu entry (hover haptics should still occur).
3. Pull the **right trigger** fully, release. Repeat on a different menu entry.
4. Press **A** (right), then **B**. Press **X** and **Y** (left).
5. Pull the **left trigger** once (this is also the "switch menu hand" gesture), then try to click with the left controller.
6. Press the left **Menu** button once (expected: Escape).
7. Do NOT press the right PICO/Home button (reserved by the system).
8. Stop the capture and save the log plus a note of the date, headset firmware and APK commit/SHA-256.

## 4. Pass criteria

| # | Check | Expected |
|---|---|---|
| 1 | Menu entry activates on right trigger | PASS = entry reacts |
| 2 | `ev=profile hand=R` shows the bound profile | records which profile PICO uses |
| 3 | `ev=suggest ... bytedance/pico4_controller result=0` | bindings accepted (PASS) or the reason (FAIL) |
| 4 | `ev=btn hand=R name=trigger down=1/0` lines present | one pair per pull |
| 5 | `ev=key stage=gen name=MOUSE1` then `stage=deliver ... route=ui` | one pair per click |
| 6 | `ev=sync` never stays at `XR_SESSION_NOT_FOCUSED` | PASS |

If menu clicks still do not work, the log shows the first broken link (see "Reading a trace" in
`INPUT_DIAGNOSTICS.md`). Attach the log; do not guess.

## 5. Regression checklist (after clicks work)

Controller behaviour must be unchanged from before M2A. Check on the PICO 4, and on a Quest if one is available:

- [ ] Right controller moves the menu cursor; hover haptics still fire.
- [ ] Left trigger switches the menu cursor hand; haptic pulse confirms.
- [ ] Right trigger and A both click in the menu.
- [ ] In game: right trigger fires, A ducks, B jumps (right-handed default scheme, `VrInputDefault.c`).
- [ ] Left X = alternate weapon mode / detach scope, left Y = notebook.
- [ ] Left stick moves, right stick snap-turns, left trigger runs (Shift), right stick up/down changes weapon.
- [ ] Right grip: reload (tap) / weapon wheel (hold).
- [ ] Left grip + proximity: weapon stabilisation still works.
- [ ] Stick click: right = use/activate, left = kick.
- [ ] Left Menu button opens/closes the in-game menu.
- [ ] No new crash or ANR; frame rate unchanged (diagnostics are state-change-only).
- [ ] `adb logcat -s RTCWInput:I` shows only a handful of lines at idle plus one `hb` line every 5 s.
- [ ] (Quest) `ev=ext name=XR_BD_controller_interaction enabled=0` and controls behave as before.

## 6. Evidence record (fill in)

| Field | Value |
|---|---|
| Date | |
| Headset / firmware | PICO 4 (A8110), |
| APK commit and SHA-256 | |
| Result of check 1 | |
| `profile` line (R / L) | |
| `suggest` pico4 line | |
| Log file | |
