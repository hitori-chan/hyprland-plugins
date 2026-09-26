//! vptr — a wlr-virtual-pointer injector. Reads a gesture script on stdin so
//! a whole press/move/release lives in one process (the pointer dies with
//! it). Rust port of the C fixture (awesome-rust plan, Track D).
//!
//!   argv: W H   (extent = monitor size in px; default 1280 800)
//!   stdin lines: move X Y | rel DX DY | press BTN | release BTN | scroll AXIS VAL | sleep MS
//!   BTN = linux code (272 left, 273 right, 274 middle); AXIS 0=vert 1=horiz

use std::io::BufRead;
use std::process::ExitCode;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use wayland_client::globals::{GlobalListContents, registry_queue_init};
use wayland_client::protocol::{
    wl_pointer::{Axis, ButtonState},
    wl_registry,
    wl_seat::{Event as WlSeatEvent, WlSeat},
};
use wayland_client::{Connection, Dispatch, QueueHandle};
use wayland_protocols_wlr::virtual_pointer::v1::client::{
    zwlr_virtual_pointer_manager_v1::{Event as ZMgrEvent, ZwlrVirtualPointerManagerV1},
    zwlr_virtual_pointer_v1::{Event as ZPtrEvent, ZwlrVirtualPointerV1},
};

// millisecond timestamps with monotonic granularity (the compositor only
// orders by them; the wrap matches the C fixture's u32 wrap)
fn time_ms() -> u32 {
    static T0: OnceLock<Instant> = OnceLock::new();
    let elapsed = T0.get_or_init(Instant::now).elapsed().as_millis() & u128::from(u32::MAX);
    u32::try_from(elapsed).unwrap_or(u32::MAX)
}

// the C fixture truncated stdin values to the wire types; the gate only
// sends sane values, so clamp to the domain instead of truncating
fn to_u32(value: i64) -> u32 {
    u32::try_from(value.clamp(0, i64::from(u32::MAX))).unwrap_or(0)
}

fn to_f64(value: i64) -> f64 {
    let clamped = value.clamp(i64::from(i32::MIN), i64::from(i32::MAX));
    f64::from(i32::try_from(clamped).expect("clamped to i32 range"))
}

fn fail(code: u8, message: &str) -> ! {
    eprintln!("{message}");
    std::process::exit(i32::from(code));
}

// queue state: vptr holds its objects itself; no per-queue state needed
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
        // vptr binds its globals at startup and creates the pointer before
        // reading any gesture — the C fixture had the same effective
        // requirement (pointer created right after the first roundtrip),
        // so a seat or manager appearing later is not retrofitted.
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

impl Dispatch<ZwlrVirtualPointerManagerV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwlrVirtualPointerManagerV1,
        _event: ZMgrEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ZwlrVirtualPointerV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwlrVirtualPointerV1,
        _event: ZPtrEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

fn main() -> ExitCode {
    let mut args = std::env::args().skip(1);
    let ext_w: u32 = args.next().and_then(|arg| arg.parse().ok()).unwrap_or(1280);
    let ext_h: u32 = args.next().and_then(|arg| arg.parse().ok()).unwrap_or(800);

    let conn = match Connection::connect_to_env() {
        Ok(conn) => conn,
        Err(err) => fail(1, &format!("vptr: no display: {err}")),
    };
    let (globals, mut queue) = match registry_queue_init::<State>(&conn) {
        Ok(init) => init,
        Err(err) => fail(1, &format!("vptr: no display: {err}")),
    };
    let qh = queue.handle();

    let mgr: ZwlrVirtualPointerManagerV1 =
        match globals.bind::<ZwlrVirtualPointerManagerV1, State, _>(&qh, 1..=2, ()) {
            Ok(mgr) => mgr,
            Err(err) => fail(
                2,
                &format!("vptr: no zwlr_virtual_pointer_manager_v1: {err}"),
            ),
        };
    let seat: WlSeat = match globals.bind::<WlSeat, State, _>(&qh, 1..=1, ()) {
        Ok(seat) => seat,
        Err(err) => fail(2, &format!("vptr: no wl_seat: {err}")),
    };
    let pointer = mgr.create_virtual_pointer(Some(&seat), &qh, ());
    let _ = queue.roundtrip(&mut State);

    let mut line = String::new();
    let stdin = std::io::stdin();
    loop {
        line.clear();
        match stdin.lock().read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {}
            Err(err) => {
                eprintln!("vptr: stdin: {err}");
                break;
            }
        }
        let mut it = line.split_whitespace();
        let Some(cmd) = it.next() else { continue };
        let v1: i64 = it.next().and_then(|arg| arg.parse().ok()).unwrap_or(0);
        let v2: i64 = it.next().and_then(|arg| arg.parse().ok()).unwrap_or(0);
        let time = time_ms();
        match cmd {
            "move" => {
                pointer.motion_absolute(time, to_u32(v1), to_u32(v2), ext_w, ext_h);
                pointer.frame();
            }
            "rel" => {
                pointer.motion(time, to_f64(v1), to_f64(v2));
                pointer.frame();
            }
            "press" => {
                pointer.button(time, to_u32(v1), ButtonState::Pressed);
                pointer.frame();
            }
            "release" => {
                pointer.button(time, to_u32(v1), ButtonState::Released);
                pointer.frame();
            }
            "scroll" => {
                let axis = if v1 == 0 {
                    Axis::VerticalScroll
                } else {
                    Axis::HorizontalScroll
                };
                pointer.axis(time, axis, to_f64(v2));
                pointer.frame();
            }
            "sleep" => {
                let _ = queue.flush();
                std::thread::sleep(Duration::from_millis(u64::from(to_u32(v1))));
                continue;
            }
            _ => continue,
        }
        let _ = queue.flush();
    }

    let _ = queue.roundtrip(&mut State);
    let _ = conn.flush();
    ExitCode::SUCCESS
}
