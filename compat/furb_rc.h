// Compiled resources (menus, accelerators, dialog templates, strings), as
// gui/rc2cpp.py emits them from a module's .rc script.  Each module -- the
// program and each mapper pack -- links its own table as
// 'furb_module_resources'; its address is the module's HINSTANCE, which is
// how LoadMenu / DialogBox / LoadString find the right module's resources.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct FurbRcMenuItem {
	const wchar_t *text;	// "&File", "&Open...\tCtrl+O"
	int id;			// command (0 for popups and separators)
	unsigned flags;		// MF_POPUP 0x10, MF_SEPARATOR 0x800, MF_CHECKED 8, MF_GRAYED 1
	int nchild;
	const FurbRcMenuItem *child;
};
struct FurbRcMenu { int id; int n; const FurbRcMenuItem *items; };

struct FurbRcAccel { unsigned key, flags; int cmd; };	// flags: FVIRTKEY 1, FSHIFT 4, FCONTROL 8, FALT 0x10
struct FurbRcAccelTable { int id; int n; const FurbRcAccel *a; };

struct FurbRcControl {
	const wchar_t *cls;	// "Button", "Static", "Edit", "ComboBox", "ListBox", "ScrollBar", "msctls_trackbar32", ...
	const wchar_t *text;
	int id;
	int x, y, cx, cy;	// dialog units
	uint32_t style, exstyle;
};
struct FurbRcDialog {
	int id;
	const wchar_t *caption;
	int x, y, cx, cy;
	uint32_t style, exstyle;
	const wchar_t *font;
	int fontsize;
	int menu;
	int n;
	const FurbRcControl *controls;
};
struct FurbRcString { int id; const wchar_t *s; };

struct FurbResources {
	const FurbRcMenu *menus; int nmenus;
	const FurbRcAccelTable *accels; int naccels;
	const FurbRcDialog *dialogs; int ndialogs;
	const FurbRcString *strings; int nstrings;
};
extern "C" const FurbResources furb_module_resources;
