#!/usr/bin/env python3
# profile.py TRACE [N] — where a gate run's wall time went.
#
# gate.sh -p traces the batteries (xtrace, a timestamp and the call stack
# per line, gate.sh's PS4) into TRACE. A main-shell line owns the time
# until the next main-shell line: a command substitution or a pipeline's
# subshell bills its caller. Prints the top N battery lines (the frame in
# a battery file: what a trim or a rewrite would change) and the top N
# lines where the time was actually spent (a helper's sleep, a capture).
import collections, os, sys

BATTERIES = {f"{b}.sh" for b in ("quick", "shell", "windows", "notify", "system", "pipeline", "lifecycle")}


def main():
    path, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 30
    recs = []
    with open(path, errors="replace") as f:
        for line in f:
            # +<subshell>\t<epoch>\t<sources>\t<lineno> <bash_lineno...>\t<command>
            parts = line.lstrip("+").split("\t", 4)
            if len(parts) < 5 or parts[0] != "0":
                continue
            try:
                t = float(parts[1])
            except ValueError:
                continue
            srcs = [os.path.basename(s) for s in parts[2].split()]
            lines = parts[3].split()
            frames = list(zip(srcs, [lines[0]] + lines[1:len(srcs)]))
            here = f"{frames[0][0]}:{frames[0][1]}" if frames else "?"
            owner = next((f"{s}:{l}" for s, l in frames if s in BATTERIES), here)
            recs.append((t, owner, here, parts[4].strip()[:80]))
    by_owner, by_here, by_battery, text = collections.Counter(), collections.Counter(), collections.Counter(), {}
    for (t, owner, here, cmd), nxt in zip(recs, recs[1:]):
        d = nxt[0] - t
        by_owner[owner] += d
        by_here[here] += d
        by_battery[owner.split(":")[0]] += d
        text.setdefault(owner, cmd)
        text.setdefault(here, cmd)
    print("== per file ==")
    for k, v in by_battery.most_common():
        print(f"{v:8.1f} s  {k}")
    print(f"== top {n} battery lines ==")
    for k, v in by_owner.most_common(n):
        print(f"{v:8.1f} s  {k:18s} {text[k]}")
    print(f"== top {n} lines the time was spent on ==")
    for k, v in by_here.most_common(n):
        print(f"{v:8.1f} s  {k:18s} {text[k]}")


main()
