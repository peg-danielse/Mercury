"""Shared constants for the hotel/social/media evaluation dataset.

build.py (numpy only) and train.py (torch) both use these, so the label and the
reported core-seconds stay on the same clock.
"""

# Critical routes, the same sets compare_systems.sh scores.
CRITICAL = {
    "hotel": ("hotels", "reservation"),
    "social": ("compose-post", "read-home-timeline"),
    "media": ("read-page", "compose-review"),
}

# Keep a policy in a scenario when its critical-route p95 is within this factor of the best.
MARGIN = 1.2
# A scenario needs this many completed critical requests, and this many policies, before it is a label.
MIN_REQUESTS = 30
MIN_POLICIES = 2
# train_seq.py's under-allocation offset: the target the loss aims at is C* - OFF.
OFF = 0.3
# Deadband on |P(t) - P(t-1)|, in cores. Moves inside it are free.
DEADBAND = 0.25
# Largest request the metrics will count. The evaluation budgets stop at 40.
REQ_MAX = 40
# Native ring period is 10 ms. Keeping every 5th sample is a 50 ms tick.
STRIDE = 5

APPS = ("hotel", "social", "media")
ARMS = (("", "main"), ("-p40", "p40"), ("-gnn", "gnn"))


def metrics(pairs, dt):
    """pairs: (request per tick, label C*, valid mask). request is already an integer core count."""
    under = over = n = 0
    cs_req = cs_lab = 0.0
    changes = 0
    secs = 0.0
    for req, label, valid in pairs:
        lab = label.round()
        step = valid[1:] & valid[:-1]
        changes += int(((req[1:] != req[:-1]) & step).sum())
        secs += dt * float(step.sum())
        keep = valid
        under += int((req[keep] < lab[keep]).sum())
        over += int((req[keep] > lab[keep]).sum())
        n += int(keep.sum())
        cs_req += float(req[keep].sum()) * dt
        cs_lab += float(lab[keep].sum()) * dt
    return {
        "ticks": n,
        "under_%": 100.0 * under / max(n, 1),
        "over_%": 100.0 * over / max(n, 1),
        "core_s_pred": cs_req,
        "core_s_label": cs_lab,
        "req_changes_per_s": changes / max(secs, 1e-9),
    }
