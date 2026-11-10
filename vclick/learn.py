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
  group), only motion and mouse buttons, never a keyboard: the exact time
  of each report and of each click.

It never grabs a device: every event still reaches the compositor.

fit splits the recording into movements (a still pointer, a move, still
again), keeps those that end in a click or a clear stop, and fits:

- Fitts's law, MT = a + b * log2(D / W + 1), with W the default target
  width vclick assumes (32 px), through the medians of 8 bands of D;
- the path's bow: the largest sideways distance over the length;
- where the speed peaks in the move (0.5 is symmetric; people peak early);
- how often and how far a long move overshoots the target;
- how long the pointer rests on the target before a click;
- the device's report rate.

The profile is a key=value file vclick reads on load and on
`hyprctl vclick profile reload`.
"""
import argparse
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

EV_SYN, EV_KEY, EV_REL = 0x00, 0x01, 0x02
REL_X, REL_Y = 0x00, 0x01
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

def load(dirpath):
    pos = []
    with open(os.path.join(dirpath, "pos.csv")) as f:
        for line in f:
            t, x, y = line.strip().split(",")
            pos.append((float(t), float(x), float(y)))
    clicks, reports = [], {}
    try:
        with open(os.path.join(dirpath, "clock.json")) as f:
            offset = json.load(f)["realtime_minus_monotonic"]
    except (OSError, KeyError, ValueError):
        # an older recording: the offset now holds unless the machine slept since
        offset = time.time() - time.monotonic()
    try:
        with open(os.path.join(dirpath, "input.csv")) as f:
            for line in f:
                parts = line.strip().split(",")
                t = float(parts[0]) - offset  # to the monotonic clock
                if parts[2] == "btn" and parts[4] == "1":
                    clicks.append(t)
                elif parts[2] == "rel":
                    reports.setdefault(parts[1], []).append(t)
    except OSError:
        pass
    return pos, sorted(clicks), reports


def segments(pos, still_ms=90, min_d=24.0, max_s=2.5):
    """Movements between two rests: lists of (t, x, y). The poller writes a
    position only when it changes (and once a second when it does not), so
    a rest is a gap of still_ms or more between two samples."""
    out, cur = [], []
    for prev, p in zip(pos, pos[1:]):
        if p[1:] == prev[1:]:
            continue  # the once-a-second sample of a still pointer
        if p[0] - prev[0] >= still_ms / 1000:
            if len(cur) > 2:
                out.append(cur)
            cur = [(p[0] - 0.004, *prev[1:])]  # it left the rest one poll before
        elif not cur:
            cur = [prev]
        cur.append(p)
    if len(cur) > 2:
        out.append(cur)
    return [s for s in out
            if math.dist(s[0][1:], s[-1][1:]) >= min_d and 0.03 < s[-1][0] - s[0][0] <= max_s]


def analyse(s):
    t0, x0, y0 = s[0]
    t1, x1, y1 = s[-1]
    d = math.dist((x0, y0), (x1, y1))
    ux, uy = (x1 - x0) / d, (y1 - y0) / d
    along = [((x - x0) * ux + (y - y0) * uy) for _, x, y in s]
    side = [(-(x - x0) * uy + (y - y0) * ux) for _, x, y in s]
    bow = max(side, key=abs) / d
    over = max(0.0, max(along) - d)
    speeds = []
    for (ta, xa, ya), (tb, xb, yb) in zip(s, s[1:]):
        if tb > ta:
            speeds.append(((ta + tb) / 2, math.dist((xa, ya), (xb, yb)) / (tb - ta)))
    tpeak = max(speeds, key=lambda v: v[1])[0] if speeds else (t0 + t1) / 2
    return {"t0": t0, "t1": t1, "d": d, "mt": t1 - t0, "bow": bow, "over": over / d,
            "peak": (tpeak - t0) / (t1 - t0) if t1 > t0 else 0.5}


def fit(a):
    dirs = a.dirs or sorted(os.path.join(STATE, "learn", d) for d in os.listdir(os.path.join(STATE, "learn")))
    moves, clicks, reports = [], [], {}
    for d in dirs:
        pos, c, r = load(d)
        clicks += c
        for k, v in r.items():
            reports.setdefault(k, []).extend(v)
        moves += [m for m in (analyse(s) for s in segments(pos)) if abs(m["bow"]) <= 0.5]
    if len(moves) < 20:
        sys.exit(f"learn: only {len(moves)} movements recorded; use the mouse a while longer")

    # a move that ends in a click within 1.2 s is an aimed move: Fitts's own case
    clicked = []
    j = 0
    for m in moves:
        while j < len(clicks) and clicks[j] < m["t1"]:
            j += 1
        if j < len(clicks) and clicks[j] - m["t1"] <= 1.2:
            m["dwell"] = clicks[j] - m["t1"]
            clicked.append(m)
    aimed = clicked if len(clicked) >= 15 else moves

    # MT = a + b * ID, ID = log2(D / W + 1): least squares through the
    # medians of 8 equal-count ID bands. The medians shrug off hesitations;
    # trimming the slowest moves instead would drop mostly the long ones and
    # flatten b. Keep a, b in sane bounds.
    pts = sorted((math.log2(m["d"] / W_DEFAULT + 1), m["mt"]) for m in aimed)
    k = min(8, len(pts) // 2)
    bands = [pts[i * len(pts) // k : (i + 1) * len(pts) // k] for i in range(k)]
    bx = [statistics.median(p[0] for p in g) for g in bands]
    by = [statistics.median(p[1] for p in g) for g in bands]
    mx, my = statistics.fmean(bx), statistics.fmean(by)
    sxx = sum((x - mx) ** 2 for x in bx)
    b = sum((x - mx) * (y - my) for x, y in zip(bx, by)) / sxx if sxx else 0.12
    b = min(max(b, 0.03), 0.4)
    a_ = min(max(my - b * mx, 0.0), 0.4)

    long_moves = [m for m in aimed if m["d"] >= 300]
    over = [m["over"] for m in long_moves if m["over"] > 0.005]
    rates = []
    for v in reports.values():
        v.sort()
        # one report carries x and y at one time: intervals between reports;
        # slow drags stretch them, so the rate is the short end (10th percentile)
        dts = sorted(b_ - a_ for a_, b_ in zip(v, v[1:]) if 0.0002 < b_ - a_ < 0.02)
        if len(dts) > 200:
            rates.append(1 / dts[len(dts) // 10])

    prof = {
        "fitts_a_ms": round(a_ * 1000),
        "fitts_b_ms": round(b * 1000),
        "bow_mean": round(statistics.fmean(abs(m["bow"]) for m in aimed), 4),
        "bow_sd": round(statistics.pstdev(abs(m["bow"]) for m in aimed), 4),
        "peak": round(statistics.median(m["peak"] for m in aimed), 3),
        "overshoot_p": round(len(over) / len(long_moves), 3) if long_moves else 0.0,
        "overshoot": round(statistics.median(over), 4) if over else 0.0,
        "dwell_ms": round(statistics.median(m["dwell"] for m in clicked) * 1000) if clicked else 80,
        "rate_hz": round(max(rates)) if rates else 125,
        "moves": len(aimed),
    }
    os.makedirs(os.path.dirname(a.profile), exist_ok=True)
    with open(a.profile, "w") as f:
        f.write("# vclick glide profile, fitted by learn.py from the user's own movements\n")
        for k, v in prof.items():
            f.write(f"{k}={v}\n")
    print(f"learn: {len(moves)} movements, {len(clicked)} ending in a click; profile: {a.profile}")
    for k, v in prof.items():
        print(f"  {k} = {v}")


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
