// GDI on cairo and pango: memory DCs and bitmaps (the debugger's pattern /
// nametable / palette views, the palette dialog's swatches), window DCs that
// draw into a window's backing surface (shown by its GTK draw handler),
// brushes, pens, fonts and text.  All under one lock: the emulation thread
// updates the debugger's bitmaps while the main thread paints.
#include "gui.h"

namespace gui {
std::recursive_mutex gdi_lock;
thread_local Wnd *painting = NULL;
}
using namespace gui;

enum { O_BITMAP = 1, O_BRUSH, O_PEN, O_FONT, O_DC };
struct Obj { uint32_t magic = 0x46555247; int type; bool stock = false; };
struct Bmp : Obj { cairo_surface_t *s = NULL; int w = 1, h = 1; };
struct Brush : Obj { COLORREF c = 0; bool null = false; };
struct Pen : Obj { COLORREF c = 0; int width = 1; bool null = false; };
struct Font : Obj { int height = 0, weight = 400; std::string face; };
struct Dc : Obj {
	Wnd *wnd = NULL;
	Bmp *bmp = NULL;
	Brush *brush = NULL;
	Pen *pen = NULL;
	Font *font = NULL;
	COLORREF text = 0, bk = 0xFFFFFF;
	int bkmode = OPAQUE;
	POINT cur = {0, 0};
};

template <class T> static T *as(void *h, int type) {
	Obj *o = (Obj *)h;
	return (uintptr_t)h > 0x10000 && o->magic == 0x46555247 && o->type == type ? (T *)o : NULL;
}

// ---- stock objects and system colours
static COLORREF sys_color(int i) {
	switch (i) {
	case COLOR_WINDOW: return RGB(255, 255, 255);
	case COLOR_WINDOWTEXT: case 7: case 18: return RGB(0, 0, 0);	// menu/button text
	case COLOR_BTNFACE: return RGB(240, 240, 240);
	case 16: return RGB(160, 160, 160);	// COLOR_BTNSHADOW
	case 20: return RGB(255, 255, 255);	// COLOR_BTNHIGHLIGHT
	case 13: return RGB(0, 120, 215);	// COLOR_HIGHLIGHT
	case 14: return RGB(255, 255, 255);	// COLOR_HIGHLIGHTTEXT
	case 1: return RGB(0, 0, 0);		// COLOR_BACKGROUND
	}
	return RGB(240, 240, 240);
}
static Brush *stock_brush(COLORREF c, bool null = false) {
	Brush *b = new Brush; b->type = O_BRUSH; b->c = c; b->null = null; b->stock = true;
	return b;
}
static Brush *brush_of(HBRUSH h) {
	if (Brush *b = as<Brush>(h, O_BRUSH)) return b;
	uintptr_t i = (uintptr_t)h;		// (HBRUSH)(COLOR_x + 1)
	static std::map<uintptr_t, Brush *> sys;
	if (i > 0 && i < 64) {
		Brush *&b = sys[i];
		if (!b) b = stock_brush(sys_color((int)i - 1));
		return b;
	}
	return NULL;
}
static Font *default_font(void) {
	static Font *f = [] { Font *f = new Font; f->type = O_FONT; f->stock = true; f->height = -12; f->face = "Sans"; return f; }();
	return f;
}
DWORD GetSysColor(int i) { return sys_color(i); }
HBRUSH GetSysColorBrush(int i) { return (HBRUSH)brush_of((HBRUSH)(uintptr_t)(i + 1)); }
HGDIOBJ GetStockObject(int i) {
	static std::map<int, Obj *> stock;
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Obj *&o = stock[i];
	if (o) return o;
	switch (i) {
	case 0: o = stock_brush(RGB(255, 255, 255)); break;	// WHITE_BRUSH
	case 1: o = stock_brush(RGB(192, 192, 192)); break;
	case 2: o = stock_brush(RGB(128, 128, 128)); break;
	case 3: o = stock_brush(RGB(64, 64, 64)); break;
	case 4: o = stock_brush(RGB(0, 0, 0)); break;		// BLACK_BRUSH
	case 5: o = stock_brush(0, true); break;		// NULL_BRUSH
	case 6: case 7: case 8: {
		Pen *p = new Pen; p->type = O_PEN; p->stock = true;
		p->c = i == 6 ? RGB(255, 255, 255) : 0; p->null = i == 8;
		o = p;
		break;
	}
	default: o = default_font(); break;			// the fonts
	}
	return o;
}

// ---- DCs
namespace gui {
HDC window_dc(Wnd *w) {
	Dc *d = new Dc;
	d->type = O_DC;
	d->wnd = w;
	return (HDC)d;
}
}
static Dc *dc_of(HDC h) { return as<Dc>(h, O_DC); }
static cairo_surface_t *target(Dc *d) {
	if (!d) return NULL;
	if (d->bmp) return d->bmp->s;
	if (d->wnd && wnd(hwnd(d->wnd))) return backing_for(d->wnd);
	static cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	return scratch;
}
static void touched(Dc *d) {
	if (d && d->wnd && d->wnd != painting) invalidate(d->wnd);
}
static void color(cairo_t *cr, COLORREF c) {
	cairo_set_source_rgb(cr, GetRValue(c) / 255.0, GetGValue(c) / 255.0, GetBValue(c) / 255.0);
}

HDC GetDC(HWND h) {
	Wnd *w = wnd(h);
	if (h && !w) return NULL;
	return window_dc(w);
}
HDC GetWindowDC(HWND h) { return GetDC(h); }
int ReleaseDC(HWND, HDC h) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Dc *d = dc_of(h);
	if (!d) return 0;
	touched(d);
	delete d;
	return 1;
}
HDC BeginPaint(HWND h, PAINTSTRUCT *ps) {
	Wnd *w = wnd(h);
	HDC dc = window_dc(w);
	if (ps) {
		memset(ps, 0, sizeof *ps);
		ps->hdc = dc;
		RECT r = {0, 0, 0, 0};
		if (w) GetClientRect(h, &r);
		ps->rcPaint = r;
	}
	return dc;
}
BOOL EndPaint(HWND h, const PAINTSTRUCT *ps) { return ps ? ReleaseDC(h, ps->hdc) : FALSE; }
HDC CreateCompatibleDC(HDC) {
	Dc *d = new Dc;
	d->type = O_DC;
	Bmp *b = new Bmp;	// the 1x1 bitmap a new memory DC starts with
	b->type = O_BITMAP;
	b->s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	b->stock = true;
	d->bmp = b;
	return (HDC)d;
}
BOOL DeleteDC(HDC h) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Dc *d = dc_of(h);
	if (!d) return FALSE;
	if (d->bmp && d->bmp->stock) { cairo_surface_destroy(d->bmp->s); delete d->bmp; }
	delete d;
	return TRUE;
}

// ---- objects
static Bmp *new_bitmap(int w, int h) {
	Bmp *b = new Bmp;
	b->type = O_BITMAP;
	b->w = std::max(1, w);
	b->h = std::max(1, abs(h));
	b->s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, b->w, b->h);
	return b;
}
HBITMAP CreateCompatibleBitmap(HDC, int w, int h) { return (HBITMAP)new_bitmap(w, h); }
HBITMAP CreateDIBSection(HDC, const BITMAPINFO *bi, UINT, void **bits, HANDLE, DWORD) {
	if (!bi) return NULL;
	Bmp *b = new_bitmap(bi->bmiHeader.biWidth, bi->bmiHeader.biHeight);
	if (bits) *bits = cairo_image_surface_get_data(b->s);	// 32-bit, top-down
	return (HBITMAP)b;
}
HBRUSH CreateSolidBrush(COLORREF c) { Brush *b = new Brush; b->type = O_BRUSH; b->c = c; return (HBRUSH)b; }
HPEN CreatePen(int style, int width, COLORREF c) {
	Pen *p = new Pen; p->type = O_PEN; p->c = c; p->width = std::max(1, width); p->null = style == 5;	// PS_NULL
	return (HPEN)p;
}
HFONT CreateFont(int h, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD pitch, LPCTSTR face) {
	Font *f = new Font;
	f->type = O_FONT;
	f->height = h ? h : -12;
	f->weight = weight ? weight : 400;
	std::string n = utf8(face ? face : L"");
	std::string ln = n;
	for (auto &c : ln) c = (char)tolower((unsigned char)c);
	bool mono = (pitch & 3) == 1 || ln.find("courier") != std::string::npos || ln.find("consol") != std::string::npos ||
		ln.find("lucida console") != std::string::npos || ln.find("fixedsys") != std::string::npos || ln.find("terminal") != std::string::npos;
	f->face = mono ? "Monospace" : n.empty() || ln.find("ms shell") != std::string::npos || ln == "system" ? "Sans" : n;
	return (HFONT)f;
}
HFONT CreateFontIndirect(const LOGFONT *lf) {
	if (!lf) return NULL;
	return CreateFont(lf->lfHeight, lf->lfWidth, 0, 0, lf->lfWeight, 0, 0, 0, 0, 0, 0, 0, lf->lfPitchAndFamily, lf->lfFaceName);
}
HGDIOBJ SelectObject(HDC h, HGDIOBJ o) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Dc *d = dc_of(h);
	if (!d || !o) return NULL;
	HGDIOBJ old = NULL;
	if (Bmp *b = as<Bmp>(o, O_BITMAP)) {
		old = d->bmp;
		if (d->bmp && d->bmp->stock) { cairo_surface_destroy(d->bmp->s); delete d->bmp; old = NULL; }
		d->bmp = b;
	} else if (Brush *b = brush_of((HBRUSH)o)) { old = d->brush; d->brush = b; }
	else if (Pen *p = as<Pen>(o, O_PEN)) { old = d->pen; d->pen = p; }
	else if (Font *f = as<Font>(o, O_FONT)) { old = d->font; d->font = f; }
	return old;
}
HGDIOBJ GetCurrentObject(HDC h, UINT type) {
	Dc *d = dc_of(h);
	if (!d) return NULL;
	switch (type) {
	case 1: return d->pen;
	case 2: return d->brush;
	case 6: return d->font ? d->font : default_font();
	case 7: return d->bmp;
	}
	return NULL;
}
BOOL DeleteObject(HGDIOBJ o) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Obj *ob = (Obj *)o;
	if ((uintptr_t)o <= 0x10000 || ob->magic != 0x46555247 || ob->stock) return FALSE;
	if (ob->type == O_BITMAP) cairo_surface_destroy(((Bmp *)ob)->s);
	ob->magic = 0;
	switch (ob->type) {
	case O_BITMAP: delete (Bmp *)ob; break;
	case O_BRUSH: delete (Brush *)ob; break;
	case O_PEN: delete (Pen *)ob; break;
	case O_FONT: delete (Font *)ob; break;
	}
	return TRUE;
}
int GetObject(HGDIOBJ o, int n, void *buf) {
	if (Bmp *b = as<Bmp>(o, O_BITMAP)) {
		if (buf && n >= (int)sizeof(BITMAP_)) {
			BITMAP_ *bm = (BITMAP_ *)buf;
			memset(bm, 0, sizeof *bm);
			bm->bmWidth = b->w; bm->bmHeight = b->h; bm->bmWidthBytes = b->w * 4; bm->bmPlanes = 1; bm->bmBitsPixel = 32;
			bm->bmBits = cairo_image_surface_get_data(b->s);
		}
		return sizeof(BITMAP_);
	}
	if (Font *f = as<Font>(o, O_FONT)) {
		if (buf && n >= (int)sizeof(LOGFONT)) {
			LOGFONT *lf = (LOGFONT *)buf;
			memset(lf, 0, sizeof *lf);
			lf->lfHeight = f->height; lf->lfWeight = f->weight;
			wcsncpy(lf->lfFaceName, wide(f->face.c_str()).c_str(), 31);
		}
		return sizeof(LOGFONT);
	}
	return 0;
}
COLORREF SetTextColor(HDC h, COLORREF c) { Dc *d = dc_of(h); if (!d) return 0; COLORREF o = d->text; d->text = c; return o; }
COLORREF SetBkColor(HDC h, COLORREF c) { Dc *d = dc_of(h); if (!d) return 0; COLORREF o = d->bk; d->bk = c; return o; }
int SetBkMode(HDC h, int m) { Dc *d = dc_of(h); if (!d) return 0; int o = d->bkmode; d->bkmode = m; return o; }
int SetStretchBltMode(HDC, int) { return 1; }
int GetDeviceCaps(HDC, int i) {
	switch (i) {
	case 88: case 90: return 96;		// LOGPIXELSX/Y
	case 12: return 32;			// BITSPIXEL
	case 14: return 1;			// PLANES
	case 8: return GetSystemMetrics(SM_CXSCREEN);
	case 10: return GetSystemMetrics(SM_CYSCREEN);
	case 116: return 60;			// VREFRESH
	}
	return 0;
}

// ---- drawing
struct Draw {
	std::lock_guard<std::recursive_mutex> l{gdi_lock};
	Dc *d;
	cairo_t *cr = NULL;
	Draw(HDC h) : d(dc_of(h)) {
		cairo_surface_t *s = target(d);
		if (s) { cairo_surface_flush(s); cairo_surface_mark_dirty(s); cr = cairo_create(s); }
	}
	~Draw() { if (cr) { cairo_destroy(cr); cairo_surface_flush(target(d)); touched(d); } }
};

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
	Draw dr(dst);
	if (!dr.cr) return FALSE;
	cairo_rectangle(dr.cr, x, y, w, h);
	if (rop == BLACKNESS || rop == WHITENESS) {
		cairo_set_source_rgb(dr.cr, rop == WHITENESS, rop == WHITENESS, rop == WHITENESS);
		cairo_fill(dr.cr);
		return TRUE;
	}
	Dc *s = dc_of(src);
	cairo_surface_t *ss = target(s);
	if (!ss) return FALSE;
	cairo_surface_flush(ss);
	cairo_surface_mark_dirty(ss);
	cairo_set_operator(dr.cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(dr.cr, ss, x - sx, y - sy);
	cairo_fill(dr.cr);
	return TRUE;
}
BOOL StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD) {
	Draw dr(dst);
	cairo_surface_t *ss = target(dc_of(src));
	if (!dr.cr || !ss || !sw || !sh) return FALSE;
	cairo_surface_mark_dirty(ss);
	cairo_rectangle(dr.cr, x, y, w, h);
	cairo_clip(dr.cr);
	cairo_translate(dr.cr, x, y);
	cairo_scale(dr.cr, (double)w / sw, (double)h / sh);
	cairo_set_source_surface(dr.cr, ss, -sx, -sy);
	cairo_pattern_set_filter(cairo_get_source(dr.cr), CAIRO_FILTER_NEAREST);
	cairo_set_operator(dr.cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(dr.cr);
	return TRUE;
}

// DIB rows (bottom-up when biHeight > 0; 32/24/16/8/4/1 bpp) -> a cairo surface
static void dib_to(cairo_surface_t *s, int start, int lines, const void *bits, const BITMAPINFO *bi) {
	const BITMAPINFOHEADER &h = bi->bmiHeader;
	int w = h.biWidth, height = abs(h.biHeight), bpp = h.biBitCount;
	int stride = ((w * bpp + 31) / 32) * 4;
	cairo_surface_flush(s);
	uint8_t *dst = cairo_image_surface_get_data(s);
	int dstride = cairo_image_surface_get_stride(s), dw = cairo_image_surface_get_width(s), dh = cairo_image_surface_get_height(s);
	const RGBQUAD *pal = bi->bmiColors;
	for (int line = 0; line < lines; line++) {
		int row = start + line;			// DIB scan line number (0 = bottom when bottom-up)
		int y = h.biHeight > 0 ? height - 1 - row : row;
		if (y < 0 || y >= dh) continue;
		const uint8_t *p = (const uint8_t *)bits + (size_t)line * stride;
		uint32_t *o = (uint32_t *)(dst + (size_t)y * dstride);
		for (int x = 0; x < w && x < dw; x++) {
			uint32_t v;
			switch (bpp) {
			case 32: v = p[x * 4] | p[x * 4 + 1] << 8 | p[x * 4 + 2] << 16; break;
			case 24: v = p[x * 3] | p[x * 3 + 1] << 8 | p[x * 3 + 2] << 16; break;
			case 16: { uint16_t c = p[x * 2] | p[x * 2 + 1] << 8; v = ((c >> 10 & 31) * 255 / 31) << 16 | ((c >> 5 & 31) * 255 / 31) << 8 | (c & 31) * 255 / 31; break; }
			case 8: { const RGBQUAD &q = pal[p[x]]; v = q.rgbRed << 16 | q.rgbGreen << 8 | q.rgbBlue; break; }
			case 4: { const RGBQUAD &q = pal[(p[x / 2] >> (x & 1 ? 0 : 4)) & 15]; v = q.rgbRed << 16 | q.rgbGreen << 8 | q.rgbBlue; break; }
			case 1: { const RGBQUAD &q = pal[(p[x / 8] >> (7 - (x & 7))) & 1]; v = q.rgbRed << 16 | q.rgbGreen << 8 | q.rgbBlue; break; }
			default: v = 0;
			}
			o[x] = v | 0xFF000000;
		}
	}
	cairo_surface_mark_dirty(s);
}
int SetDIBits(HDC, HBITMAP hb, UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Bmp *b = as<Bmp>(hb, O_BITMAP);
	if (!b || !bits || !bi) return 0;
	dib_to(b->s, start, lines, bits, bi);
	return (int)lines;
}
int GetDIBits(HDC, HBITMAP hb, UINT start, UINT lines, void *bits, BITMAPINFO *bi, UINT) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Bmp *b = as<Bmp>(hb, O_BITMAP);
	if (!b || !bi) return 0;
	if (!bits) {
		bi->bmiHeader.biWidth = b->w; bi->bmiHeader.biHeight = b->h; bi->bmiHeader.biBitCount = 32; bi->bmiHeader.biPlanes = 1;
		return b->h;
	}
	cairo_surface_flush(b->s);
	const uint8_t *src = cairo_image_surface_get_data(b->s);
	int sstride = cairo_image_surface_get_stride(b->s);
	for (UINT line = 0; line < lines; line++) {
		int row = start + line, y = bi->bmiHeader.biHeight > 0 ? b->h - 1 - row : row;
		if (y < 0 || y >= b->h) continue;
		memcpy((uint8_t *)bits + (size_t)line * b->w * 4, src + (size_t)y * sstride, b->w * 4);
	}
	return (int)lines;
}
static int dib_blit(HDC hdc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits, const BITMAPINFO *bi) {
	if (!bits || !bi) return 0;
	int bw = bi->bmiHeader.biWidth, bh = abs(bi->bmiHeader.biHeight);
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(1, bw), std::max(1, bh));
	dib_to(tmp, 0, bh, bits, bi);
	Draw dr(hdc);
	if (dr.cr) {
		cairo_rectangle(dr.cr, x, y, w, h);
		cairo_clip(dr.cr);
		cairo_translate(dr.cr, x, y);
		if (sw && sh) cairo_scale(dr.cr, (double)w / sw, (double)h / sh);
		cairo_set_source_surface(dr.cr, tmp, -sx, -sy);
		cairo_pattern_set_filter(cairo_get_source(dr.cr), CAIRO_FILTER_NEAREST);
		cairo_paint(dr.cr);
	}
	cairo_surface_destroy(tmp);
	return h;
}
int StretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void *bits, const BITMAPINFO *bi, UINT, DWORD) {
	return dib_blit(dc, x, y, w, h, sx, sy, sw, sh, bits, bi);
}
int SetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT, UINT, const void *bits, const BITMAPINFO *bi, UINT) {
	return dib_blit(dc, x, y, (int)w, (int)h, sx, sy, (int)w, (int)h, bits, bi);
}
int FillRect(HDC h, const RECT *r, HBRUSH hb) {
	Brush *b = brush_of(hb);
	if (!r || !b || b->null) return 0;
	Draw dr(h);
	if (!dr.cr) return 0;
	color(dr.cr, b->c);
	cairo_rectangle(dr.cr, r->left, r->top, r->right - r->left, r->bottom - r->top);
	cairo_fill(dr.cr);
	return 1;
}
int FrameRect(HDC h, const RECT *r, HBRUSH hb) {
	Brush *b = brush_of(hb);
	if (!r || !b) return 0;
	Draw dr(h);
	if (!dr.cr) return 0;
	color(dr.cr, b->c);
	cairo_set_line_width(dr.cr, 1);
	cairo_rectangle(dr.cr, r->left + 0.5, r->top + 0.5, r->right - r->left - 1, r->bottom - r->top - 1);
	cairo_stroke(dr.cr);
	return 1;
}
BOOL Rectangle(HDC h, int l, int t, int r, int b) {
	Draw dr(h);
	if (!dr.cr) return FALSE;
	Brush *br = dr.d->brush ? dr.d->brush : (Brush *)GetStockObject(0);
	Pen *p = dr.d->pen ? dr.d->pen : (Pen *)GetStockObject(7);
	cairo_rectangle(dr.cr, l + 0.5, t + 0.5, r - l - 1, b - t - 1);
	if (!br->null) { color(dr.cr, br->c); cairo_fill_preserve(dr.cr); }
	if (!p->null) { color(dr.cr, p->c); cairo_set_line_width(dr.cr, p->width); cairo_stroke(dr.cr); }
	cairo_new_path(dr.cr);
	return TRUE;
}
BOOL MoveToEx(HDC h, int x, int y, POINT *old) {
	Dc *d = dc_of(h);
	if (!d) return FALSE;
	if (old) *old = d->cur;
	d->cur = {x, y};
	return TRUE;
}
BOOL LineTo(HDC h, int x, int y) {
	Draw dr(h);
	if (!dr.cr) return FALSE;
	Pen *p = dr.d->pen ? dr.d->pen : (Pen *)GetStockObject(7);
	if (!p->null) {
		color(dr.cr, p->c);
		cairo_set_line_width(dr.cr, p->width);
		cairo_move_to(dr.cr, dr.d->cur.x + 0.5, dr.d->cur.y + 0.5);
		cairo_line_to(dr.cr, x + 0.5, y + 0.5);
		cairo_stroke(dr.cr);
	}
	dr.d->cur = {x, y};
	return TRUE;
}
COLORREF SetPixel(HDC h, int x, int y, COLORREF c) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	Dc *d = dc_of(h);
	cairo_surface_t *s = target(d);
	if (!s || x < 0 || y < 0 || x >= cairo_image_surface_get_width(s) || y >= cairo_image_surface_get_height(s)) return (COLORREF)-1;
	cairo_surface_flush(s);
	uint32_t *p = (uint32_t *)(cairo_image_surface_get_data(s) + (size_t)y * cairo_image_surface_get_stride(s)) + x;
	*p = 0xFF000000 | GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c);
	cairo_surface_mark_dirty(s);
	touched(d);
	return c;
}
COLORREF GetPixel(HDC h, int x, int y) {
	std::lock_guard<std::recursive_mutex> l(gdi_lock);
	cairo_surface_t *s = target(dc_of(h));
	if (!s || x < 0 || y < 0 || x >= cairo_image_surface_get_width(s) || y >= cairo_image_surface_get_height(s)) return (COLORREF)-1;
	cairo_surface_flush(s);
	uint32_t v = *((uint32_t *)(cairo_image_surface_get_data(s) + (size_t)y * cairo_image_surface_get_stride(s)) + x);
	return RGB(v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF);
}

// ---- text
static PangoLayout *layout(cairo_t *cr, Dc *d, const wchar_t *s, int n) {
	Font *f = d && d->font ? d->font : default_font();
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_new();
	pango_font_description_set_family(fd, f->face.c_str());
	int px = f->height < 0 ? -f->height : f->height * 3 / 4;
	pango_font_description_set_absolute_size(fd, std::max(6, px) * PANGO_SCALE);
	pango_font_description_set_weight(fd, (PangoWeight)f->weight);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	std::wstring t = n < 0 ? std::wstring(s ? s : L"") : std::wstring(s, n);
	std::wstring clean;
	for (size_t i = 0; i < t.size(); i++) {
		if (t[i] == L'&' && i + 1 < t.size() && t[i + 1] != L'&') continue;
		if (t[i] != L'\r') clean += t[i];
	}
	pango_layout_set_text(l, utf8(clean).c_str(), -1);
	return l;
}
BOOL GetTextExtentPoint32(HDC h, LPCTSTR s, int n, SIZE *sz) {
	if (!sz) return FALSE;
	std::lock_guard<std::recursive_mutex> lk(gdi_lock);
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
	cairo_t *cr = cairo_create(tmp);
	PangoLayout *l = layout(cr, dc_of(h), s, n);
	int w, hh;
	pango_layout_get_pixel_size(l, &w, &hh);
	sz->cx = w; sz->cy = hh;
	g_object_unref(l);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);
	return TRUE;
}
BOOL TextOut(HDC h, int x, int y, LPCTSTR s, int n) {
	Draw dr(h);
	if (!dr.cr) return FALSE;
	PangoLayout *l = layout(dr.cr, dr.d, s, n);
	int w, hh;
	pango_layout_get_pixel_size(l, &w, &hh);
	if (dr.d->bkmode == OPAQUE) { color(dr.cr, dr.d->bk); cairo_rectangle(dr.cr, x, y, w, hh); cairo_fill(dr.cr); }
	color(dr.cr, dr.d->text);
	cairo_move_to(dr.cr, x, y);
	pango_cairo_show_layout(dr.cr, l);
	g_object_unref(l);
	return TRUE;
}
int DrawText(HDC h, LPCTSTR s, int n, RECT *r, UINT f) {
	if (!r) return 0;
	if (f & DT_CALCRECT) {
		SIZE sz;
		GetTextExtentPoint32(h, s, n, &sz);
		r->right = r->left + sz.cx;
		r->bottom = r->top + sz.cy;
		return sz.cy;
	}
	Draw dr(h);
	if (!dr.cr) return 0;
	PangoLayout *l = layout(dr.cr, dr.d, s, n);
	if (!(f & DT_SINGLELINE)) pango_layout_set_width(l, (r->right - r->left) * PANGO_SCALE);
	int w, hh;
	pango_layout_get_pixel_size(l, &w, &hh);
	int x = r->left, y = r->top;
	if (f & DT_CENTER) x = (r->left + r->right - w) / 2;
	else if (f & 2) x = r->right - w;		// DT_RIGHT
	if (f & DT_VCENTER) y = (r->top + r->bottom - hh) / 2;
	if (dr.d->bkmode == OPAQUE) { color(dr.cr, dr.d->bk); cairo_rectangle(dr.cr, x, y, w, hh); cairo_fill(dr.cr); }
	cairo_rectangle(dr.cr, r->left, r->top, r->right - r->left, r->bottom - r->top);
	cairo_clip(dr.cr);
	color(dr.cr, dr.d->text);
	cairo_move_to(dr.cr, x, y);
	pango_cairo_show_layout(dr.cr, l);
	g_object_unref(l);
	return hh;
}
