# HANDOFF — Milestone 0

## Status
M0 (audit and docs only) done. Nothing compiled or run. No game/build files changed.

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
- No `.github/` directory, no CI.
- Whether it builds at all: UNVERIFIED.

## M0 files created
`CLAUDE.md`, `docs/HANDOFF.md`, `docs/PICO4_BUILD_AUDIT.md` (no other files modified).

## Git
Commit hash: see `git log -1` on `pico4-development` (single M0 docs commit).

## Next
M1: reproducible debug APK build (GitHub Actions or a cloud session with SDK/NDK), fixing only what blocks compilation.
