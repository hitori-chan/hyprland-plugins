//! fixwin — a fixed-size xdg-toplevel (min == max) for placement tests: the
//! dialog/splash shape (a Discord-updater-window stand-in).
//!
//! usage: fixwin <w> <h> [title]
//!
//! Rust port of the C fixture (awesome-rust plan, Track D).

use std::os::fd::AsFd;

use rustix::fs::{MemfdFlags, memfd_create};
use rustix::io::write;
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_registry, wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, QueueHandle, delegate_noop};
use wayland_protocols::xdg::shell::client::{
    xdg_surface::{Event as XdgSurfaceEvent, XdgSurface},
    xdg_toplevel::{Event as XdgToplevelEvent, XdgToplevel},
    xdg_wm_base::{Event as XdgWmBaseEvent, XdgWmBase},
};

// queue state: one fixture window, no reuse. The globals are collected first
// (C drains the whole registry before creating anything); the window is built
// and committed exactly once they are all in, so the xdg toplevel exists
// BEFORE the first buffer commit.
struct State {
    width: i32,
    height: i32,
    title: String,
    comp: Option<wl_compositor::WlCompositor>,
    shm: Option<wl_shm::WlShm>,
    wm: Option<XdgWmBase>,
    surf: Option<wl_surface::WlSurface>,
    buf: Option<wl_buffer::WlBuffer>,
    done: bool,
}

impl State {
    fn new(width: i32, height: i32, title: String) -> Self {
        Self {
            width,
            height,
            title,
            comp: None,
            shm: None,
            wm: None,
            surf: None,
            buf: None,
            done: false,
        }
    }

    fn ready(&self) -> bool {
        self.comp.is_some() && self.shm.is_some() && self.wm.is_some()
    }

    // C order: surface -> xdg surface -> toplevel (+ app id, title, min ==
    // max) -> pool/buffer -> attach -> commit. Never ack configure: the first
    // committed buffer is the single truth for this min == max window.
    fn map_window(&mut self, qh: &QueueHandle<State>) {
        let (Some(comp), Some(shm), Some(wm)) = (&self.comp, &self.shm, &self.wm) else {
            return;
        };
        let surf = comp.create_surface(qh, ());
        let xsd = wm.get_xdg_surface(&surf, qh, ());
        let top = xsd.get_toplevel(qh, ());
        top.set_app_id(self.title.clone());
        top.set_title(self.title.clone());
        top.set_min_size(self.width, self.height);
        top.set_max_size(self.width, self.height); // min == max: the dialog shape
        self.surf = Some(surf);
        let width = u32::try_from(self.width).unwrap_or(1);
        let height = u32::try_from(self.height).unwrap_or(1);
        let count = (width * height) as usize;
        // the fd lives with the process; one fixture window, no reuse. A flat
        // 0xff202030 ARGB8888 pixmap, stride = w*4.
        let pixel = 0xff_20_20_30u32.to_ne_bytes();
        let mut pixels = Vec::with_capacity(count * 4);
        for _ in 0..count {
            pixels.extend_from_slice(&pixel);
        }
        let fd = memfd_create("fixwin", MemfdFlags::empty()).unwrap_or_else(|err| {
            eprintln!("fixwin: memfd: {err}");
            std::process::exit(1);
        });
        let mut offset = 0;
        while offset < pixels.len() {
            let n = match write(&fd, &pixels[offset..]) {
                Ok(n) => n,
                Err(err) => {
                    eprintln!("fixwin: pixel write: {err}");
                    std::process::exit(1);
                }
            };
            offset += n;
        }
        let size = i32::try_from(pixels.len()).unwrap_or(i32::MAX);
        let pool = shm.create_pool(fd.as_fd(), size, qh, ());
        let buf = pool.create_buffer(
            0,
            self.width,
            self.height,
            self.width * 4,
            wl_shm::Format::Argb8888,
            qh,
            (),
        );
        pool.destroy();
        self.buf = Some(buf);
        let (Some(surf), Some(b)) = (self.surf.as_ref(), self.buf.as_ref()) else {
            return;
        };
        surf.attach(Some(b), 0, 0);
        surf.commit();
    }
}

impl Dispatch<wl_registry::WlRegistry, ()> for State {
    fn event(
        state: &mut State,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name, interface, ..
        } = event
        {
            match interface.as_str() {
                "wl_compositor" if state.comp.is_none() => {
                    state.comp =
                        Some(registry.bind::<wl_compositor::WlCompositor, _, _>(name, 4, qh, ()));
                }
                "wl_shm" if state.shm.is_none() => {
                    state.shm = Some(registry.bind::<wl_shm::WlShm, _, _>(name, 1, qh, ()));
                }
                "xdg_wm_base" if state.wm.is_none() => {
                    state.wm = Some(registry.bind::<XdgWmBase, _, _>(name, 1, qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<XdgWmBase, ()> for State {
    fn event(
        _state: &mut State,
        proxy: &XdgWmBase,
        event: XdgWmBaseEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let XdgWmBaseEvent::Ping { serial } = event {
            proxy.pong(serial);
        }
    }
}

// The C fixture never acks configure (no xdg_surface configure listener, no
// toplevel configure action): the first committed buffer is the single truth
// for this min == max window, and acking changes what the compositor's
// placement sees. The port must stay byte-identical in protocol behavior.
impl Dispatch<XdgSurface, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &XdgSurface,
        _event: XdgSurfaceEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<XdgToplevel, ()> for State {
    fn event(
        state: &mut State,
        _proxy: &XdgToplevel,
        event: XdgToplevelEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let XdgToplevelEvent::Close = event {
            state.done = true;
        }
    }
}

// no handlers for these: the fixture never reacts to them
delegate_noop!(State: ignore wl_compositor::WlCompositor);
delegate_noop!(State: ignore wl_shm::WlShm);
delegate_noop!(State: ignore wl_shm_pool::WlShmPool);
delegate_noop!(State: ignore wl_buffer::WlBuffer);
delegate_noop!(State: ignore wl_surface::WlSurface);

fn main() {
    let mut args = std::env::args().skip(1);
    let width: i32 = args
        .next()
        .and_then(|arg| arg.parse().ok())
        .unwrap_or(310)
        .clamp(1, i32::MAX);
    let height: i32 = args
        .next()
        .and_then(|arg| arg.parse().ok())
        .unwrap_or(360)
        .clamp(1, i32::MAX);
    let title = args.next().unwrap_or_else(|| "fixwin".into());

    let Ok(conn) = Connection::connect_to_env() else {
        std::process::exit(1);
    };
    let mut queue = conn.new_event_queue();
    let qh = queue.handle();
    conn.display().get_registry(&qh, ());

    let mut state = State::new(width, height, title);
    // C drains the registry (dispatch + roundtrip) before creating anything
    while !state.ready() {
        if queue.blocking_dispatch(&mut state).is_err() {
            std::process::exit(1);
        }
    }
    if queue.roundtrip(&mut state).is_err() {
        std::process::exit(1);
    }
    state.map_window(&qh);
    while !state.done {
        if let Err(err) = queue.blocking_dispatch(&mut state) {
            eprintln!("fixwin: dispatch: {err}");
            break;
        }
    }
    let _ = conn.flush();
}
