"""Independent fare check: recompute what a trip should cost from the tariff the meter downloaded.

It follows the rules of the meter (zephyr-app/core/fare_engine.c) but on its own, so a mistake in the
meter's math shows up as a "mismatch". The meter counts per wheel pulse and per second and rounds the
running cost DOWN to `fraction`, so the check uses a small tolerance instead of exact numbers.
"""


def _rates(t, bt):
    """single meter (bt 5) uses tariff 1, the 4 channels (bt 1..4) use tariff 2"""
    if bt == 5:
        return t.get("pri_cost", 0), t.get("Cost_200", 0), t.get("Cost_stop", 0), True
    return t.get("pri2_cost", 0), t.get("Cost2_200", 0), t.get("Cost2_stop", 0), False


def _floor_to(v, step):
    v = int(v)
    return v - (v % step) if step and step > 1 else v


def check(t, r):
    """t: tariff dict, r: trip with int fields. Returns dict(verdict, low, high, text, problems)."""
    pm = int(t.get("PM") or 0)
    if pm <= 0:
        return {"verdict": "unchecked", "low": None, "high": None, "text": "tariff has no PM", "problems": []}
    pri, c200, cstop, single = _rates(t, r["bt"])
    pct = float(t.get("Percent_night", 0)) / 100.0
    frac = int(t.get("fraction", 0) or 0)
    free_m = int(t.get("free_trip", 0) or 0)
    free_s = int(t.get("free_time", 0) or 0)

    dist = max(0, r["distance"] - free_m)
    wait = max(0, r["wait"] - free_s)

    # tariff 3 (single meter only): after `trip3` pulses another price per 200 m / minute
    trip3 = int(t.get("trip3", 0) or 0) if single else 0
    if trip3 > 0:
        d3_start = trip3 * 10 / pm  # meters where tariff 3 starts
        d1 = min(r["distance"], d3_start) - min(free_m, d3_start)
        d1 = max(0, d1)
        d3 = max(0, r["distance"] - max(d3_start, free_m))
        dist_lo = dist_hi = d1 * c200 / 200 + d3 * t.get("Cost3_200", 0) / 200
        s_lo = min(cstop, t.get("Cost3_stop", 0))
        s_hi = max(cstop, t.get("Cost3_stop", 0))
        wait_lo, wait_hi = wait * s_lo / 60, wait * s_hi / 60
    else:
        dist_lo = dist_hi = dist * c200 / 200
        wait_lo = wait_hi = wait * cstop / 60

    day_lo = pri + _floor_to(dist_lo + wait_lo, frac)
    day_hi = pri + _floor_to(dist_hi + wait_hi, frac)
    f = 1.0 + pct
    night_hi = int(pri * f) + _floor_to((dist_hi + wait_hi) * f, frac)

    total = r["total"]
    tol = max(frac, 10) + int(0.01 * max(total, day_hi)) + 2  # pulse / second rounding of the meter

    if r["night_price"] <= 0:
        low, high, mode = day_lo - tol, day_hi + tol, "day"
    elif r["night_price"] >= r["trip_price"] - tol:
        low, high, mode = int(day_lo * f) - tol, night_hi + tol, "night"
    else:
        low, high, mode = day_lo - tol, night_hi + tol, "day + night"

    problems = []
    if not low <= total <= high:
        problems.append(f"fare mismatch: total {total} but the tariff gives {max(low, 0)}..{high} ({mode})")
    # waiting part on its own
    w_lo = _floor_to(wait_lo, 1) - tol
    w_hi = int(wait_hi * (f if r["night_price"] > 0 else 1)) + tol
    if not w_lo <= r["traffic_price"] <= w_hi:
        problems.append(f"traffic_price {r['traffic_price']} does not match {r['wait']} s waiting ({max(w_lo, 0)}..{w_hi})")
    # average speed = distance / time
    if r["duration"] > 0:
        avg = r["distance"] * 36 // (r["duration"] * 10)
        if abs(avg - r["avg_speed"]) > 1:
            problems.append(f"avg_speed {r['avg_speed']} but distance/time gives {avg}")
    verdict = "ok" if not problems else "mismatch"
    text = f"{mode}: expected {max(low, 0)}..{high} Rial" + (" (tariff 3 in use)" if trip3 else "")
    return {"verdict": verdict, "low": max(low, 0), "high": high, "text": text, "problems": problems}
