/*
===========================================================================
g_vrsquad.h -- pure bookkeeping and formation logic for the mercenary squad
(up to VRMERC_MAX_SQUAD independent companions).  Plain C, static inline, no
engine includes; built by tests/vr_squad together with the real game module.
===========================================================================
*/

#ifndef G_VRSQUAD_H
#define G_VRSQUAD_H

#include <math.h>
#include "g_vrtactics.h"

#define VRMERC_MAX_SQUAD        4       /* hard ceiling of the instance array                 */
#define VRSQUAD_DEFAULT_LIMIT   2       /* default for the cvar vr_merc_squad                 */

/* Squad limit from the cvar: 0 or garbage -> default, clamped to 1..VRMERC_MAX_SQUAD. */
static inline int VrSquad_Limit( int cvarValue ) {
	if ( cvarValue <= 0 ) {
		return VRSQUAD_DEFAULT_LIMIT;
	}
	return cvarValue > VRMERC_MAX_SQUAD ? VRMERC_MAX_SQUAD : cvarValue;
}

typedef enum {
	VRSQ_OK = 0,
	VRSQ_FULL,              /* the configured squad limit is reached        */
	VRSQ_PENDING,           /* another recruit is still on the way          */
	VRSQ_NO_INSTANCE        /* no free instance record (cannot happen below the ceiling) */
} vrSquadHire_t;

/* May one more companion be recruited right now? */
static inline int VrSquad_CanAdd( int liveCount, int pending, int limit ) {
	if ( pending ) {
		return VRSQ_PENDING;
	}
	if ( liveCount >= limit ) {
		return VRSQ_FULL;
	}
	if ( liveCount >= VRMERC_MAX_SQUAD ) {
		return VRSQ_NO_INSTANCE;
	}
	return VRSQ_OK;
}

/* First instance index that is not in use, or -1. */
static inline int VrSquad_PickFreeInstance( const int *inUse, int n ) {
	int i;

	for ( i = 0; i < n && i < VRMERC_MAX_SQUAD; i++ ) {
		if ( !inUse[i] ) {
			return i;
		}
	}
	return -1;
}

/* ---- identity: one unique AI name per instance ---- */
static inline const char *VrSquad_AiName( int idx ) {
	static const char *names[VRMERC_MAX_SQUAD] = { "vr_merc_1", "vr_merc_2", "vr_merc_3", "vr_merc_4" };

	return ( idx >= 0 && idx < VRMERC_MAX_SQUAD ) ? names[idx] : "";
}

/* Instance index of an AI name ("vr_merc_<1..4>"), or -1 if the name is not a companion's. */
static inline int VrSquad_IdxFromName( const char *name ) {
	static const char prefix[] = "vr_merc_";
	int i, n;

	if ( !name ) {
		return -1;
	}
	for ( i = 0; prefix[i]; i++ ) {
		if ( name[i] != prefix[i] ) {
			return -1;
		}
	}
	n = name[i] - '0';
	if ( n < 1 || n > VRMERC_MAX_SQUAD || name[i + 1] != 0 ) {
		return -1;
	}
	return n - 1;
}

/* ---- addressing: which companions does a command apply to? ---- */
typedef enum {
	VRSQ_SEL_OK = 0,
	VRSQ_SEL_BAD_ARG,       /* not "all" or a number 1..VRMERC_MAX_SQUAD */
	VRSQ_SEL_NO_SUCH,       /* that companion is not in the squad        */
	VRSQ_SEL_NONE_ALIVE     /* nobody to address                         */
} vrSquadSel_t;

/* arg: the explicit argument ("" = none, "all", "1".."4").  cvarSel: the notebook's selector
 * (0 = all, 1..4 = one companion).  liveMask: bit i set = instance i is in the squad.
 * Writes the addressed instances to *outMask. */
static inline int VrSquad_ResolveSelection( const char *arg, int cvarSel, int liveMask, int *outMask ) {
	int sel;

	*outMask = 0;
	if ( arg && arg[0] ) {
		if ( ( arg[0] == 'a' || arg[0] == 'A' ) && ( arg[1] == 'l' || arg[1] == 'L' ) &&
			 ( arg[2] == 'l' || arg[2] == 'L' ) && arg[3] == 0 ) {
			sel = 0;
		} else if ( arg[0] >= '1' && arg[0] <= '0' + VRMERC_MAX_SQUAD && arg[1] == 0 ) {
			sel = arg[0] - '0';
		} else {
			return VRSQ_SEL_BAD_ARG;
		}
	} else {
		sel = ( cvarSel >= 1 && cvarSel <= VRMERC_MAX_SQUAD ) ? cvarSel : 0;
	}
	if ( sel == 0 ) {
		*outMask = liveMask & ( ( 1 << VRMERC_MAX_SQUAD ) - 1 );
		return *outMask ? VRSQ_SEL_OK : VRSQ_SEL_NONE_ALIVE;
	}
	if ( !( liveMask & ( 1 << ( sel - 1 ) ) ) ) {
		return VRSQ_SEL_NO_SUCH;
	}
	*outMask = 1 << ( sel - 1 );
	return VRSQ_SEL_OK;
}

static inline int VrSquad_CountBits( int mask ) {
	int n = 0;

	for ( ; mask; mask >>= 1 ) {
		n += mask & 1;
	}
	return n;
}

/* Position among the living companions (0 = lowest instance index). */
static inline int VrSquad_Rank( int liveMask, int idx ) {
	int i, rank = 0;

	for ( i = 0; i < idx && i < VRMERC_MAX_SQUAD; i++ ) {
		rank += ( liveMask >> i ) & 1;
	}
	return rank;
}

/* ---- formation: approximate roles, not rigid positions ---- */
#define VRSQUAD_NARROW_WIDTH    220.0f  /* left + right clearance below this = corridor / doorway */
#define VRSQUAD_MIN_SEPARATION  70.0f   /* closer than this and the higher-numbered soldier steps away */
#define VRSQUAD_SLOT_RADIUS     150.0f  /* farther than this from its slot (player standing) = move  */
#define VRSQUAD_STILL_MS        1200    /* the player stood still this long                        */

typedef enum {
	VRSQ_ROLE_LEFT = 0,     /* companion 1: behind-left              */
	VRSQ_ROLE_RIGHT,        /* companion 2: behind-right             */
	VRSQ_ROLE_REAR,         /* companion 3: rear, centred            */
	VRSQ_ROLE_WIDE_LEFT     /* companion 4: rear, wide on the left   */
} vrSquadRole_t;

/* Slot for a role.  In a narrow passage everybody lines up single file behind the player. */
static inline void VrSquad_Slot( const float *player, float yawDeg, int role, int narrow, float *out ) {
	float back, lateral;
	int side;

	if ( role < 0 ) {
		role = 0;
	}
	if ( narrow ) {
		back = 120.0f + 100.0f * (float)role;
		lateral = 0.0f;
		side = 1;
	} else {
		switch ( role ) {
		case VRSQ_ROLE_LEFT:        back = 130.0f; lateral = 90.0f;  side = -1; break;
		case VRSQ_ROLE_RIGHT:       back = 130.0f; lateral = 90.0f;  side = 1;  break;
		case VRSQ_ROLE_REAR:        back = 250.0f; lateral = 0.0f;   side = 1;  break;
		default:                    back = 210.0f; lateral = 200.0f; side = -1; break;
		}
	}
	VrTac_FormationSlot( player, yawDeg, side, back, lateral, out );
}

static inline int VrSquad_IsNarrow( float leftClear, float rightClear ) {
	return ( leftClear + rightClear ) < VRSQUAD_NARROW_WIDTH;
}

typedef enum {
	VRSQ_POS_NONE = 0,
	VRSQ_POS_SLOT,          /* wander back to the formation slot while the player stands */
	VRSQ_POS_SEPARATE       /* too close to a squad mate                                  */
} vrSquadPos_t;

/* Does a companion that is idle near the player need a formation move? */
static inline int VrSquad_PositionNeed( int idx, float distToSlot, int msPlayerStill, int nearestMate, float nearestMateDist ) {
	if ( nearestMate >= 0 && nearestMate < idx && nearestMateDist < VRSQUAD_MIN_SEPARATION ) {
		return VRSQ_POS_SEPARATE;       /* only the higher number moves: no two step away at once */
	}
	if ( msPlayerStill >= VRSQUAD_STILL_MS && distToSlot > VRSQUAD_SLOT_RADIUS ) {
		return VRSQ_POS_SLOT;
	}
	return VRSQ_POS_NONE;
}

/* Does a spawn candidate keep its distance from the squad mates? */
static inline int VrSquad_SpawnClear( const float *cand, const float *mates, int n ) {
	int i;

	for ( i = 0; i < n; i++ ) {
		float dx = cand[0] - mates[i * 3], dy = cand[1] - mates[i * 3 + 1], dz = cand[2] - mates[i * 3 + 2];

		if ( sqrtf( dx * dx + dy * dy + dz * dz ) < VRSQUAD_MIN_SEPARATION ) {
			return 0;
		}
	}
	return 1;
}

/* Companion count to remember after one is removed (death or dismissal). */
static inline int VrSquad_WishAfterRemoval( int liveCount ) {
	return liveCount > 0 ? liveCount - 1 : 0;
}

#endif /* G_VRSQUAD_H */
