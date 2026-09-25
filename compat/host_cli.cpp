// furb_cli's host services: headless dialogs (a dialog furb_cli.cpp scripted
// runs its real dialog procedure against fake controls, any other is
// cancelled), file pickers answered from a queue, a scripted cursor and
// microphone, a capture-only DirectSound for --wav/--avi, and every other
// window-system call from winapi.def as an inert no-op.  No display, no
// DirectDraw, no DirectInput (furb_cli.cpp sets the input state itself).
#include <windows.h>
#include <ddraw.h>
#include <dinput.h>
#include <dsound.h>
#include "furb_host.h"
#include <deque>
#include <map>
#include <set>
#include <vector>

#define W(ret, name, params, args) ret name params { return furb_zero<ret>(); }
#define H(ret, name, params, args)
#define WR W
#define HR H
#include "winapi.def"
#undef W
#undef H
#undef WR
#undef HR

namespace FurbHost {
// ---- headless dialogs ----
struct FakeDlg;
struct FakeCtl { FakeDlg *dlg; int id; };
struct FakeDlg {
	int tmpl; DLGPROC proc;
	std::map<int, std::wstring> text;
	std::map<int, int> check, pos;
	std::map<int, FakeCtl *> ctls;
	bool ended = false; INT_PTR result = 0;
};
static std::set<void *> dialogs, controls;
static std::map<int, DialogScript> scripts;
static std::map<int, FakeDlg *> modeless_by_tmpl;
static std::deque<std::wstring> file_queue;

static FakeDlg *as_dlg(HWND h) { return dialogs.count(h) ? (FakeDlg *)h : NULL; }
static FakeCtl *as_ctl(HWND h) { return controls.count(h) ? (FakeCtl *)h : NULL; }
static int tmpl_id(LPCTSTR t) { return (uintptr_t)t < 0x10000 ? (int)(uintptr_t)t : -1; }

void on_dialog(int id, DialogScript s) { scripts[id] = s; }
HWND modeless(int id) { auto it = modeless_by_tmpl.find(id); return it == modeless_by_tmpl.end() ? NULL : (HWND)it->second; }
void click(HWND h, int id) {
	FakeDlg *d = as_dlg(h);
	if (d && !d->ended) d->proc(h, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(h, id));
}
void set_text(HWND h, int id, const std::wstring &t) { if (FakeDlg *d = as_dlg(h)) d->text[id] = t; }
void set_check(HWND h, int id, int c) { if (FakeDlg *d = as_dlg(h)) d->check[id] = c; }
void set_pos(HWND h, int id, int p) { if (FakeDlg *d = as_dlg(h)) d->pos[id] = p; }
std::wstring get_text(HWND h, int id) { FakeDlg *d = as_dlg(h); return d ? d->text[id] : L""; }
void queue_file(const std::wstring &p) { file_queue.push_back(p); }

static FakeDlg *open_dialog(int id, DLGPROC proc, LPARAM lp) {
	FakeDlg *d = new FakeDlg;
	d->tmpl = id; d->proc = proc;
	dialogs.insert(d);
	proc((HWND)d, WM_INITDIALOG, 0, lp);
	return d;
}
static void close_dialog(FakeDlg *d) {
	for (auto &c : d->ctls) { controls.erase(c.second); delete c.second; }
	dialogs.erase(d);
	delete d;
}

// ---- cursor / mic / audio ----
static int cur_x = 255, cur_y = 255, client_w = 256, client_h = 240;
static float mic = 0;
static AudioSink audio_sink;
void set_cursor(int x, int y) { cur_x = x; cur_y = y; }
void get_cursor(int &x, int &y) { x = cur_x; y = cur_y; }
void set_client(int w, int h) { client_w = w; client_h = h; }
void set_mic(float level) { mic = level; }
void set_audio_sink(AudioSink s) { audio_sink = s; }
} // namespace FurbHost

using namespace FurbHost;


// ---------------------------------------------------------------- dialogs API
int MessageBox(HWND, LPCTSTR text, LPCTSTR caption, UINT type) {
	// the missing display/sound/input devices are expected: only say so with --verbose
	bool expected = text && (wcsstr(text, L"DirectDraw") || wcsstr(text, L"DirectInput") || wcsstr(text, L"DirectSound"));
	if (!quiet && (verbose || !expected))
		fprintf(stderr, "furb_cli: [%s] %s\n", furb_narrow(caption ? caption : L"").c_str(), furb_narrow(text ? text : L"").c_str());
	UINT kind = type & 0xF;
	return (kind == MB_YESNO || kind == MB_YESNOCANCEL) ? IDNO : IDOK;	// never agree to anything unasked
}
INT_PTR DialogBoxParam(HINSTANCE, LPCTSTR t, HWND, DLGPROC proc, LPARAM lp) {
	int id = tmpl_id(t);
	auto s = scripts.find(id);
	if (s == scripts.end() || !proc) {
		if (verbose) fprintf(stderr, "furb_cli: dialog %d cancelled (no script)\n", id);
		return 0;
	}
	FakeDlg *d = open_dialog(id, proc, lp);
	if (!d->ended) s->second((HWND)d);
	INT_PTR r = d->ended ? d->result : 0;
	close_dialog(d);
	return r;
}
INT_PTR DialogBoxIndirectParam(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM) {
	if (verbose) fprintf(stderr, "furb_cli: in-memory dialog cancelled\n");
	return 0;
}
HWND CreateDialogParam(HINSTANCE, LPCTSTR t, HWND, DLGPROC proc, LPARAM lp) {
	if (!proc) return NULL;
	int id = tmpl_id(t);
	FakeDlg *d = open_dialog(id, proc, lp);
	modeless_by_tmpl[id] = d;
	auto s = scripts.find(id);
	if (s != scripts.end()) s->second((HWND)d);
	return (HWND)d;
}
HWND CreateDialogIndirectParam(HINSTANCE, LPCDLGTEMPLATE, HWND, DLGPROC, LPARAM) { return NULL; }
BOOL EndDialog(HWND h, INT_PTR r) {
	FakeDlg *d = as_dlg(h);
	if (!d) return FALSE;
	d->ended = true; d->result = r;
	return TRUE;
}
HWND GetDlgItem(HWND h, int id) {
	FakeDlg *d = as_dlg(h);
	if (!d) return NULL;
	FakeCtl *&c = d->ctls[id];
	if (!c) { c = new FakeCtl{d, id}; controls.insert(c); }
	return (HWND)c;
}
BOOL SetDlgItemText(HWND h, int id, LPCTSTR t) { if (FakeDlg *d = as_dlg(h)) { d->text[id] = t ? t : L""; return TRUE; } return FALSE; }
UINT GetDlgItemText(HWND h, int id, LPTSTR buf, int n) {
	FakeDlg *d = as_dlg(h);
	if (!buf || n <= 0) return 0;
	std::wstring t = d ? d->text[id] : L"";
	wcsncpy(buf, t.c_str(), n - 1);
	buf[n - 1] = 0;
	return (UINT)wcslen(buf);
}
BOOL SetDlgItemInt(HWND h, int id, UINT v, BOOL sgn) {
	wchar_t b[32];
	swprintf(b, 32, sgn ? L"%d" : L"%u", v);
	return SetDlgItemText(h, id, b);
}
UINT GetDlgItemInt(HWND h, int id, BOOL *ok, BOOL sgn) {
	FakeDlg *d = as_dlg(h);
	std::wstring t = d ? d->text[id] : L"";
	if (ok) *ok = !t.empty();
	return sgn ? (UINT)wcstol(t.c_str(), NULL, 10) : (UINT)wcstoul(t.c_str(), NULL, 10);
}
BOOL CheckDlgButton(HWND h, int id, UINT c) { if (FakeDlg *d = as_dlg(h)) { d->check[id] = c; return TRUE; } return FALSE; }
UINT IsDlgButtonChecked(HWND h, int id) { FakeDlg *d = as_dlg(h); return d ? d->check[id] : 0; }
BOOL CheckRadioButton(HWND h, int first, int last, int on) {
	FakeDlg *d = as_dlg(h);
	if (!d) return FALSE;
	for (int i = first; i <= last; i++) d->check[i] = (i == on);
	return TRUE;
}
LRESULT SendDlgItemMessage(HWND h, int id, UINT msg, WPARAM wp, LPARAM lp) {
	FakeDlg *d = as_dlg(h);
	if (!d) {
		// Nintendulator's debug window (hDebug is NULL here): AddDebug text
		if (verbose && msg == EM_REPLACESEL && lp) {
			std::string s = furb_narrow((const wchar_t *)lp);
			if (s == "\r\n") fputc('\n', stderr);
			else fprintf(stderr, "furb_cli: %s", s.c_str());
		}
		return 0;
	}
	switch (msg) {
	case TBM_SETPOS: d->pos[id] = (int)lp; return 0;
	case TBM_GETPOS: return d->pos[id];
	case CB_SETCURSEL: case LB_SETCURSEL: d->pos[id] = (int)wp; return 0;
	case CB_GETCURSEL: case LB_GETCURSEL: return d->pos[id];
	case BM_SETCHECK: d->check[id] = (int)wp; return 0;
	case BM_GETCHECK: return d->check[id];
	case WM_SETTEXT: d->text[id] = lp ? (const wchar_t *)lp : L""; return TRUE;
	case WM_GETTEXT: {
		std::wstring t = d->text[id];
		if (!lp || !wp) return 0;
		wcsncpy((wchar_t *)lp, t.c_str(), wp - 1);
		((wchar_t *)lp)[wp - 1] = 0;
		return wcslen((wchar_t *)lp);
	}
	default: return 0;
	}
}
LRESULT SendMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
	if (FakeCtl *c = as_ctl(h)) return SendDlgItemMessage((HWND)c->dlg, c->id, msg, wp, lp);
	if (FakeDlg *d = as_dlg(h)) return d->proc(h, msg, wp, lp);
	return 0;
}
int GetWindowTextLength(HWND h) {
	if (FakeCtl *c = as_ctl(h)) return (int)c->dlg->text[c->id].size();
	return 0;
}
static BOOL pick_file(OPENFILENAME *o) {
	if (file_queue.empty()) {
		if (verbose) fprintf(stderr, "furb_cli: file dialog cancelled (no file queued)\n");
		return FALSE;
	}
	std::wstring f = file_queue.front();
	file_queue.pop_front();
	if (!o->lpstrFile || o->nMaxFile == 0) return FALSE;
	wcsncpy(o->lpstrFile, f.c_str(), o->nMaxFile - 1);
	o->lpstrFile[o->nMaxFile - 1] = 0;
	size_t slash = f.find_last_of(L"/\\"), dot = f.find_last_of(L'.');
	o->nFileOffset = (WORD)(slash == std::wstring::npos ? 0 : slash + 1);
	o->nFileExtension = (WORD)(dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? f.size() : dot + 1);
	return TRUE;
}
BOOL GetOpenFileName(OPENFILENAME *o) { return pick_file(o); }
BOOL GetSaveFileName(OPENFILENAME *o) { return pick_file(o); }

// ---------------------------------------------------------------- cursor / mic
BOOL GetCursorPos(POINT *p) { p->x = cur_x; p->y = cur_y; return TRUE; }
BOOL SetCursorPos(int x, int y) { cur_x = x; cur_y = y; return TRUE; }
BOOL GetClientRect(HWND, RECT *r) { r->left = r->top = 0; r->right = client_w; r->bottom = client_h; return TRUE; }
BOOL ScreenToClient(HWND, POINT *) { return TRUE; }
BOOL ClientToScreen(HWND, POINT *) { return TRUE; }
float furb_mic_level(void) { return mic; }

// ---------------------------------------------------------------- DirectX
HRESULT DirectDrawCreateEx(GUID *, void **out, REFIID, IUnknown *) { *out = NULL; return E_FAIL; }
// An inert DirectInput: a keyboard and a mouse whose GetDeviceState leaves
// Controllers' state arrays alone (furb_cli.cpp fills them from the input
// script before each frame), and no joysticks (furb_cli's pads are the
// virtual joysticks 2+port, whose JoyState it sets directly).
struct NullDevice : IDirectInputDevice8 {
	const wchar_t *name;
	NullDevice(const wchar_t *n) : name(n) {}
	HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
	HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
	HRESULT GetCapabilities(DIDEVCAPS *caps) { DWORD n = caps->dwSize; memset(caps, 0, sizeof *caps); caps->dwSize = n; return S_OK; }
	HRESULT GetDeviceInfo(DIDEVICEINSTANCE *inst) {
		DWORD n = inst->dwSize;
		memset(inst, 0, sizeof *inst);
		inst->dwSize = n;
		wcscpy(inst->tszInstanceName, name);
		wcscpy(inst->tszProductName, name);
		return S_OK;
	}
	HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACK, LPVOID, DWORD) { return S_OK; }
	HRESULT Acquire(void) { return S_OK; }
	HRESULT Unacquire(void) { return S_OK; }
	HRESULT GetDeviceState(DWORD, LPVOID) { return S_OK; }
	HRESULT Poll(void) { return S_OK; }
	HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
	ULONG Release(void) { delete this; return 0; }
};
struct NullDirectInput : IDirectInput8 {
	HRESULT CreateDevice(REFGUID g, IDirectInputDevice8 **out, IUnknown *) {
		*out = new NullDevice(IsEqualGUID(g, GUID_SysMouse) ? L"Mouse" : L"Keyboard");
		return S_OK;
	}
	HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACK, LPVOID, DWORD) { return S_OK; }
	ULONG Release(void) { delete this; return 0; }
};
HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, void **out, IUnknown *) { *out = new NullDirectInput; return S_OK; }

// The capture-only DirectSound: handed out only when furb_cli is recording
// audio (--wav/--avi); Sound.cpp then mixes exactly as on Windows, and every
// frame's Lock/Unlock delivers the samples the speakers would have played.
// The play cursor always reads "far ahead", so Sound::Run never waits.
struct CaptureBuffer : IDirectSoundBuffer {
	bool clearing = false;
	std::vector<unsigned char> mem;
	HRESULT SetFormat(const WAVEFORMATEX *) { return S_OK; }
	HRESULT Play(DWORD, DWORD, DWORD) { return S_OK; }
	HRESULT Stop(void) { return S_OK; }
	HRESULT GetCurrentPosition(DWORD *play, DWORD *write) {
		if (play) *play = 0x7FFFFFFF;
		if (write) *write = 0x7FFFFFFF;
		return S_OK;
	}
	HRESULT Lock(DWORD, DWORD bytes, void **ptr1, DWORD *len1, void **ptr2, DWORD *len2, DWORD flags) {
		clearing = (flags & DSBLOCK_ENTIREBUFFER) != 0;
		if (clearing) bytes = (DWORD)mem.size();
		if (mem.size() < bytes) mem.resize(bytes);
		*ptr1 = mem.data(); *len1 = bytes;
		if (ptr2) *ptr2 = NULL;
		if (len2) *len2 = 0;
		return S_OK;
	}
	HRESULT Unlock(void *ptr1, DWORD len1, void *, DWORD) {
		if (!clearing && audio_sink && len1) audio_sink(ptr1, len1);
		clearing = false;
		return S_OK;
	}
	HRESULT GetStatus(DWORD *s) { *s = DSBSTATUS_PLAYING; return S_OK; }
	ULONG Release(void) { delete this; return 0; }
};
struct CaptureDS : IDirectSound {
	HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
	HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, IUnknown *) {
		CaptureBuffer *b = new CaptureBuffer;
		b->mem.resize(desc->dwBufferBytes);
		*out = b;
		return S_OK;
	}
	ULONG Release(void) { delete this; return 0; }
};
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) {
	if (!audio_sink) { *out = NULL; return E_FAIL; }
	*out = new CaptureDS;
	return S_OK;
}
