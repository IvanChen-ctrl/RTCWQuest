# CLAUDE.md — RTCWQuest PICO 4 Enhanced

## Purpose
Fork of RTCWQuest (native Android VR port of Return to Castle Wolfenstein, id Tech 3).
Goal: run natively on the **original PICO 4** (not Ultra), keep Meta Quest working where feasible.
Future (NOT started): AI mercenaries, LAN co-op multiplayer. Do not implement until a milestone asks.

## Verified toolchain (from repo files; not yet built or run)
- Android project root: `Projects/Android` (Gradle wrapper 8.2.1, AGP 8.2.1; JDK 17 expected for AGP 8.2, not pinned in repo)
- compileSdk 32, minSdk 29, targetSdk 32, ABI `arm64-v8a` only, `ndkVersion 25.1.8937393`
- Native: ndk-build, entry `Projects/Android/jni/Android.mk` + `Application.mk`, `APP_STL=c++_shared`, `APP_PLATFORM=android-29`
- Modules: `gl4es`, `qagame`, `ui`, `cgame`, `rtcw_client` (lib name loaded by manifest: `rtcwquest`)
- OpenXR loader: Maven `org.khronos.openxr:openxr_loader_for_android:1.1.60`, extracted by Gradle task `extractOpenXrLoader`
- Build: `cd Projects/Android && ./gradlew assembleDebug` -> `build/outputs/apk/debug/rtcwquest-debug.apk`

## Source map
- `Projects/Android/jni/RTCWVR/` — OpenXR layer (`TBXR_Common.c`), input (`OpenXrInput.c`, `VrInput*.c`), frame/surface (`RTCWVR_SurfaceView.c`)
- `Projects/Android/jni/rtcw/src/` — engine (client, server, renderer, game, cgame, ui, botai, botlib, qcommon)
- `Projects/Android/jni/SupportLibs/gl4es/` — GLES translation
- `java/com/drbeef/rtcwquest/` — `GLES3JNIActivity`, `GLES3JNILib` (JNI)
- `Projects/Android/z_vr_assets/` -> packed to `assets/z_vr_assets.pk3` by Gradle
- Legacy (ignore): `build.py`, `build.bat`, Oculus VrApi path

## Rules
- Preserve the existing architecture. No new engine/framework. Minimal, reversible, documented changes.
- No dependency, SDK, NDK, AGP, or API-target upgrades without explicit user approval.
- No paid services or APIs. Never touch the upstream Team-Beef-Studios / DrBeef repos; work only in this fork.
- Never commit RTCW game data (`*.pk3` from retail), SDK paths (`local.properties`), or secrets.
- Respect licenses: repo LICENSE is LGPL-3.0; engine under `jni/rtcw` has its own `COPYING.txt`/`LICENSE`/`README.txt` (RTCW GPL). Check before importing outside code.

## Workflow
- Branch `pico4-development` off `master`; never push to `master`; never merge PRs without the user.
- Commits: short imperative subject, scope prefix (e.g. `build:`, `input:`, `docs:`). One concern per commit.
- Code and comments in English only.
- Before claiming anything works: state whether it is VERIFIED (built/run, with evidence) or ASSUMED. Record physical PICO 4 test evidence (date, build, steps, logcat/photo) in `docs/`.
- Keep changes small; run the narrowest build/test that proves them.

## Future sessions
Read `docs/HANDOFF.md` first, then `docs/PICO4_BUILD_AUDIT.md`. Do not redo the broad audit. Update HANDOFF.md at the end of each session.
