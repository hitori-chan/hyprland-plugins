#!/usr/bin/env bash
# sweep.sh BASE TIP [WORKERS] — at every commit in BASE..TIP each plugin
# (awesome, vclick) builds (the release variant) and passes its
# `make test`, against the fork headers the gate staged (`gate.sh -s`:
# $HYPR_GATE_TMP/fork-pkg). Run it before a push, after a history rewrite.
#
# Fast by construction, never by checking less:
#  - a plugin at a commit is keyed by its build inputs (its directory
#    minus the docs; awesome's minus the gate, vclick's minus its nested
#    check), the staged version.h and the compiler; a key that passed once
#    (this run or an earlier one) is skipped: same inputs, same result
#  - compiles go through ccache when it is installed: a translation unit
#    whose preprocessed source is unchanged comes from the cache, across
#    workers, commits and later sweeps
#  - -O0: the builds have no -Werror and the unit tests do not depend on
#    it, so pass/fail is the same; the -O2 binary is the gate's
#  - WORKERS (default 3) worktrees take contiguous chunks in order, each
#    one incremental; they split the memory cap's job budget
#    (memcap.sh) and make's load limit shares the cores between them
# One line per plugin built (ok / skip / FAIL), the summary line last; exit
# 1 on any failure. Build logs: $HYPR_GATE_TMP/sweep-w*-{build,test}.log.
set -u
[[ $# -ge 2 ]] || { echo "usage: sweep.sh BASE TIP [WORKERS]" >&2; exit 2; }
source "$(dirname "${BASH_SOURCE[0]}")/memcap.sh" # the run's memory cap: before anything builds or launches
BASE=$1 TIP=$2 W=${3:-3}
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROOT=${HYPR_GATE_TMP:-/tmp/hypr-gate}
PKG=$ROOT/fork-pkg
[[ -f $PKG/include/hyprland/src/version.h ]] || { echo "sweep.sh: no staged headers in $PKG: run the gate with -s first" >&2; exit 2; }
export PKG_CONFIG_PATH=$PKG/share/pkgconfig
export CXXFLAGS=-O0 # the environment, not a make argument: the Makefile's += still applies
# the compiler itself carries ccache: a swept commit's Makefile may predate
# the CCACHE launcher (memcap.sh), and a newer one must not wrap it twice
command -v ccache >/dev/null && export CXX="ccache g++" CCACHE=
OK=$ROOT/sweep-ok.keys
mkdir -p "$ROOT" && touch "$OK"

SALT="$(sha1sum < "$PKG/include/hyprland/src/version.h")$(g++ -dumpfullversion)"
key() { # key <commit> <plugin>: ls-tree takes no exclude pathspec, so filter its "<mode> <type> <id>\t<path>" lines
	{ git -C "$REPO" ls-tree -r "$1" -- "$2" | grep -v -e $'\tawesome/gate/' -e $'\tvclick/check.sh$' -e '\.md$'; echo "$2 $SALT"; } | sha1sum | cut -c1-40
}

declare -A seen
todo=()
skipped=0
mapfile -t ALL < <(git -C "$REPO" rev-list --reverse "$BASE..$TIP")
for c in "${ALL[@]}"; do
	for p in awesome vclick; do
		git -C "$REPO" cat-file -e "$c:$p/Makefile" 2>/dev/null || continue
		k=$(key "$c" "$p")
		if grep -qx "$k" "$OK" || [[ -n "${seen[$k]:-}" ]]; then
			printf 'skip        %-8s %s %s\n' "$p" "${c:0:7}" "$(git -C "$REPO" log -1 --format=%s "$c")"
			skipped=$((skipped + 1))
			continue
		fi
		seen[$k]=1
		todo+=("$c $p $k")
	done
done

# the worktrees live on tmpfs: a reboot takes them but not their
# registration, and `worktree add` refuses a registered path
git -C "$REPO" worktree prune

worker() { # worker N "COMMIT PLUGIN KEY"... — in order, incremental, in its own worktree
	local n=$1
	shift
	local wt=$ROOT/sweep-$n out=$ROOT/sweep-w$n.out
	# a tmp cleaner can take the .git link and leave the tree, and a copied
	# repository brings another checkout's worktree records: rebuild it
	{ [[ -e $wt/.git ]] && git -C "$wt" rev-parse -q --verify HEAD > /dev/null 2>&1; } ||
		{ rm -rf "$wt" && git -C "$REPO" worktree prune && git -C "$REPO" worktree add -q --detach "$wt" HEAD; }
	: > "$out"
	for job in "$@"; do
		local c p k subj
		read -r c p k <<< "$job"
		subj=$(printf '%-8s %s %s' "$p" "${c:0:7}" "$(git -C "$REPO" log -1 --format=%s "$c")")
		# forced: a stale index or stray file must never leave the last
		# commit's tree under this one's name (ignored build output stays)
		if ! git -C "$wt" checkout -q -f --detach "$c" || ! git -C "$wt" clean -fdq; then
			echo "$c $p FAIL checkout $subj" >> "$out"
			continue
		fi
		if ! make -C "$wt/$p" -j"$J" -l"$(nproc)" > "$ROOT/sweep-w$n-build.log" 2>&1; then
			echo "$c $p FAIL build  $subj" >> "$out"
			continue
		fi
		# awesome's harness prints its verdict; vclick's suites fail make
		if ! make -C "$wt/$p" -j"$J" test > "$ROOT/sweep-w$n-test.log" 2>&1 || { [[ $p == awesome ]] && ! grep -q "ALL .* CASES PASSED" "$ROOT/sweep-w$n-test.log"; }; then
			echo "$c $p FAIL test   $subj" >> "$out"
			continue
		fi
		echo "$k" >> "$OK"
		echo "$c $p ok          $subj" >> "$out"
	done
}

n=${#todo[@]}
((W > n)) && W=$n
# the scope's build budget, shared by the workers that have work
J=$((HYPR_HARNESS_JOBS / (W > 0 ? W : 1)))
((J < 1)) && J=1
rm -f "$ROOT"/sweep-w*.out
pids=()
for ((i = 0; i < W; i++)); do
	lo=$((i * n / W)) hi=$(((i + 1) * n / W))
	worker "$i" "${todo[@]:lo:hi-lo}" &
	pids+=($!)
done
((${#pids[@]})) && wait "${pids[@]}"

fails=0
for job in "${todo[@]}"; do
	read -r c p _ <<< "$job"
	line=$(cat "$ROOT"/sweep-w*.out 2> /dev/null | grep -m1 "^$c $p " | cut -d' ' -f3-)
	echo "${line:-FAIL lost    $p ${c:0:7}}"
	[[ "$line" == ok* ]] || fails=$((fails + 1))
done
echo "== sweep: ${#ALL[@]} commits, $n builds, $skipped skipped, $fails failed =="
((fails == 0))
