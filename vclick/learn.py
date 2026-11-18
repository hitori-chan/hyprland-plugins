#!/usr/bin/env python3
"""vclick learn: record the user's own pointer movements, then fit the glide
profile vclick moves with.

    learn.py record [--minutes 30] [--out DIR]   record until Ctrl-C or the time is up
    learn.py fit [DIR...] [--profile FILE]       fit the profile from recordings
    learn.py show [--profile FILE]               print the profile

record reads two things, and changes nothing:

- the cursor's position, polled from the compositor (`cursorpos` on
  Hyprland's socket) at about 250 Hz: where the pointer is on screen,
  after acceleration;
- the pointer devices' raw events (/dev/input/event*, needs the `input`
  group), only motion, wheels and mouse buttons, never a keyboard: the exact
  time of each report, each wheel step and each click.

It never grabs a device: every event still reaches the compositor.

fit takes one movement per click: from the last rest or button event to
the press. Moves with a separate leg (the first stroke stops far short) are
left out of the time: a recording cannot tell a second leg from a pause to
look for the target. It fits:

- Fitts's law, MT = a + b * log2(D / W + 1), with W the default target
  width vclick assumes (32 px), through the medians of 8 bands of D;
- the bell of the first stroke's speed: how sharply it peaks, and where;
- the path's bow: the largest sideways distance over the length;
- how often a small correction follows the first stroke, which way, how far;
- the rest: from arriving within 3 px of the press point to the press;
- the button: how long a click holds it, the gap inside a double click, and a
  drag's lead (press until 3 px away) and rest (3 px from the drop until the
  release);
- the wheel's pace: the time between the steps of a spin;
- the device's report rate.

A value with too few samples is left out of the profile, so vclick's default
stands for it; fit says which.

The profile is a key=value file vclick reads on load and on
`hyprctl vclick profile reload`.
"""
import argparse
import bisect
import json
import math
import os
import select
import socket
import statistics
import struct
import sys
import threading
import time

STATE = os.path.join(os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state")), "vclick")
PROFILE = os.path.join(STATE, "profile")
W_DEFAULT = 32.0
BTN_LEFT = 0x110

EV_SYN, EV_KEY, EV_REL = 0x00, 0x01, 0x02
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL = 0x00, 0x01, 0x06, 0x08
BTN_FIRST, BTN_LAST = 0x110, 0x117  # the BTN_MOUSE block: never a keyboard key
EVENT = struct.Struct("llHHi")       # struct input_event on 64-bit Linux


# --- record ---------------------------------------------------------------------

def pointer_devices():
    """(path, name) of every device that reports relative motion (mice,
    trackpoints) or calls itself a touchpad. Keyboards are left out, even a
    gaming mouse's keyboard half."""
    out, name, handlers, ev = [], "", [], 0
    with open("/proc/bus/input/devices") as f:
        for line in f.read().split("\n") + [""]:
            if line.startswith("N: Name="):
                name = line[8:].strip().strip('"')
            elif line.startswith("H: Handlers="):
                handlers = line[12:].split()
            elif line.startswith("B: EV="):
                ev = int(line[6:].strip(), 16)
            elif not line.strip():
                is_kbd = "kbd" in handlers and not (ev & (1 << EV_REL))
                pointer = (ev & (1 << EV_REL)) or "touchpad" in name.lower()
                if pointer and not is_kbd and "keyboard" not in name.lower():
                    for h in handlers:
                        if h.startswith("event"):
                            out.append(("/dev/input/" + h, name))
                name, handlers, ev = "", [], 0
    return out


def hypr_socket():
    sig = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    run = os.environ.get("XDG_RUNTIME_DIR")
    if not sig or not run:
        sys.exit("learn: not inside a Hyprland session")
    return os.path.join(run, "hypr", sig, ".socket.sock")


def cursorpos(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
        s.sendall(b"cursorpos")
        data = s.recv(64).decode()
    finally:
        s.close()
    x, y = data.split(",")
    return float(x), float(y)


def record(a):
    out = a.out or os.path.join(STATE, "learn", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    sock = hypr_socket()
    devs = []
    for path, name in pointer_devices():
        try:
            devs.append((os.open(path, os.O_RDONLY | os.O_NONBLOCK), path, name))
        except OSError as e:
            print(f"learn: skipping {path} ({name}): {e.strerror}", file=sys.stderr)
    with open(os.path.join(out, "devices.json"), "w") as f:
        json.dump([{"path": p, "name": n} for _, p, n in devs], f)

    # evdev stamps wall-clock time, positions the monotonic clock: their
    # offset now, so a fit of a recording still running can join them
    with open(os.path.join(out, "clock.json"), "w") as f:
        json.dump({"realtime_minus_monotonic": time.time() - time.monotonic()}, f)

    stop = threading.Event()
    end = time.monotonic() + a.minutes * 60

    def poll():
        # positions at ~250 Hz; a still pointer is written once a second
        last, last_t = None, 0.0
        with open(os.path.join(out, "pos.csv"), "w", buffering=1) as f:
            while not stop.is_set() and time.monotonic() < end:
                t = time.monotonic()
                try:
                    p = cursorpos(sock)
                except (OSError, ValueError):
                    time.sleep(0.5)
                    continue
                if p != last or t - last_t >= 1.0:
                    f.write(f"{t:.4f},{p[0]:.1f},{p[1]:.1f}\n")
                    last, last_t = p, t
                time.sleep(max(0.0, 0.004 - (time.monotonic() - t)))

    th = threading.Thread(target=poll, daemon=True)
    th.start()
    print(f"learn: recording into {out} for up to {a.minutes} min; Ctrl-C ends it", file=sys.stderr)
    by_fd = {fd: (path, name) for fd, path, name in devs}
    try:
        with open(os.path.join(out, "input.csv"), "w", buffering=1) as f:
            while time.monotonic() < end:
                ready, _, _ = select.select(list(by_fd), [], [], 0.5)
                for fd in ready:
                    try:
                        data = os.read(fd, EVENT.size * 64)
                    except BlockingIOError:
                        continue
                    except OSError:  # the device went away
                        del by_fd[fd]
                        continue
                    dev = os.path.basename(by_fd[fd][0])
                    for i in range(0, len(data) - EVENT.size + 1, EVENT.size):
                        sec, usec, typ, code, value = EVENT.unpack_from(data, i)
                        t = sec + usec / 1e6  # evdev stamps CLOCK_REALTIME
                        if typ == EV_REL and code in (REL_X, REL_Y):
                            f.write(f"{t:.6f},{dev},rel,{'x' if code == REL_X else 'y'},{value}\n")
                        elif typ == EV_REL and code in (REL_WHEEL, REL_HWHEEL):
                            f.write(f"{t:.6f},{dev},wheel,{code},{value}\n")
                        elif typ == EV_KEY and BTN_FIRST <= code <= BTN_LAST:
                            f.write(f"{t:.6f},{dev},btn,{code},{value}\n")
                if not by_fd:
                    time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    stop.set()
    th.join(timeout=2)
    print(f"learn: done: {out}", file=sys.stderr)


# --- fit --------------------------------------------------------------------------

DT = 0.004      # movements are read on a 4 ms grid
REST = 0.150    # a pointer still this long has ended a movement
GAP = 0.040     # only changes are logged: a longer gap is a stillness, past any device's report gap
LEG = 0.15      # a first stroke ending further than this share of the way off: a separate leg
NEAR = 3.0      # px: arrived at the point
TRAVEL = 24.0   # px: the shortest movement that says anything
ENOUGH = 30     # samples a value needs
SEEN = 0.03     # of the peak speed: where a recorded movement starts and ends

KERNEL = [math.exp(-0.5 * (i / 3) ** 2) for i in range(-12, 13)]  # sigma 12 ms: about 11 Hz
KERNEL = [k / sum(KERNEL) for k in KERNEL]


class Recording:
    """One recording's positions (only changes), buttons and device reports,
    all on the monotonic clock."""

    def __init__(self, dirpath):
        with open(os.path.join(dirpath, "clock.json")) as f:
            offset = json.load(f)["realtime_minus_monotonic"]
        self.t, self.x, self.y = [], [], []
        with open(os.path.join(dirpath, "pos.csv")) as f:
            for line in f:
                t, x, y = map(float, line.strip().split(","))
                if not self.t or (x, y) != (self.x[-1], self.y[-1]):
                    self.t.append(t)
                    self.x.append(x)
                    self.y.append(y)
        self.buttons, self.wheel, reports = [], [], set()
        with open(os.path.join(dirpath, "input.csv")) as f:
            for line in f:
                p = line.strip().split(",")
                t = float(p[0]) - offset
                if p[2] == "btn":
                    self.buttons.append((t, int(p[3]), p[4] == "1"))
                elif p[2] == "rel":
                    reports.add(t)  # one report carries x and y at one time
                elif p[2] == "wheel":
                    self.wheel.append(t)
        self.buttons.sort()
        self.wheel.sort()
        self.reports = sorted(reports)


def aimed(r):
    """The movement ending in each left press, as (times, xs, ys)."""
    every = [b[0] for b in r.buttons]
    for tc, code, down in r.buttons:
        if code != BTN_LEFT or not down:
            continue
        i = bisect.bisect_left(r.t, tc) - 1
        if i < 3 or tc - r.t[i] > 1.2:
            continue
        k = bisect.bisect_left(every, tc)
        floor = every[k - 1] if k else -math.inf
        j = i
        while j > 0 and r.t[j] - r.t[j - 1] < REST and r.t[j - 1] > floor and r.t[i] - r.t[j - 1] <= 2.5:
            j -= 1
        if j == 0 or (r.t[j] - r.t[j - 1] < REST and r.t[j - 1] > floor):
            continue  # never at rest: it wandered
        ts = [r.t[j] - DT] + r.t[j:i + 1]
        xs, ys = r.x[j - 1:i + 1], r.y[j - 1:i + 1]
        steps = [math.dist((xs[n], ys[n]), (xs[n + 1], ys[n + 1])) for n in range(len(xs) - 1)]
        d = math.dist((xs[0], ys[0]), (xs[-1], ys[-1]))
        if d < TRAVEL or sum(steps) / d > 2.0:
            continue
        # the compositor moved the cursor: a step no device report made
        if max(steps) > 250 or bisect.bisect(r.reports, ts[-1]) == bisect.bisect(r.reports, ts[0]):
            continue
        yield ts, xs, ys


def grid(ts, xs, ys):
    """The positions every 4 ms; a pointer stands still over a gap."""
    T, X, Y = [ts[0]], [xs[0]], [ys[0]]
    for k in range(1, len(ts)):
        if ts[k] - ts[k - 1] > GAP:
            T.append(ts[k] - DT)
            X.append(xs[k - 1])
            Y.append(ys[k - 1])
        T.append(ts[k])
        X.append(xs[k])
        Y.append(ys[k])
    out_x, out_y, k = [], [], 0
    for n in range(int((T[-1] - T[0]) / DT) + 1):
        t = T[0] + n * DT
        while k + 2 < len(T) and T[k + 1] < t:
            k += 1
        u = min(1.0, max(0.0, (t - T[k]) / (T[k + 1] - T[k]))) if T[k + 1] > T[k] else 1.0
        out_x.append(X[k] + (X[k + 1] - X[k]) * u)
        out_y.append(Y[k] + (Y[k + 1] - Y[k]) * u)
    return out_x, out_y


def smooth(v):
    pad = [v[0]] * 12 + v + [v[-1]] * 12
    return [sum(k * pad[i + j] for j, k in enumerate(KERNEL)) for i in range(len(v))]


def first_stroke(v, x, y, ip, off, vmax, dip, rise):
    """Where the first stroke ends: the first dip after the peak that a later
    rise follows, with the target still some way off; the end if none does."""
    rise = max(rise * vmax, 40.0)
    tail = [0.0] * (off + 2)  # the fastest speed from each sample on
    for m in range(off, ip, -1):
        tail[m] = max(v[m], tail[m + 1])
    for m in range(ip + 1, off):
        if v[m] < v[m - 1] and v[m] <= v[m + 1] and v[m] <= dip * vmax and tail[m] - v[m] >= rise and math.dist((x[-1], y[-1]), (x[m], y[m])) >= NEAR:
            return m
    return off


def measure(ts, xs, ys):
    x, y = grid(ts, xs, ys)
    if len(x) < 6:
        return None
    sx, sy = smooth(x), smooth(y)
    n = len(x)
    v = [math.dist((sx[min(i + 1, n - 1)], sy[min(i + 1, n - 1)]), (sx[max(i - 1, 0)], sy[max(i - 1, 0)])) / ((min(i + 1, n - 1) - max(i - 1, 0)) * DT) for i in range(n)]
    vmax = max(v)
    floor = max(SEEN * vmax, 20.0)
    seen = [i for i in range(n) if v[i] >= floor]
    if len(seen) < 4:
        return None
    on, off = seen[0], seen[-1]
    ip = max(range(on, off + 1), key=v.__getitem__)
    d = math.dist((x[0], y[0]), (x[-1], y[-1]))
    ux, uy = (x[-1] - x[0]) / d, (y[-1] - y[0]) / d

    m = {"d": d, "mt": (off - on) * DT}
    pe = first_stroke(v, x, y, ip, off, vmax, 0.5, 0.04)
    m["leg"] = pe != off and math.dist((x[-1], y[-1]), (x[pe], y[pe])) / d > LEG
    # the strict reading finds fewer corrections: better too few than invented
    strict = first_stroke(v, x, y, ip, off, vmax, 0.25, 0.15)
    left = math.dist((x[-1], y[-1]), (x[strict], y[strict])) / d
    m["corrected"] = strict != off and left <= LEG
    m["short"] = (x[strict] - x[0]) * ux + (y[strict] - y[0]) * uy < d
    m["left"] = left

    # the stroke itself, when nothing follows it
    if pe == off and off > on:
        chord = math.dist((x[on], y[on]), (x[off], y[off]))
        m["sharp"] = vmax / max(chord / ((off - on) * DT), 1e-6)
        m["peak"] = (ip - on) / (off - on)
    px, py = x[pe] - x[on], y[pe] - y[on]
    chord = math.hypot(px, py)
    if chord >= 16:
        m["bow"] = max(abs(-(x[i] - x[on]) * py + (y[i] - y[on]) * px) / chord for i in range(on, pe + 1)) / chord
    return m


def arrivals(r):
    """From arriving within 3 px of each travelled-to press point to the press, in ms."""
    every = [b[0] for b in r.buttons]
    out = []
    for tc, code, down in r.buttons:
        if code != BTN_LEFT or not down:
            continue
        i = bisect.bisect_left(r.t, tc) - 1
        if i < 3:
            continue
        k = bisect.bisect_left(every, tc)
        floor = every[k - 1] if k else -math.inf
        j = i
        while j >= 0 and r.t[j] > floor and r.t[i] - r.t[j] <= 2.5 and math.dist((r.x[j], r.y[j]), (r.x[i], r.y[i])) <= NEAR:
            j -= 1
        if j < 0 or r.t[j] <= floor or r.t[i] - r.t[j] > 2.5:
            continue  # it never travelled here since the last button
        if max(math.dist((r.x[n], r.y[n]), (r.x[i], r.y[i])) for n in range(max(0, j - 700), j + 1)) >= TRAVEL:
            out.append((tc - r.t[j + 1]) * 1000)
    return out


def button_times(r):
    """The left button's holds, the gaps inside double clicks, and each drag's
    lead and rest before the release, in ms."""
    holds, gaps, leads, drops = [], [], [], []
    at = lambda t: (r.x[max(0, bisect.bisect_right(r.t, t) - 1)], r.y[max(0, bisect.bisect_right(r.t, t) - 1)])
    down, last = None, None
    for t, code, pressed in r.buttons:
        if code != BTN_LEFT:
            continue
        if pressed:
            down = t
            continue
        if down is None:
            continue
        a, b = at(down), at(t)
        d = math.dist(a, b)
        if d < 4:
            holds.append((t - down) * 1000)
            if last and down - last[0] < 0.5 and math.dist(last[2], a) < 5:
                gaps.append((down - last[1]) * 1000)
            last = (down, t, a)
        elif d >= TRAVEL:
            span = range(bisect.bisect_right(r.t, down), bisect.bisect_left(r.t, t))
            away = [k for k in span if math.dist((r.x[k], r.y[k]), a) > NEAR]
            short = [k for k in span if math.dist((r.x[k], r.y[k]), b) > NEAR]
            if away:
                leads.append((r.t[away[0]] - down) * 1000)
            if short:
                drops.append((t - r.t[min(short[-1] + 1, span[-1])]) * 1000)
        down = None
    return holds, gaps, leads, drops


def percentile(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def log_spread(v):
    """The sigma of a log-normal, from the middle of the samples: tails shrugged off."""
    v = [max(x, 1e-3) for x in v]
    return (math.log(percentile(v, 0.84)) - math.log(percentile(v, 0.16))) / 2


def bell(sharp, peak):
    """The speed t^A (1-t)^B whose recorded shape is this: a recording starts
    and ends where the speed clears its noise, so the tails are out of its
    sight. Answers A + B, where it peaks, and the share of its time seen."""
    N = 200
    us = [(i + 0.5) / N for i in range(N)]
    best = (math.inf, 4.0, 0.45, 1.0)
    for s2 in range(4, 49):
        s = s2 / 2
        for p100 in range(20, 71):
            p = p100 / 100
            f = [u ** (s * p) * (1 - u) ** (s * (1 - p)) for u in us]
            top = max(f)
            seen = [i for i in range(N) if f[i] >= SEEN * top]
            on, off = seen[0], seen[-1]
            width = (off - on + 1) / N
            mean = sum(f[on:off + 1]) / (off - on + 1)
            at = (f.index(top) - on) / max(1, off - on)
            miss = (top / mean / sharp - 1) ** 2 + (at - peak) ** 2
            if miss < best[0]:
                best = (miss, s, p, width)
    return best[1:]


def fitts(moves, width):
    """MT = a + b * ID through the medians of 8 equal-count ID bands: medians
    shrug off hesitations. In the time of the whole bell, its unseen tails too."""
    pts = sorted((math.log2(m["d"] / W_DEFAULT + 1), m["mt"]) for m in moves)
    bands = [pts[i * len(pts) // 8 : (i + 1) * len(pts) // 8] for i in range(8)]
    bx = [statistics.median(p[0] for p in g) for g in bands]
    by = [statistics.median(p[1] for p in g) for g in bands]
    mx, my = statistics.fmean(bx), statistics.fmean(by)
    sxx = sum((x - mx) ** 2 for x in bx)
    b = sum((x - mx) * (y - my) for x, y in zip(bx, by)) / sxx if sxx else 0.12
    b = min(max(b / width, 0.03), 0.4)
    return min(max((my / width) - b * mx, 0.0), 0.4), b


def derive(recordings):
    """The profile's values from these recordings, and the keys too few samples left out."""
    moves, rests, gaps = [], [], []
    holds, double, leads, drops, spins = [], [], [], [], []
    for r in recordings:
        moves += [m for m in (measure(*s) for s in aimed(r)) if m]
        rests += arrivals(r)
        h, g, l, d = button_times(r)
        holds += h
        double += g
        leads += l
        drops += d
        spins += [(b - a) * 1000 for a, b in zip(r.wheel, r.wheel[1:]) if b - a < 0.4]
        gaps += [(b - a) * 1000 for a, b in zip(r.reports, r.reports[1:]) if 0.0002 < b - a < 0.05]

    prof, short = {}, []

    def have(keys, enough):
        if not enough:
            short.extend(keys)
        return enough

    whole = [m for m in moves if not m["leg"]]
    strokes = [m for m in whole if "sharp" in m and m["d"] >= 100]
    width = 1.0
    if have(["bell", "peak"], len(strokes) >= ENOUGH):
        s, p, width = bell(statistics.median(m["sharp"] for m in strokes), statistics.median(m["peak"] for m in strokes))
        prof["bell"], prof["peak"] = s, p
    if have(["fitts_a_ms", "fitts_b_ms"], len(whole) >= ENOUGH):
        a, b = fitts(whole, width)
        prof["fitts_a_ms"], prof["fitts_b_ms"] = round(a * 1000), round(b * 1000)

    bows = [m["bow"] for m in whole if "bow" in m and m["d"] >= 100]
    if have(["bow_median", "bow_sigma"], len(bows) >= ENOUGH):
        prof["bow_median"], prof["bow_sigma"] = round(statistics.median(bows), 4), round(log_spread(bows), 3)

    far = [m for m in whole if m["d"] >= 100]
    fixed = [m for m in far if m["corrected"]]
    if have(["correct_p"], len(far) >= ENOUGH):
        prof["correct_p"] = round(len(fixed) / len(far), 3)
    if have(["under_share", "correct_size"], len(fixed) >= 5):
        prof["under_share"] = round(sum(m["short"] for m in fixed) / len(fixed), 3)
        prof["correct_size"] = round(statistics.median(m["left"] for m in fixed), 4)

    if have(["rest_ms", "rest_sigma"], len(rests) >= ENOUGH):
        prof["rest_ms"], prof["rest_sigma"] = round(statistics.median(rests)), round(log_spread(rests), 3)

    if have(["hold_ms", "hold_sigma"], len(holds) >= ENOUGH):
        prof["hold_ms"], prof["hold_sigma"] = round(statistics.median(holds)), round(log_spread(holds), 3)
    if have(["double_gap_ms"], len(double) >= 10):
        prof["double_gap_ms"] = round(statistics.median(double))
    if have(["drag_lead_ms"], len(leads) >= 10):
        prof["drag_lead_ms"] = round(statistics.median(leads))
    if have(["drop_rest_ms"], len(drops) >= 10):
        prof["drop_rest_ms"] = round(statistics.median(drops))
    if have(["wheel_ms"], len(spins) >= ENOUGH):
        prof["wheel_ms"] = round(statistics.median(spins))

    if have(["rate_hz"], len(gaps) >= 200):
        bins = {}
        for g in gaps:
            bins[int(g * 2)] = bins.get(int(g * 2), 0) + 1
        prof["rate_hz"] = round(1000 / ((max(bins, key=bins.get) + 0.5) / 2))
    prof["moves"] = len(moves)
    return prof, short


def fit(a):
    dirs = a.dirs or sorted(os.path.join(STATE, "learn", d) for d in os.listdir(os.path.join(STATE, "learn")))
    prof, short = derive([Recording(d) for d in dirs])
    if prof["moves"] < 20:
        sys.exit(f"learn: only {prof['moves']} movements ending in a click; use the mouse a while longer")
    os.makedirs(os.path.dirname(a.profile), exist_ok=True)
    with open(a.profile, "w") as f:
        f.write("# vclick glide profile, fitted by learn.py from the user's own movements\n")
        for k, v in prof.items():
            f.write(f"{k}={v}\n")
    print(f"learn: {prof['moves']} movements ending in a click; profile: {a.profile}")
    for k, v in prof.items():
        print(f"  {k} = {v}")
    if short:
        print(f"  too few samples, vclick's defaults stand: {', '.join(short)}")


def show(a):
    with open(a.profile) as f:
        sys.stdout.write(f.read())


def main():
    p = argparse.ArgumentParser(description="record the user's pointer movements and fit vclick's glide profile")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--minutes", type=float, default=30)
    r.add_argument("--out")
    f = sub.add_parser("fit")
    f.add_argument("dirs", nargs="*")
    f.add_argument("--profile", default=PROFILE)
    s = sub.add_parser("show")
    s.add_argument("--profile", default=PROFILE)
    a = p.parse_args()
    {"record": record, "fit": fit, "show": show}[a.cmd](a)


if __name__ == "__main__":
    main()
