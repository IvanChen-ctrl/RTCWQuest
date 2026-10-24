/*
===========================================================================
g_vr.c -- single-player God Mode for the PICO 4 / Quest port.

The option is the cvar "vr_godmode" (default 0, not archived, so every launch
starts with it off).  It is switched from the VR notebook ("cheats" page) or
the console:

    vr_godmode 1
    vr_godmode 0

Damage is not rewritten every frame.  G_Damage consults G_VrGodProtects() at
the same place as the stock FL_GODMODE flag, so the existing damage path stays
the only authority and DAMAGE_NO_PROTECTION (telefrag, kill triggers) still
kills.  Enemy AI, notarget and noclip are untouched.

The decision itself is VrGod_Protects() in g_vrlogic.h (host tested).
===========================================================================
*/

#include "g_local.h"
#include "g_vrlogic.h"

/*
==================
G_VrGodProtects

True if the "God Mode" option protects targ from this damage.  Only the local
single-player human can be protected: multiplayer game types, AI characters
and every other entity are never affected, whatever the cvar says.
==================
*/
qboolean G_VrGodProtects( gentity_t *targ, int dflags ) {
	if ( !targ || !targ->client ) {
		return qfalse;
	}
	return VrGod_Protects( g_gametype.integer == GT_SINGLE_PLAYER,
						   vr_godmode.integer != 0,
						   targ->client != NULL,
						   ( targ->r.svFlags & SVF_CASTAI ) != 0,
						   targ->s.number,
						   ( dflags & DAMAGE_NO_PROTECTION ) != 0 ) ? qtrue : qfalse;
}

/*
==================
G_VrGodModeChanged

Called from G_UpdateCvars when vr_godmode changes value.  Gives the player
immediate, visible feedback and, when switching on, restores health to the
maximum so the HUD and the authoritative health agree.  Switching off only
reports: normal damage resumes with the next hit.
==================
*/
void G_VrGodModeChanged( void ) {
	gentity_t *player = &g_entities[0];

	if ( g_gametype.integer != GT_SINGLE_PLAYER ) {
		if ( vr_godmode.integer ) {
			// refuse: this option is single player only
			trap_Cvar_Set( "vr_godmode", "0" );
		}
		return;
	}
	if ( !player->inuse || !player->client ) {
		return;
	}

	if ( vr_godmode.integer ) {
		int maxHealth = player->client->ps.stats[STAT_MAX_HEALTH];

		if ( player->health > 0 ) {
			int restored = VrGod_RestoreHealth( player->health, maxHealth );

			player->health = restored;
			player->client->ps.stats[STAT_HEALTH] = restored;
		}
		trap_SendServerCommand( 0, "cp \"God Mode: ON\n\"" );
	} else {
		trap_SendServerCommand( 0, "cp \"God Mode: OFF\n\"" );
	}
}
