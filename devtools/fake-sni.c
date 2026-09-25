// fake-sni.c — a StatusNotifierItem that serves a dbusmenu carrying the
// separator defects real applets ship: nm-applet via appindicator appends a
// separator after "Edit Connections…" (its About row only exists in the
// GtkStatusIcon fallback) and GTK menus routinely emit a separator after
// every hidden section. The menu here ends on a trailing separator, doubles
// one mid-list, and repeats the trailing one on the submenu level, so the
// gate can assert the bar's renderer draws none of them (height, pixels).
//
//   fake-sni            — register with the session bus's watcher, serve /menu
//
// The icon is a solid 22x22 magenta pixmap (SNI ARGB32, network byte order)
// so the gate can locate the item in the bar band by color, theme-free.
//
// Layout (ids are stable across GetLayout calls):
//   root   : 1 "one" · 2 "two" · 3 SEP · 4 SEP · 5 "three" · 6 "sub"▸ · 7 SEP
//   sub (6): 8 "sub-one" · 9 "sub-two" · 10 SEP
//
// libdbus writer notes (cost hours, do not "simplify" back):
//  - a plain struct opened with open_container takes contained_signature
//    NULL — a non-NULL signature there is a builder assertion. So are
//    a{sv} entries: DBUS_TYPE_DICT_ENTRY with a NULL signature (the older
//    STRUCT+"{sv}" idiom aborts the builder in nested contexts).
//  - a VARIANT writes only the 'v' marker: the content must be written by
//    hand, including its own array header. A variant holding an array
//    therefore takes the content signature WITH the array marker ("a(iiay)")
//    and a real DBUS_TYPE_ARRAY container (element signature "(iiay)")
//    opened inside it; the (iiay) structs (contained_signature NULL) go in
//    that array iterator. Writing the struct straight into the variant
//    iterator instead (content signature "(iiay)") silently emits a single
//    struct where an array is expected — sdbus-c++'s vector decode then
//    throws and hyprbar drops the icon. A variant holding a plain struct
//    (GetLayout children) takes "(ia{sv}av)" and the struct IS written
//    directly into the variant iterator. ("(ay)" is a STRUCT with one
//    array field, not an array — parentheses matter.)
//  - dbus_message_iter_append_fixed_array(BYTE, ...) SEGVs on this system
//    (dbus 1.16.2 + glibc AVX512 memmove; same crash path as the 2017
//    digiKam report) at every size. Bytes are appended one by one with
//    append_basic instead — slow, but this is a fixture, not a hot path.
//  - SNI IconPixmap elements are (int, int, byte-array): signature "iiay".
//  - the main loop uses the canonical vtable + read_write_dispatch pattern;
//    read_write + pop_message without dispatch is not a supported manual
//    mode and mangles popped messages.
//  - the connection from dbus_bus_get is shared: releasing it with
//    dbus_connection_close() is a libdbus fatal ("must not close shared
//    connections" -> SIGABRT at every teardown); dbus_connection_unref is
//    the documented release.

#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *SERVICE = "org.freedesktop.HyprFakeSNI";

static DBusConnection *bus = nullptr;
static int             registered = 0;
static volatile sig_atomic_t stop = 0;

static void on_signal(int) {
	stop = 1;
}

// ---- dbusmenu layout ------------------------------------------------------

struct Row {
	int32_t    id;
	const char *label; // NULL = separator
	int        submenu;
};

static const struct Row ROOT[] = {
	{1, "one", 0}, {2, "two", 0}, {3, NULL, 0}, {4, NULL, 0}, {5, "three", 0}, {6, "sub", 1}, {7, NULL, 0},
};
static const struct Row SUB[] = {
	{8, "sub-one", 0}, {9, "sub-two", 0}, {10, NULL, 0},
};

// {key: val} entry in an a{sv}. Must be DBUS_TYPE_DICT_ENTRY with a NULL
// signature: STRUCT+"{sv}" aborts the builder in nested contexts (the
// open_container assertion), even though it is the older documented idiom.
static void kv(DBusMessageIter *map, const char *key, int variantType, void **val) {
	DBusMessageIter ent, v;
	const char *    sig = variantType == DBUS_TYPE_STRING ? "s" : variantType == DBUS_TYPE_BOOLEAN ? "b" : "o";
	dbus_message_iter_open_container(map, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
	dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, sig, &v);
	dbus_message_iter_append_basic(&v, variantType, val);
	dbus_message_iter_close_container(&ent, &v);
	dbus_message_iter_close_container(map, &ent);
}

// one child of the children array. hyprbar (like nm-applet and every other
// modern dbusmenu host) speaks spec >= 0.5: GetLayout(i, i, as) ->
// u(ia{sv}av) where the children are an array of VARIANTS, each wrapping a
// 3-field (id, props, children) struct — NOT the legacy bare-struct
// (iasvav) shape. The nested children array stays empty: the bar fetches
// submenu levels with recursive GetLayout calls.
static void append_row(DBusMessageIter *ch, const struct Row *r) {
	DBusMessageIter vv, item, props, kids;
	dbus_message_iter_open_container(ch, DBUS_TYPE_VARIANT, "(ia{sv}av)", &vv);
	dbus_message_iter_open_container(&vv, DBUS_TYPE_STRUCT, NULL, &item);
	dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &r->id);
	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &props);
	if (r->label) {
		const int    b  = 1;
		const char *ty = "standard";
		const char *cd = r->submenu ? "submenu" : "nothing";
		kv(&props, "type", DBUS_TYPE_STRING, (void **)&ty);
		kv(&props, "label", DBUS_TYPE_STRING, (void **)&r->label);
		kv(&props, "enabled", DBUS_TYPE_BOOLEAN, (void **)&b);
		kv(&props, "visible", DBUS_TYPE_BOOLEAN, (void **)&b);
		kv(&props, "children-display", DBUS_TYPE_STRING, (void **)&cd);
	} else {
		const char *sep = "separator";
		kv(&props, "type", DBUS_TYPE_STRING, (void **)&sep);
	}
	dbus_message_iter_close_container(&item, &props);
	dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "v", &kids);
	dbus_message_iter_close_container(&item, &kids);
	dbus_message_iter_close_container(&vv, &item);
	dbus_message_iter_close_container(ch, &vv);
}

static void reply_layout(DBusConnection *c, DBusMessage *m, int32_t parent) {
	DBusMessage *r = dbus_message_new_method_return(m);
	if (!r)
		return;
	DBusMessageIter it, root, props, kids;
	dbus_message_iter_init_append(r, &it);
	uint32_t rev = 1;
	dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &rev);
	dbus_message_iter_open_container(&it, DBUS_TYPE_STRUCT, NULL, &root);
	dbus_message_iter_append_basic(&root, DBUS_TYPE_INT32, &parent);
	dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &props); // empty props
	dbus_message_iter_close_container(&root, &props);
	dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "v", &kids);
	const struct Row *rows = parent == 6 ? SUB : ROOT;
	const size_t      n    = parent == 6 ? (sizeof(SUB) / sizeof(*SUB)) : (sizeof(ROOT) / sizeof(*ROOT));
	for (size_t i = 0; i < n; i++)
		append_row(&kids, &rows[i]);
	dbus_message_iter_close_container(&root, &kids);
	dbus_message_iter_close_container(&it, &root);
	dbus_connection_send(c, r, nullptr);
	dbus_message_unref(r);
}

static void pixmap_value(DBusMessageIter *it, int empty);

static DBusHandlerResult handle_menu(DBusConnection *c, DBusMessage *m, void *) {
	const char *member = dbus_message_get_member(m);
	if (strcmp(member, "GetLayout") == 0) {
		DBusMessageIter it;
		int32_t         parent = 0;
		if (dbus_message_iter_init(m, &it))
			dbus_message_iter_get_basic(&it, &parent);
		reply_layout(c, m, parent);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(member, "AboutToShow") == 0) {
		DBusMessage *r = dbus_message_new_method_return(m);
		DBusMessageIter it;
		dbus_message_iter_init_append(r, &it);
		int b = 1;
		dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &b);
		dbus_connection_send(c, r, nullptr);
		dbus_message_unref(r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(member, "Event") == 0 || strcmp(member, "SendActionForId") == 0) {
		DBusMessage *r = dbus_message_new_method_return(m); // no reply would hang the host's async call
		if (r)
			dbus_connection_send(c, r, nullptr);
		dbus_message_unref(r);
		return DBUS_HANDLER_RESULT_HANDLED; // the gate never acts on a row
	}
	// Properties.Get on the menu object: dbusmenu declares no properties —
	// the SNI properties live on /StatusNotifierItem (handle_sni). Answer
	// UnknownProperty rather than invent values.
	if (strcmp(member, "Get") == 0) {
		DBusMessage *err = dbus_message_new_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "no properties on the menu object");
		if (err)
			dbus_connection_send(c, err, nullptr);
		dbus_message_unref(err);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ---- SNI object ------------------------------------------------------------

// One (iiay) pixmap element into `arr` (an array-position iterator).
// `alpha` 0 = fully transparent ("empty" slot), 255 = solid magenta.
// Bytes go in one by one: append_fixed_array(BYTE) is broken here (see
// the header notes).
static void pixmap_element(DBusMessageIter *arr, int alpha) {
	int32_t w = 22, h = 22;
	uint8_t px[22 * 22 * 4];
	for (size_t i = 0; i < 22 * 22; i++) {
		px[i * 4]     = (uint8_t)alpha;
		px[i * 4 + 1] = 0xFF;
		px[i * 4 + 2] = 0x00;
		px[i * 4 + 3] = 0xFF;
	}
	DBusMessageIter st, ay;
	dbus_message_iter_open_container(arr, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &w);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &h);
	dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "y", &ay);
	for (size_t i = 0; i < sizeof(px); i++)
		dbus_message_iter_append_basic(&ay, DBUS_TYPE_BYTE, &px[i]);
	dbus_message_iter_close_container(&st, &ay);
	dbus_message_iter_close_container(arr, &st);
}

// GetAll map entry: a{sv} key with a (iiay) pixmap variant (DICT_ENTRY,
// like kv() — see its note).
static void pixmap_prop(DBusMessageIter *map, const char *key, int empty) {
	DBusMessageIter ent, v, arr;
	dbus_message_iter_open_container(map, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
	dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "a(iiay)", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(iiay)", &arr);
	pixmap_element(&arr, empty ? 0 : 255);
	dbus_message_iter_close_container(&v, &arr);
	dbus_message_iter_close_container(&ent, &v);
	dbus_message_iter_close_container(map, &ent);
}

// Get reply: a (v) whose content is the a(iiay) pixmap array — the variant
// writes only its marker, so the array container is opened inside it.
static void pixmap_value(DBusMessageIter *it, int empty) {
	DBusMessageIter v, arr;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a(iiay)", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "(iiay)", &arr);
	pixmap_element(&arr, empty ? 0 : 255);
	dbus_message_iter_close_container(&v, &arr);
	dbus_message_iter_close_container(it, &v);
}

static DBusHandlerResult handle_sni(DBusConnection *c, DBusMessage *m, void *) {
	const char *member = dbus_message_get_member(m);
	if (strcmp(member, "Get") != 0 && strcmp(member, "GetAll") != 0) {
		if (strcmp(member, "Activate") == 0 || strcmp(member, "SecondaryActivate") == 0 || strcmp(member, "ContextMenu") == 0)
			return DBUS_HANDLER_RESULT_HANDLED; // no-op
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	DBusMessage *r = dbus_message_new_method_return(m);
	if (!r)
		return DBUS_HANDLER_RESULT_HANDLED;
	DBusMessageIter it;
	dbus_message_iter_init_append(r, &it);

	if (strcmp(member, "GetAll") == 0) {
		// (a{sv})
		DBusMessageIter map;
		dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &map);
		{
			const char *v = "Applications";
			kv(&map, "Category", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "fakesni";
			kv(&map, "Id", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "Fake";
			kv(&map, "Title", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "Active";
			kv(&map, "Status", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "";
			kv(&map, "IconName", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "";
			kv(&map, "AttentionIconName", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const char *v = "";
			kv(&map, "OverlayIconName", DBUS_TYPE_STRING, (void **)&v);
		}
		{
			const int b = 1;
			kv(&map, "ItemIsMenu", DBUS_TYPE_BOOLEAN, (void **)&b);
		}
		{
			const char *v = "/menu";
			kv(&map, "Menu", DBUS_TYPE_OBJECT_PATH, (void **)&v);
		}
		pixmap_prop(&map, "IconPixmap", 0);
		pixmap_prop(&map, "AttentionIconPixmap", 1);
		pixmap_prop(&map, "OverlayIconPixmap", 1);
		dbus_message_iter_close_container(&it, &map);
	} else {
		// Get: a single (v), not a map
		DBusMessageIter in, v;
		const char *    want = nullptr;
		if (dbus_message_iter_init(m, &in) && dbus_message_iter_next(&in))
			dbus_message_iter_get_basic(&in, &want); // arg1: arg0 is the interface name
		if (!want)
			want = "";
		if (strcmp(want, "Category") == 0) {
			const char *s = "Applications";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "Id") == 0) {
			const char *s = "fakesni";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "Title") == 0) {
			const char *s = "Fake";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "Status") == 0) {
			const char *s = "Active";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "IconName") == 0 || strcmp(want, "AttentionIconName") == 0 || strcmp(want, "OverlayIconName") == 0) {
			const char *s = "";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "ItemIsMenu") == 0) {
			const int b = 1;
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "Menu") == 0) {
			const char *p = "/menu";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "o", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &p);
			dbus_message_iter_close_container(&it, &v);
		} else if (strcmp(want, "IconPixmap") == 0)
			pixmap_value(&it, 0);
		else if (strcmp(want, "AttentionIconPixmap") == 0 || strcmp(want, "OverlayIconPixmap") == 0)
			pixmap_value(&it, 1);
		else if (strcmp(want, "Version") == 0) {
			uint32_t u = 3;
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &u);
			dbus_message_iter_close_container(&it, &v);
		} else {
			const char *s = "";
			dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
			dbus_message_iter_close_container(&it, &v);
		}
	}
	dbus_connection_send(c, r, nullptr);
	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult vtable_msg(DBusConnection *c, DBusMessage *m, void *) {
	const char *path = dbus_message_get_path(m);
	if (path && strcmp(path, "/menu") == 0)
		return handle_menu(c, m, nullptr);
	if (path && strcmp(path, "/StatusNotifierItem") == 0)
		return handle_sni(c, m, nullptr);
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable SNI_VTABLE = {
	.message_function = vtable_msg,
};

// ---- registration ----------------------------------------------------------

static void try_register(void) {
	if (registered)
		return;
	DBusMessage *m = dbus_message_new_method_call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &SERVICE, DBUS_TYPE_INVALID);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, 1000, nullptr);
	dbus_message_unref(m);
	if (r) {
		registered = 1;
		dbus_message_unref(r);
	} else
		fprintf(stderr, "fake-sni: no watcher on the bus yet\n");
}

int main(void) {
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	bus = dbus_bus_get(DBUS_BUS_SESSION, nullptr);
	if (!bus) {
		fprintf(stderr, "fake-sni: no session bus\n");
		return 1;
	}
	DBusError e;
	dbus_error_init(&e);
	int rn = dbus_bus_request_name(bus, SERVICE, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
	if (rn != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "fake-sni: cannot own %s (%s)\n", SERVICE, e.message ? e.message : "?");
		return 1;
	}
	dbus_error_free(&e);

	try_register();

	dbus_connection_register_object_path(bus, "/StatusNotifierItem", &SNI_VTABLE, nullptr);
	dbus_connection_register_object_path(bus, "/menu", &SNI_VTABLE, nullptr);

	while (!stop) {
		dbus_connection_read_write_dispatch(bus, 200);
		if (!registered)
			try_register();
	}

	// bus came from dbus_bus_get (a shared connection): close() on a shared
	// connection is a libdbus fatal ("must not close shared connections"),
	// unref is the documented release.
	dbus_connection_unref(bus);
	return 0;
}
