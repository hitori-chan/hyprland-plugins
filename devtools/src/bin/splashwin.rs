//! splashwin — the Discord-updater-splash shape for placement tests: a CSD
//! toplevel whose committed buffer is larger than the declared window
//! geometry (a shadow margin on all sides), min pinned to max in the
//! geometry frame.
//!
//! Usage: `splashwin W H [MARGIN] [APP_ID] [late] [parented] [resz]
//! [vismargin] [pinx] [parentonly] [pgeo]`
//! - W, H: content size = the declared window geometry, min == max
//!   (`resz`: min only)
//! - MARGIN: shadow inset; the buffer is (W + 2*M) x (H + 2*M)
//! - `late`: map first (buffer commit), THEN declare the size limits
//! - `parented`: also create a big resizable toplevel first and transient-for it
//! - `resz`: declare the min size only (a resizable CSD window)
//! - `vismargin`: paint the margin opaque light gray (visible over any background)
//! - `pinx`: pin the width only (max = W x 0): the per-axis pin shape
//! - `parentonly`: create only the parent toplevel (no child)
//! - `pgeo`: give the parent an explicit window geometry (0,0,PW,PH)

use std::os::fd::AsFd;

// TEMP use rustix::event::{PollFd, PollFlags, poll};
use rustix::fs::{MemfdFlags, memfd_create};
use rustix::io::write;
use wayland_client::protocol::wl_shm::Format;
use wayland_client::protocol::{
    wl_buffer::WlBuffer, wl_compositor::WlCompositor, wl_registry, wl_shm::WlShm, wl_shm_pool,
};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, delegate_noop};
use wayland_protocols::xdg::shell::client::{xdg_surface, xdg_toplevel, xdg_wm_base};
use xdg_surface::{Event as XdgSurfaceEvent, XdgSurface};
use xdg_toplevel::{Event as ToplevelEvent, XdgToplevel};
use xdg_wm_base::{Event as WmBaseEvent, XdgWmBase};

// the parent's paint color and the child's content/margin colors
const PARENT_COLOR: u32 = 0xff20_3040;
const CONTENT_COLOR: u32 = 0xff30_2020;
const VISIBLE_MARGIN_COLOR: u32 = 0xffb0_b0b0;
const TRANSPARENT: u32 = 0x0000_0000;

fn arg_i64(args: &[String], idx: usize, default: i64) -> i64 {
    args.get(idx)
        .and_then(|a| a.parse().ok())
        .unwrap_or(default)
}

fn flag(args: &[String], idx: usize, name: &str) -> bool {
    args.get(idx).is_some_and(|a| a == name)
}

fn c32(v: i64) -> i32 {
    i32::try_from(v).unwrap_or(i32::MAX)
}

fn fail(code: u8, msg: &str) -> ! {
    eprintln!("{msg}");
    std::process::exit(i32::from(code));
}

// the creation parameters for one fixture window
#[derive(Clone, Copy)]
struct WinSpec<'a> {
    wm: &'a XdgWmBase,
    compositor: &'a WlCompositor,
    shm: &'a WlShm,
    app_id: &'a str,
    width: i64,
    height: i64,
    pixel: &'a dyn Fn(i64, i64) -> u32,
}

// one toplevel: surface + xdg_surface + toplevel + its buffer. The memfd is
// held open for the process lifetime — closing it while the compositor is
// still mapping the buffer destroys the surface.
struct Win {
    surface: Option<wayland_client::protocol::wl_surface::WlSurface>,
    xdg: Option<XdgSurface>,
    toplevel: Option<XdgToplevel>,
    buffer: Option<WlBuffer>,
    _fd: Option<std::os::fd::OwnedFd>,
}

impl Win {
    fn new() -> Self {
        Self {
            surface: None,
            xdg: None,
            toplevel: None,
            buffer: None,
            _fd: None,
        }
    }

    fn create(spec: WinSpec<'_>, qh: &QueueHandle<State>) -> Win {
        let WinSpec {
            wm,
            compositor,
            shm,
            app_id,
            width,
            height,
            pixel,
        } = spec;
        let surface = compositor.create_surface(qh, ());
        let xsd = wm.get_xdg_surface(&surface, qh, ());
        let toplevel = xsd.get_toplevel(qh, ());
        toplevel.set_app_id(app_id.to_string());
        toplevel.set_title(app_id.to_string());

        let byte_count = u64::try_from(width)
            .unwrap_or(u64::MAX)
            .saturating_mul(u64::try_from(height).unwrap_or(u64::MAX))
            .saturating_mul(4);
        let count = usize::try_from(byte_count).unwrap_or(usize::MAX);
        let mut pixels = Vec::with_capacity(count);
        for y in 0..height {
            for x in 0..width {
                pixels.extend_from_slice(&pixel(x, y).to_le_bytes());
            }
        }
        let fd = memfd_create("splashwin", MemfdFlags::CLOEXEC)
            .unwrap_or_else(|err| fail(6, &format!("splashwin: memfd: {err}")));
        let mut offset = 0;
        while offset < pixels.len() {
            let n = write(fd.as_fd(), &pixels[offset..])
                .unwrap_or_else(|err| fail(6, &format!("splashwin: write: {err}")));
            offset += n;
        }
        let size = i32::try_from(pixels.len()).unwrap_or(i32::MAX);
        let pool = shm.create_pool(fd.as_fd(), size, qh, ());
        let buf = pool.create_buffer(
            0,
            i32::try_from(width).unwrap_or(i32::MAX),
            i32::try_from(height).unwrap_or(i32::MAX),
            i32::try_from(width * 4).unwrap_or(i32::MAX),
            Format::Argb8888,
            qh,
            (),
        );
        pool.destroy();
        // attach but do NOT commit: the caller orders the first commit
        // against set_window_geometry / set_parent / size limits, exactly as
        // the C fixture does (the child declares its CSD geometry BEFORE its
        // first frame; the parent maps first and declares geometry after)
        surface.attach(Some(&buf), 0, 0);
        Win {
            surface: Some(surface),
            xdg: Some(xsd),
            toplevel: Some(toplevel),
            buffer: Some(buf),
            _fd: Some(fd),
        }
    }

    fn commit(&self) {
        self.surface.as_ref().unwrap().commit();
    }

    fn teardown(&mut self) {
        if let Some(toplevel) = self.toplevel.take() {
            toplevel.destroy();
        }
        if let Some(xsd) = self.xdg.take() {
            xsd.destroy();
        }
        if let Some(buffer) = self.buffer.take() {
            buffer.destroy();
        }
        if let Some(surface) = self.surface.take() {
            surface.destroy();
        }
    }
}

struct State {
    compositor: Option<WlCompositor>,
    shm: Option<WlShm>,
    wm: Option<XdgWmBase>,
    parent: Win,
    child: Win,
    done: bool,
}

impl Dispatch<wl_registry::WlRegistry, ()> for State {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _data: &(),
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "wl_compositor" if state.compositor.is_none() => {
                    state.compositor =
                        Some(registry.bind::<WlCompositor, _, _>(name, version.min(4), qh, ()));
                }
                "wl_shm" if state.shm.is_none() => {
                    state.shm = Some(registry.bind::<WlShm, _, _>(name, version.min(1), qh, ()));
                }
                "xdg_wm_base" if state.wm.is_none() => {
                    state.wm = Some(registry.bind::<XdgWmBase, _, _>(name, version.min(1), qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<XdgWmBase, ()> for State {
    fn event(
        _state: &mut Self,
        wm: &XdgWmBase,
        event: WmBaseEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let WmBaseEvent::Ping { serial } = event {
            wm.pong(serial);
        }
    }
}

impl Dispatch<XdgSurface, ()> for State {
    fn event(
        _state: &mut Self,
        _xdg: &XdgSurface,
        _event: XdgSurfaceEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<XdgToplevel, ()> for State {
    fn event(
        state: &mut Self,
        _toplevel: &XdgToplevel,
        event: ToplevelEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        if let ToplevelEvent::Close = event {
            state.done = true;
        }
    }
}

delegate_noop!(State: ignore WlCompositor);
delegate_noop!(State: ignore WlShm);
delegate_noop!(State: ignore wl_shm_pool::WlShmPool);
delegate_noop!(State: ignore WlBuffer);
delegate_noop!(State: ignore wayland_client::protocol::wl_surface::WlSurface);

// the CLI shape, parsed once; the flag bools mirror the positional CLI
#[allow(clippy::struct_excessive_bools)]
struct Cli {
    content_w: i64,
    content_h: i64,
    margin: i64,
    app_id: String,
    late: bool,
    parented: bool,
    resz: bool,
    vis: bool,
    pinx: bool,
    parentonly: bool,
    pgeo: bool,
}

fn parse_cli() -> Cli {
    let args: Vec<String> = std::env::args().skip(1).collect();
    Cli {
        content_w: arg_i64(&args, 0, 300).max(1),
        content_h: arg_i64(&args, 1, 350).max(1),
        margin: arg_i64(&args, 2, 10).max(0),
        app_id: args
            .get(3)
            .cloned()
            .unwrap_or_else(|| "splashwin".to_string()),
        late: flag(&args, 4, "late"),
        parented: flag(&args, 5, "parented"),
        resz: flag(&args, 6, "resz"),
        vis: flag(&args, 7, "vismargin"),
        pinx: flag(&args, 8, "pinx"),
        parentonly: flag(&args, 9, "parentonly"),
        pgeo: flag(&args, 10, "pgeo"),
    }
}

fn main() {
    let cli = parse_cli();

    let Ok(conn) = Connection::connect_to_env() else {
        fail(1, "splashwin: no Wayland display");
    };
    let mut queue: EventQueue<State> = conn.new_event_queue();
    let mut state = State {
        compositor: None,
        shm: None,
        wm: None,
        parent: Win::new(),
        child: Win::new(),
        done: false,
    };
    build_windows(&conn, &mut queue, &mut state, &cli);
    run(&conn, &mut queue, &mut state);
}

fn build_windows(conn: &Connection, queue: &mut EventQueue<State>, state: &mut State, cli: &Cli) {
    let qh = queue.handle();
    let content_w = cli.content_w;
    let content_h = cli.content_h;
    let margin = cli.margin;
    let app_id = &cli.app_id;
    let late = cli.late;
    let parented = cli.parented;
    let resz = cli.resz;
    let vis = cli.vis;
    let pinx = cli.pinx;
    let parentonly = cli.parentonly;
    let pgeo = cli.pgeo;
    let _registry = conn.display().get_registry(&qh, ());
    if queue.roundtrip(state).is_err() {
        fail(8, "splashwin: registry roundtrip failed");
    }
    let (compositor, shm, wm) = match (&state.compositor, &state.shm, &state.wm) {
        (Some(c), Some(s), Some(wm)) => (c.clone(), s.clone(), wm.clone()),
        _ => fail(2, "splashwin: compositor/shm/xdg-wm-base unavailable"),
    };

    if parented {
        // the parent: big, resizable, class "discord" — covers the center
        state.parent = Win::create(
            WinSpec {
                wm: &wm,
                compositor: &compositor,
                shm: &shm,
                app_id: "discord",
                width: 1000,
                height: 600,
                pixel: &|_, _| PARENT_COLOR,
            },
            &qh,
        );
        state.parent.commit();
        if queue.roundtrip(state).is_err() {
            fail(9, "splashwin: parent roundtrip failed");
        }
        if pgeo {
            state
                .parent
                .xdg
                .as_ref()
                .unwrap()
                .set_window_geometry(0, 0, 1000, 600);
        }
    }

    if !parentonly {
        let child_pixel = |x: i64, y: i64| -> u32 {
            if x >= margin && x < margin + content_w && y >= margin && y < margin + content_h {
                CONTENT_COLOR
            } else if vis {
                VISIBLE_MARGIN_COLOR
            } else {
                TRANSPARENT
            }
        };
        state.child = Win::create(
            WinSpec {
                wm: &wm,
                compositor: &compositor,
                shm: &shm,
                app_id: app_id.as_str(),
                width: content_w + 2 * margin, // buffer = content + shadow margin
                height: content_h + 2 * margin,
                pixel: &child_pixel,
            },
            &qh,
        );
        if let Some(xsd) = state.child.xdg.as_ref() {
            // the CSD content frame: inset from the surface on all sides
            xsd.set_window_geometry(c32(margin), c32(margin), c32(content_w), c32(content_h));
        }
        if let Some(toplevel) = state.child.toplevel.as_ref() {
            if parented {
                toplevel.set_parent(Some(state.parent.toplevel.as_ref().unwrap()));
            }
            if !late {
                toplevel.set_min_size(c32(content_w), c32(content_h));
                if pinx {
                    toplevel.set_max_size(c32(content_w), 0); // width pinned, height free
                } else if !resz {
                    toplevel.set_max_size(c32(content_w), c32(content_h)); // min == max: the splash shape
                }
            }
        }
        // the child's first frame: AFTER the CSD geometry and size limits
        // (C order: geometry/limits/parent, then attach + commit)
        state.child.commit();
        if late {
            // the race shape: the limits land only after the first map commit
            let toplevel = state
                .child
                .toplevel
                .as_ref()
                .expect("child toplevel")
                .clone();
            if queue.roundtrip(state).is_err() {
                fail(10, "splashwin: late roundtrip failed");
            }
            toplevel.set_min_size(c32(content_w), c32(content_h));
            toplevel.set_max_size(c32(content_w), c32(content_h));
            state.child.surface.as_ref().unwrap().commit();
        }
    }
}

fn run(conn: &Connection, queue: &mut EventQueue<State>, state: &mut State) {
    // blocking dispatch: it flushes the creation burst (binds + pool + buffer
    // + attach + commit) that the setup enqueued after the registry roundtrip.
    // A custom prepare_read/poll loop must flush BETWEEN prepare_read and poll
    // (wl_display_prepare_read_queue does not flush); without it the requests
    // never reach the server and the loop deadlocks waiting for a first frame
    // the compositor can never produce.
    while !state.done {
        if let Err(err) = queue.blocking_dispatch(state) {
            eprintln!("splashwin: dispatch: {err}");
            break;
        }
    }
    state.child.teardown();
    state.parent.teardown();
    if let Some(wm) = state.wm.take() {
        wm.destroy();
    }
    state.shm = None;
    state.compositor = None;
    let _ = conn.flush();
}
