/*
===========================================================================
g_vrlogic.h -- pure decision logic for the PICO 4 / Quest gameplay additions
(single-player God Mode).

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

#endif /* G_VRLOGIC_H */
