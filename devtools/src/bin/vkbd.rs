//! vkbd — a `zwp_virtual_keyboard_v1` injector, vptr's twin for keys. Reads a
//! script on stdin so a whole chord lives in one process (the keyboard dies
//! with it), which is what keeps a stuck modifier from outliving the test.
//!
//!   stdin lines: tap KEY | press KEY | release KEY | mods MASK | sleep MS
//!   KEY = a name below, or a raw linux evdev code
//!   MASK = whitespace-separated subset of shift ctrl alt (gate extension:
//!   sets the virtual keyboard's modifier state so chords bind on the real
//!   config; the C fixture's plain `key` line never carried modifiers)
//!
//! Rust port of the C fixture (awesome-rust plan, Track D).

#[allow(clippy::wildcard_imports)]
pub mod vkbd_protocol {
    use wayland_client;
    use wayland_client::protocol::*;

    #[allow(unused_imports)]
    pub mod __interfaces {
        use wayland_client::protocol::__interfaces::*;
        wayland_scanner::generate_interfaces!("protocols/vkbd.xml");
    }
    #[allow(unused_imports)]
    use self::__interfaces::*;
    wayland_scanner::generate_client_code!("protocols/vkbd.xml");
}

use std::io::BufRead;
use std::os::fd::AsFd;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use rustix::fs::{MemfdFlags, memfd_create};
use rustix::io::write;
use xkbcommon::xkb::{
    CONTEXT_NO_FLAGS, KEYMAP_COMPILE_NO_FLAGS, KEYMAP_FORMAT_TEXT_V1, Keymap, MOD_INVALID,
};

use vkbd_protocol::zwp_virtual_keyboard_manager_v1::{
    Event as ZMgrEvent, ZwpVirtualKeyboardManagerV1,
};
use vkbd_protocol::zwp_virtual_keyboard_v1::{Event as ZKbEvent, ZwpVirtualKeyboardV1};
use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{
    wl_registry,
    wl_seat::{Event as WlSeatEvent, WlSeat},
};
use wayland_client::{Connection, Dispatch, QueueHandle};

fn fail(code: u8, message: &str) -> ! {
    eprintln!("{message}");
    std::process::exit(i32::from(code));
}

// millisecond timestamps with monotonic granularity (the compositor only
// orders by them; the wrap matches the C fixture's u32 wrap)
fn time_ms() -> u32 {
    static T0: OnceLock<Instant> = OnceLock::new();
    let elapsed = T0.get_or_init(Instant::now).elapsed().as_millis() & u128::from(u32::MAX);
    u32::try_from(elapsed).unwrap_or(u32::MAX)
}

// evdev codes: the protocol carries the same values wl_keyboard.key does, so
// no +8 here — the compositor adds it before xkb sees it
const KEYS: &[(&str, u32)] = &[
    ("esc", 1),
    ("backspace", 14),
    ("tab", 15),
    ("enter", 28),
    ("a", 30),
    ("b", 48),
    ("c", 46),
    ("d", 32),
    ("e", 18),
    ("f", 33),
    ("g", 34),
    ("h", 35),
    ("i", 23),
    ("j", 36),
    ("k", 37),
    ("l", 38),
    ("m", 50),
    ("n", 49),
    ("o", 24),
    ("p", 25),
    ("q", 16),
    ("r", 19),
    ("s", 31),
    ("t", 20),
    ("u", 22),
    ("v", 47),
    ("w", 17),
    ("x", 45),
    ("y", 21),
    ("z", 44),
    ("space", 57),
    ("home", 102),
    ("up", 103),
    ("left", 105),
    ("right", 106),
    ("end", 107),
    ("down", 108),
    ("delete", 111),
];

fn keycode(name: &str) -> Option<u32> {
    if let Some((_, code)) = KEYS.iter().find(|(n, _)| *n == name) {
        return Some(*code);
    }
    let raw: u64 = name.parse().ok()?;
    (1..=u64::from(u32::MAX))
        .contains(&raw)
        .then(|| u32::try_from(raw).unwrap_or(u32::MAX))
}

// the protocol demands a keymap before any key event; the default xkb rules
// are the same ones the compositor would compile for a real keyboard.
// Returns the memfd, its size, and the shift/ctrl/alt modifier masks.
fn keymap_fd() -> (std::os::fd::OwnedFd, u32, u32, u32, u32) {
    let ctx = xkbcommon::xkb::Context::new(CONTEXT_NO_FLAGS);
    let km = Keymap::new_from_names(&ctx, "", "", "", "", None, KEYMAP_COMPILE_NO_FLAGS)
        .unwrap_or_else(|| fail(3, "vkbd: no keymap"));
    let mods = |name: &str| {
        let index = km.mod_get_index(name);
        if index != MOD_INVALID && index < 32 {
            1u32 << index
        } else {
            0
        }
    };
    let (shift_mask, ctrl_mask, alt_mask) = (mods("Shift"), mods("Control"), mods("Mod1"));
    let mut bytes = km.get_as_string(KEYMAP_FORMAT_TEXT_V1).into_bytes();
    bytes.push(0);
    let size = u32::try_from(bytes.len()).expect("keymap shorter than 4 GiB");
    let fd = memfd_create("vkbd-keymap", MemfdFlags::CLOEXEC)
        .unwrap_or_else(|err| fail(3, &format!("vkbd: memfd: {err}")));
    write_all_fd(&fd, &bytes);
    (fd, size, shift_mask, ctrl_mask, alt_mask)
}

fn write_all_fd(fd: &impl AsFd, bytes: &[u8]) {
    let mut offset = 0;
    while offset < bytes.len() {
        let n = match write(fd, &bytes[offset..]) {
            Ok(n) => n,
            Err(err) => fail(3, &format!("vkbd: keymap write: {err}")),
        };
        offset += n;
    }
}

// queue state: vkbd holds its objects itself; no per-queue state needed
struct State;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _state: &mut State,
        _proxy: &wl_registry::WlRegistry,
        _event: wl_registry::Event,
        _data: &GlobalListContents,
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        // vkbd binds its globals at startup and creates the keyboard before
        // reading any key — the C fixture had the same effective requirement.
    }
}

impl Dispatch<WlSeat, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &WlSeat,
        _event: WlSeatEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ZwpVirtualKeyboardManagerV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwpVirtualKeyboardManagerV1,
        _event: ZMgrEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ZwpVirtualKeyboardV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwpVirtualKeyboardV1,
        _event: ZKbEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

fn main() {
    let Ok(conn) = Connection::connect_to_env() else {
        fail(1, "vkbd: no display");
    };
    let Ok((globals, mut queue)) = registry_queue_init::<State>(&conn) else {
        fail(1, "vkbd: no display");
    };
    let qh = queue.handle();

    let Ok(mgr) = globals.bind::<ZwpVirtualKeyboardManagerV1, State, _>(&qh, 1..=1, ()) else {
        fail(2, "vkbd: no zwp_virtual_keyboard_manager_v1");
    };
    let Ok(seat) = globals.bind::<WlSeat, State, _>(&qh, 1..=1, ()) else {
        fail(2, "vkbd: no wl_seat");
    };
    let kb = mgr.create_virtual_keyboard(&seat, &qh, ());

    let (fd, size, shift_mask, ctrl_mask, alt_mask) = keymap_fd();
    // WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
    kb.keymap(1, fd.as_fd(), size);
    // a defined state: no stuck modifier
    kb.modifiers(0, 0, 0, 0);
    let _ = queue.roundtrip(&mut State);

    let mut line = String::new();
    let stdin = std::io::stdin();
    loop {
        line.clear();
        match stdin.lock().read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {}
            Err(err) => {
                eprintln!("vkbd: stdin: {err}");
                break;
            }
        }
        let mut it = line.split_whitespace();
        let Some(cmd) = it.next() else { continue };
        let arg = it.next().unwrap_or("");
        match cmd {
            "sleep" => {
                let _ = queue.flush();
                let ms: u64 = arg.parse().unwrap_or(0);
                std::thread::sleep(Duration::from_millis(ms));
            }
            "mods" => {
                let depressed = (u32::from(arg.contains("shift")) * shift_mask)
                    | (u32::from(arg.contains("ctrl")) * ctrl_mask)
                    | (u32::from(arg.contains("alt")) * alt_mask);
                kb.modifiers(depressed, 0, 0, 0);
                let _ = queue.flush();
            }
            "press" | "release" | "tap" => {
                let Some(code) = keycode(arg) else {
                    fail(4, &format!("vkbd: unknown key: {arg}"));
                };
                let time = time_ms();
                match cmd {
                    "press" => kb.key(time, code, 1),
                    "release" => kb.key(time, code, 0),
                    "tap" => {
                        kb.key(time, code, 1);
                        let _ = queue.flush();
                        std::thread::sleep(Duration::from_millis(30));
                        kb.key(time_ms(), code, 0);
                    }
                    _ => unreachable!(),
                }
                let _ = queue.flush();
            }
            _ => {}
        }
    }

    let _ = queue.roundtrip(&mut State);
    let _ = conn.flush();
}
