// The one D-Bus thread. zbus on a tokio current_thread runtime, living on
// its own std::thread — no bus call ever runs on the compositor's event
// loop (the plan's D-Bus line), and the bus thread never blocks the
// compositor (each command is one bounded async step).
//
// The event loop talks to it three ways:
//   - commands: a bounded crossbeam channel; `try_send` drops under
//     overload (cards and OSD steps are best-effort — the C++ links did
//     the same with their bounded post queues).
//   - requests: the object-server methods (Notify, CloseNotification, …)
//     run on the bus thread but the MODEL runs on the event loop, so each
//     method forwards its payload here and awaits a reply the event loop
//     answers (a drop reads as a D-Bus error at the 5 s timeout).
//   - replies: a shared queue + a non-blocking wake pipe. The bus thread
//     pushes a reply and writes one byte; the event loop's persistent
//     readable watch drains the queue. The queue is the source of truth;
//     the byte is only the nudge.
//
// No reconnect (parity with the C++ CBusLink: bus death turns the cards
// off, the keys keep working).

use std::collections::{BTreeMap, VecDeque};
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender, bounded};
use zbus::fdo::DBusProxy;
use zbus::zvariant::Value;
use zbus::{Connection, Result as ZResult};

// The command channel is bounded: a stuck event loop cannot fill memory,
// and a stuck bus thread only stalls its own queue (the compositor keeps
// running; sends drop at the bound).
const CMD_QUEUE: usize = 512;

// the image-data hint's pixel buffer, hard-capped at de-serialization (a
// hostile sender can put megabytes in a hint; the unpack applies its own
// bound, this one is the wire bound)
const IMAGE_DATA_CAP: usize = 16 * 1024 * 1024;

// ---------------------------------------------------------------------------
// the Notify card (client side: the OSD's fire-and-forget)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// the hints (the Notify call's a{sv}, de-serialized on the bus thread)
// ---------------------------------------------------------------------------

/// The image-data hint, unpacked to raw pixels (the fd.o int16 array:
/// width, height, stride, has_alpha, bytes_per_pixel, channels, then the
/// buffer).
pub struct ImageData {
    pub w: u32,
    pub h: u32,
    pub stride: u32,
    pub has_alpha: bool,
    pub bps: u8,
    pub ch: u8,
    pub data: Vec<u8>,
}

/// One parsed hint value (the getters are type-strict, as the C++ variant
/// getters were).
enum HintValue {
    Bool(bool),
    U8(u8),
    I32(i32),
    U32(u32),
    I64(i64),
    Str(String),
    Other,
}

fn hint_from_owned(v: zbus::zvariant::OwnedValue) -> HintValue {
    if let Ok(b) = bool::try_from(&v) {
        return HintValue::Bool(b);
    }
    if let Ok(x) = u8::try_from(&v) {
        return HintValue::U8(x);
    }
    if let Ok(x) = i32::try_from(&v) {
        return HintValue::I32(x);
    }
    if let Ok(x) = u32::try_from(&v) {
        return HintValue::U32(x);
    }
    if let Ok(x) = i64::try_from(&v) {
        return HintValue::I64(x);
    }
    if let Ok(x) = String::try_from(v) {
        return HintValue::Str(x);
    }
    HintValue::Other
}

/// The a{sv} hint map, parsed on the bus thread so the event loop sees only
/// owned Rust values (a zbus `Value<'static>` argument cannot cross the
/// object-server's body-deserialization boundary).
pub struct HintMap {
    map: BTreeMap<String, HintValue>,
    image: Option<ImageData>,
}

impl Default for HintMap {
    fn default() -> Self {
        Self::new(BTreeMap::new())
    }
}

impl HintMap {
    pub fn new(map: BTreeMap<String, zbus::zvariant::OwnedValue>) -> Self {
        let image = map
            .get("image-data")
            .and_then(|v| v.try_clone().ok())
            .and_then(parse_image_data);
        let map = map
            .into_iter()
            .map(|(k, v)| (k, hint_from_owned(v)))
            .collect();
        Self { map, image }
    }

    /// A string hint, as the empty string when absent or mistyped (the C++
    /// variant getters' type-strict behavior).
    pub fn get_str(&self, k: &str) -> &str {
        match self.map.get(k) {
            Some(HintValue::Str(s)) => s.as_str(),
            _ => "",
        }
    }

    pub fn get_bool(&self, k: &str) -> Option<bool> {
        match self.map.get(k)? {
            HintValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn get_u8(&self, k: &str) -> Option<u8> {
        match self.map.get(k)? {
            HintValue::U8(v) => Some(*v),
            _ => None,
        }
    }

    pub fn get_i32(&self, k: &str) -> Option<i32> {
        match self.map.get(k)? {
            HintValue::I32(v) => Some(*v),
            _ => None,
        }
    }

    pub fn get_u32(&self, k: &str) -> Option<u32> {
        match self.map.get(k)? {
            HintValue::U32(v) => Some(*v),
            _ => None,
        }
    }

    pub fn get_i64(&self, k: &str) -> Option<i64> {
        match self.map.get(k)? {
            HintValue::I64(v) => Some(*v),
            _ => None,
        }
    }

    pub fn image_data(&self) -> Option<&ImageData> {
        self.image.as_ref()
    }
}

/// The fd.o image-data layout (int16 or int32 arrays); a short array or an
/// oversized buffer reads as absent.
fn parse_image_data(v: zbus::zvariant::OwnedValue) -> Option<ImageData> {
    let Ok(a) = zbus::zvariant::Array::<'static>::try_from(v) else {
        return None;
    };
    let items: Vec<i64> = a
        .iter()
        .filter_map(|it| match it {
            Value::I16(x) => Some(*x as i64),
            Value::I32(x) => Some(*x as i64),
            _ => None,
        })
        .collect();
    if items.len() < 6 {
        return None;
    }
    let w = items[0] as u32;
    let h = items[1] as u32;
    let stride = items[2] as u32;
    let has_alpha = items[3] != 0;
    let bps = items[4] as u8;
    let ch = items[5] as u8;
    let n = items.len() - 6;
    let want = (w as usize)
        .saturating_mul(h as usize)
        .saturating_mul(ch as usize)
        .saturating_mul((bps as usize).div_ceil(8));
    if n < want || want > IMAGE_DATA_CAP || n > IMAGE_DATA_CAP {
        return None;
    }
    let data: Vec<u8> = items[6..6 + want]
        .iter()
        .flat_map(|&x| {
            let b = x as u32;
            let sz = (bps as usize).div_ceil(8);
            (0..sz).map(move |i| ((b >> (8 * i)) & 0xff) as u8)
        })
        .collect();
    Some(ImageData {
        w,
        h,
        stride,
        has_alpha,
        bps,
        ch,
        data,
    })
}

// ---------------------------------------------------------------------------
// commands (event loop -> bus thread)
// ---------------------------------------------------------------------------

/// A bus command.
pub enum BusCmd {
    /// Fire-and-forget Notify on the session bus (the OSD cards).
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
    /// The NotificationClosed signal (the model asked to close `id`).
    EmitClosed { id: u32, reason: u32 },
    /// The ActivationToken + ActionInvoked pair (the token precedes the
    /// action, spec 1.3).
    EmitAction {
        id: u32,
        action: String,
        token: Option<String>,
    },
    /// The ActivationToken + NotificationReplied pair.
    EmitReplied {
        id: u32,
        text: String,
        token: Option<String>,
    },
    /// The shell-face State signal (the bar's bell).
    EmitState {
        live: u32,
        kept: u32,
        dnd: bool,
        center: bool,
    },
    /// Resolve the sender's pid (GetConnectionUnixProcessID) for the X11
    /// activation; the answer comes back as `PidResolved`. One request is
    /// in flight at a time (the reply channel serializes them): the pid reply
    /// correlates by position, not by id.
    ResolvePid { sender: String },
}

// ---------------------------------------------------------------------------
// replies (bus thread -> event loop)
// ---------------------------------------------------------------------------

/// A bus reply.
#[derive(Debug)]
pub enum BusReply {
    BrightnessFailed {
        gen_id: u64,
    },
    /// The sender's pid (the X11 activation's lookup landed).
    PidResolved {
        pid: u32,
    },
    /// The model closed `id` (the wake drain emits the NotificationClosed
    /// signal, carrying the model's reason code).
    Closed {
        id: u32,
        reason: u32,
    },
    /// The model's state changed (the wake drain re-queries the counts and
    /// emits the shell State signal for the bar's bell).
    StateChanged,
    /// A bare wake (the object-server handlers nudge the pipe before
    /// queueing a request so the event loop picks it up promptly).
    Nudge,
}

// ---------------------------------------------------------------------------
// requests (bus thread -> event loop): the model runs on the event loop
// ---------------------------------------------------------------------------

/// One object-server method that needs the model. The reply channel carries the
/// answer; a drop (the event loop died) reads as a D-Bus error at the
/// 5 s timeout. At most one request is in flight per connection, so the
/// Arrive payload's size gap costs one allocation, not a buffer.
#[allow(clippy::large_enum_variant)]
pub enum BusRequest {
    /// Notify: the whole card. Answers the id.
    Arrive {
        app: String,
        replaces: u32,
        icon: String,
        summary: String,
        body: String,
        actions: Vec<String>,
        hints: HintMap,
        expire: i32,
        sender: Option<String>,
        reply: crossbeam_channel::Sender<u32>,
    },
    /// CloseNotification. Answers whether the id was known (false is the
    /// spec's error, not a silent no-op).
    Close {
        id: u32,
        reply: crossbeam_channel::Sender<bool>,
    },
    /// The shell-face State query.
    State {
        reply: crossbeam_channel::Sender<(u32, u32, bool, bool)>,
    },
    /// The shell-face Toggle (no reply: the answer is the State signal).
    Toggle,
    /// The shell-face Peek (the bar's bell hover).
    Peek { on_bell: bool },
}

// ---------------------------------------------------------------------------
// the event-loop handle
// ---------------------------------------------------------------------------

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
// end (the sources out on the event loop happen before this call).
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
    /// the wake pipe's write end (the thread holds the other Arc; the fd
    /// closes when both are gone — at `stop` + the thread's exit)
    wake: Arc<OwnedFd>,
}

impl BusHandle {
    /// Send a command; drops it if the channel is full (bounded
    /// backpressure) or the thread is gone.
    pub fn send(&self, cmd: BusCmd) {
        let _ = self.tx.try_send(cmd);
    }

    /// Send a reply to the event loop (a pipe byte + the queue entry): the
    /// object-server handlers use it to wake the loop before queueing a
    /// request, so the answer does not wait for the next unrelated nudge.
    pub fn reply(&self, r: BusReply) {
        self.q
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .push_back(r);
        // the pipe is non-blocking; a full buffer (a wedged event loop) loses
        // the nudge only — the entry stays queued and the next nudge drains it
        crate::ffi::write_nul(self.wake.as_raw_fd());
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

/// What `start` hands back: the handle plus the request channel the event
/// loop drains (the `Receiver` is !Clone, so it lives in the probe's
/// State and is `take()`n at teardown).
pub struct BusStart {
    pub req_rx: Receiver<BusRequest>,
}

#[derive(Default)]
struct BusState {
    session: Option<Connection>,
    system: Option<Connection>,
    /// the session connection is the org.freedesktop.Notifications server
    /// (the name requested at build). When another daemon owns the name
    /// (dunst, or the C++ plugin during the coexistence phases) the build
    /// falls back to a client-only connection and this stays false.
    serving: bool,
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

// ---------------------------------------------------------------------------
// the object server: org.freedesktop.Notifications + the shell's face
// ---------------------------------------------------------------------------

/// The service state: one bounded channel to the event loop + the wake
/// handle (a cheap clone).
#[derive(Clone)]
struct NotifyService {
    req: Sender<BusRequest>,
    h: BusHandle,
}

#[zbus::interface(name = "org.freedesktop.Notifications")]
impl NotifyService {
    /// The whole arrival goes to the event loop (the model is there); the
    /// id comes back on the reply channel.
    #[allow(clippy::too_many_arguments)]
    async fn notify(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        app_name: &str,
        replaces_id: u32,
        app_icon: &str,
        summary: &str,
        body: &str,
        actions: Vec<String>,
        hints: BTreeMap<String, zbus::zvariant::OwnedValue>,
        expire_timeout: i32,
    ) -> zbus::fdo::Result<u32> {
        let (tx, rx) = crossbeam_channel::bounded(1);
        let sender = header
            .sender()
            .map(|s| s.as_ref().to_string())
            .filter(|s| !s.is_empty());
        if self
            .req
            .try_send(BusRequest::Arrive {
                app: app_name.to_owned(),
                replaces: replaces_id,
                icon: app_icon.to_owned(),
                summary: summary.to_owned(),
                body: body.to_owned(),
                actions,
                hints: HintMap::new(hints),
                expire: expire_timeout,
                sender,
                reply: tx,
            })
            .is_err()
        {
            return Err(zbus::fdo::Error::Failed(
                "the notification queue is full".to_owned(),
            ));
        }
        self.h.reply(BusReply::Nudge);
        roundtrip(&rx)
    }

    /// spec: an unknown ID is an error, not a silent no-op.
    async fn close_notification(&self, id: u32) -> zbus::fdo::Result<()> {
        let (tx, rx) = crossbeam_channel::bounded(1);
        if self
            .req
            .try_send(BusRequest::Close { id, reply: tx })
            .is_err()
        {
            return Err(zbus::fdo::Error::Failed(
                "the notification queue is full".to_owned(),
            ));
        }
        self.h.reply(BusReply::Nudge);
        match roundtrip(&rx) {
            Ok(true) => Ok(()),
            Ok(false) => Err(zbus::fdo::Error::Failed(
                "Unknown notification ID".to_owned(),
            )),
            Err(e) => Err(e),
        }
    }

    async fn get_capabilities(&self) -> zbus::fdo::Result<Vec<String>> {
        Ok(vec![
            "actions".into(),
            "action-icons".into(),
            "body".into(),
            "body-markup".into(),
            "body-hyperlinks".into(),
            "body-images".into(),
            "icon-static".into(),
            "inline-reply".into(),
            "persistence".into(),
            "sound".into(),
        ])
    }

    async fn get_server_information(&self) -> zbus::fdo::Result<(String, String, String, String)> {
        Ok((
            "awesome".into(),
            "hitori".into(),
            env!("CARGO_PKG_VERSION").into(),
            "1.3".into(),
        ))
    }
}

/// The shell's face on the same object (dunst does the same with
/// org.dunstproject.cmd0): the bar's bell reads State and calls Toggle over
/// the bus — the sanctioned cross-plugin channel, never symbols.
#[derive(Clone)]
struct ShellService {
    req: Sender<BusRequest>,
    h: BusHandle,
}

#[zbus::interface(name = "org.hitori.hyprnotify")]
impl ShellService {
    async fn toggle(&self) -> zbus::fdo::Result<()> {
        if self.req.try_send(BusRequest::Toggle).is_ok() {
            self.h.reply(BusReply::Nudge);
        }
        Ok(())
    }

    async fn peek(&self, on_bell: bool) -> zbus::fdo::Result<()> {
        if self.req.try_send(BusRequest::Peek { on_bell }).is_ok() {
            self.h.reply(BusReply::Nudge);
        }
        Ok(())
    }

    async fn state(&self) -> zbus::fdo::Result<(u32, u32, bool, bool)> {
        let (tx, rx) = crossbeam_channel::bounded(1);
        if self.req.try_send(BusRequest::State { reply: tx }).is_err() {
            return Err(zbus::fdo::Error::Failed(
                "the notification queue is full".to_owned(),
            ));
        }
        self.h.reply(BusReply::Nudge);
        roundtrip(&rx)
    }
}

/// The model round-trip: wait for the event loop's answer, bounded. The
/// zbus method handlers run on the connection's own executor thread —
/// outside any tokio runtime — so this is a crossbeam wait, never a
/// tokio future (tokio::time::timeout panics here and kills the executor,
/// taking the whole service down with it).
fn roundtrip<T>(rx: &crossbeam_channel::Receiver<T>) -> zbus::fdo::Result<T> {
    match rx.recv_timeout(Duration::from_secs(5)) {
        Ok(v) => Ok(v),
        Err(_) => Err(zbus::fdo::Error::Failed(
            "the notification service is busy".to_owned(),
        )),
    }
}

// ---------------------------------------------------------------------------
// the thread
// ---------------------------------------------------------------------------

async fn send_notify(conn: &Connection, c: &Card) -> ZResult<()> {
    // The hints are a BTreeMap<String, Value> — zvariant's Dict is
    // Serialize-only (no DynamicType), so a std map is what the body bound
    // wants; it serializes as a{sv}, exactly the Notify spec.
    let mut hints: BTreeMap<String, Value> = BTreeMap::new();
    hints.insert("urgency".to_owned(), Value::from(c.urgency));
    if c.osd {
        hints.insert("x-hyprnotify-osd".to_owned(), Value::from(true));
    }
    if c.value >= 0 {
        hints.insert("value".to_owned(), Value::from(c.value));
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

async fn emit<B: zbus::zvariant::Type + serde::Serialize>(
    st: &BusState,
    iface: &str,
    name: &str,
    args: &B,
) {
    let Some(conn) = st.session.as_ref().filter(|c| !c.is_closed()) else {
        return;
    };
    if !st.serving {
        return; // nobody is listening: another daemon owned the name
    }
    let _ = conn
        .emit_signal(
            None::<zbus::names::BusName<'_>>,
            "/org/freedesktop/Notifications",
            iface,
            name,
            args,
        )
        .await;
}

#[allow(clippy::too_many_lines)]
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
        BusCmd::EmitClosed { id, reason } => {
            emit(
                st,
                "org.freedesktop.Notifications",
                "NotificationClosed",
                &(id, reason),
            )
            .await;
        }
        BusCmd::EmitAction { id, action, token } => {
            // spec 1.3: the token precedes the action, so the sender's
            // xdg-activation request can actually raise it
            if let Some(t) = &token {
                emit(
                    st,
                    "org.freedesktop.Notifications",
                    "ActivationToken",
                    &(id, t.as_str()),
                )
                .await;
            }
            emit(
                st,
                "org.freedesktop.Notifications",
                "ActionInvoked",
                &(id, action.as_str()),
            )
            .await;
        }
        BusCmd::EmitReplied { id, text, token } => {
            if let Some(t) = &token {
                emit(
                    st,
                    "org.freedesktop.Notifications",
                    "ActivationToken",
                    &(id, t.as_str()),
                )
                .await;
            }
            emit(
                st,
                "org.freedesktop.Notifications",
                "NotificationReplied",
                &(id, text.as_str()),
            )
            .await;
        }
        BusCmd::EmitState {
            live,
            kept,
            dnd,
            center,
        } => {
            emit(
                st,
                "org.hitori.hyprnotify",
                "State",
                &(live, kept, dnd, center),
            )
            .await;
        }
        BusCmd::ResolvePid { sender } => {
            let Some(conn) = st.session.as_ref().filter(|c| !c.is_closed()) else {
                return;
            };
            let Ok(bus) = zbus::names::BusName::try_from(sender.as_str()) else {
                return;
            };
            let Ok(proxy) = DBusProxy::new(conn).await else {
                return;
            };
            // a unique name that vanished: the app is gone, nothing to focus
            if let Ok(pid) = proxy.get_connection_unix_process_id(bus).await {
                nudge(w, q, BusReply::PidResolved { pid });
            }
        }
    }
}

async fn set_brightness(conn: &Connection, dev: &str, raw: u32) -> ZResult<()> {
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

/// Build the session connection. The first attempt serves
/// org.freedesktop.Notifications + the shell face and requests the name;
/// when another daemon owns it (dunst, or the C++ plugin during the
/// coexistence phases) the name request fails the build and the fallback
/// connects client-only (the OSD keeps working, the server face is not ours).
async fn build_session(req: &Sender<BusRequest>, h: &BusHandle) -> (Option<Connection>, bool) {
    // attempt 1: the server
    let Ok(mut b) = zbus::conn::Builder::session() else {
        return (None, false);
    };
    b = b.method_timeout(Duration::from_secs(5));
    let svc = NotifyService {
        req: req.clone(),
        h: h.clone(),
    };
    let shell = ShellService {
        req: req.clone(),
        h: h.clone(),
    };
    let path = "/org/freedesktop/Notifications";

    if let Ok(b) = b.serve_at(path, svc)
        && let Ok(b) = b.serve_at(path, shell)
        && let Ok(b) = b.name("org.freedesktop.Notifications")
        && let Ok(c) = b.build().await
    {
        (Some(c), true)
    } else {
        // attempt 2: client-only
        let Ok(b) = zbus::conn::Builder::session() else {
            return (None, false);
        };
        match b.method_timeout(Duration::from_secs(5)).build().await {
            Ok(c) => (Some(c), false),
            Err(_) => (None, false),
        }
    }
}

/// Start the bus thread. `write_fd` is the event loop's wake pipe (the
/// thread takes ownership; it is closed when the thread exits). Returns
/// None if the thread could not start (the commands then just drop — the
/// keys keep working, the cards don't).
pub fn start(write_fd: RawFd) -> Option<BusStart> {
    let (tx, rx) = bounded::<BusCmd>(CMD_QUEUE);
    let (req_tx, req_rx) = bounded::<BusRequest>(CMD_QUEUE);
    let q: Arc<Mutex<VecDeque<BusReply>>> = Arc::new(Mutex::new(VecDeque::new()));
    let q2 = q.clone();
    // the wake pipe: the thread and the handle each hold an Arc; the fd
    // closes when both are gone (the handle at stop, the thread at its exit)
    let wake = Arc::new(crate::ffi::owned_fd(write_fd));
    let handle = BusHandle {
        tx: tx.clone(),
        q,
        wake: wake.clone(),
    };
    let started_handle = handle.clone();
    let started = std::thread::Builder::new()
        .name("awesome-bus".to_owned())
        .spawn(move || {
            let w = wake;
            let Some(rt) = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .ok()
            else {
                return;
            };
            let mut st = BusState::default();
            // the connection is built on the FIRST command: the io loops
            // must be created in a runtime context. A missing bus is not
            // fatal (the OSD degrades; the keys keep working).
            let (sess, serving) = rt.block_on(build_session(&req_tx, &started_handle));
            st.session = sess;
            st.serving = serving;
            if st.system.is_none()
                && let Ok(b) = zbus::conn::Builder::system()
            {
                let sys = rt
                    .block_on(b.method_timeout(Duration::from_secs(5)).build())
                    .ok();
                st.system = sys;
            }
            while let Ok(cmd) = rx.recv() {
                // a RecvError (the event loop dropped the sender = exit)
                // breaks the loop and the thread exits below
                rt.block_on(handle_cmd(&mut st, &w, &q2, cmd));
            }
        })
        .is_ok();
    if started {
        *HANDLE
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(handle.clone());
        Some(BusStart { req_rx })
    } else {
        None
    }
}
