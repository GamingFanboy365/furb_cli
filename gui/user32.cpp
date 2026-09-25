// Windows, messages, menus, accelerators, dialogs and their controls,
// message boxes, file pickers, cursor, keyboard and threads, on GTK 3.
// See gui.h for how this fits together.
#include "gui.h"
#include <algorithm>
#include <condition_variable>
#include <pthread.h>
#include <deque>
#include <set>
#include <thread>
#include <fnmatch.h>
#include <unistd.h>
#include <time.h>

namespace gui {

// ================================================================ strings
std::string utf8(const wchar_t *s) { return furb_narrow(s ? s : L""); }
std::string utf8(const std::wstring &s) { return furb_narrow(s.c_str()); }
std::wstring wide(const char *s) { return furb_widen(s ? s : ""); }

// "&File" -> "_File" (GTK mnemonic), "&&" -> "&", "_" -> "__"
static std::string mnemonic(const std::wstring &t, bool keep) {
	std::wstring r;
	for (size_t i = 0; i < t.size(); i++) {
		if (t[i] == L'&') {
			if (i + 1 < t.size() && t[i + 1] == L'&') { r += L'&'; i++; }
			else if (keep) r += L'_';
		} else if (t[i] == L'_') r += keep ? L"__" : L"_";
		else r += t[i];
	}
	return utf8(r);
}
// Win32 text uses \r\n; GTK wants \n
static std::string gtext(const std::wstring &t) {
	std::wstring r;
	for (wchar_t c : t) if (c != L'\r') r += c;
	return utf8(r);
}

// ================================================================ threads
static std::thread::id main_id = std::this_thread::get_id();
bool on_main(void) { return std::this_thread::get_id() == main_id; }

struct Call {
	const std::function<void()> *f;
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
};
static gboolean call_cb(gpointer p) {
	Call *c = (Call *)p;
	(*c->f)();
	std::lock_guard<std::mutex> l(c->m);
	c->done = true;
	c->cv.notify_one();
	return G_SOURCE_REMOVE;
}
void run_on_main(const std::function<void()> &f) {
	Call c;
	c.f = &f;
	GSource *s = g_idle_source_new();
	g_source_set_priority(s, G_PRIORITY_DEFAULT);
	g_source_set_callback(s, call_cb, &c, NULL);
	g_source_attach(s, NULL);
	g_source_unref(s);
	std::unique_lock<std::mutex> l(c.m);
	c.cv.wait(l, [&] { return c.done; });
}
void later(const std::function<void()> &f) {
	auto *p = new std::function<void()>(f);
	g_idle_add([](gpointer p) -> gboolean {
		auto *f = (std::function<void()> *)p;
		(*f)();
		delete f;
		return G_SOURCE_REMOVE;
	}, p);
}

// ================================================================ windows
static std::set<Wnd *> live;
static std::mutex live_lock;
static Wnd *the_main = NULL;
Wnd *main_window(void) { return the_main; }

Wnd *wnd(HWND h) {
	Wnd *w = (Wnd *)h;
	std::lock_guard<std::mutex> l(live_lock);
	return live.count(w) && !w->dead ? w : NULL;
}
static Wnd *new_wnd(void) {
	Wnd *w = new Wnd;
	std::lock_guard<std::mutex> l(live_lock);
	live.insert(w);
	return w;
}
LRESULT send(Wnd *w, UINT msg, WPARAM wp, LPARAM lp) {
	if (!w || w->dead || !w->wndproc) return 0;
	return w->wndproc(hwnd(w), msg, wp, lp);
}
static Wnd *toplevel(Wnd *w) {
	while (w && w->kind != K_MAIN && w->kind != K_DIALOG) w = w->parent;
	return w;
}
static int client_w(Wnd *w) { return w->rect.right - w->rect.left; }
static int client_h(Wnd *w) { return w->rect.bottom - w->rect.top; }
// "screen" position of a window's client origin (all rects are kept by us)
static POINT origin(Wnd *w) {
	POINT p = {0, 0};
	for (; w; w = (w->kind == K_MAIN || w->kind == K_DIALOG) ? NULL : w->parent) {
		p.x += w->rect.left;
		p.y += w->rect.top;
	}
	return p;
}

cairo_surface_t *backing_for(Wnd *w) {
	int cw = std::max(1, client_w(w)), ch = std::max(1, client_h(w));
	if (w->widget && gtk_widget_get_realized(w->widget)) {
		cw = std::max(1, gtk_widget_get_allocated_width(w->widget));
		ch = std::max(1, gtk_widget_get_allocated_height(w->widget));
	}
	if (w->backing && (cairo_image_surface_get_width(w->backing) != cw || cairo_image_surface_get_height(w->backing) != ch)) {
		cairo_surface_t *n = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
		cairo_t *cr = cairo_create(n);
		cairo_set_source_surface(cr, w->backing, 0, 0);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(w->backing);
		w->backing = n;
	}
	if (!w->backing) w->backing = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cw, ch);
	return w->backing;
}

void invalidate(Wnd *w) {
	if (!w) return;
	w->dirty = true;
	later([w] { if (wnd(hwnd(w)) && w->widget) gtk_widget_queue_draw(w->widget); });
}

// ================================================================ message queue
static std::mutex qlock;
static std::deque<MSG> queue;
static bool quitting = false;
static int quit_code = 0;

static void post(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	{
		std::lock_guard<std::mutex> l(qlock);
		if (m == WM_PAINT)
			for (auto &q : queue) if (q.hwnd == h && q.message == WM_PAINT) return;
		MSG msg = {};
		msg.hwnd = h; msg.message = m; msg.wParam = wp; msg.lParam = lp; msg.time = GetTickCount();
		queue.push_back(msg);
	}
	g_main_context_wakeup(NULL);
}

// ================================================================ keyboard
std::atomic<unsigned char> dik_state[256];
std::atomic<unsigned char> vk_state[256];
std::atomic<int> mouse_x(0), mouse_y(0), mouse_dx(0), mouse_dy(0), mouse_dz(0);
std::atomic<unsigned char> mouse_buttons(0);

// evdev key code -> DirectInput scan code (set 1; evdev 1..88 are equal)
static int dik_of(guint hw) {
	int ev = (int)hw - 8;
	if (ev > 0 && ev <= 88) return ev;
	switch (ev) {
	case 89: return 0x73;	// RO
	case 92: return 0x79;	// HENKAN (convert)
	case 93: return 0x70;	// KATAKANAHIRAGANA (kana)
	case 94: return 0x7B;	// MUHENKAN
	case 96: return 0x9C;	// KP enter
	case 97: return 0x9D;	// right ctrl
	case 98: return 0xB5;	// KP /
	case 99: return 0xB7;	// SysRq
	case 100: return 0xB8;	// right alt
	case 102: return 0xC7;	// home
	case 103: return 0xC8;	// up
	case 104: return 0xC9;	// page up
	case 105: return 0xCB;	// left
	case 106: return 0xCD;	// right
	case 107: return 0xCF;	// end
	case 108: return 0xD0;	// down
	case 109: return 0xD1;	// page down
	case 110: return 0xD2;	// insert
	case 111: return 0xD3;	// delete
	case 119: return 0xC5;	// pause
	case 124: return 0x7D;	// yen
	case 125: return 0xDB;	// left win
	case 126: return 0xDC;	// right win
	case 127: return 0xDD;	// menu
	}
	return 0;
}

UINT vk_of(GdkEventKey *e) {
	guint k = gdk_keyval_to_upper(e->keyval);
	if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9')) return k;
	if (k >= GDK_KEY_F1 && k <= GDK_KEY_F24) return VK_F1 + (k - GDK_KEY_F1);
	if (k >= GDK_KEY_KP_0 && k <= GDK_KEY_KP_9) return 0x60 + (k - GDK_KEY_KP_0);
	switch (k) {
	case GDK_KEY_BackSpace: return VK_BACK;
	case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return VK_TAB;
	case GDK_KEY_Return: case GDK_KEY_KP_Enter: return VK_RETURN;
	case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: return VK_SHIFT;
	case GDK_KEY_Control_L: case GDK_KEY_Control_R: return VK_CONTROL;
	case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: case GDK_KEY_Meta_L: case GDK_KEY_Meta_R: return VK_MENU;
	case GDK_KEY_Pause: return 0x13;
	case GDK_KEY_Caps_Lock: return VK_CAPITAL;
	case GDK_KEY_Escape: return VK_ESCAPE;
	case GDK_KEY_space: return VK_SPACE;
	case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return VK_PRIOR;
	case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return VK_NEXT;
	case GDK_KEY_End: case GDK_KEY_KP_End: return VK_END;
	case GDK_KEY_Home: case GDK_KEY_KP_Home: return VK_HOME;
	case GDK_KEY_Left: case GDK_KEY_KP_Left: return VK_LEFT;
	case GDK_KEY_Up: case GDK_KEY_KP_Up: return VK_UP;
	case GDK_KEY_Right: case GDK_KEY_KP_Right: return VK_RIGHT;
	case GDK_KEY_Down: case GDK_KEY_KP_Down: return VK_DOWN;
	case GDK_KEY_Insert: case GDK_KEY_KP_Insert: return VK_INSERT;
	case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return VK_DELETE;
	case GDK_KEY_KP_Multiply: return 0x6A;
	case GDK_KEY_KP_Add: return VK_ADD;
	case GDK_KEY_KP_Subtract: return VK_SUBTRACT;
	case GDK_KEY_KP_Decimal: return 0x6E;
	case GDK_KEY_KP_Divide: return 0x6F;
	case GDK_KEY_Num_Lock: return VK_NUMLOCK;
	case GDK_KEY_Scroll_Lock: return VK_SCROLL;
	case GDK_KEY_semicolon: case GDK_KEY_colon: return 0xBA;
	case GDK_KEY_equal: case GDK_KEY_plus: return 0xBB;
	case GDK_KEY_comma: case GDK_KEY_less: return 0xBC;
	case GDK_KEY_minus: case GDK_KEY_underscore: return 0xBD;
	case GDK_KEY_period: case GDK_KEY_greater: return 0xBE;
	case GDK_KEY_slash: case GDK_KEY_question: return 0xBF;
	case GDK_KEY_grave: case GDK_KEY_asciitilde: return 0xC0;
	case GDK_KEY_bracketleft: case GDK_KEY_braceleft: return 0xDB;
	case GDK_KEY_backslash: case GDK_KEY_bar: return 0xDC;
	case GDK_KEY_bracketright: case GDK_KEY_braceright: return 0xDD;
	case GDK_KEY_apostrophe: case GDK_KEY_quotedbl: return 0xDE;
	}
	return 0;
}

void key_event(GdkEventKey *e, bool down) {
	int d = dik_of(e->hardware_keycode);
	static bool debug = getenv("FURB_DEBUG_KEYS") != NULL;
	if (debug) fprintf(stderr, "furb: key %s keyval %s hw %d dik %02X vk %02X\n", down ? "down" : "up",
		gdk_keyval_name(e->keyval), e->hardware_keycode, d, vk_of(e));
	if (d) dik_state[d] = down ? 0x80 : 0;
	UINT vk = vk_of(e);
	if (vk) vk_state[vk] = down ? 0x80 : 0;
	// the left/right variants
	switch (e->keyval) {
	case GDK_KEY_Shift_L: vk_state[0xA0] = down ? 0x80 : 0; break;
	case GDK_KEY_Shift_R: vk_state[0xA1] = down ? 0x80 : 0; break;
	case GDK_KEY_Control_L: vk_state[0xA2] = down ? 0x80 : 0; break;
	case GDK_KEY_Control_R: vk_state[0xA3] = down ? 0x80 : 0; break;
	case GDK_KEY_Alt_L: vk_state[0xA4] = down ? 0x80 : 0; break;
	case GDK_KEY_Alt_R: vk_state[0xA5] = down ? 0x80 : 0; break;
	}
}
void clear_keys(void) {
	for (auto &k : dik_state) k = 0;
	for (auto &k : vk_state) k = 0;
	mouse_buttons = 0;
}

// Caps Lock / Scroll Lock toggle state for GetKeyState's low bit
static bool lock_on(int vk) {
	GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
	if (vk == VK_CAPITAL) return gdk_keymap_get_caps_lock_state(km);
	if (vk == VK_NUMLOCK) return gdk_keymap_get_num_lock_state(km);
	if (vk == VK_SCROLL) return gdk_keymap_get_scroll_lock_state(km);
	return false;
}

}	// namespace gui
using namespace gui;

SHORT GetKeyState(int vk) {
	if (vk < 0 || vk > 255) return 0;
	SHORT s = (vk_state[vk] & 0x80) ? (SHORT)0x8000 : 0;
	if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
		if (sync([&] { return lock_on(vk); })) s |= 1;
	return s;
}
SHORT GetAsyncKeyState(int vk) { return (vk >= 0 && vk < 256 && (vk_state[vk] & 0x80)) ? (SHORT)0x8000 : 0; }
UINT MapVirtualKey(UINT code, UINT) { return code; }
int GetKeyNameText(LONG l, LPTSTR s, int n) {
	if (!s || n <= 0) return 0;
	swprintf(s, n, L"Key %d", (int)(l >> 16 & 0x1FF));
	return (int)wcslen(s);
}

// ================================================================ menus
namespace gui {
static Menu *as_menu(HMENU h) { Menu *m = (Menu *)h; return m && m->magic == 0x4655524D ? m : NULL; }

static Menu *build_menu(const FurbRcMenuItem *items, int n) {
	Menu *m = new Menu;
	for (int i = 0; i < n; i++) {
		MenuItem it;
		it.text = items[i].text ? items[i].text : L"";
		it.id = items[i].id;
		it.flags = items[i].flags;
		if (items[i].flags & MF_POPUP) it.sub = build_menu(items[i].child, items[i].nchild);
		m->items.push_back(it);
	}
	return m;
}

static MenuItem *find_item(Menu *m, UINT id, UINT flags, Menu **in = NULL) {
	if (!m) return NULL;
	if (flags & MF_BYPOSITION) {
		if (id >= m->items.size()) return NULL;
		if (in) *in = m;
		return &m->items[id];
	}
	for (auto &it : m->items) {
		if (!(it.flags & (MF_POPUP | MF_SEPARATOR)) && it.id == id) { if (in) *in = m; return &it; }
		if (it.sub) if (MenuItem *r = find_item(it.sub, id, flags, in)) return r;
	}
	return NULL;
}

enum { MF_RADIO_ = 0x200 };
static void update_item(MenuItem &it) {
	if (!it.w) return;
	gtk_widget_set_sensitive(it.w, !(it.flags & (MF_GRAYED | MF_DISABLED)));
	if (it.mark)
		gtk_label_set_text(GTK_LABEL(it.mark), (it.flags & MF_CHECKED) ? ((it.flags & MF_RADIO_) ? "\xE2\x97\x8F" : "\xE2\x9C\x93") : "");
}

static void menu_activate(GtkMenuItem *, gpointer p) {
	MenuItem *it = (MenuItem *)p;
	// find the owning window: walk up through the widget's menu's attach
	Wnd *owner = (Wnd *)g_object_get_data(G_OBJECT(it->w), "furb-owner");
	if (owner && wnd(hwnd(owner))) post(hwnd(owner), WM_COMMAND, MAKEWPARAM(it->id, 0), 0);
}

GtkWidget *menu_widget(Menu *m, Wnd *owner, bool bar) {
	m->owner = owner;
	GtkWidget *g = bar ? gtk_menu_bar_new() : gtk_menu_new();
	for (auto &it : m->items) {
		if (it.flags & MF_SEPARATOR) {
			it.w = gtk_separator_menu_item_new();
			gtk_menu_shell_append(GTK_MENU_SHELL(g), it.w);
			continue;
		}
		std::wstring label = it.text, accel;
		size_t tab = label.find(L'\t');
		if (tab != std::wstring::npos) { accel = label.substr(tab + 1); label = label.substr(0, tab); }
		it.w = gtk_menu_item_new();
		g_object_set_data(G_OBJECT(it.w), "furb-owner", owner);
		if (bar || it.sub) {
			GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic(label, true).c_str());
			gtk_container_add(GTK_CONTAINER(it.w), l);
			if (it.sub) gtk_menu_item_set_submenu(GTK_MENU_ITEM(it.w), menu_widget(it.sub, owner, false));
		} else {
			GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			it.mark = gtk_label_new("");
			gtk_widget_set_size_request(it.mark, 12, -1);
			GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic(label, true).c_str());
			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_box_pack_start(GTK_BOX(box), it.mark, FALSE, FALSE, 0);
			gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
			if (!accel.empty()) {
				GtkWidget *a = gtk_label_new(utf8(accel).c_str());
				gtk_style_context_add_class(gtk_widget_get_style_context(a), "dim-label");
				gtk_box_pack_end(GTK_BOX(box), a, FALSE, FALSE, 12);
			}
			gtk_container_add(GTK_CONTAINER(it.w), box);
			g_signal_connect(it.w, "activate", G_CALLBACK(menu_activate), &it);
		}
		update_item(it);
		gtk_menu_shell_append(GTK_MENU_SHELL(g), it.w);
	}
	m->gtk = g;
	gtk_widget_show_all(g);
	return g;
}

bool menu_enabled(Menu *m, UINT id) {
	MenuItem *it = find_item(m, id, MF_BYCOMMAND);
	return !it || !(it->flags & (MF_GRAYED | MF_DISABLED));
}
}	// namespace gui

static const FurbResources *res_of(HINSTANCE h) {
	return h ? (const FurbResources *)h : &furb_module_resources;
}
static int res_id(LPCTSTR name) { return (uintptr_t)name < 0x10000 ? (int)(uintptr_t)name : -1; }

HMENU LoadMenu(HINSTANCE hinst, LPCTSTR name) {
	const FurbResources *r = res_of(hinst);
	for (int i = 0; i < r->nmenus; i++)
		if (r->menus[i].id == res_id(name)) return (HMENU)build_menu(r->menus[i].items, r->menus[i].n);
	return NULL;
}
HMENU CreateMenu(void) { return (HMENU)new Menu; }
HMENU CreatePopupMenu(void) { return (HMENU)new Menu; }
BOOL DestroyMenu(HMENU) { return TRUE; }
HMENU GetMenu(HWND h) { return sync([&] { Wnd *w = wnd(h); return w ? (HMENU)w->menu : (HMENU)NULL; }); }
HMENU GetSubMenu(HMENU h, int pos) {
	Menu *m = as_menu(h);
	return m && pos >= 0 && pos < (int)m->items.size() ? (HMENU)m->items[pos].sub : NULL;
}
int GetMenuItemCount(HMENU h) { Menu *m = as_menu(h); return m ? (int)m->items.size() : -1; }
DWORD CheckMenuItem(HMENU h, UINT id, UINT f) {
	return sync([&]() -> DWORD {
		MenuItem *it = find_item(as_menu(h), id, f);
		if (!it) return (DWORD)-1;
		DWORD old = it->flags & MF_CHECKED;
		it->flags = (it->flags & ~(MF_CHECKED | MF_RADIO_)) | (f & MF_CHECKED);
		update_item(*it);
		return old;
	});
}
BOOL CheckMenuRadioItem(HMENU h, UINT first, UINT last, UINT check, UINT f) {
	return sync([&]() -> BOOL {
		Menu *m = as_menu(h), *in = NULL;
		if (!find_item(m, check, f, &in)) return FALSE;
		for (UINT i = first; i <= last; i++) {
			MenuItem *it = find_item(in, i, f);
			if (!it) continue;
			it->flags = (it->flags & ~(MF_CHECKED | MF_RADIO_)) | (i == check ? MF_CHECKED | MF_RADIO_ : 0);
			update_item(*it);
		}
		return TRUE;
	});
}
BOOL EnableMenuItem(HMENU h, UINT id, UINT f) {
	return sync([&]() -> BOOL {
		MenuItem *it = find_item(as_menu(h), id, f);
		if (!it) return -1;
		BOOL old = it->flags & (MF_GRAYED | MF_DISABLED);
		it->flags = (it->flags & ~(MF_GRAYED | MF_DISABLED)) | (f & (MF_GRAYED | MF_DISABLED));
		update_item(*it);
		return old;
	});
}
UINT GetMenuState(HMENU h, UINT id, UINT f) {
	return sync([&]() -> UINT { MenuItem *it = find_item(as_menu(h), id, f); return it ? (it->flags & 0xFF) : (UINT)-1; });
}
BOOL DrawMenuBar(HWND) { return TRUE; }
static BOOL set_item(Menu *m, MenuItem *it, UINT f, UINT_PTR id, LPCTSTR text) {
	it->flags = f & ~MF_BYPOSITION;
	if (f & MF_POPUP) it->sub = as_menu((HMENU)id);
	else it->id = (UINT)id;
	if (!(f & MF_SEPARATOR) && text) it->text = text;
	if (m->gtk && m->owner) {	// rebuild the widget tree of the owner's bar
		Wnd *o = m->owner;
		if (o->menubar && o->menu) {
			GtkWidget *box = gtk_widget_get_parent(o->menubar);
			gtk_widget_destroy(o->menubar);
			o->menubar = menu_widget(o->menu, o, true);
			gtk_box_pack_start(GTK_BOX(box), o->menubar, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(box), o->menubar, 0);
		}
	}
	return TRUE;
}
BOOL AppendMenu(HMENU h, UINT f, UINT_PTR id, LPCTSTR text) {
	return sync([&]() -> BOOL {
		Menu *m = as_menu(h);
		if (!m) return FALSE;
		m->items.push_back(MenuItem());
		return set_item(m, &m->items.back(), f, id, text);
	});
}
BOOL InsertMenu(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCTSTR text) {
	return sync([&]() -> BOOL {
		Menu *m = as_menu(h);
		if (!m) return FALSE;
		size_t at = m->items.size();
		if (f & MF_BYPOSITION) at = std::min<size_t>(pos, at);
		else for (size_t i = 0; i < m->items.size(); i++) if (m->items[i].id == pos) { at = i; break; }
		m->items.insert(m->items.begin() + at, MenuItem());
		return set_item(m, &m->items[at], f, id, text);
	});
}
BOOL ModifyMenu(HMENU h, UINT pos, UINT f, UINT_PTR id, LPCTSTR text) {
	return sync([&]() -> BOOL {
		Menu *in = NULL;
		MenuItem *it = find_item(as_menu(h), pos, f, &in);
		return it ? set_item(in, it, f, id, text) : FALSE;
	});
}
BOOL DeleteMenu(HMENU h, UINT pos, UINT f) {
	return sync([&]() -> BOOL {
		Menu *in = NULL;
		MenuItem *it = find_item(as_menu(h), pos, f, &in);
		if (!it) return FALSE;
		if (it->w) gtk_widget_destroy(it->w);
		in->items.erase(in->items.begin() + (it - &in->items[0]));
		return TRUE;
	});
}
BOOL RemoveMenu(HMENU h, UINT pos, UINT f) { return DeleteMenu(h, pos, f); }
BOOL TrackPopupMenu(HMENU h, UINT, int, int, int, HWND owner, const RECT *) {
	return sync([&]() -> BOOL {
		Menu *m = as_menu(h);
		Wnd *o = wnd(owner);
		if (!m || !o) return FALSE;
		GtkWidget *g = menu_widget(m, o, false);
		gtk_menu_popup_at_pointer(GTK_MENU(g), NULL);
		return TRUE;
	});
}
BOOL SetMenu(HWND h, HMENU hm) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w || w->kind != K_MAIN) return FALSE;
		Menu *m = as_menu(hm);
		if (w->menubar) { gtk_widget_destroy(w->menubar); w->menubar = NULL; }
		w->menu = m ? m : w->menu;
		if (m) {
			GtkWidget *box = gtk_bin_get_child(GTK_BIN(w->top));
			w->menubar = menu_widget(m, w, true);
			gtk_box_pack_start(GTK_BOX(box), w->menubar, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(box), w->menubar, 0);
		}
		return TRUE;
	});
}

// ---- accelerators
HACCEL LoadAccelerators(HINSTANCE hinst, LPCTSTR name) {
	const FurbResources *r = res_of(hinst);
	for (int i = 0; i < r->naccels; i++)
		if (r->accels[i].id == res_id(name)) return (HACCEL)&r->accels[i];
	return NULL;
}
int TranslateAccelerator(HWND h, HACCEL ha, MSG *msg) {
	if (!ha || !msg || (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN)) return 0;
	const FurbRcAccelTable *t = (const FurbRcAccelTable *)ha;
	bool shift = vk_state[VK_SHIFT] & 0x80, ctrl = vk_state[VK_CONTROL] & 0x80, alt = vk_state[VK_MENU] & 0x80;
	for (int i = 0; i < t->n; i++) {
		const FurbRcAccel &a = t->a[i];
		if (!(a.flags & 1) || a.key != msg->wParam) continue;
		if (!!(a.flags & 4) != shift || !!(a.flags & 8) != ctrl || !!(a.flags & 0x10) != alt) continue;
		Wnd *w = wnd(h);
		if (w && w->menu && !menu_enabled(w->menu, a.cmd)) return 1;	// as Windows: a disabled item's key does nothing
		SendMessage(h, WM_COMMAND, MAKEWPARAM(a.cmd, 1), 0);
		return 1;
	}
	return 0;
}

// ================================================================ messages
BOOL PostMessage(HWND h, UINT m, WPARAM wp, LPARAM lp) { post(h, m, wp, lp); return TRUE; }
void PostQuitMessage(int code) {
	{ std::lock_guard<std::mutex> l(qlock); quitting = true; quit_code = code; }
	g_main_context_wakeup(NULL);
}
LRESULT SendMessage(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	return sync([&] { return send(wnd(h), m, wp, lp); });
}
static bool pop(MSG *msg, bool remove, bool &quit) {
	std::lock_guard<std::mutex> l(qlock);
	quit = quitting;
	if (queue.empty()) return false;
	*msg = queue.front();
	if (remove) queue.pop_front();
	return true;
}
// Returns WM_NULL after any GTK event that queued nothing, so the caller's
// loop (WinMain's, a modal dialog's) gets to look at its state.
BOOL GetMessage(MSG *msg, HWND, UINT, UINT) {
	bool quit;
	for (int round = 0; round < 2; round++) {
		if (pop(msg, true, quit)) return TRUE;
		if (quit) { memset(msg, 0, sizeof *msg); msg->message = WM_QUIT; msg->wParam = quit_code; return FALSE; }
		if (!round) g_main_context_iteration(NULL, TRUE);
	}
	memset(msg, 0, sizeof *msg);
	return TRUE;
}
BOOL PeekMessage(MSG *msg, HWND, UINT, UINT, UINT remove) {
	for (int i = 0; i < 64 && g_main_context_iteration(NULL, FALSE); i++) {}
	bool quit;
	return pop(msg, (remove & PM_REMOVE) != 0, quit);
}
BOOL TranslateMessage(const MSG *) { return TRUE; }
LRESULT DispatchMessage(const MSG *msg) {
	if (!msg || !msg->hwnd) return 0;
	if (msg->message == WM_TIMER && msg->lParam) {
		((void (*)(HWND, UINT, UINT_PTR, DWORD))msg->lParam)(msg->hwnd, WM_TIMER, msg->wParam, msg->time);
		return 0;
	}
	return send(wnd(msg->hwnd), msg->message, msg->wParam, msg->lParam);
}
BOOL IsDialogMessage(HWND, MSG *) { return FALSE; }	// GTK handles keyboard navigation in dialogs

struct Timer { HWND h; UINT_PTR id; void *fn; guint src; };
static std::vector<Timer> timers;
UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *fn) {
	return sync([&]() -> UINT_PTR {
		KillTimer(h, id);
		Timer *t = new Timer{h, id, fn, 0};
		t->src = g_timeout_add(std::max(1u, ms), [](gpointer p) -> gboolean {
			Timer *t = (Timer *)p;
			post(t->h, WM_TIMER, t->id, (LPARAM)t->fn);
			return G_SOURCE_CONTINUE;
		}, t);
		timers.push_back(*t);
		return id ? id : 1;
	});
}
BOOL KillTimer(HWND h, UINT_PTR id) {
	return sync([&]() -> BOOL {
		for (size_t i = 0; i < timers.size(); i++)
			if (timers[i].h == h && timers[i].id == id) { g_source_remove(timers[i].src); timers.erase(timers.begin() + i); return TRUE; }
		return FALSE;
	});
}

LRESULT DefWindowProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	Wnd *w = wnd(h);
	if (!w) return 0;
	switch (m) {
	case WM_CLOSE: DestroyWindow(h); return 0;
	case WM_SETTEXT: SetWindowText(h, (LPCTSTR)lp); return TRUE;
	case WM_GETTEXT: return GetWindowText(h, (LPTSTR)lp, (int)wp);
	case WM_GETTEXTLENGTH: return (LRESULT)w->text.size();
	}
	return 0;
}
LRESULT CallWindowProc(WNDPROC p, HWND h, UINT m, WPARAM wp, LPARAM lp) { return p ? p(h, m, wp, lp) : 0; }

// ================================================================ the main window
static std::map<std::wstring, WNDCLASSEX> classes;
ATOM RegisterClassEx(const WNDCLASSEX *wc) {
	if (!wc || !wc->lpszClassName) return 0;
	classes[wc->lpszClassName] = *wc;
	return (ATOM)classes.size();
}

struct Drop { std::vector<std::wstring> files; };

static void post_mouse(Wnd *w, UINT msg, double x, double y, guint state) {
	WPARAM mk = ((state & GDK_BUTTON1_MASK) ? 1 : 0) | ((state & GDK_BUTTON3_MASK) ? 2 : 0) |
		((state & GDK_SHIFT_MASK) ? 4 : 0) | ((state & GDK_CONTROL_MASK) ? 8 : 0) | ((state & GDK_BUTTON2_MASK) ? 0x10 : 0);
	post(hwnd(w), msg, mk, MAKELPARAM((int)x, (int)y));
}

static gboolean main_key(GtkWidget *, GdkEventKey *e, gpointer p) {
	Wnd *w = (Wnd *)p;
	bool down = e->type == GDK_KEY_PRESS;
	key_event(e, down);
	UINT vk = vk_of(e);
	bool alt = (e->state & GDK_MOD1_MASK) || vk == VK_MENU;
	UINT msg = alt || vk == VK_F1 + 9 ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
	LPARAM lp = 1 | (LPARAM)dik_of(e->hardware_keycode) << 16 | (alt ? 1 << 29 : 0) | (down ? 0 : 3u << 30);
	if (vk) post(hwnd(w), msg, vk, lp);
	// Alt+letter opens the menus (GTK's mnemonics); every other key belongs to the emulator
	return !(alt && vk != VK_MENU && vk != VK_RETURN);
}

static gboolean main_draw(GtkWidget *area, cairo_t *cr, gpointer p) {
	Wnd *w = (Wnd *)p;
	int aw = gtk_widget_get_allocated_width(area), ah = gtk_widget_get_allocated_height(area);
	if (draw_frame(cr, aw, ah)) return TRUE;
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	if (w->backing) {
		cairo_set_source_surface(cr, w->backing, 0, 0);
		cairo_paint(cr);
	}
	return TRUE;
}

static int cursor_count = 0;
static void set_pointer(Wnd *w);
static gboolean main_motion(GtkWidget *, GdkEventMotion *e, gpointer p) {
	Wnd *w = (Wnd *)p;
	int x = (int)e->x, y = (int)e->y;
	mouse_dx += x - mouse_x; mouse_dy += y - mouse_y;
	mouse_x = x; mouse_y = y;
	send(w, WM_SETCURSOR, (WPARAM)hwnd(w), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
	post_mouse(w, WM_MOUSEMOVE, e->x, e->y, e->state);
	return FALSE;
}
static gboolean main_button(GtkWidget *area, GdkEventButton *e, gpointer p) {
	Wnd *w = (Wnd *)p;
	if (e->type != GDK_BUTTON_PRESS && e->type != GDK_BUTTON_RELEASE) return FALSE;
	bool down = e->type == GDK_BUTTON_PRESS;
	int bit = e->button == 1 ? 1 : e->button == 3 ? 2 : e->button == 2 ? 4 : 0;
	if (bit) mouse_buttons = down ? (mouse_buttons | bit) : (mouse_buttons & ~bit);
	UINT msg = e->button == 1 ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP) : e->button == 3 ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
		: (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
	if (down) gtk_widget_grab_focus(area);
	post_mouse(w, msg, e->x, e->y, e->state);
	return FALSE;
}
static gboolean main_scroll(GtkWidget *, GdkEventScroll *e, gpointer) {
	if (e->direction == GDK_SCROLL_UP) mouse_dz += 120;
	else if (e->direction == GDK_SCROLL_DOWN) mouse_dz -= 120;
	return FALSE;
}
static void main_size(GtkWidget *, GdkRectangle *a, gpointer p) {
	Wnd *w = (Wnd *)p;
	if (a->width == client_w(w) && a->height == client_h(w)) return;
	w->rect.right = w->rect.left + a->width;
	w->rect.bottom = w->rect.top + a->height;
	post(hwnd(w), WM_SIZE, 0, MAKELPARAM(a->width, a->height));
	post(hwnd(w), WM_PAINT, 0, 0);
}
static void main_drop(GtkWidget *, GdkDragContext *ctx, gint, gint, GtkSelectionData *data, guint, guint t, gpointer p) {
	Wnd *w = (Wnd *)p;
	gchar **uris = gtk_selection_data_get_uris(data);
	Drop *d = new Drop;
	for (int i = 0; uris && uris[i]; i++) {
		gchar *f = g_filename_from_uri(uris[i], NULL, NULL);
		if (f) { d->files.push_back(wide(f)); g_free(f); }
	}
	g_strfreev(uris);
	gtk_drag_finish(ctx, !d->files.empty(), FALSE, t);
	if (d->files.empty()) delete d;
	else post(hwnd(w), WM_DROPFILES, (WPARAM)d, 0);
}

static void set_pointer(Wnd *w) {
	GdkWindow *gw = w->widget ? gtk_widget_get_window(w->widget) : NULL;
	if (!gw) return;
	GdkDisplay *d = gdk_display_get_default();
	GdkCursor *c = cursor_count < 0 ? gdk_cursor_new_for_display(d, GDK_BLANK_CURSOR)
		: gdk_cursor_new_from_name(d, w->cursor == 32515 ? "crosshair" : "default");
	gdk_window_set_cursor(gw, c);
	if (c) g_object_unref(c);
}

static Wnd *create_main(const WNDCLASSEX &wc, LPCTSTR title, DWORD style, HMENU menu) {
	Wnd *w = new_wnd();
	w->kind = K_MAIN;
	w->cls = wc.lpszClassName;
	w->text = title ? title : L"";
	w->style = style;
	w->wndproc = (WNDPROC)wc.lpfnWndProc;
	w->menu = as_menu(menu);
	w->rect = {40, 40, 40 + 512, 40 + 480};
	the_main = w;
	w->top = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(w->top), utf8(w->text).c_str());
	gtk_window_set_default_size(GTK_WINDOW(w->top), 512, 480);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(w->top), box);
	if (w->menu) {
		w->menubar = menu_widget(w->menu, w, true);
		gtk_box_pack_start(GTK_BOX(box), w->menubar, FALSE, FALSE, 0);
	}
	w->widget = gtk_drawing_area_new();
	gtk_widget_set_can_focus(w->widget, TRUE);
	gtk_widget_add_events(w->widget, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_SCROLL_MASK);
	gtk_box_pack_start(GTK_BOX(box), w->widget, TRUE, TRUE, 0);
	g_signal_connect(w->top, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
		post(hwnd((Wnd *)p), WM_CLOSE, 0, 0);
		return TRUE;
	}), w);
	g_signal_connect(w->top, "key-press-event", G_CALLBACK(main_key), w);
	g_signal_connect(w->top, "key-release-event", G_CALLBACK(main_key), w);
	g_signal_connect(w->top, "focus-out-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer) -> gboolean { clear_keys(); return FALSE; }), NULL);
	g_signal_connect(w->widget, "draw", G_CALLBACK(main_draw), w);
	g_signal_connect(w->widget, "motion-notify-event", G_CALLBACK(main_motion), w);
	g_signal_connect(w->widget, "button-press-event", G_CALLBACK(main_button), w);
	g_signal_connect(w->widget, "button-release-event", G_CALLBACK(main_button), w);
	g_signal_connect(w->widget, "scroll-event", G_CALLBACK(main_scroll), w);
	g_signal_connect(w->widget, "size-allocate", G_CALLBACK(main_size), w);
	gtk_drag_dest_set(w->widget, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets(w->widget);
	g_signal_connect(w->widget, "drag-data-received", G_CALLBACK(main_drop), w);
	gtk_widget_show_all(box);
	return w;
}

// ================================================================ dialogs
// A template from a module's .rc (furb_rc.h) or built in memory (DIPSwitch.cpp:
// DLGTEMPLATEEX, strings in this platform's 4-byte wchar_t).
struct Item { std::wstring cls, text; int id, x, y, cx, cy; DWORD style, ex; };
struct Tmpl { std::wstring caption, font; int size = 8, x = 0, y = 0, cx = 0, cy = 0; DWORD style = 0, ex = 0; std::vector<Item> items; };

static bool from_rc(const FurbResources *r, int id, Tmpl &t) {
	for (int i = 0; i < r->ndialogs; i++) {
		const FurbRcDialog &d = r->dialogs[i];
		if (d.id != id) continue;
		t.caption = d.caption; t.font = d.font; t.size = d.fontsize;
		t.x = d.x; t.y = d.y; t.cx = d.cx; t.cy = d.cy; t.style = d.style; t.ex = d.exstyle;
		for (int k = 0; k < d.n; k++) {
			const FurbRcControl &c = d.controls[k];
			t.items.push_back({c.cls, c.text, c.id, c.x, c.y, c.cx, c.cy, c.style, c.exstyle});
		}
		return true;
	}
	return false;
}
static bool from_memory(LPCDLGTEMPLATE p, Tmpl &t) {
	const BYTE *b = (const BYTE *)p;
	auto u16 = [&](size_t &o) { uint16_t v; memcpy(&v, b + o, 2); o += 2; return v; };
	auto u32 = [&](size_t &o) { uint32_t v; memcpy(&v, b + o, 4); o += 4; return v; };
	auto s16 = [&](size_t &o) { return (int16_t)u16(o); };
	auto str = [&](size_t &o, int *ord) -> std::wstring {	// sz_Or_Ord in wchar_t units
		uint32_t first; memcpy(&first, b + o, 4);
		if ((first & 0xFFFF) == 0xFFFF) { o += 4; if (ord) *ord = first >> 16; return L""; }
		std::wstring s;
		for (;;) { uint32_t c; memcpy(&c, b + o, 4); o += 4; if (!c) break; s += (wchar_t)c; }
		return s;
	};
	size_t o = 0;
	if (u16(o) != 1 || u16(o) != 0xFFFF) return false;	// only DLGTEMPLATEEX is built in memory
	u32(o);
	t.ex = u32(o); t.style = u32(o);
	int n = u16(o);
	t.x = s16(o); t.y = s16(o); t.cx = s16(o); t.cy = s16(o);
	str(o, NULL); str(o, NULL);
	t.caption = str(o, NULL);
	if (t.style & DS_SETFONT) { t.size = u16(o); u16(o); o += 2; t.font = str(o, NULL); }
	for (int i = 0; i < n; i++) {
		o = (o + 3) & ~(size_t)3;
		Item it;
		u32(o);
		it.ex = u32(o); it.style = u32(o);
		it.x = s16(o); it.y = s16(o); it.cx = s16(o); it.cy = s16(o);
		it.id = (int)u32(o);
		int ord = 0;
		it.cls = str(o, &ord);
		if (ord) it.cls = ord == 0x80 ? L"Button" : ord == 0x81 ? L"Edit" : ord == 0x82 ? L"Static" : ord == 0x83 ? L"ListBox"
			: ord == 0x84 ? L"ScrollBar" : ord == 0x85 ? L"ComboBox" : L"";
		it.text = str(o, NULL);
		o += u16(o);
		t.items.push_back(it);
	}
	return true;
}

// dialog units -> pixels: Windows' base units for the dialog font, a little
// roomier because GTK's widgets are
static int bx(const Tmpl &t) { return std::max(6, t.size * 7 / 8 + (t.size >= 10 ? 1 : 0)) ; }
static int by(const Tmpl &t) { return std::max(13, t.size * 15 / 8); }

static LRESULT CALLBACK dialog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
LRESULT CALLBACK control_proc(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void notify(Wnd *c, int code) {
	if (c->parent) send(c->parent, WM_COMMAND, MAKEWPARAM(c->id, code), (LPARAM)hwnd(c));
}
static std::vector<Wnd *> radio_group(Wnd *c) {
	std::vector<Wnd *> &sib = c->parent->children, out;
	size_t i = std::find(sib.begin(), sib.end(), c) - sib.begin();
	size_t a = i;
	while (a > 0 && !(sib[a]->style & WS_GROUP)) a--;
	for (size_t k = a; k < sib.size(); k++) {
		if (k > a && (sib[k]->style & WS_GROUP)) break;
		if (sib[k]->kind == K_RADIO) out.push_back(sib[k]);
	}
	return out;
}
static void set_check(Wnd *c, int v) {
	c->check = v;
	if (!c->inner) return;
	c->block++;
	if (c->kind == K_RADIO) {
		if (v) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->inner), TRUE);
		else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->dummy), TRUE);
	} else {
		gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(c->inner), v == BST_INDETERMINATE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->inner), v == BST_CHECKED);
	}
	c->block--;
}

// -------------------------------------------------------------- control widgets
static void on_clicked(GtkButton *, gpointer p) {
	Wnd *c = (Wnd *)p;
	if (!c->block) notify(c, BN_CLICKED);
}
static void on_toggled(GtkToggleButton *b, gpointer p) {
	Wnd *c = (Wnd *)p;
	if (c->block) return;
	bool active = gtk_toggle_button_get_active(b);
	int type = c->style & 0xF;
	if (c->kind == K_RADIO) {
		if (!active) return;
		if (type == BS_AUTORADIOBUTTON) {
			for (Wnd *o : radio_group(c)) if (o != c) set_check(o, 0);
			c->check = 1;
		} else set_check(c, c->check);		// BS_RADIOBUTTON: the program decides
	} else if (type == BS_AUTOCHECKBOX) c->check = active;
	else if (type == BS_AUTO3STATE) set_check(c, (c->check + 1) % 3);
	else set_check(c, c->check);			// BS_CHECKBOX / BS_3STATE
	notify(c, BN_CLICKED);
}
static gboolean canvas_draw(GtkWidget *wd, cairo_t *cr, gpointer p) {
	Wnd *c = (Wnd *)p;
	int w = gtk_widget_get_allocated_width(wd), h = gtk_widget_get_allocated_height(wd);
	int type = c->style & 0x1F;
	if (c->kind == K_CANVAS && c->cls == L"Static" && type >= SS_BLACKRECT && type <= SS_WHITEFRAME) {
		double g = (type == SS_BLACKRECT || type == SS_BLACKFRAME) ? 0 : (type == SS_GRAYRECT || type == SS_GRAYFRAME) ? 0.5 : 1;
		cairo_set_source_rgb(cr, g, g, g);
		if (type <= SS_WHITERECT) cairo_paint(cr);
		else { cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1); cairo_set_line_width(cr, 1); cairo_stroke(cr); }
	}
	Wnd *was = painting;
	painting = c;
	bool owner = (c->cls == L"Button" && (c->style & 0xF) == BS_OWNERDRAW) || (c->cls == L"Static" && type == SS_OWNERDRAW);
	if (owner && c->parent) {
		DRAWITEMSTRUCT di = {};
		di.CtlType = c->cls == L"Button" ? 4 : 5;	// ODT_BUTTON / ODT_STATIC
		di.CtlID = c->id;
		di.itemAction = 1;				// ODA_DRAWENTIRE
		di.hwndItem = hwnd(c);
		di.hDC = window_dc(c);
		di.rcItem = {0, 0, w, h};
		send(c->parent, WM_DRAWITEM, (WPARAM)c->id, (LPARAM)&di);
		ReleaseDC(hwnd(c), di.hDC);
	}
	if (c->wndproc != control_proc) {		// a subclassed control may paint itself
		c->dirty = false;
		send(c, WM_PAINT, 0, 0);
	}
	painting = was;
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	if (c->backing) {
		cairo_set_source_surface(cr, c->backing, 0, 0);
		cairo_paint(cr);
	}
	return TRUE;
}
static gboolean canvas_mouse(GtkWidget *wd, GdkEvent *e, gpointer p) {
	Wnd *c = (Wnd *)p;
	double x, y;
	gdk_event_get_coords(e, &x, &y);
	LPARAM lp = MAKELPARAM((int)x, (int)y);
	switch (e->type) {
	case GDK_MOTION_NOTIFY: send(c, WM_MOUSEMOVE, 0, lp); break;
	case GDK_BUTTON_PRESS: {
		guint b = ((GdkEventButton *)e)->button;
		send(c, b == 1 ? WM_LBUTTONDOWN : b == 3 ? WM_RBUTTONDOWN : WM_MBUTTONDOWN, 0, lp);
		if (b == 1 && c->cls == L"Button") notify(c, BN_CLICKED);
		break;
	}
	case GDK_BUTTON_RELEASE: {
		guint b = ((GdkEventButton *)e)->button;
		send(c, b == 1 ? WM_LBUTTONUP : b == 3 ? WM_RBUTTONUP : WM_MBUTTONUP, 0, lp);
		break;
	}
	default: break;
	}
	return FALSE;
}
static void list_changed(GtkTreeSelection *, gpointer p) {
	Wnd *c = (Wnd *)p;
	if (!c->block && (c->kind != K_LIST || (c->style & LBS_NOTIFY))) notify(c, LBN_SELCHANGE);
}
static void list_activated(GtkTreeView *, GtkTreePath *, GtkTreeViewColumn *, gpointer p) {
	Wnd *c = (Wnd *)p;
	if (!c->block && (c->style & LBS_NOTIFY)) notify(c, LBN_DBLCLK);
}
static void lv_toggled(GtkCellRendererToggle *, gchar *path, gpointer p) {
	Wnd *c = (Wnd *)p;
	GtkTreeIter it;
	if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(c->store), &it, path)) return;
	gboolean v;
	gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, 0, &v, -1);
	gtk_list_store_set(c->store, &it, 0, !v, -1);
}
static void scale_changed(GtkRange *r, gpointer p) {
	Wnd *c = (Wnd *)p;
	c->pos = (int)gtk_range_get_value(r);
	if (c->block || !c->parent) return;
	bool vert = c->kind == K_TRACKBAR ? (c->style & 2) : (c->style & 1);
	send(c->parent, vert ? WM_VSCROLL : WM_HSCROLL, MAKEWPARAM(c->kind == K_TRACKBAR ? 5 : SB_THUMBPOSITION, c->pos), (LPARAM)hwnd(c));
}

static std::wstring label_text(Wnd *c) {
	std::wstring r;
	for (size_t i = 0; i < c->text.size(); i++) {
		if (c->text[i] == L'&') { if (i + 1 < c->text.size() && c->text[i + 1] == L'&') { r += L'&'; i++; } }
		else if (c->text[i] != L'\r') r += c->text[i];
	}
	return r;
}

static void make_control(Wnd *d, const Item &it, const Tmpl &t) {
	Wnd *c = new_wnd();
	c->parent = d;
	c->cls = it.cls;
	c->text = it.text;
	c->id = it.id;
	c->style = it.style;
	c->exstyle = it.ex;
	c->wndproc = control_proc;
	c->enabled = !(it.style & WS_DISABLED);
	c->visible = (it.style & WS_VISIBLE) != 0;
	int x = it.x * bx(t) / 4, y = it.y * by(t) / 8, w = it.cx * bx(t) / 4, h = it.cy * by(t) / 8;
	c->rect = {x, y, x + w, y + h};
	std::wstring cls = it.cls;
	for (auto &ch : cls) ch = towlower(ch);
	int type = it.style & 0xF;
	GtkWidget *g = NULL;
	if (cls == L"button") {
		if (type == BS_GROUPBOX) {
			c->kind = K_GROUP;
			g = gtk_frame_new(utf8(label_text(c)).c_str());
		} else if (type == BS_OWNERDRAW) {
			c->kind = K_CANVAS;
		} else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE) {
			c->kind = K_CHECK;
			g = c->inner = gtk_check_button_new_with_mnemonic(mnemonic(it.text, true).c_str());
			g_signal_connect(g, "toggled", G_CALLBACK(on_toggled), c);
		} else if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {
			c->kind = K_RADIO;
			c->dummy = gtk_radio_button_new(NULL);
			g_object_ref_sink(c->dummy);
			g = c->inner = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(c->dummy), mnemonic(it.text, true).c_str());
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->dummy), TRUE);
			g_signal_connect(g, "toggled", G_CALLBACK(on_toggled), c);
		} else {
			c->kind = K_BUTTON;
			g = c->inner = gtk_button_new_with_mnemonic(mnemonic(it.text, true).c_str());
			if (type == BS_DEFPUSHBUTTON) d->defid = it.id;
			g_signal_connect(g, "clicked", G_CALLBACK(on_clicked), c);
		}
	} else if (cls == L"static") {
		int st = it.style & 0x1F;
		if (st == SS_LEFT || st == SS_CENTER || st == SS_RIGHT || st == SS_SIMPLE || st == SS_LEFTNOWORDWRAP) {
			c->kind = K_LABEL;
			g = c->inner = gtk_label_new(utf8(label_text(c)).c_str());
			gtk_label_set_xalign(GTK_LABEL(g), st == SS_CENTER ? 0.5 : st == SS_RIGHT ? 1.0 : 0.0);
			gtk_label_set_yalign(GTK_LABEL(g), (it.style & SS_CENTERIMAGE) ? 0.5 : 0.0);
			// SS_CENTERIMAGE text is one line on Windows; other text wraps
			if (st != SS_LEFTNOWORDWRAP && st != SS_SIMPLE && !(it.style & SS_CENTERIMAGE)) gtk_label_set_line_wrap(GTK_LABEL(g), TRUE);
			else gtk_label_set_ellipsize(GTK_LABEL(g), PANGO_ELLIPSIZE_END);
		} else if (st == 0x10 || st == 0x11 || st == 0x12) {
			c->kind = K_LABEL;
			g = gtk_separator_new(st == 0x11 ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
		} else c->kind = K_CANVAS;
	} else if (cls == L"edit") {
		c->kind = K_EDIT;
		if (it.style & ES_MULTILINE) {
			g = gtk_scrolled_window_new(NULL, NULL);
			c->inner = gtk_text_view_new();
			gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(c->inner), (it.style & WS_HSCROLL) ? GTK_WRAP_NONE : GTK_WRAP_WORD_CHAR);
			gtk_text_view_set_monospace(GTK_TEXT_VIEW(c->inner), TRUE);
			gtk_text_view_set_editable(GTK_TEXT_VIEW(c->inner), !(it.style & ES_READONLY));
			gtk_container_add(GTK_CONTAINER(g), c->inner);
			g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->inner)), "changed", G_CALLBACK(+[](GtkTextBuffer *, gpointer p) {
				Wnd *c = (Wnd *)p; if (!c->block) notify(c, EN_CHANGE);
			}), c);
		} else {
			g = c->inner = gtk_entry_new();
			gtk_entry_set_width_chars(GTK_ENTRY(g), 1);
			gtk_editable_set_editable(GTK_EDITABLE(g), !(it.style & ES_READONLY));
			gtk_entry_set_alignment(GTK_ENTRY(g), (it.style & ES_RIGHT) ? 1.0 : (it.style & ES_CENTER) ? 0.5 : 0.0);
			if (it.style & ES_PASSWORD) gtk_entry_set_visibility(GTK_ENTRY(g), FALSE);
			g_signal_connect(g, "changed", G_CALLBACK(+[](GtkEditable *, gpointer p) {
				Wnd *c = (Wnd *)p; if (!c->block) notify(c, EN_CHANGE);
			}), c);
		}
		g_signal_connect(c->inner, "focus-out-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
			notify((Wnd *)p, EN_KILLFOCUS); return FALSE;
		}), c);
		if (!it.text.empty()) { c->block++; SetWindowText(hwnd(c), it.text.c_str()); c->block--; }
	} else if (cls == L"combobox") {
		c->kind = K_COMBO;
		c->store = gtk_list_store_new(1, G_TYPE_STRING);
		bool entry = (it.style & 3) == CBS_DROPDOWN || (it.style & 3) == 1;
		g = c->inner = entry ? gtk_combo_box_new_with_model_and_entry(GTK_TREE_MODEL(c->store)) : gtk_combo_box_new_with_model(GTK_TREE_MODEL(c->store));
		if (entry) gtk_combo_box_set_entry_text_column(GTK_COMBO_BOX(g), 0);
		else {
			GtkCellRenderer *r = gtk_cell_renderer_text_new();
			gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(g), r, TRUE);
			gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(g), r, "text", 0);
		}
		g_signal_connect(g, "changed", G_CALLBACK(+[](GtkComboBox *, gpointer p) {
			Wnd *c = (Wnd *)p; if (!c->block) notify(c, CBN_SELCHANGE);
		}), c);
		h = -1;
	} else if (cls == L"listbox" || cls == L"syslistview32") {
		bool lv = cls == L"syslistview32";
		c->kind = lv ? K_LISTVIEW : K_LIST;
		GType types[9] = {G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING};
		c->store = gtk_list_store_newv(9, types);
		g = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(g), GTK_SHADOW_IN);
		c->inner = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c->store));
		gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(c->inner), lv);
		if (!lv) {
			GtkCellRenderer *r = gtk_cell_renderer_text_new();
			g_object_set(r, "ypad", 0, NULL);
			gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(c->inner), -1, "", r, "text", 1, NULL);
			if (it.style & LBS_USETABSTOPS) g_object_set(r, "family", "monospace", NULL);
		}
		gtk_container_add(GTK_CONTAINER(g), c->inner);
		GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->inner));
		gtk_tree_selection_set_mode(sel, (it.style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
		g_signal_connect(sel, "changed", G_CALLBACK(list_changed), c);
		g_signal_connect(c->inner, "row-activated", G_CALLBACK(list_activated), c);
	} else if (cls == L"scrollbar" || cls == L"msctls_trackbar32") {
		bool tb = cls == L"msctls_trackbar32";
		c->kind = tb ? K_TRACKBAR : K_SCROLL;
		bool vert = tb ? (it.style & 2) : (it.style & 1);
		GtkAdjustment *a = gtk_adjustment_new(0, 0, 100, 1, 10, 0);
		g = c->inner = tb ? gtk_scale_new(vert ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, a)
			: gtk_scrollbar_new(vert ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, a);
		if (tb) { gtk_scale_set_draw_value(GTK_SCALE(g), FALSE); gtk_range_set_round_digits(GTK_RANGE(g), 0); }
		g_signal_connect(g, "value-changed", G_CALLBACK(scale_changed), c);
	} else {
		c->kind = K_LABEL;
		g = gtk_label_new(utf8(c->text).c_str());
	}
	if (c->kind == K_CANVAS) {
		g = gtk_drawing_area_new();
		gtk_widget_add_events(g, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
		g_signal_connect(g, "draw", G_CALLBACK(canvas_draw), c);
		g_signal_connect(g, "motion-notify-event", G_CALLBACK(canvas_mouse), c);
		g_signal_connect(g, "button-press-event", G_CALLBACK(canvas_mouse), c);
		g_signal_connect(g, "button-release-event", G_CALLBACK(canvas_mouse), c);
	}
	c->widget = g;
	gtk_widget_set_size_request(g, std::max(1, w), h < 0 ? -1 : std::max(1, h));
	gtk_fixed_put(GTK_FIXED(d->fixed), g, x, y);
	gtk_widget_set_sensitive(g, c->enabled);
	if (c->visible) gtk_widget_show_all(g);
	else gtk_widget_set_no_show_all(g, TRUE);
	d->children.push_back(c);
}

static gboolean dialog_draw(GtkWidget *, cairo_t *cr, gpointer p) {
	Wnd *d = (Wnd *)p;
	if (d->dirty) {
		d->dirty = false;
		Wnd *was = painting;
		painting = d;
		send(d, WM_PAINT, 0, 0);
		painting = was;
	}
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	if (d->backing) {
		cairo_set_source_surface(cr, d->backing, 0, 0);
		cairo_paint(cr);
	}
	return FALSE;
}
static gboolean dialog_key(GtkWidget *top, GdkEventKey *e, gpointer p) {
	Wnd *d = (Wnd *)p;
	key_event(e, e->type == GDK_KEY_PRESS);
	if (e->type != GDK_KEY_PRESS) return FALSE;
	if (e->keyval == GDK_KEY_Escape) {
		send(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(hwnd(d), IDCANCEL));
		return TRUE;
	}
	if (e->keyval == GDK_KEY_Return || e->keyval == GDK_KEY_KP_Enter) {
		GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(top));
		if (f && (GTK_IS_TEXT_VIEW(f) || GTK_IS_BUTTON(f))) return FALSE;
		HWND def = GetDlgItem(hwnd(d), d->defid);
		Wnd *dc = wnd(def);
		if (dc && !dc->enabled) return TRUE;
		send(d, WM_COMMAND, MAKEWPARAM(d->defid, BN_CLICKED), (LPARAM)def);
		return TRUE;
	}
	return FALSE;
}

static Wnd *create_dialog(const Tmpl &t, const FurbResources *res, HWND parent, DLGPROC proc, LPARAM lp, bool modal) {
	Wnd *d = new_wnd();
	d->kind = K_DIALOG;
	d->cls = L"#32770";
	d->text = t.caption;
	d->style = t.style;
	d->exstyle = t.ex;
	d->dlgproc = proc;
	d->wndproc = dialog_proc;
	d->res = res;
	d->modal = modal;
	Wnd *owner = toplevel(wnd(parent));
	if (!owner) owner = the_main;
	d->parent = owner;
	int w = t.cx * bx(t) / 4, h = t.cy * by(t) / 8;
	POINT o = owner ? origin(owner) : POINT{0, 0};
	d->rect = {o.x + t.x * bx(t) / 4, o.y + t.y * by(t) / 8, 0, 0};
	d->rect.right = d->rect.left + w;
	d->rect.bottom = d->rect.top + h;
	d->top = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_style_context_add_class(gtk_widget_get_style_context(d->top), "furb-dialog");
	gtk_window_set_title(GTK_WINDOW(d->top), utf8(t.caption).c_str());
	gtk_window_set_resizable(GTK_WINDOW(d->top), (t.style & WS_THICKFRAME) != 0);
	if (owner && owner->top) gtk_window_set_transient_for(GTK_WINDOW(d->top), GTK_WINDOW(owner->top));
	gtk_window_set_type_hint(GTK_WINDOW(d->top), GDK_WINDOW_TYPE_HINT_DIALOG);
	if (modal) {
		gtk_window_set_modal(GTK_WINDOW(d->top), TRUE);
		gtk_window_set_position(GTK_WINDOW(d->top), GTK_WIN_POS_CENTER_ON_PARENT);
	}
	GtkWidget *overlay = gtk_overlay_new();
	gtk_container_add(GTK_CONTAINER(d->top), overlay);
	d->widget = gtk_drawing_area_new();
	gtk_widget_set_size_request(d->widget, w, h);
	gtk_container_add(GTK_CONTAINER(overlay), d->widget);
	d->fixed = gtk_fixed_new();
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->fixed);
	g_signal_connect(d->widget, "draw", G_CALLBACK(dialog_draw), d);
	g_signal_connect(d->top, "key-press-event", G_CALLBACK(dialog_key), d);
	g_signal_connect(d->top, "key-release-event", G_CALLBACK(dialog_key), d);
	g_signal_connect(d->top, "delete-event", G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer p) -> gboolean {
		send((Wnd *)p, WM_CLOSE, 0, 0);
		return TRUE;
	}), d);
	g_signal_connect(d->top, "configure-event", G_CALLBACK((+[](GtkWidget *, GdkEventConfigure *e, gpointer p) -> gboolean {
		Wnd *d = (Wnd *)p;
		int w = d->rect.right - d->rect.left;
		int h = d->rect.bottom - d->rect.top;
		if (e->width != w || e->height != h) {
			d->rect.right = d->rect.left + e->width;
			d->rect.bottom = d->rect.top + e->height;
			send(d, WM_SIZE, 0, MAKELPARAM(e->width, e->height));
		}
		return FALSE;
	})), d);
	// group boxes first, so they are drawn behind the controls they frame
	for (const Item &it : t.items) if (it.cls == L"Button" && (it.style & 0xF) == BS_GROUPBOX) make_control(d, it, t);
	std::vector<Wnd *> groups = d->children;
	d->children.clear();
	for (const Item &it : t.items) if (!(it.cls == L"Button" && (it.style & 0xF) == BS_GROUPBOX)) make_control(d, it, t);
	// restore the template's order (radio groups and tab order depend on it)
	std::vector<Wnd *> ordered;
	size_t gi = 0, ci = 0;
	for (const Item &it : t.items)
		ordered.push_back(it.cls == L"Button" && (it.style & 0xF) == BS_GROUPBOX ? groups[gi++] : d->children[ci++]);
	d->children = ordered;
	gtk_widget_show(overlay);
	gtk_widget_show(d->widget);
	gtk_widget_show(d->fixed);
	HWND focus = NULL;
	for (Wnd *c : d->children) if ((c->style & WS_TABSTOP) && c->enabled && c->visible) { focus = hwnd(c); break; }
	if (proc && proc(hwnd(d), WM_INITDIALOG, (WPARAM)focus, lp) && focus) SetFocus(focus);
	if (d->dead) return NULL;
	if (t.style & WS_VISIBLE || modal) {
		if (!d->ended) { gtk_widget_show(d->top); d->visible = true; }
	}
	return d;
}

static LRESULT CALLBACK dialog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	Wnd *d = wnd(h);
	if (!d) return 0;
	INT_PTR r = d->dlgproc ? d->dlgproc(h, m, wp, lp) : 0;
	if (r) return m == WM_INITDIALOG || m == WM_DRAWITEM ? r : (d->msgresult ? d->msgresult : r);
	switch (m) {
	case WM_CLOSE:
		send(d, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM)GetDlgItem(h, IDCANCEL));
		return 0;
	case WM_SETTEXT: case WM_GETTEXT: case WM_GETTEXTLENGTH:
		return DefWindowProc(h, m, wp, lp);
	case DM_SETDEFID: d->defid = (int)wp; return TRUE;
	}
	return 0;
}

static INT_PTR modal_loop(Wnd *d) {
	if (!d) return -1;
	while (!d->ended && !d->dead) {
		MSG m;
		if (!GetMessage(&m, NULL, 0, 0)) { PostQuitMessage((int)m.wParam); break; }
		TranslateMessage(&m);
		DispatchMessage(&m);
	}
	INT_PTR r = d->result;
	DestroyWindow(hwnd(d));
	return r;
}

INT_PTR DialogBoxParam(HINSTANCE hinst, LPCTSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
	return sync([&]() -> INT_PTR {
		Tmpl t;
		const FurbResources *r = res_of(hinst);
		if (!from_rc(r, res_id(tmpl), t)) { fprintf(stderr, "furb: no dialog %d\n", res_id(tmpl)); return -1; }
		return modal_loop(create_dialog(t, r, parent, proc, lp, true));
	});
}
INT_PTR DialogBoxIndirectParam(HINSTANCE hinst, LPCDLGTEMPLATE tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
	return sync([&]() -> INT_PTR {
		Tmpl t;
		if (!from_memory(tmpl, t)) return -1;
		return modal_loop(create_dialog(t, res_of(hinst), parent, proc, lp, true));
	});
}
HWND CreateDialogParam(HINSTANCE hinst, LPCTSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
	return sync([&]() -> HWND {
		Tmpl t;
		const FurbResources *r = res_of(hinst);
		if (!from_rc(r, res_id(tmpl), t)) return NULL;
		return hwnd(create_dialog(t, r, parent, proc, lp, false));
	});
}
HWND CreateDialogIndirectParam(HINSTANCE hinst, LPCDLGTEMPLATE tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
	return sync([&]() -> HWND {
		Tmpl t;
		if (!from_memory(tmpl, t)) return NULL;
		return hwnd(create_dialog(t, res_of(hinst), parent, proc, lp, false));
	});
}
BOOL EndDialog(HWND h, INT_PTR r) {
	return sync([&]() -> BOOL {
		Wnd *d = wnd(h);
		if (!d) return FALSE;
		d->ended = true;
		d->result = r;
		if (d->top) gtk_widget_hide(d->top);
		g_main_context_wakeup(NULL);
		return TRUE;
	});
}
HWND GetDlgItem(HWND h, int id) {
	return sync([&]() -> HWND {
		Wnd *d = wnd(h);
		if (!d) return NULL;
		for (Wnd *c : d->children) if (c->id == id && !c->dead) return hwnd(c);
		return NULL;
	});
}
int GetDlgCtrlID(HWND h) { Wnd *w = wnd(h); return w ? w->id : 0; }
BOOL SetDlgItemText(HWND h, int id, LPCTSTR s) { HWND c = GetDlgItem(h, id); return c ? SetWindowText(c, s) : FALSE; }
UINT GetDlgItemText(HWND h, int id, LPTSTR s, int n) { HWND c = GetDlgItem(h, id); return c ? GetWindowText(c, s, n) : (s && n ? (s[0] = 0) : 0); }
BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sign) {
	wchar_t b[32];
	swprintf(b, 32, sign ? L"%d" : L"%u", v);
	return SetDlgItemText(h, id, b);
}
UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sign) {
	wchar_t b[64] = {0};
	GetDlgItemText(h, id, b, 64);
	wchar_t *e;
	long v = sign ? wcstol(b, &e, 10) : (long)wcstoul(b, &e, 10);
	if (ok) *ok = e != b;
	return (UINT)v;
}
BOOL CheckDlgButton(HWND h, int id, UINT c) { return SendDlgItemMessage(h, id, BM_SETCHECK, c, 0), TRUE; }
UINT IsDlgButtonChecked(HWND h, int id) { return (UINT)SendDlgItemMessage(h, id, BM_GETCHECK, 0, 0); }
BOOL CheckRadioButton(HWND h, int first, int last, int check) {
	for (int i = first; i <= last; i++) SendDlgItemMessage(h, i, BM_SETCHECK, i == check, 0);
	return TRUE;
}
LRESULT SendDlgItemMessage(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) {
	return sync([&] { return send(wnd(GetDlgItem(h, id)), m, wp, lp); });
}

// -------------------------------------------------------------- control messages
static std::wstring store_text(GtkListStore *s, int i, int col) {
	GtkTreeIter it;
	if (i < 0 || !gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(s), &it, NULL, i)) return L"";
	gchar *t = NULL;
	gtk_tree_model_get(GTK_TREE_MODEL(s), &it, col, &t, -1);
	std::wstring r = wide(t ? t : "");
	g_free(t);
	return r;
}
static int store_count(GtkListStore *s) { return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(s), NULL); }
static std::string list_utf8(const wchar_t *s) {	// tab stops become spaces
	std::wstring r;
	for (; s && *s; s++) r += *s == L'\t' ? std::wstring(L"    ") : std::wstring(1, *s);
	return utf8(r);
}
static int list_sel(Wnd *c) {
	if (c->kind == K_COMBO) return gtk_combo_box_get_active(GTK_COMBO_BOX(c->inner));
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->inner));
	GList *rows = gtk_tree_selection_get_selected_rows(sel, NULL);
	int r = rows ? gtk_tree_path_get_indices((GtkTreePath *)rows->data)[0] : -1;
	g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
	return r;
}
static void list_set_sel(Wnd *c, int i) {
	c->block++;
	if (c->kind == K_COMBO) gtk_combo_box_set_active(GTK_COMBO_BOX(c->inner), i);
	else {
		GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->inner));
		gtk_tree_selection_unselect_all(sel);
		if (i >= 0 && i < store_count(c->store)) {
			GtkTreePath *p = gtk_tree_path_new_from_indices(i, -1);
			gtk_tree_selection_select_path(sel, p);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->inner), p, NULL, FALSE, 0, 0);
			gtk_tree_path_free(p);
		}
	}
	c->block--;
}
static int list_insert(Wnd *c, int at, const wchar_t *s) {
	int n = store_count(c->store);
	if (at < 0 || at > n) at = n;
	if ((c->kind == K_COMBO && (c->style & CBS_SORT)) || (c->kind == K_LIST && (c->style & LBS_SORT)))
		for (at = 0; at < n && wcscasecmp(store_text(c->store, at, c->kind == K_COMBO ? 0 : 1).c_str(), s) <= 0; at++) {}
	GtkTreeIter it;
	c->block++;
	gtk_list_store_insert(c->store, &it, at);
	gtk_list_store_set(c->store, &it, c->kind == K_COMBO ? 0 : 1, list_utf8(s).c_str(), -1);
	c->block--;
	c->itemdata.insert(c->itemdata.begin() + std::min<size_t>(at, c->itemdata.size()), 0);
	return at;
}
static void list_delete(Wnd *c, int i) {
	GtkTreeIter it;
	if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c->store), &it, NULL, i)) return;
	c->block++;
	gtk_list_store_remove(c->store, &it);
	c->block--;
	if (i < (int)c->itemdata.size()) c->itemdata.erase(c->itemdata.begin() + i);
}
static LRESULT copy_text(const std::wstring &t, WPARAM n, LPARAM buf) {
	if (!buf) return (LRESULT)t.size();
	if (n == 0) return 0;
	size_t k = std::min<size_t>(t.size(), n - 1);
	wmemcpy((wchar_t *)buf, t.c_str(), k);
	((wchar_t *)buf)[k] = 0;
	return (LRESULT)k;
}
static std::wstring widget_text(Wnd *c) {
	if (c->kind == K_EDIT) {
		if (GTK_IS_ENTRY(c->inner)) return wide(gtk_entry_get_text(GTK_ENTRY(c->inner)));
		GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->inner));
		GtkTextIter a, e;
		gtk_text_buffer_get_bounds(b, &a, &e);
		gchar *t = gtk_text_buffer_get_text(b, &a, &e, FALSE);
		std::wstring r;
		for (wchar_t ch : wide(t)) { if (ch == L'\n') r += L'\r'; r += ch; }
		g_free(t);
		return r;
	}
	if (c->kind == K_COMBO && gtk_combo_box_get_has_entry(GTK_COMBO_BOX(c->inner)))
		return wide(gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(c->inner)))));
	if (c->kind == K_COMBO) return store_text(c->store, list_sel(c), 0);
	return c->text;
}
static void lv_set(Wnd *c, const LVITEM *li, int item) {
	GtkTreeIter it;
	if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c->store), &it, NULL, item)) return;
	if ((li->mask & LVIF_TEXT) && li->pszText && li->iSubItem < 8)
		gtk_list_store_set(c->store, &it, 1 + li->iSubItem, utf8(li->pszText).c_str(), -1);
}

LRESULT CALLBACK control_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
	Wnd *c = wnd(h);
	if (!c) return 0;
	switch (m) {
	case WM_SETTEXT: SetWindowText(h, (LPCTSTR)lp); return TRUE;
	case WM_GETTEXT: return copy_text(widget_text(c), wp, lp);
	case WM_GETTEXTLENGTH: return (LRESULT)widget_text(c).size();
	case WM_SETREDRAW: case WM_SETFONT: return 0;
	case WM_PAINT: return 0;
	}
	switch (c->kind) {
	case K_CHECK: case K_RADIO: case K_BUTTON:
		if (m == BM_GETCHECK) return c->check;
		if (m == BM_SETCHECK) { set_check(c, (int)wp); return 0; }
		if (m == 0xF5) { notify(c, BN_CLICKED); return 0; }	// BM_CLICK
		break;
	case K_EDIT: {
		bool tv = GTK_IS_TEXT_VIEW(c->inner);
		switch (m) {
		case EM_SETLIMITTEXT: c->limit = (int)wp; if (!tv) gtk_entry_set_max_length(GTK_ENTRY(c->inner), (gint)wp); return 0;
		case EM_SETREADONLY:
			if (tv) gtk_text_view_set_editable(GTK_TEXT_VIEW(c->inner), !wp);
			else gtk_editable_set_editable(GTK_EDITABLE(c->inner), !wp);
			return TRUE;
		case EM_SETSEL:
			if (tv) {
				GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->inner));
				GtkTextIter a, e;
				gtk_text_buffer_get_end_iter(b, &e);
				int n = gtk_text_iter_get_offset(&e);
				int s0 = (int)wp < 0 ? n : std::min((int)wp, n), s1 = (int)lp < 0 ? n : std::min((int)lp, n);
				gtk_text_buffer_get_iter_at_offset(b, &a, s0);
				gtk_text_buffer_get_iter_at_offset(b, &e, s1);
				gtk_text_buffer_select_range(b, &a, &e);
			} else gtk_editable_select_region(GTK_EDITABLE(c->inner), (gint)wp, (gint)lp);
			return 0;
		case EM_REPLACESEL: {
			std::string t = gtext(lp ? (const wchar_t *)lp : L"");
			c->block++;
			if (tv) {
				GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->inner));
				gtk_text_buffer_delete_selection(b, TRUE, TRUE);
				gtk_text_buffer_insert_at_cursor(b, t.c_str(), -1);
				GtkTextMark *mk = gtk_text_buffer_get_insert(b);
				gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(c->inner), mk);
			} else {
				gint s, e;
				gtk_editable_get_selection_bounds(GTK_EDITABLE(c->inner), &s, &e);
				gtk_editable_delete_text(GTK_EDITABLE(c->inner), s, e);
				gint p = s;
				gtk_editable_insert_text(GTK_EDITABLE(c->inner), t.c_str(), -1, &p);
			}
			c->block--;
			return 0;
		}
		case EM_GETLINECOUNT:
			return tv ? gtk_text_buffer_get_line_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->inner))) : 1;
		case EM_SCROLLCARET: case EM_LINESCROLL: return TRUE;
		}
		break;
	}
	case K_COMBO: case K_LIST: {
		bool cb = c->kind == K_COMBO;
		int col = cb ? 0 : 1;
		switch (m) {
		case CB_ADDSTRING: case LB_ADDSTRING:
			if ((m == CB_ADDSTRING) != cb) break;
			return list_insert(c, -1, lp ? (const wchar_t *)lp : L"");
		case CB_INSERTSTRING: case LB_INSERTSTRING:
			if ((m == CB_INSERTSTRING) != cb) break;
			return list_insert(c, (int)wp, lp ? (const wchar_t *)lp : L"");
		case 0x144: case LB_DELETESTRING:	// CB_DELETESTRING
			if ((m == 0x144) != cb) break;
			list_delete(c, (int)wp);
			return store_count(c->store);
		case CB_RESETCONTENT: case LB_RESETCONTENT:
			if ((m == CB_RESETCONTENT) != cb) break;
			c->block++; gtk_list_store_clear(c->store); c->block--;
			c->itemdata.clear();
			return 0;
		case CB_SETCURSEL: case LB_SETCURSEL:
			if ((m == CB_SETCURSEL) != cb) break;
			list_set_sel(c, (int)wp);
			return (int)wp < store_count(c->store) ? (LRESULT)wp : -1;
		case CB_GETCURSEL: case LB_GETCURSEL:
			if ((m == CB_GETCURSEL) != cb) break;
			return list_sel(c);
		case CB_GETCOUNT: case LB_GETCOUNT:
			if ((m == CB_GETCOUNT) != cb) break;
			return store_count(c->store);
		case CB_SETITEMDATA: case LB_SETITEMDATA:
			if ((m == CB_SETITEMDATA) != cb) break;
			if (wp < c->itemdata.size()) { c->itemdata[wp] = lp; return TRUE; }
			return -1;
		case CB_GETITEMDATA: case LB_GETITEMDATA:
			if ((m == CB_GETITEMDATA) != cb) break;
			return wp < c->itemdata.size() ? c->itemdata[wp] : -1;
		case CB_FINDSTRINGEXACT: case LB_FINDSTRINGEXACT: {
			if ((m == CB_FINDSTRINGEXACT) != cb) break;
			std::string s = list_utf8((const wchar_t *)lp);
			int n = store_count(c->store);
			for (int k = 0; k < n; k++) {
				int i = ((int)wp + 1 + k) % n;
				if (!strcasecmp(utf8(store_text(c->store, i, col)).c_str(), s.c_str())) return i;
			}
			return -1;
		}
		case CB_GETLBTEXT: case LB_GETTEXT: {
			if ((m == CB_GETLBTEXT) != cb) break;
			std::wstring t = store_text(c->store, (int)wp, col);
			if (lp) wcscpy((wchar_t *)lp, t.c_str());
			return (LRESULT)t.size();
		}
		case 0x149: case LB_GETTEXTLEN:		// CB_GETLBTEXTLEN
			if ((m == 0x149) != cb) break;
			return (LRESULT)store_text(c->store, (int)wp, col).size();
		case LB_SETTOPINDEX: case LB_GETTOPINDEX: case LB_SETTABSTOPS: case LB_SETHORIZONTALEXTENT: case CB_SETDROPPEDWIDTH:
			return 0;
		case LB_SETSEL: {
			if (cb) break;
			GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->inner));
			c->block++;
			if ((int)lp < 0) { if (wp) gtk_tree_selection_select_all(sel); else gtk_tree_selection_unselect_all(sel); }
			else {
				GtkTreePath *p = gtk_tree_path_new_from_indices((int)lp, -1);
				if (wp) gtk_tree_selection_select_path(sel, p); else gtk_tree_selection_unselect_path(sel, p);
				gtk_tree_path_free(p);
			}
			c->block--;
			return 0;
		}
		case LB_GETSEL: {
			if (cb) break;
			GtkTreePath *p = gtk_tree_path_new_from_indices((int)wp, -1);
			bool s = gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(c->inner)), p);
			gtk_tree_path_free(p);
			return s;
		}
		}
		break;
	}
	case K_TRACKBAR: case K_SCROLL: {
		GtkRange *r = GTK_RANGE(c->inner);
		auto apply = [&] {
			c->block++;
			gtk_range_set_range(r, c->lo, std::max(c->lo + 1, c->hi + (c->kind == K_SCROLL ? std::max(c->page, 1) : 0)));
			if (c->kind == K_SCROLL) gtk_adjustment_set_page_size(gtk_range_get_adjustment(r), std::max(c->page, 1));
			gtk_range_set_value(r, c->pos);
			c->block--;
		};
		switch (m) {
		case TBM_SETRANGE: c->lo = (short)LOWORD(lp); c->hi = (short)HIWORD(lp); apply(); return 0;
		case TBM_SETRANGEMIN: c->lo = (int)lp; apply(); return 0;
		case TBM_SETRANGEMAX: c->hi = (int)lp; apply(); return 0;
		case TBM_GETRANGEMIN: return c->lo;
		case TBM_GETRANGEMAX: return c->hi;
		case TBM_SETPOS: c->pos = (int)lp; apply(); return 0;
		case TBM_GETPOS: return c->pos;
		case TBM_SETTICFREQ: case TBM_SETPAGESIZE: return 0;
		}
		break;
	}
	case K_LISTVIEW:
		switch (m) {
		case LVM_INSERTCOLUMN: {
			const LVCOLUMN *col = (const LVCOLUMN *)lp;
			int i = c->columns++;
			if (i == 0 && c->checkboxes) {
				GtkCellRenderer *t = gtk_cell_renderer_toggle_new();
				g_signal_connect(t, "toggled", G_CALLBACK(lv_toggled), c);
				gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(c->inner), -1, "", t, "active", 0, NULL);
			}
			GtkCellRenderer *r = gtk_cell_renderer_text_new();
			if (col && (col->fmt & 3) == LVCFMT_CENTER) g_object_set(r, "xalign", 0.5, NULL);
			GtkTreeViewColumn *tc = gtk_tree_view_column_new_with_attributes(col && col->pszText ? utf8(col->pszText).c_str() : "", r, "text", 1 + i, NULL);
			if (col && (col->mask & LVCF_WIDTH)) gtk_tree_view_column_set_fixed_width(tc, col->cx * 7 / 5);
			gtk_tree_view_column_set_resizable(tc, TRUE);
			gtk_tree_view_append_column(GTK_TREE_VIEW(c->inner), tc);
			return i;
		}
		case LVM_SETEXTENDEDLISTVIEWSTYLE:
			if ((lp & LVS_EX_CHECKBOXES) && !c->checkboxes) {
				c->checkboxes = true;
				GtkCellRenderer *t = gtk_cell_renderer_toggle_new();
				g_signal_connect(t, "toggled", G_CALLBACK(lv_toggled), c);
				gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(c->inner), 0, "", t, "active", 0, NULL);
			}
			gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(c->inner), (lp & LVS_EX_GRIDLINES) ? GTK_TREE_VIEW_GRID_LINES_BOTH : GTK_TREE_VIEW_GRID_LINES_NONE);
			return 0;
		case LVM_SETVIEW: return 1;
		case LVM_INSERTITEM: {
			const LVITEM *li = (const LVITEM *)lp;
			int n = store_count(c->store), at = li ? std::min(std::max(li->iItem, 0), n) : n;
			GtkTreeIter it;
			gtk_list_store_insert(c->store, &it, at);
			gtk_list_store_set(c->store, &it, 0, FALSE, -1);
			if (li) { LVITEM x = *li; x.iSubItem = 0; lv_set(c, &x, at); }
			return at;
		}
		case LVM_SETITEM: { const LVITEM *li = (const LVITEM *)lp; if (li) lv_set(c, li, li->iItem); return TRUE; }
		case LVM_SETITEMTEXT: { LVITEM x = *(const LVITEM *)lp; x.mask |= LVIF_TEXT; lv_set(c, &x, (int)wp); return TRUE; }
		case LVM_SETITEMSTATE: {
			const LVITEM *li = (const LVITEM *)lp;
			GtkTreeIter it;
			if (li && (li->stateMask & LVIS_STATEIMAGEMASK) && gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c->store), &it, NULL, (int)wp))
				gtk_list_store_set(c->store, &it, 0, (li->state >> 12) == 2, -1);
			return TRUE;
		}
		case LVM_GETITEMSTATE: {
			GtkTreeIter it;
			gboolean v = FALSE;
			if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c->store), &it, NULL, (int)wp))
				gtk_tree_model_get(GTK_TREE_MODEL(c->store), &it, 0, &v, -1);
			return (LRESULT)((v ? 2u : 1u) << 12) & lp;
		}
		case LVM_DELETEALLITEMS: gtk_list_store_clear(c->store); return TRUE;
		case LVM_GETITEMCOUNT: return store_count(c->store);
		case LVM_GETNEXTITEM: {
			int s = list_sel(c);
			return s > (int)wp ? s : -1;
		}
		}
		break;
	default: break;
	}
	return 0;
}

// ================================================================ window API
HWND CreateWindowEx(DWORD, LPCTSTR cls, LPCTSTR name, DWORD style, int, int, int, int, HWND, HMENU menu, HINSTANCE, LPVOID) {
	return sync([&]() -> HWND {
		auto it = classes.find(cls ? cls : L"");
		if (it == classes.end()) return NULL;
		Wnd *w = create_main(it->second, name, style, menu);
		send(w, WM_CREATE, 0, 0);
		return hwnd(w);
	});
}
BOOL DestroyWindow(HWND h) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w) return FALSE;
		send(w, WM_DESTROY, 0, 0);
		for (Wnd *c : w->children) c->dead = true;
		w->dead = true;
		if (w->top) gtk_widget_destroy(w->top);
		else if (w->widget) gtk_widget_destroy(w->widget);
		if (w == the_main) the_main = NULL;
		g_main_context_wakeup(NULL);
		return TRUE;
	});
}
BOOL ShowWindow(HWND h, int cmd) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w) return FALSE;
		BOOL was = w->visible;
		GtkWidget *g = w->top ? w->top : w->widget;
		if (!g) return was;
		switch (cmd) {
		case SW_HIDE: gtk_widget_hide(g); w->visible = false; break;
		case SW_MINIMIZE: if (w->top) gtk_window_iconify(GTK_WINDOW(w->top)); break;
		case SW_MAXIMIZE: if (w->top) { gtk_widget_show(g); gtk_window_maximize(GTK_WINDOW(w->top)); } w->visible = true; break;
		case SW_RESTORE:
			if (w->top) { gtk_window_deiconify(GTK_WINDOW(w->top)); gtk_window_unmaximize(GTK_WINDOW(w->top)); }
			// fall through
		default:
			gtk_widget_set_no_show_all(g, FALSE);
			if (w->top) gtk_widget_show(g); else gtk_widget_show_all(g);
			w->visible = true;
		}
		return was;
	});
}
BOOL UpdateWindow(HWND h) { invalidate(wnd(h)); return TRUE; }
BOOL InvalidateRect(HWND h, const RECT *, BOOL) {
	Wnd *w = wnd(h);
	if (!w) return FALSE;
	if (w->kind == K_MAIN) post(h, WM_PAINT, 0, 0);
	invalidate(w);
	return TRUE;
}
BOOL RedrawWindow(HWND h, const RECT *r, HRGN, UINT) { return InvalidateRect(h, r, TRUE); }

static void place(Wnd *w) {
	int cw = client_w(w), ch = client_h(w);
	if (w->kind == K_MAIN) {
		int mh = 0;
		if (w->menubar && gtk_widget_get_visible(w->menubar)) gtk_widget_get_preferred_height(w->menubar, NULL, &mh);
		gtk_window_resize(GTK_WINDOW(w->top), std::max(1, cw), std::max(1, ch) + mh);
		gtk_window_move(GTK_WINDOW(w->top), w->rect.left, w->rect.top);
	} else if (w->kind == K_DIALOG) {
		gtk_widget_set_size_request(w->widget, cw, ch);
		gtk_window_resize(GTK_WINDOW(w->top), std::max(1, cw), std::max(1, ch));
		gtk_window_move(GTK_WINDOW(w->top), w->rect.left, w->rect.top);
	} else if (w->widget && w->parent && w->parent->fixed) {
		gtk_fixed_move(GTK_FIXED(w->parent->fixed), w->widget, w->rect.left, w->rect.top);
		gtk_widget_set_size_request(w->widget, std::max(1, cw), std::max(1, ch));
	}
}
BOOL SetWindowPos(HWND h, HWND, int x, int y, int cx, int cy, UINT f) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w) return FALSE;
		if (!(f & SWP_NOMOVE)) { int cw = client_w(w), ch = client_h(w); w->rect = {x, y, x + cw, y + ch}; }
		if (!(f & SWP_NOSIZE)) { w->rect.right = w->rect.left + cx; w->rect.bottom = w->rect.top + cy; }
		if (!(f & (SWP_NOMOVE | SWP_NOSIZE)) || !(f & SWP_NOSIZE) || !(f & SWP_NOMOVE)) place(w);
		if (f & SWP_HIDEWINDOW) ShowWindow(h, SW_HIDE);
		if (f & SWP_SHOWWINDOW) ShowWindow(h, SW_SHOW);
		return TRUE;
	});
}
BOOL MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL) {
	return SetWindowPos(h, NULL, x, y, cx, cy, SWP_NOZORDER);
}
BOOL GetWindowRect(HWND h, RECT *r) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w || !r) return FALSE;
		POINT o = origin(w);
		*r = {o.x, o.y, o.x + client_w(w), o.y + client_h(w)};
		return TRUE;
	});
}
BOOL GetClientRect(HWND h, RECT *r) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w || !r) return FALSE;
		*r = {0, 0, client_w(w), client_h(w)};
		return TRUE;
	});
}
BOOL AdjustWindowRect(RECT *, DWORD, BOOL) { return TRUE; }
BOOL AdjustWindowRectEx(RECT *, DWORD, BOOL, DWORD) { return TRUE; }
BOOL ScreenToClient(HWND h, POINT *p) {
	return sync([&]() -> BOOL { Wnd *w = wnd(h); if (!w || !p) return FALSE; POINT o = origin(w); p->x -= o.x; p->y -= o.y; return TRUE; });
}
BOOL ClientToScreen(HWND h, POINT *p) {
	return sync([&]() -> BOOL { Wnd *w = wnd(h); if (!w || !p) return FALSE; POINT o = origin(w); p->x += o.x; p->y += o.y; return TRUE; });
}
BOOL SetWindowText(HWND h, LPCTSTR s) {
	std::wstring t = s ? s : L"";
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w) return FALSE;
		w->text = t;
		if (w->top) gtk_window_set_title(GTK_WINDOW(w->top), utf8(t).c_str());
		else if (w->inner || w->widget) {
			w->block++;
			switch (w->kind) {
			case K_LABEL: if (GTK_IS_LABEL(w->inner)) gtk_label_set_text(GTK_LABEL(w->inner), utf8(label_text(w)).c_str()); break;
			case K_BUTTON: case K_CHECK: case K_RADIO:
				gtk_button_set_label(GTK_BUTTON(w->inner), mnemonic(t, true).c_str());
				gtk_button_set_use_underline(GTK_BUTTON(w->inner), TRUE);
				break;
			case K_GROUP: gtk_frame_set_label(GTK_FRAME(w->widget), utf8(label_text(w)).c_str()); break;
			case K_EDIT:
				if (GTK_IS_ENTRY(w->inner)) gtk_entry_set_text(GTK_ENTRY(w->inner), utf8(t).c_str());
				else gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->inner)), gtext(t).c_str(), -1);
				break;
			case K_COMBO:
				if (gtk_combo_box_get_has_entry(GTK_COMBO_BOX(w->inner)))
					gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(w->inner))), utf8(t).c_str());
				break;
			default: break;
			}
			w->block--;
			if (w->kind == K_EDIT) notify(w, EN_CHANGE);	// as Windows does for WM_SETTEXT
		}
		return TRUE;
	});
}
int GetWindowText(HWND h, LPTSTR s, int n) {
	return sync([&]() -> int {
		Wnd *w = wnd(h);
		if (!w) { if (s && n) s[0] = 0; return 0; }
		return (int)copy_text(w->top ? w->text : widget_text(w), n, (LPARAM)s);
	});
}
int GetWindowTextLength(HWND h) {
	return sync([&]() -> int { Wnd *w = wnd(h); return w ? (int)(w->top ? w->text : widget_text(w)).size() : 0; });
}
LONG_PTR SetWindowLongPtr(HWND h, int i, LONG_PTR v) {
	return sync([&]() -> LONG_PTR {
		Wnd *w = wnd(h);
		if (!w) return 0;
		LONG_PTR old = 0;
		switch (i) {
		case GWLP_WNDPROC: old = (LONG_PTR)w->wndproc; w->wndproc = (WNDPROC)v; break;
		case GWLP_USERDATA: old = w->userdata; w->userdata = v; break;
		case DWLP_USER: old = w->dlguser; w->dlguser = v; break;
		case 0: old = w->msgresult; w->msgresult = v; break;	// DWLP_MSGRESULT
		case GWL_STYLE:
			old = w->style; w->style = (DWORD)v;
			if (w->kind == K_MAIN && w->menubar) gtk_widget_set_visible(w->menubar, !(v & WS_POPUP));
			break;
		case GWL_EXSTYLE: old = w->exstyle; w->exstyle = (DWORD)v; break;
		}
		return old;
	});
}
LONG_PTR GetWindowLongPtr(HWND h, int i) {
	return sync([&]() -> LONG_PTR {
		Wnd *w = wnd(h);
		if (!w) return 0;
		switch (i) {
		case GWLP_WNDPROC: return (LONG_PTR)w->wndproc;
		case GWLP_USERDATA: return w->userdata;
		case DWLP_USER: return w->dlguser;
		case 0: return w->msgresult;
		case GWL_STYLE: return (LONG_PTR)w->style | (w->visible ? WS_VISIBLE : 0) | (w->enabled ? 0 : WS_DISABLED);
		case GWL_EXSTYLE: return w->exstyle;
		case -12: return w->id;	// GWL_ID
		}
		return 0;
	});
}
LONG SetWindowLong(HWND h, int i, LONG v) { return (LONG)SetWindowLongPtr(h, i, v); }
LONG GetWindowLong(HWND h, int i) { return (LONG)GetWindowLongPtr(h, i); }
BOOL IsWindow(HWND h) { return wnd(h) != NULL; }
BOOL IsWindowVisible(HWND h) { Wnd *w = wnd(h); return w && w->visible; }
BOOL IsIconic(HWND) { return FALSE; }
BOOL IsZoomed(HWND h) {
	return sync([&]() -> BOOL { Wnd *w = wnd(h); return w && w->top && gtk_window_is_maximized(GTK_WINDOW(w->top)); });
}
HWND SetFocus(HWND h) {
	return sync([&]() -> HWND {
		Wnd *w = wnd(h);
		if (w && (w->inner || w->widget)) gtk_widget_grab_focus(w->inner ? w->inner : w->widget);
		return NULL;
	});
}
static Wnd *active(void) {
	std::lock_guard<std::mutex> l(live_lock);
	for (Wnd *w : live)
		if (!w->dead && w->top && gtk_window_is_active(GTK_WINDOW(w->top))) return w;
	return NULL;
}
HWND GetFocus(void) { return sync([] { return hwnd(active()); }); }
HWND GetActiveWindow(void) { return sync([] { return hwnd(active()); }); }
HWND GetForegroundWindow(void) { return sync([] { return hwnd(active()); }); }
BOOL SetForegroundWindow(HWND h) {
	return sync([&]() -> BOOL { Wnd *w = wnd(h); if (w && w->top) gtk_window_present(GTK_WINDOW(w->top)); return w != NULL; });
}
HWND SetActiveWindow(HWND h) { SetForegroundWindow(h); return NULL; }
BOOL BringWindowToTop(HWND h) { return SetForegroundWindow(h); }
BOOL EnableWindow(HWND h, BOOL e) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w) return FALSE;
		BOOL was = !w->enabled;
		w->enabled = e;
		GtkWidget *g = w->top ? w->top : w->widget;
		if (g) gtk_widget_set_sensitive(g, e);
		return was;
	});
}
BOOL IsWindowEnabled(HWND h) { Wnd *w = wnd(h); return w && w->enabled; }
HWND GetParent(HWND h) { Wnd *w = wnd(h); return w ? hwnd(w->parent) : NULL; }
HWND GetDesktopWindow(void) { return NULL; }
int GetSystemMetrics(int i) {
	return sync([&]() -> int {
		GdkRectangle r = {0, 0, 1920, 1080};
		GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
		if (!m) m = gdk_display_get_monitor(gdk_display_get_default(), 0);
		if (m) gdk_monitor_get_workarea(m, &r);
		switch (i) {
		case SM_CXSCREEN: case SM_CXVIRTUALSCREEN: return r.width;
		case SM_CYSCREEN: case SM_CYVIRTUALSCREEN: return r.height;
		case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN: return 0;
		case SM_CYCAPTION: case SM_CYMENU: return 0;
		case SM_CXFRAME: case SM_CYFRAME: return 0;
		}
		return 0;
	});
}
BOOL GetWindowPlacement(HWND h, WINDOWPLACEMENT *p) {
	if (!p) return FALSE;
	GetWindowRect(h, &p->rcNormalPosition);
	p->showCmd = SW_SHOWNORMAL;
	return TRUE;
}
BOOL SetWindowPlacement(HWND h, const WINDOWPLACEMENT *p) {
	if (!p) return FALSE;
	const RECT &r = p->rcNormalPosition;
	return SetWindowPos(h, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}
HMONITOR MonitorFromWindow(HWND, DWORD) { return (HMONITOR)1; }
BOOL GetMonitorInfo(HMONITOR, MONITORINFO *mi) {
	if (!mi) return FALSE;
	mi->rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
	mi->rcWork = mi->rcMonitor;
	mi->dwFlags = 1;
	return TRUE;
}
int SetScrollInfo(HWND h, int bar, const SCROLLINFO *si, BOOL) {
	return sync([&]() -> int {
		Wnd *w = wnd(h);
		if (!w || !si || w->kind != K_SCROLL || bar != SB_CTL) return 0;
		if (si->fMask & SIF_RANGE) { w->lo = si->nMin; w->hi = si->nMax; }
		if (si->fMask & SIF_PAGE) w->page = si->nPage;
		if (si->fMask & SIF_POS) w->pos = si->nPos;
		GtkRange *r = GTK_RANGE(w->inner);
		w->block++;
		gtk_range_set_range(r, w->lo, std::max(w->lo + 1, w->hi + 1));
		gtk_adjustment_set_page_size(gtk_range_get_adjustment(r), std::max(w->page, 1));
		gtk_range_set_value(r, w->pos);
		w->block--;
		return w->pos;
	});
}
BOOL GetScrollInfo(HWND h, int, SCROLLINFO *si) {
	return sync([&]() -> BOOL {
		Wnd *w = wnd(h);
		if (!w || !si) return FALSE;
		si->nMin = w->lo; si->nMax = w->hi; si->nPage = w->page; si->nPos = si->nTrackPos = w->pos;
		return TRUE;
	});
}
int SetScrollPos(HWND h, int bar, int pos, BOOL redraw) {
	SCROLLINFO si = {sizeof si, SIF_POS, 0, 0, 0, pos, 0};
	return SetScrollInfo(h, bar, &si, redraw);
}
int GetScrollPos(HWND h, int) { Wnd *w = wnd(h); return w ? w->pos : 0; }
BOOL SetScrollRange(HWND h, int bar, int lo, int hi, BOOL redraw) {
	SCROLLINFO si = {sizeof si, SIF_RANGE, lo, hi, 0, 0, 0};
	return SetScrollInfo(h, bar, &si, redraw) >= 0;
}
BOOL ShowScrollBar(HWND, int, BOOL) { return TRUE; }

// ================================================================ message boxes, files, shell
int MessageBox(HWND h, LPCTSTR text, LPCTSTR caption, UINT type) {
	std::wstring t = text ? text : L"", c = caption ? caption : L"";
	fprintf(stderr, "furb: [%s] %s\n", utf8(c).c_str(), utf8(t).c_str());
	return sync([&]() -> int {
		Wnd *o = toplevel(wnd(h));
		if (!o) o = the_main;
		UINT kind = type & 0xF, icon = type & 0xF0;
		GtkMessageType mt = icon == MB_ICONERROR ? GTK_MESSAGE_ERROR : icon == MB_ICONWARNING ? GTK_MESSAGE_WARNING
			: icon == MB_ICONQUESTION ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO;
		GtkWidget *d = gtk_message_dialog_new(o && o->top ? GTK_WINDOW(o->top) : NULL, GTK_DIALOG_MODAL, mt, GTK_BUTTONS_NONE, "%s", utf8(t).c_str());
		gtk_window_set_title(GTK_WINDOW(d), utf8(c).c_str());
		switch (kind) {
		case MB_OKCANCEL: gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", IDCANCEL, "_OK", IDOK, NULL); break;
		case MB_YESNO: gtk_dialog_add_buttons(GTK_DIALOG(d), "_No", IDNO, "_Yes", IDYES, NULL); break;
		case MB_YESNOCANCEL: gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", IDCANCEL, "_No", IDNO, "_Yes", IDYES, NULL); break;
		case 5: gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", IDCANCEL, "_Retry", IDRETRY, NULL); break;
		case 2: gtk_dialog_add_buttons(GTK_DIALOG(d), "_Abort", IDABORT, "_Retry", IDRETRY, "_Ignore", IDIGNORE, NULL); break;
		default: gtk_dialog_add_buttons(GTK_DIALOG(d), "_OK", IDOK, NULL);
		}
		int r = gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(d);
		if (r < 0) r = kind == MB_OK ? IDOK : kind == MB_YESNO ? IDNO : IDCANCEL;
		return r;
	});
}

static BOOL file_dialog(OPENFILENAME *o, bool save) {
	return sync([&]() -> BOOL {
		Wnd *w = toplevel(wnd(o->hwndOwner));
		if (!w) w = the_main;
		GtkWidget *d = gtk_file_chooser_dialog_new(o->lpstrTitle ? utf8(o->lpstrTitle).c_str() : (save ? "Save" : "Open"),
			w && w->top ? GTK_WINDOW(w->top) : NULL, save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
			"_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, NULL);
		GtkFileChooser *fc = GTK_FILE_CHOOSER(d);
		if (save && (o->Flags & OFN_OVERWRITEPROMPT)) gtk_file_chooser_set_do_overwrite_confirmation(fc, TRUE);
		// "Description\0*.a;*.b\0...\0\0": case-insensitive, as on Windows
		std::vector<GtkFileFilter *> filters;
		for (const wchar_t *f = o->lpstrFilter; f && *f;) {
			std::wstring desc = f; f += desc.size() + 1;
			if (!*f) break;
			std::wstring pats = f; f += pats.size() + 1;
			GtkFileFilter *ff = gtk_file_filter_new();
			gtk_file_filter_set_name(ff, utf8(desc).c_str());
			auto *list = new std::vector<std::string>;
			size_t p = 0;
			for (;;) {
				size_t q = pats.find(L';', p);
				std::string pat = utf8(pats.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p));
				if (!pat.empty()) list->push_back(pat);
				if (q == std::wstring::npos) break;
				p = q + 1;
			}
			gtk_file_filter_add_custom(ff, GTK_FILE_FILTER_DISPLAY_NAME, [](const GtkFileFilterInfo *i, gpointer p) -> gboolean {
				for (auto &pat : *(std::vector<std::string> *)p)
					if (!fnmatch(pat.c_str(), i->display_name, FNM_CASEFOLD)) return TRUE;
				return FALSE;
			}, list, [](gpointer p) { delete (std::vector<std::string> *)p; });
			gtk_file_chooser_add_filter(fc, ff);
			filters.push_back(ff);
		}
		if (o->nFilterIndex >= 1 && o->nFilterIndex <= filters.size()) gtk_file_chooser_set_filter(fc, filters[o->nFilterIndex - 1]);
		if (o->lpstrInitialDir && *o->lpstrInitialDir) {
			std::string dir = utf8(o->lpstrInitialDir);
			for (auto &ch : dir) if (ch == '\\') ch = '/';
			gtk_file_chooser_set_current_folder(fc, dir.c_str());
		}
		if (o->lpstrFile && *o->lpstrFile) {
			std::string f = utf8(o->lpstrFile);
			for (auto &ch : f) if (ch == '\\') ch = '/';
			if (save) {
				size_t s = f.find_last_of('/');
				if (s != std::string::npos) { gtk_file_chooser_set_current_folder(fc, f.substr(0, s).c_str()); f = f.substr(s + 1); }
				gtk_file_chooser_set_current_name(fc, f.c_str());
			} else gtk_file_chooser_set_filename(fc, f.c_str());
		}
		BOOL ok = FALSE;
		if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
			gchar *fn = gtk_file_chooser_get_filename(fc);
			if (fn) {
				std::wstring path = wide(fn);
				g_free(fn);
				size_t slash = path.find_last_of(L'/'), dot = path.find_last_of(L'.');
				if (save && o->lpstrDefExt && *o->lpstrDefExt && (dot == std::wstring::npos || dot < slash))
					path += std::wstring(L".") + o->lpstrDefExt, dot = path.find_last_of(L'.');
				if (o->lpstrFile && o->nMaxFile > path.size()) {
					wcscpy(o->lpstrFile, path.c_str());
					o->nFileOffset = (WORD)(slash + 1);
					o->nFileExtension = (WORD)(dot == std::wstring::npos || dot < slash ? path.size() : dot + 1);
					GtkFileFilter *cur = gtk_file_chooser_get_filter(fc);
					for (size_t i = 0; i < filters.size(); i++) if (filters[i] == cur) o->nFilterIndex = (DWORD)i + 1;
					if (o->lpstrFileTitle && o->nMaxFileTitle) wcsncpy(o->lpstrFileTitle, path.c_str() + slash + 1, o->nMaxFileTitle - 1);
					ok = TRUE;
				}
			}
		}
		gtk_widget_destroy(d);
		return ok;
	});
}
BOOL GetOpenFileName(OPENFILENAME *o) { return o ? file_dialog(o, false) : FALSE; }
BOOL GetSaveFileName(OPENFILENAME *o) { return o ? file_dialog(o, true) : FALSE; }

HINSTANCE ShellExecute(HWND, LPCTSTR, LPCTSTR file, LPCTSTR, LPCTSTR, int) {
	if (!file) return NULL;
	std::string f = utf8(file);
	for (auto &ch : f) if (ch == '\\') ch = '/';
	gchar *uri = strstr(f.c_str(), "://") ? g_strdup(f.c_str()) : g_filename_to_uri(f.c_str(), NULL, NULL);
	if (uri) {
		sync([&] { gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL); });
		g_free(uri);
	}
	return (HINSTANCE)33;
}
void DragAcceptFiles(HWND, BOOL) {}
UINT DragQueryFile(HDROP d, UINT i, LPTSTR s, UINT n) {
	Drop *dr = (Drop *)d;
	if (!dr) return 0;
	if (i == 0xFFFFFFFF) return (UINT)dr->files.size();
	if (i >= dr->files.size()) return 0;
	const std::wstring &f = dr->files[i];
	if (s && n) { wcsncpy(s, f.c_str(), n - 1); s[n - 1] = 0; }
	return (UINT)f.size();
}
void DragFinish(HDROP d) { delete (Drop *)d; }

// ---- resources, cursor
int LoadString(HINSTANCE hinst, UINT id, LPTSTR buf, int n) {
	const FurbResources *r = res_of(hinst);
	for (int i = 0; i < r->nstrings; i++)
		if (r->strings[i].id == (int)id) {
			if (buf && n > 0) { wcsncpy(buf, r->strings[i].s, n - 1); buf[n - 1] = 0; }
			return (int)wcslen(r->strings[i].s);
		}
	if (buf && n > 0) buf[0] = 0;
	return 0;
}
HICON LoadIcon(HINSTANCE, LPCTSTR name) { return (HICON)(uintptr_t)(res_id(name) + 1); }
HCURSOR LoadCursor(HINSTANCE, LPCTSTR name) { return (HCURSOR)(uintptr_t)res_id(name); }
BOOL DestroyCursor(HCURSOR) { return TRUE; }
HMODULE GetModuleHandle(LPCTSTR) { return (HMODULE)&furb_module_resources; }
HCURSOR SetCursor(HCURSOR c) {
	return sync([&]() -> HCURSOR {
		Wnd *w = the_main;
		if (!w) return NULL;
		HCURSOR old = (HCURSOR)(uintptr_t)w->cursor;
		int want = (int)(uintptr_t)c;
		if (want != w->cursor) { w->cursor = want; set_pointer(w); }
		return old;
	});
}
int ShowCursor(BOOL show) {
	return sync([&]() -> int {
		int before = cursor_count;
		cursor_count += show ? 1 : -1;
		if ((before < 0) != (cursor_count < 0) && the_main) set_pointer(the_main);
		return cursor_count;
	});
}
BOOL ClipCursor(const RECT *) { return TRUE; }
BOOL GetCursorPos(POINT *p) {
	if (!p) return FALSE;
	Wnd *m = the_main;
	POINT o = m ? origin(m) : POINT{0, 0};
	p->x = o.x + mouse_x;
	p->y = o.y + mouse_y;
	return TRUE;
}
BOOL SetCursorPos(int, int) { return TRUE; }
HWND SetCapture(HWND) { return NULL; }
BOOL ReleaseCapture(void) { return TRUE; }
BOOL RegisterHotKey(HWND, int, UINT, UINT) { return FALSE; }
BOOL UnregisterHotKey(HWND, int) { return TRUE; }
HRESULT SHGetFolderPath(HWND, int, HANDLE, DWORD, LPTSTR path) {
	extern std::wstring furb_data_parent;
	wcscpy(path, furb_data_parent.c_str());
	return S_OK;
}
float furb_mic_level(void) { return 0; }

// ================================================================ threads
struct Thread { std::thread t; };
void Sleep(DWORD ms) {
	if (!ms) { std::this_thread::yield(); return; }
	struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
HANDLE CreateThread(void *, SIZE_T, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD *id) {
	static std::atomic<DWORD> next(1);
	if (id) *id = next++;
	std::thread t([fn, arg] { fn(arg); });
	t.detach();
	return (HANDLE)1;
}
DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_OBJECT_0; }
BOOL TerminateThread(HANDLE, DWORD) { return FALSE; }
void ExitThread(DWORD) { pthread_exit(NULL); }
