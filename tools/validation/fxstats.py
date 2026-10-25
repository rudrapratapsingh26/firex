"""
Small statistics for the M4 validation reports: binomial confidence intervals,
percentiles, distribution summaries and Markdown tables. Standard library only,
so every tool in tools/validation runs, and is tested, anywhere Python runs.

A bench produces small trial counts. Every rate these tools report carries its
95 % Wilson interval, because "0 misses in 10 trials" bounds the miss rate only
below about 28 %, and a report that prints "0 %" without that is overclaiming.
"""
from __future__ import annotations

import math

Z95 = 1.959963984540054


def wilson(k: int, n: int, z: float = Z95) -> tuple[float, float]:
    """95 % Wilson score interval for k events in n trials. (0, 1) when n is 0."""
    if n <= 0:
        return 0.0, 1.0
    if k < 0 or k > n:
        raise ValueError(f"{k} events in {n} trials")
    p = k / n
    z2 = z * z
    denom = 1.0 + z2 / n
    centre = (p + z2 / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z2 / (4 * n * n)) / denom
    return max(0.0, centre - half), min(1.0, centre + half)


def percentile(values: list[float], p: float) -> float | None:
    """Linear interpolation between closest ranks (the numpy default). p in 0-100."""
    if not values:
        return None
    if not 0.0 <= p <= 100.0:
        raise ValueError(f"percentile {p}")
    ordered = sorted(values)
    rank = (len(ordered) - 1) * p / 100.0
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return float(ordered[low])
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def summarize(values: list[float]) -> dict:
    """n, min, median, p90, p99, max and mean; every statistic None when there are no values."""
    if not values:
        return {"n": 0, "min": None, "median": None, "p90": None, "p99": None, "max": None, "mean": None}
    return {
        "n": len(values),
        "min": float(min(values)),
        "median": percentile(values, 50),
        "p90": percentile(values, 90),
        "p99": percentile(values, 99),
        "max": float(max(values)),
        "mean": sum(values) / len(values),
    }


def rate(k: int, n: int) -> dict:
    """A rate as the reports store it: counts, the point estimate and its interval."""
    lo, hi = wilson(k, n)
    return {"k": k, "n": n, "rate": (k / n) if n else None, "ci95": [lo, hi]}


def poisson_cdf(k: int, mu: float) -> float:
    """P(X <= k) for X ~ Poisson(mu), summed in log space so large counts do not overflow."""
    if k < 0:
        return 0.0
    if mu <= 0.0:
        return 1.0
    log_mu = math.log(mu)
    return min(1.0, sum(math.exp(-mu + i * log_mu - math.lgamma(i + 1)) for i in range(k + 1)))


def _solve(f, target: float, lo: float, hi: float) -> float:
    """Bisection for a decreasing f on [lo, hi] with f(lo) >= target >= f(hi)."""
    for _ in range(200):
        mid = (lo + hi) / 2.0
        if f(mid) > target:
            lo = mid
        else:
            hi = mid
        if hi - lo <= 1e-9 * max(1.0, hi):
            break
    return (lo + hi) / 2.0


def poisson_ci(k: int, exposure: float = 1.0, confidence: float = 0.95) -> tuple[float, float]:
    """Exact (Garwood) interval for a rate: k events over `exposure` units of time.

    For pilot counts per unit-day. Zero events over one unit-day bounds the rate
    below 3.69; the Wilson interval does not apply, because events are counted
    over time, not out of trials."""
    if k < 0 or exposure <= 0:
        raise ValueError(f"{k} events over exposure {exposure}")
    tail = (1.0 - confidence) / 2.0
    hi_bound = max(10.0, 2.0 * (k + 1) + 10.0 * math.sqrt(k + 1))
    while poisson_cdf(k, hi_bound) > tail:
        hi_bound *= 2.0
    upper = _solve(lambda mu: poisson_cdf(k, mu), tail, 0.0, hi_bound)
    lower = 0.0 if k == 0 else _solve(lambda mu: poisson_cdf(k - 1, mu), 1.0 - tail, 0.0, hi_bound)
    return lower / exposure, upper / exposure


def rate_per(k: int, exposure: float, per: float = 1.0) -> dict:
    """A count over an exposure, scaled per `per` units (per 100 unit-days, say), with its exact interval."""
    if exposure <= 0:
        return {"k": k, "exposure": exposure, "per": per, "rate": None, "ci95": [None, None]}
    lo, hi = poisson_ci(k, exposure)
    return {"k": k, "exposure": exposure, "per": per, "rate": k / exposure * per, "ci95": [lo * per, hi * per]}


# --- formatting ---------------------------------------------------------------

def pct(x: float | None, digits: int = 0) -> str:
    return "-" if x is None else f"{100.0 * x:.{digits}f} %"


def rate_text(r: dict) -> str:
    """'0/10 (0 %, 95 % CI 0-28 %)'."""
    if not r["n"]:
        return "no trials"
    lo, hi = r["ci95"]
    return f"{r['k']}/{r['n']} ({pct(r['rate'])}, 95 % CI {100 * lo:.0f}-{100 * hi:.0f} %)"


def num(x: float | None, digits: int = 1, unit: str = "") -> str:
    if x is None:
        return "-"
    return f"{x:.{digits}f}{(' ' + unit) if unit else ''}"


def summary_text(s: dict, digits: int = 1, unit: str = "") -> str:
    """'median 12.0 s, p90 15.5 s, max 18.0 s (n 9)'."""
    if not s["n"]:
        return "no samples"
    return (f"median {num(s['median'], digits, unit)}, p90 {num(s['p90'], digits, unit)}, "
            f"max {num(s['max'], digits, unit)} (n {s['n']})")


def md_table(headers: list[str], rows: list[list[object]]) -> str:
    """A GitHub-flavoured Markdown table. Cells are converted with str(); pipes are escaped."""
    def cell(value: object) -> str:
        return str(value).replace("|", "\\|").replace("\n", " ")

    lines = ["| " + " | ".join(cell(h) for h in headers) + " |",
             "|" + "|".join("---" for _ in headers) + "|"]
    lines += ["| " + " | ".join(cell(v) for v in row) + " |" for row in rows]
    return "\n".join(lines)
