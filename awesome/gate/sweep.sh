#!/usr/bin/env bash
# awesome/gate/sweep.sh BASE TIP [WORKERS] — every commit in BASE..TIP
# builds (the release variant) and passes `make test`, against the fork
# headers the gate staged (`gate.sh -s`: $HYPR_GATE_TMP/fork-pkg). Run it
# before a push, after a history rewrite.
#
# Fast by construction, never by checking less:
#  - a commit is keyed by its build inputs (awesome/ minus the gate and
#    the docs), the staged version.h and the compiler; a key that passed
#    once (this run or an earlier one) is skipped: same inputs, same result
#  - compiles go through ccache when it is installed: a translation unit
#    whose preprocessed source is unchanged comes from the cache, across
#    workers, commits and later sweeps
#  - -O0: the build has no -Werror and the unit tests build at -O0
#    already, so pass/fail is the same; the -O2 binary is the gate's
#  - WORKERS (default 3) worktrees take contiguous chunks in order, each
#    one incremental; they split the memory cap's job budget
#    (memcap.sh) and make's load limit shares the cores between them
# One line per commit (ok / skip / FAIL), the summary line last; exit 1 on
# any failure. Build logs: $HYPR_GATE_TMP/sweep-w*-{build,test}.log.
set -u
[[ $# -ge 2 ]] || { echo "usage: sweep.sh BASE TIP [WORKERS]" >&2; exit 2; }
source "$(dirname "${BASH_SOURCE[0]}")/memcap.sh" # the run's memory cap: before anything builds or launches
BASE=$1 TIP=$2 W=${3:-3}
J=$((HYPR_HARNESS_JOBS / W)) # the scope's build budget, shared by the workers
((J < 1)) && J=1
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT=${HYPR_GATE_TMP:-/tmp/hypr-gate}
PKG=$ROOT/fork-pkg
[[ -f $PKG/include/hyprland/src/version.h ]] || { echo "sweep.sh: no staged headers in $PKG: run the gate with -s first" >&2; exit 2; }
export PKG_CONFIG_PATH=$PKG/share/pkgconfig
export CXXFLAGS=-O0 # the environment, not a make argument: the Makefile's += still applies
command -v ccache >/dev/null && export CXX="ccache g++"
OK=$ROOT/sweep-ok.keys
mkdir -p "$ROOT" && touch "$OK"

SALT="$(sha1sum < "$PKG/include/hyprland/src/version.h")$(g++ -dumpfullversion)"
key() { # ls-tree takes no exclude pathspec: filter its "<mode> <type> <id>\t<path>" lines
	{ git -C "$REPO" ls-tree -r "$1" -- awesome | grep -v -e $'\tawesome/gate/' -e '\.md$'; echo "$SALT"; } | sha1sum | cut -c1-40
}

declare -A seen
todo=()
mapfile -t ALL < <(git -C "$REPO" rev-list --reverse "$BASE..$TIP")
for c in "${ALL[@]}"; do
	k=$(key "$c")
	if grep -qx "$k" "$OK" || [[ -n "${seen[$k]:-}" ]]; then
		echo "skip        ${c:0:7} $(git -C "$REPO" log -1 --format=%s "$c")"
		continue
	fi
	seen[$k]=1
	todo+=("$c $k")
done

worker() { # worker N "COMMIT KEY"... — in order, incremental, in its own worktree
	local n=$1
	shift
	local wt=$ROOT/sweep-$n out=$ROOT/sweep-w$n.out
	[[ -d $wt ]] || git -C "$REPO" worktree add -q --detach "$wt" HEAD
	: > "$out"
	for ck in "$@"; do
		local c=${ck% *} k=${ck#* } subj
		git -C "$wt" checkout -q --detach "$c"
		subj=$(git -C "$wt" log -1 --format=%s)
		if ! make -C "$wt/awesome" -j"$J" -l"$(nproc)" > "$ROOT/sweep-w$n-build.log" 2>&1; then
			echo "$c FAIL build  ${c:0:7} $subj" >> "$out"
			continue
		fi
		if ! make -C "$wt/awesome" test > "$ROOT/sweep-w$n-test.log" 2>&1 || ! grep -q "ALL .* CASES PASSED" "$ROOT/sweep-w$n-test.log"; then
			echo "$c FAIL test   ${c:0:7} $subj" >> "$out"
			continue
		fi
		echo "$k" >> "$OK"
		echo "$c ok          ${c:0:7} $subj" >> "$out"
	done
}

n=${#todo[@]}
((W > n)) && W=$n
rm -f "$ROOT"/sweep-w*.out
pids=()
for ((i = 0; i < W; i++)); do
	lo=$((i * n / W)) hi=$(((i + 1) * n / W))
	worker "$i" "${todo[@]:lo:hi-lo}" &
	pids+=($!)
done
((${#pids[@]})) && wait "${pids[@]}"

fails=0
for ck in "${todo[@]}"; do
	line=$(cat "$ROOT"/sweep-w*.out 2> /dev/null | grep -m1 "^${ck% *} " | cut -d' ' -f2-)
	echo "${line:-FAIL lost    ${ck:0:7}}"
	[[ "$line" == ok* ]] || fails=$((fails + 1))
done
echo "== sweep: ${#ALL[@]} commits, $n built, $((${#ALL[@]} - n)) skipped, $fails failed =="
((fails == 0))
