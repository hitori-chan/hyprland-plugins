//! `fake-sni` — a `StatusNotifierItem` that serves a dbusmenu carrying the
//! separator defects real applets ship: nm-applet via appindicator appends a
//! separator after "Edit Connections…" (its About row only exists in the
//! `GtkStatusIcon` fallback) and `GTK` menus routinely emit a separator after
//! every hidden section. The menu here ends on a trailing separator, doubles
//! one mid-list, and repeats the trailing one on the submenu level, so the
//! gate can assert the bar's renderer draws none of them (height, pixels).
//!
//! `fake-sni` — register with the session bus's watcher, serve /menu
//!
//! The icon is a solid 22x22 magenta pixmap (SNI ARGB32, network byte order)
//! so the gate can locate the item in the bar band by color, theme-free.
//!
//! Layout (ids are stable across `GetLayout` calls):
//!   root   : 1 "one" · 2 "two" · 3 SEP · 4 SEP · 5 "three" · 6 "sub"▸ · 7 SEP
//!   sub (6): 8 "sub-one" · 9 "sub-two" · 10 SEP
//!
//! Client-contract notes (the bar speaks through `sdbus-c++`):
//! - dbusmenu spec >= 0.5 shape: `GetLayout` -> (u, (i, a{sv}, av)) where the
//!   children are an array of VARIANTS, each wrapping a 3-field
//!   (id, props, children) struct; the nested children array stays empty
//!   (the bar fetches submenu levels recursively).
//! - the bar's property getter (sdbus `getPropertyAsync` + `onInterface`)
//!   sends `Get` with TWO string args (interface name, property name) under
//!   the SNI interface name; only the second is meaningful. Unknown names
//!   answer with the string "" rather than an error (the C fixture's
//!   fallback).
//! - `Event`/`SendActionForId` answer with an empty reply: no reply would hang
//!   the host's async call. The gate never acts on a row.

use std::collections::HashMap;

use std::result::Result;
use zbus::fdo::Error as FdoError;
use zbus::interface;
use zbus::zvariant::{Array, ObjectPath, OwnedValue, Str, Value};

use zbus::Connection;

const SERVICE: &str = "org.freedesktop.HyprFakeSNI";
const WATCHER_SERVICE: &str = "org.kde.StatusNotifierWatcher";
const WATCHER_PATH: &str = "/StatusNotifierWatcher";
const SNI_PATH: &str = "/StatusNotifierItem";
const MENU_PATH: &str = "/menu";

const PXM: i32 = 22;

// one (w, h, ARGB32-bytes) pixmap element of the SNI IconPixmap arrays
#[derive(Clone, Value, OwnedValue)]
struct PixmapElem {
    w: i32,
    h: i32,
    data: Vec<u8>,
}

// the root of a GetLayout reply: (revision, (id, props, children))
type RootLayout = (u32, (i32, HashMap<String, OwnedValue>, Vec<OwnedValue>));

// one child of the children array: a (id, props, children) struct
#[derive(Clone, Value, OwnedValue)]
struct ChildRow {
    id: i32,
    props: HashMap<String, OwnedValue>,
    children: Vec<OwnedValue>,
}

fn s(v: &str) -> OwnedValue {
    OwnedValue::from(Str::from(v))
}

// one (iiay) pixmap; alpha 0 = "empty" slot
fn pixmap(alpha: u8) -> PixmapElem {
    let mut data = Vec::with_capacity(4 * PXM as usize * PXM as usize);
    for _ in 0..(PXM * PXM) {
        data.extend_from_slice(&[alpha, 0xFF, 0x00, 0xFF]);
    }
    PixmapElem {
        w: PXM,
        h: PXM,
        data,
    }
}

fn pixmap_list(empty: bool) -> OwnedValue {
    let a = if empty { 0u8 } else { 255u8 };
    let inner: Value = Value::from(pixmap(a));
    OwnedValue::try_from(Array::from(vec![inner])).expect("pixmap array is owned")
}

// the SNI object: /StatusNotifierItem
struct Sni;

#[interface(name = "org.kde.StatusNotifierItem")]
#[allow(clippy::unused_self, clippy::unnecessary_wraps)]
impl Sni {
    // zbus properties: the bar (sdbus-c++) reads them through the
    // org.freedesktop.DBus.Properties interface with its two-arg
    // Get(interface, property) shape, which zbus serves from these
    // declarations — a plain method named "Get" on the SNI interface is
    // never seen (the 2026-09-26 gate29: every Get answered
    // UnknownProperty and the bar's fetch chain aborted at Id).
    #[zbus(property)]
    fn category(&self) -> &'static str {
        "Applications"
    }

    #[zbus(property)]
    fn id(&self) -> &'static str {
        "fakesni"
    }

    #[zbus(property)]
    fn title(&self) -> &'static str {
        "Fake"
    }

    #[zbus(property)]
    fn status(&self) -> &'static str {
        "Active"
    }

    #[zbus(property)]
    fn item_is_menu(&self) -> bool {
        true
    }

    #[zbus(property)]
    fn menu(&self) -> ObjectPath<'static> {
        ObjectPath::try_from(MENU_PATH).expect("MENU_PATH is a valid object path")
    }

    #[zbus(property)]
    fn icon_name(&self) -> &'static str {
        ""
    }

    #[zbus(property)]
    fn attention_icon_name(&self) -> &'static str {
        ""
    }

    #[zbus(property)]
    fn overlay_icon_name(&self) -> &'static str {
        ""
    }

    #[zbus(property)]
    fn icon_theme_path(&self) -> &'static str {
        ""
    }

    #[zbus(property)]
    fn version(&self) -> u32 {
        3
    }

    // the pixmap elements are plain tuples (not PixmapElem): zbus property
    // getters need `Value: From<T>` on the return type, and the derive only
    // provides that for tuples, not for derived structs inside a Vec.
    #[zbus(property)]
    fn icon_pixmap(&self) -> Vec<(i32, i32, Vec<u8>)> {
        let p = pixmap(255);
        vec![(p.w, p.h, p.data)]
    }

    #[zbus(property)]
    fn attention_icon_pixmap(&self) -> Vec<(i32, i32, Vec<u8>)> {
        let p = pixmap(0);
        vec![(p.w, p.h, p.data)]
    }

    #[zbus(property)]
    fn overlay_icon_pixmap(&self) -> Vec<(i32, i32, Vec<u8>)> {
        let p = pixmap(0);
        vec![(p.w, p.h, p.data)]
    }

    // the two-arg sdbus shape; the interface name is ignored
    #[zbus(name = "Get")]
    fn get(&self, interface_name: &str, property_name: &str) -> Result<OwnedValue, FdoError> {
        let _ = interface_name; // the sdbus interface-name arg is not meaningful
        Ok(match property_name {
            "Category" => s("Applications"),
            "Id" => s("fakesni"),
            "Title" => s("Fake"),
            "Status" => s("Active"),
            "ItemIsMenu" => OwnedValue::from(true),
            "Menu" => menu_path_value(),
            "IconPixmap" => pixmap_list(false),
            "AttentionIconPixmap" | "OverlayIconPixmap" => pixmap_list(true),
            "Version" => OwnedValue::from(3u32),
            // the C fixture's fallback: an empty string, not an error
            _ => s(""),
        })
    }

    // note: the C fixture's GetAll does not carry Version
    #[zbus(name = "GetAll")]
    fn get_all(&self, interface_name: &str) -> Result<HashMap<String, OwnedValue>, FdoError> {
        let _ = interface_name; // the sdbus interface-name arg is not meaningful
        Ok(HashMap::from([
            ("Category".to_string(), s("Applications")),
            ("Id".to_string(), s("fakesni")),
            ("Title".to_string(), s("Fake")),
            ("Status".to_string(), s("Active")),
            ("IconName".to_string(), s("")),
            ("AttentionIconName".to_string(), s("")),
            ("OverlayIconName".to_string(), s("")),
            ("ItemIsMenu".to_string(), OwnedValue::from(true)),
            ("Menu".to_string(), menu_path_value()),
            ("IconPixmap".to_string(), pixmap_list(false)),
            ("AttentionIconPixmap".to_string(), pixmap_list(true)),
            ("OverlayIconPixmap".to_string(), pixmap_list(true)),
        ]))
    }

    // left/secondary/right clicks: no-op, but answer
    #[zbus(name = "Activate")]
    fn activate(&self, x: i32, y: i32) -> Result<(), FdoError> {
        let _ = (x, y); // the click position is not meaningful
        Ok(())
    }

    #[zbus(name = "SecondaryActivate")]
    fn secondary_activate(&self, x: i32, y: i32) -> Result<(), FdoError> {
        let _ = (x, y); // the click position is not meaningful
        Ok(())
    }

    #[zbus(name = "ContextMenu")]
    fn context_menu(&self, x: i32, y: i32) -> Result<(), FdoError> {
        let _ = (x, y); // the click position is not meaningful
        Ok(())
    }
}

fn menu_path_value() -> OwnedValue {
    OwnedValue::from(ObjectPath::try_from(MENU_PATH).expect("MENU_PATH is a valid object path"))
}

fn child(id: i32, label: Option<&str>) -> OwnedValue {
    let props: HashMap<String, OwnedValue> = match label {
        Some(label) => {
            let display = if label == "sub" { "submenu" } else { "nothing" };
            HashMap::from([
                ("type".to_string(), s("standard")),
                ("label".to_string(), s(label)),
                ("enabled".to_string(), OwnedValue::from(true)),
                ("visible".to_string(), OwnedValue::from(true)),
                ("children-display".to_string(), s(display)),
            ])
        }
        None => HashMap::from([("type".to_string(), s("separator"))]),
    };
    let row = ChildRow {
        id,
        props,
        children: Vec::new(),
    };
    // the vec-of-OwnedValue serializes each element as a variant already;
    // an extra Value::Value wrap would emit variant(variant(struct)) and
    // sdbus-c++'s Struct decode on the bar side would reject every row
    // (the 2026-09-26 gate29: the root panel collapsed to an empty level).
    OwnedValue::try_from(Value::from(row)).expect("child row is owned")
}

fn children(parent: i32) -> Vec<OwnedValue> {
    let rows: &[(i32, Option<&str>)] = if parent == 6 {
        &[(8, Some("sub-one")), (9, Some("sub-two")), (10, None)]
    } else {
        &[
            (1, Some("one")),
            (2, Some("two")),
            (3, None),
            (4, None),
            (5, Some("three")),
            (6, Some("sub")),
            (7, None),
        ]
    };
    rows.iter().map(|(id, label)| child(*id, *label)).collect()
}

// the menu object: /menu
struct Menu;

#[interface(name = "com.canonical.dbusmenu")]
#[allow(clippy::unused_self, clippy::unnecessary_wraps)]
impl Menu {
    #[zbus(name = "GetLayout")]
    fn get_layout(
        &self,
        parent: i32,
        recursion_depth: i32,
        property_names: Vec<String>,
    ) -> Result<RootLayout, FdoError> {
        let _ = (recursion_depth, property_names); // nested children stay empty; the bar recurses
        // (revision, (parent_id, empty props, children))
        Ok((1u32, (parent, HashMap::new(), children(parent))))
    }

    #[zbus(name = "AboutToShow")]
    fn about_to_show(&self, parent: i32) -> Result<bool, FdoError> {
        let _ = parent; // the bar loads either way
        Ok(true)
    }

    #[zbus(name = "Event")]
    fn event(
        &self,
        id: i32,
        event_type: String,
        data: OwnedValue,
        timestamp: u32,
    ) -> Result<(), FdoError> {
        let _ = (id, event_type, data, timestamp); // the gate never acts on a row
        Ok(())
    }

    #[zbus(name = "SendActionForId")]
    fn send_action_for_id(
        &self,
        id: i32,
        action: String,
        data: OwnedValue,
        timestamp: u32,
    ) -> Result<(), FdoError> {
        let _ = (id, action, data, timestamp); // the gate never acts on a row
        Ok(())
    }
}

// register with the watcher; the bar starts its tray watcher late, so this
// retries every 200 ms with a 1 s call deadline (like the C fixture)
async fn register_with_watcher(conn: &Connection) -> bool {
    let timeout = tokio::time::timeout(
        std::time::Duration::from_millis(1000),
        conn.call_method(
            Some(WATCHER_SERVICE),
            WATCHER_PATH,
            Some("org.kde.StatusNotifierWatcher"),
            "RegisterStatusNotifierItem",
            &(SERVICE,),
        ),
    );
    if matches!(timeout.await, Ok(Ok(_))) {
        true
    } else {
        eprintln!("fake-sni: no watcher on the bus yet");
        false
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), FdoError> {
    let conn = Connection::session()
        .await
        .map_err(|err| FdoError::Failed(format!("no session bus: {err}")))?;

    // default request flags already carry DoNotQueue
    conn.request_name(SERVICE)
        .await
        .map_err(|err| FdoError::Failed(format!("cannot own {SERVICE}: {err}")))?;

    let server = conn.object_server();
    server.at(SNI_PATH, Sni).await?;
    server.at(MENU_PATH, Menu).await?;

    // wait for the watcher (the bar's tray starts its watcher lazily)
    let mut interval = tokio::time::interval(std::time::Duration::from_millis(200));
    interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    loop {
        interval.tick().await;
        if register_with_watcher(&conn).await {
            break;
        }
    }

    // run until SIGTERM/SIGINT
    let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
        .map_err(|err| FdoError::Failed(format!("sigterm handler: {err}")))?;
    let mut intr = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::interrupt())
        .map_err(|err| FdoError::Failed(format!("sigint handler: {err}")))?;
    tokio::select! {
        _ = term.recv() => {}
        _ = intr.recv() => {}
    }
    Ok(())
}
