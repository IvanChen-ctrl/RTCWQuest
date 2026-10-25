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

Console / VR command: merc <hire|follow|hold|assist [0|1]|type [model|auto]|models|dismiss|status>

Pure decision logic is in g_vrlogic.h and covered by tests/vr_gameplay.
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

#define VRMERC_AINAME       "vr_merc_1"
#define VRMERC_WAITGOAL     "AI_wait_goal"      // classname used by the stock "stay" order

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

typedef struct {
	int state;                  // vrMercState_t
	int order;                  // vrMercOrder_t
	int slot;                   // entity (client) number of the companion, -1 if none
	int character;              // AICHAR_* used for the current / pending companion
	qboolean modelsProbed;      // model files were checked on this level
	int numCand;                // candidates probed so far (native ones first)
	vrMercCand_t cand[VRMERC_MAX_CAND];
	int chosen;                 // index into cand[] used by the current / pending companion
	int holdGoal;               // entity number of the wait goal, -1 if none
	vec3_t holdPos;
	qboolean dismissRequested;

	qboolean spawnPending;
	int spawnStart;
	int spawnEntity;            // temporary entity that AIChar_spawn will consume
	int spawnAttempts;
	int nextSpawnAttempt;

	int nextThink;
	int levelStart;
	int lastTargetTime;
	int returnAllowedAt;
	vrMercProgress_t progress;

	// counters for "merc status" (profiling aid, no per-frame logging)
	int supervisedFrames;
	int stateChanges;
} vrMerc_t;

static vrMerc_t merc;

static const vec3_t mercMins = { -18, -18, -24 };       // same box as BBOX_SMALL in ai_cast_characters.c
static const vec3_t mercMaxs = { 18, 18, 48 };

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
	return ent && ent->inuse && ent->client && ( ent->r.svFlags & SVF_CASTAI ) &&
		   ent->aiName && !strcmp( ent->aiName, VRMERC_AINAME );
}

// The companion's entity, or NULL if it is not (or no longer) in the game.
static gentity_t *VrMerc_Entity( void ) {
	gentity_t *ent;

	if ( merc.slot < 0 || merc.slot >= level.maxclients ) {
		return NULL;
	}
	ent = &g_entities[merc.slot];
	return VrMerc_IsCompanion( ent ) ? ent : NULL;
}

static void VrMerc_Message( const char *text ) {
	trap_SendServerCommand( 0, va( "cp \"%s\n\"", text ) );
}

static const char *VrMerc_HireReasonText( int reason ) {
	switch ( reason ) {
	case VRMERC_HIRE_NOT_SINGLEPLAYER:  return "Mercenary: single player only";
	case VRMERC_HIRE_NO_PLAYER:         return "Mercenary: player not ready";
	case VRMERC_HIRE_PLAYER_DEAD:       return "Mercenary: you are dead";
	case VRMERC_HIRE_CINEMATIC:         return "Mercenary: not during a cutscene";
	case VRMERC_HIRE_LOADING:           return "Mercenary: level is still loading";
	case VRMERC_HIRE_AI_DISABLED:       return "Mercenary: AI is disabled (bot_enable)";
	case VRMERC_HIRE_ALREADY_ACTIVE:    return "Mercenary: already hired";
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

	if ( merc.modelsProbed ) {
		return;
	}
	merc.numCand = 0;
	for ( i = 0; i < (int)( sizeof( mercNativeChars ) / sizeof( mercNativeChars[0] ) ) && merc.numCand < VRMERC_MAX_CAND; i++ ) {
		vrMercCand_t *c = &merc.cand[merc.numCand];
		char model[VRMERC_NAME_LEN];

		memset( c, 0, sizeof( *c ) );
		c->aiChar = mercNativeChars[i];
		VrMerc_NativeModelName( c->aiChar, model, sizeof( model ) );
		VrMerc_ProbeModelDir( c, model );
		merc.numCand++;
	}
	merc.modelsProbed = qtrue;
}

// Index into merc.cand[] of the pairing to spawn, or -1.  A model named by the cvar
// vr_merc_model is tried first; it may be a native model or any other directory under
// models/players that passes the same checks (it then gets the American behaviour).
static int VrMerc_PickModel( void ) {
	int problems[VRMERC_MAX_CAND];
	int i, preferred = -1;
	char want[VRMERC_NAME_LEN];

	VrMerc_ProbeModels();

	trap_Cvar_VariableStringBuffer( "vr_merc_model", want, sizeof( want ) );
	if ( want[0] ) {
		for ( i = 0; i < merc.numCand; i++ ) {
			if ( VrMerc_StrEqNoCase( merc.cand[i].model, want ) ) {
				preferred = i;
			}
		}
		if ( preferred < 0 && merc.numCand < VRMERC_MAX_CAND ) {
			vrMercCand_t *c = &merc.cand[merc.numCand];

			memset( c, 0, sizeof( *c ) );
			c->aiChar = AICHAR_AMERICAN;
			VrMerc_ProbeModelDir( c, want );
			preferred = merc.numCand++;
		}
	}
	for ( i = 0; i < merc.numCand; i++ ) {
		problems[i] = merc.cand[i].problem;
	}
	return VrMerc_PickFirstViable( problems, merc.numCand, preferred );
}

static void VrMerc_BuildHireContext( gentity_t *player, vrMercHireContext_t *c ) {
	memset( c, 0, sizeof( *c ) );
	c->singlePlayerGame = ( g_gametype.integer == GT_SINGLE_PLAYER );
	c->playerPresent = ( player != NULL );
	c->playerAlive = ( player && player->health > 0 );
	c->cameraActive = ( player && VrMerc_CameraActive( player ) );
	c->loading = VrMerc_Loading();
	c->aiEnabled = ( trap_Cvar_VariableIntegerValue( "bot_enable" ) != 0 );
	c->companionActive = ( VrMerc_Entity() != NULL );
	c->spawnPending = merc.spawnPending;
	c->freeClientSlots = aicast_maxclients - level.numPlayingClients;
	c->navigationLoaded = trap_AAS_Initialized() ? 1 : 0;
	c->modelUsable = ( VrMerc_PickModel() >= 0 );
}

/*
===============
VrMerc_IsMercEntity

Used by the savegame writer: the companion is not saved (a load would find a
client slot that the freshly started level has not created and abort).
===============
*/
qboolean VrMerc_IsMercEntity( int entnum ) {
	gentity_t *ent;

	if ( entnum < 0 || entnum >= MAX_GENTITIES ) {
		return qfalse;
	}
	ent = &g_entities[entnum];
	if ( VrMerc_IsCompanion( ent ) ) {
		return qtrue;
	}
	// also the unconsumed spawn request, otherwise a save taken in that short window
	// would bring a second companion back when it is loaded
	return ( ent->inuse && ent->think == AIChar_spawn && ent->aiName &&
			 !strcmp( ent->aiName, VRMERC_AINAME ) ) ? qtrue : qfalse;
}

/*
===============
VrMerc_DamageBlocked
===============
*/
qboolean VrMerc_DamageBlocked( gentity_t *targ, gentity_t *attacker ) {
	if ( !targ || !attacker || ( merc.slot < 0 && !merc.spawnPending ) ) {
		return qfalse;
	}
	if ( g_gametype.integer != GT_SINGLE_PLAYER ) {
		return qfalse;
	}
	return VrMerc_DamagePolicy(
			   VrMerc_IsCompanion( attacker ),
			   attacker->client && attacker->s.number == 0 && !( attacker->r.svFlags & SVF_CASTAI ),
			   VrMerc_IsCompanion( targ ),
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
VrMerc_SetOrder

Applies a standing order immediately (the supervisor then follows it).
===============
*/
static void VrMerc_SetOrder( gentity_t *player, gentity_t *ent, int order, qboolean announce ) {
	cast_state_t *cs = AICast_GetCastState( ent->s.number );

	merc.order = order;
	merc.returnAllowedAt = 0;       // an explicit command ends any return cooldown
	if ( order == VRMERC_ORDER_HOLD ) {
		vec3_t pos;

		VectorCopy( ent->r.currentOrigin, pos );
		VrMerc_AnchorAt( ent, cs, pos );
		merc.state = VRMERC_HOLD;
		VrMerc_Sound( ent, STAYSOUNDSCRIPT );
		if ( announce ) {
			VrMerc_Message( "Mercenary: holding position" );
		}
	} else {
		VrMerc_ApplyFollow( player, cs );
		merc.state = VRMERC_FOLLOW;
		VrMerc_Sound( ent, FOLLOWSOUNDSCRIPT );
		if ( announce ) {
			VrMerc_Message( "Mercenary: following you" );
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
	gentity_t *ent = VrMerc_Entity();
	cast_state_t *cs;

	if ( !player || !ent || ent->s.number != entNum || activatorNum != player->s.number ) {
		return;
	}
	if ( !VrMerc_StateIsActive( merc.state ) ) {
		return;
	}
	cs = AICast_GetCastState( entNum );
	if ( cs->lastActivate > level.time - 1000 ) {
		return;
	}
	cs->lastActivate = level.time;
	VrMerc_SetOrder( player, ent, merc.order == VRMERC_ORDER_FOLLOW ? VRMERC_ORDER_HOLD : VRMERC_ORDER_FOLLOW, qtrue );
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

	VrMerc_FreeHoldGoal( cs );
	if ( cs ) {
		cs->activate = NULL;
		cs->leaderNum = -1;
	}

	if ( terminalState == VRMERC_DISMISSED && ent ) {
		// make anybody who is still targeting us forget us, then remove the character
		// the same way the engine removes Cast AI on level shutdown
		ent->health = 0;
		ent->client->ps.stats[STAT_HEALTH] = 0;
		trap_DropClient( ent->s.number, "" );
	}

	merc.state = terminalState;
	merc.slot = -1;
	merc.dismissRequested = qfalse;
	merc.stateChanges++;
	trap_Cvar_Set( "vr_merc", "0" );        // no automatic re-hire after death or dismissal

	VrMerc_Message( terminalState == VRMERC_DEAD ? "Mercenary: killed in action" : "Mercenary: dismissed" );
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

	VectorCopy( player->client->ps.origin, origin );

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
	merc.character = merc.cand[pick].aiChar;
	spawn->classname = ( merc.character == AICHAR_PARTISAN ) ? "ai_partisan" :
					   ( merc.character == AICHAR_SOLDIER ) ? "ai_soldier" :
					   ( merc.character == AICHAR_ELITEGUARD ) ? "ai_eliteguard" : "ai_american";
	spawn->aiCharacter = merc.character;
	spawn->aiTeam = AITEAM_ALLIES;
	spawn->aiName = VRMERC_AINAME;
	// visual identity: discovered model and skins; faction stays allied (aiTeam above)
	// (static buffers: AIChar_spawn reads them a few frames later and only one request is pending)
	Com_sprintf( mercSpawnSkin, sizeof( mercSpawnSkin ), "%s/%s", merc.cand[pick].model, merc.cand[pick].bodySkin );
	Q_strncpyz( mercSpawnHead, merc.cand[pick].headSkin, sizeof( mercSpawnHead ) );
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

	merc.spawnPending = qtrue;
	merc.spawnStart = level.time;
	merc.spawnEntity = spawn->s.number;
	merc.dismissRequested = qfalse;
	G_Printf( "VR mercenary: spawning %s behaviour with model %s skin %s head %s\n",
			  aiDefaults[merc.character].name, merc.cand[pick].model, merc.cand[pick].bodySkin,
			  merc.cand[pick].headSkin );
	return qtrue;
}

static void VrMerc_CancelSpawn( void ) {
	if ( merc.spawnEntity >= 0 ) {
		gentity_t *spawn = &g_entities[merc.spawnEntity];

		// only free it if it is still our unconsumed spawn request
		if ( spawn->inuse && spawn->think == AIChar_spawn && spawn->aiName && !strcmp( spawn->aiName, VRMERC_AINAME ) ) {
			G_FreeEntity( spawn );
		}
	}
	merc.spawnEntity = -1;
	merc.spawnPending = qfalse;
}

static gentity_t *VrMerc_FindNewCompanion( void ) {
	int i;

	for ( i = 1; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( VrMerc_IsCompanion( ent ) && ent->health > 0 ) {
			return ent;
		}
	}
	return NULL;
}

static void VrMerc_Adopt( gentity_t *player, gentity_t *ent ) {
	cast_state_t *cs = AICast_GetCastState( ent->s.number );

	merc.spawnPending = qfalse;
	merc.spawnEntity = -1;
	merc.spawnAttempts = 0;
	merc.slot = ent->s.number;
	merc.lastTargetTime = level.time;
	cs->activate = VrMerc_Activate;
	VrMerc_SetPassive( cs, vr_merc_assist.integer == 0 );

	// start in the current standing order
	VrMerc_SetOrder( player, ent, merc.order, qfalse );
	VrMerc_Message( "Mercenary: hired" );
}

static void VrMerc_PollSpawn( gentity_t *player ) {
	gentity_t *ent = VrMerc_FindNewCompanion();

	if ( ent ) {
		VrMerc_Adopt( player, ent );
		return;
	}
	if ( level.time - merc.spawnStart > VRMERC_SPAWN_TIMEOUT_MS ) {
		VrMerc_CancelSpawn();
		merc.nextSpawnAttempt = level.time + VRMERC_SPAWN_RETRY_MS;
		G_Printf( "VR mercenary: spawn timed out (attempt %d)\n", merc.spawnAttempts );
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
	int result;
	gentity_t *orphan;

	VrMerc_BuildHireContext( player, &ctx );
	result = VrMerc_CanHire( &ctx );
	if ( result != VRMERC_HIRE_OK ) {
		return result;
	}
	// a character from an earlier request that finished after we gave up on it: take it over
	// instead of creating a second one
	orphan = VrMerc_FindNewCompanion();
	if ( orphan ) {
		VrMerc_Adopt( player, orphan );
		return VRMERC_HIRE_OK;
	}
	if ( !VrMerc_FindSpawnPos( player, pos ) ) {
		return VRMERC_HIRE_NO_SPAWN_POINT;
	}
	if ( !VrMerc_RequestSpawn( player, pos ) ) {
		return VRMERC_HIRE_NO_MODEL;
	}
	merc.spawnAttempts++;
	return VRMERC_HIRE_OK;
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

	memset( &in, 0, sizeof( in ) );
	in.alive = 1;
	in.dismissRequested = merc.dismissRequested;
	in.order = merc.order;
	in.assist = ( vr_merc_assist.integer != 0 );
	in.returnAllowed = ( now >= merc.returnAllowedAt );
	in.distToPlayer = Distance( selfPos, player->r.currentOrigin );
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
		return;
	}

	// ---- state entry actions ----
	merc.stateChanges++;
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
	merc.state = res.state;
}

/*
===============
VrMerc_Init

Called once per level load (the game module state is reset on every map load).
===============
*/
void VrMerc_Init( void ) {
	memset( &merc, 0, sizeof( merc ) );
	merc.state = VRMERC_NONE;
	merc.order = VRMERC_ORDER_FOLLOW;
	merc.slot = -1;
	merc.holdGoal = -1;
	merc.spawnEntity = -1;
	merc.levelStart = level.time;
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

	if ( g_gametype.integer != GT_SINGLE_PLAYER ) {
		return;
	}
	if ( merc.slot < 0 && !merc.spawnPending && !vr_merc.integer ) {
		return;     // fast path: nothing to do
	}
	if ( level.time < merc.nextThink ) {
		return;
	}
	merc.nextThink = level.time + VRMERC_THINK_MS;

	player = VrMerc_Player();
	if ( !player ) {
		return;
	}

	if ( merc.spawnPending ) {
		VrMerc_PollSpawn( player );
	} else if ( merc.slot >= 0 ) {
		VrMerc_Supervise( player );
	} else if ( vr_merc.integer && merc.state != VRMERC_DEAD && merc.state != VRMERC_DISMISSED ) {
		// automatic (re-)hire after a level change or a load
		vrMercHireContext_t ctx;
		int result;

		VrMerc_BuildHireContext( player, &ctx );
		result = VrMerc_CanHire( &ctx );
		if ( VrMerc_ShouldAutoSpawn( 1, result, merc.spawnAttempts, level.time,
									 merc.nextSpawnAttempt, level.time - merc.levelStart ) ) {
			result = VrMerc_TryHire( player );
			merc.nextSpawnAttempt = level.time + VRMERC_SPAWN_RETRY_MS;
			if ( result != VRMERC_HIRE_OK ) {
				merc.spawnAttempts++;
				if ( merc.spawnAttempts >= VRMERC_SPAWN_MAX_ATTEMPTS ) {
					VrMerc_Message( VrMerc_HireReasonText( result ) );
				}
			}
		}
	}
}

/*
===============
VrMerc_ClientCommand

  merc hire | follow | hold | assist [0|1] | type [model|auto] | models | dismiss | status
===============
*/
void VrMerc_ClientCommand( gentity_t *ent ) {
	char sub[MAX_QPATH];
	char arg[MAX_QPATH];
	gentity_t *player = VrMerc_Player();
	gentity_t *comp;
	int result;

	// local single-player human only
	if ( g_gametype.integer != GT_SINGLE_PLAYER || !player || ent != player ) {
		return;
	}

	trap_Argv( 1, sub, sizeof( sub ) );
	comp = VrMerc_Entity();

	if ( !Q_stricmp( sub, "hire" ) ) {
		// (re)start from a clean slate: a previous death or dismissal does not block a new hire
		if ( comp || merc.spawnPending ) {
			VrMerc_Message( VrMerc_HireReasonText( comp ? VRMERC_HIRE_ALREADY_ACTIVE : VRMERC_HIRE_SPAWN_PENDING ) );
			return;
		}
		merc.state = VRMERC_NONE;
		merc.order = VRMERC_ORDER_FOLLOW;
		result = VrMerc_TryHire( player );
		if ( result != VRMERC_HIRE_OK ) {
			VrMerc_Message( VrMerc_HireReasonText( result ) );
			return;
		}
		trap_Cvar_Set( "vr_merc", "1" );        // remember the wish across level changes and loads
		merc.nextThink = 0;
	} else if ( !Q_stricmp( sub, "dismiss" ) ) {
		if ( merc.spawnPending ) {
			VrMerc_CancelSpawn();
			trap_Cvar_Set( "vr_merc", "0" );
			VrMerc_Message( "Mercenary: cancelled" );
		} else if ( comp ) {
			merc.dismissRequested = qtrue;
			merc.nextThink = 0;
		} else {
			trap_Cvar_Set( "vr_merc", "0" );
			VrMerc_Message( "Mercenary: nobody to dismiss" );
		}
	} else if ( !Q_stricmp( sub, "follow" ) || !Q_stricmp( sub, "hold" ) ) {
		int order = !Q_stricmp( sub, "hold" ) ? VRMERC_ORDER_HOLD : VRMERC_ORDER_FOLLOW;

		if ( !comp ) {
			merc.order = order;     // applies when the companion arrives
			VrMerc_Message( "Mercenary: not hired yet" );
			return;
		}
		VrMerc_SetOrder( player, comp, order, qtrue );
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
			for ( i = 0; i < merc.numCand; i++ ) {
				if ( cur[0] && VrMerc_StrEqNoCase( merc.cand[i].model, cur ) ) {
					curIdx = i;
				}
			}
			for ( i = curIdx + 1; i < merc.numCand; i++ ) {
				if ( merc.cand[i].problem == VRMERC_MODEL_OK ) {
					nextIdx = i;
					break;
				}
			}
			if ( nextIdx < 0 ) {
				trap_Cvar_Set( "vr_merc_model", "" );
				VrMerc_Message( "Mercenary model: automatic" );
			} else {
				trap_Cvar_Set( "vr_merc_model", merc.cand[nextIdx].model );
				VrMerc_Message( va( "Mercenary model: %s", merc.cand[nextIdx].model ) );
			}
		}
	} else if ( !Q_stricmp( sub, "models" ) ) {
		// full report: native pairings, then every other model directory that would qualify
		static char dirBuf[8192];
		int i, n, pick = VrMerc_PickModel();
		char *d;

		for ( i = 0; i < merc.numCand; i++ ) {
			G_Printf( "VR mercenary: candidate %d: %s/%s model %s: %s%s%s\n", i, aiDefaults[merc.cand[i].aiChar].name,
					  merc.cand[i].model, merc.cand[i].bodySkin, VrMerc_ModelProblemText( merc.cand[i].problem ),
					  merc.cand[i].missing[0] ? " - missing: " : "", merc.cand[i].missing );
		}
		n = trap_FS_GetFileList( "models/players", "/", dirBuf, sizeof( dirBuf ) );
		for ( i = 0, d = dirBuf; i < n; i++, d += strlen( d ) + 1 ) {
			vrMercCand_t probe;
			int k, known = 0;

			if ( !strcmp( d, "." ) || !strcmp( d, ".." ) ) {
				continue;
			}
			for ( k = 0; k < merc.numCand; k++ ) {
				if ( VrMerc_StrEqNoCase( merc.cand[k].model, d ) ) {
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
					   va( "Mercenary model: %s", merc.cand[pick].model ) );
	} else if ( !Q_stricmp( sub, "assist" ) ) {
		int on;

		trap_Argv( 2, arg, sizeof( arg ) );
		on = arg[0] ? ( atoi( arg ) != 0 ) : ( vr_merc_assist.integer == 0 );      // no argument: toggle
		trap_Cvar_Set( "vr_merc_assist", on ? "1" : "0" );
		if ( comp ) {
			VrMerc_SetPassive( AICast_GetCastState( comp->s.number ), on ? qfalse : qtrue );
		}
		VrMerc_Message( on ? "Mercenary: will assist in combat" : "Mercenary: will not engage" );
	} else {
		// status (also the default)
		G_Printf( "VR mercenary: state=%s order=%s slot=%d pending=%d assist=%d wanted=%d "
				  "supervised=%d changes=%d attempts=%d\n",
				  VrMerc_StateName( merc.state ), merc.order == VRMERC_ORDER_HOLD ? "HOLD" : "FOLLOW",
				  merc.slot, merc.spawnPending, vr_merc_assist.integer, vr_merc.integer,
				  merc.supervisedFrames, merc.stateChanges, merc.spawnAttempts );
		trap_SendServerCommand( 0, va( "print \"Mercenary: %s (%s)\n\"", VrMerc_StateName( merc.state ),
									   merc.order == VRMERC_ORDER_HOLD ? "hold" : "follow" ) );
	}
}
