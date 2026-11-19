#!/usr/bin/env python3
"""learn.py against the planner it fits, scored by an analyzer of its own
(not learn.py's). Needs numpy and the planner's sampler (`make -C vclick
fit-check` builds it); reads the user's recordings, which stay local.

    fit.py table PROFILE             the recording against the planner at PROFILE, in-sample
    fit.py heldout LEARN [LEARN...]  fit one half (or recording), score the other
    fit.py plants LEARN [LEARN...]   known profiles through the planner: given back?
    fit.py seeds LEARN [LEARN...]    the recording fitted under other simulation seeds

The first LEARN is the reference. Pass rules (a key a learner leaves out
is a miss):
- heldout: each row's mean error, |log(model / recording)| over both splits
  and 4 planner seeds, at most 0.05 above the reference's, and their sum no
  more than the reference's;
- plants: on every plant, stroke_a within 10 ms, stroke_b within 20 % (6 ms
  at least), bell 0.75, peak 0.05, tail_p and tail_share 0.10, tail_size
  0.02, lead_p 0.08, lead_ms 80;
- seeds: each of those keys' spread over 3 seeds within the same bound,
  and none left out under some seeds only.
"""
import importlib.machinery
import importlib.util
import json
import math
import os
import random
import subprocess
import sys
import types

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PLAN = os.path.join(HERE, "..", ".build-test", "plan")
SCRATCH = "/tmp/hypr-gate/vclick-fit"
STATE = os.path.join(os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state")), "vclick", "learn")
DT = 0.004
GAP = 0.040
LEG = 0.15
LEFT = 272
KERNEL = np.exp(-0.5 * (np.arange(-12, 13) * DT / 0.012) ** 2)
KERNEL /= KERNEL.sum()

# --- the analyzer: vclick's paths read as the recording reads a hand ---------------


def load(path):
    pos = np.loadtxt(path + "/pos.csv", delimiter=",")
    off = json.load(open(path + "/clock.json"))["realtime_minus_monotonic"]
    btn, rel = [], []
    for line in open(path + "/input.csv"):
        p = line.strip().split(",")
        t = float(p[0]) - off
        if p[2] == "btn":
            btn.append((t, int(p[3]), int(p[4])))
        elif p[2] == "rel":
            rel.append((t, 0 if p[3] == "x" else 1, int(p[4])))
    keep = np.r_[True, np.any(pos[1:, 1:] != pos[:-1, 1:], axis=1)]
    return pos[keep], sorted(btn), np.array(rel)


def resample(ts, xs, ys):
    T, X, Y = [ts[0]], [xs[0]], [ys[0]]
    for k in range(1, len(ts)):
        if ts[k] - ts[k - 1] > GAP:
            T.append(ts[k] - DT)
            X.append(xs[k - 1])
            Y.append(ys[k - 1])
        T.append(ts[k])
        X.append(xs[k])
        Y.append(ys[k])
    g = np.arange(T[0], T[-1] + DT / 2, DT)
    return g, np.interp(g, T, X), np.interp(g, T, Y)


def parse(ts, xs, ys, dip=0.5, rise_frac=0.04):
    g, x, y = resample(np.asarray(ts), np.asarray(xs), np.asarray(ys))
    pad = 40
    x0, y0, x1, y1 = x[0], y[0], x[-1], y[-1]
    D = math.hypot(x1 - x0, y1 - y0)
    if D < 24 or len(g) < 6:
        return None
    sx = np.convolve(np.r_[[x0] * pad, x, [x1] * pad], KERNEL, "same")[pad:-pad]
    sy = np.convolve(np.r_[[y0] * pad, y, [y1] * pad], KERNEL, "same")[pad:-pad]
    v = np.hypot(np.gradient(sx, DT), np.gradient(sy, DT))
    vmax = float(v.max())
    if vmax <= 0:
        return None
    floor = max(0.03 * vmax, 20.0)
    idx = np.nonzero(v >= floor)[0]
    if len(idx) < 4:
        return None
    on, off = int(idx[0]), int(idx[-1])
    ip = on + int(np.argmax(v[on : off + 1]))
    ux, uy = (x1 - x0) / D, (y1 - y0) / D
    along = (x - x0) * ux + (y - y0) * uy
    rise = max(rise_frac * vmax, 40.0)
    ends = []
    m = ip + 1
    while m < off:
        if v[m] < v[m - 1] and v[m] <= v[m + 1] and v[m] <= dip * vmax:
            if v[m : off + 1].max() - v[m] >= rise and math.hypot(x1 - x[m], y1 - y[m]) >= 3:
                ends.append(m)
                n = m + 1
                while n < off and v[n + 1] >= v[n]:
                    n += 1
                m = n
        m += 1
    pe = ends[0] if ends else off
    rem = math.hypot(x1 - x[pe], y1 - y[pe])
    err = float(along[pe] - D)
    kind = "direct"
    if ends:
        kind = "leg" if rem / D > LEG else ("under" if err < 0 else "over")
    px, py = x[pe] - x[on], y[pe] - y[on]
    pd = math.hypot(px, py)
    bow, s_curve = float("nan"), False
    if pd >= 16:
        side = (-(x[on : pe + 1] - x[on]) * py + (y[on : pe + 1] - y[on]) * px) / pd
        hi, lo = float(side.max()), float(side.min())
        big = hi if abs(hi) >= abs(lo) else lo
        bow = big / pd
        s_curve = abs(big) >= 0.01 * pd and min(abs(hi), abs(lo)) >= 0.25 * abs(big)
    return {"D": D, "mt": (off - on) * DT, "kind": kind, "bow": bow, "s": s_curve, "vmax": vmax,
            "start": (float(x0), float(y0)), "end": (float(x1), float(y1))}


def episodes(pos, btn, rel, rest, **kw):
    """the movement ending in each left press, after a rest"""
    t, x, y = pos[:, 0], pos[:, 1], pos[:, 2]
    every = np.array([b[0] for b in btn])
    rt = rel[:, 0] if len(rel) else np.array([])
    out = []
    for tc in (b[0] for b in btn if b[1] == LEFT and b[2] == 1):
        i = int(np.searchsorted(t, tc)) - 1
        if i < 3 or tc - t[i] > 1.2:
            continue
        prev = every[every < tc]
        floor = prev[-1] if len(prev) else -1.0
        j = i
        while j > 0 and t[j] - t[j - 1] < rest and t[j - 1] > floor and t[i] - t[j - 1] <= 2.5:
            j -= 1
        if j == 0 or (t[j] - t[j - 1] < rest and t[j - 1] > floor):
            continue
        ts = np.r_[t[j] - DT, t[j : i + 1]]
        xs = np.r_[x[j - 1], x[j : i + 1]]
        ys = np.r_[y[j - 1], y[j : i + 1]]
        steps = np.hypot(np.diff(xs), np.diff(ys))
        d = math.hypot(xs[-1] - xs[0], ys[-1] - ys[0])
        if d < 24:
            continue
        reports = int(np.searchsorted(rt, ts[-1]) - np.searchsorted(rt, ts[0]))
        if steps.max() > 250 or reports == 0 or steps.sum() / d > 2.0:
            continue  # a warp (the compositor moved the cursor), or a wander
        m = parse(ts, xs, ys, **kw)
        if m:
            out.append(m)
    return out


def tracks(eps, profile, seed, repeat):
    src = "".join(f"{e['start'][0]} {e['start'][1]} {e['end'][0]} {e['end'][1]}\n" for e in eps) * repeat
    run = subprocess.run([PLAN, profile, str(seed)], input=src, capture_output=True, text=True, check=True)
    by = {}
    for line in run.stdout.splitlines():
        i, tm, a, b = line.split()
        by.setdefault(int(i), []).append((float(tm) / 1000, float(a), float(b)))
    return [np.array(v) for v in by.values()]


def degrade(arr, gaps, rng):
    """as the recording sees a hand: the device's real report gaps, whole pixels, a 250 Hz poll"""
    end = arr[-1, 0]
    n = int(end / 0.002) + 8
    start = int(rng.integers(len(gaps)))
    g = np.take(gaps, np.arange(start, start + n), mode="wrap") / 1000
    t = np.r_[0.0, np.cumsum(g)]
    t = np.r_[t[t < end], end]
    x = np.rint(np.interp(t, arr[:, 0], arr[:, 1]))
    y = np.rint(np.interp(t, arr[:, 0], arr[:, 2]))
    t = np.ceil(t / DT - 1e-9) * DT
    keep = np.r_[True, (np.diff(x) != 0) | (np.diff(y) != 0)]
    t, x, y = t[keep], x[keep], y[keep]
    last = np.r_[t[1:] != t[:-1], True]
    return t[last], x[last], y[last]


def report_gaps(rel):
    ts = np.unique(rel[:, 0])
    g = np.diff(ts) * 1000
    return g[(g > 0.2) & (g < 40)]


# --- recordings, whole or split -----------------------------------------------------


def recordings():
    return [os.path.join(STATE, d) for d in sorted(os.listdir(STATE))]


def cut(part, lo, hi):
    pos, btn, rel = part
    return pos[(pos[:, 0] >= lo) & (pos[:, 0] < hi)], [b for b in btn if lo <= b[0] < hi], rel[(rel[:, 0] >= lo) & (rel[:, 0] < hi)]


def cut_learn(l, r, lo, hi):
    """a learn.Recording's span"""
    c = object.__new__(type(r))
    keep = [k for k, t in enumerate(r.t) if lo <= t < hi]
    c.t, c.x, c.y = [r.t[k] for k in keep], [r.x[k] for k in keep], [r.y[k] for k in keep]
    c.buttons = [b for b in r.buttons if lo <= b[0] < hi]
    c.wheel = [w for w in r.wheel if lo <= w < hi]
    c.reports = [t for t in r.reports if lo <= t < hi]
    return c


def splits(l):
    """(fit on, score on): one recording against another once there are two,
    else each half of the one against the other"""
    dirs = recordings()
    if len(dirs) >= 2:
        for k, d in enumerate(dirs):
            yield [l.Recording(o) for i, o in enumerate(dirs) if i != k], [load(d)]
        return
    r, part = l.Recording(dirs[0]), load(dirs[0])
    mid = (part[0][0, 0] + part[0][-1, 0]) / 2
    yield [cut_learn(l, r, -math.inf, mid)], [cut(part, mid, math.inf)]
    yield [cut_learn(l, r, mid, math.inf)], [cut(part, -math.inf, mid)]


def learner(path, name):
    loader = importlib.machinery.SourceFileLoader(name, path)
    m = importlib.util.module_from_spec(importlib.util.spec_from_loader(name, loader))
    loader.exec_module(m)
    return m


def write_profile(prof, name):
    os.makedirs(SCRATCH, exist_ok=True)
    path = os.path.join(SCRATCH, name)
    with open(path, "w") as f:
        f.write("".join(f"{k}={v}\n" for k, v in prof.items()))
    return path


# --- the rows ------------------------------------------------------------------------

ROWS = ("time 24-100", "time 100-300", "time 300+", "peak 100-300", "peak 300+",
        "bow 24-100", "bow 100-300", "bow 300+", "S-shaped %", "corrections %")


def rows(moves, strict):
    def band(lo, hi, legs=True):
        return [m for m in moves if lo <= m["D"] < hi and (legs or m["kind"] != "leg")]

    def med(v):
        return float(np.median(v)) if len(v) else float("nan")

    out = {}
    for lo, hi, name in ((24, 100, "24-100"), (100, 300, "100-300"), (300, 1e9, "300+")):
        out[f"time {name}"] = 1000 * med([m["mt"] for m in band(lo, hi, False)])
        out[f"bow {name}"] = med([abs(m["bow"]) for m in band(lo, hi) if not math.isnan(m["bow"])])
        if lo >= 100:
            out[f"peak {name}"] = med([m["vmax"] for m in band(lo, hi, False)])
    bent = [m["s"] for m in moves if m["D"] >= 100 and not math.isnan(m["bow"])]
    out["S-shaped %"] = 100 * float(np.mean(bent)) if bent else float("nan")
    out["corrections %"] = 100 * float(np.mean([m["kind"] in ("under", "over") for m in strict])) if strict else float("nan")
    return out


def error(row, model, rec):
    c = 1.0 if row.endswith("%") else 0.0
    return abs(math.log((model + c) / (rec + c))) if model > 0 and rec > 0 else float("nan")


def model_rows(eps, gaps, profile, seed):
    rng = np.random.default_rng(100 + seed)
    tr = tracks(eps, profile, seed, 6)
    model = [m for m in (parse(*degrade(t, gaps, rng)) for t in tr) if m]
    strict = [m for m in (parse(*degrade(t, gaps, rng), dip=0.25, rise_frac=0.15) for t in tr) if m]
    return rows(model, strict)


# --- modes ---------------------------------------------------------------------------


def table(profile):
    """the fidelity table of record: the whole recording, three rest thresholds"""
    part = load(recordings()[0])
    gaps = report_gaps(part[2])
    rests = (0.090, 0.150, 0.300)
    human = {r: episodes(*part, r) for r in rests}
    strict = {r: episodes(*part, r, dip=0.25, rise_frac=0.15) for r in rests}
    rng = np.random.default_rng(23)
    tr = tracks(human[0.150], profile, 5, 6)
    model = [m for m in (parse(*degrade(t, gaps, rng)) for t in tr) if m]
    model_strict = [m for m in (parse(*degrade(t, gaps, rng), dip=0.25, rise_frac=0.15) for t in tr) if m]

    def band(ms, lo, hi):
        return [m for m in ms if lo <= m["D"] < hi]

    def span(fn):
        v = [fn(human[r]) for r in rests]
        return min(v), max(v)

    def verdict(val, lo, hi, tol=0.0):
        return "ok " if lo * (1 - tol) <= val <= hi * (1 + tol) else "OFF"

    print(f"{'measure':34s} {'recording':>16s} {'model':>9s}")
    for lo, hi, tol in ((24, 100, 0.15), (100, 300, 0.15), (300, 700, 0), (700, 5000, 0)):
        f = lambda g: 1000 * float(np.median([m["mt"] for m in band(g, lo, hi) if m["kind"] != "leg"]))
        l, h = span(f)
        v = f(model)
        print(f"{f'time {lo}-{hi} px':34s} {l:7.0f}-{h:<8.0f} {v:9.0f}  {verdict(v, l, h, tol)}")
    for lo, hi, mode in ((100, 300, "ratio"), (300, 700, "ratio"), (700, 5000, "range")):
        f = lambda g: float(np.median([m["vmax"] for m in band(g, lo, hi) if m["kind"] != "leg"]))
        l, h = span(f)
        v = f(model)
        ok = verdict(v, l, h) if mode == "range" else ("ok " if 0.8 <= v / ((l + h) / 2) <= 1.25 else "OFF")
        print(f"{f'peak speed {lo}-{hi} px':34s} {l:7.0f}-{h:<8.0f} {v:9.0f}  {ok}")
    for lo, hi in ((24, 100), (100, 300), (300, 700), (700, 5000)):
        f = lambda g: float(np.median([abs(m["bow"]) for m in band(g, lo, hi) if not math.isnan(m["bow"])]))
        l, h = span(f)
        v = f(model)
        print(f"{f'median bow {lo}-{hi} px':34s} {l:7.3f}-{h:<8.3f} {v:9.3f}  {verdict(v, l, h, 0.25)}")
    f = lambda g: 100 * float(np.mean([m["s"] for m in g if m["D"] >= 100 and not math.isnan(m["bow"])]))
    v = f(model)
    print(f"{'S-shaped, 100 px and up (%)':34s} {min(f(human[r]) for r in rests):7.1f}-{max(f(human[r]) for r in rests):<8.1f} {v:9.1f}  {verdict(v, 9, 27)}")
    f = lambda g: 100 * float(np.mean([m["kind"] in ("under", "over") for m in g]))
    v = f(model_strict)
    print(f"{'small corrections, strict (%)':34s} {min(f(strict[r]) for r in rests):7.1f}-{max(f(strict[r]) for r in rests):<8.1f} {v:9.1f}  {verdict(v, 3, 8)}")


def heldout(paths):
    errs = {p: {r: [] for r in ROWS} for p in paths}
    for k, p in enumerate(paths):
        l = learner(p, f"learn{k}")
        for n, (fit_on, score_on) in enumerate(splits(l)):
            prof, _ = l.derive(fit_on)
            profile = write_profile(prof, f"heldout-{k}-{n}")
            for part in score_on:
                gaps = report_gaps(part[2])
                eps = episodes(*part, 0.150)
                rec = rows(eps, episodes(*part, 0.150, dip=0.25, rise_frac=0.15))
                for seed in range(1, 5):
                    got = model_rows(eps, gaps, profile, seed)
                    for r in ROWS:
                        errs[p][r].append(error(r, got[r], rec[r]))
    ref = paths[0]
    print(f"{'row':16s}" + "".join(f"{os.path.basename(p)[:14]:>16s}" for p in paths))
    worse = {p: [] for p in paths[1:]}
    for r in ROWS:
        means = [float(np.nanmean(errs[p][r])) for p in paths]
        print(f"{r:16s}" + "".join(f"{m:16.3f}" for m in means))
        for p, m in zip(paths[1:], means[1:]):
            if m > means[0] + 0.05:
                worse[p].append(r)
    sums = {p: sum(float(np.nanmean(errs[p][r])) for r in ROWS) for p in paths}
    print(f"{'sum':16s}" + "".join(f"{sums[p]:16.3f}" for p in paths))
    for p in paths[1:]:
        if sums[p] > sums[ref]:
            worse[p].append("the sum")
        print(f"{os.path.basename(p)}: " + ("PASS" if not worse[p] else "FAIL, worse than the reference: " + ", ".join(worse[p])))


BASE = {"stroke_a_ms": 40, "stroke_b_ms": 60, "bell": 4, "peak": .45, "correct_p": 0, "bow_median": .05, "bow_sigma": .6, "s_share": .2}
PLANTS = (
    ("a40 b60", {**BASE, "tail_p": .5, "tail_share": .4, "tail_size": .05}),
    ("a90 b30", {**BASE, "stroke_a_ms": 90, "stroke_b_ms": 30, "tail_p": .5, "tail_share": .4, "tail_size": .06}),
    ("a20 b100", {**BASE, "stroke_a_ms": 20, "stroke_b_ms": 100, "bell": 5, "peak": .4, "tail_p": .6, "tail_share": .3, "tail_size": .05}),
    ("a60 b40 drift", {**BASE, "stroke_a_ms": 60, "stroke_b_ms": 40, "tail_p": .5, "tail_share": .4, "tail_size": .05, "lead_p": .3, "lead_ms": 400, "lead_size": .15}),
)
SHOWN = ("stroke_a_ms", "stroke_b_ms", "bell", "peak", "tail_p", "tail_share", "tail_size", "lead_p", "lead_ms", "correct_p")
TOL = {"stroke_a_ms": 10, "bell": 0.75, "peak": 0.05, "tail_p": 0.10, "tail_share": 0.10, "tail_size": 0.02, "lead_p": 0.08, "lead_ms": 80}


def tol(k, want):
    return max(0.2 * want, 6) if k == "stroke_b_ms" else TOL.get(k, math.inf)


def within(k, got, want):
    return abs(got - want) <= tol(k, want)


def motion(l, moves, gaps):
    have = lambda _k, enough: enough
    if l.motion.__code__.co_argcount >= 3:
        return l.motion(moves, have, [float(g) / 1000 for g in gaps])
    return l.motion(moves, have)


def plants(paths):
    part = load(recordings()[0])
    gaps = report_gaps(part[2])
    eps = episodes(*part, 0.150)
    # and one like the user's: the reference's own fit of the recordings, never written down
    ref = learner(paths[0], "reference")
    near = ref.derive([ref.Recording(d) for d in recordings()])[0]
    for k, p in enumerate(paths):
        l = learner(p, f"learn{k}")
        bad = 0
        print(f"== {os.path.basename(p)}")
        for name, truth in (*PLANTS, ("near-real", near)):
            profile = write_profile(truth, "plant")
            rng = np.random.default_rng(3)
            ms = [m for m in (l.measure(*[v.tolist() for v in degrade(t, gaps, rng)]) for t in tracks(eps, profile, 5, 4)) if m]
            got = motion(l, [m for m in ms if not m["leg"]], gaps)
            cells = []
            for key in SHOWN:
                g, w = got.get(key), truth.get(key)
                ok = w is None or (g is not None and within(key, g, w))  # a key left out is a miss
                bad += not ok
                cells.append(f"{key.replace('_ms', '')} {'-' if w is None else f'{w:g}'}→{'-' if g is None else f'{g:g}'}{'' if ok else '!'}")
            print(f"  {name:14s} " + "  ".join(cells))
        print(f"  {'PASS' if not bad else f'FAIL: {bad} out of bounds (!)'}")


def seeds(paths):
    for k, p in enumerate(paths):
        l = learner(p, f"learn{k}")
        fits = []
        for s in (7, 11, 29):
            # the learner's simulation seed, whatever it asks for
            l.random = types.SimpleNamespace(Random=lambda _seed, s=s: random.Random(s))
            fits.append(l.derive([l.Recording(d) for d in recordings()])[0])
        bad = []
        print(f"== {os.path.basename(p)}")
        for key in SHOWN:
            v = [f.get(key) for f in fits]
            if all(x is None for x in v):
                continue
            if any(x is None for x in v):  # left out under some seeds
                bad.append(key)
                print(f"  {key:14s} " + "  ".join("-" if x is None else f"{x:g}" for x in v) + "  !")
                continue
            spread_ok = max(v) - min(v) <= tol(key, float(np.median(v)))
            bad += [] if spread_ok else [key]
            print(f"  {key:14s} " + "  ".join(f"{x:g}" for x in v) + ("" if spread_ok else "  !"))
        print(f"  {'PASS' if not bad else 'FAIL: ' + ', '.join(bad)}")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("table", "heldout", "plants", "seeds"):
        sys.exit(__doc__)
    if not os.path.exists(PLAN):
        sys.exit("fit.py: no planner sampler; make -C vclick fit-check")
    mode, args = sys.argv[1], [os.path.abspath(a) for a in sys.argv[2:]]
    {"table": lambda: table(args[0]), "heldout": lambda: heldout(args), "plants": lambda: plants(args), "seeds": lambda: seeds(args)}[mode]()


if __name__ == "__main__":
    main()
