/*
===========================================================================
VrCrouchLogic.h -- pure crouch height rule shared by the game and cgame
player movement code (bg_pmove.c) and the host tests (tests/vr_input).

With "IRL crouch" on (vr_irl_crouch_enabled, default 1) PM_CheckDuck derives the
player's height from the headset height and used to return before it ever
looked at the crouch button (usercmd upmove < 0), so the A button did nothing
except recalibrate the standing height.  The button now forces the crouched
height, and releasing it hands control back to the headset height.

No engine includes; plain C only.
===========================================================================
*/

#ifndef VRCROUCHLOGIC_H
#define VRCROUCHLOGIC_H

/* Height (collision box top, game units) the player should have this frame.
 *
 *   buttonDuck   the crouch button is held (usercmd upmove < 0)
 *   curHeight    current headset height above the floor (metres, tracking space)
 *   maxHeight    calibrated standing headset height
 *   ratio        vr_irl_crouch_to_stand_ratio: headset height ratio that counts as fully crouched
 *   standHeight  maxs[2] standing
 *   crouchHeight crouchMaxZ
 */
static inline int VrCrouch_ComputeHeight( int buttonDuck, float curHeight, float maxHeight, float ratio,
										  int standHeight, int crouchHeight ) {
	int computed = standHeight;

	if ( crouchHeight < standHeight && curHeight < maxHeight ) {
		float minHeight = maxHeight * ratio;

		if ( curHeight < minHeight ) {          /* never crawl: clamp at the crouched height */
			computed = crouchHeight;
		} else if ( maxHeight > minHeight ) {
			float heightRatio = ( curHeight - minHeight ) / ( maxHeight - minHeight );

			computed = crouchHeight + (int)( heightRatio * ( standHeight - crouchHeight ) );
		}
	}
	/* the button wins over the headset: A crouches even when the player stands (or sits) */
	if ( buttonDuck && crouchHeight < standHeight ) {
		computed = crouchHeight;
	}
	return computed;
}

#endif /* VRCROUCHLOGIC_H */
