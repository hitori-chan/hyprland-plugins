//! cliphold — nested-gate clipboard source that deliberately delays transfer.
//! It uses wlr-data-control so no surface or keyboard focus is needed.
//!
//!   `cliphold DELAY_MS TEXT`
//!
//! A negative delay holds the requested fd until the process or compositor
//! exits. READY is printed after the selection belongs to this client; SEND
//! is printed when a consumer requests it.
//!
//! Rust port of the C fixture (awesome-rust plan, Track D).

use std::os::fd::OwnedFd;
use std::time::{Duration, Instant};

use std::io::Write;

use rustix::event::Timespec;
use rustix::event::{PollFd, PollFlags, poll};
use rustix::io::write;
use wayland_client::backend::WaylandError;
use wayland_client::protocol::{wl_registry, wl_seat};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, event_created_child};
use wayland_protocols_wlr::data_control::v1::client::{
    zwlr_data_control_device_v1::{Event as DeviceEvent, ZwlrDataControlDeviceV1},
    zwlr_data_control_manager_v1::{Event as ManagerEvent, ZwlrDataControlManagerV1},
    zwlr_data_control_offer_v1::{Event as OfferEvent, ZwlrDataControlOfferV1},
    zwlr_data_control_source_v1::{Event as SourceEvent, ZwlrDataControlSourceV1},
};

// queue state: one data source, one pending transfer
struct State {
    manager: Option<ZwlrDataControlManagerV1>,
    seat: Option<wl_seat::WlSeat>,
    device: Option<ZwlrDataControlDeviceV1>,
    source: Option<ZwlrDataControlSourceV1>,
    delay_ms: i64,
    send_fd: Option<OwnedFd>,
    send_at: Option<Instant>,
    cancelled: bool,
}

impl State {
    fn delay(&self) -> Option<Duration> {
        (self.delay_ms >= 0)
            .then(|| Duration::from_millis(u64::try_from(self.delay_ms).unwrap_or(u64::MAX)))
    }

    const fn new(delay_ms: i64) -> Self {
        Self {
            manager: None,
            seat: None,
            device: None,
            source: None,
            delay_ms,
            send_fd: None,
            send_at: None,
            cancelled: false,
        }
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
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "zwlr_data_control_manager_v1" if state.manager.is_none() => {
                    state.manager = Some(registry.bind::<ZwlrDataControlManagerV1, _, _>(
                        name,
                        version.min(2),
                        qh,
                        (),
                    ));
                }
                "wl_seat" if state.seat.is_none() => {
                    state.seat =
                        Some(registry.bind::<wl_seat::WlSeat, _, _>(name, version.min(7), qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<ZwlrDataControlManagerV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwlrDataControlManagerV1,
        _event: ManagerEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<ZwlrDataControlDeviceV1, ()> for State {
    fn event(
        state: &mut State,
        _proxy: &ZwlrDataControlDeviceV1,
        event: DeviceEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match event {
            DeviceEvent::DataOffer { id: _offer } => {
                // an offer we never use; nothing to answer
            }
            // a foreign selection: C destroys the advertised offer and does
            // NOT cancel here — cancellation arrives via the source's
            // cancelled event (setting cancelled early changes the timing the
            // stale-transfer battery asserts on)
            DeviceEvent::Selection { id } | DeviceEvent::PrimarySelection { id } => {
                if let Some(offer) = id {
                    offer.destroy();
                }
            }
            DeviceEvent::Finished => state.cancelled = true,
            _ => {}
        }
    }
    // the data_offer event (opcode 0) creates a zwlr_data_control_offer_v1
    // child: without this specialization the queue panics on the first
    // foreign selection (the C fixture added an offer listener implicitly)
    event_created_child!(State, ZwlrDataControlDeviceV1, [
        0 => (ZwlrDataControlOfferV1, ()),
    ]);
}

impl Dispatch<ZwlrDataControlSourceV1, ()> for State {
    fn event(
        state: &mut State,
        _proxy: &ZwlrDataControlSourceV1,
        event: SourceEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
        match event {
            SourceEvent::Send { mime_type: _, fd } => {
                if let Some(old) = state.send_fd.replace(fd) {
                    drop(old);
                }
                // C: send_at = now + delay when delay >= 0, now + 0 otherwise
                // (the negative case never fires the write — see has_delay)
                state.send_at = Some(Instant::now() + (state.delay()).unwrap_or(Duration::ZERO));
                println!("SEND");
                let mut out = std::io::stdout();
                let _ = out.flush();
            }
            SourceEvent::Cancelled => state.cancelled = true,
            _ => {}
        }
    }
}

impl Dispatch<ZwlrDataControlOfferV1, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &ZwlrDataControlOfferV1,
        _event: OfferEvent,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for State {
    fn event(
        _state: &mut State,
        _proxy: &wl_seat::WlSeat,
        _event: wl_seat::Event,
        _data: &(),
        _conn: &Connection,
        _qh: &QueueHandle<Self>,
    ) {
    }
}

// full write of the payload into the transfer fd (EINTR retried, a dead
// consumer ends the write like the C fixture's SIGPIPE ignore)
fn write_payload(fd: &OwnedFd, payload: &[u8]) {
    let mut offset = 0;
    while offset < payload.len() {
        match write(fd, &payload[offset..]) {
            Ok(n) => offset += n,
            Err(rustix::io::Errno::INTR) => {}
            Err(_) => break,
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let Some(delay_arg) = args.next() else {
        eprintln!("usage: cliphold DELAY_MS TEXT");
        std::process::exit(2);
    };
    let delay_ms: i64 = delay_arg.parse().unwrap_or_else(|_| {
        eprintln!("usage: cliphold DELAY_MS TEXT");
        std::process::exit(2);
    });
    let Some(payload) = args.next() else {
        eprintln!("usage: cliphold DELAY_MS TEXT");
        std::process::exit(2);
    };

    let Ok(conn) = Connection::connect_to_env() else {
        std::process::exit(3);
    };
    let mut queue: EventQueue<State> = conn.new_event_queue();
    let qh = queue.handle();
    conn.display().get_registry(&qh, ());

    let mut state = State::new(delay_ms);
    // wait for the globals
    loop {
        if queue.blocking_dispatch(&mut state).is_err() {
            std::process::exit(4);
        }
        if state.manager.is_some() && state.seat.is_some() {
            break;
        }
    }
    let (Some(manager), Some(seat)) = (state.manager.clone(), state.seat.clone()) else {
        std::process::exit(4);
    };

    let device = manager.get_data_device(&seat, &qh, ());
    let source = manager.create_data_source(&qh, ());
    state.device = Some(device);
    source.offer("text/plain;charset=utf-8".to_string());
    state.device.as_ref().unwrap().set_selection(Some(&source));
    state.source = Some(source);
    if queue.roundtrip(&mut state).is_err() {
        std::process::exit(5);
    }
    println!("READY");
    let mut out = std::io::stdout();
    let _ = out.flush();

    run_transfer(&mut queue, &mut state, delay_ms >= 0, payload.as_bytes());
    teardown(&mut state);
    let _ = conn.flush();
}

// a negative delay holds the fd until the process or compositor exits
fn run_transfer(
    queue: &mut EventQueue<State>,
    state: &mut State,
    has_delay: bool,
    payload_bytes: &[u8],
) {
    while !state.cancelled {
        // the delayed write fires when its deadline passes
        if let (Some(fd), Some(due)) = (&state.send_fd, state.send_at)
            && has_delay
            && Instant::now() >= due
        {
            write_payload(fd, payload_bytes);
            break;
        }
        let timeout = state
            .send_at
            .filter(|_| has_delay)
            .map(|due| due.saturating_duration_since(Instant::now()));
        let Some(guard) = queue.prepare_read() else {
            let _ = queue.dispatch_pending(state);
            continue;
        };
        let timeout_ts = timeout.map(|t| Timespec {
            tv_sec: i64::try_from(t.as_secs()).unwrap_or(i64::MAX),
            tv_nsec: i64::from(t.subsec_nanos()),
        });
        let conn_fd = guard.connection_fd();
        let mut fds = [PollFd::new(
            &conn_fd,
            PollFlags::IN | PollFlags::HUP | PollFlags::ERR,
        )];
        #[allow(clippy::match_same_arms)]
        match poll(&mut fds, timeout_ts.as_ref()) {
            Ok(0) => {}
            Ok(_) => {
                let ready = fds[0].revents();
                if ready.intersects(PollFlags::HUP | PollFlags::ERR)
                    && !ready.intersects(PollFlags::IN)
                {
                    break;
                }
                if let Err(err) = guard.read() {
                    let would_block = matches!(&err, WaylandError::Io(io) if io.kind() == std::io::ErrorKind::WouldBlock);
                    if !would_block {
                        eprintln!("cliphold: read: {err}");
                        break;
                    }
                }
                let _ = queue.flush();
            }
            Err(rustix::io::Errno::INTR) => {}
            Err(err) => {
                eprintln!("cliphold: poll: {err}");
                break;
            }
        }
        if let Err(err) = queue.dispatch_pending(state) {
            eprintln!("cliphold: dispatch: {err}");
            break;
        }
    }
}

fn teardown(state: &mut State) {
    state.send_fd.take();
    let _ = state.source.take().map(|source| source.destroy());
    let _ = state.device.take().map(|device| device.destroy());
    let _ = state.manager.take().map(|manager| manager.destroy());
    // wl_seat has no destroy request; dropping the proxy suffices
    state.seat.take();
}
