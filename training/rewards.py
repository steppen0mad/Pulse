"""
Reward functions (training-only; never needed at deployment, so they live in
Python where they are fast to iterate on).

Each returns a scalar reward and a 'done' flag for one agent for one env step
(one frame-skip block). Distances are horizontal (x,z): navigation happens on the
floor plane, so vertical motion (the jump/up button) neither helps nor is
required, and the agent learns to ignore it.
"""

NAV_PROGRESS_W   = 1.0
NAV_TIME_PENALTY = 0.01
NAV_SUCCESS      = 10.0
NAV_REACH_RADIUS = 1.5


def reward_navigation(prev_dist, new_dist, reached):
    """Shaped distance-delta toward the target, a small time penalty, and a
    terminal success bonus."""
    r = NAV_PROGRESS_W * (prev_dist - new_dist) - NAV_TIME_PENALTY
    if reached:
        r += NAV_SUCCESS
    return r


PUR_CLOSE_W      = 1.0
PUR_TAG_BONUS    = 10.0
PUR_TIME_PENALTY = 0.01
EVA_SURVIVE      = 0.02
EVA_TAGGED       = 10.0
EVA_ESCAPE       = 5.0
TAG_RADIUS       = 1.5


def reward_pursuit(role, prev_range, new_range, tagged, has_los, timed_out):
    """Zero-sum-ish shaping for a pursuer/evader pair. role: 'pursuer'|'evader'.
    A tag only counts when line-of-sight is clear (discourages wall-tagging)."""
    if role == "pursuer":
        r = PUR_CLOSE_W * (prev_range - new_range) - PUR_TIME_PENALTY
        if tagged and has_los:
            r += PUR_TAG_BONUS
        return r
    else:
        r = EVA_SURVIVE
        if tagged and has_los:
            r -= EVA_TAGGED
        if timed_out:
            r += EVA_ESCAPE
        return r
