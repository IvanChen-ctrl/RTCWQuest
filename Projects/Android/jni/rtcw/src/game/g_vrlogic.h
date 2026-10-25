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
	VRMERC_HIRE_NO_SPAWN_POINT,
	VRMERC_HIRE_NO_MODEL            /* no allied character has complete model/animation files */
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
	int modelUsable;        /* an allied character with complete files exists  */
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
	if ( !c->modelUsable ) {
		return VRMERC_HIRE_NO_MODEL;
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

/* ------------------------------------------------------------------------
 * Character model discovery and completeness
 *
 * A Cast AI character is created through ClientUserinfoChanged, which calls
 * G_ParseAnimationFiles() and aborts the whole game with
 * "Failed to load animation scripts for model <name>" unless
 *     models/players/<model>/wolfanim.cfg                         can be opened and
 *     models/players/<model>/wolfanim.script                      can be opened, or
 *     (the cfg declares "version" <= 1 and) models/players/default.script exists.
 * The client additionally needs the body skin and mesh and a head skin
 * (cg_players.c: CG_RegisterClientSkin / CG_RegisterClientModelname), and the AI
 * needs an animation script that has the AI states (RELAXED/ALERT/COMBAT), or the
 * character would stand in its bind pose.  Skin names are NOT assumed: they are
 * discovered from the virtual file system (all pk3 files and mod directories, with
 * the engine's own search order) and the first usable one is used.
 *
 * Nothing here touches the file system; the caller reads the files and fills the
 * struct so the same code runs on the host in tests/vr_gameplay.
 * ---------------------------------------------------------------------- */
#define VRMERC_NAME_LEN     64
#define VRMERC_MAX_SKINS    16

/* Bits of the animation-script state mask (STATE <name> { ... } blocks). */
#define VRMERC_STATE_RELAXED    1
#define VRMERC_STATE_QUERY      2
#define VRMERC_STATE_ALERT      4
#define VRMERC_STATE_COMBAT     8
#define VRMERC_STATES_REQUIRED  ( VRMERC_STATE_RELAXED | VRMERC_STATE_ALERT | VRMERC_STATE_COMBAT )

typedef struct {
	int hasAnimCfg;         /* wolfanim.cfg                                   */
	int cfgVersion;         /* "version" value declared by the cfg (0 if none) */
	int hasAnimScript;      /* wolfanim.script                                */
	int hasDefaultScript;   /* models/players/default.script (legacy models)  */
	int scriptStateMask;    /* VRMERC_STATE_* found in the script that will be used */
	int hasBodySkin;        /* body_<skin>.skin (skeletal)                    */
	int hasBodyMesh;        /* body.mds                                       */
	int hasLowerSkin;       /* lower_<skin>.skin (legacy md3 models)          */
	int hasUpperSkin;       /* upper_<skin>.skin                              */
	int hasLowerMesh;       /* lower.md3                                      */
	int hasUpperMesh;       /* upper.md3                                      */
	int hasHeadSkin;        /* head_<headskin>.skin                           */
} vrMercModelFiles_t;

typedef enum {
	VRMERC_MODEL_OK = 0,
	VRMERC_MODEL_NO_ANIM_CFG,
	VRMERC_MODEL_NO_ANIM_SCRIPT,
	VRMERC_MODEL_NO_AI_STATES,  /* script exists but has no RELAXED/ALERT/COMBAT AI states */
	VRMERC_MODEL_NO_BODY,       /* neither a complete skeletal nor a complete md3 body */
	VRMERC_MODEL_NO_HEAD_SKIN
} vrMercModelProblem_t;

/* The loader's own rule for the animation script (g_client.c G_ParseAnimationFiles). */
static inline int VrMerc_AnimScriptAvailable( const vrMercModelFiles_t *f ) {
	if ( f->hasAnimScript ) {
		return 1;
	}
	return ( f->cfgVersion <= 1 && f->hasDefaultScript ) ? 1 : 0;
}

static inline int VrMerc_ModelProblem( const vrMercModelFiles_t *f ) {
	int skeletal = f->hasBodySkin && f->hasBodyMesh;
	int legacy = f->hasLowerSkin && f->hasUpperSkin && f->hasLowerMesh && f->hasUpperMesh;

	if ( !f->hasAnimCfg ) {
		return VRMERC_MODEL_NO_ANIM_CFG;
	}
	if ( !VrMerc_AnimScriptAvailable( f ) ) {
		return VRMERC_MODEL_NO_ANIM_SCRIPT;
	}
	if ( ( f->scriptStateMask & VRMERC_STATES_REQUIRED ) != VRMERC_STATES_REQUIRED ) {
		return VRMERC_MODEL_NO_AI_STATES;
	}
	if ( !skeletal && !legacy ) {
		return VRMERC_MODEL_NO_BODY;
	}
	if ( !f->hasHeadSkin ) {
		return VRMERC_MODEL_NO_HEAD_SKIN;
	}
	return VRMERC_MODEL_OK;
}

/* Candidate selection.  problems[i] is VrMerc_ModelProblem() of candidate i, in
 * preference order.  `preferred` (or -1) is tried first, then the rest in order.
 * Returns the chosen index or -1 if no candidate is usable. */
static inline int VrMerc_PickFirstViable( const int *problems, int count, int preferred ) {
	int i;

	if ( preferred >= 0 && preferred < count && problems[preferred] == VRMERC_MODEL_OK ) {
		return preferred;
	}
	for ( i = 0; i < count; i++ ) {
		if ( problems[i] == VRMERC_MODEL_OK ) {
			return i;
		}
	}
	return -1;
}

/* ---- tiny case-insensitive helpers (no libc locale dependence) ---- */
static inline int VrMerc_Lower( int c ) {
	return ( c >= 'A' && c <= 'Z' ) ? c + 32 : c;
}

static inline int VrMerc_StrEqNoCase( const char *a, const char *b ) {
	while ( *a && *b ) {
		if ( VrMerc_Lower( (unsigned char)*a ) != VrMerc_Lower( (unsigned char)*b ) ) {
			return 0;
		}
		a++;
		b++;
	}
	return *a == *b;
}

/* ---- skin discovery -------------------------------------------------------
 * `list` is what trap_FS_GetFileList returns: `count` consecutive NUL-terminated
 * names ("body_default.skin", ...).  Extracts <name> from <prefix><name>.skin. */
static inline int VrMerc_SkinNamesFromList( const char *list, int count, const char *prefix,
											char names[][VRMERC_NAME_LEN], int maxNames ) {
	int found = 0, i, plen = 0;

	while ( prefix[plen] ) {
		plen++;
	}
	for ( i = 0; i < count && found < maxNames; i++ ) {
		int len = 0, k;

		while ( list[len] ) {
			len++;
		}
		if ( len > plen + 5 && len - plen - 5 < VRMERC_NAME_LEN ) {
			int match = 1;

			for ( k = 0; k < plen && match; k++ ) {
				if ( VrMerc_Lower( (unsigned char)list[k] ) != VrMerc_Lower( (unsigned char)prefix[k] ) ) {
					match = 0;
				}
			}
			if ( match && VrMerc_StrEqNoCase( list + len - 5, ".skin" ) ) {
				int n = len - plen - 5;

				for ( k = 0; k < n; k++ ) {
					names[found][k] = list[plen + k];
				}
				names[found][n] = 0;
				found++;
			}
		}
		list += len + 1;
	}
	return found;
}

/* Index of "default" if present, otherwise 0; -1 for an empty list. */
static inline int VrMerc_PickSkinIndex( char names[][VRMERC_NAME_LEN], int count ) {
	int i;

	for ( i = 0; i < count; i++ ) {
		if ( VrMerc_StrEqNoCase( names[i], "default" ) ) {
			return i;
		}
	}
	return count > 0 ? 0 : -1;
}

/* Legacy md3 characters need lower_X and upper_X with the same X.  Returns the index
 * into `lower` of the common name (default preferred), or -1. */
static inline int VrMerc_PickCommonSkin( char lower[][VRMERC_NAME_LEN], int nLower,
										 char upper[][VRMERC_NAME_LEN], int nUpper ) {
	int i, j, first = -1;

	for ( i = 0; i < nLower; i++ ) {
		for ( j = 0; j < nUpper; j++ ) {
			if ( VrMerc_StrEqNoCase( lower[i], upper[j] ) ) {
				if ( VrMerc_StrEqNoCase( lower[i], "default" ) ) {
					return i;
				}
				if ( first < 0 ) {
					first = i;
				}
			}
		}
	}
	return first;
}

/* ---- text scanning of wolfanim.cfg / wolfanim.script ---------------------- */
static inline int VrMerc_IsSpace( int c ) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Next whitespace-separated token; skips // and C comments.  Returns its length (0 at end). */
static inline int VrMerc_NextToken( const char **p, char *out, int outSize ) {
	const char *s = *p;
	int n = 0;

	for ( ;; ) {
		while ( *s && VrMerc_IsSpace( (unsigned char)*s ) ) {
			s++;
		}
		if ( s[0] == '/' && s[1] == '/' ) {
			while ( *s && *s != '\n' ) {
				s++;
			}
			continue;
		}
		if ( s[0] == '/' && s[1] == '*' ) {
			s += 2;
			while ( *s && !( s[0] == '*' && s[1] == '/' ) ) {
				s++;
			}
			if ( *s ) {
				s += 2;
			}
			continue;
		}
		break;
	}
	while ( *s && !VrMerc_IsSpace( (unsigned char)*s ) && ( *s != '{' && *s != '}' ) && n < outSize - 1 ) {
		out[n++] = *s++;
	}
	if ( n == 0 && ( *s == '{' || *s == '}' ) ) {
		out[n++] = *s++;
	}
	out[n] = 0;
	*p = s;
	return n;
}

/* Value of the cfg "version" token (the parser's default is 0). */
static inline int VrMerc_CfgVersion( const char *text ) {
	const char *p = text;
	char tok[VRMERC_NAME_LEN];

	while ( VrMerc_NextToken( &p, tok, sizeof( tok ) ) ) {
		if ( VrMerc_StrEqNoCase( tok, "version" ) ) {
			if ( VrMerc_NextToken( &p, tok, sizeof( tok ) ) ) {
				int v = 0, i = 0, neg = 0;

				if ( tok[0] == '-' ) {
					neg = 1;
					i = 1;
				}
				for ( ; tok[i] >= '0' && tok[i] <= '9'; i++ ) {
					v = v * 10 + ( tok[i] - '0' );
				}
				return neg ? -v : v;
			}
			return 0;
		}
	}
	return 0;
}

/* Which AI states the script defines ("STATE <name> {" blocks). */
static inline int VrMerc_ScriptStateMask( const char *text ) {
	const char *p = text;
	char tok[VRMERC_NAME_LEN];
	int mask = 0, afterState = 0;

	while ( VrMerc_NextToken( &p, tok, sizeof( tok ) ) ) {
		if ( afterState ) {
			if ( VrMerc_StrEqNoCase( tok, "relaxed" ) ) {
				mask |= VRMERC_STATE_RELAXED;
			} else if ( VrMerc_StrEqNoCase( tok, "query" ) ) {
				mask |= VRMERC_STATE_QUERY;
			} else if ( VrMerc_StrEqNoCase( tok, "alert" ) ) {
				mask |= VRMERC_STATE_ALERT;
			} else if ( VrMerc_StrEqNoCase( tok, "combat" ) ) {
				mask |= VRMERC_STATE_COMBAT;
			}
			afterState = 0;
		} else if ( VrMerc_StrEqNoCase( tok, "state" ) ) {
			afterState = 1;
		}
	}
	return mask;
}

/* ---- exact missing-resource report ---------------------------------------- */
static inline int VrMerc_Append( char *out, int outSize, int len, const char *a, const char *b, const char *c ) {
	const char *parts[3];
	int i;

	parts[0] = a;
	parts[1] = b;
	parts[2] = c;
	for ( i = 0; i < 3; i++ ) {
		const char *s = parts[i];

		while ( s && *s && len < outSize - 1 ) {
			out[len++] = *s++;
		}
	}
	out[len] = 0;
	return len;
}

/* Writes "models/players/<model>/<file>; ..." for everything that is missing.  `bodySkin`
 * and `headSkin` are the names that were tried (the discovered ones, or "default" when
 * nothing was found).  Returns the number of missing items. */
static inline int VrMerc_DescribeMissing( const char *model, const char *bodySkin, const char *headSkin,
										  const vrMercModelFiles_t *f, char *out, int outSize ) {
	int len = 0, count = 0;
	int skeletal = f->hasBodySkin && f->hasBodyMesh;
	int legacy = f->hasLowerSkin && f->hasUpperSkin && f->hasLowerMesh && f->hasUpperMesh;

	out[0] = 0;
#define VRMERC_MISSING( cond, mid, name, suffix ) \
	if ( cond ) { \
		len = VrMerc_Append( out, outSize, len, count ? "; " : "", "models/players/", model ); \
		len = VrMerc_Append( out, outSize, len, "/", mid, name ); \
		len = VrMerc_Append( out, outSize, len, suffix, "", "" ); \
		count++; \
	}
	VRMERC_MISSING( !f->hasAnimCfg, "", "wolfanim.cfg", "" )
	VRMERC_MISSING( !VrMerc_AnimScriptAvailable( f ), "", "wolfanim.script", "" )
	if ( !skeletal && !legacy ) {
		VRMERC_MISSING( !f->hasBodySkin, "body_", bodySkin, ".skin" )
		VRMERC_MISSING( !f->hasBodyMesh, "", "body.mds", "" )
	}
	VRMERC_MISSING( !f->hasHeadSkin, "head_", headSkin, ".skin" )
#undef VRMERC_MISSING
	if ( f->hasAnimCfg && VrMerc_AnimScriptAvailable( f ) &&
		 ( f->scriptStateMask & VRMERC_STATES_REQUIRED ) != VRMERC_STATES_REQUIRED ) {
		len = VrMerc_Append( out, outSize, len, count ? "; " : "", "animation script has no RELAXED/ALERT/COMBAT AI states", "" );
		count++;
	}
	return count;
}

/* A resting surface is walkable when its normal is steep enough. */
static inline int VrMerc_GroundWalkable( float normalZ ) {
	return normalZ >= 0.7f;
}

#endif /* G_VRLOGIC_H */
