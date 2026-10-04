# awesome/gate/host.sh — the nested's private host compositor. Sourced by
# launch.sh and stop.sh.
#
# The nested Hyprland runs aquamarine's WAYLAND backend (its headless
# backend cannot allocate buffers on its own), so it needs a parent
# compositor. That parent used to be the user's LIVE session: an output,
# a workspace and a window there, and the user's Super+N and cursor landed
# on the gate (2026-10-04). Now the parent is a private headless labwc:
# its own socket, no input devices, never connected to the live display,
# rendering through the GPU render node. The live session is not a party
# to any run.
#
# The nested also loads aquamarine at the fork's flake.lock pin, built
# once into $HYPR_GATE_TMP/deps: the distro 0.15.1 never flushes the
# output's first commit to an idle parent (fixed upstream in 7bb8bdf), so
# under a quiet headless host the configure handshake never starts. The
# live session keeps the distro library; only the nested loads the pin.

HYPR_GATE_TMP="${HYPR_GATE_TMP:-/tmp/hypr-gate}"
HOST_DIR="$HARNESS_DIR/host"
HOST_BIN="${HYPR_HOST_BIN:-$(command -v labwc 2>/dev/null)}"
HOST_LIBS="${HYPR_HOST_LIBS:-}" # LD_LIBRARY_PATH for an unpacked labwc (HYPR_HOST_BIN)
AQ_SRC="${HYPR_AQ_SRC:-$HOME/repo/aquamarine}"

host_alive() {
	local pid
	pid="$(cat "$HOST_DIR/pid" 2>/dev/null)"
	[[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null && [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" == labwc ]] &&
		[[ -S "$XDG_RUNTIME_DIR/$(cat "$HOST_DIR/wl" 2>/dev/null)" ]]
}

# host_start <WxH>: start (or keep) the host; the nested window is placed
# at 0,0 and sized WxH by a window rule (the headless output's own size
# does not matter — frame callbacks follow the surface, not its extent)
host_start() {
	local size=$1 before wl pid
	host_alive && return 0
	host_stop
	[[ -x "$HOST_BIN" ]] || { echo "host: no labwc (install labwc, or set HYPR_HOST_BIN)" >&2; return 1; }
	mkdir -p "$HOST_DIR/xdg/labwc"
	cat >"$HOST_DIR/xdg/labwc/rc.xml" <<-EOF
		<?xml version="1.0"?>
		<labwc_config>
		  <core><gap>0</gap></core>
		  <windowRules>
		    <windowRule identifier="aquamarine" serverDecoration="no">
		      <action name="MoveTo" x="0" y="0" />
		      <action name="ResizeTo" width="${size%x*}" height="${size#*x}" />
		    </windowRule>
		  </windowRules>
		</labwc_config>
	EOF
	before="$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort)"
	# nothing of the user's session: no display to connect to, no session
	# bus to activate on, no activation-environment update, no seat
	env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u DISPLAY -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_ID \
		-u LABWC_UPDATE_ACTIVATION_ENV DBUS_SESSION_BUS_ADDRESS=disabled: \
		XDG_CONFIG_HOME="$HOST_DIR/xdg" LD_LIBRARY_PATH="$HOST_LIBS" \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2 LIBSEAT_BACKEND=seatd \
		setsid "$HOST_BIN" >"$HOST_DIR/log" 2>&1 &
	pid=$!
	for _ in $(seq 1 50); do
		sleep 0.1
		wl="$(comm -13 <(printf '%s\n' "$before") <(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | sort) | head -1)"
		[[ -n "$wl" ]] && break
	done
	if [[ -z "$wl" ]] || ! kill -0 "$pid" 2>/dev/null; then
		echo "host: labwc did not come up — $HOST_DIR/log" >&2
		kill "$pid" 2>/dev/null
		return 1
	fi
	printf '%s\n' "$pid" >"$HOST_DIR/pid"
	printf '%s\n' "$wl" >"$HOST_DIR/wl"
}

host_stop() {
	local pid
	pid="$(cat "$HOST_DIR/pid" 2>/dev/null)"
	if [[ "$pid" =~ ^[0-9]+$ ]] && [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" == labwc ]]; then
		kill "$pid" 2>/dev/null
		for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
		kill -9 "$pid" 2>/dev/null
	fi
	rm -f "$HOST_DIR/pid" "$HOST_DIR/wl"
}

# The pinned aquamarine's lib dir (built on first use, cached by rev).
aq_pinned_libdir() {
	local rev prefix src
	rev="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['nodes']['aquamarine']['locked']['rev'])" "${FORK_SRC:-$HOME/repo/Hyprland}/flake.lock" 2>/dev/null)"
	[[ "$rev" =~ ^[0-9a-f]{40}$ ]] || { echo "host: no aquamarine pin in the fork's flake.lock" >&2; return 1; }
	prefix="$HYPR_GATE_TMP/deps/aquamarine-${rev:0:12}"
	if [[ ! -e "$prefix/lib/libaquamarine.so" ]]; then
		git -C "$AQ_SRC" cat-file -e "$rev^{commit}" 2>/dev/null || git -C "$AQ_SRC" fetch -q origin 2>/dev/null
		src="$HYPR_GATE_TMP/deps/aq-src-${rev:0:12}"
		rm -rf "$src" "$src.build" && mkdir -p "$src" &&
			git -C "$AQ_SRC" archive "$rev" | tar -x -C "$src" &&
			cmake -S "$src" -B "$src.build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null &&
			cmake --build "$src.build" -j"$(nproc)" >/dev/null && cmake --install "$src.build" >/dev/null || {
			echo "host: building aquamarine $rev failed" >&2
			return 1
		}
		rm -rf "$src" "$src.build"
	fi
	printf '%s\n' "$prefix/lib"
}

# --- the private system bus -------------------------------------------------
# A dbus-daemon of the nested's own with a fake logind (devtools/fakes):
# the plugin's brightness path sends its real SetBrightness calls, the fake
# writes a fake backlight class dir ($AW_BACKLIGHT_DIR, read by the plugin),
# and the host's panel never moves. Nothing else on it: the nested's other
# system-bus clients (power-profiles, apps) find no service and degrade.
SYSBUS_DIR="$HARNESS_DIR/sysbus"
SYSBUS_ADDR="unix:path=$SYSBUS_DIR/bus"
SYSBUS_BACKLIGHT="${AW_BACKLIGHT_DIR:-$SYSBUS_DIR/backlight}"
SYSBUS_FAKE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/devtools/fakes/fake-logind"

# a recorded pid is ours only while it still runs the program we started
# (a pid file can outlive a reboot; the pid is then someone else's)
_sysbus_pid() { # _sysbus_pid <file> <comm>
	local p
	p="$(cat "$1" 2>/dev/null)"
	[[ "$p" =~ ^[0-9]+$ && "$(cat "/proc/$p/comm" 2>/dev/null)" == "$2" ]] && printf '%s\n' "$p"
}

sysbus_alive() {
	_sysbus_pid "$SYSBUS_DIR/daemon.pid" dbus-daemon >/dev/null && _sysbus_pid "$SYSBUS_DIR/logind.pid" python3 >/dev/null &&
		[[ -S "$SYSBUS_DIR/bus" ]]
}

sysbus_start() {
	# the fake device is (re)seeded every launch: the battery steps from 500
	if [[ ! -e "$SYSBUS_BACKLIGHT/gate0/max_brightness" ]]; then
		mkdir -p "$SYSBUS_BACKLIGHT/gate0" && printf '1000\n' >"$SYSBUS_BACKLIGHT/gate0/max_brightness"
	fi
	printf '500\n' >"$SYSBUS_BACKLIGHT/gate0/brightness"
	sysbus_alive && return 0
	sysbus_stop
	mkdir -p "$SYSBUS_DIR"
	cat >"$SYSBUS_DIR/bus.conf" <<-EOF
		<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
		 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
		<busconfig>
		  <type>system</type>
		  <listen>$SYSBUS_ADDR</listen>
		  <auth>EXTERNAL</auth>
		  <policy context="default">
		    <allow user="*"/>
		    <allow own="*"/>
		    <allow send_destination="*" eavesdrop="true"/>
		    <allow receive_sender="*"/>
		    <allow eavesdrop="true"/>
		  </policy>
		</busconfig>
	EOF
	setsid dbus-daemon --config-file="$SYSBUS_DIR/bus.conf" --nofork --nopidfile >"$SYSBUS_DIR/daemon.log" 2>&1 &
	printf '%s\n' "$!" >"$SYSBUS_DIR/daemon.pid"
	for _ in $(seq 1 30); do [[ -S "$SYSBUS_DIR/bus" ]] && break; sleep 0.1; done
	DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" AW_BACKLIGHT_DIR="$SYSBUS_BACKLIGHT" AW_LOGIND_LOG="${AW_LOGIND_LOG:-$SYSBUS_DIR/logind.log}" \
		setsid python3 "$SYSBUS_FAKE" >"$SYSBUS_DIR/logind.stderr" 2>&1 &
	printf '%s\n' "$!" >"$SYSBUS_DIR/logind.pid"
	for _ in $(seq 1 30); do
		DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS_ADDR" timeout 1 busctl --system status org.freedesktop.login1 >/dev/null 2>&1 && return 0
		sleep 0.1
	done
	echo "host: the private system bus / fake logind did not come up — $SYSBUS_DIR" >&2
	return 1
}

sysbus_stop() {
	local p
	p="$(_sysbus_pid "$SYSBUS_DIR/logind.pid" python3)" && kill "$p" 2>/dev/null
	p="$(_sysbus_pid "$SYSBUS_DIR/daemon.pid" dbus-daemon)" && kill "$p" 2>/dev/null
	rm -f "$SYSBUS_DIR/logind.pid" "$SYSBUS_DIR/daemon.pid" "$SYSBUS_DIR/bus"
}
