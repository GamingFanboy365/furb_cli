// The GUI's Win32 layer: internal interfaces shared by gui/*.cpp.
//
// Furbtendulator runs unmodified: its own WinMain, window procedure, dialog
// procedures and emulation thread.  What Windows would supply is supplied
// here on GTK 3 (windows, menus, dialogs and their controls, GDI) and SDL 2
// (sound, joysticks):
//   user32.cpp  windows, message queue, menus, accelerators, dialogs, controls,
//               message boxes, file pickers, cursor, keyboard state, threads
//   gdi.cpp     device contexts, bitmaps, brushes, fonts, text (cairo/pango)
//   dx.cpp      DirectDraw (in-memory surfaces shown in the main window),
//               DirectSound (SDL audio), DirectInput (keyboard/mouse from the
//               windows' events, joysticks from SDL)
//   main.cpp    paths, settings file, command line; calls _tWinMain
//
// Threading follows Win32: GTK objects belong to the main thread, and a
// window call made on the emulation thread runs on the main thread while the
// caller waits -- as a cross-thread SendMessage does -- whenever the main
// thread is in its message loop (GetMessage/PeekMessage, or any GTK loop).
// PostMessage, key state, frame presentation and audio never wait.
#pragma once
#include <windows.h>
#include <ddraw.h>
#include <dinput.h>
#include <dsound.h>
#include <gtk/gtk.h>
#include "furb_rc.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gui {

// ---- threads
bool on_main(void);
void run_on_main(const std::function<void()> &f);	// synchronous
template <class F> auto sync(F f) -> decltype(f()) {
	if (on_main()) return f();
	typedef decltype(f()) R;
	if constexpr (std::is_void<R>::value) run_on_main(f);
	else {
		R r{};
		run_on_main([&] { r = f(); });
		return r;
	}
}
void later(const std::function<void()> &f);		// asynchronous, on the main thread

// ---- windows
enum Kind { K_MAIN, K_DIALOG, K_BUTTON, K_CHECK, K_RADIO, K_GROUP, K_LABEL, K_CANVAS, K_EDIT, K_COMBO,
	K_LIST, K_SCROLL, K_TRACKBAR, K_LISTVIEW, K_OTHER };
struct Menu;
struct Wnd {
	uint32_t magic = 0x46555257;		// 'FURW'
	Kind kind = K_OTHER;
	std::wstring cls, text;
	DWORD style = 0, exstyle = 0;
	int id = 0;
	Wnd *parent = NULL;			// controls: their dialog; dialogs: owner
	std::vector<Wnd *> children;
	WNDPROC wndproc = NULL;			// who SendMessage calls
	DLGPROC dlgproc = NULL;
	LONG_PTR userdata = 0, dlguser = 0, msgresult = 0;
	RECT rect = {0, 0, 0, 0};		// pixels: toplevels on "screen", controls in their dialog
	bool visible = false, enabled = true, dead = false;
	const FurbResources *res = NULL;
	Menu *menu = NULL;
	// GTK
	GtkWidget *top = NULL;			// GtkWindow (main window, dialogs)
	GtkWidget *widget = NULL;		// the control's widget, or the toplevel's client area
	GtkWidget *fixed = NULL;		// dialogs: where controls live
	GtkWidget *menubar = NULL;
	GtkWidget *inner = NULL;		// the part a control's state lives in (entry, text view, tree view, scale...)
	GtkWidget *dummy = NULL;		// radio buttons: the hidden group partner used to un-check
	GtkListStore *store = NULL;		// list boxes, list views
	int block = 0;				// > 0 while we change a widget: its signals are not user actions
	// control state
	std::vector<LPARAM> itemdata;
	int check = 0;
	int lo = 0, hi = 100, pos = 0, page = 0;	// trackbars and scroll bars
	int limit = 0;
	int defid = IDOK;
	int columns = 0;
	bool checkboxes = false;
	// modal loop
	bool modal = false, ended = false;
	INT_PTR result = 0;
	// GDI: what BeginPaint/GetDC draw into, shown by the widget's draw handler
	cairo_surface_t *backing = NULL;
	bool dirty = true;
	int cursor = 0;				// main window: IDC_ARROW / IDC_CROSS
};
Wnd *wnd(HWND h);				// the window, or NULL if h is not a live window
inline HWND hwnd(Wnd *w) { return (HWND)w; }
Wnd *main_window(void);
cairo_surface_t *backing_for(Wnd *w);		// sized to the window's client area
void invalidate(Wnd *w);			// redraw soon (any thread)
LRESULT send(Wnd *w, UINT msg, WPARAM wp, LPARAM lp);

// ---- menus
struct MenuItem {
	std::wstring text;
	UINT id = 0, flags = 0;			// MF_CHECKED, MF_GRAYED, MF_POPUP, MF_SEPARATOR, and radio (0x200)
	Menu *sub = NULL;
	GtkWidget *w = NULL, *mark = NULL;
};
struct Menu {
	uint32_t magic = 0x4655524D;		// 'FURM'
	std::vector<MenuItem> items;
	Wnd *owner = NULL;
	GtkWidget *gtk = NULL;
};
GtkWidget *menu_widget(Menu *m, Wnd *owner, bool bar);
bool menu_enabled(Menu *m, UINT id);

// ---- keyboard, mouse (updated from every window's events)
extern std::atomic<unsigned char> dik_state[256];	// DirectInput scan codes, 0x80 = down
extern std::atomic<unsigned char> vk_state[256];	// virtual keys, 0x80 = down
extern std::atomic<int> mouse_x, mouse_y;		// main window client coordinates
extern std::atomic<int> mouse_dx, mouse_dy, mouse_dz;	// accumulated for DirectInput
extern std::atomic<unsigned char> mouse_buttons;
void key_event(GdkEventKey *e, bool down);		// updates the state; returns nothing
void clear_keys(void);
UINT vk_of(GdkEventKey *e);

// ---- video (dx.cpp): the frame the main window shows
void present(const uint32_t *px, int w, int h, int pitch_px);
bool draw_frame(cairo_t *cr, int w, int h);		// false if there is no frame
void set_fullscreen(bool on);

// ---- GDI (gdi.cpp)
extern std::recursive_mutex gdi_lock;
HDC window_dc(Wnd *w);					// a DC drawing into w's backing surface
extern thread_local Wnd *painting;			// the window whose draw handler is running

std::string utf8(const wchar_t *s);
std::string utf8(const std::wstring &s);
std::wstring wide(const char *s);
}
