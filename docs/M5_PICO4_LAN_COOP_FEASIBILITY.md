# M5 research — two PICO 4 headsets playing RTCW co-op on one LAN

Status: **research only.** No networking or multiplayer code was written or changed in M4. Nothing here was run on a
headset. Date of the research: 2026-10-25.

Labels used below:

- **VERIFIED** — read in source (file and line given) during this research.
- **ASSUMED** — reasoned from general knowledge or from the verified facts; not checked in code or on hardware.
- **UNKNOWN** — needs a spike (a small experiment) before anyone can say.

## 1. What was inspected

| Item | Version |
|---|---|
| RTCWCoop | `rtcwcoop/rtcwcoop`, shallow clone of the default branch, commit `4bd1b6a` ("bugfix, thx ManiC", 2023-01-28). No commit after January 2023. |
| RTCWQuest | this repository, branch `pico4-m4-tactical-ai` (M3 + M4). |

Read-only clone, no changes to either upstream. Nothing was copied into this repository.

## 2. Verified findings

### 2.1 RTCWCoop is an iortcw fork, not a patch for the single-player engine

| Fact | Evidence |
|---|---|
| Its README is the iortcw README (SDL, OpenAL, x86_64, Mumble/VoIP, cURL, Ogg, "iowolfmp/iowolfsp"). The engine is the iortcw/ioquake3-derived tree. | `README.md` |
| It adds game types for co-op on top of the multiplayer-style game module: `GT_COOP_BATTLE` (1 vs 1), `GT_COOP_SPEEDRUN`, `GT_COOP_CLASSES`, `GT_COOP`, placed **before** `GT_SINGLE_PLAYER` in the enum so `g_gametype <= GT_COOP` means "co-op". | `code/game/bg_public.h:204-219` |
| Humans occupy the first `sv_maxcoopclients` client slots; bots/AI start after them. | `code/server/sv_bot.c:64` (`botStartSlot = sv_maxcoopclients ...`), `sv_client.c:548`, `sv_game.c:88` |
| `MAX_COOP_CLIENTS` is 8. | `code/qcommon/q_shared.h:94` |
| The original Cast AI runs in co-op: `AICast_Init()` is called when `g_gametype <= GT_SINGLE_PLAYER` (which includes every co-op type). The AI stops thinking while no co-op player is present. | `code/game/g_main.c:1465-1470`, `ai_cast_think.c:858-859` |
| AI code was adapted for several human players: loops over `MAX_COOP_CLIENTS`, "check against coop clients" in movement and blocking code. | `ai_cast.c:849-861`, `ai_cast_think.c:1061,1450,1544`, `ai_cast_script_actions.c:2642-2643,2818-2819` |
| Co-op maps use their **own script files**: `maps/<map>.coop.script` instead of the retail `.script`. | `code/game/g_script.c:276-278` |
| Save and load are disabled outside single player (`g_gametype != GT_SINGLE_PLAYER` -> refused). Co-op therefore has no savegame. | `code/game/g_save.c:1110,1384` |
| Cutscenes: the script action `startcam` just sends a `startCam <file>` server command to **all** clients. The single-player branches (camera portal on `g_entities[0]`) are guarded by `GT_SINGLE_PLAYER`. | `g_script_actions.c:1228-1247`, `g_main.c:1181` |
| Wire protocol version is **15** (with a commented-out 43). RTCWQuest's is **49**. The `msg.c` field tables differ (2229 vs 2284 lines). The two cannot talk to each other. | `rtcwcoop/code/qcommon/qcommon.h:289-291`; `RTCWQuest/.../qcommon/qcommon.h:243` |
| Co-op specific code is in `g_coop.c` (990 lines: spawn selection, class weapons, per-player stats, `coop_spawnpoint` entities) plus many `g_gametype` branches in the AI, script and trigger code. | `g_coop.c`, `g_spawn.c:427` |
| The repository does not contain the co-op map scripts (`main/` holds only `ui/menudef.h`). | directory listing |

### 2.2 RTCWQuest is the original single-player engine with an Android/OpenXR layer

| Fact | Evidence |
|---|---|
| Engine tree is the id single-player release layout (`client server renderer game cgame ui botai botlib qcommon`), `PROTOCOL_VERSION 49`. | `jni/rtcw/src/qcommon/qcommon.h:243` |
| The Android client build already compiles the **server** sources (`sv_*.c`) and the BSD-socket UDP layer `unix/unix_net.c`; the client has the `localservers` command and `NA_BROADCAST` support. So listen-server code and Quake-3-style LAN scanning exist in the binary. | `Android_client.mk:173-189`, `unix_net.c:74,215,529`, `cl_main.c:2618,3113` |
| Manifest permissions: `INTERNET`, `ACCESS_NETWORK_STATE` — no Wi-Fi multicast permission. | `AndroidManifest.xml:14-16` |
| `MAX_CLIENTS` is 128, `GENTITYNUM_BITS` 10, `MAX_PACKET_USERCMDS` 32, `PACKET_BACKUP` 32. | `game/q_shared.h`, `qcommon/qcommon.h:131-135` |
| The multiplayer menus (`joinserver`, `multiplayer`) are commented out of the menu list. | `Projects/Android/z_vr_assets/ui/menus.txt` |

### 2.3 The critical VR-specific finding

The VR pose is **not** part of the game protocol. The engine hands the game module a pointer to the **local process's**
`vr_client_info_t` every server frame:

```
sv_main.c:829   VM_Call( gvm, GAME_SET_VR_CLIENT_INFO, &vr );
g_main.c:342    gVR = (vr_client_info_t*)arg0;
```

- `usercmd_t` has no VR fields (angles, buttons, three move bytes only) — `game/q_shared.h:1322-1335`.
- The game module reads the head, hand and weapon pose from the global `gVR` **100 times**: 56 in `g_weapon.c`
  (muzzle and melee origin, aim), 16 in `bg_pmove.c`, 8 in `g_items.c`, 7 in `g_mover.c`, 5 in `g_misc.c`, the rest in
  `g_active.c`, `g_client.c`, `g_main.c`, and 2 in `g_merc.c` (added in M4 for the attack-designation ray).
- The game module assumes the human is entity 0: 22 uses of `g_entities[0]` in 11 files (M3/M4 mercenary code included).
- Consequence: today's game module works only when the server and the one VR client are the **same process**. For two
  headsets, the server needs a per-client pose (head, both hands, weapon) delivered through the network, and
  `CalcMuzzlePoints`, melee, activation and `PM_` code must read the **firing client's** pose instead of a global.
  This is the largest piece of work and is independent of the hosting choice.

### 2.4 Licensing

| Item | Finding |
|---|---|
| RTCWQuest `LICENSE` | LGPL-3.0 for the VR additions (per project rules). |
| `jni/rtcw/COPYING.txt`, `README.txt` | GPL v3 plus id Software's **additional terms** for the RTCW SP source ("EXCLUDED CODE" is not covered by the GPL). |
| RTCWCoop `COPYING.txt` | GPL v3. The co-op game files carry the same "RTCW SP Source Code ... GPL v3 or later + additional terms" header (checked in `g_coop.c`). |
| Conclusion | The RTCW-derived game files in both trees are under the same GPL v3 + id additional terms, so reusing co-op game-side logic is **possible in principle**. iortcw/ioquake3 engine parts, bundled third-party libraries (`curl`, `freetype`, `jpeg`, `libogg`, `opus`, `SDL2`, `zlib`, ...) and the co-op map/script assets have their own terms and must be checked **file by file** before any import. This is not legal advice. |
| Retail assets | Not redistributable; players need their own `pak0.pk3` etc. (unchanged from today). |

## 3. What cannot be reused directly

- **The RTCWCoop engine.** It is a different engine lineage (iortcw: SDL/OpenAL/renderer changes) and a different wire
  protocol. It cannot replace RTCWQuest's OpenXR/GLES client. *(verified: sections 2.1-2.2)*
- **RTCWCoop's game module as a drop-in.** It is the multiplayer game module (client sessions, teams, classes, stats).
  RTCWQuest's game module is the single-player one with VR changes. They differ in `playerState_t`/`entityState_t`
  fields, session handling and spawning. Merging them is a port, not a copy. *(verified in part; the size of the port is
  ASSUMED)*
- **RTCWCoop's map scripts.** Not in the repository; the campaign scripts must be authored or obtained separately.
  *(verified for the repository; availability elsewhere UNKNOWN)*

What **is** worth borrowing (as design, then code after a licence check):

1. The slot scheme: humans in `0..N-1`, AI after them (`sv_bot.c`).
2. `g_gametype <= GT_COOP` style grouping and the per-player loops over `MAX_COOP_CLIENTS` in the Cast AI.
3. Spawn selection and "no save/load in co-op" policy.
4. The `.coop.script` approach to cutscenes and objectives (scripts decide what is synchronised).

## 4. Required engine and game work (derived, ASSUMED sizes)

| Area | What is needed | Size (ASSUMED) |
|---|---|---|
| Per-client VR pose on the wire | Extend `usercmd_t` (or a parallel client->server message) with head position/orientation, both hand poses, weapon/handedness; bump `PROTOCOL_VERSION`; update `MSG_WriteDeltaUsercmd` | M |
| Server-side pose store | Replace global `gVR` by `vr[clientNum]` filled from the network; keep `gVR` for the local listen-server client | L (100 call sites) |
| Replicating the pose to other clients | Head/hands/weapon of remote players in `playerState_t`/`entityState_t` (or a dedicated entity), plus cgame drawing of a floating head and two hands | M |
| Entity-0 assumptions | Generalise 22 `g_entities[0]` uses, the mercenary code (`VrMerc_Player`), cinematic code, HUD | M |
| Game type | A `GT_COOP`-like type on top of the SP game module (spawn points, scoreboard-less HUD, respawn rules, shared objectives) | L |
| Shared enemy AI | Cast AI already runs server-side; needs "nearest player" targeting (RTCWCoop shows the pattern) | M |
| Campaign flow | Level transitions without savegames: carry weapons/health through cvars, `nextmap` voting; no checkpoint saves | M |
| Cutscenes | Skip them in co-op, or send `startCam` to all clients like RTCWCoop; scripts must not assume one player | M |
| Mercenary | Companion belongs to a player; slot scheme must reserve AI slots after the humans | S |
| LAN discovery / direct IP | Direct-IP connect already works through the stock `connect`; broadcast scan exists; Android may need a multicast/broadcast lock | S |
| Disconnect/reconnect | Stock Quake 3 client timeout and rejoin; game-side drop handling and spawn on rejoin | S-M |

## 5. Hosting options

### Option A — a PICO 4 hosts (listen server)

| Topic | Assessment | Basis |
|---|---|---|
| Code | Least new code: the Android client already links the server and game module. | VERIFIED (2.2) |
| CPU/memory | One XR2-class SoC would run the renderer at headset refresh rate, the server frame, Cast AI/AAS for the whole level and the networking. Unknown headroom. | ASSUMED; no measurement. M4 deliberately adds a per-scan budget but has no frame-time numbers yet. |
| Thermals/battery | Host headset works harder than the client for the whole session. | ASSUMED |
| Wi-Fi latency | Wi-Fi adds a variable 2-20 ms plus jitter and occasional multi-100 ms spikes; Quake-3-style prediction hides some. | ASSUMED |
| Host sleeps/exits | Session dies for both players. Headset sleep/resume is still unproven on hardware (M3 hotfix 2B). | VERIFIED (HANDOFF) / ASSUMED effect |
| Debuggability | Logcat only. | ASSUMED |
| Android networking | UDP sockets work. Wi-Fi power saving can drop broadcast/multicast; a multicast lock may be needed for discovery. App-to-app on the same SSID can be blocked by "AP/client isolation" on some routers. | ASSUMED |

### Option B — a local Windows/Linux PC hosts (dedicated server)

| Topic | Assessment | Basis |
|---|---|---|
| Code | A host build of server + game module + botlib is needed. The tree still contains `unix/` and Linux-era files, but the VR code, `VrClientInfo.h` and Android-specific parts must be stubbed. A separate host-only build target (the Android project stays ndk-build). | VERIFIED tree layout; build effort ASSUMED |
| Performance | Plenty of headroom for AI/AAS and more than two players. | ASSUMED |
| Failure behaviour | Headset sleep affects only that player; the server keeps running. | ASSUMED |
| Debugging/testing | Server logs, a debugger, headless scripted test clients, CI runs. Large speed-up for protocol work. | ASSUMED |
| Setup cost for the owner | A PC on the same router with the retail pk3 files; one command to start. | ASSUMED |
| Offline | No Internet needed. | VERIFIED by design (LAN UDP) |
| Campaign compatibility | Same game module either way; the server just runs the SP-derived co-op game type. | ASSUMED |

## 6. Recommendation

**Prototype on Option B (a PC runs the authoritative server); keep Option A as the follow-up product goal.**

Reasons:

1. The hard parts — per-client VR pose on the wire (2.3), a co-op game type, remote-player rendering — are identical in
   both options. Option B removes the unknown of "can a PICO 4 host a whole level at VR frame rate" from the first
   prototype so those problems can be isolated.
2. It allows server logs, desktop debugging and automated headless tests, which is how a netcode change this size
   should be developed.
3. A host sleeping or overheating cannot end the session while the protocol is still being debugged.
4. The same game-module code can later be hosted by a headset (Option A) once measurements show it is viable.

Main risk of this choice: a host build of the modified server/game module must be created (stubs for the VR/Android
parts). That is a bounded task and is the first spike below. If the owner has no PC available, Option A is the fallback
for the prototype, with the extra risks in the table above.

## 7. Proposed M5 prototype (deliberately small)

Acceptance target (matches the brief):

1. One authoritative server starts (`map <testmap>` with the new co-op game type).
2. Two clients connect on the same LAN (direct IP first).
3. One simple test map loads on both.
4. Both player positions are synchronised.
5. Each client sees the other's **head and two hands** at the transmitted pose.
6. One basic weapon-fire event (muzzle position and direction from the firing player's own pose) is synchronised and
   hits an entity.
7. Disconnect of one client leaves the other and the server running; the slot is freed; rejoin works.

Suggested order (each step ends with something testable):

| Step | Content | Exit test |
|---|---|---|
| M5.0 spike | Build the server + game module on a PC (stubbed VR), start a map, connect one **non-VR** client | Client reaches the map; server log shows it |
| M5.1 | Add the per-client VR pose to the usercmd path and a `vr[client]` array on the server; keep `gVR` for the local client | Host test round-trips the pose; server log shows two different poses |
| M5.2 | Game type with two human slots, spawn points, remote-player entity (head + hands) | Two clients see each other |
| M5.3 | Weapon fire from the firing client's pose; damage to a target entity | One hit registered with correct muzzle origin |
| M5.4 | Disconnect/rejoin, LAN scan or direct IP UI, hardware test with two PICO 4 | Checklist on two headsets |

Only after that: shared enemy AI on a real map, objectives, level transitions, mercenary participation.

## 8. Risks and unknowns

| Risk | Note |
|---|---|
| **UNKNOWN** | Whether the SP game module's AAS/botlib setup works unchanged with several human clients (RTCWCoop shows it can in its own module). |
| **UNKNOWN** | Real Wi-Fi behaviour on the owner's router (client isolation, power saving, jitter). Needs a 10-minute ping/iperf test between the headsets and the PC. |
| **UNKNOWN** | Cost of reading the pose from the network in `bg_pmove.c` (shared with client prediction): prediction needs the pose too. |
| Content | Campaign scripts for co-op do not exist in this repository; RTCWCoop-style `.coop.script` files must be written for each map, or the SP scripts adapted. |
| Scope creep | Do not start with the full campaign. |
| Licence | Check each imported file's header and the third-party libraries separately. |
| Security | The stock Quake 3 protocol is unauthenticated; acceptable on a home LAN, not for the Internet. |

## 9. What M4 did and did not do for M5

- Did: research and this document.
- Did not: change the engine, the protocol, `gVR`, or any multiplayer code. The M4 mercenary still assumes one local
  human in entity 0 (documented in `docs/M4_TACTICAL_AI.md`).
- Compatible by design: the tactical layer keeps its state in one struct and one-companion limit; a future per-player
  companion would turn it into an array indexed by owner.
