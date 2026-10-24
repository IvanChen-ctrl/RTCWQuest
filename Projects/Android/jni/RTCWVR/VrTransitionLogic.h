/*
===========================================================================
VrTransitionLogic.h -- tracks actions and keys the VR layer is holding down
(M2B), so they can be released when the input mode changes.

Pure logic, no engine includes; compiled by the engine and by the host tests
in tests/vr_m2b.

Problem: gameplay actions ("+attack", "+kick", ...) and keys (K_SPACE) are
released by a later button edge.  If the mode switches in between (gameplay
<-> menu/screen layer) that edge is handled by a different branch, the release
is never sent and the action stays stuck.  Only actions this layer actually
started are released; nothing else is cleared, so movement, held weapons and
the engine's own key state are not touched.
===========================================================================
*/

#ifndef VRTRANSITIONLOGIC_H
#define VRTRANSITIONLOGIC_H

#include <string.h>

#define VRT_MAX_ACTIONS     16
#define VRT_ACTION_NAME_MAX 24
#define VRT_MAX_KEYS        8

typedef struct {
    char actions[VRT_MAX_ACTIONS][VRT_ACTION_NAME_MAX];  /* name without the leading '+' */
    int  actionCount;
    int  keys[VRT_MAX_KEYS];
    int  keyCount;
} vrHeld_t;

static inline void VrHeld_Init(vrHeld_t *h)
{
    memset(h, 0, sizeof(*h));
}

/* Records a "+name" press (down != 0) or release (down == 0).  Commands that
 * do not start with '+' are not holdable and are ignored. */
static inline void VrHeld_Action(vrHeld_t *h, const char *action, int down)
{
    int i;

    if (!action || action[0] != '+' || action[1] == '\0') {
        return;
    }
    action++;
    if (strlen(action) >= VRT_ACTION_NAME_MAX) {
        return;
    }
    for (i = 0; i < h->actionCount; i++) {
        if (strcmp(h->actions[i], action) == 0) {
            if (!down) {
                memmove(h->actions[i], h->actions[i + 1],
                        (size_t)(h->actionCount - i - 1) * VRT_ACTION_NAME_MAX);
                h->actionCount--;
            }
            return;
        }
    }
    if (down && h->actionCount < VRT_MAX_ACTIONS) {
        strcpy(h->actions[h->actionCount++], action);
    }
}

static inline void VrHeld_Key(vrHeld_t *h, int key, int down)
{
    int i;

    for (i = 0; i < h->keyCount; i++) {
        if (h->keys[i] == key) {
            if (!down) {
                memmove(&h->keys[i], &h->keys[i + 1], (size_t)(h->keyCount - i - 1) * sizeof(int));
                h->keyCount--;
            }
            return;
        }
    }
    if (down && h->keyCount < VRT_MAX_KEYS) {
        h->keys[h->keyCount++] = key;
    }
}

/* Pops one held action into out (size >= VRT_ACTION_NAME_MAX).  0 = none left. */
static inline int VrHeld_TakeAction(vrHeld_t *h, char *out)
{
    if (h->actionCount == 0) {
        return 0;
    }
    h->actionCount--;
    strcpy(out, h->actions[h->actionCount]);
    return 1;
}

static inline int VrHeld_TakeKey(vrHeld_t *h, int *key)
{
    if (h->keyCount == 0) {
        return 0;
    }
    h->keyCount--;
    *key = h->keys[h->keyCount];
    return 1;
}

/* Tracks the screen-layer mode between input frames.  Returns 1 on the frame
 * where the mode differs from the previous frame (first call only records). */
typedef struct {
    int known;
    int screen;
} vrModeTracker_t;

static inline int VrMode_Changed(vrModeTracker_t *m, int screen)
{
    int changed = m->known && m->screen != (screen != 0);

    m->known = 1;
    m->screen = (screen != 0);
    return changed;
}

#endif /* VRTRANSITIONLOGIC_H */
