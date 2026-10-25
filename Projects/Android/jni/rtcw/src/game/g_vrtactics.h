/*
===========================================================================
g_vrtactics.h -- pure tactical decision logic for the AI mercenary companion (M4).

Like g_vrlogic.h this header uses only plain C types and static inline functions,
so the exact code that runs in the game module (g_merc.c) is also built and tested
on a host machine (tests/vr_tactics).  No engine includes.

What lives here is the "what should the companion do" layer: command validation,
threat scoring, target selection with hysteresis, friendly-fire line checks,
formation spacing, cover and flank candidate evaluation, and the entry / exit
rules of the tactical maneuvers.  How the companion moves, aims, animates and
fires stays with the stock AICast code; g_merc.c turns these decisions into
stock AI calls.

Everything is deterministic and allocation free.  Distances are game units
(about one inch), times are milliseconds.
===========================================================================
*/

#ifndef G_VRTACTICS_H
#define G_VRTACTICS_H

#include <math.h>
#include "g_vrlogic.h"

/* ------------------------------------------------------------------------
 * Tuning constants (budgets and thresholds)
 * ---------------------------------------------------------------------- */
#define VRTAC_PERCEPTION_MS         250     /* candidate scan + target choice interval */
#define VRTAC_MAX_CAND              8       /* hostile candidates kept per scan        */
#define VRTAC_MAX_FRIENDS           8       /* friendly positions kept per scan        */
#define VRTAC_STALE_MS              6000    /* a sighting older than this is forgotten */
#define VRTAC_VISIBLE_MS            700     /* "visible now" = real sighting this recent */
#define VRTAC_TARGET_COMMIT_MS      2500    /* minimum time on a target unless emergency */
#define VRTAC_SWITCH_RATIO          1.30f   /* newcomer must beat the current score by 30%... */
#define VRTAC_SWITCH_ABS            12.0f   /* ...and by this many points                */
#define VRTAC_EMERGENCY_MARGIN      90.0f   /* ...or by this much to override the commitment */
#define VRTAC_REASSERT_MS           1500    /* min time between forced stock-target overrides */

#define VRTAC_DESIGNATE_RANGE       2400.0f /* "attack my target" selection range      */
#define VRTAC_DESIGNATE_ARM_MS      15000   /* time to aim and fire after arming       */
#define VRTAC_DESIGNATE_HOLD_MS     30000   /* a designation expires after this        */
#define VRTAC_COVER_ME_MS           20000   /* "cover me" duration                     */

#define VRTAC_FF_RADIUS_PLAYER      44.0f   /* keep this clear of the player's body    */
#define VRTAC_FF_RADIUS_ALLY        36.0f
#define VRTAC_FF_HOLD_MS            450     /* hold fire this long per blocked sample  */
#define VRTAC_FF_LOG_MS             2000    /* friendly-fire log rate limit            */

#define VRTAC_FORM_CHECK_MS         750     /* formation check interval                */
#define VRTAC_FORM_COOLDOWN_MS      4000    /* min time between formation moves        */
#define VRTAC_FORM_BACK             130.0f  /* slot distance behind the player         */
#define VRTAC_FORM_SIDE             90.0f   /* slot distance to the side               */
#define VRTAC_FORM_MIN_DIST         56.0f   /* closer than this = in the player's way  */
#define VRTAC_FORM_FRONT_MAX        420.0f  /* in-front rule applies up to this range  */
#define VRTAC_FORM_FRONT_COS        0.91f   /* about 24 degrees around the aim line    */
#define VRTAC_FORM_REACH            40.0f
#define VRTAC_FORM_MAX_MS           5000

/* ------------------------------------------------------------------------
 * Reason codes (stable, used in logs and tests)
 * ---------------------------------------------------------------------- */
typedef enum {
	VRTAC_OK = 0,
	/* command validation */
	VRTAC_R_NO_COMPANION,
	VRTAC_R_COMPANION_DEAD,
	VRTAC_R_NOT_ACTIVE,
	VRTAC_R_PLAYER_UNAVAILABLE,
	VRTAC_R_CINEMATIC,
	VRTAC_R_LOADING,
	VRTAC_R_ASSIST_OFF,
	VRTAC_R_UNKNOWN_COMMAND,
	/* target classification */
	VRTAC_R_NO_TARGET,
	VRTAC_R_TARGET_DEAD,
	VRTAC_R_TARGET_PLAYER,
	VRTAC_R_TARGET_SELF,
	VRTAC_R_TARGET_FRIENDLY,
	VRTAC_R_TARGET_NEUTRAL,
	VRTAC_R_TARGET_NOT_AI,
	VRTAC_R_TARGET_RANGE,
	VRTAC_R_TARGET_STALE,
	/* cover */
	VRTAC_R_COVER_SOLID,
	VRTAC_R_COVER_UNREACHABLE,
	VRTAC_R_COVER_TOO_FAR,
	VRTAC_R_COVER_TOO_CLOSE,
	VRTAC_R_COVER_EXPOSED,
	VRTAC_R_COVER_BLOCKS_PLAYER,
	VRTAC_R_COVER_TOWARD_ENEMY,
	VRTAC_R_COVER_HOLD_LEASH,
	VRTAC_R_COVER_NONE,
	/* flank */
	VRTAC_R_FLANK_STANCE,
	VRTAC_R_FLANK_ORDER,
	VRTAC_R_FLANK_COOLDOWN,
	VRTAC_R_FLANK_HEALTH,
	VRTAC_R_FLANK_FAILURES,
	VRTAC_R_FLANK_TARGET_DIST,
	VRTAC_R_FLANK_UNREACHABLE,
	VRTAC_R_FLANK_DETOUR,
	VRTAC_R_FLANK_CROSSES_FIRE,
	VRTAC_R_FLANK_NO_SHOT,
	VRTAC_R_FLANK_SOLID,
	VRTAC_R_FLANK_NONE,
	/* maneuver exits */
	VRTAC_R_DONE,
	VRTAC_R_TIMEOUT,
	VRTAC_R_NAV_STALL,
	VRTAC_R_TARGET_LOST,
	VRTAC_R_ORDER_CHANGED,
	VRTAC_R_HURT,
	VRTAC_R_DISMISSED,
	VRTAC_R_DEAD,
	VRTAC_R_SCRIPT,
	/* maneuver entries */
	VRTAC_R_COMMAND,
	VRTAC_R_SUSTAINED_FIRE,
	VRTAC_R_HEAVY_DAMAGE,
	VRTAC_R_EXPOSED_HOLD,
	VRTAC_R_LOW_HEALTH,
	VRTAC_R_ISOLATED,
	VRTAC_R_LINE_OF_FIRE,
	VRTAC_R_AGGRESSIVE,
	VRTAC_R_CLEAR_BEHIND,
	/* formation and generic navigation checks */
	VRTAC_R_TOO_CLOSE,
	VRTAC_R_IN_FRONT,
	VRTAC_R_NAV_SOLID,
	VRTAC_R_NAV_UNREACHABLE,
	/* base supervisor transitions */
	VRTAC_R_TARGET_SEEN,
	VRTAC_R_FAR,
	VRTAC_R_ARRIVED,
	VRTAC_R_COMBAT_OVER,
	VRTAC_R_GAVE_UP,
	VRTAC_R_COUNT
} vrTacReason_t;

static inline const char *VrTac_ReasonName( int r ) {
	static const char *names[VRTAC_R_COUNT] = {
		"ok",
		"no_companion", "companion_dead", "not_active", "player_unavailable", "cinematic", "loading",
		"assist_off", "unknown_command",
		"no_target", "target_dead", "target_player", "target_self", "target_friendly", "target_neutral",
		"target_not_ai", "target_range", "target_stale",
		"cover_solid", "cover_unreachable", "cover_too_far", "cover_too_close", "cover_exposed",
		"cover_blocks_player", "cover_toward_enemy", "cover_hold_leash", "cover_none",
		"flank_stance", "flank_order", "flank_cooldown", "flank_health", "flank_failures",
		"flank_target_dist", "flank_unreachable", "flank_detour", "flank_crosses_fire",
		"flank_no_shot", "flank_solid", "flank_none",
		"done", "timeout", "nav_stall", "target_lost", "order_changed", "hurt", "dismissed", "dead", "script",
		"command", "sustained_fire", "heavy_damage", "exposed_hold", "low_health", "isolated",
		"line_of_fire", "aggressive", "clear_behind",
		"too_close", "in_front", "nav_solid", "nav_unreachable",
		"target_seen", "too_far", "arrived", "combat_over", "gave_up"
	};

	if ( r < 0 || r >= VRTAC_R_COUNT ) {
		return "unknown";
	}
	return names[r];
}

/* ------------------------------------------------------------------------
 * Stance and commands
 * ---------------------------------------------------------------------- */
typedef enum {
	VRTAC_STANCE_DEFENSIVE = 0,     /* player safety, cover, controlled engagement */
	VRTAC_STANCE_AGGRESSIVE         /* engage visible threats, short flanks         */
} vrTacStance_t;

typedef enum {
	VRTAC_CMD_FOLLOW = 0,
	VRTAC_CMD_HOLD,
	VRTAC_CMD_ATTACK,       /* attack my target (designation)    */
	VRTAC_CMD_COVER_ME,
	VRTAC_CMD_REGROUP,
	VRTAC_CMD_STANCE,
	VRTAC_CMD_ASSIST,
	VRTAC_CMD_DISMISS,
	VRTAC_CMD_COUNT
} vrTacCommand_t;

static inline const char *VrTac_CommandName( int cmd ) {
	static const char *names[VRTAC_CMD_COUNT] = {
		"follow", "hold", "attack", "cover_me", "regroup", "stance", "assist", "dismiss"
	};

	return ( cmd >= 0 && cmd < VRTAC_CMD_COUNT ) ? names[cmd] : "unknown";
}

typedef struct {
	int companionPresent;   /* entity in use */
	int companionAlive;
	int spawnPending;
	int state;              /* vrMercState_t */
	int playerOk;           /* connected, alive human */
	int cinematic;
	int loading;
	int assist;             /* assisting in combat is enabled */
} vrTacOrderCtx_t;

/* Returns VRTAC_OK or the reason a command is refused right now. */
static inline int VrTac_ValidateCommand( int cmd, const vrTacOrderCtx_t *c ) {
	if ( cmd < 0 || cmd >= VRTAC_CMD_COUNT ) {
		return VRTAC_R_UNKNOWN_COMMAND;
	}
	/* stance and assist are plain settings, valid at any time */
	if ( cmd == VRTAC_CMD_STANCE || cmd == VRTAC_CMD_ASSIST ) {
		return VRTAC_OK;
	}
	if ( !c->companionPresent ) {
		return VRTAC_R_NO_COMPANION;
	}
	if ( !c->companionAlive ) {
		return VRTAC_R_COMPANION_DEAD;
	}
	if ( !VrMerc_StateIsActive( c->state ) ) {
		return VRTAC_R_NOT_ACTIVE;
	}
	if ( cmd == VRTAC_CMD_DISMISS || cmd == VRTAC_CMD_FOLLOW || cmd == VRTAC_CMD_HOLD ) {
		return VRTAC_OK;
	}
	/* tactical commands act in the world, so the game must be in a sane state */
	if ( !c->playerOk ) {
		return VRTAC_R_PLAYER_UNAVAILABLE;
	}
	if ( c->cinematic ) {
		return VRTAC_R_CINEMATIC;
	}
	if ( c->loading ) {
		return VRTAC_R_LOADING;
	}
	if ( ( cmd == VRTAC_CMD_ATTACK || cmd == VRTAC_CMD_COVER_ME ) && !c->assist ) {
		return VRTAC_R_ASSIST_OFF;
	}
	return VRTAC_OK;
}

/* ------------------------------------------------------------------------
 * Target classification (friend or foe)
 * ---------------------------------------------------------------------- */

/* VRTAC_OK means "legitimate hostile". Everything else names why the entity is refused.
 * Team numbers follow AITeam_t; the caller passes plain integers. */
static inline int VrTac_ClassifyTarget( int exists, int alive, int isCast, int isPlayer, int isSelf,
										int isCompanion, int mercTeam, int targetTeam, int neutralTeam ) {
	if ( !exists ) {
		return VRTAC_R_NO_TARGET;
	}
	if ( isPlayer ) {
		return VRTAC_R_TARGET_PLAYER;
	}
	if ( isSelf ) {
		return VRTAC_R_TARGET_SELF;
	}
	if ( isCompanion ) {
		return VRTAC_R_TARGET_FRIENDLY;
	}
	if ( !alive ) {
		return VRTAC_R_TARGET_DEAD;
	}
	if ( !isCast ) {
		return VRTAC_R_TARGET_NOT_AI;       /* props, doors, world: not a combat target */
	}
	if ( targetTeam == neutralTeam ) {
		return VRTAC_R_TARGET_NEUTRAL;
	}
	if ( targetTeam == mercTeam ) {
		return VRTAC_R_TARGET_FRIENDLY;
	}
	return VRTAC_OK;
}

/* Designation ("attack my target"): classification plus range. */
static inline int VrTac_ValidateDesignation( int classification, float distFromPlayer ) {
	if ( classification != VRTAC_OK ) {
		return classification;
	}
	if ( distFromPlayer < 0.0f || distFromPlayer > VRTAC_DESIGNATE_RANGE ) {
		return VRTAC_R_TARGET_RANGE;
	}
	return VRTAC_OK;
}

/* ------------------------------------------------------------------------
 * Recent damage window
 * ---------------------------------------------------------------------- */
#define VRTAC_DMG_SLOTS 8

typedef struct {
	int time[VRTAC_DMG_SLOTS];
	int amount[VRTAC_DMG_SLOTS];
	int next;
} vrTacDmgLog_t;

static inline void VrTac_DmgReset( vrTacDmgLog_t *l ) {
	int i;

	for ( i = 0; i < VRTAC_DMG_SLOTS; i++ ) {
		l->time[i] = -1000000;
		l->amount[i] = 0;
	}
	l->next = 0;
}

static inline void VrTac_DmgAdd( vrTacDmgLog_t *l, int now, int amount ) {
	if ( amount <= 0 ) {
		return;
	}
	l->time[l->next] = now;
	l->amount[l->next] = amount;
	l->next = ( l->next + 1 ) % VRTAC_DMG_SLOTS;
}

/* Number of hits and total damage inside the last windowMs. */
static inline void VrTac_DmgWindow( const vrTacDmgLog_t *l, int now, int windowMs, int *hits, int *total ) {
	int i, h = 0, t = 0;

	for ( i = 0; i < VRTAC_DMG_SLOTS; i++ ) {
		if ( now - l->time[i] >= 0 && now - l->time[i] <= windowMs ) {
			h++;
			t += l->amount[i];
		}
	}
	*hits = h;
	*total = t;
}

/* ------------------------------------------------------------------------
 * Threat scoring and target selection
 * ---------------------------------------------------------------------- */
typedef struct {
	int   entNum;
	float dist;             /* companion to target                                      */
	float distToPlayer;
	int   visibleNow;       /* the companion physically saw it a moment ago             */
	int   msSinceSeen;      /* since the last sighting or other legitimate knowledge    */
	int   targetsPlayer;    /* its own AI target is the player (observed while visible) */
	int   targetsSelf;      /* its own AI target is the companion                       */
	int   msSinceHurtPlayer;    /* -1 if it did not damage the player recently          */
	int   msSinceHurtSelf;      /* -1 if it did not damage the companion recently       */
	int   designated;       /* the player pointed at it                                 */
	int   inWeaponRange;
} vrTacCand_t;

typedef struct {
	int stance;             /* vrTacStance_t              */
	int coverMeActive;      /* the player asked for cover */
} vrTacScoreParams_t;

static inline float VrTac_Clamp01( float v ) {
	return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
}

/* Returns a score >= 0 for a usable target, or -1 if the candidate is stale / unusable. */
static inline float VrTac_ThreatScore( const vrTacCand_t *c, const vrTacScoreParams_t *p ) {
	float s = 100.0f;

	if ( !c->designated && c->msSinceSeen > VRTAC_STALE_MS ) {
		return -1.0f;
	}
	s += ( 1.0f - VrTac_Clamp01( c->dist / 1500.0f ) ) * 60.0f;
	if ( c->visibleNow ) {
		s += 40.0f;
	} else {
		s -= VrTac_Clamp01( (float)c->msSinceSeen / (float)VRTAC_STALE_MS ) * 50.0f;
	}
	if ( c->targetsPlayer ) {
		s += 45.0f;
		if ( p->stance == VRTAC_STANCE_DEFENSIVE || p->coverMeActive ) {
			s += 25.0f;     /* player safety comes first */
		}
	}
	if ( c->targetsSelf ) {
		s += 35.0f;
	}
	if ( c->msSinceHurtPlayer >= 0 && c->msSinceHurtPlayer < 3000 ) {
		s += 30.0f;
	}
	if ( c->msSinceHurtSelf >= 0 && c->msSinceHurtSelf < 3000 ) {
		s += 25.0f;
	}
	if ( p->coverMeActive ) {
		/* cover me: protect the player, so threats near the player matter more */
		s += ( 1.0f - VrTac_Clamp01( c->distToPlayer / 1200.0f ) ) * 40.0f;
	}
	if ( p->stance == VRTAC_STANCE_AGGRESSIVE ) {
		s += ( 1.0f - VrTac_Clamp01( c->dist / 900.0f ) ) * 15.0f;     /* nearby enemies first */
	}
	s += c->inWeaponRange ? 15.0f : -25.0f;
	if ( c->designated ) {
		s += 200.0f;
	}
	return s < 0.0f ? 0.0f : s;
}

/* Picks an index into scores[] (-1 = no usable target).
 *   current        index of the committed target, or -1
 *   msOnTarget     time since the last switch
 * A committed target is kept while it is usable unless the commitment time has passed and a
 * newcomer is clearly better (ratio and absolute margin), or a newcomer is overwhelmingly
 * better (emergency, for example a designation or a sudden direct threat).
 * *switched is set when the result differs from `current`. */
static inline int VrTac_SelectTarget( const float *scores, int n, int current, int msOnTarget, int *switched ) {
	int i, best = -1;
	float bestScore = -1.0f;

	for ( i = 0; i < n; i++ ) {
		if ( scores[i] >= 0.0f && scores[i] > bestScore ) {
			bestScore = scores[i];
			best = i;
		}
	}
	*switched = 0;
	if ( best < 0 ) {
		*switched = ( current >= 0 );
		return -1;
	}
	if ( current >= 0 && current < n && scores[current] >= 0.0f ) {
		float cur = scores[current];

		if ( best == current ) {
			return current;
		}
		if ( bestScore >= cur + VRTAC_EMERGENCY_MARGIN ) {
			*switched = 1;
			return best;
		}
		if ( msOnTarget >= VRTAC_TARGET_COMMIT_MS &&
			 bestScore > cur * VRTAC_SWITCH_RATIO && bestScore > cur + VRTAC_SWITCH_ABS ) {
			*switched = 1;
			return best;
		}
		return current;
	}
	*switched = 1;
	return best;
}

/* ------------------------------------------------------------------------
 * Friendly-fire avoidance
 * ---------------------------------------------------------------------- */

/* Distance from point p to the segment a-b (3D). *t receives the projection parameter 0..1. */
static inline float VrTac_PointSegmentDist( const float *a, const float *b, const float *p, float *t ) {
	float ab[3], ap[3], len2, u, dx, dy, dz;
	int i;

	for ( i = 0; i < 3; i++ ) {
		ab[i] = b[i] - a[i];
		ap[i] = p[i] - a[i];
	}
	len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
	if ( len2 < 1e-4f ) {
		*t = 0.0f;
		return sqrtf( ap[0] * ap[0] + ap[1] * ap[1] + ap[2] * ap[2] );
	}
	u = ( ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2] ) / len2;
	u = u < 0.0f ? 0.0f : ( u > 1.0f ? 1.0f : u );
	*t = u;
	dx = a[0] + ab[0] * u - p[0];
	dy = a[1] + ab[1] * u - p[1];
	dz = a[2] + ab[2] * u - p[2];
	return sqrtf( dx * dx + dy * dy + dz * dz );
}

/* 1 if a friendly body (player or ally) stands close to the line from the shooter to the
 * target and between them, so firing would risk hitting it. */
static inline int VrTac_FriendInLineOfFire( const float *shooter, const float *target,
											const float *friendPos, float radius ) {
	float t, d;

	d = VrTac_PointSegmentDist( shooter, target, friendPos, &t );
	if ( d >= radius ) {
		return 0;
	}
	/* a friend behind the shooter cannot be hit; one right at the target is the target's problem */
	return ( t > 0.0f && t < 0.98f ) ? 1 : 0;
}

/* ------------------------------------------------------------------------
 * Follow spacing / formation
 * ---------------------------------------------------------------------- */
typedef enum {
	VRTAC_REPOS_NONE = 0,
	VRTAC_REPOS_TOO_CLOSE,
	VRTAC_REPOS_IN_FRONT
} vrTacRepos_t;

/* Horizontal forward and right unit vectors of a yaw angle in degrees. */
static inline void VrTac_YawBasis( float yawDeg, float *fx, float *fy, float *rx, float *ry ) {
	float r = yawDeg * 3.14159265f / 180.0f;

	*fx = cosf( r );
	*fy = sinf( r );
	*rx = sinf( r );
	*ry = -cosf( r );
}

/* A spot behind and to one side of the player.  side is +1 (right) or -1 (left). */
static inline void VrTac_FormationSlot( const float *player, float yawDeg, int side,
										float back, float lateral, float *out ) {
	float fx, fy, rx, ry;

	VrTac_YawBasis( yawDeg, &fx, &fy, &rx, &ry );
	out[0] = player[0] - fx * back + rx * lateral * (float)side;
	out[1] = player[1] - fy * back + ry * lateral * (float)side;
	out[2] = player[2];
}

/* Which side of the player the companion is already on (so the slot on that side is the
 * shortest move). */
static inline int VrTac_PickSide( const float *self, const float *player, float yawDeg ) {
	float fx, fy, rx, ry;

	VrTac_YawBasis( yawDeg, &fx, &fy, &rx, &ry );
	return ( ( self[0] - player[0] ) * rx + ( self[1] - player[1] ) * ry >= 0.0f ) ? 1 : -1;
}

/* Is the companion in the player's way?  aimX/aimY is the horizontal aim direction of the
 * player's weapon (need not be normalised; (0,0) = unknown, only the distance rule applies). */
static inline int VrTac_RepositionReason( const float *self, const float *player, float aimX, float aimY ) {
	float dx = self[0] - player[0], dy = self[1] - player[1];
	float hd = sqrtf( dx * dx + dy * dy );
	float al = sqrtf( aimX * aimX + aimY * aimY );

	if ( hd < VRTAC_FORM_MIN_DIST ) {
		return VRTAC_REPOS_TOO_CLOSE;
	}
	if ( al > 0.01f && hd <= VRTAC_FORM_FRONT_MAX ) {
		float cosang = ( dx * aimX + dy * aimY ) / ( hd * al );

		if ( cosang > VRTAC_FORM_FRONT_COS ) {
			return VRTAC_REPOS_IN_FRONT;
		}
	}
	return VRTAC_REPOS_NONE;
}

#endif /* G_VRTACTICS_H */
