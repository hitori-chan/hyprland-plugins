// The hyprosd port (Phase 3): the volume/brightness OSD. The Lua face is
// hl.plugin.osd.*. Nothing is drawn here — the value cards ride the bus
// thread's Notify (ids 9992 brightness / 9993 volume / 9995 mic, replaced
// in place like the old scripts pinned them).
//
// - Brightness is fork-free: current/max read from /sys/class/backlight,
//   ±5% linear steps, floor 2 raw, written through logind
//   Session.SetBrightness on the system bus (the bus thread); the card
//   waits for logind's ack (a refused write must not flash a percent that
//   never applied).
// - Volume/mic go through wpctl (PipeWire stays out of the process): the
//   set spawns, its pidfd tells the event loop when it's done, then the
//   get spawns with its stdout on a pipe the event loop drains — two short
//   forks per keypress, render/input never wait on any of it.
// - Everything is queued and drained from the event loop (never inside a
//   bind's input emission); every queue, chain, and child is bounded.

// The port keeps the C++ arithmetic (bounded steps, percentages, pids) and
// the bounded indices stay in range by construction; allow the cast lints at
// module scope rather than scatter allows through the geometry.
#![allow(
    clippy::cast_lossless,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss
)]

use std::collections::VecDeque;
use std::os::fd::AsRawFd;

use crate::bus::{BusCmd, BusHandle, Card};
use crate::ffi;
use crate::probe;

// The actions (the Lua face indexes them).
pub const ACT_VOL_UP: u8 = 0;
pub const ACT_VOL_DOWN: u8 = 1;
pub const ACT_VOL_MUTE: u8 = 2;
pub const ACT_MIC_MUTE: u8 = 3;
pub const ACT_BRI_UP: u8 = 4;
pub const ACT_BRI_DOWN: u8 = 5;

const MAX_ACTION_QUEUE: usize = 128;
const MAX_ACTIVE_CHAINS: usize = 16;
const MAX_ORPHANS: usize = 32;
const MAX_TRACKED_CHILDREN: usize = 32;
// The retained readback cap is also the work cap: a noisy producer is cut
// off at the cap (the close gives it SIGPIPE), never left in the read loop.
const MAX_READBACK: usize = 4096;
// A keypress bases its step on what the previous one just asked for:
// logind's write is asynchronous, so a fast repeat would read stale sysfs
// and re-step from the same value. Half a second of trust, then sysfs is
// the truth again (external tools, resume).
const TRUST_WINDOW_MS: i64 = 500;
const ORPHAN_TICK_MS: u32 = 100;

// A sequenced wpctl pair: the set (pidfd-watched) then the get (stdout-
// pipe-watched). Chains overlap freely under key repeat: every set runs
// (each IS a step), late gets just show the final state.
struct Chain {
    mic: bool,
    generation: u64,
    // 0 = the set is running (watching its pidfd), 1 = reading the get's stdout
    phase: u8,
    set_pid: i32,
    get_pid: i32,
    // the fd the live watch is on (the pidfd until set-done, then stdout)
    watch_fd: i32,
    // the get's stdout, held for the chain's lifetime (its drop closes the
    // pipe read end; phase 1 only)
    stdout: Option<std::process::ChildStdout>,
    out: Vec<u8>,
    token: u64,
}

pub struct OsdState {
    // The actions queue and drains from the event loop; a queue rather than
    // one deferred slot so a key-repeat burst never coalesces two steps.
    queued: VecDeque<u8>,
    drain_armed: bool,
    // Brightness (sysfs + logind, zero forks).
    backlight_dev: String,
    backlight_max: u32,
    last_set_raw: i64,
    last_set_at_ms: i64,
    // Generations: a late reply from an older step must not show a card.
    brightness_gen: u64,
    volume_gen: u64,
    mic_gen: u64,
    volume_feedback: u64,
    mic_feedback: u64,
    chains: Vec<Chain>,
    // Children without a live event source (the pidfd failed to open):
    // re-reaped by the orphan tick, bounded.
    orphans: Vec<i32>,
    orphan_tick: u64,
}

impl OsdState {
    pub fn new() -> Self {
        Self {
            queued: VecDeque::new(),
            drain_armed: false,
            backlight_dev: String::new(),
            backlight_max: 0,
            last_set_raw: -1,
            last_set_at_ms: 0,
            brightness_gen: 0,
            volume_gen: 0,
            mic_gen: 0,
            volume_feedback: 0,
            mic_feedback: 0,
            chains: Vec::new(),
            orphans: Vec::new(),
            orphan_tick: 0,
        }
    }
}

fn now_ms() -> i64 {
    let d = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    d.as_millis() as i64
}

// ---------------------------------------------------------------------------
// the Lua face (hl.plugin.osd.*)
// ---------------------------------------------------------------------------

fn lua_impl(action: u8) -> i32 {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = probe::state() {
            let mut o = probe::osd_lock(state);
            enqueue(state.ctx, &mut o, action);
        }
    }));
    0
}

pub fn lua_volume_up_impl() -> i32 {
    lua_impl(ACT_VOL_UP)
}
pub fn lua_volume_down_impl() -> i32 {
    lua_impl(ACT_VOL_DOWN)
}
pub fn lua_mute_impl() -> i32 {
    lua_impl(ACT_VOL_MUTE)
}
pub fn lua_mic_mute_impl() -> i32 {
    lua_impl(ACT_MIC_MUTE)
}
pub fn lua_brightness_up_impl() -> i32 {
    lua_impl(ACT_BRI_UP)
}
pub fn lua_brightness_down_impl() -> i32 {
    lua_impl(ACT_BRI_DOWN)
}

// ---------------------------------------------------------------------------
// the queue (drained from the event loop)
// ---------------------------------------------------------------------------

pub fn enqueue(ctx: ffi::Ctx, st: &mut OsdState, a: u8) {
    if st.queued.len() >= MAX_ACTION_QUEUE {
        return; // bounded backpressure under a key-repeat storm
    }
    st.queued.push_back(a);
    if !st.drain_armed && probe::arm_job(ctx, probe::JOB_OSD_DRAIN) {
        st.drain_armed = true;
    }
}

pub fn drain(ctx: ffi::Ctx, st: &mut OsdState) {
    st.drain_armed = false;
    reap_orphans(ctx, st);
    let Some(bus) = crate::bus::handle() else {
        // the bus thread never started: the steps are dropped (the C++ links
        // did the same when the bus was missing)
        st.queued.clear();
        return;
    };
    // collect first: the per-step calls borrow `st` (generations, chains)
    let queued: Vec<u8> = st.queued.drain(..).collect();
    for a in queued {
        match a {
            ACT_BRI_UP => brightness_step(&bus, st, 1),
            ACT_BRI_DOWN => brightness_step(&bus, st, -1),
            _ => wpctl_action(ctx, st, a),
        }
    }
}

// ---------------------------------------------------------------------------
// brightness (sysfs + logind, zero forks)
// ---------------------------------------------------------------------------

fn find_backlight(st: &mut OsdState) {
    let Ok(rd) = std::fs::read_dir("/sys/class/backlight") else {
        return;
    };
    for e in rd.flatten() {
        let max = match std::fs::read_to_string(e.path().join("max_brightness")) {
            Ok(s) => s.trim().parse::<u32>().unwrap_or(0),
            Err(_) => 0,
        };
        if max > 0 {
            st.backlight_dev = e.file_name().to_string_lossy().into_owned();
            st.backlight_max = max;
            return;
        }
    }
}

fn brightness_step(bus: &BusHandle, st: &mut OsdState, dir: i64) {
    let gen_id = st.brightness_gen.wrapping_add(1);
    st.brightness_gen = gen_id;
    if st.backlight_dev.is_empty() {
        return;
    }

    let now = now_ms();
    let mut raw: i64 = -1;
    if st.last_set_raw >= 0 && now - st.last_set_at_ms < TRUST_WINDOW_MS {
        raw = st.last_set_raw;
    } else if let Ok(s) = std::fs::read_to_string(format!(
        "/sys/class/backlight/{}/brightness",
        st.backlight_dev
    )) {
        raw = s.trim().parse::<i64>().unwrap_or(-1);
    }
    if raw < 0 {
        return;
    }

    // ±5% of max, linear, floored at 2 raw (the panel never goes black)
    let step = (st.backlight_max as f64 * 0.05).round() as i64;
    let step = step.max(1);
    raw = (raw + dir * step).clamp(2, st.backlight_max as i64);
    let pct = (100.0 * raw as f64 / st.backlight_max as f64).round() as i32;
    let card = Card::osd(
        9992,
        "display-brightness-symbolic",
        "Brightness",
        format!("{pct}%"),
        pct,
    );
    bus.send(BusCmd::SetBrightness {
        dev: st.backlight_dev.clone(),
        raw: raw as u32,
        gen_id,
        card,
    });
    st.last_set_raw = raw;
    st.last_set_at_ms = now;
}

/// A bus reply: logind refused the write (or the system bus is gone) — drop
/// the trust window so the next press re-reads sysfs.
pub fn on_brightness_failed(st: &mut OsdState, gen_id: u64) {
    if gen_id == st.brightness_gen {
        st.last_set_raw = -1;
    }
}

// ---------------------------------------------------------------------------
// volume / mic (wpctl, sequenced on the event loop)
// ---------------------------------------------------------------------------

fn tracked_children(st: &OsdState) -> usize {
    let chains = st
        .chains
        .iter()
        .map(|c| usize::from(c.set_pid > 0) + usize::from(c.get_pid > 0))
        .sum::<usize>();
    st.orphans.len() + chains
}

fn can_track_child(st: &OsdState, new_chain: bool) -> bool {
    (!new_chain || st.chains.len() < MAX_ACTIVE_CHAINS)
        && st.orphans.len() < MAX_ORPHANS
        && tracked_children(st) < MAX_TRACKED_CHILDREN
}

fn remember_orphan(ctx: ffi::Ctx, st: &mut OsdState, pid: i32) {
    if pid <= 0 || st.orphans.contains(&pid) {
        return;
    }
    // Every spawn is admitted through can_track_child(); this guard keeps a
    // late fallback bounded if the ownership path changes in the future.
    if st.orphans.len() >= MAX_ORPHANS {
        return;
    }
    st.orphans.push(pid);
    if st.orphan_tick == 0 {
        st.orphan_tick = probe::arm_timer_repeat(ctx, ORPHAN_TICK_MS, probe::JOB_OSD_ORPHAN_TICK);
    }
}

fn reap_orphans(ctx: ffi::Ctx, st: &mut OsdState) {
    // >0 reaped, <0 (ECHILD: SA_NOCLDWAIT already took it) dropped; 0 = still
    // running, keep the tick
    st.orphans.retain(|&p| ffi::waitpid_nohang(p) == 0);
    if st.orphans.is_empty() && st.orphan_tick != 0 {
        ffi::job_cancel(ctx, st.orphan_tick);
        st.orphan_tick = 0;
    }
}

pub fn drain_orphan_tick(ctx: ffi::Ctx, st: &mut OsdState) {
    reap_orphans(ctx, st);
}

fn wpctl_action(ctx: ffi::Ctx, st: &mut OsdState, a: u8) {
    // the program name + its arguments (separate: Command::new takes the
    // program, args() appends — a single array would duplicate the name)
    let (prog, argv): (&str, &[&str]) = match a {
        ACT_VOL_UP => (
            "wpctl",
            &["set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+"],
        ),
        ACT_VOL_DOWN => ("wpctl", &["set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"]),
        ACT_VOL_MUTE => ("wpctl", &["set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"]),
        ACT_MIC_MUTE => ("wpctl", &["set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"]),
        _ => return,
    };

    if !can_track_child(st, true) {
        return;
    }

    let pid = match std::process::Command::new(prog).args(argv).spawn() {
        Ok(child) => {
            let p = child.id() as i32;
            // Hyprland installs SA_NOCLDWAIT: no child exit status remains to
            // reap. The pidfd is the completion signal; the readback is the
            // authoritative state exposed to the user.
            drop(child);
            p
        }
        Err(_) => return,
    };

    let mic = a == ACT_MIC_MUTE;
    let generation = if mic {
        st.mic_gen = st.mic_gen.wrapping_add(1);
        st.mic_gen
    } else {
        st.volume_gen = st.volume_gen.wrapping_add(1);
        st.volume_gen
    };

    let pidfd = ffi::pidfd_open(pid);
    if pidfd < 0 {
        // no pidfd (EMFILE, ancient kernel): never block the loop on a
        // reap — the orphan list re-reaps it; the card is skipped
        remember_orphan(ctx, st, pid);
        return;
    }
    // SYS_pidfd_open takes no CLOEXEC flag; keep it out of concurrent spawns
    ffi::set_cloexec(pidfd);
    let idx = st.chains.len() as u32;
    st.chains.push(Chain {
        mic,
        generation,
        phase: 0,
        set_pid: pid,
        get_pid: -1,
        watch_fd: pidfd,
        stdout: None,
        out: Vec::new(),
        token: 0,
    });
    let token = probe::watch_job(ctx, pidfd, probe::JOB_OSD_SET_DONE, idx);
    if token == 0 {
        ffi::close_fd(pidfd);
        if ffi::waitpid_nohang(pid) == 0 {
            remember_orphan(ctx, st, pid);
        }
        st.chains.pop();
        return;
    }
    st.chains[idx as usize].token = token;
}

/// Fired by `JOB_OSD_SET_DONE`: the set child exited (its pidfd fired).
/// Spawn the readback with its stdout on a pipe.
pub fn on_set_done(ctx: ffi::Ctx, st: &mut OsdState, idx: u32) {
    let Ok(i) = usize::try_from(idx) else {
        return;
    };
    if i >= st.chains.len() || st.chains[i].phase != 0 {
        return; // stale (the chain already ended)
    }
    {
        let c = &mut st.chains[i];
        ffi::job_cancel(ctx, c.token);
        c.token = 0;
        ffi::close_fd(c.watch_fd); // the pidfd
        c.set_pid = -1;
    }

    if !can_track_child(st, false) {
        chain_done(ctx, st, idx);
        return;
    }

    let target = if st.chains[i].mic {
        "@DEFAULT_AUDIO_SOURCE@"
    } else {
        "@DEFAULT_AUDIO_SINK@"
    };
    let Ok(mut child) = std::process::Command::new("wpctl")
        .arg("get-volume")
        .arg(target)
        .stdout(std::process::Stdio::piped())
        .spawn()
    else {
        chain_done(ctx, st, idx);
        return;
    };
    let pid = child.id() as i32;
    let Some(out) = child.stdout.take() else {
        drop(child);
        chain_done(ctx, st, idx);
        return;
    };
    let fd = out.as_raw_fd();
    let token = probe::watch_job(ctx, fd, probe::JOB_OSD_GET_OUT, idx);
    if token == 0 {
        drop(out); // closes the pipe read end
        if ffi::waitpid_nohang(pid) == 0 {
            remember_orphan(ctx, st, pid);
        }
        chain_done(ctx, st, idx);
        return;
    }
    let c = &mut st.chains[i];
    c.phase = 1;
    c.get_pid = pid;
    c.watch_fd = fd;
    c.stdout = Some(out);
    c.token = token;
}

/// Fired by `JOB_OSD_GET_OUT`: read the readback (until EOF, EAGAIN — the
/// source re-arms — or the retained-output cap).
pub fn on_get_out(ctx: ffi::Ctx, st: &mut OsdState, idx: u32) {
    let Ok(i) = usize::try_from(idx) else {
        return;
    };
    if i >= st.chains.len() || st.chains[i].phase != 1 {
        return;
    }
    let fd = st.chains[i].watch_fd;
    let mut done = false;
    loop {
        let mut buf = [0u8; 256];
        match ffi::read_fd(fd, &mut buf) {
            Some(0) => {
                done = true; // EOF: the child is done talking
                break;
            }
            Some(n) => {
                if st.chains[i].out.len() + n > MAX_READBACK {
                    // The retained-output cap is also the work cap. Closing
                    // now gives a noisy producer SIGPIPE instead of letting
                    // it keep this callback in its read loop.
                    chain_done(ctx, st, idx);
                    return; // no parseable readback: no card (don't guess)
                }
                st.chains[i].out.extend_from_slice(&buf[..n]);
            }
            None => break, // EAGAIN or error: more later (the source re-arms)
        }
    }
    if !done {
        return;
    }

    let text = String::from_utf8_lossy(&st.chains[i].out);
    let (generation, mic) = {
        let c = &st.chains[i];
        (c.generation, c.mic)
    };
    // no parseable readback (no default device, wpctl error): no card —
    // asserting "live"/a percent for a state that never changed lies
    let Some((value, muted)) = parse_readback(&text) else {
        chain_done(ctx, st, idx);
        return;
    };
    let pct = if value >= 1.0 {
        100
    } else {
        (value * 100.0).round() as i32
    };
    let newer = if mic {
        let n = generation > st.mic_feedback;
        if n {
            st.mic_feedback = generation;
        }
        n
    } else {
        let n = generation > st.volume_feedback;
        if n {
            st.volume_feedback = generation;
        }
        n
    };
    if newer && let Some(bus) = crate::bus::handle() {
        if mic {
            if muted || pct >= 0 {
                let (icon, body) = if muted {
                    ("microphone-sensitivity-muted", "muted")
                } else {
                    ("microphone-sensitivity-high", "live")
                };
                bus.send(BusCmd::NotifyCard(Card::osd(
                    9995,
                    icon,
                    "Microphone",
                    body.to_owned(),
                    -1,
                )));
            }
        } else if muted {
            bus.send(BusCmd::NotifyCard(Card::osd(
                9993,
                "audio-volume-muted",
                "Volume",
                "muted".to_owned(),
                -1,
            )));
        } else if pct >= 0 {
            bus.send(BusCmd::NotifyCard(Card::osd(
                9993,
                volume_icon(pct),
                "Volume",
                format!("{pct}%"),
                pct.min(100),
            )));
        }
    }
    chain_done(ctx, st, idx);
}

fn volume_icon(pct: i32) -> &'static str {
    if pct <= 33 {
        "audio-volume-low"
    } else if pct <= 66 {
        "audio-volume-medium"
    } else {
        "audio-volume-high"
    }
}

/// Remove a finished chain: the watch out, the fds closed (the phase-0
/// pidfd explicitly; the phase-1 stdout with the chain), any child that is
/// still running handed to the orphan list.
fn chain_done(ctx: ffi::Ctx, st: &mut OsdState, idx: u32) {
    let Ok(i) = usize::try_from(idx) else {
        return;
    };
    if i >= st.chains.len() {
        return;
    }
    let (token, phase, fd, set_pid, get_pid) = {
        let c = &st.chains[i];
        (c.token, c.phase, c.watch_fd, c.set_pid, c.get_pid)
    };
    if token != 0 {
        ffi::job_cancel(ctx, token);
    }
    if phase == 0 && fd >= 0 {
        ffi::close_fd(fd); // the pidfd (the phase-1 stdout closes below)
    }
    for pid in [set_pid, get_pid] {
        // a child that closed its stdout a hair before exiting may not be
        // reaped yet; hand it to the orphan list rather than leak it
        if pid > 0 && ffi::waitpid_nohang(pid) == 0 {
            remember_orphan(ctx, st, pid);
        }
    }
    st.chains.remove(i); // drops the stdout, closing the pipe read end
}

// `wpctl get-volume` readback: "Volume: <value> [MUTED]" (the fake and the
// real wpctl both lead with "Volume:"). Mirrors hyprosd/wpctl.hpp.
fn parse_readback(output: &str) -> Option<(f64, bool)> {
    let ws = |c: char| matches!(c, ' ' | '\t' | '\r' | '\n');
    let s = output.trim_start_matches(ws);
    let s = s.strip_prefix("Volume:")?;
    let s = s.trim_start_matches(ws);
    let token_end = s.find(|c| ws(c) || c == '[').unwrap_or(s.len());
    let mut token = s[..token_end].to_owned();
    if token.is_empty() {
        return None;
    }
    // a locale decimal comma ("0,56"): only when there is no dot and exactly
    // one comma
    if !token.contains('.') && token.matches(',').count() == 1 {
        token = token.replace(',', ".");
    }
    let number: f64 = token.parse().ok()?;
    if !number.is_finite() || number < 0.0 {
        return None;
    }
    let rest = s[token_end..].trim_start_matches(ws);
    if rest.is_empty() {
        return Some((number, false));
    }
    let rest = rest.strip_prefix("[MUTED]")?;
    if !rest.trim_start_matches(ws).is_empty() {
        return None;
    }
    Some((number, true))
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

pub fn init(st: &mut OsdState) {
    find_backlight(st);
}

/// Teardown: every chain watch out, every fd closed, the orphan tick
/// cancelled (`SA_NOCLDWAIT` means a late exit cannot zombie, so the orphan
/// list can be dropped).
pub fn exit(ctx: ffi::Ctx, st: &mut OsdState) {
    for i in 0..st.chains.len() {
        let (token, phase, fd) = {
            let c = &st.chains[i];
            (c.token, c.phase, c.watch_fd)
        };
        if token != 0 {
            ffi::job_cancel(ctx, token);
        }
        if phase == 0 && fd >= 0 {
            ffi::close_fd(fd);
        }
    }
    st.chains.clear(); // drops every stdout (pipe read end) with it
    if st.orphan_tick != 0 {
        ffi::job_cancel(ctx, st.orphan_tick);
        st.orphan_tick = 0;
    }
    st.orphans.clear();
    st.queued.clear();
    st.drain_armed = false;
    st.backlight_dev.clear();
    st.backlight_max = 0;
    st.last_set_raw = -1;
    st.brightness_gen = 0;
    st.volume_gen = 0;
    st.mic_gen = 0;
    st.volume_feedback = 0;
    st.mic_feedback = 0;
}
