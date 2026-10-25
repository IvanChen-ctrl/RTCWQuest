/*
===========================================================================
g_merc.c -- AI mercenary companion for the single-player campaign.

The companion is a normal Cast AI character (an "American" soldier on the
allied team) created through the stock AIChar_spawn path.  Following, door
handling, combat, weapon use and obstacle avoidance are the stock AICast
systems: the player is made the companion's leader (cs->leaderNum, exactly what
using a friendly NPC does in the original game) and HOLD is the stock
"wait goal" used by the same use-key toggle.  This file only adds a thin
supervisor around them:

  * hire / dismiss / orders (console command "merc ...", VR notebook page)
  * a small explicit state machine (see VrMerc_NextState in g_vrlogic.h)
  * spawn position validation, duplicate prevention, cleanup
  * friendly-fire protection (VrMerc_DamageBlocked)
  * a stall watchdog so an unreachable destination never causes endless
    movement

The companion is never written into savegames.  The player's wish for a
companion lives in the cvar "vr_merc" (not archived), so after a level
change or a load the supervisor hires a fresh one near the player.

Console / VR command: merc <hire|follow|hold|assist [0|1]|type [model|auto]|models|dismiss|status|
                           attack [now]|cover|regroup|stance [defensive|aggressive]>

M4 adds a tactical layer on top (target scoring with hysteresis, designation, friendly-fire
avoidance, formation spacing, cover, flanking, regroup/retreat).  It decides WHAT the companion
does and then calls the stock AICast functions (AIFunc_ChaseGoalStart, AIFunc_BattleTakeCoverStart,
the "noattack" mechanism) for HOW.  Decisions are in g_vrtactics.h; docs/M4_TACTICAL_AI.md.

Pure decision logic is in g_vrlogic.h / g_vrtactics.h and covered by tests/vr_gameplay and
tests/vr_tactics.
===========================================================================
*/

#include "g_local.h"
#include "../game/botlib.h"
#include "../game/be_aas.h"
#include "../game/be_ea.h"
#include "../game/be_ai_gen.h"
#include "../game/be_ai_goal.h"
#include "../game/be_ai_move.h"
#include "../botai/botai.h"
#include "ai_cast.h"
#include "g_vrlogic.h"
#include "g_vrtactics.h"
#include "g_vrmove.h"
#include "g_vrsquad.h"

#include "../../../RTCWVR/VrClientInfo.h"

extern vr_client_info_t* gVR;

#include <stdarg.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#define VRMERC_AINAME       "vr_merc_1"
#define VRMERC_WAITGOAL     "AI_wait_goal"      // classname used by the stock "stay" order
#define VRMERC_MANGOAL      "vr_merc_goal"      // temporary destination of a tactical maneuver
#define VRTAC_LOG_TAG       "RTCWTactical"
#define VRMERC_LOG_TAG      "RTCWMerc"
#define VRTAC_LOG_BURST     40                  // max diagnostic lines per second

// One (behaviour, visual) pairing that can be spawned.  The AI character fixes the weapons,
// attributes and sound scripts; model + skins are the visual identity.  Faction is set
// separately (the spawn entity's aiTeam), so a hostile-looking model is only ever a
// visual choice.
typedef struct {
	int  aiChar;                    // AICHAR_*
	char model[VRMERC_NAME_LEN];    // models/players/<model>
	char bodySkin[VRMERC_NAME_LEN];
	char headSkin[VRMERC_NAME_LEN];
	int  problem;                   // vrMercModelProblem_t
	char missing[320];              // exact missing resources (empty if complete)
} vrMercCand_t;

#define VRMERC_MAX_CAND 8

// A short scripted movement of the companion to a point: the tactical layer parks a temporary
// goal entity there and lets the stock chase function walk to it (the same mechanism as HOLD).
typedef enum {
	MAN_NONE = 0,
	MAN_REPOSITION,     // formation correction while following (state stays FOLLOW)
	MAN_COVER,          // the stock take-cover function owns the movement
	MAN_FLANK,
	MAN_REGROUP,
	MAN_TRAIL           // recovery: walk to a reachable point on the player's recent path
} vrMercManKind_t;

typedef struct {
	int kind;                   // vrMercManKind_t
	int goalEnt;                // temporary goal entity number, -1 if none
	vec3_t pos;                 // destination
	int startedAt;
	int maxMs;
	float reach;                // arrival distance
	int reason;                 // vrTacReason_t that started it
	vrMercProgress_t progress;
} vrMercManeuver_t;

typedef struct {
	vrTacCand_t cand[VRTAC_MAX_CAND];
	float score[VRTAC_MAX_CAND];
	int n;
	vec3_t friends[VRTAC_MAX_FRIENDS];
	int nFriends;
	int sustained;              // sustained fire on the companion
	int heavy;                  // heavy damage on the companion
	int playerUnderFire;
	int hostilesSeen;           // hostiles visible right now
	float healthFrac;
	float weaponRange;
} vrMercCtx_t;

typedef struct {
	int id;                     // companion number (log identity and AI name suffix), 1..VRMERC_MAX_SQUAD
	int state;                  // vrMercState_t
	int order;                  // vrMercOrder_t
	int slot;                   // entity (client) number of the companion, -1 if none
	int character;              // AICHAR_* used for the current / pending companion
	int chosen;                 // index into sq.cand[] used by this companion
	int holdGoal;               // entity number of the wait goal, -1 if none
	vec3_t holdPos;
	qboolean dismissRequested;

	int nextThink;
	int lastTargetTime;
	int returnAllowedAt;
	vrMercProgress_t progress;

	// ---- M4 tactical layer ----
	int stance;                 // vrTacStance_t, mirrors the cvar vr_merc_stance
	int stateEnterAt;
	int nextPerception;
	int target;                 // committed tactical target (entity number) or -1
	int targetSince;
	int lastOverrideAt;         // last time the stock AI's target was replaced
	int designated;             // entity the player pointed at, or -1
	int designatedUntil;
	int coverMeUntil;
	vrTacDmgLog_t dmgSelf;      // damage taken by the companion
	vrTacDmgLog_t dmgPlayer;    // damage taken by the player
	int hurtSelfBy, hurtSelfAt;
	int hurtPlayerBy, hurtPlayerAt;
	// adaptive following (g_vrmove.h)
	vrMove_t move;
	vrRecover_t rec;
	vrTrail_t trail;
	int moveNextMetrics;
	int moveLOS;
	float movePathDist;
	int moveLastPlayerArea;
	vec3_t moveLastPlayerAreaPos;
	int moveChaseAt;            // last time we pointed the AI at the player
	int moveForced;             // we currently own the chase (RUN / CATCHUP)
	int moveWaitLogged;

	int manArrivedAt;           // when the current maneuver reached its destination (0 = not yet)
	int manTarget;              // target the current maneuver was started for
	int nextCoverSearch;
	int coverCooldownUntil;
	int coverFails;
	int nextFlankSearch;
	int lastFlankAt;
	int lastFlankFailAt;
	int flankFails;
	int noShotSince;            // 0 = the committed target has a clear shot
	int regroupEndAt;
	int ffValue;                // scriptNoAttackTime value that WE set (0 = none)
	int ffLogAt;
	int nextFormationCheck;
	int nextFormationAt;
	vrMercManeuver_t man;

	// counters for "merc status" (profiling aid, no per-frame logging)
	int supervisedFrames;
	int stateChanges;
	int perceptionRuns;

	vrMercCtx_t ctx;            // this companion's own tactical context (rebuilt every 250 ms)
	int nextSlotCheck;          // squad formation check
	int lastRole;               // formation role last used (for merc_formation_changed)
} vrMerc_t;

// State shared by the whole squad: one recruit may be on the way at a time, the model survey
// is done once per level, and the "attack my target" arming is the player's, not a soldier's.
typedef struct {
	qboolean modelsProbed;      // model files were checked on this level
	int numCand;                // candidates probed so far (native ones first)
	vrMercCand_t cand[VRMERC_MAX_CAND];

	qboolean spawnPending;      // a recruit is on the way (instance pendingIdx)
	int pendingIdx;             // squad[] index of the recruit on the way, -1 if none
	int spawnStart;
	int spawnEntity;            // temporary entity that AIChar_spawn will consume
	int spawnAttempts;
	int nextSpawnAttempt;
	int levelStart;

	int designateArmedUntil;    // waiting for the player's next fire press
	int lastAttackButton;
	int narrow;                 // the player is in a corridor / doorway (single-file formation)
	int nextNarrowCheck;
	int playerStillSince;       // level time the player stopped moving (0 = moving)
	int lastLimitLog;           // rate limit for merc_limit_reached
	int nextPoll;               // spawn polling cadence
	int designateMask;          // soldiers that the armed "attack my target" order addresses
} vrSquad_t;

static vrSquad_t sq;
static vrMerc_t squad[VRMERC_MAX_SQUAD];
static vrMerc_t *mcur = &squad[0];      // the soldier the code below is currently working on
#define merc ( *mcur )
#define mtx ( merc.ctx )
static unsigned char mercMoveMode[MAX_CLIENTS];     // vrMoveMode_t per entity number (side table read by the AI think hook)

static const vec3_t mercMins = { -18, -18, -24 };       // same box as BBOX_SMALL in ai_cast_characters.c
static const vec3_t mercMaxs = { 18, 18, 48 };

/*
===============
Soldier records
===============
*/
static void VrMerc_InstInit( int idx ) {
	vrMerc_t *m = &squad[idx];

	memset( m, 0, sizeof( *m ) );
	m->id = idx + 1;
	m->state = VRMERC_NONE;
	m->order = VRMERC_ORDER_FOLLOW;
	m->slot = -1;
	m->holdGoal = -1;
	m->target = -1;
	m->designated = -1;
	m->hurtSelfBy = m->hurtPlayerBy = -1;
	m->man.kind = MAN_NONE;
	m->man.goalEnt = -1;
	m->stance = vr_merc_stance.integer ? VRTAC_STANCE_AGGRESSIVE : VRTAC_STANCE_DEFENSIVE;
	m->lastRole = -1;
	VrMove_Reset( &m->move, level.time );
	VrTac_DmgReset( &m->dmgSelf );
	VrTac_DmgReset( &m->dmgPlayer );
}

/*
===============
helpers
===============
*/
static gentity_t *VrMerc_Player( void ) {
	gentity_t *p = &g_entities[0];

	if ( !p->inuse || !p->client || ( p->r.svFlags & SVF_CASTAI ) ) {
		return NULL;
	}
	if ( p->client->pers.connected != CON_CONNECTED ) {
		return NULL;
	}
	return p;
}

static qboolean VrMerc_IsCompanion( gentity_t *ent ) {
	return ( ent && ent->inuse && ent->client && ( ent->r.svFlags & SVF_CASTAI ) &&
			 VrSquad_IdxFromName( ent->aiName ) >= 0 ) ? qtrue : qfalse;
}

// The soldier record that owns this entity number, or NULL.
static vrMerc_t *VrMerc_ForEntity( int entNum ) {
	int i;

	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		if ( squad[i].slot >= 0 && squad[i].slot == entNum ) {
			return &squad[i];
		}
	}
	return NULL;
}

static int VrMerc_LiveMask( void ) {
	int i, mask = 0;

	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		if ( squad[i].slot >= 0 ) {
			mask |= 1 << i;
		}
	}
	return mask;
}

static int VrMerc_LiveCount( void ) {
	return VrSquad_CountBits( VrMerc_LiveMask() );
}

static int VrMerc_SquadLimit( void ) {
	return VrSquad_Limit( vr_merc_squad.integer );
}
// The companion's entity, or NULL if it is not (or no longer) in the game.
static gentity_t *VrMerc_Entity( void ) {
	gentity_t *ent;

	if ( merc.slot < 0 || merc.slot >= level.maxclients ) {
		return NULL;
	}
	ent = &g_entities[merc.slot];
	// the entity must carry THIS soldier's own AI name
	return ( VrMerc_IsCompanion( ent ) && VrSquad_IdxFromName( ent->aiName ) == merc.id - 1 ) ? ent : NULL;
}
static void VrMerc_Message( const char *text ) {
	trap_SendServerCommand( 0, va( "cp \"%s\n\"", text ) );
}

// A message from the current soldier: "Mercenary 2: ..." once the squad has several members.
static void VrMerc_Say( const char *text ) {
	if ( VrMerc_LiveCount() > 1 && !Q_strncmp( text, "Mercenary: ", 11 ) ) {
		VrMerc_Message( va( "Mercenary %d: %s", merc.id, text + 11 ) );
	} else {
		VrMerc_Message( text );
	}
}

static const char *VrMerc_HireReasonText( int reason ) {
	switch ( reason ) {
	case VRMERC_HIRE_NOT_SINGLEPLAYER:  return "Mercenary: single player only";
	case VRMERC_HIRE_NO_PLAYER:         return "Mercenary: player not ready";
	case VRMERC_HIRE_PLAYER_DEAD:       return "Mercenary: you are dead";
	case VRMERC_HIRE_CINEMATIC:         return "Mercenary: not during a cutscene";
	case VRMERC_HIRE_LOADING:           return "Mercenary: level is still loading";
	case VRMERC_HIRE_AI_DISABLED:       return "Mercenary: AI is disabled (bot_enable)";
	case VRMERC_HIRE_ALREADY_ACTIVE:    return "Mercenary: squad is full";
	case VRMERC_HIRE_SPAWN_PENDING:     return "Mercenary: already on the way";
	case VRMERC_HIRE_NO_CLIENT_SLOT:    return "Mercenary: no free character slot";
	case VRMERC_HIRE_NO_NAVIGATION:     return "Mercenary: this map has no navigation data";
	case VRMERC_HIRE_NO_SPAWN_POINT:    return "Mercenary: no safe spot near you";
	case VRMERC_HIRE_NO_MODEL:          return "Mercenary: no compatible soldier model (see log, merc models)";
	default:                            return "Mercenary: cannot hire now";
	}
}

static const char *VrMerc_StateName( int state ) {
	switch ( state ) {
	case VRMERC_FOLLOW:     return "FOLLOW";
	case VRMERC_HOLD:       return "HOLD";
	case VRMERC_ENGAGE:     return "ENGAGE";
	case VRMERC_RETURN:     return "RETURN_TO_PLAYER";
	case VRMERC_DEAD:       return "DEAD";
	case VRMERC_DISMISSED:  return "DISMISSED";
	case VRMERC_COVER:      return "SEEK_COVER";
	case VRMERC_FLANK:      return "FLANK";
	case VRMERC_REGROUP:    return "REGROUP";
	default:                return "NONE";
	}
}

static qboolean VrMerc_Loading( void ) {
	return ( saveGamePending || g_reloading.integer || level.reloadPauseTime > level.time ||
			 level.intermissiontime || trap_Cvar_VariableIntegerValue( "cg_norender" ) ) ? qtrue : qfalse;
}

static qboolean VrMerc_CameraActive( gentity_t *player ) {
	return ( player->client->cameraPortal || ( player->client->ps.eFlags & EF_VIEWING_CAMERA ) ) ? qtrue : qfalse;
}

/*
===============
Model discovery

Reads (read-only, nothing is extracted, copied or uploaded) the files the server and cgame
loaders need for a character model, through the engine's virtual file system, so the result
follows the real search order: base game pk3 files and any mod directory / mission pack,
later pk3 files overriding earlier ones.  Without these files ClientUserinfoChanged aborts with
"Failed to load animation scripts for model <name>" or the character would be invisible / in
its bind pose.  Skin names are discovered, not assumed.
===============
*/
// preference order of the native (behaviour + visual) pairings
static const int mercNativeChars[] = { AICHAR_AMERICAN, AICHAR_PARTISAN, AICHAR_SOLDIER, AICHAR_ELITEGUARD };

#define VRMERC_TEXT_MAX 100000      // same limit as G_ParseAnimationFiles
static char mercText[VRMERC_TEXT_MAX];

// Reads a whole text file. Returns its length, or 0 if it is missing, empty or too long.
static int VrMerc_ReadFile( const char *name ) {
	fileHandle_t f = 0;
	int len = trap_FS_FOpenFile( name, &f, FS_READ );

	if ( len <= 0 || len >= VRMERC_TEXT_MAX - 1 ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return 0;
	}
	trap_FS_Read( mercText, len, f );
	mercText[len] = 0;
	trap_FS_FCloseFile( f );
	return len;
}

static qboolean VrMerc_FileOk( const char *name ) {
	fileHandle_t f = 0;
	int len = trap_FS_FOpenFile( name, &f, FS_READ );

	if ( f ) {
		trap_FS_FCloseFile( f );
	}
	return len > 0 ? qtrue : qfalse;
}

static const char *VrMerc_ModelProblemText( int problem ) {
	switch ( problem ) {
	case VRMERC_MODEL_OK:               return "ok";
	case VRMERC_MODEL_NO_ANIM_CFG:      return "wolfanim.cfg missing";
	case VRMERC_MODEL_NO_ANIM_SCRIPT:   return "wolfanim.script missing";
	case VRMERC_MODEL_NO_AI_STATES:     return "animation script has no AI states";
	case VRMERC_MODEL_NO_BODY:          return "body mesh/skin files missing";
	case VRMERC_MODEL_NO_HEAD_SKIN:     return "head skin missing";
	default:                            return "unknown";
	}
}

// Lists the "<prefix><name>.skin" names of a model directory.
static int VrMerc_ListSkins( const char *model, const char *prefix, char names[][VRMERC_NAME_LEN] ) {
	static char listBuf[8192];
	char dir[MAX_QPATH];
	int count;

	Com_sprintf( dir, sizeof( dir ), "models/players/%s", model );
	count = trap_FS_GetFileList( dir, ".skin", listBuf, sizeof( listBuf ) );
	if ( count <= 0 ) {
		return 0;
	}
	return VrMerc_SkinNamesFromList( listBuf, count, prefix, names, VRMERC_MAX_SKINS );
}

// Fills c (model, skins, problem, missing list) for the given model directory.
static void VrMerc_ProbeModelDir( vrMercCand_t *c, const char *model ) {
	char bodyNames[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
	char lowerNames[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
	char upperNames[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
	char headNames[VRMERC_MAX_SKINS][VRMERC_NAME_LEN];
	char path[MAX_QPATH];
	int nBody, nLower, nUpper, nHead, idx;
	vrMercModelFiles_t f;

	memset( &f, 0, sizeof( f ) );
	Q_strncpyz( c->model, model, sizeof( c->model ) );
	Q_strncpyz( c->bodySkin, "default", sizeof( c->bodySkin ) );
	Q_strncpyz( c->headSkin, "default", sizeof( c->headSkin ) );

	// animation config and script (the server loader's exact files and rules)
	Com_sprintf( path, sizeof( path ), "models/players/%s/wolfanim.cfg", model );
	if ( VrMerc_ReadFile( path ) > 0 ) {
		f.hasAnimCfg = 1;
		f.cfgVersion = VrMerc_CfgVersion( mercText );
	}
	Com_sprintf( path, sizeof( path ), "models/players/%s/wolfanim.script", model );
	if ( VrMerc_ReadFile( path ) > 0 ) {
		f.hasAnimScript = 1;
		f.scriptStateMask = VrMerc_ScriptStateMask( mercText );
	} else if ( f.hasAnimCfg && f.cfgVersion <= 1 && VrMerc_ReadFile( "models/players/default.script" ) > 0 ) {
		f.hasDefaultScript = 1;
		f.scriptStateMask = VrMerc_ScriptStateMask( mercText );
	}

	// skins (discovered) and meshes
	nBody = VrMerc_ListSkins( model, "body_", bodyNames );
	nLower = VrMerc_ListSkins( model, "lower_", lowerNames );
	nUpper = VrMerc_ListSkins( model, "upper_", upperNames );
	nHead = VrMerc_ListSkins( model, "head_", headNames );

	idx = VrMerc_PickSkinIndex( bodyNames, nBody );
	if ( idx >= 0 ) {
		f.hasBodySkin = 1;
		Q_strncpyz( c->bodySkin, bodyNames[idx], sizeof( c->bodySkin ) );
	} else {
		idx = VrMerc_PickCommonSkin( lowerNames, nLower, upperNames, nUpper );
		if ( idx >= 0 ) {
			f.hasLowerSkin = f.hasUpperSkin = 1;
			Q_strncpyz( c->bodySkin, lowerNames[idx], sizeof( c->bodySkin ) );
		}
	}
	idx = VrMerc_PickSkinIndex( headNames, nHead );
	if ( idx >= 0 ) {
		f.hasHeadSkin = 1;
		Q_strncpyz( c->headSkin, headNames[idx], sizeof( c->headSkin ) );
	}
	Com_sprintf( path, sizeof( path ), "models/players/%s/body.mds", model );
	f.hasBodyMesh = VrMerc_FileOk( path );
	Com_sprintf( path, sizeof( path ), "models/players/%s/lower.md3", model );
	f.hasLowerMesh = VrMerc_FileOk( path );
	Com_sprintf( path, sizeof( path ), "models/players/%s/upper.md3", model );
	f.hasUpperMesh = VrMerc_FileOk( path );

	c->problem = VrMerc_ModelProblem( &f );
	c->missing[0] = 0;
	if ( c->problem != VRMERC_MODEL_OK ) {
		VrMerc_DescribeMissing( model, c->bodySkin, c->headSkin, &f, c->missing, sizeof( c->missing ) );
	}
	G_Printf( "VR mercenary: model '%s' (%s): %s [cfg=%d v%d script=%d defscript=%d states=0x%x "
			  "body=%d/%d lower/upper=%d%d%d%d head=%d] skins %s/%s%s%s\n",
			  model, aiDefaults[c->aiChar].name, VrMerc_ModelProblemText( c->problem ),
			  f.hasAnimCfg, f.cfgVersion, f.hasAnimScript, f.hasDefaultScript, f.scriptStateMask,
			  f.hasBodySkin, f.hasBodyMesh, f.hasLowerSkin, f.hasUpperSkin, f.hasLowerMesh, f.hasUpperMesh,
			  f.hasHeadSkin, c->bodySkin, c->headSkin,
			  c->missing[0] ? " MISSING: " : "", c->missing );
}

// Model directory name of a native character (the part of its default skin before '/').
static void VrMerc_NativeModelName( int aiChar, char *out, int outSize ) {
	char *slash;

	Q_strncpyz( out, aiDefaults[aiChar].skin, outSize );
	slash = strchr( out, '/' );
	if ( slash ) {
		*slash = 0;
	}
}

// Probes the native pairings once per level.
static void VrMerc_ProbeModels( void ) {
	int i;

	if ( sq.modelsProbed ) {
		return;
	}
	sq.numCand = 0;
	for ( i = 0; i < (int)( sizeof( mercNativeChars ) / sizeof( mercNativeChars[0] ) ) && sq.numCand < VRMERC_MAX_CAND; i++ ) {
		vrMercCand_t *c = &sq.cand[sq.numCand];
		char model[VRMERC_NAME_LEN];

		memset( c, 0, sizeof( *c ) );
		c->aiChar = mercNativeChars[i];
		VrMerc_NativeModelName( c->aiChar, model, sizeof( model ) );
		VrMerc_ProbeModelDir( c, model );
		sq.numCand++;
	}
	sq.modelsProbed = qtrue;
}

// Index into sq.cand[] of the pairing to spawn, or -1.  A model named by the cvar
// vr_merc_model is tried first; it may be a native model or any other directory under
// models/players that passes the same checks (it then gets the American behaviour).
static int VrMerc_PickModel( void ) {
	int problems[VRMERC_MAX_CAND];
	int i, preferred = -1;
	char want[VRMERC_NAME_LEN];

	VrMerc_ProbeModels();

	trap_Cvar_VariableStringBuffer( "vr_merc_model", want, sizeof( want ) );
	if ( want[0] ) {
		for ( i = 0; i < sq.numCand; i++ ) {
			if ( VrMerc_StrEqNoCase( sq.cand[i].model, want ) ) {
				preferred = i;
			}
		}
		if ( preferred < 0 && sq.numCand < VRMERC_MAX_CAND ) {
			vrMercCand_t *c = &sq.cand[sq.numCand];

			memset( c, 0, sizeof( *c ) );
			c->aiChar = AICHAR_AMERICAN;
			VrMerc_ProbeModelDir( c, want );
			preferred = sq.numCand++;
		}
	}
	for ( i = 0; i < sq.numCand; i++ ) {
		problems[i] = sq.cand[i].problem;
	}
	return VrMerc_PickFirstViable( problems, sq.numCand, preferred );
}

static void VrMerc_BuildHireContext( gentity_t *player, vrMercHireContext_t *c ) {
	memset( c, 0, sizeof( *c ) );
	c->singlePlayerGame = ( g_gametype.integer == GT_SINGLE_PLAYER );
	c->playerPresent = ( player != NULL );
	c->playerAlive = ( player && player->health > 0 );
	c->cameraActive = ( player && VrMerc_CameraActive( player ) );
	c->loading = VrMerc_Loading();
	c->aiEnabled = ( trap_Cvar_VariableIntegerValue( "bot_enable" ) != 0 );
	c->companionActive = ( VrMerc_LiveCount() >= VrMerc_SquadLimit() );     // "squad full"
	c->spawnPending = sq.spawnPending;
	c->freeClientSlots = aicast_maxclients - level.numPlayingClients;
	c->navigationLoaded = trap_AAS_Initialized() ? 1 : 0;
	c->modelUsable = ( VrMerc_PickModel() >= 0 );
}

/*
===============
VrMerc_IsMercEntity

Used by the savegame writer: the companion is not saved (a load would find a
client slot that the freshly started level has not created and abort), and neither
are the temporary goal entities it walks to.
===============
*/
qboolean VrMerc_IsMercEntity( int entnum ) {
	gentity_t *ent;
	int i;

	if ( entnum < 0 || entnum >= MAX_GENTITIES ) {
		return qfalse;
	}
	ent = &g_entities[entnum];
	if ( VrMerc_IsCompanion( ent ) ) {
		return qtrue;
	}
	// every soldier's temporary destination entities (HOLD post, maneuver goal) mean nothing without it
	if ( ent->inuse && ent->classname ) {
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			if ( ( entnum == squad[i].holdGoal && !strcmp( ent->classname, VRMERC_WAITGOAL ) ) ||
				 ( entnum == squad[i].man.goalEnt && !strcmp( ent->classname, VRMERC_MANGOAL ) ) ) {
				return qtrue;
			}
		}
	}
	// also the unconsumed spawn request, otherwise a save taken in that short window
	// would bring a second companion back when it is loaded
	return ( ent->inuse && ent->think == AIChar_spawn && VrSquad_IdxFromName( ent->aiName ) >= 0 ) ? qtrue : qfalse;
}
/*
===============
VrMerc_DamageBlocked
===============
*/
qboolean VrMerc_DamageBlocked( gentity_t *targ, gentity_t *attacker ) {
	int attackerComp, targComp;

	if ( !targ || !attacker || ( VrMerc_LiveMask() == 0 && !sq.spawnPending ) ) {
		return qfalse;
	}
	if ( g_gametype.integer != GT_SINGLE_PLAYER ) {
		return qfalse;
	}
	attackerComp = VrMerc_IsCompanion( attacker );
	// a soldier's own splash is left to the stock rules; every OTHER soldier is an ally and is protected
	targComp = attackerComp ? ( targ == attacker ) : VrMerc_IsCompanion( targ );
	return VrMerc_DamagePolicy(
			   attackerComp,
			   attacker->client && attacker->s.number == 0 && !( attacker->r.svFlags & SVF_CASTAI ),
			   targComp,
			   targ->client && targ->s.number == 0 && !( targ->r.svFlags & SVF_CASTAI ),
			   targ->aiTeam, AITEAM_ALLIES, AITEAM_NEUTRAL,
			   ( targ->r.svFlags & SVF_CASTAI ) != 0 ) ? qtrue : qfalse;
}
/*
===============
wait goal (HOLD anchor) helpers
===============
*/
static void VrMerc_FreeHoldGoal( cast_state_t *cs ) {
	if ( merc.holdGoal >= 0 ) {
		gentity_t *goal = &g_entities[merc.holdGoal];

		// stop following the goal before it disappears (otherwise the AI waits on a dead entity)
		if ( cs && cs->followEntity == merc.holdGoal ) {
			cs->followEntity = -1;
		}
		if ( goal->inuse && goal->classname && !strcmp( goal->classname, VRMERC_WAITGOAL ) ) {
			G_FreeEntity( goal );
		}
		merc.holdGoal = -1;
	}
}

static void VrMerc_AnchorAt( gentity_t *ent, cast_state_t *cs, vec3_t pos ) {
	gentity_t *goal;

	VrMerc_FreeHoldGoal( cs );
	VectorCopy( pos, merc.holdPos );

	goal = G_Spawn();
	goal->classname = VRMERC_WAITGOAL;
	goal->r.ownerNum = ent->s.number;
	G_SetOrigin( goal, pos );
	merc.holdGoal = goal->s.number;

	cs->leaderNum = -1;
	AIFunc_ChaseGoalStart( cs, goal->s.number, 128, qtrue );
}

static void VrMerc_ApplyFollow( gentity_t *player, cast_state_t *cs ) {
	VrMerc_FreeHoldGoal( cs );
	cs->followEntity = -1;
	cs->leaderNum = player->s.number;
}

static void VrMerc_Sound( gentity_t *ent, int which ) {
	const char *script = aiDefaults[merc.character].soundScripts[which];

	if ( script && ent->eventTime != level.time ) {
		G_AddEvent( ent, EV_GENERAL_SOUND, G_SoundIndex( (char *)script ) );
	}
}

static void VrMerc_SetPassive( cast_state_t *cs, qboolean passive ) {
	// stock "noattack" mechanism (script actions noattack / attack)
	cs->castScriptStatus.scriptNoAttackTime = passive ? 0x7fffffff : 0;
}

/*
===============
M4 tactical layer, part 1: diagnostics, state bookkeeping, damage notes, perception,
target selection, friendly-fire avoidance, designation, formation and the maneuver primitive.
===============
*/

/*
===============
VrTac_Log

Diagnostics on the Android logcat tag RTCWTactical (adb logcat -s RTCWTactical:I).
Format (same family as RTCWInput):   v1 t=<level.time ms> ev=<name> key=value ...
vr_merc_diag: 0 = off, 1 = events (default, a handful per minute), 2 = also rejections.
A burst limiter (VRTAC_LOG_BURST lines per second) protects against floods.
===============
*/
static int tacLogWindowStart;
static int tacLogCount;
static int tacLogDropped;

static void VrLog_Emit( const char *tag, const char *line ) {
#ifdef __ANDROID__
	__android_log_print( ANDROID_LOG_INFO, tag, "%s", line );
#else
	G_Printf( "%s: %s\n", tag, line );
#endif
}

// tag RTCWMerc: recruitment and lifecycle; tag RTCWTactical: decisions.  id = companion number (0 = squad).
static void VrLog_( const char *tag, int verbosity, int id, const char *ev, const char *fmt, ... ) {
	char body[320];
	char line[400];
	va_list ap;

	if ( vr_merc_diag.integer < verbosity ) {
		return;
	}
	if ( level.time - tacLogWindowStart >= 1000 || level.time < tacLogWindowStart ) {
		if ( tacLogDropped ) {
			Com_sprintf( line, sizeof( line ), "v1 t=%d ev=drop id=0 n=%d", level.time, tacLogDropped );
			VrLog_Emit( tag, line );
		}
		tacLogWindowStart = level.time;
		tacLogCount = 0;
		tacLogDropped = 0;
	}
	if ( ++tacLogCount > VRTAC_LOG_BURST ) {
		tacLogDropped++;
		return;
	}
	va_start( ap, fmt );
	vsnprintf( body, sizeof( body ), fmt, ap );
	va_end( ap );
	Com_sprintf( line, sizeof( line ), "v1 t=%d ev=%s id=%d %s", level.time, ev, id, body );
	VrLog_Emit( tag, line );
}

#define VrTac_Log( v, ev, ... )   VrLog_( VRTAC_LOG_TAG, v, merc.id, ev, __VA_ARGS__ )
#define VrMerc_Log( v, ev, ... )  VrLog_( VRMERC_LOG_TAG, v, merc.id, ev, __VA_ARGS__ )
#define VrSquad_Log( v, ev, ... ) VrLog_( VRMERC_LOG_TAG, v, 0, ev, __VA_ARGS__ )

// Single place where the supervisor state changes, so every transition is logged once.
static void VrMerc_SetState( int newState, int reason ) {
	if ( newState == merc.state ) {
		return;
	}
	VrTac_Log( 1, "state", "from=%s to=%s reason=%s after_ms=%d", VrMerc_StateName( merc.state ),
			   VrMerc_StateName( newState ), VrTac_ReasonName( reason ), level.time - merc.stateEnterAt );
	merc.state = newState;
	merc.stateEnterAt = level.time;
}

/*
===============
VrMerc_NoteDamage

Called from G_Damage for every hit: feeds the "recent damage" part of the tactical context.
===============
*/
void VrMerc_NoteDamage( gentity_t *targ, gentity_t *attacker, int damage ) {
	vrMerc_t *m;
	int i;

	if ( !targ || damage <= 0 || VrMerc_LiveMask() == 0 ) {
		return;
	}
	m = VrMerc_IsCompanion( targ ) ? VrMerc_ForEntity( targ->s.number ) : NULL;
	if ( m ) {
		VrTac_DmgAdd( &m->dmgSelf, level.time, damage );
		m->hurtSelfAt = level.time;
		m->hurtSelfBy = ( attacker && attacker != targ ) ? attacker->s.number : -1;
	} else if ( targ->client && targ->s.number == 0 && !( targ->r.svFlags & SVF_CASTAI ) ) {
		// the player was hurt: every soldier notes it for itself
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			if ( squad[i].slot < 0 ) {
				continue;
			}
			VrTac_DmgAdd( &squad[i].dmgPlayer, level.time, damage );
			squad[i].hurtPlayerAt = level.time;
			squad[i].hurtPlayerBy = ( attacker && attacker != targ ) ? attacker->s.number : -1;
		}
	}
}
static qboolean VrMerc_ScriptBusy( cast_state_t *cs ) {
	// a mission script is steering this character: never interfere with it
	return ( cs->castScriptStatus.castScriptEventIndex >= 0 ||
			 cs->castScriptStatus.scriptNoMoveTime > level.time ) ? qtrue : qfalse;
}

// Effective range of the companion's current weapon (game units).
static float VrMerc_WeaponRange( int weapon ) {
	switch ( weapon ) {
	case WP_KNIFE:
		return 96.0f;
	case WP_FLAMETHROWER:
	case WP_TESLA:
	case WP_VENOM:
		return 500.0f;
	case WP_LUGER:
	case WP_COLT:
	case WP_SILENCER:
	case WP_AKIMBO:
		return 900.0f;
	case WP_MP40:
	case WP_THOMPSON:
	case WP_STEN:
	case WP_AKIMBO_MP40:
	case WP_AKIMBO_THOMPSON:
		return 1100.0f;
	case WP_MAUSER:
	case WP_GARAND:
	case WP_FG42:
		return 1800.0f;
	case WP_SNIPERRIFLE:
	case WP_SNOOPERSCOPE:
	case WP_FG42SCOPE:
	case WP_SNIPER:
		return 3000.0f;
	default:
		return 1000.0f;
	}
}

// Aim ray of the player's dominant hand (VR) or of the view (fallback).
static void VrMerc_PlayerAim( gentity_t *p, vec3_t start, vec3_t dir ) {
	if ( gVR != NULL ) {
		vec3_t end;

		CalcMuzzlePointForHandActivate( p, qfalse, start, dir, end );
	} else {
		vec3_t right, up;

		VectorCopy( p->client->ps.origin, start );
		start[2] += p->client->ps.viewheight;
		AngleVectors( p->client->ps.viewangles, dir, right, up );
	}
}

static qboolean VrMerc_HostileAlive( gentity_t *ent, int idx ) {
	gentity_t *t;

	if ( idx < 0 || idx >= level.maxclients ) {
		return qfalse;
	}
	t = &g_entities[idx];
	return VrTac_ClassifyTarget( t->inuse, t->health > 0, ( t->r.svFlags & SVF_CASTAI ) != 0,
								 idx == 0, idx == ent->s.number, VrMerc_IsCompanion( t ),
								 ent->aiTeam, t->aiTeam, AITEAM_NEUTRAL ) == VRTAC_OK ? qtrue : qfalse;
}

/*
===============
Tactical context: what the companion legitimately knows.  Rebuilt every VRTAC_PERCEPTION_MS.
Hostiles are only considered if the companion's own AICast sight memory (cs->vislist) has a
sighting of them, or the player explicitly pointed at them.  Nothing is read for entities the
companion has never detected.
===============
*/


static void VrMerc_SetStockTarget( gentity_t *ent, cast_state_t *cs, int target ) {
	cast_visibility_t *vis = &cs->vislist[target];

	cs->enemyNum = target;
	vis->flags |= AIVIS_ENEMY;
	if ( cs->aiState < AISTATE_COMBAT ) {
		AICast_StateChange( cs, AISTATE_COMBAT );
	}
	merc.lastOverrideAt = level.time;
}

static void VrMerc_ClearDesignation( const char *why ) {
	if ( merc.designated >= 0 ) {
		VrTac_Log( 1, "designation_end", "target=%d reason=%s", merc.designated, why );
	}
	merc.designated = -1;
	merc.designatedUntil = 0;
}

static int VrMerc_CommittedTargetIndex( void );

static void VrMerc_Perceive( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	int now = level.time, i, k, hits, total, cur = -1, sel, switched, newTarget;
	vrTacScoreParams_t sp;
	vec3_t selfPos;
	int maxHealth = ent->client->ps.stats[STAT_MAX_HEALTH] > 0 ? ent->client->ps.stats[STAT_MAX_HEALTH] : 100;

	merc.perceptionRuns++;
	VectorCopy( ent->r.currentOrigin, selfPos );
	mtx.n = 0;
	mtx.nFriends = 0;
	mtx.hostilesSeen = 0;
	mtx.weaponRange = VrMerc_WeaponRange( cs->weaponNum );
	mtx.healthFrac = (float)ent->health / (float)maxHealth;

	VrTac_DmgWindow( &merc.dmgSelf, now, 4000, &hits, &total );
	mtx.sustained = ( hits >= 3 ) ? 1 : 0;
	mtx.heavy = ( total >= maxHealth * 3 / 10 ) ? 1 : 0;
	VrTac_DmgWindow( &merc.dmgPlayer, now, 3000, &hits, &total );
	mtx.playerUnderFire = ( hits >= 1 ) ? 1 : 0;

	if ( merc.designated >= 0 ) {
		if ( now >= merc.designatedUntil ) {
			VrMerc_ClearDesignation( "expired" );
		} else if ( !VrMerc_HostileAlive( ent, merc.designated ) ) {
			VrMerc_ClearDesignation( "invalid_or_dead" );
		}
	}

	sp.stance = merc.stance;
	sp.coverMeActive = ( now < merc.coverMeUntil ) ? 1 : 0;

	for ( i = 1; i < level.maxclients; i++ ) {
		gentity_t *t = &g_entities[i];
		cast_state_t *tcs;
		cast_visibility_t *vis;
		vrTacCand_t *c;
		int cls, lastSeen;
		float dist;

		if ( i == ent->s.number || !t->inuse || !t->client || !( t->r.svFlags & SVF_CASTAI ) || t->health <= 0 ) {
			continue;
		}
		cls = VrTac_ClassifyTarget( 1, 1, 1, 0, 0, VrMerc_IsCompanion( t ), ent->aiTeam, t->aiTeam, AITEAM_NEUTRAL );
		if ( cls != VRTAC_OK ) {
			// friendly bodies the companion must not shoot through
			if ( mtx.nFriends < VRTAC_MAX_FRIENDS && Distance( selfPos, t->r.currentOrigin ) < 1500.0f ) {
				VectorCopy( t->r.currentOrigin, mtx.friends[mtx.nFriends] );
				mtx.nFriends++;
			}
			continue;
		}
		if ( t->flags & FL_NOTARGET ) {
			continue;
		}
		vis = &cs->vislist[i];
		lastSeen = vis->real_visible_timestamp > vis->visible_timestamp ? vis->real_visible_timestamp : vis->visible_timestamp;
		if ( lastSeen <= 0 && i != merc.designated ) {
			continue;       // never detected: not part of the companion's knowledge
		}
		dist = Distance( selfPos, t->r.currentOrigin );
		if ( mtx.n >= VRTAC_MAX_CAND ) {
			// keep the nearest candidates: replace the farthest one if this one is closer
			int far = 0;

			for ( k = 1; k < mtx.n; k++ ) {
				if ( mtx.cand[k].dist > mtx.cand[far].dist ) {
					far = k;
				}
			}
			if ( dist >= mtx.cand[far].dist ) {
				continue;
			}
			c = &mtx.cand[far];
		} else {
			c = &mtx.cand[mtx.n++];
		}
		memset( c, 0, sizeof( *c ) );
		tcs = AICast_GetCastState( i );
		c->entNum = i;
		c->dist = dist;
		c->distToPlayer = Distance( player->r.currentOrigin, t->r.currentOrigin );
		c->visibleNow = ( vis->real_visible_timestamp > 0 && now - vis->real_visible_timestamp <= VRTAC_VISIBLE_MS ) ? 1 : 0;
		c->msSinceSeen = lastSeen > 0 ? now - lastSeen : 0;
		c->targetsPlayer = ( c->visibleNow && tcs && tcs->bs && tcs->enemyNum == player->s.number ) ? 1 : 0;
		c->targetsSelf = ( c->visibleNow && tcs && tcs->bs && tcs->enemyNum == ent->s.number ) ? 1 : 0;
		c->msSinceHurtPlayer = ( merc.hurtPlayerBy == i && now - merc.hurtPlayerAt < 3000 ) ? now - merc.hurtPlayerAt : -1;
		c->msSinceHurtSelf = ( merc.hurtSelfBy == i && now - merc.hurtSelfAt < 3000 ) ? now - merc.hurtSelfAt : -1;
		c->designated = ( i == merc.designated ) ? 1 : 0;
		c->inWeaponRange = ( dist <= mtx.weaponRange ) ? 1 : 0;
		{
			// how many squad mates are already on this enemy (fire is spread unless the enemy is dangerous)
			int j;

			for ( j = 0; j < VRMERC_MAX_SQUAD; j++ ) {
				if ( &squad[j] != mcur && squad[j].slot >= 0 && squad[j].target == i ) {
					c->squadClaims++;
				}
			}
		}
		if ( c->visibleNow ) {
			mtx.hostilesSeen++;
		}
	}

	for ( k = 0; k < mtx.n; k++ ) {
		mtx.score[k] = VrTac_ThreatScore( &mtx.cand[k], &sp );
		if ( mtx.cand[k].entNum == merc.target ) {
			cur = k;
		}
	}
	sel = VrTac_SelectTarget( mtx.score, mtx.n, cur, now - merc.targetSince, &switched );
	newTarget = sel >= 0 ? mtx.cand[sel].entNum : -1;

	if ( newTarget != merc.target ) {
		if ( newTarget >= 0 ) {
			VrTac_Log( 1, "merc_target_selected", "ent=%d prev=%d score=%.0f dist=%.0f visible=%d designated=%d candidates=%d",
					   newTarget, merc.target, mtx.score[sel], mtx.cand[sel].dist, mtx.cand[sel].visibleNow,
					   mtx.cand[sel].designated, mtx.n );
		} else {
			VrTac_Log( 1, "merc_target_lost", "prev=%d candidates=%d", merc.target, mtx.n );
		}
		merc.target = newTarget;
		merc.targetSince = now;
		merc.noShotSince = 0;
	}

	// line of fire to the committed target (one stock visibility check per scan)
	if ( merc.target >= 0 && vr_merc_assist.integer ) {
		if ( AICast_CheckAttack( cs, merc.target, qfalse ) ) {
			merc.noShotSince = 0;
		} else if ( !merc.noShotSince ) {
			merc.noShotSince = now;
		}
	} else {
		merc.noShotSince = 0;
	}

	// apply to the stock AI.  The stock acquisition keeps working; we replace its choice when it has
	// none or an invalid one, when the tactical choice just changed, or while a designation is active.
	if ( merc.target >= 0 && vr_merc_assist.integer && cs->enemyNum != merc.target &&
		 !( merc.move.mode == VRMOVE_CATCHUP && !mtx.sustained && !mtx.heavy && merc.designated < 0 ) ) {
		qboolean stockValid = VrMerc_HostileAlive( ent, cs->enemyNum );
		qboolean designatedActive = ( merc.target == merc.designated ) ? qtrue : qfalse;

		int ci = VrMerc_CommittedTargetIndex();
		qboolean sighted = ( ci >= 0 && ( mtx.cand[ci].visibleNow || mtx.cand[ci].designated ) ) ? qtrue : qfalse;

		// never push the AI at a remembered-but-unseen enemy: it would shoot at walls and chase ghosts
		if ( sighted && ( !stockValid || switched || ( designatedActive && now - merc.lastOverrideAt >= VRTAC_REASSERT_MS ) ) ) {
			VrMerc_SetStockTarget( ent, cs, merc.target );
		}
	}
}

/*
===============
VrMerc_AvoidFriendlyFire

Active, not just immunity: when the player or an allied character stands on the line between the
companion and its target, the companion holds fire (stock "noattack" timer, 450 ms, re-evaluated
every supervisor tick) until the line is clear again.
===============
*/
static void VrMerc_AvoidFriendlyFire( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vec3_t shooter, target;
	int blockedBy = -1, k;

	if ( !vr_merc_assist.integer ) {
		return;
	}
	if ( VrMerc_HostileAlive( ent, cs->enemyNum ) ) {
		VectorCopy( ent->r.currentOrigin, shooter );
		VectorCopy( g_entities[cs->enemyNum].r.currentOrigin, target );
		if ( VrTac_FriendInLineOfFire( shooter, target, player->r.currentOrigin, VRTAC_FF_RADIUS_PLAYER ) ) {
			blockedBy = 0;
		} else {
			for ( k = 0; k < mtx.nFriends; k++ ) {
				if ( VrTac_FriendInLineOfFire( shooter, target, mtx.friends[k], VRTAC_FF_RADIUS_ALLY ) ) {
					blockedBy = 1000 + k;
					break;
				}
			}
		}
	}
	if ( blockedBy >= 0 ) {
		merc.ffValue = level.time + VRTAC_FF_HOLD_MS;
		cs->castScriptStatus.scriptNoAttackTime = merc.ffValue;
		if ( level.time - merc.ffLogAt >= VRTAC_FF_LOG_MS ) {
			merc.ffLogAt = level.time;
			VrTac_Log( 1, "ff_avoid", "target=%d blocker=%s", cs->enemyNum, blockedBy == 0 ? "player" : "ally" );
		}
	} else if ( merc.ffValue ) {
		// release only our own hold, never a script's
		if ( cs->castScriptStatus.scriptNoAttackTime == merc.ffValue ) {
			cs->castScriptStatus.scriptNoAttackTime = 0;
		}
		merc.ffValue = 0;
	}
}

/*
===============
Player designation ("attack my target")
===============
*/
static const char *VrMerc_ReasonText( int reason ) {
	switch ( reason ) {
	case VRTAC_R_NO_COMPANION:          return "Mercenary: not hired";
	case VRTAC_R_COMPANION_DEAD:        return "Mercenary: not alive";
	case VRTAC_R_NOT_ACTIVE:            return "Mercenary: not ready";
	case VRTAC_R_PLAYER_UNAVAILABLE:    return "Mercenary: you cannot give orders now";
	case VRTAC_R_CINEMATIC:             return "Mercenary: not during a cutscene";
	case VRTAC_R_LOADING:               return "Mercenary: level is still loading";
	case VRTAC_R_ASSIST_OFF:            return "Mercenary: assist is off";
	case VRTAC_R_NO_TARGET:             return "Mercenary: no target aimed at";
	case VRTAC_R_TARGET_DEAD:           return "Mercenary: target is already dead";
	case VRTAC_R_TARGET_PLAYER:         return "Mercenary: that is you";
	case VRTAC_R_TARGET_SELF:           return "Mercenary: that is me";
	case VRTAC_R_TARGET_FRIENDLY:       return "Mercenary: that is a friend";
	case VRTAC_R_TARGET_NEUTRAL:        return "Mercenary: that is a civilian";
	case VRTAC_R_TARGET_NOT_AI:         return "Mercenary: not a combatant";
	case VRTAC_R_TARGET_RANGE:          return "Mercenary: target too far";
	default:                            return "Mercenary: cannot do that now";
	}
}

static vrTacOrderCtx_t VrMerc_OrderContext( gentity_t *player ) {
	vrTacOrderCtx_t c;
	gentity_t *comp = VrMerc_Entity();

	memset( &c, 0, sizeof( c ) );
	c.companionPresent = ( comp != NULL );
	c.companionAlive = ( comp && comp->health > 0 );
	c.spawnPending = sq.spawnPending;
	c.state = merc.state;
	c.playerOk = ( player && player->health > 0 );
	c.cinematic = ( player && VrMerc_CameraActive( player ) );
	c.loading = VrMerc_Loading();
	c.assist = ( vr_merc_assist.integer != 0 );
	return c;
}

static qboolean VrMerc_Designate( gentity_t *player, int mask ) {
	vec3_t start, dir, end, mins = { -6, -6, -6 }, maxs = { 6, 6, 6 };
	trace_t tr;
	gentity_t *t;
	int cls, num, i, applied = 0;
	vrMerc_t *saved = mcur;
	int first = -1;

	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		if ( ( mask & ( 1 << i ) ) && squad[i].slot >= 0 ) {
			first = i;
			break;
		}
	}
	if ( first < 0 ) {
		return qfalse;
	}
	VrMerc_PlayerAim( player, start, dir );
	VectorMA( start, VRTAC_DESIGNATE_RANGE, dir, end );
	trap_Trace( &tr, start, mins, maxs, end, player->s.number, MASK_SHOT );

	num = tr.entityNum;
	if ( num < 0 || num >= level.maxclients ) {
		cls = VRTAC_R_NO_TARGET;        // a wall, a prop or nothing
	} else {
		gentity_t *comp = &g_entities[squad[first].slot];

		t = &g_entities[num];
		cls = VrTac_ClassifyTarget( t->inuse, t->health > 0, ( t->r.svFlags & SVF_CASTAI ) != 0,
									num == player->s.number, VrMerc_IsCompanion( t ) && num == comp->s.number,
									VrMerc_IsCompanion( t ), comp->aiTeam, t->aiTeam, AITEAM_NEUTRAL );
		cls = VrTac_ValidateDesignation( cls, Distance( player->r.currentOrigin, t->r.currentOrigin ) );
	}
	if ( cls != VRTAC_OK ) {
		mcur = &squad[first];
		VrTac_Log( 1, "target_rejected", "ent=%d reason=%s", num, VrTac_ReasonName( cls ) );
		mcur = saved;
		VrMerc_Message( VrMerc_ReasonText( cls ) );
		return qfalse;
	}

	t = &g_entities[num];
	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		cast_state_t *cs;

		if ( !( mask & ( 1 << i ) ) || squad[i].slot < 0 ) {
			continue;
		}
		mcur = &squad[i];
		// the player pointed it out: tell the soldier where it is (the AI's own "last known position")
		cs = AICast_GetCastState( squad[i].slot );
		VectorCopy( t->r.currentOrigin, cs->vislist[num].visible_pos );
		cs->vislist[num].visible_timestamp = level.time;
		cs->vislist[num].flags |= AIVIS_ENEMY;
		merc.designated = num;
		merc.designatedUntil = level.time + VRTAC_DESIGNATE_HOLD_MS;
		merc.nextPerception = 0;
		VrTac_Log( 1, "designate", "ent=%d dist=%.0f", num, Distance( player->r.currentOrigin, t->r.currentOrigin ) );
		applied++;
	}
	mcur = saved;
	sq.designateArmedUntil = 0;
	VrMerc_Message( applied > 1 ? "Mercenaries: attacking your target" : "Mercenary: attacking your target" );
	return qtrue;
}
/*
===============
Maneuver primitive: send the companion to a point through the stock chase function.
The destination is a temporary goal entity (like the HOLD anchor); the standing order is
re-applied when the maneuver ends or is cancelled.
===============
*/
static void VrMerc_ReapplyOrder( gentity_t *player, cast_state_t *cs ) {
	if ( merc.order == VRMERC_ORDER_HOLD ) {
		if ( merc.holdGoal >= 0 ) {
			AIFunc_ChaseGoalStart( cs, merc.holdGoal, 128, qtrue );
		}
	} else {
		cs->followEntity = -1;
		cs->leaderNum = player->s.number;
		AIFunc_IdleStart( cs );
	}
}

static void VrMerc_ManeuverFreeGoal( cast_state_t *cs ) {
	if ( merc.man.goalEnt >= 0 ) {
		gentity_t *goal = &g_entities[merc.man.goalEnt];

		if ( cs && cs->followEntity == merc.man.goalEnt ) {
			cs->followEntity = -1;
		}
		if ( goal->inuse && goal->classname && !strcmp( goal->classname, VRMERC_MANGOAL ) ) {
			G_FreeEntity( goal );
		}
		merc.man.goalEnt = -1;
	}
}

static const char *VrMerc_ManName( int kind ) {
	switch ( kind ) {
	case MAN_REPOSITION:    return "reposition";
	case MAN_COVER:         return "cover";
	case MAN_FLANK:         return "flank";
	case MAN_REGROUP:       return "regroup";
	case MAN_TRAIL:         return "trail";
	default:                return "none";
	}
}

// Ends the current maneuver.  restore = hand control back to the standing order.
static void VrMerc_ManeuverEnd( gentity_t *player, cast_state_t *cs, int reason, qboolean restore ) {
	if ( merc.man.kind == MAN_NONE ) {
		return;
	}
	VrTac_Log( 1, "maneuver_end", "kind=%s reason=%s took_ms=%d", VrMerc_ManName( merc.man.kind ),
			   VrTac_ReasonName( reason ), level.time - merc.man.startedAt );
	VrMerc_ManeuverFreeGoal( cs );
	merc.man.kind = MAN_NONE;
	if ( restore && cs && player ) {
		VrMerc_ReapplyOrder( player, cs );
	}
}

// Records a maneuver (no goal entity: cover and regroup are driven by stock AI functions).
static void VrMerc_ManeuverMark( int kind, vec3_t pos, float reach, int maxMs, int reason, gentity_t *ent ) {
	merc.man.kind = kind;
	merc.man.goalEnt = -1;
	VectorCopy( pos, merc.man.pos );
	merc.man.startedAt = level.time;
	merc.man.maxMs = maxMs;
	merc.man.reach = reach;
	merc.man.reason = reason;
	merc.manArrivedAt = 0;
	VrMerc_ProgressReset( &merc.man.progress, Distance( ent->r.currentOrigin, pos ), level.time );
}

static qboolean VrMerc_ManeuverBegin( gentity_t *ent, cast_state_t *cs, int kind, vec3_t pos, float reach,
									  int maxMs, int reason ) {
	gentity_t *goal;

	if ( merc.man.kind != MAN_NONE ) {
		return qfalse;
	}
	goal = G_Spawn();
	goal->classname = VRMERC_MANGOAL;
	goal->r.ownerNum = ent->s.number;
	G_SetOrigin( goal, pos );

	VrMerc_ManeuverMark( kind, pos, reach, maxMs, reason, ent );
	merc.man.goalEnt = goal->s.number;

	AIFunc_ChaseGoalStart( cs, goal->s.number, reach, qtrue );
	return qtrue;
}

// Arrival / timeout / stall for goal-based maneuvers.  Returns the exit reason or VRTAC_OK to continue.
static int VrMerc_ManeuverProgress( gentity_t *ent ) {
	float dist = Distance( ent->r.currentOrigin, merc.man.pos );
	float flat;
	vec3_t d;

	VectorSubtract( ent->r.currentOrigin, merc.man.pos, d );
	d[2] = 0;
	flat = VectorLength( d );
	if ( flat <= merc.man.reach + 24.0f ) {
		return VRTAC_R_DONE;
	}
	if ( level.time - merc.man.startedAt >= merc.man.maxMs ) {
		return VRTAC_R_TIMEOUT;
	}
	if ( VrMerc_ProgressStalled( &merc.man.progress, dist, level.time, 24.0f, 3000 ) ) {
		return VRTAC_R_NAV_STALL;
	}
	return VRTAC_OK;
}

/*
===============
Navigation check for a candidate position: its floor, contents, AAS area and the travel time
from the companion.  Returns the travel time in 1/100 s, or -1 if the spot is unusable
(*reason receives the vrTacReason_t).
===============
*/
static int VrMerc_NavCheck( gentity_t *player, gentity_t *ent, cast_state_t *cs, vec3_t pos, vec3_t outFloor,
							int *reason ) {
	vec3_t top, bottom, feet;
	trace_t tr;
	int area, travel;

	// snap to the floor below the candidate
	VectorCopy( pos, top );
	top[2] += 24;
	VectorCopy( pos, bottom );
	bottom[2] -= 120;
	trap_Trace( &tr, top, mercMins, mercMaxs, bottom, ent->s.number, MASK_PLAYERSOLID );
	if ( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f || !VrMerc_GroundWalkable( tr.plane.normal[2] ) ) {
		*reason = VRTAC_R_NAV_SOLID;
		return -1;
	}
	VectorCopy( tr.endpos, outFloor );

	trap_Trace( &tr, outFloor, mercMins, mercMaxs, outFloor, ent->s.number, MASK_PLAYERSOLID );
	if ( tr.startsolid || tr.allsolid ) {
		*reason = VRTAC_R_NAV_SOLID;
		return -1;
	}
	VectorCopy( outFloor, feet );
	feet[2] += mercMins[2] + 2;
	if ( ( trap_PointContents( outFloor, -1 ) | trap_PointContents( feet, -1 ) ) &
		 ( CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP | CONTENTS_DONOTENTER ) ) {
		*reason = VRTAC_R_NAV_SOLID;
		return -1;
	}
	area = trap_AAS_PointAreaNum( outFloor );
	if ( area <= 0 || !trap_AAS_AreaReachability( area ) ) {
		*reason = VRTAC_R_NAV_UNREACHABLE;
		return -1;
	}
	if ( !cs->bs || cs->bs->areanum <= 0 ) {
		*reason = VRTAC_R_NAV_UNREACHABLE;
		return -1;
	}
	travel = ( area == cs->bs->areanum ) ? 1 :
			 trap_AAS_AreaTravelTimeToGoalArea( cs->bs->areanum, ent->r.currentOrigin, area, cs->travelflags );
	if ( travel <= 0 ) {
		*reason = VRTAC_R_NAV_UNREACHABLE;
		return -1;
	}
	*reason = VRTAC_OK;
	return travel;
}

/*
===============
Formation: while following with no target, do not stand on the player or in front of the
player's weapon.  A bounded, cooled-down move to a slot behind and to the side of the player;
the stock leader logic does everything else.
===============
*/
static void VrMerc_Formation( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vec3_t start, dir, slot, floor;
	int inWay, need = VRSQ_POS_NONE, attempt, nav, why, role, live, k, mateIdx = -1, stillMs;
	int now = level.time;
	float mateDist = 99999.0f, slotDist;
	const char *trigger;

	if ( merc.state != VRMERC_FOLLOW || merc.order != VRMERC_ORDER_FOLLOW || merc.man.kind != MAN_NONE ||
		 merc.move.mode != VRMOVE_NEAR ) {
		return;     // (while running / catching up the follow policy owns the movement)
	}
	if ( now < merc.nextFormationCheck || now < merc.nextFormationAt ) {
		return;
	}
	merc.nextFormationCheck = now + VRTAC_FORM_CHECK_MS;
	if ( merc.target >= 0 || VrMerc_ScriptBusy( cs ) || cs->enemyNum >= 0 ) {
		return;
	}
	live = VrMerc_LiveMask();
	role = VrSquad_Rank( live, merc.id - 1 );
	if ( role != merc.lastRole ) {
		if ( merc.lastRole >= 0 ) {
			VrMerc_Log( 1, "merc_formation_changed", "role=%d previous=%d squad=%d narrow=%d", role, merc.lastRole,
						VrSquad_CountBits( live ), sq.narrow );
		}
		merc.lastRole = role;
	}

	// 1. in the player's way (too close, or in front of the weapon)
	VrMerc_PlayerAim( player, start, dir );
	inWay = VrTac_RepositionReason( ent->r.currentOrigin, player->r.currentOrigin, dir[0], dir[1] );

	// 2. squad needs: distance to the nearest squad mate, and to this soldier's formation slot
	for ( k = 0; k < VRMERC_MAX_SQUAD; k++ ) {
		if ( &squad[k] != mcur && squad[k].slot >= 0 ) {
			float dm = Distance( ent->r.currentOrigin, g_entities[squad[k].slot].r.currentOrigin );

			if ( dm < mateDist ) {
				mateDist = dm;
				mateIdx = k;
			}
		}
	}
	VrSquad_Slot( player->r.currentOrigin, player->client->ps.viewangles[YAW], role, sq.narrow, slot );
	slotDist = Distance( ent->r.currentOrigin, slot );
	stillMs = sq.playerStillSince ? now - sq.playerStillSince : 0;
	need = VrSquad_PositionNeed( merc.id - 1, slotDist, stillMs, mateIdx, mateDist );

	if ( inWay == VRTAC_REPOS_NONE && need == VRSQ_POS_NONE ) {
		return;
	}
	trigger = inWay == VRTAC_REPOS_TOO_CLOSE ? "too_close" : inWay == VRTAC_REPOS_IN_FRONT ? "in_front" :
			  need == VRSQ_POS_SEPARATE ? "separation" : "slot";

	// the role slot first, then a closer one; both are checked against the navigation data
	for ( attempt = 0; attempt < 2; attempt++ ) {
		if ( attempt == 1 ) {
			VrTac_FormationSlot( player->r.currentOrigin, player->client->ps.viewangles[YAW],
								 role == VRSQ_ROLE_RIGHT ? 1 : -1, VRTAC_FORM_BACK * 0.7f, VRTAC_FORM_SIDE * 0.7f, slot );
		}
		nav = VrMerc_NavCheck( player, ent, cs, slot, floor, &why );
		if ( nav < 0 || nav > 600 ) {
			VrTac_Log( 2, "formation_slot_rejected", "role=%d attempt=%d reason=%s", role, attempt, VrTac_ReasonName( why ) );
			continue;
		}
		if ( VrMerc_ManeuverBegin( ent, cs, MAN_REPOSITION, floor, VRTAC_FORM_REACH, VRTAC_FORM_MAX_MS,
								   inWay == VRTAC_REPOS_TOO_CLOSE ? VRTAC_R_TOO_CLOSE :
								   inWay == VRTAC_REPOS_IN_FRONT ? VRTAC_R_IN_FRONT :
								   need == VRSQ_POS_SEPARATE ? VRTAC_R_SEPARATION : VRTAC_R_SLOT_FAR ) ) {
			merc.nextFormationAt = now + VRTAC_FORM_COOLDOWN_MS;
			VrTac_Log( 1, "merc_formation_changed", "role=%d trigger=%s narrow=%d travel=%d squad=%d", role, trigger, sq.narrow, nav,
					   VrSquad_CountBits( live ) );
		}
		return;
	}
	merc.nextFormationAt = now + VRTAC_FORM_COOLDOWN_MS;      // no usable slot: stay put, do not retry every tick
}
/*
===============
VrMerc_SetOrder

Applies a standing order immediately (the supervisor then follows it).
===============
*/
static void VrMerc_SetOrder( gentity_t *player, gentity_t *ent, int order, qboolean announce ) {
	cast_state_t *cs = AICast_GetCastState( ent->s.number );

	// an explicit order replaces any running maneuver (the order below re-points the AI)
	VrMerc_ManeuverEnd( player, cs, VRTAC_R_ORDER_CHANGED, qfalse );
	merc.moveForced = 0;                    // the new order re-points the AI; the follow policy starts over
	if ( merc.slot >= 0 && merc.slot < MAX_CLIENTS ) {
		mercMoveMode[merc.slot] = VRMOVE_NEAR;
	}
	merc.move.mode = VRMOVE_NEAR;
	merc.move.since = level.time;
	VrTac_Log( 1, "order", "cmd=%s", order == VRMERC_ORDER_HOLD ? "hold" : "follow" );
	merc.order = order;
	merc.returnAllowedAt = 0;       // an explicit command ends any return cooldown
	if ( order == VRMERC_ORDER_HOLD ) {
		vec3_t pos;

		VectorCopy( ent->r.currentOrigin, pos );
		VrMerc_AnchorAt( ent, cs, pos );
		VrMerc_SetState( VRMERC_HOLD, VRTAC_R_COMMAND );
		VrMerc_Sound( ent, STAYSOUNDSCRIPT );
		if ( announce ) {
			VrMerc_Say( "Mercenary: holding position" );
		}
	} else {
		VrMerc_ApplyFollow( player, cs );
		VrMerc_SetState( VRMERC_FOLLOW, VRTAC_R_COMMAND );
		VrMerc_Sound( ent, FOLLOWSOUNDSCRIPT );
		if ( announce ) {
			VrMerc_Say( "Mercenary: following you" );
		}
	}
	merc.stateChanges++;
}

/*
===============
VrMerc_Activate

Replaces the stock use-key handler: looking at the mercenary and pressing
"use" toggles follow / hold (same gesture as the original game's friendly NPCs).
===============
*/
static void VrMerc_Activate( int entNum, int activatorNum ) {
	gentity_t *player = VrMerc_Player();
	vrMerc_t *m = VrMerc_ForEntity( entNum ), *saved = mcur;
	gentity_t *ent;
	cast_state_t *cs;

	if ( !player || !m || activatorNum != player->s.number ) {
		return;
	}
	mcur = m;               // this callback comes from the engine: work on the soldier that was used
	ent = VrMerc_Entity();
	if ( ent && ent->s.number == entNum && VrMerc_StateIsActive( merc.state ) ) {
		cs = AICast_GetCastState( entNum );
		if ( cs->lastActivate <= level.time - 1000 ) {
			cs->lastActivate = level.time;
			VrMerc_SetOrder( player, ent, merc.order == VRMERC_ORDER_FOLLOW ? VRMERC_ORDER_HOLD : VRMERC_ORDER_FOLLOW, qtrue );
		}
	}
	mcur = saved;
}
/*
===============
VrMerc_Finish

The companion is gone (killed or dismissed): release every reference.
===============
*/
static void VrMerc_Finish( int terminalState ) {
	gentity_t *ent = VrMerc_Entity();
	cast_state_t *cs = ent ? AICast_GetCastState( ent->s.number ) : NULL;

	VrMerc_ManeuverEnd( NULL, cs, terminalState == VRMERC_DEAD ? VRTAC_R_DEAD : VRTAC_R_DISMISSED, qfalse );
	VrMerc_FreeHoldGoal( cs );
	if ( cs ) {
		if ( merc.ffValue && cs->castScriptStatus.scriptNoAttackTime == merc.ffValue ) {
			cs->castScriptStatus.scriptNoAttackTime = 0;
		}
		cs->activate = NULL;
		cs->leaderNum = -1;
	}
	merc.ffValue = 0;
	if ( merc.slot >= 0 && merc.slot < MAX_CLIENTS ) {
		mercMoveMode[merc.slot] = VRMOVE_NEAR;
	}
	merc.moveForced = 0;
	merc.target = -1;
	merc.designated = -1;
	merc.coverMeUntil = 0;
	VrMerc_Log( 1, terminalState == VRMERC_DEAD ? "merc_died" : "merc_dismissed", "reason=%s", terminalState == VRMERC_DEAD ? "dead" : "dismissed" );

	if ( terminalState == VRMERC_DISMISSED && ent ) {
		// make anybody who is still targeting us forget us, then remove the character
		// the same way the engine removes Cast AI on level shutdown
		ent->health = 0;
		ent->client->ps.stats[STAT_HEALTH] = 0;
		trap_DropClient( ent->s.number, "" );
	}

	VrMerc_Say( terminalState == VRMERC_DEAD ? "Mercenary: killed in action" : "Mercenary: dismissed" );   // while still counted
	if ( vr_merc_sel.integer == merc.id ) {
		trap_Cvar_Set( "vr_merc_sel", "0" );        // the soldier the notebook addressed is gone: address all again
	}
	VrMerc_SetState( terminalState, terminalState == VRMERC_DEAD ? VRTAC_R_DEAD : VRTAC_R_DISMISSED );
	merc.slot = -1;                         // the record is free again (it keeps the terminal state for "merc status")
	merc.dismissRequested = qfalse;
	merc.stateChanges++;
	// the wish is the number of soldiers that stay: no automatic re-hire of the one that is gone
	trap_Cvar_Set( "vr_merc", va( "%d", VrMerc_LiveCount() ) );
}

/*
===============
spawn
===============
*/
static qboolean VrMerc_FindSpawnPos( gentity_t *player, vec3_t outPos ) {
	int i;
	int playerArea;
	float yaw = player->client->ps.viewangles[YAW];
	vec3_t origin;
	vec3_t matePos[VRMERC_MAX_SQUAD];
	int nMates = 0;

	VectorCopy( player->client->ps.origin, origin );

	// positions of the soldiers that are already in the squad: a recruit never appears on top of one
	{
		int m;

		for ( m = 0; m < VRMERC_MAX_SQUAD; m++ ) {
			if ( squad[m].slot >= 0 && nMates < VRMERC_MAX_SQUAD ) {
				VectorCopy( g_entities[squad[m].slot].r.currentOrigin, matePos[nMates] );
				nMates++;
			}
		}
	}

	trap_AAS_SetCurrentWorld( AASWORLD_STANDARD );
	playerArea = trap_AAS_PointAreaNum( origin );

	for ( i = 0; i < VRMERC_SPAWN_CANDIDATES; i++ ) {
		float dx, dy;
		vec3_t start, top, bottom, pos, feet;
		trace_t tr;
		int area;

		if ( !VrMerc_SpawnOffset( i, yaw, &dx, &dy ) ) {
			continue;
		}
		VectorCopy( origin, start );
		start[0] += dx;
		start[1] += dy;

		// 1. a straight, unobstructed walk from the player (no walls or doors in between)
		trap_Trace( &tr, origin, mercMins, mercMaxs, start, player->s.number, MASK_PLAYERSOLID );
		if ( tr.startsolid || tr.allsolid || tr.fraction < 1.0f ) {
			continue;
		}

		// 2. find the floor below the candidate
		VectorCopy( start, top );
		top[2] += 16;
		VectorCopy( start, bottom );
		bottom[2] -= 96;
		trap_Trace( &tr, top, mercMins, mercMaxs, bottom, player->s.number, MASK_PLAYERSOLID );
		if ( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f ) {
			continue;
		}
		if ( !VrMerc_GroundWalkable( tr.plane.normal[2] ) ) {
			continue;
		}
		VectorCopy( tr.endpos, pos );
		if ( fabs( pos[2] - origin[2] ) > 64 ) {
			continue;       // different storey / ledge
		}

		// 3. nothing (solid, player or another character) may occupy the spot
		trap_Trace( &tr, pos, mercMins, mercMaxs, pos, player->s.number, MASK_PLAYERSOLID );
		if ( tr.startsolid || tr.allsolid ) {
			continue;
		}
		if ( !VrSquad_SpawnClear( pos, &matePos[0][0], nMates ) ) {
			continue;       // too close to a squad mate
		}

		// 4. not in water, lava, slime or a no-drop volume
		VectorCopy( pos, feet );
		feet[2] += mercMins[2] + 2;
		if ( ( trap_PointContents( pos, -1 ) | trap_PointContents( feet, -1 ) ) &
			 ( CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP ) ) {
			continue;
		}

		// 5. navigation: inside a walkable AAS area connected to the player's area
		area = trap_AAS_PointAreaNum( pos );
		if ( area <= 0 || !trap_AAS_AreaReachability( area ) ) {
			continue;
		}
		if ( playerArea > 0 && area != playerArea &&
			 trap_AAS_AreaTravelTimeToGoalArea( area, pos, playerArea, AICAST_TFL_DEFAULT ) <= 0 ) {
			continue;
		}

		VectorCopy( pos, outPos );
		return qtrue;
	}
	return qfalse;
}

static char mercSpawnSkin[2 * VRMERC_NAME_LEN + 2];
static char mercSpawnHead[VRMERC_NAME_LEN];

static qboolean VrMerc_RequestSpawn( gentity_t *player, vec3_t pos ) {
	gentity_t *spawn = G_Spawn();
	int i, pick;

	// Same fields the map loader fills for an "ai_american" entity; AIChar_spawn
	// (stock) consumes this entity and creates the real character a few frames later.
	// only a pairing whose model and animation files exist (CanHire has checked this)
	pick = VrMerc_PickModel();
	if ( pick < 0 ) {
		G_FreeEntity( spawn );
		return qfalse;
	}
	merc.chosen = pick;
	merc.character = sq.cand[pick].aiChar;
	spawn->classname = ( merc.character == AICHAR_PARTISAN ) ? "ai_partisan" :
					   ( merc.character == AICHAR_SOLDIER ) ? "ai_soldier" :
					   ( merc.character == AICHAR_ELITEGUARD ) ? "ai_eliteguard" : "ai_american";
	spawn->aiCharacter = merc.character;
	spawn->aiTeam = AITEAM_ALLIES;
	spawn->aiName = (char *)VrSquad_AiName( merc.id - 1 );      // unique per soldier (static string)
	// visual identity: discovered model and skins; faction stays allied (aiTeam above)
	// (static buffers: AIChar_spawn reads them a few frames later and only one request is pending)
	Com_sprintf( mercSpawnSkin, sizeof( mercSpawnSkin ), "%s/%s", sq.cand[pick].model, sq.cand[pick].bodySkin );
	Q_strncpyz( mercSpawnHead, sq.cand[pick].headSkin, sizeof( mercSpawnHead ) );
	spawn->aiSkin = mercSpawnSkin;
	spawn->aihSkin = mercSpawnHead;
	VectorCopy( pos, spawn->s.origin );
	VectorCopy( pos, spawn->r.currentOrigin );
	spawn->s.angles[YAW] = player->client->ps.viewangles[YAW];

	// the weapons must be registered so clients know their models
	for ( i = 0; aiDefaults[merc.character].weapons[i]; i++ ) {
		RegisterItem( BG_FindItemForWeapon( aiDefaults[merc.character].weapons[i] ) );
	}

	spawn->think = AIChar_spawn;
	spawn->nextthink = level.time + FRAMETIME * 4;

	sq.spawnPending = qtrue;
	sq.pendingIdx = merc.id - 1;
	sq.spawnStart = level.time;
	sq.spawnEntity = spawn->s.number;
	merc.dismissRequested = qfalse;
	G_Printf( "VR mercenary: spawning %s behaviour with model %s skin %s head %s\n",
			  aiDefaults[merc.character].name, sq.cand[pick].model, sq.cand[pick].bodySkin,
			  sq.cand[pick].headSkin );
	return qtrue;
}

static void VrMerc_CancelSpawn( void ) {
	if ( sq.spawnEntity >= 0 ) {
		gentity_t *spawn = &g_entities[sq.spawnEntity];

		// only free it if it is still our unconsumed spawn request
		if ( spawn->inuse && spawn->think == AIChar_spawn && VrSquad_IdxFromName( spawn->aiName ) >= 0 ) {
			G_FreeEntity( spawn );
		}
	}
	sq.spawnEntity = -1;
	sq.spawnPending = qfalse;
	sq.pendingIdx = -1;
}

static gentity_t *VrMerc_FindNewCompanion( void ) {
	int i;
	const char *name = VrSquad_AiName( merc.id - 1 );

	// the character that carries THIS recruit's unique name and is not yet in the squad
	for ( i = 1; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( VrMerc_IsCompanion( ent ) && ent->health > 0 && !strcmp( ent->aiName, name ) && !VrMerc_ForEntity( i ) ) {
			return ent;
		}
	}
	return NULL;
}
static void VrMerc_Adopt( gentity_t *player, gentity_t *ent ) {
	cast_state_t *cs = AICast_GetCastState( ent->s.number );

	sq.spawnPending = qfalse;
	sq.pendingIdx = -1;
	sq.spawnEntity = -1;
	sq.spawnAttempts = 0;
	merc.slot = ent->s.number;
	merc.nextThink = level.time + ( merc.id - 1 ) * 30;      // stagger the soldiers' ticks
	merc.lastTargetTime = level.time;
	merc.target = -1;
	merc.designated = -1;
	VrMove_Reset( &merc.move, level.time );
	VrRecover_Reset( &merc.rec, ent->r.currentOrigin, level.time );
	VrTrail_Reset( &merc.trail );
	merc.moveForced = 0;
	merc.moveLastPlayerArea = 0;
	merc.hurtSelfBy = merc.hurtPlayerBy = -1;
	merc.coverCooldownUntil = merc.coverFails = merc.nextCoverSearch = 0;
	merc.lastFlankAt = merc.lastFlankFailAt = merc.flankFails = merc.nextFlankSearch = 0;
	merc.noShotSince = merc.regroupEndAt = merc.coverMeUntil = 0;
	merc.man.kind = MAN_NONE;
	merc.man.goalEnt = -1;
	VrTac_DmgReset( &merc.dmgSelf );
	VrTac_DmgReset( &merc.dmgPlayer );
	VrMerc_Log( 1, "merc_hired", "ent=%d name=%s squad=%d character=%s stance=%s", ent->s.number, ent->aiName, VrMerc_LiveCount(), aiDefaults[merc.character].name,
			   merc.stance == VRTAC_STANCE_AGGRESSIVE ? "aggressive" : "defensive" );
	cs->activate = VrMerc_Activate;
	VrMerc_SetPassive( cs, vr_merc_assist.integer == 0 );

	// start in the current standing order
	VrMerc_SetOrder( player, ent, merc.order, qfalse );
	VrMerc_Say( "Mercenary: hired" );
}

static void VrMerc_PollSpawn( gentity_t *player ) {
	gentity_t *ent = VrMerc_FindNewCompanion();

	if ( ent ) {
		VrMerc_Adopt( player, ent );
		return;
	}
	if ( level.time - sq.spawnStart > VRMERC_SPAWN_TIMEOUT_MS ) {
		VrMerc_CancelSpawn();
		sq.nextSpawnAttempt = level.time + VRMERC_SPAWN_RETRY_MS;
		G_Printf( "VR mercenary: spawn timed out (attempt %d)\n", sq.spawnAttempts );
	}
}

/*
===============
VrMerc_TryHire

Shared by the hire command and the automatic re-hire.
Returns a vrMercHireResult_t.
===============
*/
static int VrMerc_TryHire( gentity_t *player ) {
	vrMercHireContext_t ctx;
	vec3_t pos;
	int result, i, idx, used[VRMERC_MAX_SQUAD];
	vrMerc_t *saved = mcur;
	gentity_t *orphan;

	VrMerc_BuildHireContext( player, &ctx );
	result = VrMerc_CanHire( &ctx );
	if ( result != VRMERC_HIRE_OK ) {
		return result;
	}
	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		used[i] = ( squad[i].slot >= 0 );
	}
	idx = VrSquad_PickFreeInstance( used, VRMERC_MAX_SQUAD );
	if ( idx < 0 ) {
		return VRMERC_HIRE_ALREADY_ACTIVE;
	}
	VrMerc_InstInit( idx );                 // a fresh record with its own identity
	mcur = &squad[idx];
	result = VRMERC_HIRE_OK;

	// a character from an earlier request that finished after we gave up on it: take it over
	// instead of creating a second one
	orphan = VrMerc_FindNewCompanion();
	if ( orphan ) {
		VrMerc_Adopt( player, orphan );
	} else if ( !VrMerc_FindSpawnPos( player, pos ) ) {
		result = VRMERC_HIRE_NO_SPAWN_POINT;
	} else if ( !VrMerc_RequestSpawn( player, pos ) ) {
		result = VRMERC_HIRE_NO_MODEL;
	} else {
		sq.spawnAttempts++;
	}
	mcur = saved;
	return result;
}
/*
===============
M4 tactical layer, part 2: cover, flank, regroup / retreat.
Entry decisions and exit rules are the pure functions in g_vrtactics.h; the code below
gathers the engine facts (traces, AAS, the AI's sight memory) and calls the stock AI.
===============
*/
static int VrMerc_CommittedTargetIndex( void ) {
	int k;

	for ( k = 0; k < mtx.n; k++ ) {
		if ( mtx.cand[k].entNum == merc.target ) {
			return k;
		}
	}
	return -1;
}

static qboolean VrMerc_HaveTargetNow( gentity_t *ent, cast_state_t *cs ) {
	return ( merc.target >= 0 || VrMerc_HostileAlive( ent, cs->enemyNum ) ) ? qtrue : qfalse;
}

// Hand the AI back after a maneuver and move to the matching base state.
static void VrMerc_AfterManeuver( gentity_t *player, gentity_t *ent, cast_state_t *cs, int reason ) {
	int next = VrTac_StateAfterManeuver( merc.order, vr_merc_assist.integer != 0, VrMerc_HaveTargetNow( ent, cs ) );

	if ( merc.order == VRMERC_ORDER_HOLD ) {
		VrMerc_ReapplyOrder( player, cs );          // walk back to the post
	} else if ( next == VRMERC_ENGAGE && VrMerc_HostileAlive( ent, cs->enemyNum ) ) {
		AIFunc_DefaultStart( cs );                  // continue the fight with the stock AI
	} else {
		VrMerc_ReapplyOrder( player, cs );
	}
	VrMerc_SetState( next, reason );
	merc.stateChanges++;
}

// Ends whatever maneuver is running and puts the companion back on its standing order.
static void VrMerc_CancelManeuver( gentity_t *player, gentity_t *ent, cast_state_t *cs, int reason ) {
	if ( merc.man.kind == MAN_NONE ) {
		return;
	}
	if ( VrMerc_StateIsManeuver( merc.state ) ) {
		VrMerc_ManeuverEnd( player, cs, reason, qfalse );
		VrMerc_AfterManeuver( player, ent, cs, reason );
	} else {
		VrMerc_ManeuverEnd( player, cs, reason, qtrue );
	}
}

/*
===============
Cover
===============
*/
static void VrMerc_FillCoverCand( gentity_t *player, gentity_t *ent, cast_state_t *cs, vec3_t pos,
								  vec3_t threatPos, qboolean crouch, vrTacCoverCand_t *c, vec3_t outPos ) {
	int why = VRTAC_OK, travel;

	memset( c, 0, sizeof( *c ) );
	c->holdOrder = ( merc.order == VRMERC_ORDER_HOLD );
	if ( crouch ) {
		// the stock check already proved that a crouching companion is hidden where it stands
		VectorCopy( ent->r.currentOrigin, outPos );
		c->crouchHere = 1;
		c->reachable = 1;
		c->travelTime = 1;
		c->concealed = 1;
	} else {
		travel = VrMerc_NavCheck( player, ent, cs, pos, outPos, &why );
		c->insideSolid = ( why == VRTAC_R_NAV_SOLID ) ? 1 : 0;
		c->reachable = ( travel > 0 ) ? 1 : 0;
		c->travelTime = travel > 0 ? travel : 0;
		if ( c->insideSolid || !c->reachable ) {
			return;     // no point spending a visibility trace on an unusable spot
		}
		c->concealed = AICast_VisibleFromPos( threatPos, merc.target, outPos, ent->s.number, qfalse ) ? 0 : 1;
	}
	c->distFromSelf = Distance( ent->r.currentOrigin, outPos );
	c->distFromPlayer = Distance( player->r.currentOrigin, outPos );
	c->distFromHold = Distance( merc.holdPos, outPos );
	c->threatCloser = Distance( ent->r.currentOrigin, threatPos ) - Distance( outPos, threatPos );
	{
		float t;

		c->blocksPlayer = ( c->distFromPlayer < VRTAC_COVER_PLAYER_CLEAR ||
							VrTac_PointSegmentDist( player->r.currentOrigin, threatPos, outPos, &t ) < VRTAC_COVER_LINE_CLEAR ) ? 1 : 0;
	}
}

static qboolean VrMerc_TryCover( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacCoverWant_t w;
	int reason, k, idx, bestK = -1, cnt = 0, lastReject = VRTAC_R_COVER_NONE;
	int now = level.time;
	vec3_t threatPos, self, positions[10], stock;
	vrTacCoverCand_t cc[10];
	float scores[10];
	qboolean crouchPick = qfalse;
	static const float ringDeg[8] = { 0, 45, -45, 90, -90, 135, -135, 180 };

	idx = VrMerc_CommittedTargetIndex();
	memset( &w, 0, sizeof( w ) );
	w.state = merc.state;
	w.order = merc.order;
	w.stance = merc.stance;
	w.assist = vr_merc_assist.integer != 0;
	w.scriptBusy = VrMerc_ScriptBusy( cs );
	w.haveTarget = ( idx >= 0 ) ? 1 : 0;
	w.targetVisible = ( idx >= 0 && mtx.cand[idx].visibleNow ) ? 1 : 0;
	w.targetDist = ( idx >= 0 ) ? mtx.cand[idx].dist : 99999.0f;
	w.sustainedFire = mtx.sustained;
	w.heavyDamage = mtx.heavy;
	w.healthFrac = mtx.healthFrac;
	w.cooldownOk = ( now >= merc.coverCooldownUntil ) ? 1 : 0;
	w.maneuverActive = ( merc.man.kind != MAN_NONE ) ? 1 : 0;
	w.outOfAmmo = AICast_GotEnoughAmmoForWeapon( cs, cs->weaponNum ) ? 0 : 1;
	{
		int h, tot;

		VrTac_DmgWindow( &merc.dmgSelf, now, 4000, &h, &tot );
		w.recentDamage = ( h >= 1 ) ? 1 : 0;
	}
	if ( !VrTac_WantCover( &w, &reason ) || now < merc.nextCoverSearch ) {
		return qfalse;
	}
	merc.nextCoverSearch = now + VRTAC_COVER_SEARCH_MS;       // trace budget: one search per interval

	VectorCopy( cs->vislist[merc.target].visible_pos, threatPos );      // the companion's own memory
	if ( VectorLength( threatPos ) < 1.0f ) {
		VectorCopy( g_entities[merc.target].r.currentOrigin, threatPos );
	}
	VectorCopy( ent->r.currentOrigin, self );

	// 1. the engine's own choice: crouch where we stand, or an AAS route-table hiding spot
	if ( AICast_GetTakeCoverPos( cs, merc.target, threatPos, stock ) ) {
		VrMerc_FillCoverCand( player, ent, cs, stock, threatPos, cs->crouchHideFlag ? qtrue : qfalse, &cc[cnt], positions[cnt] );
		cnt++;
	}
	// 2. a small ring of local samples, starting away from the threat
	{
		float baseYaw = atan2f( self[1] - threatPos[1], self[0] - threatPos[0] );

		for ( k = 0; k < 8 && cnt < 10; k++ ) {
			vec3_t p;
			float a = baseYaw + ringDeg[k] * 3.14159265f / 180.0f;

			p[0] = self[0] + cosf( a ) * 250.0f;
			p[1] = self[1] + sinf( a ) * 250.0f;
			p[2] = self[2];
			VrMerc_FillCoverCand( player, ent, cs, p, threatPos, qfalse, &cc[cnt], positions[cnt] );
			cnt++;
		}
	}
	for ( k = 0; k < cnt; k++ ) {
		int rej = VrTac_CoverReject( &cc[k] );

		scores[k] = VrTac_CoverScore( &cc[k] );
		if ( rej != VRTAC_OK ) {
			lastReject = rej;
			VrTac_Log( 2, "cover_rejected", "cand=%d reason=%s dist=%.0f", k, VrTac_ReasonName( rej ), cc[k].distFromSelf );
		}
	}
	bestK = VrTac_PickBest( scores, cnt );
	if ( bestK < 0 ) {
		// no usable cover: keep fighting with the stock behaviour, try again later
		merc.coverFails++;
		merc.coverCooldownUntil = now + VRTAC_COVER_FAIL_COOLDOWN;
		VrTac_Log( 1, "cover_none", "candidates=%d last_reject=%s trigger=%s", cnt, VrTac_ReasonName( lastReject ),
				   VrTac_ReasonName( reason ) );
		return qfalse;
	}
	crouchPick = cc[bestK].crouchHere ? qtrue : qfalse;

	if ( cs->enemyNum != merc.target ) {
		VrMerc_SetStockTarget( ent, cs, merc.target );
	}
	VectorCopy( positions[bestK], cs->takeCoverPos );
	cs->crouchHideFlag = crouchPick;
	cs->takeCoverTime = now + 3500;
	AIFunc_BattleTakeCoverStart( cs );

	VrMerc_ManeuverMark( MAN_COVER, positions[bestK], crouchPick ? 0.0f : 40.0f, VRTAC_COVER_MAX_MS, reason, ent );
	merc.manArrivedAt = crouchPick ? now : 0;
	merc.manTarget = merc.target;
	VrMerc_SetState( VRMERC_COVER, reason );
	merc.stateChanges++;
	VrTac_Log( 1, "cover", "trigger=%s kind=%s dist=%.0f travel=%d score=%.0f candidates=%d target=%d",
			   VrTac_ReasonName( reason ), crouchPick ? "crouch" : "move", cc[bestK].distFromSelf,
			   cc[bestK].travelTime, scores[bestK], cnt, merc.target );
	return qtrue;
}

static void VrMerc_CoverStep( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacCoverState_t st;
	int now = level.time, why;
	float dist;
	vec3_t d;

	VectorSubtract( ent->r.currentOrigin, merc.man.pos, d );
	d[2] = 0;
	dist = VectorLength( d );
	if ( !merc.manArrivedAt && dist <= merc.man.reach + 24.0f ) {
		merc.manArrivedAt = now;
	}
	memset( &st, 0, sizeof( st ) );
	st.msInState = now - merc.man.startedAt;
	st.arrived = merc.manArrivedAt ? 1 : 0;
	st.msSinceArrival = merc.manArrivedAt ? now - merc.manArrivedAt : 0;
	st.stalled = ( !merc.manArrivedAt && VrMerc_ProgressStalled( &merc.man.progress, dist, now, 24.0f, VRTAC_COVER_STALL_MS ) ) ? 1 : 0;
	st.scriptBusy = VrMerc_ScriptBusy( cs );
	st.msSinceTarget = now - merc.lastTargetTime;
	st.msSinceDamage = now - merc.hurtSelfAt;
	why = VrTac_CoverExit( &st );
	if ( why == VRTAC_OK ) {
		return;
	}
	if ( why == VRTAC_R_NAV_STALL ) {
		merc.coverFails++;
		merc.coverCooldownUntil = now + VRTAC_COVER_FAIL_COOLDOWN;
		VrTac_Log( 1, "nav_failure", "maneuver=cover dist=%.0f", dist );
	} else {
		merc.coverCooldownUntil = now + VRTAC_COVER_DONE_COOLDOWN;
		merc.coverFails = 0;
	}
	VrMerc_ManeuverEnd( player, cs, why, qfalse );
	VrMerc_AfterManeuver( player, ent, cs, why );
}

/*
===============
Flank
===============
*/
static qboolean VrMerc_TryFlank( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacFlankWant_t w;
	int idx = VrMerc_CommittedTargetIndex(), trigger, why, side, ang, k, cnt = 0, best = -1;
	int now = level.time;
	vec3_t enemyPos, self, pos[4];
	vrTacFlankCand_t cc[4];
	float score[4];
	static const float angles[2] = { VRTAC_FLANK_ANGLE_DEG, 100.0f };

	if ( merc.flankFails && merc.lastFlankFailAt && now - merc.lastFlankFailAt > VRTAC_FLANK_FAILS_RESET_MS ) {
		merc.flankFails = 0;
	}
	memset( &w, 0, sizeof( w ) );
	w.state = merc.state;
	w.order = merc.order;
	w.stance = merc.stance;
	w.assist = vr_merc_assist.integer != 0;
	w.scriptBusy = VrMerc_ScriptBusy( cs );
	w.coverMeActive = ( now < merc.coverMeUntil ) ? 1 : 0;
	w.haveTarget = ( idx >= 0 && mtx.cand[idx].msSinceSeen <= 3000 ) ? 1 : 0;
	w.targetDist = ( idx >= 0 ) ? mtx.cand[idx].dist : 0.0f;
	w.healthFrac = mtx.healthFrac;
	w.msNoShot = merc.noShotSince ? now - merc.noShotSince : 0;
	w.msEngaged = now - merc.targetSince;
	w.msSinceFlank = merc.lastFlankAt ? now - merc.lastFlankAt : 1000000;
	w.flankFails = merc.flankFails;
	w.maneuverActive = ( merc.man.kind != MAN_NONE ) ? 1 : 0;
	if ( merc.move.mode == VRMOVE_CATCHUP ) {
		return qfalse;      // catching up has priority over repositioning
	}
	why = VrTac_WantFlank( &w, &trigger );
	if ( why != VRTAC_OK ) {
		if ( why != VRTAC_R_FLANK_NONE ) {
			VrTac_Log( 2, "flank_rejected", "reason=%s", VrTac_ReasonName( why ) );
		}
		return qfalse;
	}
	if ( now < merc.nextFlankSearch ) {
		return qfalse;
	}
	merc.nextFlankSearch = now + 2000;

	VectorCopy( mtx.cand[idx].visibleNow ? g_entities[merc.target].r.currentOrigin : cs->vislist[merc.target].visible_pos, enemyPos );
	VectorCopy( ent->r.currentOrigin, self );

	for ( ang = 0; ang < 2; ang++ ) {
		for ( side = -1; side <= 1; side += 2 ) {
			vec3_t p, floor, eye, chest;
			trace_t tr;
			int nav, nwhy;
			vrTacFlankCand_t *c = &cc[cnt];

			VrTac_FlankPoint( self, enemyPos, side, angles[ang], p );
			memset( c, 0, sizeof( *c ) );
			nav = VrMerc_NavCheck( player, ent, cs, p, floor, &nwhy );
			c->insideSolid = ( nwhy == VRTAC_R_NAV_SOLID ) ? 1 : 0;
			c->reachable = ( nav > 0 ) ? 1 : 0;
			c->travelTime = nav > 0 ? nav : 0;
			VectorCopy( floor, pos[cnt] );
			if ( nav > 0 ) {
				c->straightDist = Distance( self, floor );
				c->distToEnemy = Distance( floor, enemyPos );
				c->crossesFireLine = VrTac_PathCrossesFireLine( self, floor, player->r.currentOrigin, enemyPos,
																VRTAC_FLANK_LINE_CLEAR );
				VectorCopy( floor, eye );
				eye[2] += 20;
				VectorCopy( enemyPos, chest );
				chest[2] += 20;
				trap_Trace( &tr, eye, NULL, NULL, chest, ent->s.number, MASK_SHOT );
				c->shotFromPos = ( tr.fraction >= 0.97f || tr.entityNum == merc.target ) ? 1 : 0;
			}
			score[cnt] = VrTac_FlankScore( c );
			if ( score[cnt] < 0.0f ) {
				VrTac_Log( 2, "flank_rejected", "cand=%d side=%d reason=%s", cnt, side, VrTac_ReasonName( VrTac_FlankReject( c ) ) );
			}
			cnt++;
		}
	}
	for ( k = 0; k < cnt; k++ ) {
		if ( score[k] > 0.0f && ( best < 0 || score[k] > score[best] ) ) {
			best = k;
		}
	}
	merc.lastFlankAt = now;       // starts the cooldown whether or not a flank was found
	if ( best < 0 ) {
		merc.flankFails++;
		merc.lastFlankFailAt = now;
		merc.lastFlankAt = now - VRTAC_FLANK_COOLDOWN_MS + VRTAC_FLANK_FAIL_COOLDOWN;
		VrTac_Log( 1, "flank_cancelled", "reason=%s candidates=%d fails=%d", VrTac_ReasonName( VRTAC_R_FLANK_NONE ), cnt, merc.flankFails );
		return qfalse;
	}
	if ( !VrMerc_ManeuverBegin( ent, cs, MAN_FLANK, pos[best], 48.0f, VRTAC_FLANK_MAX_MS, trigger ) ) {
		return qfalse;
	}
	merc.manArrivedAt = 0;
	merc.manTarget = merc.target;
	VrMerc_SetState( VRMERC_FLANK, trigger );
	merc.stateChanges++;
	VrTac_Log( 1, "flank", "trigger=%s target=%d travel=%d dist_to_enemy=%.0f no_shot_ms=%d", VrTac_ReasonName( trigger ),
			   merc.target, cc[best].travelTime, cc[best].distToEnemy, w.msNoShot );
	return qtrue;
}

static void VrMerc_FlankStep( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacFlankState_t f;
	int now = level.time, why, prog;

	prog = VrMerc_ManeuverProgress( ent );
	if ( prog == VRTAC_R_DONE && !merc.manArrivedAt ) {
		merc.manArrivedAt = now;
	}
	memset( &f, 0, sizeof( f ) );
	f.scriptBusy = VrMerc_ScriptBusy( cs );
	f.targetValid = VrMerc_HostileAlive( ent, merc.manTarget ) ? 1 : 0;
	f.healthFrac = mtx.healthFrac;
	f.progress = merc.manArrivedAt ? VRTAC_R_DONE : prog;
	f.msSinceArrival = merc.manArrivedAt ? now - merc.manArrivedAt : -1;
	why = VrTac_FlankExit( &f );
	if ( why == VRTAC_OK ) {
		return;
	}
	if ( why == VRTAC_R_NAV_STALL || why == VRTAC_R_TIMEOUT ) {
		merc.flankFails++;
		merc.lastFlankFailAt = now;
		VrTac_Log( 1, "nav_failure", "maneuver=flank reason=%s", VrTac_ReasonName( why ) );
		VrTac_Log( 1, "flank_cancelled", "reason=%s fails=%d", VrTac_ReasonName( why ), merc.flankFails );
	} else if ( why == VRTAC_R_DONE ) {
		merc.flankFails = 0;
	} else {
		VrTac_Log( 1, "flank_cancelled", "reason=%s fails=%d", VrTac_ReasonName( why ), merc.flankFails );
	}
	VrMerc_ManeuverEnd( player, cs, why, qfalse );      // frees the goal entity
	VrMerc_AfterManeuver( player, ent, cs, why );
}

/*
===============
Regroup / retreat: go back to the player through the stock chase (the same call the M3 RETURN
state uses), with a hard time limit and a progress watchdog.
===============
*/
static qboolean VrMerc_StartRegroup( gentity_t *player, gentity_t *ent, cast_state_t *cs, int reason ) {
	if ( merc.man.kind != MAN_NONE ) {
		VrMerc_ManeuverEnd( player, cs, VRTAC_R_ORDER_CHANGED, qfalse );
	}
	if ( merc.order == VRMERC_ORDER_HOLD ) {
		// a safety retreat (or an explicit command) ends the post: the companion now stays with the player
		// (otherwise it would walk straight back into the fire that made it retreat)
		VrMerc_FreeHoldGoal( cs );
		merc.order = VRMERC_ORDER_FOLLOW;
		VrMerc_Say( "Mercenary: falling back to you" );
	}
	cs->followEntity = -1;
	cs->leaderNum = player->s.number;
	AIFunc_ChaseGoalStart( cs, player->s.number, 160, qtrue );

	VrMerc_ManeuverMark( MAN_REGROUP, player->r.currentOrigin, 160.0f, VRTAC_REGROUP_MAX_MS, reason, ent );
	merc.manTarget = -1;
	VrMerc_SetState( VRMERC_REGROUP, reason );
	merc.stateChanges++;
	VrTac_Log( 1, "merc_regroup", "trigger=%s dist_to_player=%.0f health=%.2f", VrTac_ReasonName( reason ),
			   Distance( ent->r.currentOrigin, player->r.currentOrigin ), mtx.healthFrac );
	return qtrue;
}

static qboolean VrMerc_TryRegroup( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacRegroupWant_t w;
	int reason;

	memset( &w, 0, sizeof( w ) );
	w.state = merc.state;
	w.order = merc.order;
	w.assist = vr_merc_assist.integer != 0;
	w.scriptBusy = VrMerc_ScriptBusy( cs );
	w.regroupRequested = 0;     // commands start a regroup directly (VrMerc_ClientCommand)
	w.haveTarget = VrMerc_HaveTargetNow( ent, cs ) ? 1 : 0;
	w.sustainedFire = mtx.sustained;
	w.heavyDamage = mtx.heavy;
	w.healthFrac = mtx.healthFrac;
	w.distToPlayer = Distance( ent->r.currentOrigin, player->r.currentOrigin );
	w.msSinceRegroup = merc.regroupEndAt ? level.time - merc.regroupEndAt : 1000000;
	w.maneuverActive = ( merc.man.kind != MAN_NONE ) ? 1 : 0;
	if ( !VrTac_WantRegroup( &w, &reason ) ) {
		return qfalse;
	}
	return VrMerc_StartRegroup( player, ent, cs, reason );
}

static void VrMerc_RegroupStep( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrTacRegroupState_t r;
	int now = level.time, why;
	float dist = Distance( ent->r.currentOrigin, player->r.currentOrigin );

	memset( &r, 0, sizeof( r ) );
	r.scriptBusy = VrMerc_ScriptBusy( cs );
	r.distToPlayer = dist;
	r.progress = VRTAC_OK;
	if ( now - merc.man.startedAt >= merc.man.maxMs ) {
		r.progress = VRTAC_R_TIMEOUT;
	} else if ( VrMerc_ProgressStalled( &merc.man.progress, dist, now, 48.0f, VRTAC_REGROUP_STALL_MS ) ) {
		r.progress = VRTAC_R_NAV_STALL;
	}
	why = VrTac_RegroupExit( &r );
	if ( why == VRTAC_OK ) {
		return;
	}
	if ( why == VRTAC_R_NAV_STALL || why == VRTAC_R_TIMEOUT ) {
		VrTac_Log( 1, "nav_failure", "maneuver=regroup reason=%s dist=%.0f", VrTac_ReasonName( why ), dist );
	}
	merc.regroupEndAt = now;       // starts the cooldown that prevents retreat / engage loops
	VrMerc_ManeuverEnd( player, cs, why, qfalse );
	VrMerc_AfterManeuver( player, ent, cs, why );
}

/*
===============
Adaptive following (policy in g_vrmove.h)

Moves the companion between NEAR (stock behaviour), RUN and CATCHUP using the AAS path
distance, so a companion that is only a few metres away in a straight line but around a corner
is not forgotten.  RUN and CATCHUP point the stock chase at the player (no slow approach) and
raise the character's speedScale floor through the hook in ai_cast_think.c; leaving them hands
control back to the standing order.  A stuck companion goes up a bounded recovery ladder:
repath, then the player's recent walkable trail, then a short wait.  No teleport.
===============
*/
// Called by the AI think code (ai_cast_think.c): speedScale the character is guaranteed, or 0.
float VrMerc_SpeedScaleFloor( int entNum, float runSpeed ) {
	if ( entNum < 0 || entNum >= MAX_CLIENTS || !mercMoveMode[entNum] ) {
		return 0.0f;
	}
	return VrMove_SpeedFloor( mercMoveMode[entNum], runSpeed );
}

static const char *VrMove_ModeName( int mode ) {
	return mode == VRMOVE_CATCHUP ? "catchup" : ( mode == VRMOVE_RUN ? "run" : "near" );
}

static void VrMerc_PointAtPlayer( gentity_t *player, cast_state_t *cs, int mode ) {
	cs->leaderNum = player->s.number;
	cs->followEntity = -1;
	AIFunc_ChaseGoalStart( cs, player->s.number, VrMove_ChaseReach( mode ), mode == VRMOVE_RUN ? qtrue : qfalse );
	merc.moveChaseAt = level.time;
	merc.moveForced = 1;
}

static void VrMerc_MoveRelease( gentity_t *player, cast_state_t *cs, qboolean restore ) {
	if ( merc.slot >= 0 && merc.slot < MAX_CLIENTS ) {
		mercMoveMode[merc.slot] = VRMOVE_NEAR;
	}
	if ( merc.moveForced && restore && cs && player ) {
		VrMerc_ReapplyOrder( player, cs );
	}
	merc.moveForced = 0;
	merc.move.mode = VRMOVE_NEAR;
	merc.move.since = level.time;
}

// Straight-line / path metrics, refreshed every 250 ms (one trace, two AAS queries at most).
static void VrMerc_MoveMetrics( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vec3_t a, b;
	trace_t tr;
	int playerArea, travel;
	float euclid = Distance( ent->r.currentOrigin, player->r.currentOrigin );

	merc.moveNextMetrics = level.time + VRTAC_PERCEPTION_MS;

	VectorCopy( ent->r.currentOrigin, a );
	VectorCopy( player->r.currentOrigin, b );
	a[2] += 24;
	b[2] += 24;
	trap_Trace( &tr, a, NULL, NULL, b, ent->s.number, MASK_PLAYERSOLID );
	// another character in the way is not a wall
	merc.moveLOS = ( tr.fraction >= 0.99f || tr.entityNum == player->s.number || tr.entityNum < MAX_CLIENTS ) ? 1 : 0;

	merc.movePathDist = -1.0f;
	if ( !cs->bs || cs->bs->areanum <= 0 ) {
		return;
	}
	trap_AAS_SetCurrentWorld( cs->aasWorldIndex );
	playerArea = trap_AAS_PointAreaNum( player->r.currentOrigin );
	if ( playerArea > 0 ) {
		merc.moveLastPlayerArea = playerArea;
		VectorCopy( player->r.currentOrigin, merc.moveLastPlayerAreaPos );
	} else {
		playerArea = merc.moveLastPlayerArea;       // the player stands off the navigation mesh: use the last good spot
	}
	if ( playerArea <= 0 ) {
		return;
	}
	if ( playerArea == cs->bs->areanum ) {
		merc.movePathDist = VrMove_PathDistFromTravel( 0, 1, euclid );
		return;
	}
	travel = trap_AAS_AreaTravelTimeToGoalArea( cs->bs->areanum, ent->r.currentOrigin, playerArea, cs->travelflags );
	merc.movePathDist = VrMove_PathDistFromTravel( travel, 0, euclid );
}

// Walk to a reachable point on the player's recent path (recovery ladder step 2).
static qboolean VrMerc_StartTrailWalk( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	int travelTime[VRTRAIL_MAX], farEnough[VRTRAIL_MAX], k, pick, idx;
	vec3_t pos;

	memset( travelTime, 0, sizeof( travelTime ) );
	memset( farEnough, 0, sizeof( farEnough ) );
	if ( !cs->bs || cs->bs->areanum <= 0 || merc.man.kind != MAN_NONE ) {
		return qfalse;
	}
	trap_AAS_SetCurrentWorld( cs->aasWorldIndex );
	for ( k = 0; k < merc.trail.count && k < 6; k++ ) {       // at most 6 AAS queries per step
		int area;

		idx = VrTrail_Index( &merc.trail, k );
		VectorCopy( merc.trail.pos[idx], pos );
		area = trap_AAS_PointAreaNum( pos );
		if ( area > 0 ) {
			travelTime[k] = ( area == cs->bs->areanum ) ? 1 :
							trap_AAS_AreaTravelTimeToGoalArea( cs->bs->areanum, ent->r.currentOrigin, area, cs->travelflags );
		}
		farEnough[k] = ( Distance( ent->r.currentOrigin, pos ) > 200.0f ) ? 1 : 0;
	}
	pick = VrTrail_Pick( &merc.trail, level.time, travelTime, farEnough );
	if ( pick < 0 ) {
		return qfalse;
	}
	idx = VrTrail_Index( &merc.trail, pick );
	VectorCopy( merc.trail.pos[idx], pos );
	if ( !VrMerc_ManeuverBegin( ent, cs, MAN_TRAIL, pos, 90.0f, 7000, VRTAC_R_NAV_STALL ) ) {
		return qfalse;
	}
	merc.moveForced = 0;
	return qtrue;
}

static void VrMerc_FollowTick( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	vrMoveIn_t in;
	int now = level.time, prev = merc.move.mode, mode, action;
	float d;
	qboolean managed;

	if ( !vr_merc_move.integer ) {
		if ( merc.moveForced || mercMoveMode[merc.slot] ) {
			VrMerc_MoveRelease( player, cs, qtrue );
		}
		return;
	}
	VrTrail_Add( &merc.trail, player->r.currentOrigin, now );
	if ( now >= merc.moveNextMetrics ) {
		VrMerc_MoveMetrics( player, ent, cs );
	}
	d = Distance( ent->r.currentOrigin, player->r.currentOrigin );

	// A regroup / retreat is a forced run to the player: it only needs the speed floor.
	if ( merc.state == VRMERC_REGROUP ) {
		mercMoveMode[merc.slot] = VRMOVE_CATCHUP;
		return;
	}
	managed = ( merc.order == VRMERC_ORDER_FOLLOW &&
				( merc.state == VRMERC_FOLLOW || merc.state == VRMERC_ENGAGE ) &&
				merc.man.kind == MAN_NONE && !VrMerc_ScriptBusy( cs ) ) ? qtrue : qfalse;

	if ( !managed ) {
		// a maneuver, a HOLD order, a script or another state owns the AI function now: only forget our own
		// bookkeeping, never call the stock AI (that would overwrite the other owner's function)
		if ( merc.moveForced || mercMoveMode[merc.slot] ) {
			merc.moveForced = 0;
			mercMoveMode[merc.slot] = VRMOVE_NEAR;
			merc.move.mode = VRMOVE_NEAR;
			merc.move.since = now;
		}
		VrRecover_Step( &merc.rec, ent->r.currentOrigin, 0, now );     // keeps the observation window fresh
		return;
	}

	memset( &in, 0, sizeof( in ) );
	in.euclid = d;
	in.pathDist = merc.movePathDist;
	in.hasLOS = merc.moveLOS;
	in.playerSpeed = sqrtf( player->client->ps.velocity[0] * player->client->ps.velocity[0] +
							player->client->ps.velocity[1] * player->client->ps.velocity[1] );
	// combat owns the movement while the companion is under fire, or fighting close to the player
	in.tight = ( now < merc.coverMeUntil ) ? 1 : 0;         // "cover me": stay closer to the player
	in.suppressed = ( mtx.sustained || mtx.heavy || merc.rec.waitUntil > now ||
					  ( merc.state == VRMERC_ENGAGE && d < ( in.tight ? 300.0f : 420.0f ) ) ) ? 1 : 0;

	mode = VrMove_Update( &merc.move, &in, now );
	mercMoveMode[merc.slot] = (unsigned char)mode;

	if ( mode != prev ) {
		VrMerc_Log( 1, "merc_follow_distance", "mode=%s from=%s straight=%.0f path=%.0f los=%d player_speed=%.0f",
					VrMove_ModeName( mode ), VrMove_ModeName( prev ), d, merc.movePathDist, merc.moveLOS, in.playerSpeed );
		if ( mode == VRMOVE_CATCHUP ) {
			VrMerc_Log( 1, "merc_catchup_started", "straight=%.0f path=%.0f los=%d", d, merc.movePathDist, merc.moveLOS );
		}
	}
	if ( VrMove_ForcesChase( mode ) ) {
		// point the AI at the player when entering, when the reach changes, and every few seconds
		if ( mode != prev || !merc.moveForced || now - merc.moveChaseAt >= 4000 ) {
			VrMerc_PointAtPlayer( player, cs, mode );
		}
	} else if ( merc.moveForced ) {
		merc.moveForced = 0;
		VrMerc_ReapplyOrder( player, cs );              // back to the stock follow / formation logic
	}

	// stuck detection and the bounded recovery ladder
	action = VrRecover_Step( &merc.rec, ent->r.currentOrigin, ( merc.moveForced && d > 200.0f ) ? 1 : 0, now );
	switch ( action ) {
	case VRREC_REPATH:
		trap_BotResetAvoidReach( cs->bs->ms );          // forget the areas the AI decided to avoid, then ask again
		VrMerc_PointAtPlayer( player, cs, mode );
		VrMerc_Log( 1, "merc_navigation_stuck", "attempt=1 straight=%.0f path=%.0f", d, merc.movePathDist );
		VrMerc_Log( 1, "merc_repath", "attempt=1 method=reset_avoid");
		break;
	case VRREC_TRAIL:
		VrMerc_Log( 1, "merc_navigation_stuck", "attempt=2 straight=%.0f path=%.0f", d, merc.movePathDist );
		trap_BotResetAvoidReach( cs->bs->ms );
		if ( VrMerc_StartTrailWalk( player, ent, cs ) ) {
			VrMerc_Log( 1, "merc_repath", "attempt=2 method=trail crumbs=%d", merc.trail.count );
		} else {
			VrMerc_Log( 1, "merc_repath", "attempt=2 method=trail result=no_crumb" );
		}
		break;
	case VRREC_WAIT:
		VrMerc_Log( 1, "merc_navigation_stuck", "attempt=3 straight=%.0f path=%.0f wait_ms=%d", d, merc.movePathDist, VRMOVE_RECOVER_WAIT_MS );
		VrMerc_MoveRelease( player, cs, qtrue );        // stand down; the episode restarts after the wait
		break;
	default:
		break;
	}
}

/*
===============
VrMerc_Tactics

Per-tick tactical decisions while the base state machine has no transition to make.
Priority: running maneuver > regroup/retreat > cover > flank > formation.
===============
*/
static void VrMerc_Tactics( gentity_t *player, gentity_t *ent, cast_state_t *cs ) {
	int now = level.time;

	if ( merc.coverMeUntil && now >= merc.coverMeUntil ) {
		merc.coverMeUntil = 0;
		VrTac_Log( 1, "cover_me_end", "reason=timeout" );
	}
	// consistency guard: a maneuver state always has its maneuver record
	if ( VrMerc_StateIsManeuver( merc.state ) && merc.man.kind == MAN_NONE ) {
		VrTac_Log( 1, "recover", "state=%s reason=no_maneuver", VrMerc_StateName( merc.state ) );
		VrMerc_AfterManeuver( player, ent, cs, VRTAC_R_DONE );
		return;
	}
	switch ( merc.man.kind ) {
	case MAN_REPOSITION: {
		int why = VRTAC_OK;

		if ( VrMerc_ScriptBusy( cs ) ) {
			why = VRTAC_R_SCRIPT;
		} else if ( merc.target >= 0 || cs->enemyNum >= 0 ) {
			why = VRTAC_R_TARGET_SEEN;
		} else {
			why = VrMerc_ManeuverProgress( ent );
		}
		if ( why != VRTAC_OK ) {
			if ( why == VRTAC_R_NAV_STALL ) {
				VrTac_Log( 1, "nav_failure", "maneuver=reposition dist=%.0f", Distance( ent->r.currentOrigin, merc.man.pos ) );
			}
			VrMerc_ManeuverEnd( player, cs, why, qtrue );
		}
		return;
	}
	case MAN_TRAIL: {
		int why = VRTAC_OK;

		if ( VrMerc_ScriptBusy( cs ) ) {
			why = VRTAC_R_SCRIPT;
		} else {
			why = VrMerc_ManeuverProgress( ent );
		}
		if ( why != VRTAC_OK ) {
			VrMerc_ManeuverEnd( player, cs, why, qtrue );       // then the follow policy points the AI at the player again
			merc.move.since = level.time - VRMOVE_MIN_DWELL_MS;
		}
		return;
	}
	case MAN_COVER:
		VrMerc_CoverStep( player, ent, cs );
		return;
	case MAN_FLANK:
		VrMerc_FlankStep( player, ent, cs );
		return;
	case MAN_REGROUP:
		VrMerc_RegroupStep( player, ent, cs );
		return;
	default:
		break;
	}
	if ( !vr_merc_assist.integer ) {
		VrMerc_Formation( player, ent, cs );
		return;
	}
	if ( VrMerc_TryRegroup( player, ent, cs ) || VrMerc_TryCover( player, ent, cs ) || VrMerc_TryFlank( player, ent, cs ) ) {
		return;
	}
	VrMerc_Formation( player, ent, cs );
}

/*
===============
VrMerc_Supervise

Runs every VRMERC_THINK_MS for an existing companion.
===============
*/
static void VrMerc_Supervise( gentity_t *player ) {
	gentity_t *ent = VrMerc_Entity();
	cast_state_t *cs;
	vrMercInput_t in;
	vrMercResult_t res;
	vec3_t selfPos;
	int enemy;
	int now = level.time;
	int stalled = 0;
	float distToTarget;
	int prev = merc.state;

	if ( !ent || ent->health <= 0 ) {
		VrMerc_Finish( VRMERC_DEAD );
		return;
	}

	cs = AICast_GetCastState( ent->s.number );
	merc.supervisedFrames++;
	VectorCopy( ent->r.currentOrigin, selfPos );

	// ---- tactical layer: stance, perception (throttled), friendly-fire avoidance ----
	{
		int stance = vr_merc_stance.integer ? VRTAC_STANCE_AGGRESSIVE : VRTAC_STANCE_DEFENSIVE;

		if ( stance != merc.stance ) {
			VrTac_Log( 1, "stance", "to=%s", stance == VRTAC_STANCE_AGGRESSIVE ? "aggressive" : "defensive" );
			merc.stance = stance;
		}
	}
	if ( now >= merc.nextPerception ) {
		merc.nextPerception = now + VRTAC_PERCEPTION_MS;
		VrMerc_Perceive( player, ent, cs );
	}
	VrMerc_AvoidFriendlyFire( player, ent, cs );
	VrMerc_FollowTick( player, ent, cs );

	memset( &in, 0, sizeof( in ) );
	in.alive = 1;
	in.dismissRequested = merc.dismissRequested;
	in.order = merc.order;
	in.assist = ( vr_merc_assist.integer != 0 );
	in.returnAllowed = ( now >= merc.returnAllowedAt );
	in.distToPlayer = Distance( selfPos, player->r.currentOrigin );
	if ( vr_merc_move.integer && merc.order == VRMERC_ORDER_FOLLOW ) {
		in.distToPlayer = 0.0f;     // the adaptive follow policy owns "too far"; the M3 RETURN state is for HOLD only
	}
	in.distToHold = Distance( selfPos, merc.holdPos );

	// Friend-or-foe: the stock AI picks its target; make sure it never is the
	// player, a companion, an ally or a neutral story NPC.
	enemy = cs->enemyNum;
	if ( enemy >= 0 && enemy < MAX_GENTITIES ) {
		gentity_t *t = &g_entities[enemy];

		if ( VrMerc_IsValidHostile( ent->aiTeam, t->aiTeam, AITEAM_NEUTRAL,
									enemy == player->s.number, VrMerc_IsCompanion( t ),
									t->inuse && t->health > 0 ) ) {
			in.haveTarget = 1;
		} else if ( t->inuse && t->health > 0 ) {
			cs->enemyNum = -1;      // dead targets are cleared by the AI itself; this clears friends
		}
	}
	if ( in.haveTarget ) {
		merc.lastTargetTime = now;
		in.msSinceTarget = 0;
	} else {
		in.msSinceTarget = now - merc.lastTargetTime;
	}

	distToTarget = ( merc.order == VRMERC_ORDER_HOLD ) ? in.distToHold : in.distToPlayer;
	if ( merc.state == VRMERC_RETURN ) {
		stalled = VrMerc_ProgressStalled( &merc.progress, distToTarget, now,
										  VRMERC_RETURN_MIN_GAIN, VRMERC_RETURN_STALL_MS );
	}

	res = VrMerc_NextState( merc.state, &in, stalled );
	if ( res.state == prev ) {
		VrMerc_Tactics( player, ent, cs );
		return;
	}

	// ---- state entry actions ----
	merc.stateChanges++;
	if ( merc.man.kind == MAN_REPOSITION ) {
		VrMerc_ManeuverEnd( player, cs, VRTAC_R_ORDER_CHANGED, qfalse );    // the new state points the AI itself
	}
	switch ( res.state ) {
	case VRMERC_DEAD:
	case VRMERC_DISMISSED:
		VrMerc_Finish( res.state );
		return;

	case VRMERC_RETURN:
		VrMerc_ProgressReset( &merc.progress, distToTarget, now );
		if ( merc.order == VRMERC_ORDER_HOLD ) {
			if ( merc.holdGoal < 0 ) {
				VrMerc_AnchorAt( ent, cs, merc.holdPos );
			} else {
				AIFunc_ChaseGoalStart( cs, merc.holdGoal, 96, qtrue );
			}
		} else {
			cs->leaderNum = player->s.number;
			AIFunc_ChaseGoalStart( cs, player->s.number, 240, qtrue );
		}
		break;

	case VRMERC_HOLD:
		if ( res.gaveUp ) {
			// unreachable or blocked: stay where we are and do not retry for a while
			vec3_t here;

			VectorCopy( selfPos, here );
			VrMerc_AnchorAt( ent, cs, here );
			merc.returnAllowedAt = now + VRMERC_RETURN_COOLDOWN_MS;
			G_Printf( "VR mercenary: return stalled at %.0f units, holding for %d ms\n",
					  distToTarget, VRMERC_RETURN_COOLDOWN_MS );
			VrTac_Log( 1, "nav_failure", "maneuver=return dist=%.0f cooldown_ms=%d", distToTarget, VRMERC_RETURN_COOLDOWN_MS );
		} else if ( merc.holdGoal >= 0 ) {
			AIFunc_ChaseGoalStart( cs, merc.holdGoal, 128, qtrue );     // walk back to the post
		}
		break;

	case VRMERC_FOLLOW:
		if ( prev == VRMERC_HOLD ) {
			VrMerc_ApplyFollow( player, cs );       // cooldown over: release the post, follow again
		} else {
			cs->leaderNum = player->s.number;
		}
		break;

	default:
		break;
	}
	VrMerc_SetState( res.state, res.state == VRMERC_ENGAGE ? VRTAC_R_TARGET_SEEN :
						 res.state == VRMERC_RETURN ? VRTAC_R_FAR :
						 res.gaveUp ? VRTAC_R_GAVE_UP :
						 prev == VRMERC_ENGAGE ? VRTAC_R_COMBAT_OVER : VRTAC_R_ARRIVED );
}

/*
===============
VrMerc_Init

Called once per level load (the game module state is reset on every map load).
===============
*/
void VrMerc_Init( void ) {
	int i;

	memset( &sq, 0, sizeof( sq ) );
	sq.pendingIdx = -1;
	sq.spawnEntity = -1;
	sq.levelStart = level.time;
	// no soldier record, goal entity or entity number may survive a level change or a load
	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		VrMerc_InstInit( i );
	}
	mcur = &squad[0];
	memset( mercMoveMode, 0, sizeof( mercMoveMode ) );
	tacLogWindowStart = tacLogCount = tacLogDropped = 0;
}
/*
===============
Squad sensing (once per second, for the whole squad): is the player standing still, and is the
player in a corridor or doorway (then the formation becomes single file)?
===============
*/
static void VrMerc_SquadSense( gentity_t *player ) {
	int now = level.time;
	float speed = sqrtf( player->client->ps.velocity[0] * player->client->ps.velocity[0] +
						 player->client->ps.velocity[1] * player->client->ps.velocity[1] );

	if ( speed < 40.0f ) {
		if ( !sq.playerStillSince ) {
			sq.playerStillSince = now;
		}
	} else {
		sq.playerStillSince = 0;
	}
	if ( now >= sq.nextNarrowCheck ) {
		vec3_t o, l, r, right, fwd, up;
		trace_t tl, tr;
		float width;
		int narrow;

		sq.nextNarrowCheck = now + 1000;
		AngleVectors( player->client->ps.viewangles, fwd, right, up );
		VectorCopy( player->r.currentOrigin, o );
		VectorMA( o, -120, right, l );
		VectorMA( o, 120, right, r );
		trap_Trace( &tl, o, NULL, NULL, l, player->s.number, MASK_PLAYERSOLID );
		trap_Trace( &tr, o, NULL, NULL, r, player->s.number, MASK_PLAYERSOLID );
		width = tl.fraction * 120.0f + tr.fraction * 120.0f;
		// hysteresis: narrow below 220 units, wide again only when both sides are clear
		narrow = sq.narrow ? ( width < 235.0f ) : VrSquad_IsNarrow( tl.fraction * 120.0f, tr.fraction * 120.0f );
		if ( narrow != sq.narrow ) {
			sq.narrow = narrow;
			VrSquad_Log( 1, "merc_formation_changed", "narrow=%d width=%.0f squad=%d", narrow, width, VrMerc_LiveCount() );
		}
	}
}

/*
===============
VrMerc_Frame

Called once per server frame after the AI has run.  Does nothing unless a
companion exists, a spawn is pending or the player asked for one.
===============
*/
void VrMerc_Frame( void ) {
	gentity_t *player;
	int i, live, wanted, now = level.time;

	if ( g_gametype.integer != GT_SINGLE_PLAYER ) {
		return;
	}
	live = VrMerc_LiveMask();
	if ( !live && !sq.spawnPending && vr_merc.integer <= 0 ) {
		return;     // fast path: nothing to do
	}
	player = VrMerc_Player();

	// armed "attack my target": the player's next fire press designates what the weapon points at
	if ( player && sq.designateArmedUntil ) {
		int pressed = ( player->client->pers.cmd.buttons & BUTTON_ATTACK ) ? 1 : 0;
		int addressed = sq.designateMask & live;

		if ( !addressed || now >= sq.designateArmedUntil ) {
			sq.designateArmedUntil = 0;
			VrSquad_Log( 1, "designate_disarmed", "reason=%s", addressed ? "timeout" : "no_companion" );
		} else if ( pressed && !sq.lastAttackButton ) {
			VrMerc_Designate( player, addressed );
		}
		sq.lastAttackButton = pressed;
	}
	if ( !player ) {
		return;
	}

	// a recruit on the way
	if ( sq.spawnPending && now >= sq.nextPoll && sq.pendingIdx >= 0 ) {
		sq.nextPoll = now + VRMERC_THINK_MS;
		mcur = &squad[sq.pendingIdx];
		VrMerc_PollSpawn( player );
		mcur = &squad[0];
	}

	// every soldier thinks on its own schedule (staggered), independently of the others
	if ( live ) {
		VrMerc_SquadSense( player );
	}
	for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
		if ( squad[i].slot < 0 || now < squad[i].nextThink ) {
			continue;
		}
		mcur = &squad[i];
		merc.nextThink = now + VRMERC_THINK_MS;
		VrMerc_Supervise( player );
	}
	mcur = &squad[0];

	// automatic (re-)hire after a level change or a load: up to the remembered number of soldiers,
	// one recruit at a time, with the M3 retry/limit policy
	wanted = vr_merc.integer < VrMerc_SquadLimit() ? vr_merc.integer : VrMerc_SquadLimit();
	if ( !sq.spawnPending && VrMerc_LiveCount() < wanted && now >= sq.nextPoll ) {
		vrMercHireContext_t ctx;
		int result;

		VrMerc_BuildHireContext( player, &ctx );
		result = VrMerc_CanHire( &ctx );
		if ( VrMerc_ShouldAutoSpawn( 1, result, sq.spawnAttempts, now, sq.nextSpawnAttempt, now - sq.levelStart ) ) {
			result = VrMerc_TryHire( player );
			sq.nextSpawnAttempt = now + VRMERC_SPAWN_RETRY_MS;
			if ( result != VRMERC_HIRE_OK ) {
				sq.spawnAttempts++;
				if ( sq.spawnAttempts >= VRMERC_SPAWN_MAX_ATTEMPTS ) {
					VrMerc_Message( VrMerc_HireReasonText( result ) );
				}
			}
		}
	}
}
/*
===============
Command helpers
===============
*/
// Which soldiers does a command address?  Explicit "all" / 1..4 first, then the notebook selector.
static qboolean VrMerc_Select( const char *arg, int *mask ) {
	switch ( VrSquad_ResolveSelection( arg, vr_merc_sel.integer, VrMerc_LiveMask(), mask ) ) {
	case VRSQ_SEL_OK:
		return qtrue;
	case VRSQ_SEL_BAD_ARG:
		VrMerc_Message( "Mercenary: use all or a number from 1 to 4" );
		break;
	case VRSQ_SEL_NO_SUCH:
		VrMerc_Message( "Mercenary: no such soldier in your squad" );
		break;
	default:
		VrMerc_Message( "Mercenary: not hired yet" );
		break;
	}
	return qfalse;
}

/*
===============
VrMerc_ClientCommand

  merc hire | dismiss [all|N] | follow [all|N] | hold [all|N] | regroup [all|N] | cover [all|N]
        attack [now] [all|N] | stance [defensive|aggressive] | assist [0|1] | select [all|N]
        type [model|auto] | models | status
===============
*/
void VrMerc_ClientCommand( gentity_t *ent ) {
	char sub[MAX_QPATH];
	char arg[MAX_QPATH];
	char arg2[MAX_QPATH];
	gentity_t *player = VrMerc_Player();
	int result, i, mask, live;
	vrMerc_t *saved = mcur;

	// local single-player human only
	if ( g_gametype.integer != GT_SINGLE_PLAYER || !player || ent != player ) {
		return;
	}

	trap_Argv( 1, sub, sizeof( sub ) );
	trap_Argv( 2, arg, sizeof( arg ) );
	trap_Argv( 3, arg2, sizeof( arg2 ) );
	live = VrMerc_LiveCount();

	if ( !Q_stricmp( sub, "hire" ) ) {
		int limit = VrMerc_SquadLimit(), can = VrSquad_CanAdd( live, sq.spawnPending ? 1 : 0, limit );

		if ( can == VRSQ_PENDING ) {
			VrMerc_Message( VrMerc_HireReasonText( VRMERC_HIRE_SPAWN_PENDING ) );
			return;
		}
		if ( can != VRSQ_OK ) {
			// a clear refusal, never a silent failure
			VrSquad_Log( 1, "merc_limit_reached", "squad=%d limit=%d reason=%s", live, limit, VrTac_ReasonName( VRTAC_R_SQUAD_FULL ) );
			VrMerc_Message( va( "Mercenary: squad is full (%d of %d)", live, limit ) );
			return;
		}
		result = VrMerc_TryHire( player );
		if ( result != VRMERC_HIRE_OK ) {
			if ( result == VRMERC_HIRE_NO_CLIENT_SLOT ) {
				VrSquad_Log( 1, "merc_limit_reached", "squad=%d limit=%d reason=%s", live, limit, VrTac_ReasonName( VRTAC_R_NO_SLOT ) );
			}
			VrMerc_Message( VrMerc_HireReasonText( result ) );
			return;
		}
		VrSquad_Log( 1, "order", "cmd=hire squad=%d limit=%d", live, limit );
		// remember the wish (the number of soldiers) across level changes and loads
		trap_Cvar_Set( "vr_merc", va( "%d", VrMerc_LiveCount() + 1 ) );
	} else if ( !Q_stricmp( sub, "dismiss" ) ) {
		if ( sq.spawnPending ) {
			VrMerc_CancelSpawn();
			trap_Cvar_Set( "vr_merc", va( "%d", live ) );
			VrMerc_Message( "Mercenary: cancelled" );
		} else if ( VrMerc_Select( arg, &mask ) ) {
			for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
				if ( mask & ( 1 << i ) ) {
					mcur = &squad[i];
					VrTac_Log( 1, "order", "cmd=dismiss" );
					merc.dismissRequested = qtrue;
					merc.nextThink = 0;
				}
			}
			mcur = saved;
		} else if ( live == 0 ) {
			trap_Cvar_Set( "vr_merc", "0" );
		}
	} else if ( !Q_stricmp( sub, "follow" ) || !Q_stricmp( sub, "hold" ) ) {
		int order = !Q_stricmp( sub, "hold" ) ? VRMERC_ORDER_HOLD : VRMERC_ORDER_FOLLOW;

		if ( !VrMerc_Select( arg, &mask ) ) {
			return;
		}
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			gentity_t *comp;

			if ( !( mask & ( 1 << i ) ) ) {
				continue;
			}
			mcur = &squad[i];
			comp = VrMerc_Entity();
			if ( comp ) {
				VrMerc_SetOrder( player, comp, order, qtrue );
			}
		}
		mcur = saved;
	} else if ( !Q_stricmp( sub, "type" ) ) {
		trap_Argv( 2, arg, sizeof( arg ) );
		VrMerc_ProbeModels();
		if ( arg[0] && Q_stricmp( arg, "auto" ) ) {
			trap_Cvar_Set( "vr_merc_model", arg );      // validated at hire time; invalid ones fall back
			VrMerc_Message( va( "Mercenary model: %s (if usable)", arg ) );
		} else if ( arg[0] ) {
			trap_Cvar_Set( "vr_merc_model", "" );
			VrMerc_Message( "Mercenary model: automatic" );
		} else {
			// no argument: cycle automatic -> each usable native model -> automatic
			char cur[VRMERC_NAME_LEN];
			int i, curIdx = -1, nextIdx = -1;

			trap_Cvar_VariableStringBuffer( "vr_merc_model", cur, sizeof( cur ) );
			for ( i = 0; i < sq.numCand; i++ ) {
				if ( cur[0] && VrMerc_StrEqNoCase( sq.cand[i].model, cur ) ) {
					curIdx = i;
				}
			}
			for ( i = curIdx + 1; i < sq.numCand; i++ ) {
				if ( sq.cand[i].problem == VRMERC_MODEL_OK ) {
					nextIdx = i;
					break;
				}
			}
			if ( nextIdx < 0 ) {
				trap_Cvar_Set( "vr_merc_model", "" );
				VrMerc_Message( "Mercenary model: automatic" );
			} else {
				trap_Cvar_Set( "vr_merc_model", sq.cand[nextIdx].model );
				VrMerc_Message( va( "Mercenary model: %s", sq.cand[nextIdx].model ) );
			}
		}
	} else if ( !Q_stricmp( sub, "models" ) ) {
		// full report: native pairings, then every other model directory that would qualify
		static char dirBuf[8192];
		int i, n, pick = VrMerc_PickModel();
		char *d;

		for ( i = 0; i < sq.numCand; i++ ) {
			G_Printf( "VR mercenary: candidate %d: %s/%s model %s: %s%s%s\n", i, aiDefaults[sq.cand[i].aiChar].name,
					  sq.cand[i].model, sq.cand[i].bodySkin, VrMerc_ModelProblemText( sq.cand[i].problem ),
					  sq.cand[i].missing[0] ? " - missing: " : "", sq.cand[i].missing );
		}
		n = trap_FS_GetFileList( "models/players", "/", dirBuf, sizeof( dirBuf ) );
		for ( i = 0, d = dirBuf; i < n; i++, d += strlen( d ) + 1 ) {
			vrMercCand_t probe;
			int k, known = 0;

			if ( !strcmp( d, "." ) || !strcmp( d, ".." ) ) {
				continue;
			}
			for ( k = 0; k < sq.numCand; k++ ) {
				if ( VrMerc_StrEqNoCase( sq.cand[k].model, d ) ) {
					known = 1;
				}
			}
			if ( !known ) {
				memset( &probe, 0, sizeof( probe ) );
				probe.aiChar = AICHAR_AMERICAN;
				VrMerc_ProbeModelDir( &probe, d );
			}
		}
		VrMerc_Message( pick < 0 ? "Mercenary: no usable soldier model (see log)" :
					   va( "Mercenary model: %s", sq.cand[pick].model ) );
	} else if ( !Q_stricmp( sub, "regroup" ) || !Q_stricmp( sub, "cover" ) ) {
		qboolean cover = !Q_stricmp( sub, "cover" ) ? qtrue : qfalse;

		if ( !VrMerc_Select( arg, &mask ) ) {
			return;
		}
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			gentity_t *comp;
			cast_state_t *ccs;
			vrTacOrderCtx_t octx;
			float dist;

			if ( !( mask & ( 1 << i ) ) ) {
				continue;
			}
			mcur = &squad[i];
			comp = VrMerc_Entity();
			octx = VrMerc_OrderContext( player );
			result = VrTac_ValidateCommand( cover ? VRTAC_CMD_COVER_ME : VRTAC_CMD_REGROUP, &octx );
			if ( result != VRTAC_OK || !comp ) {
				VrTac_Log( 1, "order_rejected", "cmd=%s reason=%s", cover ? "cover_me" : "regroup", VrTac_ReasonName( result ) );
				VrMerc_Say( VrMerc_ReasonText( result ) );
				continue;
			}
			ccs = AICast_GetCastState( comp->s.number );
			if ( VrMerc_ScriptBusy( ccs ) ) {
				VrTac_Log( 1, "order_rejected", "cmd=%s reason=%s", cover ? "cover_me" : "regroup", VrTac_ReasonName( VRTAC_R_SCRIPT ) );
				VrMerc_Say( "Mercenary: busy right now" );
				continue;
			}
			VrTac_Log( 1, "order", "cmd=%s", cover ? "cover_me" : "regroup" );
			if ( merc.order == VRMERC_ORDER_HOLD ) {
				VrMerc_SetOrder( player, comp, VRMERC_ORDER_FOLLOW, qfalse );   // leaves the post, ends any maneuver
			} else if ( !cover || merc.man.kind == MAN_FLANK ) {
				VrMerc_CancelManeuver( player, comp, ccs, VRTAC_R_ORDER_CHANGED );
			}
			dist = Distance( comp->r.currentOrigin, player->r.currentOrigin );
			if ( cover ) {
				merc.coverMeUntil = level.time + VRTAC_COVER_ME_MS;
				VrMerc_Say( "Mercenary: covering you" );
			}
			if ( dist > ( cover ? 400.0f : VRTAC_REGROUP_ARRIVE_DIST ) ) {
				VrMerc_StartRegroup( player, comp, ccs, VRTAC_R_COMMAND );
				if ( !cover ) {
					VrMerc_Say( "Mercenary: regrouping" );
				}
			} else if ( !cover ) {
				VrMerc_Say( "Mercenary: already with you" );
			}
		}
		mcur = saved;
	} else if ( !Q_stricmp( sub, "select" ) ) {
		// notebook selector: no argument cycles all -> 1 -> 2 ... -> all over the soldiers that exist
		int cur = vr_merc_sel.integer, next = 0, liveMask = VrMerc_LiveMask();

		if ( arg[0] ) {
			next = ( !Q_stricmp( arg, "all" ) ) ? 0 : atoi( arg );
		} else {
			for ( i = cur < 0 ? 0 : cur; i < VRMERC_MAX_SQUAD; i++ ) {
				if ( liveMask & ( 1 << i ) ) {          // squad[i] has number i + 1 > cur
					next = i + 1;
					break;
				}
			}
		}
		if ( next < 0 || next > VRMERC_MAX_SQUAD || ( next && !( liveMask & ( 1 << ( next - 1 ) ) ) ) ) {
			VrMerc_Message( "Mercenary: no such soldier in your squad" );
			return;
		}
		trap_Cvar_Set( "vr_merc_sel", va( "%d", next ) );
		VrSquad_Log( 1, "order", "cmd=select value=%d", next );
		VrMerc_Message( next ? va( "Mercenary: addressing soldier %d", next ) : "Mercenary: addressing the whole squad" );
	} else if ( !Q_stricmp( sub, "stance" ) ) {
		int aggressive;

		trap_Argv( 2, arg, sizeof( arg ) );
		if ( !Q_stricmp( arg, "aggressive" ) ) {
			aggressive = 1;
		} else if ( !Q_stricmp( arg, "defensive" ) ) {
			aggressive = 0;
		} else {
			aggressive = ( merc.stance != VRTAC_STANCE_AGGRESSIVE );     // no argument: toggle
		}
		trap_Cvar_Set( "vr_merc_stance", aggressive ? "1" : "0" );
		merc.stance = aggressive ? VRTAC_STANCE_AGGRESSIVE : VRTAC_STANCE_DEFENSIVE;
		VrTac_Log( 1, "order", "cmd=stance value=%s", aggressive ? "aggressive" : "defensive" );
		VrMerc_Message( aggressive ? "Mercenary: aggressive stance" : "Mercenary: defensive stance" );
	} else if ( !Q_stricmp( sub, "attack" ) ) {
		qboolean now = !Q_stricmp( arg, "now" ) ? qtrue : qfalse;
		int okMask = 0;

		if ( !VrMerc_Select( now ? arg2 : arg, &mask ) ) {
			return;
		}
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			vrTacOrderCtx_t octx;

			if ( !( mask & ( 1 << i ) ) ) {
				continue;
			}
			mcur = &squad[i];
			octx = VrMerc_OrderContext( player );
			result = VrTac_ValidateCommand( VRTAC_CMD_ATTACK, &octx );
			if ( result != VRTAC_OK ) {
				VrTac_Log( 1, "order_rejected", "cmd=attack reason=%s", VrTac_ReasonName( result ) );
				VrMerc_Say( VrMerc_ReasonText( result ) );
			} else {
				okMask |= 1 << i;
			}
		}
		mcur = saved;
		if ( !okMask ) {
			return;
		}
		if ( now ) {
			VrMerc_Designate( player, okMask );     // immediate: whatever the weapon points at
		} else {
			// armed: the next fire press designates, so moving the controller alone never issues an order
			sq.designateMask = okMask;
			sq.designateArmedUntil = level.time + VRTAC_DESIGNATE_ARM_MS;
			sq.lastAttackButton = ( player->client->pers.cmd.buttons & BUTTON_ATTACK ) ? 1 : 0;
			VrSquad_Log( 1, "order", "cmd=attack armed_ms=%d soldiers=%d", VRTAC_DESIGNATE_ARM_MS, VrSquad_CountBits( okMask ) );
			VrMerc_Message( "Mercenary: aim at an enemy and fire" );
		}
	} else if ( !Q_stricmp( sub, "assist" ) ) {
		int on = arg[0] ? ( atoi( arg ) != 0 ) : ( vr_merc_assist.integer == 0 );      // no argument: toggle

		trap_Cvar_Set( "vr_merc_assist", on ? "1" : "0" );
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			gentity_t *comp;

			if ( squad[i].slot < 0 ) {
				continue;
			}
			mcur = &squad[i];
			comp = VrMerc_Entity();
			if ( comp ) {
				VrMerc_SetPassive( AICast_GetCastState( comp->s.number ), on ? qfalse : qtrue );
			}
		}
		mcur = saved;
		VrSquad_Log( 1, "order", "cmd=assist value=%d", on );
		VrMerc_Message( on ? "Mercenary: will assist in combat" : "Mercenary: will not engage" );
	} else {
		// status (also the default)
		int liveMask = VrMerc_LiveMask();

		G_Printf( "VR mercenary squad: live=%d limit=%d pending=%d wanted=%d assist=%d stance=%s selected=%d narrow=%d\n",
				  VrSquad_CountBits( liveMask ), VrMerc_SquadLimit(), sq.spawnPending, vr_merc.integer,
				  vr_merc_assist.integer, vr_merc_stance.integer ? "aggressive" : "defensive", vr_merc_sel.integer, sq.narrow );
		for ( i = 0; i < VRMERC_MAX_SQUAD; i++ ) {
			if ( squad[i].slot < 0 && squad[i].state == VRMERC_NONE ) {
				continue;
			}
			G_Printf( "VR mercenary %d: state=%s order=%s slot=%d health=%d target=%d designated=%d maneuver=%s move=%s "
					  "supervised=%d changes=%d perceptions=%d\n",
					  squad[i].id, VrMerc_StateName( squad[i].state ), squad[i].order == VRMERC_ORDER_HOLD ? "HOLD" : "FOLLOW",
					  squad[i].slot, squad[i].slot >= 0 ? g_entities[squad[i].slot].health : 0, squad[i].target,
					  squad[i].designated, VrMerc_ManName( squad[i].man.kind ), VrMove_ModeName( squad[i].move.mode ),
					  squad[i].supervisedFrames, squad[i].stateChanges, squad[i].perceptionRuns );
		}
		trap_SendServerCommand( 0, va( "print \"Mercenary squad: %d of %d\n\"", VrSquad_CountBits( liveMask ), VrMerc_SquadLimit() ) );
	}
}
