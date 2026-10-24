/*
===========================================================================
g_vrlogic.h -- pure decision logic for the PICO 4 / Quest gameplay additions
(single-player God Mode and the AI mercenary companion).

Everything here is a small static inline function that uses only plain C
types, so the exact code that runs inside the game module can also be built
and unit tested on a host machine (see tests/vr_gameplay).  Keep this header
free of engine includes; the engine glue lives in g_vr.c and g_merc.c.
===========================================================================
*/

#ifndef G_VRLOGIC_H
#define G_VRLOGIC_H

#include <math.h>

/* ------------------------------------------------------------------------
 * God Mode
 * ---------------------------------------------------------------------- */

/* Decides whether the "vr_godmode" option protects an entity from damage.
 *
 *   singlePlayerGame   g_gametype == GT_SINGLE_PLAYER
 *   cvarOn             vr_godmode is non-zero
 *   hasClient          the entity is a client (has a gclient_t)
 *   isCastAI           the entity is an AI character (SVF_CASTAI)
 *   entityNum          entity number; the single-player human is always 0
 *   noProtectionDamage damage carries DAMAGE_NO_PROTECTION (telefrag,
 *                      kill triggers): the stock godmode flag ignores it too
 */
static inline int VrGod_Protects( int singlePlayerGame, int cvarOn, int hasClient,
								  int isCastAI, int entityNum, int noProtectionDamage ) {
	if ( !singlePlayerGame || !cvarOn ) {
		return 0;
	}
	if ( !hasClient || isCastAI || entityNum != 0 ) {
		return 0;
	}
	if ( noProtectionDamage ) {
		return 0;
	}
	return 1;
}

/* Health the player is restored to when God Mode is switched on. */
static inline int VrGod_RestoreHealth( int currentHealth, int maxHealth ) {
	if ( maxHealth < 1 ) {
		maxHealth = 100;
	}
	return ( currentHealth < maxHealth ) ? maxHealth : currentHealth;
}

/* ------------------------------------------------------------------------
 * Mercenary companion
 * ---------------------------------------------------------------------- */

typedef enum {
	VRMERC_NONE = 0,    /* no companion, nothing pending                         */
	VRMERC_FOLLOW,      /* staying with the player (stock leader/follower AI)    */
	VRMERC_HOLD,        /* guarding a fixed position                             */
	VRMERC_ENGAGE,      /* fighting a hostile target                             */
	VRMERC_RETURN,      /* walking back to the player / hold position            */
	VRMERC_DEAD,        /* killed in action (terminal until the next hire)       */
	VRMERC_DISMISSED    /* dismissed by the player (terminal until next hire)    */
} vrMercState_t;

typedef enum {
	VRMERC_ORDER_FOLLOW = 0,
	VRMERC_ORDER_HOLD
} vrMercOrder_t;

/* Tuning constants (game units; 1 unit is roughly 1 inch).  Times in ms. */
#define VRMERC_MAX_COMPANIONS       1       /* MVP limit                           */
#define VRMERC_THINK_MS             100     /* supervisor interval                 */
#define VRMERC_FOLLOW_RETURN_DIST   640.0f  /* too far from the player: go back    */
#define VRMERC_FOLLOW_ARRIVE_DIST   320.0f  /* close enough: resume following      */
#define VRMERC_HOLD_LEASH           512.0f  /* too far from the hold spot: go back */
#define VRMERC_HOLD_ARRIVE_DIST     96.0f
#define VRMERC_ENGAGE_LINGER_MS     2500    /* keep ENGAGE this long after losing the target */
#define VRMERC_RETURN_STALL_MS      10000   /* no progress for this long: give up  */
#define VRMERC_RETURN_MIN_GAIN      48.0f   /* units closer that count as progress */
#define VRMERC_RETURN_COOLDOWN_MS   20000   /* wait before trying to return again  */
#define VRMERC_SPAWN_TIMEOUT_MS     5000    /* spawn request must finish in this time */
#define VRMERC_SPAWN_RETRY_MS       2000
#define VRMERC_SPAWN_MAX_ATTEMPTS   6
#define VRMERC_AUTOSPAWN_DELAY_MS   4000    /* after level start before auto-spawning */

typedef struct {
	int alive;              /* companion entity is in use and health > 0       */
	int dismissRequested;
	int order;              /* vrMercOrder_t, the player's standing order      */
	int assist;             /* may engage hostile targets                      */
	int haveTarget;         /* stock AI currently has a valid hostile target   */
	int msSinceTarget;      /* time since the last valid target (0 if have)    */
	int returnAllowed;      /* 0 while a failed return is cooling down         */
	float distToPlayer;
	float distToHold;
} vrMercInput_t;

typedef struct {
	int state;              /* new vrMercState_t                               */
	int gaveUp;             /* RETURN was abandoned (unreachable / stalled)    */
} vrMercResult_t;

static inline int VrMerc_StateIsActive( int state ) {
	return state == VRMERC_FOLLOW || state == VRMERC_HOLD ||
		   state == VRMERC_ENGAGE || state == VRMERC_RETURN;
}

static inline int VrMerc_StateAfterCombatOrReturn( const vrMercInput_t *in ) {
	if ( in->order == VRMERC_ORDER_HOLD ) {
		return VRMERC_HOLD;
	}
	return VRMERC_FOLLOW;
}

/* One step of the supervisor state machine.  `stalled` is the result of
 * VrMerc_Progress for the current RETURN. */
static inline vrMercResult_t VrMerc_NextState( int cur, const vrMercInput_t *in, int stalled ) {
	vrMercResult_t r;
	float returnDist;
	float distToTarget;

	r.state = cur;
	r.gaveUp = 0;

	/* terminal and idle states only leave through an explicit hire */
	if ( cur == VRMERC_NONE || cur == VRMERC_DEAD || cur == VRMERC_DISMISSED ) {
		return r;
	}
	if ( !in->alive ) {
		r.state = VRMERC_DEAD;
		return r;
	}
	if ( in->dismissRequested ) {
		r.state = VRMERC_DISMISSED;
		return r;
	}

	distToTarget = ( in->order == VRMERC_ORDER_HOLD ) ? in->distToHold : in->distToPlayer;
	returnDist = ( in->order == VRMERC_ORDER_HOLD ) ? VRMERC_HOLD_LEASH : VRMERC_FOLLOW_RETURN_DIST;

	/* a hostile target always wins when assisting is allowed */
	if ( in->assist && in->haveTarget ) {
		r.state = VRMERC_ENGAGE;
		return r;
	}

	switch ( cur ) {
	case VRMERC_ENGAGE:
		if ( in->assist && in->msSinceTarget < VRMERC_ENGAGE_LINGER_MS ) {
			break;      /* stay in ENGAGE a little: targets flicker in and out of sight */
		}
		if ( in->returnAllowed && distToTarget > returnDist ) {
			r.state = VRMERC_RETURN;
		} else {
			r.state = VrMerc_StateAfterCombatOrReturn( in );
		}
		break;

	case VRMERC_RETURN:
		if ( distToTarget <= ( ( in->order == VRMERC_ORDER_HOLD ) ?
							   VRMERC_HOLD_ARRIVE_DIST : VRMERC_FOLLOW_ARRIVE_DIST ) ) {
			r.state = VrMerc_StateAfterCombatOrReturn( in );
		} else if ( stalled ) {
			/* unreachable or blocked: stop, guard where we stand, retry later */
			r.state = VRMERC_HOLD;
			r.gaveUp = 1;
		}
		break;

	case VRMERC_FOLLOW:
	case VRMERC_HOLD:
	default:
		if ( cur == VRMERC_FOLLOW && in->order == VRMERC_ORDER_HOLD ) {
			r.state = VRMERC_HOLD;
		} else if ( cur == VRMERC_HOLD && in->order == VRMERC_ORDER_FOLLOW && in->returnAllowed ) {
			r.state = VRMERC_FOLLOW;    /* (stays HOLD while a failed return cools down) */
		} else if ( in->returnAllowed && distToTarget > returnDist ) {
			r.state = VRMERC_RETURN;
		}
		break;
	}
	return r;
}

/* RETURN progress watchdog: remembers the closest distance reached and when. */
typedef struct {
	float bestDist;
	int   bestTime;
} vrMercProgress_t;

static inline void VrMerc_ProgressReset( vrMercProgress_t *p, float dist, int now ) {
	p->bestDist = dist;
	p->bestTime = now;
}

/* Returns 1 when the companion has not got meaningfully closer for stallMs. */
static inline int VrMerc_ProgressStalled( vrMercProgress_t *p, float dist, int now,
										  float minGain, int stallMs ) {
	if ( dist < p->bestDist - minGain ) {
		p->bestDist = dist;
		p->bestTime = now;
	}
	return ( now - p->bestTime ) >= stallMs;
}

/* A target is a valid hostile only if it is alive, not the player, not a
 * companion, not on the companion's team and not neutral (civilians and story
 * NPCs).  Team numbers follow AITeam_t: the caller passes plain integers. */
static inline int VrMerc_IsValidHostile( int mercTeam, int targetTeam, int neutralTeam,
										 int targetIsPlayer, int targetIsCompanion,
										 int targetAlive ) {
	if ( !targetAlive || targetIsPlayer || targetIsCompanion ) {
		return 0;
	}
	if ( targetTeam == mercTeam || targetTeam == neutralTeam ) {
		return 0;
	}
	return 1;
}

/* Friendly-fire policy for damage that reaches G_Damage.  Returns 1 if the
 * damage must be ignored.
 *   - the companion never hurts the player, another ally or a neutral NPC
 *   - the player's shots never hurt the companion (VR aiming accidents);
 *     use the dismiss command instead */
static inline int VrMerc_DamagePolicy( int attackerIsCompanion, int attackerIsPlayer,
										int targetIsCompanion, int targetIsPlayer,
										int targetTeam, int mercTeam, int neutralTeam,
										int targetIsCast ) {
	if ( attackerIsCompanion ) {
		if ( targetIsCompanion ) {
			return 0;       /* own splash damage is left to the stock rules */
		}
		if ( targetIsPlayer ) {
			return 1;
		}
		if ( targetIsCast && ( targetTeam == mercTeam || targetTeam == neutralTeam ) ) {
			return 1;
		}
		return 0;
	}
	if ( attackerIsPlayer && targetIsCompanion ) {
		return 1;
	}
	return 0;
}

/* Reasons a hire (or automatic respawn) can be refused. */
typedef enum {
	VRMERC_HIRE_OK = 0,
	VRMERC_HIRE_NOT_SINGLEPLAYER,
	VRMERC_HIRE_NO_PLAYER,
	VRMERC_HIRE_PLAYER_DEAD,
	VRMERC_HIRE_CINEMATIC,
	VRMERC_HIRE_LOADING,
	VRMERC_HIRE_AI_DISABLED,
	VRMERC_HIRE_ALREADY_ACTIVE,
	VRMERC_HIRE_SPAWN_PENDING,
	VRMERC_HIRE_NO_CLIENT_SLOT,
	VRMERC_HIRE_NO_NAVIGATION,
	VRMERC_HIRE_NO_SPAWN_POINT
} vrMercHireResult_t;

typedef struct {
	int singlePlayerGame;
	int playerPresent;      /* human client exists and is connected            */
	int playerAlive;
	int cameraActive;       /* in-engine cutscene running                      */
	int loading;            /* savegame load / level start still settling      */
	int aiEnabled;          /* bot_enable                                      */
	int companionActive;    /* a companion already exists                      */
	int spawnPending;       /* a spawn request is in flight                    */
	int freeClientSlots;    /* sv_maxclients minus playing clients             */
	int navigationLoaded;   /* AAS data present for this map                   */
} vrMercHireContext_t;

static inline int VrMerc_CanHire( const vrMercHireContext_t *c ) {
	if ( !c->singlePlayerGame ) {
		return VRMERC_HIRE_NOT_SINGLEPLAYER;
	}
	if ( !c->playerPresent ) {
		return VRMERC_HIRE_NO_PLAYER;
	}
	if ( !c->playerAlive ) {
		return VRMERC_HIRE_PLAYER_DEAD;
	}
	if ( c->cameraActive ) {
		return VRMERC_HIRE_CINEMATIC;
	}
	if ( c->loading ) {
		return VRMERC_HIRE_LOADING;
	}
	if ( !c->aiEnabled ) {
		return VRMERC_HIRE_AI_DISABLED;
	}
	if ( c->companionActive ) {
		return VRMERC_HIRE_ALREADY_ACTIVE;
	}
	if ( c->spawnPending ) {
		return VRMERC_HIRE_SPAWN_PENDING;
	}
	if ( c->freeClientSlots < VRMERC_MAX_COMPANIONS ) {
		return VRMERC_HIRE_NO_CLIENT_SLOT;
	}
	if ( !c->navigationLoaded ) {
		return VRMERC_HIRE_NO_NAVIGATION;
	}
	return VRMERC_HIRE_OK;
}

/* Companion that should come back after a level change or a savegame load
 * (the hire request lives in a cvar; the companion itself is never saved).
 * Returns 1 when a spawn attempt should be made now. */
static inline int VrMerc_ShouldAutoSpawn( int wanted, int hireResult, int attempts,
										  int now, int nextAttemptTime, int levelAgeMs ) {
	if ( !wanted || hireResult != VRMERC_HIRE_OK ) {
		return 0;
	}
	if ( attempts >= VRMERC_SPAWN_MAX_ATTEMPTS ) {
		return 0;
	}
	if ( levelAgeMs < VRMERC_AUTOSPAWN_DELAY_MS ) {
		return 0;
	}
	return now >= nextAttemptTime;
}

/* Deterministic candidate offsets around the player, closest ring first.
 * Angles are relative to the player's facing yaw; behind the player first so
 * the companion does not appear in the field of view. */
#define VRMERC_SPAWN_CANDIDATES 24

static inline int VrMerc_SpawnOffset( int index, float playerYawDeg, float *dx, float *dy ) {
	static const float rings[3]  = { 112.0f, 144.0f, 176.0f };
	static const float angles[8] = { 150.0f, -150.0f, 110.0f, -110.0f, 180.0f, 70.0f, -70.0f, 0.0f };
	float a;

	if ( index < 0 || index >= VRMERC_SPAWN_CANDIDATES ) {
		return 0;
	}
	a = ( playerYawDeg + angles[index % 8] ) * 3.14159265f / 180.0f;
	*dx = cosf( a ) * rings[index / 8];
	*dy = sinf( a ) * rings[index / 8];
	return 1;
}

/* Soldier type setting (cvar vr_merc_char): 0 = American (default), 1 = Partisan.
 * Both are allied Cast AI characters with a Thompson; the second one exists as a
 * fallback if the American player model is missing from a particular install. */
static inline int VrMerc_CharacterForSetting( int setting, int americanChar, int partisanChar ) {
	return ( setting == 1 ) ? partisanChar : americanChar;
}

static inline int VrMerc_NextCharacterSetting( int setting ) {
	return ( setting == 1 ) ? 0 : 1;
}

/* A resting surface is walkable when its normal is steep enough. */
static inline int VrMerc_GroundWalkable( float normalZ ) {
	return normalZ >= 0.7f;
}

#endif /* G_VRLOGIC_H */
