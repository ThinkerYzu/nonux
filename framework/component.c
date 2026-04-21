#include "framework/component.h"

/*
 * Lifecycle state machine — see framework/component.h for the edge diagram.
 *
 * Rationale for keeping the transition table open-coded in C rather than a
 * 2D lookup array: the matrix is small (six states, nine legal edges) and
 * reads more obviously as a switch than as sparse integer arrays.  When
 * PAUSING lands with the pause protocol it will grow to two more edges and
 * the switch will still fit on one screen.
 */

bool nx_lifecycle_transition_legal(enum nx_lifecycle_state from,
                                   enum nx_lifecycle_state to)
{
    switch (from) {
    case NX_LC_UNINIT:
        return to == NX_LC_INIT || to == NX_LC_READY;
    case NX_LC_INIT:
        return to == NX_LC_READY;
    case NX_LC_READY:
        return to == NX_LC_ACTIVE || to == NX_LC_DESTROYED;
    case NX_LC_ACTIVE:
        return to == NX_LC_PAUSED || to == NX_LC_READY;
    case NX_LC_PAUSED:
        return to == NX_LC_ACTIVE || to == NX_LC_READY;
    case NX_LC_DESTROYED:
        return false;
    }
    return false;
}

const char *nx_lifecycle_state_name(enum nx_lifecycle_state s)
{
    switch (s) {
    case NX_LC_UNINIT:    return "uninit";
    case NX_LC_INIT:      return "init";
    case NX_LC_READY:     return "ready";
    case NX_LC_ACTIVE:    return "active";
    case NX_LC_PAUSED:    return "paused";
    case NX_LC_DESTROYED: return "destroyed";
    }
    return "unknown";
}

/* Single-step transition helper.  `expected_from` is the only legal current
 * state; any other value (including an unknown component caught by
 * state_set's NX_ENOENT path) returns NX_ESTATE / NX_ENOENT / NX_EINVAL
 * as appropriate.  We consult the matrix as a belt-and-suspenders check so
 * a typo in a caller's expected_from can't silently bypass it. */
static int transition_from(struct nx_component *c,
                           enum nx_lifecycle_state expected_from,
                           enum nx_lifecycle_state to)
{
    if (!c) return NX_EINVAL;
    if (c->state != expected_from) return NX_ESTATE;
    if (!nx_lifecycle_transition_legal(expected_from, to)) return NX_ESTATE;
    return nx_component_state_set(c, to);
}

int nx_component_init(struct nx_component *c)
{
    /* v1 synchronous init: UNINIT → READY in a single step.  The INIT
     * state is legal-but-unused until async bring-up is added. */
    return transition_from(c, NX_LC_UNINIT, NX_LC_READY);
}

int nx_component_enable(struct nx_component *c)
{
    return transition_from(c, NX_LC_READY, NX_LC_ACTIVE);
}

int nx_component_pause(struct nx_component *c)
{
    return transition_from(c, NX_LC_ACTIVE, NX_LC_PAUSED);
}

int nx_component_resume(struct nx_component *c)
{
    return transition_from(c, NX_LC_PAUSED, NX_LC_ACTIVE);
}

int nx_component_disable(struct nx_component *c)
{
    if (!c) return NX_EINVAL;
    if (c->state != NX_LC_ACTIVE && c->state != NX_LC_PAUSED)
        return NX_ESTATE;
    return nx_component_state_set(c, NX_LC_READY);
}

int nx_component_destroy(struct nx_component *c)
{
    return transition_from(c, NX_LC_READY, NX_LC_DESTROYED);
}
