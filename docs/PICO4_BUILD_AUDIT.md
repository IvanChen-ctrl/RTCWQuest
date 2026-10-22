# PICO 4 Build Audit (M0)

## VERIFIED FROM SOURCE
**Structure:** Gradle app project at `Projects/Android` (`build.gradle`, `settings.gradle`, wrapper 8.2.1). Sources: Java `../../java`, res `../../res`, assets `../../assets`. Native via `externalNativeBuild.ndkBuild` -> `jni/Android.mk`.

**Versions:** AGP 8.2.1; compile 32; min 29; target 32; NDK 25.1.8937393; ABI arm64-v8a; Java 1.8 compat; `APP_STL=c++_shared`, `NDK_TOOLCHAIN_VERSION=clang`. Lint: `ExpiredTargetSdkVersion` disabled. Legacy JNI lib packaging enabled.

**Native outputs expected:** `libgl4es`, `libqagame`, `libui`, `libcgame`, `librtcw_client` (Application.mk `APP_MODULES`), plus prebuilt `libopenxr_loader.so` from the AAR. Manifest loads lib name `rtcwquest`; the module producing that name was not located in `Android_client.mk` (module is `rtcw_client`; check `LOCAL_MODULE_FILENAME`) — UNRESOLVED, check at compile.

**APK entry:** `./gradlew assembleDebug` -> `Projects/Android/build/outputs/apk/debug/rtcwquest-debug.apk` (release analog under `release/`). `packResources` zips `z_vr_assets` into `assets/z_vr_assets.pk3` before assemble. Signing uses committed `android.debug.keystore` (password `android`).

**OpenXR:** Khronos loader 1.1.60 via Maven, copied by `extractOpenXrLoader` (hooked to `preBuild`). Headers vendored in `jni/RTCWVR/openxr`. Layer in `TBXR_Common.c`/`OpenXrInput.c`. Manifest has OpenXR permissions, broker `<queries>`, `IMMERSIVE_HMD` category.

**PICO hooks already present:** launcher category `com.picovr.intent.category.LAUNCHER`; meta `pvr.app.type=vr`, `pxr.controller=1`; PICO 4 interaction profile `/interaction_profiles/bytedance/pico4_controller`; runtime-name "pico" detection.

**Graphics:** gl4es (GL -> GLES) in `jni/SupportLibs/gl4es`; manifest requires GLES 3.1.

**External deps:** Maven (google, mavenCentral): AGP, support-compat 28.0.0, support-core-utils 28.0.0, openxr_loader. Local: `libs/haptic_service.aar`. No git submodules.

**Missing/environment-specific:** committed `local.properties` points to a Windows SDK; SDK/NDK not in repo; retail RTCW pk3 data not in repo (runtime only, `/sdcard/RTCWQuest/Main`).

**Networking present:** stock id Tech 3 server/net code in `jni/rtcw/src/server`, `qcommon` (extension point for later co-op).
**AI present:** `botai`, `botlib` (extension point for mercenaries).

## REQUIRES COMPILATION
- Gradle sync with AGP 8.2.1 + JDK 17 works.
- All five native modules compile with NDK 25.1.8937393 (Application.mk uses `APP_ALLOW_MISSING_DEPS=true`, which may hide missing deps; link errors possible).
- Resolution of the `rtcwquest` library name vs. `rtcw_client` module.
- Debug build arg `USE_ASAN=1` is passed but no `.mk` reference was found; effect unknown.
- APK contains all `.so` files for arm64-v8a.

## REQUIRES PHYSICAL DEVICE TESTING (PICO 4)
- App launches and appears in the library; OpenXR session creation with PICO runtime.
- Correct stereo, reprojection, resolution, 72 FPS stability.
- Controller profile binding, buttons, thumbsticks, haptics, tracking pose offsets.
- Storage permission flow (`MANAGE_EXTERNAL_STORAGE`) and pk3 discovery on PICO OS.
- Whether Quest-only `supportedDevices` meta blocks or affects install/launch.

## Risks
- targetSdk 32 / legacy storage on newer PICO OS.
- Quest-specific `haptic_service.aar` and Oculus metadata.
- gl4es performance on PICO 4 GPU.

## Smallest path to a debug APK
1. JDK 17; Android SDK platform 32 + build-tools; NDK 25.1.8937393 (via `sdkmanager`).
2. Set `ANDROID_HOME`; override `local.properties` `sdk.dir` (do not commit).
3. `cd Projects/Android && ./gradlew assembleDebug`
4. Fix only compile blockers; record results in HANDOFF.md.
