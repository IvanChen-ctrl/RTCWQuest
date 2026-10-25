/*
===========================================================================
g_vrmove.h -- pure adaptive follow / catch-up / stuck-recovery policy for the
mercenary squad (M4 hardware-feedback round).  Plain C, static inline, no engine
includes: the exact code that runs in the game module is also built by the host
tests (tests/vr_squad).

Why it exists (read from the stock code):
  * AICast limits a character to speedScale (<= 1.2) x RUNNING_SPEED (about 220 for
    the American, replaced every frame by the current run animation's moveSpeed).
    At the default scale that is about 73% of the player's top speed (88% at the
    cap), and nothing in the follow code ever asks for more when the companion is
    far behind.
  * The stock chase of the leader ends as soon as the straight-line distance is
    below 240 when there is no clear line to the leader, and idle only restarts the
    chase beyond 256 straight-line units.  Around a corner the straight-line
    distance is small while the real path is long, so the companion stops and the
    player leaves it behind.
This policy uses the AAS path distance, hysteresis between modes, a speed floor that
is only applied to the companion, and a bounded recovery ladder.
===========================================================================
*/

#ifndef G_VRMOVE_H
#define G_VRMOVE_H

#include <math.h>

/* ------------------------------------------------------------------------
 * Movement modes with hysteresis
 * ---------------------------------------------------------------------- */
typedef enum {
	VRMOVE_NEAR = 0,    /* close enough: stock behaviour and the formation logic         */
	VRMOVE_RUN,         /* keep formation: forced chase at the stock top speed (1.2x)    */
	VRMOVE_CATCHUP      /* far behind or out of sight: forced chase at the player's pace */
} vrMoveMode_t;

#define VRMOVE_RUN_ENTER_MOVING     240.0f  /* player walking away: start running      */
#define VRMOVE_RUN_ENTER_STILL      320.0f  /* player standing: no need to hurry       */
#define VRMOVE_RUN_EXIT             190.0f
#define VRMOVE_CATCHUP_ENTER        520.0f
#define VRMOVE_CATCHUP_ENTER_NOLOS  300.0f  /* out of sight: the path is probably longer */
#define VRMOVE_CATCHUP_EXIT         360.0f
#define VRMOVE_CATCHUP_EXIT_NOLOS   240.0f
#define VRMOVE_MIN_DWELL_MS         1500    /* never drop to a slower mode sooner      */
#define VRMOVE_PLAYER_MOVING        70.0f   /* units/s that count as "the player moves" */
#define VRMOVE_CHASE_REACH_RUN      140.0f
#define VRMOVE_CHASE_REACH_CATCHUP  110.0f
#define VRMOVE_PLAYER_TOP_SPEED     300.0f  /* the speed the companion's run is scaled to */
#define VRMOVE_RUN_SCALE            1.2f    /* the stock cap                           */
#define VRMOVE_CATCHUP_SCALE_MAX    1.6f
#define VRMOVE_UNREACHABLE_DIST     99999.0f

typedef struct {
	int mode;           /* vrMoveMode_t */
	int since;          /* level time the current mode was entered */
} vrMove_t;

typedef struct {
	float euclid;       /* straight distance companion -> player                      */
	float pathDist;     /* AAS path estimate in units, < 0 = unknown, >= 99999 = no route */
	int   hasLOS;       /* clear line to the player                                   */
	float playerSpeed;  /* horizontal player speed, units per second                  */
	int   suppressed;   /* combat/maneuver/hold/script owns the movement              */
} vrMoveIn_t;

static inline void VrMove_Reset( vrMove_t *m, int now ) {
	m->mode = VRMOVE_NEAR;
	m->since = now;
}

/* Distance used by the policy: the larger of the straight and the path distance. */
static inline float VrMove_EffectiveDist( float euclid, float pathDist ) {
	if ( pathDist >= VRMOVE_UNREACHABLE_DIST ) {
		return VRMOVE_UNREACHABLE_DIST;
	}
	return ( pathDist > euclid ) ? pathDist : euclid;
}

/* One step.  Returns the (possibly new) mode. */
static inline int VrMove_Update( vrMove_t *m, const vrMoveIn_t *in, int now ) {
	float d = VrMove_EffectiveDist( in->euclid, in->pathDist );
	int moving = in->playerSpeed >= VRMOVE_PLAYER_MOVING;
	int target = m->mode;

	if ( in->suppressed ) {
		target = VRMOVE_NEAR;       /* combat decisions own the movement; drops at once */
	} else {
		int wantCatchup = ( d > VRMOVE_CATCHUP_ENTER ) || ( !in->hasLOS && d > VRMOVE_CATCHUP_ENTER_NOLOS );
		int wantRun = d > ( moving ? VRMOVE_RUN_ENTER_MOVING : VRMOVE_RUN_ENTER_STILL );

		if ( wantCatchup ) {
			target = VRMOVE_CATCHUP;                        /* upgrades are immediate */
		} else if ( m->mode == VRMOVE_NEAR && wantRun ) {
			target = VRMOVE_RUN;
		} else if ( now - m->since >= VRMOVE_MIN_DWELL_MS ) {
			if ( m->mode == VRMOVE_CATCHUP ) {
				if ( d < VRMOVE_RUN_EXIT ) {
					target = VRMOVE_NEAR;
				} else if ( d < ( in->hasLOS ? VRMOVE_CATCHUP_EXIT : VRMOVE_CATCHUP_EXIT_NOLOS ) ) {
					target = VRMOVE_RUN;
				}
			} else if ( m->mode == VRMOVE_RUN && d < VRMOVE_RUN_EXIT ) {
				target = VRMOVE_NEAR;
			}
		}
	}
	if ( target != m->mode ) {
		m->mode = target;
		m->since = now;
	}
	return m->mode;
}

/* speedScale the AI is guaranteed to get in this mode (0 = leave the stock value). */
static inline float VrMove_SpeedFloor( int mode, float runSpeed ) {
	float s;

	if ( mode == VRMOVE_RUN ) {
		return VRMOVE_RUN_SCALE;
	}
	if ( mode == VRMOVE_CATCHUP ) {
		if ( runSpeed < 50.0f ) {
			return VRMOVE_RUN_SCALE;        /* implausible animation data: do not over-boost */
		}
		s = VRMOVE_PLAYER_TOP_SPEED / runSpeed;
		if ( s < VRMOVE_RUN_SCALE ) {
			s = VRMOVE_RUN_SCALE;
		}
		return s > VRMOVE_CATCHUP_SCALE_MAX ? VRMOVE_CATCHUP_SCALE_MAX : s;
	}
	return 0.0f;
}

static inline int VrMove_ForcesChase( int mode ) {
	return mode == VRMOVE_RUN || mode == VRMOVE_CATCHUP;
}

static inline float VrMove_ChaseReach( int mode ) {
	return mode == VRMOVE_CATCHUP ? VRMOVE_CHASE_REACH_CATCHUP : VRMOVE_CHASE_REACH_RUN;
}

/* AAS travel time (1/100 s) -> path distance in units. */
static inline float VrMove_PathDistFromTravel( int travelTime, int sameArea, float euclid ) {
	if ( sameArea ) {
		return euclid;
	}
	if ( travelTime <= 0 ) {
		return VRMOVE_UNREACHABLE_DIST;
	}
	return (float)travelTime * ( VRMOVE_PLAYER_TOP_SPEED / 100.0f );
}

/* ------------------------------------------------------------------------
 * Stuck detection and the recovery ladder
 * ---------------------------------------------------------------------- */
#define VRMOVE_STUCK_MS             2500    /* no real displacement for this long       */
#define VRMOVE_STUCK_MIN_MOVE       48.0f
#define VRMOVE_RECOVER_GAP_MS       2500    /* min time between ladder steps            */
#define VRMOVE_RECOVER_WAIT_MS      8000    /* last step: wait before a new episode     */
#define VRMOVE_RECOVER_FORGET_MS    6000    /* moving normally this long clears the count */
#define VRMOVE_TRAIL_MAX_MS         20000   /* a crumb older than this is not used      */

typedef enum {
	VRREC_NONE = 0,
	VRREC_REPATH,       /* reset the AI's avoided areas and re-issue the chase           */
	VRREC_TRAIL,        /* walk to a reachable point on the player's recent path         */
	VRREC_WAIT          /* give up for now (bounded), then start a new episode           */
} vrRecoverAction_t;

typedef struct {
	float checkPos[3];
	int   checkTime;
	int   attempts;         /* ladder steps used in this episode            */
	int   lastAction;       /* time of the last step                        */
	int   waitUntil;        /* > now: waiting (VRREC_WAIT)                  */
	int   lastGoodMove;     /* last time the companion was seen moving      */
} vrRecover_t;

static inline void VrRecover_Reset( vrRecover_t *r, const float *pos, int now ) {
	r->checkPos[0] = pos[0];
	r->checkPos[1] = pos[1];
	r->checkPos[2] = pos[2];
	r->checkTime = now;
	r->attempts = 0;
	r->lastAction = -1000000;
	r->waitUntil = 0;
	r->lastGoodMove = now;
}

/* Called while the companion is supposed to be travelling (RUN / CATCHUP, farther than
 * `needMove` from the player).  Returns the next ladder step or VRREC_NONE.  `travelling`
 * false (near the player, blocked by order) clears the observation window. */
static inline int VrRecover_Step( vrRecover_t *r, const float *pos, int travelling, int now ) {
	float dx = pos[0] - r->checkPos[0], dy = pos[1] - r->checkPos[1], dz = pos[2] - r->checkPos[2];
	float moved = sqrtf( dx * dx + dy * dy + dz * dz );

	if ( r->waitUntil ) {
		if ( now < r->waitUntil ) {
			return VRREC_NONE;                  /* waiting out an episode */
		}
		r->waitUntil = 0;                       /* the wait is over: a fresh episode */
		r->attempts = 0;
		r->checkPos[0] = pos[0]; r->checkPos[1] = pos[1]; r->checkPos[2] = pos[2];
		r->checkTime = now;
		return VRREC_NONE;
	}
	if ( !travelling || moved >= VRMOVE_STUCK_MIN_MOVE ) {
		/* not expected to move, or it moved: restart the observation window */
		r->checkPos[0] = pos[0]; r->checkPos[1] = pos[1]; r->checkPos[2] = pos[2];
		r->checkTime = now;
		if ( travelling ) {
			r->lastGoodMove = now;
		}
		/* a long spell without a ladder step forgets earlier failures */
		if ( r->attempts && now - r->lastAction >= VRMOVE_RECOVER_FORGET_MS ) {
			r->attempts = 0;
		}
		return VRREC_NONE;
	}
	if ( now - r->checkTime < VRMOVE_STUCK_MS || now - r->lastAction < VRMOVE_RECOVER_GAP_MS ) {
		return VRREC_NONE;
	}
	/* stuck: escalate one step */
	r->lastAction = now;
	r->checkTime = now;
	r->attempts++;
	if ( r->attempts == 1 ) {
		return VRREC_REPATH;
	}
	if ( r->attempts == 2 ) {
		return VRREC_TRAIL;
	}
	r->waitUntil = now + VRMOVE_RECOVER_WAIT_MS;
	return VRREC_WAIT;
}

static inline const char *VrRecover_ActionName( int a ) {
	switch ( a ) {
	case VRREC_REPATH:  return "repath";
	case VRREC_TRAIL:   return "trail";
	case VRREC_WAIT:    return "wait";
	default:            return "none";
	}
}

/* ------------------------------------------------------------------------
 * Breadcrumb trail of the player's recent walkable path
 * ---------------------------------------------------------------------- */
#define VRTRAIL_MAX         12
#define VRTRAIL_MIN_DIST    120.0f
#define VRTRAIL_MIN_MS      800

typedef struct {
	float pos[VRTRAIL_MAX][3];
	int   time[VRTRAIL_MAX];
	int   count;
	int   head;             /* next write index */
} vrTrail_t;

static inline void VrTrail_Reset( vrTrail_t *t ) {
	t->count = 0;
	t->head = 0;
}

/* Records `pos` if the player moved far enough and long enough since the last crumb. */
static inline int VrTrail_Add( vrTrail_t *t, const float *pos, int now ) {
	if ( t->count > 0 ) {
		int last = ( t->head + VRTRAIL_MAX - 1 ) % VRTRAIL_MAX;
		float dx = pos[0] - t->pos[last][0], dy = pos[1] - t->pos[last][1], dz = pos[2] - t->pos[last][2];

		if ( sqrtf( dx * dx + dy * dy + dz * dz ) < VRTRAIL_MIN_DIST || now - t->time[last] < VRTRAIL_MIN_MS ) {
			return 0;
		}
	}
	t->pos[t->head][0] = pos[0];
	t->pos[t->head][1] = pos[1];
	t->pos[t->head][2] = pos[2];
	t->time[t->head] = now;
	t->head = ( t->head + 1 ) % VRTRAIL_MAX;
	if ( t->count < VRTRAIL_MAX ) {
		t->count++;
	}
	return 1;
}

/* k = 0 is the newest crumb.  Returns the slot index or -1. */
static inline int VrTrail_Index( const vrTrail_t *t, int k ) {
	if ( k < 0 || k >= t->count ) {
		return -1;
	}
	return ( t->head + VRTRAIL_MAX - 1 - k ) % VRTRAIL_MAX;
}

/* Of the crumbs (newest first) pick the first one that is recent enough and reachable.
 * reachable[k] is the AAS travel time to the k-th newest crumb (0 = unreachable), minDistOk[k]
 * is 1 when the crumb is far enough from the companion to be worth walking to.  Returns k or -1. */
static inline int VrTrail_Pick( const vrTrail_t *t, int now, const int *travelTime, const int *farEnough ) {
	int k;

	for ( k = 0; k < t->count; k++ ) {
		int idx = VrTrail_Index( t, k );

		if ( idx < 0 || now - t->time[idx] > VRMOVE_TRAIL_MAX_MS ) {
			continue;
		}
		if ( travelTime[k] > 0 && farEnough[k] ) {
			return k;
		}
	}
	return -1;
}

#endif /* G_VRMOVE_H */
