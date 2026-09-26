//! focustrap — X11 focus-steal probe for the stress gate.
//!
//! The EWMH regression this guards: X11 clients cannot authenticate a user
//! gesture. Wine/Proton apps (the GOG installer, games) send
//! `_NET_ACTIVE_WINDOW` on every internal `SetForegroundWindow` and map
//! `FlashWindowEx` to `_NET_WM_STATE_DEMANDS_ATTENTION`. Under
//! `misc:focus_on_activate` the compositor used to take focus for each ping
//! — the XWM now maps both to urgency only. The probe maps a window, waits
//! (the gate moves keyboard focus to a Wayland window), then sends one ping
//! of the requested kind and holds, so the gate can assert the focus stayed
//! and the socket2 `urgent` event named this window.
//!
//! Rust port of the C fixture (awesome-rust plan, Track D).
//!
//! usage: `focustrap <map|attention|activate> [delay-s] [hold-s]`
//!   - `map`: map only, no ping (baseline: map-time focus is allowed)
//!   - `attention`: `_NET_WM_STATE` ADD `_NET_WM_STATE_DEMANDS_ATTENTION`
//!   - `activate`: `_NET_ACTIVE_WINDOW` (source = application)

use std::time::Duration;

use x11rb::CURRENT_TIME;
use x11rb::connection::Connection as _;
use x11rb::protocol::xproto::{
    AtomEnum, CLIENT_MESSAGE_EVENT, ClientMessageData, ClientMessageEvent, ConnectionExt,
    CreateWindowAux, EventMask, PropMode, WindowClass,
};

fn fail(code: u8, message: &str) -> ! {
    eprintln!("{message}");
    std::process::exit(i32::from(code));
}

fn ok_or<T, E: std::fmt::Display>(result: Result<T, E>, what: &str) -> T {
    match result {
        Ok(value) => value,
        Err(err) => fail(1, &format!("focustrap: {what}: {err}")),
    }
}

fn intern_atom(
    conn: &impl x11rb::connection::RequestConnection,
    name: &str,
) -> x11rb::protocol::xproto::Atom {
    let cookie = ok_or(
        conn.intern_atom(false, name.as_bytes()),
        &format!("intern {name}"),
    );
    let reply = ok_or(cookie.reply(), &format!("intern {name}"));
    reply.atom
}

fn u32_len(slice: &[u8]) -> u32 {
    u32::try_from(slice.len()).expect("property shorter than 4 GiB")
}

// XCreateSimpleWindow(parent=Root, 900x500 @ 90,60, border 1) + name/class
// hints, mapped. Returns the window id. XCreateSimpleWindow uses the screen's
// DEFAULT depth (24 on Xwayland, whose root window is 32-bit but advertised
// as 24) — a hardcoded 32 here is a Match error on the CreateWindow.
fn map_probe_window(conn: &impl x11rb::connection::Connection, root: u32, depth: u8) -> u32 {
    let win = ok_or(conn.generate_id(), "generate id");
    let cookie = ok_or(
        conn.create_window(
            depth,
            win,
            root,
            90,
            60,
            900,
            500,
            1,
            WindowClass::INPUT_OUTPUT,
            0,
            &CreateWindowAux {
                background_pixel: Some(0xff_b0d0ff),
                ..Default::default()
            },
        ),
        "create window",
    );
    ok_or(cookie.check(), "create window");
    // XStoreName + XSetClassHint (res_name\0res_class\0)
    let wm_name = b"focustrap";
    let wm_class = b"focustrap\0focustrap\0";
    let name_cookie = ok_or(
        conn.change_property(
            PropMode::REPLACE,
            win,
            AtomEnum::WM_NAME,
            AtomEnum::STRING,
            8,
            u32_len(wm_name),
            wm_name,
        ),
        "WM_NAME",
    );
    ok_or(name_cookie.check(), "WM_NAME");
    let class_cookie = ok_or(
        conn.change_property(
            PropMode::REPLACE,
            win,
            AtomEnum::WM_CLASS,
            AtomEnum::STRING,
            8,
            u32_len(wm_class),
            wm_class,
        ),
        "WM_CLASS",
    );
    ok_or(class_cookie.check(), "WM_CLASS");
    let map_cookie = ok_or(conn.map_window(win), "map window");
    ok_or(map_cookie.check(), "map window");
    win
}

// drain events like the C fixture's XPending loop (no more XPending in
// x11rb 0.14 — poll_for_event returning None is the "no event" case)
fn drain(conn: &impl x11rb::connection::Connection, seconds: u32) {
    for _ in 0..seconds * 20 {
        match conn.poll_for_event() {
            Ok(Some(_)) => {}
            Ok(None) => std::thread::sleep(Duration::from_millis(50)),
            Err(err) => fail(1, &format!("event: {err}")),
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let mode = args.next().unwrap_or_else(|| "attention".into());
    let delay: u32 = args.next().and_then(|arg| arg.parse().ok()).unwrap_or(6);
    let hold: u32 = args.next().and_then(|arg| arg.parse().ok()).unwrap_or(18);

    let (conn, screen_num) = ok_or(x11rb::connect(None), "no X display");
    let screen = &conn.setup().roots[screen_num];

    // intern the EWMH atoms
    let net_wm_state = intern_atom(&conn, "_NET_WM_STATE");
    let demands = intern_atom(&conn, "_NET_WM_STATE_DEMANDS_ATTENTION");
    let net_active = intern_atom(&conn, "_NET_ACTIVE_WINDOW");

    let win = map_probe_window(&conn, screen.root, screen.root_depth);
    let _ = conn.flush();
    eprintln!("focustrap: mapped 0x{win:x} pid {}", std::process::id());

    // wait for the MapNotify (the C fixture waited up to 5s)
    for _ in 0..100 {
        match conn.poll_for_event() {
            Ok(Some(x11rb::protocol::Event::MapNotify(event))) if event.window == win => break,
            Ok(_) => {}
            Err(err) => fail(1, &format!("event: {err}")),
        }
        std::thread::sleep(Duration::from_millis(50));
    }

    // wait (the gate moves keyboard focus to a Wayland window)
    drain(&conn, delay);

    if mode != "map" {
        // the target XWM reads the client field, as on a real EWMH ping
        let (message_type, data) = if mode == "activate" {
            (net_active, [2, CURRENT_TIME, 0, 0, 0]) // source: application
        } else {
            (net_wm_state, [1, demands, 0, CURRENT_TIME, 0]) // _NET_WM_STATE_ADD
        };
        let client = ClientMessageEvent {
            response_type: CLIENT_MESSAGE_EVENT,
            format: 32,
            sequence: 0,
            window: win,
            type_: message_type,
            data: ClientMessageData::from(data),
        };
        let send_cookie = ok_or(
            conn.send_event(
                false,
                screen.root,
                EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT,
                client,
            ),
            &format!("send {mode}"),
        );
        ok_or(send_cookie.check(), &format!("send {mode}"));
        let _ = conn.flush();
        eprintln!("focustrap: sent {mode}");
    }

    drain(&conn, hold);
}
