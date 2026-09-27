// The one D-Bus thread. zbus on a tokio current_thread runtime, living on
// its own std::thread — no bus call ever runs on the compositor's event
// loop (the plan's D-Bus line), and the bus thread never blocks the
// compositor (each command is one bounded async step).
//
// The event loop talks to it two ways:
//   - commands: a bounded crossbeam channel; `try_send` drops under
//     overload (cards and OSD steps are best-effort — the C++ links did
//     the same with their bounded post queues).
//   - replies: a shared queue + a non-blocking wake pipe. The bus thread
//     pushes a reply and writes one byte; the event loop's persistent
//     readable watch drains the queue. The queue is the source of truth;
//     the byte is only the nudge.
//
// No reconnect (parity with the C++ CBusLink: bus death turns the cards
// off, the keys keep working).

use std::collections::VecDeque;
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::sync::{Arc, Mutex};

use crossbeam_channel::{Sender, bounded};
use zbus::Connection;

// The command channel is bounded: a stuck event loop cannot fill memory,
// and a stuck bus thread only stalls its own queue (the compositor keeps
// running; sends drop at the bound).
const CMD_QUEUE: usize = 512;

/// One pre-formatted org.freedesktop.Notifications card.
#[derive(Clone, Debug)]
pub struct Card {
    pub app: &'static str,
    pub id: u32,
    pub icon: String,
    pub summary: String,
    pub body: String,
    pub urgency: u8,
    pub timeout_ms: i32,
    pub osd: bool,
    pub value: i32,
}

impl Card {
    /// An OSD-band card (ids 9991-9995): replaces in place, 1.2 s life.
    pub fn osd(id: u32, icon: &str, summary: &str, body: String, value: i32) -> Self {
        Self {
            app: "osd",
            id,
            icon: icon.to_owned(),
            summary: summary.to_owned(),
            body,
            urgency: 0,
            timeout_ms: 1200,
            osd: true,
            value,
        }
    }
}

/// A bus command (event loop -> bus thread).
pub enum BusCmd {
    /// Fire-and-forget Notify on the session bus.
    NotifyCard(Card),
    /// logind Session.SetBrightness on the system bus. On ack the bus
    /// thread sends `card` itself (only while `gen_id` is still the newest
    /// brightness request); on refusal it replies `BrightnessFailed` so the
    /// event loop drops its trust window.
    SetBrightness {
        dev: String,
        raw: u32,
        gen_id: u64,
        card: Card,
    },
}

/// A bus reply (bus thread -> event loop).
#[derive(Debug)]
pub enum BusReply {
    BrightnessFailed { gen_id: u64 },
}

// The singleton event-loop handle. It lives in a static rather than in the
// probe's State: State is leaked and cannot be mutated from teardown
// (`&State`), and dropping the last sender is what ends the thread.
static HANDLE: Mutex<Option<BusHandle>> = Mutex::new(None);

/// The event-loop handle (a cheap clone; None before start / after stop).
pub fn handle() -> Option<BusHandle> {
    HANDLE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .clone()
}

/// Teardown: drop the last sender — the thread exits, closing the write
/// end (the sources out on the event loop happen before this call).
pub fn stop() {
    *HANDLE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner) = None;
}

/// The event-loop side of the bus thread. Cheap to clone (channel sender +
/// shared queue); the thread itself is a singleton.
#[derive(Clone)]
pub struct BusHandle {
    tx: Sender<BusCmd>,
    q: Arc<Mutex<VecDeque<BusReply>>>,
}

impl BusHandle {
    /// Send a command; drops it if the channel is full (bounded
    /// backpressure) or the thread is gone.
    pub fn send(&self, cmd: BusCmd) {
        let _ = self.tx.try_send(cmd);
    }

    /// Take every pending reply (the `JOB_BUS_WAKE` drain).
    pub fn drain_replies(&self) -> Vec<BusReply> {
        self.q
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .drain(..)
            .collect()
    }
}

#[derive(Default)]
struct BusState {
    session: Option<Connection>,
    system: Option<Connection>,
    // the newest brightness generation this thread has seen; a reply for an
    // older generation must not flash a card (a fast repeat re-based the step)
    last_brightness_gen: u64,
}

fn nudge(w: &OwnedFd, q: &Arc<Mutex<VecDeque<BusReply>>>, r: BusReply) {
    q.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .push_back(r);
    // the pipe is non-blocking; a full buffer (a wedged event loop) loses
    // the nudge only — the reply stays queued and the next nudge drains it
    crate::ffi::write_nul(w.as_raw_fd());
}

async fn send_notify(conn: &Connection, c: &Card) -> Result<(), zbus::Error> {
    // The hints are a BTreeMap<String, Value> — zvariant's Dict is
    // Serialize-only (no DynamicType), so a std map is what the body bound
    // wants; it serializes as a{sv}, exactly the Notify spec.
    let mut hints: std::collections::BTreeMap<String, zbus::zvariant::Value> =
        std::collections::BTreeMap::new();
    hints.insert("urgency".to_owned(), zbus::zvariant::Value::from(c.urgency));
    if c.osd {
        hints.insert(
            "x-hyprnotify-osd".to_owned(),
            zbus::zvariant::Value::from(true),
        );
    }
    if c.value >= 0 {
        hints.insert("value".to_owned(), zbus::zvariant::Value::from(c.value));
    }
    conn.call_method(
        Some("org.freedesktop.Notifications"),
        "/org/freedesktop/Notifications",
        Some("org.freedesktop.Notifications"),
        "Notify",
        &(
            c.app,
            c.id,
            c.icon.clone(),
            c.summary.clone(),
            c.body.clone(),
            Vec::<String>::new(),
            &hints,
            c.timeout_ms,
        ),
    )
    .await?;
    Ok(())
}

async fn handle_cmd(
    st: &mut BusState,
    w: &OwnedFd,
    q: &Arc<Mutex<VecDeque<BusReply>>>,
    cmd: BusCmd,
) {
    match cmd {
        BusCmd::NotifyCard(card) => {
            if let Some(conn) = st.session.as_ref().filter(|c| !c.is_closed()) {
                let _ = send_notify(conn, &card).await;
            }
        }
        BusCmd::SetBrightness {
            dev,
            raw,
            gen_id,
            card,
        } => {
            if gen_id > st.last_brightness_gen {
                st.last_brightness_gen = gen_id;
            }
            let ok = match st.system.as_ref().filter(|c| !c.is_closed()) {
                Some(conn) => set_brightness(conn, &dev, raw).await.is_ok(),
                None => false,
            };
            if gen_id == st.last_brightness_gen {
                if ok {
                    // the card waits for logind's ack: a refused write must
                    // not flash a percent that never applied
                    if let Some(conn) = st.session.as_ref().filter(|c| !c.is_closed()) {
                        let _ = send_notify(conn, &card).await;
                    }
                } else {
                    nudge(w, q, BusReply::BrightnessFailed { gen_id });
                }
            }
        }
    }
}

async fn set_brightness(conn: &Connection, dev: &str, raw: u32) -> Result<(), zbus::Error> {
    conn.call_method(
        Some("org.freedesktop.login1"),
        "/org/freedesktop/login1/session/auto",
        Some("org.freedesktop.login1.Session"),
        "SetBrightness",
        &("backlight", dev, raw),
    )
    .await?;
    Ok(())
}

/// Start the bus thread. `write_fd` is the event loop's wake pipe (the
/// thread takes ownership; it is closed when the thread exits). Returns
/// false if the thread could not start (the commands then just drop — the
// keys keep working, the cards don't).
pub fn start(write_fd: RawFd) -> bool {
    let (tx, rx) = bounded::<BusCmd>(CMD_QUEUE);
    let q: Arc<Mutex<VecDeque<BusReply>>> = Arc::new(Mutex::new(VecDeque::new()));
    let q2 = q.clone();
    let handle = BusHandle { tx: tx.clone(), q };
    let started = std::thread::Builder::new()
        .name("awesome-bus".to_owned())
        .spawn(move || {
            // (the wake pipe's write end is owned by the thread from here)
            // the event loop handed the fd over; from here the thread is its
            // only owner (closed exactly once, with the thread)
            let w = crate::ffi::owned_fd(write_fd);
            let Some(rt) = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .ok()
            else {
                return;
            };
            let mut st = BusState::default();
            while let Ok(cmd) = rx.recv() {
                // a RecvError (the event loop dropped the sender = exit)
                // breaks the loop and the thread exits below
                rt.block_on(async {
                    // connect (once) on the first command: the connections'
                    // io loops live on this runtime and must be created in a
                    // runtime context. A missing bus leaves a None slot —
                    // sends no-op, brightness refuses (the keys still work).
                    // The 5 s method timeout bounds a daemon that accepts but
                    // never replies (the C++ links' sdbus default was 25 s).
                    if st.session.is_none()
                        && let Ok(b) = zbus::conn::Builder::session()
                    {
                        st.session = b
                            .method_timeout(std::time::Duration::from_secs(5))
                            .build()
                            .await
                            .ok();
                    }
                    if st.system.is_none()
                        && let Ok(b) = zbus::conn::Builder::system()
                    {
                        st.system = b
                            .method_timeout(std::time::Duration::from_secs(5))
                            .build()
                            .await
                            .ok();
                    }
                    handle_cmd(&mut st, &w, &q2, cmd).await;
                });
            }
        })
        .is_ok();
    if started {
        *HANDLE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(handle);
        true
    } else {
        false
    }
}
