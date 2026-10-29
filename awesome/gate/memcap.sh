# awesome/gate/memcap.sh — sourced first by every entry point that builds
# or runs a nested (gate, probe, dev, launch, sweep): the run lives in a
# memory-capped cgroup of its own.
#
# The host can sit near its limit (other workloads, swap nearly full). The
# kernel's global OOM killer then takes whatever allocates next; under
# vm.oom_kill_allocating_task that is often the LIVE compositor
# (2026-10-26: a probe's one-file rebuild tipped such a host and killed
# the session). So a run
#   * waits to start until the host has its budget free;
#   * runs in a systemd user scope with MemoryMax and no swap: a run that
#     outgrows its budget is OOM-killed inside the scope, never outside;
#   * is killed whole (cgroup.kill) by a watchdog when the host itself
#     runs low, whoever's fault: the session gets the run's memory back.
#   HYPR_HARNESS_MEM_MAX    the scope's ceiling, MiB (default 3072)
#   HYPR_HARNESS_MEM_FLOOR  MemAvailable+SwapFree, MiB, below which the
#                           watchdog kills the run (default 1024)
#   HYPR_HARNESS_MEM_WAIT   seconds to wait for the budget (default 600)
# Exports HYPR_HARNESS_JOBS: the build parallelism the budget affords,
# and CCACHE (ccache, when installed) as the build's compiler launcher.

_mem_left() { awk '/^MemAvailable:/ { a = $2 } /^SwapFree:/ { s = $2 } END { print int((a + s) / 1024) }' /proc/meminfo; }

if [[ -z "${HYPR_HARNESS_SCOPE:-}" ]]; then
	_max=${HYPR_HARNESS_MEM_MAX:-3072} _floor=${HYPR_HARNESS_MEM_FLOOR:-1024}
	command -v systemd-run >/dev/null || { echo "$(basename "$0"): systemd-run is required (the run's memory cap)" >&2; exit 2; }
	_need=$((_max + _floor)) _waited=0
	while (($(_mem_left) < _need)); do
		((_waited == 0)) && echo "$(basename "$0"): waiting for ${_need} MiB of host memory ($(_mem_left) free)" >&2
		((_waited >= ${HYPR_HARNESS_MEM_WAIT:-600})) && { echo "$(basename "$0"): the host never freed ${_need} MiB: not starting" >&2; exit 2; }
		sleep 5
		_waited=$((_waited + 5))
	done
	export HYPR_HARNESS_SCOPE="$_max:$_floor"
	# --scope runs the command in this process: the tty, the pipe to a tee
	# and the exit status stay the caller's. OOMPolicy=continue: a kill
	# inside fails the run through its own checks (and teardown), not a
	# SIGTERM to the whole scope
	exec systemd-run --user --scope --quiet --collect --unit="hypr-harness-$$" \
		-p MemoryMax="${_max}M" -p MemorySwapMax=0 -p OOMPolicy=continue -- bash "$0" "$@"
fi

if [[ -z "${HYPR_HARNESS_JOBS:-}" ]]; then
	_max=${HYPR_HARNESS_SCOPE%%:*} _floor=${HYPR_HARNESS_SCOPE##*:}
	# an -O2 translation unit peaks near 780 MiB, 600 through the gate
	# variant's precompiled header (core/pch.hpp); the nested is not up
	# yet while the preflight builds
	HYPR_HARNESS_JOBS=$(((_max - 512) / 768)) HYPR_HARNESS_GATE_JOBS=$(((_max - 512) / 600))
	((HYPR_HARNESS_JOBS < 1)) && HYPR_HARNESS_JOBS=1
	((HYPR_HARNESS_JOBS > $(nproc))) && HYPR_HARNESS_JOBS=$(nproc)
	((HYPR_HARNESS_GATE_JOBS < 1)) && HYPR_HARNESS_GATE_JOBS=1
	((HYPR_HARNESS_GATE_JOBS > $(nproc))) && HYPR_HARNESS_GATE_JOBS=$(nproc)
	export HYPR_HARNESS_JOBS HYPR_HARNESS_GATE_JOBS
	# the Makefile's compiler launcher: unchanged sources are cache hits
	export CCACHE="${CCACHE-$(command -v ccache 2>/dev/null)}"
	_cg="/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)"
	# The watchdog lives as long as anything else in the scope (a manual
	# launch leaves its nested behind), ignores the run's ^C (its teardown
	# still needs watching) and reports what the cap did.
	if [[ -w "$_cg/cgroup.kill" ]]; then
		(
			trap '' INT
			exec 9>&-
			while sleep 1; do
				_others=0
				while read -r _p; do [[ "$_p" != "$BASHPID" ]] && _others=1; done <"$_cg/cgroup.procs"
				((_others)) || break
				if (($(_mem_left) < _floor)); then
					echo "memcap: the host is down to $(_mem_left) MiB: killing this run to spare the session" >&2
					echo 1 >"$_cg/cgroup.kill"
				fi
			done
			_oom=$(awk '$1 == "oom_kill" { print $2 }' "$_cg/memory.events")
			((_oom > 0)) && echo "memcap: the run outgrew its ${_max} MiB cap: ${_oom} process(es) killed inside it (HYPR_HARNESS_MEM_MAX)" >&2
			echo "memcap: peak $(($(cat "$_cg/memory.peak") >> 20)) of ${_max} MiB" >&2
		) &
	fi
	unset _max _floor _cg
fi
