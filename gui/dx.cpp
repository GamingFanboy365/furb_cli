// DirectX for GFX.cpp, Sound.cpp and Controllers.cpp.
//   DirectDraw: surfaces are memory (32-bit xRGB, which is also cairo's
//     format); a Blt or Flip to the primary surface hands the picture to the
//     main window, which scales it to its size.  Fullscreen is the window in
//     fullscreen state.  WaitForVerticalBlank waits for the next 60 Hz tick.
//   DirectSound: the looping secondary buffer is a ring the SDL audio callback
//     plays from; its play cursor paces the emulation exactly as on Windows.
//   DirectInput: the keyboard and mouse are what the windows' events say
//     (gui::dik_state, mouse_*); joysticks are SDL joysticks, reported with
//     DirectInput's axis range (0..65535) and POV angles.
#include "gui.h"
#include <SDL.h>
#include <thread>
#include <time.h>

using namespace gui;

// ================================================================ the frame
static std::mutex frame_lock;
static std::vector<uint32_t> frame;
static int frame_w = 0, frame_h = 0;
static std::atomic<bool> redraw_pending(false);

void gui::present(const uint32_t *px, int w, int h, int pitch) {
	{
		std::lock_guard<std::mutex> l(frame_lock);
		frame.resize((size_t)w * h);
		for (int y = 0; y < h; y++) memcpy(&frame[(size_t)y * w], px + (size_t)y * pitch, w * 4);
		frame_w = w; frame_h = h;
	}
	if (!redraw_pending.exchange(true))
		later([] {
			redraw_pending = false;
			Wnd *m = main_window();
			if (m && m->widget) gtk_widget_queue_draw(m->widget);
		});
}
bool gui::draw_frame(cairo_t *cr, int w, int h) {
	std::lock_guard<std::mutex> l(frame_lock);
	if (!frame_w || !frame_h) return false;
	cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)frame.data(), CAIRO_FORMAT_RGB24, frame_w, frame_h, frame_w * 4);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_scale(cr, (double)w / frame_w, (double)h / frame_h);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_surface_destroy(s);
	return true;
}
static void clear_frame(void) {
	std::lock_guard<std::mutex> l(frame_lock);
	frame_w = frame_h = 0;
}
void gui::set_fullscreen(bool on) {
	sync([on] {	// before GFX's own ShowWindow / UpdateInterface that follow
		Wnd *m = main_window();
		if (!m || !m->top) return;
		if (on) gtk_window_fullscreen(GTK_WINDOW(m->top));
		else gtk_window_unfullscreen(GTK_WINDOW(m->top));
	});
}

// ================================================================ DirectDraw
struct Surface : IDirectDrawSurface7 {
	int w, h;
	bool primary;
	std::vector<uint32_t> px;
	Surface *back = NULL;
	int refs = 1;			// COM reference count: GetAttachedSurface adds one, as DirectDraw does
	Surface(int w_, int h_, bool p) : w(std::max(1, w_)), h(std::max(1, h_)), primary(p), px((size_t)w * h) {}
	HRESULT Lock(RECT *, DDSURFACEDESC2 *d, DWORD, HANDLE) {
		DWORD size = d->dwSize;
		GetSurfaceDesc(d);
		d->dwSize = size;
		d->lpSurface = px.data();
		return S_OK;
	}
	HRESULT Unlock(RECT *) { return S_OK; }
	HRESULT Blt(RECT *, IDirectDrawSurface7 *src, RECT *, DWORD flags, DDBLTFX *fx) {
		Surface *s = (Surface *)src;
		if (!s) {
			if ((flags & DDBLT_COLORFILL) && fx) std::fill(px.begin(), px.end(), fx->dwFillColor);
			return S_OK;
		}
		if (primary) present(s->px.data(), s->w, s->h, s->w);	// the window scales it
		else if (s->w == w && s->h == h) px = s->px;
		return S_OK;
	}
	HRESULT Flip(IDirectDrawSurface7 *, DWORD) {
		if (back) present(back->px.data(), back->w, back->h, back->w);
		return S_OK;
	}
	HRESULT GetAttachedSurface(DDSCAPS2 *, IDirectDrawSurface7 **out) {
		*out = back;
		if (back) back->refs++;
		return back ? S_OK : DDERR_SURFACENOTATTACHED;
	}
	HRESULT GetSurfaceDesc(DDSURFACEDESC2 *d) {
		memset(d, 0, sizeof *d);
		d->dwSize = sizeof *d;
		d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
		d->dwWidth = w; d->dwHeight = h; d->lPitch = w * 4;
		d->ddpfPixelFormat.dwSize = sizeof d->ddpfPixelFormat;
		d->ddpfPixelFormat.dwFlags = DDPF_RGB;
		d->ddpfPixelFormat.dwRGBBitCount = 32;
		d->ddpfPixelFormat.dwRBitMask = 0xFF0000;
		d->ddpfPixelFormat.dwGBitMask = 0x00FF00;
		d->ddpfPixelFormat.dwBBitMask = 0x0000FF;
		d->ddsCaps.dwCaps = primary ? DDSCAPS_PRIMARYSURFACE : DDSCAPS_OFFSCREENPLAIN;
		return S_OK;
	}
	HRESULT SetClipper(IDirectDrawClipper *) { return S_OK; }
	HRESULT IsLost(void) { return S_OK; }
	HRESULT Restore(void) { return S_OK; }
	ULONG Release(void) {
		if (--refs > 0) return refs;
		if (primary) clear_frame();
		if (back) back->Release();
		delete this;
		return 0;
	}
};
struct Clipper : IDirectDrawClipper {
	HRESULT SetHWnd(DWORD, HWND) { return S_OK; }
	ULONG Release(void) { delete this; return 0; }
};
struct DDraw : IDirectDraw7 {
	int mode_w = 0, mode_h = 0;
	bool fullscreen = false;
	HRESULT SetCooperativeLevel(HWND, DWORD f) {
		fullscreen = (f & DDSCL_FULLSCREEN) != 0;
		set_fullscreen(fullscreen);
		return S_OK;
	}
	HRESULT SetDisplayMode(DWORD w, DWORD h, DWORD, DWORD, DWORD) { mode_w = w; mode_h = h; return S_OK; }
	HRESULT RestoreDisplayMode(void) { set_fullscreen(false); return S_OK; }
	HRESULT CreateSurface(DDSURFACEDESC2 *d, IDirectDrawSurface7 **out, IUnknown *) {
		bool primary = (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
		int w = (d->dwFlags & DDSD_WIDTH) ? d->dwWidth : primary && mode_w ? mode_w : 256;
		int h = (d->dwFlags & DDSD_HEIGHT) ? d->dwHeight : primary && mode_h ? mode_h : 240;
		Surface *s = new Surface(w, h, primary);
		if (primary && (d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount) s->back = new Surface(w, h, false);
		*out = s;
		return S_OK;
	}
	HRESULT CreateClipper(DWORD, IDirectDrawClipper **out, IUnknown *) { *out = new Clipper; return S_OK; }
	HRESULT WaitForVerticalBlank(DWORD, HANDLE) {
		// the next tick of a 60 Hz clock
		static const long long period = 1000000000LL / 60;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long long t = now.tv_sec * 1000000000LL + now.tv_nsec, next = (t / period + 1) * period;
		struct timespec until = {(time_t)(next / 1000000000LL), (long)(next % 1000000000LL)};
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL);
		return S_OK;
	}
	ULONG Release(void) { set_fullscreen(false); delete this; return 0; }
};
HRESULT DirectDrawCreateEx(GUID *, void **out, REFIID, IUnknown *) {
	*out = new DDraw;
	return S_OK;
}

// ================================================================ DirectSound
struct SoundBuffer : IDirectSoundBuffer {
	std::vector<unsigned char> mem;
	std::atomic<DWORD> play{0};
	std::atomic<bool> playing{false};
	DWORD chunk = 2048;
	SDL_AudioDeviceID dev = 0;
	bool primary = false;
	HRESULT SetFormat(const WAVEFORMATEX *) { return S_OK; }
	HRESULT Play(DWORD, DWORD, DWORD) {
		if (!primary) { playing = true; if (dev) SDL_PauseAudioDevice(dev, 0); }
		return S_OK;
	}
	HRESULT Stop(void) { playing = false; return S_OK; }
	HRESULT GetCurrentPosition(DWORD *p, DWORD *w) {
		DWORD pos = play;
		if (p) *p = pos;
		if (w) *w = mem.empty() ? 0 : (pos + chunk) % mem.size();
		return S_OK;
	}
	HRESULT Lock(DWORD offset, DWORD bytes, void **p1, DWORD *l1, void **p2, DWORD *l2, DWORD flags) {
		if (mem.empty()) return E_FAIL;
		if (flags & DSBLOCK_ENTIREBUFFER) { offset = 0; bytes = (DWORD)mem.size(); }
		offset %= mem.size();
		DWORD first = std::min<DWORD>(bytes, (DWORD)mem.size() - offset);
		*p1 = &mem[offset]; *l1 = first;
		if (p2) *p2 = first < bytes ? &mem[0] : NULL;
		if (l2) *l2 = bytes - first;
		return S_OK;
	}
	HRESULT Unlock(void *, DWORD, void *, DWORD) { return S_OK; }
	HRESULT GetStatus(DWORD *s) { *s = playing ? DSBSTATUS_PLAYING : 0; return S_OK; }
	ULONG Release(void);
};
static SoundBuffer *current = NULL;
static std::mutex sound_lock;
static void audio_cb(void *, Uint8 *out, int len) {
	std::lock_guard<std::mutex> l(sound_lock);
	SoundBuffer *b = current;
	if (!b || !b->playing || b->mem.empty()) { memset(out, 0, len); return; }
	DWORD pos = b->play, size = (DWORD)b->mem.size();
	for (int i = 0; i < len;) {
		int n = std::min<int>(len - i, size - pos);
		memcpy(out + i, &b->mem[pos], n);
		i += n;
		pos = (pos + n) % size;
	}
	b->play = pos;
}
ULONG SoundBuffer::Release(void) {
	{
		std::lock_guard<std::mutex> l(sound_lock);
		if (current == this) current = NULL;
	}
	delete this;
	return 0;
}
struct DSound : IDirectSound {
	SDL_AudioDeviceID dev = 0;
	HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
	HRESULT CreateSoundBuffer(const DSBUFFERDESC *d, IDirectSoundBuffer **out, IUnknown *) {
		SoundBuffer *b = new SoundBuffer;
		b->primary = (d->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
		if (!b->primary) {
			const WAVEFORMATEX *f = d->lpwfxFormat;
			if (!dev) {
				SDL_AudioSpec want = {}, have;
				want.freq = f ? f->nSamplesPerSec : 48000;
				want.format = AUDIO_S16SYS;
				want.channels = f ? f->nChannels : 1;
				want.samples = 512;
				want.callback = audio_cb;
				dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);	// SDL converts to the device's format
				if (!dev) { fprintf(stderr, "furb: no audio device: %s\n", SDL_GetError()); delete b; *out = NULL; return E_FAIL; }
			}
			b->dev = dev;
			b->mem.assign(d->dwBufferBytes, 0);
			b->chunk = 512 * (f ? f->nBlockAlign : 2);
			std::lock_guard<std::mutex> l(sound_lock);
			current = b;
		}
		*out = b;
		return S_OK;
	}
	ULONG Release(void) { if (dev) SDL_CloseAudioDevice(dev); delete this; return 0; }
};
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) {
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		fprintf(stderr, "furb: SDL audio: %s\n", SDL_GetError());
		*out = NULL;
		return E_FAIL;
	}
	*out = new DSound;
	return S_OK;
}

// ================================================================ DirectInput
enum { DEV_KEYBOARD, DEV_MOUSE, DEV_JOY };

// DirectInput's (English) key names, by scan code
static std::string key_name(int k) {
	static const std::map<int, const char *> names = {
		{0x01, "Esc"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"}, {0x0F, "Tab"}, {0x1A, "["}, {0x1B, "]"},
		{0x1C, "Enter"}, {0x1D, "Ctrl"}, {0x27, ";"}, {0x28, "'"}, {0x29, "`"}, {0x2A, "Shift"}, {0x2B, "\\"},
		{0x33, ","}, {0x34, "."}, {0x35, "/"}, {0x36, "Right Shift"}, {0x37, "Num *"}, {0x38, "Alt"}, {0x39, "Space"},
		{0x3A, "Caps Lock"}, {0x45, "Num Lock"}, {0x46, "Scroll Lock"}, {0x47, "Num 7"}, {0x48, "Num 8"}, {0x49, "Num 9"},
		{0x4A, "Num -"}, {0x4B, "Num 4"}, {0x4C, "Num 5"}, {0x4D, "Num 6"}, {0x4E, "Num +"}, {0x4F, "Num 1"},
		{0x50, "Num 2"}, {0x51, "Num 3"}, {0x52, "Num 0"}, {0x53, "Num Del"}, {0x56, "<>"}, {0x57, "F11"}, {0x58, "F12"},
		{0x70, "Kana"}, {0x73, "Ro"}, {0x79, "Convert"}, {0x7B, "No Convert"}, {0x7D, "Yen"}, {0x9C, "Num Enter"},
		{0x9D, "Right Ctrl"}, {0xB5, "Num /"}, {0xB7, "Sys Rq"}, {0xB8, "Right Alt"}, {0xC5, "Pause"}, {0xC7, "Home"},
		{0xC8, "Up"}, {0xC9, "Page Up"}, {0xCB, "Left"}, {0xCD, "Right"}, {0xCF, "End"}, {0xD0, "Down"},
		{0xD1, "Page Down"}, {0xD2, "Insert"}, {0xD3, "Delete"}, {0xDB, "Left Windows"}, {0xDC, "Right Windows"},
		{0xDD, "Application"},
	};
	auto it = names.find(k);
	if (it != names.end()) return it->second;
	static const char *row1 = "1234567890", *rowq = "QWERTYUIOP", *rowa = "ASDFGHJKL", *rowz = "ZXCVBNM";
	if (k >= 0x02 && k <= 0x0B) return std::string(1, row1[k - 0x02]);
	if (k >= 0x10 && k <= 0x19) return std::string(1, rowq[k - 0x10]);
	if (k >= 0x1E && k <= 0x26) return std::string(1, rowa[k - 0x1E]);
	if (k >= 0x2C && k <= 0x32) return std::string(1, rowz[k - 0x2C]);
	if (k >= 0x3B && k <= 0x44) return "F" + std::to_string(k - 0x3A);
	char n[16];
	snprintf(n, sizeof n, "Key %02X", k);
	return n;
}
struct Device : IDirectInputDevice8 {
	int kind;
	int index = 0;
	SDL_Joystick *joy = NULL;
	std::string name;
	HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
	HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
	HRESULT GetCapabilities(DIDEVCAPS *c) {
		DWORD size = c->dwSize;
		memset(c, 0, sizeof *c);
		c->dwSize = size;
		c->dwFlags = DIDC_ATTACHED;
		if (kind == DEV_KEYBOARD) { c->dwDevType = DI8DEVTYPE_KEYBOARD; c->dwButtons = 128; }
		else if (kind == DEV_MOUSE) { c->dwDevType = DI8DEVTYPE_MOUSE; c->dwAxes = 3; c->dwButtons = 3; }
		else {
			c->dwDevType = 0x15;	// DI8DEVTYPE_GAMEPAD
			c->dwAxes = std::min(8, SDL_JoystickNumAxes(joy));
			c->dwButtons = std::min(128, SDL_JoystickNumButtons(joy));
			c->dwPOVs = std::min(4, SDL_JoystickNumHats(joy));
		}
		return S_OK;
	}
	HRESULT GetDeviceInfo(DIDEVICEINSTANCE *i) {
		DWORD size = i->dwSize;
		memset(i, 0, sizeof *i);
		i->dwSize = size;
		i->guidInstance.Data1 = kind == DEV_KEYBOARD ? 1 : kind == DEV_MOUSE ? 2 : 0x100 + index;
		wcsncpy(i->tszInstanceName, wide(name.c_str()).c_str(), MAX_PATH - 1);
		wcsncpy(i->tszProductName, wide(name.c_str()).c_str(), MAX_PATH - 1);
		return S_OK;
	}
	HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACK cb, LPVOID ref, DWORD) {
		DIDEVICEOBJECTINSTANCE o;
		auto emit = [&](const GUID &g, DWORD type, const std::string &n, size_t ofs) {
			memset(&o, 0, sizeof o);
			o.dwSize = sizeof o;
			o.guidType = g;
			o.dwType = type;
			o.dwOfs = (DWORD)ofs;
			wcsncpy(o.tszName, wide(n.c_str()).c_str(), MAX_PATH - 1);
			return cb(&o, ref) != DIENUM_STOP;
		};
		if (kind == DEV_KEYBOARD) {
			for (int k = 1; k < 256; k++) {
				if (!emit(GUID_Key, DIDFT_BUTTON | k << 8, key_name(k), k)) break;
			}
		} else if (kind == DEV_MOUSE) {
			size_t mb = offsetof(DIMOUSESTATE2, rgbButtons);
			emit(GUID_XAxis, DIDFT_AXIS, "X Axis", offsetof(DIMOUSESTATE2, lX));
			emit(GUID_YAxis, DIDFT_AXIS | 1 << 8, "Y Axis", offsetof(DIMOUSESTATE2, lY));
			emit(GUID_ZAxis, DIDFT_AXIS | 2 << 8, "Wheel", offsetof(DIMOUSESTATE2, lZ));
			emit(GUID_Button, DIDFT_BUTTON, "Left Button", mb);
			emit(GUID_Button, DIDFT_BUTTON | 1 << 8, "Right Button", mb + 1);
			emit(GUID_Button, DIDFT_BUTTON | 2 << 8, "Middle Button", mb + 2);
		} else {
			static const size_t axis_ofs[8] = {offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY), offsetof(DIJOYSTATE2, lZ),
				offsetof(DIJOYSTATE2, lRx), offsetof(DIJOYSTATE2, lRy), offsetof(DIJOYSTATE2, lRz),
				offsetof(DIJOYSTATE2, rglSlider), offsetof(DIJOYSTATE2, rglSlider) + 4};
			static const GUID *axes[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
			static const char *axis_names[8] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider 1", "Slider 2"};
			for (int a = 0; a < std::min(8, SDL_JoystickNumAxes(joy)); a++) emit(*axes[a], DIDFT_AXIS | a << 8, axis_names[a], axis_ofs[a]);
			for (int p = 0; p < std::min(4, SDL_JoystickNumHats(joy)); p++)
				emit(GUID_POV, DIDFT_POV | p << 8, "POV " + std::to_string(p + 1), offsetof(DIJOYSTATE2, rgdwPOV) + 4 * p);
			for (int b = 0; b < std::min(128, SDL_JoystickNumButtons(joy)); b++)
				emit(GUID_Button, DIDFT_BUTTON | b << 8, "Button " + std::to_string(b + 1), offsetof(DIJOYSTATE2, rgbButtons) + b);
		}
		return S_OK;
	}
	HRESULT Acquire(void) { return S_OK; }
	HRESULT Unacquire(void) { return S_OK; }
	HRESULT Poll(void) { if (joy) SDL_JoystickUpdate(); return S_OK; }
	HRESULT GetDeviceState(DWORD size, LPVOID data) {
		if (kind == DEV_KEYBOARD) {
			unsigned char *k = (unsigned char *)data;
			for (DWORD i = 0; i < size && i < 256; i++) k[i] = dik_state[i];
		} else if (kind == DEV_MOUSE) {
			DIMOUSESTATE2 *m = (DIMOUSESTATE2 *)data;
			memset(m, 0, std::min<size_t>(size, sizeof *m));
			m->lX = mouse_dx.exchange(0);
			m->lY = mouse_dy.exchange(0);
			m->lZ = mouse_dz.exchange(0);
			unsigned char b = mouse_buttons;
			for (int i = 0; i < 3; i++) m->rgbButtons[i] = (b >> i & 1) ? 0x80 : 0;
		} else {
			DIJOYSTATE2 *j = (DIJOYSTATE2 *)data;
			memset(j, 0, std::min<size_t>(size, sizeof *j));
			LONG *axis[8] = {&j->lX, &j->lY, &j->lZ, &j->lRx, &j->lRy, &j->lRz, &j->rglSlider[0], &j->rglSlider[1]};
			for (int a = 0; a < 8; a++) *axis[a] = 0x8000;
			for (int a = 0; a < std::min(8, SDL_JoystickNumAxes(joy)); a++) *axis[a] = (LONG)SDL_JoystickGetAxis(joy, a) + 32768;
			for (int p = 0; p < 4; p++) {
				j->rgdwPOV[p] = 0xFFFFFFFF;
				if (p >= SDL_JoystickNumHats(joy)) continue;
				static const int deg[16] = {-1, 0, 9000, 4500, 18000, -1, 13500, -1, 27000, 31500, -1, -1, 22500, -1, -1, -1};
				int d = deg[SDL_JoystickGetHat(joy, p) & 15];
				if (d >= 0) j->rgdwPOV[p] = d;
			}
			for (int b = 0; b < std::min(128, SDL_JoystickNumButtons(joy)); b++) j->rgbButtons[b] = SDL_JoystickGetButton(joy, b) ? 0x80 : 0;
		}
		return S_OK;
	}
	HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
	ULONG Release(void) { if (joy) SDL_JoystickClose(joy); delete this; return 0; }
};
struct DInput : IDirectInput8 {
	HRESULT CreateDevice(REFGUID g, IDirectInputDevice8 **out, IUnknown *) {
		Device *d = new Device;
		if (IsEqualGUID(g, GUID_SysKeyboard)) { d->kind = DEV_KEYBOARD; d->name = "Keyboard"; }
		else if (IsEqualGUID(g, GUID_SysMouse)) { d->kind = DEV_MOUSE; d->name = "Mouse"; }
		else {
			d->kind = DEV_JOY;
			d->index = (int)g.Data1 - 0x100;
			d->joy = SDL_JoystickOpen(d->index);
			if (!d->joy) { delete d; *out = NULL; return E_FAIL; }
			d->name = SDL_JoystickName(d->joy) ? SDL_JoystickName(d->joy) : "Joystick";
		}
		*out = d;
		return S_OK;
	}
	HRESULT EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACK cb, LPVOID ref, DWORD) {
		if (type != DI8DEVCLASS_GAMECTRL && type != DI8DEVCLASS_ALL) return S_OK;
		for (int i = 0; i < SDL_NumJoysticks(); i++) {
			DIDEVICEINSTANCE inst = {};
			inst.dwSize = sizeof inst;
			inst.guidInstance.Data1 = 0x100 + i;
			inst.guidProduct.Data1 = 0x100 + i;
			inst.dwDevType = 0x15;
			const char *n = SDL_JoystickNameForIndex(i);
			wcsncpy(inst.tszInstanceName, wide(n ? n : "Joystick").c_str(), MAX_PATH - 1);
			wcsncpy(inst.tszProductName, wide(n ? n : "Joystick").c_str(), MAX_PATH - 1);
			if (cb(&inst, ref) == DIENUM_STOP) break;
		}
		return S_OK;
	}
	ULONG Release(void) { delete this; return 0; }
};
HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, void **out, IUnknown *) {
	*out = new DInput;
	return S_OK;
}
