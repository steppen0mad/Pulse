"""
Reward functions (training-only; never needed at deployment, so they live in
Python where they are fast to iterate on).

Each returns a scalar reward and a 'done' flag for one agent for one env step
(one frame-skip block). Distances are horizontal (x,z): navigation happens on the
floor plane, so vertical motion (the jump/up button) neither helps nor is
required, and the agent learns to ignore it.
"""

# ---- navigation ----
NAV_PROGRESS_W   = 1.0     # reward per world-unit closed toward the target
NAV_TIME_PENALTY = 0.01    # per step, to encourage urgency
NAV_SUCCESS      = 10.0     # bonus for reaching the target
NAV_REACH_RADIUS = 1.5      # within this distance counts as reached


def reward_navigation(prev_dist, new_dist, reached):
    """Shaped distance-delta toward the target, a small time penalty, and a
    terminal success bonus."""
    r = NAV_PROGRESS_W * (prev_dist - new_dist) - NAV_TIME_PENALTY
    if reached:
        r += NAV_SUCCESS
    return r


# ---- pursuit / evasion (Phase 3 self-play) ----
PUR_CLOSE_W      = 1.0      # pursuer: reward per unit of range closed
PUR_TAG_BONUS    = 10.0     # pursuer: bonus for tagging (LOS-gated)
PUR_TIME_PENALTY = 0.01     # pursuer: urgency
EVA_SURVIVE      = 0.02     # evader: reward per step survived
EVA_TAGGED       = 10.0     # evader: penalty when tagged
EVA_ESCAPE       = 5.0      # evader: bonus for surviving to timeout
TAG_RADIUS       = 1.5


def reward_pursuit(role, prev_range, new_range, tagged, has_los, timed_out):
    """Zero-sum-ish shaping for a pursuer/evader pair. role: 'pursuer'|'evader'.
    A tag only counts when line-of-sight is clear (discourages wall-tagging)."""
    if role == "pursuer":
        r = PUR_CLOSE_W * (prev_range - new_range) - PUR_TIME_PENALTY
        if tagged and has_los:
            r += PUR_TAG_BONUS
        return r
    else:  # evader
        r = EVA_SURVIVE
        if tagged and has_los:
            r -= EVA_TAGGED
        if timed_out:
            r += EVA_ESCAPE
        return r
