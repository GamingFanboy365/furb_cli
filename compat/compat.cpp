// Real implementations behind the Win32 shim (see windows.h): wide printf with
// MSVC semantics, UTF-8 paths, "DLL" loading through dlopen, directory
// helpers, zeroing operator new -- compiled into the executables and every
// mapper pack.  The host services live in the executable: host_reg.cpp
// (registry, export table) with host_cli.cpp (furb_cli) or gui/*.cpp (the
// GUI).  The mapper packs (built with -DFURB_PACK) forward to them.
#include <windows.h>
#include <dsound.h>
#include <endpointvolume.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <locale.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <vector>

std::string furb_narrow(const wchar_t *w) {
	std::string r;
	if (!w) return r;
	for (; *w; w++) {
		unsigned c = (unsigned)*w;
		if (c < 0x80) r += (char)c;
		else if (c < 0x800) { r += (char)(0xC0 | c >> 6); r += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { r += (char)(0xE0 | c >> 12); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
		else { r += (char)(0xF0 | c >> 18); r += (char)(0x80 | (c >> 12 & 0x3F)); r += (char)(0x80 | (c >> 6 & 0x3F)); r += (char)(0x80 | (c & 0x3F)); }
	}
	return r;
}

std::wstring furb_widen(const char *s) {
	std::wstring r;
	if (!s) return r;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned c = *p++;
		int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra) c &= 0x3F >> extra;
		while (extra-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
		r += (wchar_t)c;
	}
	return r;
}

// Windows paths -> POSIX: backslashes become '/', and "*.dll" means our ".so".
static std::string posix_path(const wchar_t *w) {
	std::string p = furb_narrow(w);
	for (auto &c : p) if (c == '\\') c = '/';
	if (p.size() > 4 && !strcasecmp(p.c_str() + p.size() - 4, ".dll")) p.replace(p.size() - 4, 4, ".so");
	return p;
}

// Windows file names are case-insensitive (BIOS\\DISKSYS.ROM may be disksys.rom
// on disk): when the exact path is missing, match each component ignoring case.
static std::string resolve_case(const std::string &p) {
	if (p.empty() || access(p.c_str(), F_OK) == 0) return p;
	std::string out = p[0] == '/' ? "/" : "";
	size_t pos = p[0] == '/' ? 1 : 0;
	while (pos <= p.size()) {
		size_t slash = p.find('/', pos);
		std::string part = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
		std::string dir = out.empty() ? "." : out;
		std::string hit = part;
		if (!part.empty() && access((out + part).c_str(), F_OK) != 0)
			if (DIR *d = opendir(dir.c_str())) {
				while (struct dirent *e = readdir(d))
					if (!strcasecmp(e->d_name, part.c_str())) { hit = e->d_name; break; }
				closedir(d);
			}
		out += hit;
		if (slash == std::string::npos) break;
		out += '/';
		pos = slash + 1;
	}
	return out;
}

// MSVC text mode with an encoding ("rt,ccs=UTF-16LE", used for cheats.cfg /
// dip.cfg): the BOM decides UTF-8 or UTF-16LE, and fgetwc returns characters.
// glibc's own ccs= would force UTF-16, so decode here and hand back a UTF-8
// stream, which fgetwc decodes under the C.UTF-8 locale furb_cli sets.
static FILE *open_ccs_read(const std::string &path, const std::string &ccs) {
	FILE *in = fopen(path.c_str(), "rb");
	if (!in) in = fopen(resolve_case(path).c_str(), "rb");
	if (!in) return NULL;
	std::string raw;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) raw.append(buf, n);
	fclose(in);
	std::string utf8;
	bool utf16 = raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE;
	if (raw.compare(0, 3, "\xEF\xBB\xBF") == 0) utf8 = raw.substr(3);
	else if (utf16 || (strcasestr(ccs.c_str(), "UTF-16") && raw.size() >= 2 && raw[1] == 0)) {
		std::wstring w;
		for (size_t i = utf16 ? 2 : 0; i + 1 < raw.size(); i += 2) w += (wchar_t)((unsigned char)raw[i] | (unsigned char)raw[i + 1] << 8);
		utf8 = furb_narrow(w.c_str());
	} else utf8 = raw;
	std::string text;	// text mode: CRLF -> LF
	for (size_t i = 0; i < utf8.size(); i++) if (!(utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n')) text += utf8[i];
	// Hand back a freshly opened real file: a stream we wrote to is byte-
	// oriented (glibc then fails fgetwc), and fmemopen streams cannot do wide
	// reads at all.  Written, closed, reopened, unlinked.
	char name[] = "/tmp/furb_cfg_XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0) return NULL;
	FILE *w = fdopen(fd, "wb");
	fwrite(text.data(), 1, text.size(), w);
	fclose(w);
	FILE *f = fopen(name, "r");
	unlink(name);
	return f;
}

FILE *furb_wfopen(const wchar_t *name, const wchar_t *mode) {
	std::string p = posix_path(name), m = furb_narrow(mode);
	size_t ccs = m.find(",ccs=");
	if (ccs != std::string::npos) {
		std::string enc = m.substr(ccs + 5);
		m = m.substr(0, ccs);
		if (m[0] == 'r' && m.find('+') == std::string::npos) return open_ccs_read(p, enc);
	}
	std::string plain;	// 't' (text) is the default on POSIX
	for (char c : m) if (c != 't') plain += c;
	FILE *f = fopen(p.c_str(), plain.c_str());
	if (!f && plain[0] == 'r') f = fopen(resolve_case(p).c_str(), plain.c_str());
	return f;
}

DWORD GetModuleFileName(HMODULE, wchar_t *buf, DWORD n) {
	char exe[4096];
	ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (k <= 0 || !n) { if (n) buf[0] = 0; return 0; }
	exe[k] = 0;
	std::wstring w = furb_widen(exe);
	for (auto &c : w) if (c == L'/') c = L'\\';	// callers look for the last '\\'
	wcsncpy(buf, w.c_str(), n - 1);
	buf[n - 1] = 0;
	return (DWORD)wcslen(buf);
}
int furb_wremove(const wchar_t *name) { return remove(posix_path(name).c_str()); }
int furb_access(const char *p, int m) { return access(p, m); }

DWORD GetTickCount(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

void furb_splitpath(const wchar_t *path, wchar_t *drive, wchar_t *dir, wchar_t *fname, wchar_t *ext) {
	std::wstring p(path ? path : L"");
	size_t slash = p.find_last_of(L"/\\");
	std::wstring d = slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
	std::wstring f = slash == std::wstring::npos ? p : p.substr(slash + 1);
	size_t dot = f.find_last_of(L'.');
	std::wstring e = dot == std::wstring::npos ? L"" : f.substr(dot);
	if (dot != std::wstring::npos) f = f.substr(0, dot);
	if (drive) drive[0] = 0;
	if (dir) wcscpy(dir, d.c_str());
	if (fname) wcscpy(fname, f.c_str());
	if (ext) wcscpy(ext, e.c_str());
}

// MSVC wide formats: %s and %c take WIDE arguments, %S/%C narrow; %hs narrow.
std::wstring furb_fixfmt(const wchar_t *fmt) {
	std::wstring out;
	for (const wchar_t *p = fmt; *p; p++) {
		out += *p;
		if (*p != L'%') continue;
		if (p[1] == L'%') { out += *++p; continue; }
		// copy flags/width/precision/length until the conversion char
		while (p[1] && wcschr(L"-+ #0123456789.*", p[1])) out += *++p;
		bool narrow = false, wide = false;
		while (p[1] && wcschr(L"hlLqjztI", p[1])) {
			wchar_t c = *++p;
			if (c == L'h') { narrow = true; continue; }
			if (c == L'I') {	// I64 / I32 / I
				if (p[1] == L'6' && p[2] == L'4') { out += L"ll"; p += 2; }
				else if (p[1] == L'3' && p[2] == L'2') { p += 2; }
				continue;
			}
			if (c == L'l') wide = true;
			out += c;
		}
		wchar_t conv = p[1];
		if (!conv) break;
		p++;
		if (conv == L's' || conv == L'c') {
			if (narrow) out += conv;	// %hs -> narrow
			else { if (!wide) out += L'l'; out += conv; }
		} else if (conv == L'S' || conv == L'C') {
			out += (wchar_t)(conv == L'S' ? L's' : L'c');	// %S -> narrow
		} else {
			if (narrow) out += L'h';
			out += conv;
		}
	}
	return out;
}

int furb_vswprintf(wchar_t *buf, size_t n, const wchar_t *fmt, va_list ap) {
	std::wstring f = furb_fixfmt(fmt);
	int r = vswprintf(buf, n, f.c_str(), ap);
	if (r < 0 && n) buf[n - 1] = 0;
	return r;
}
int furb_swprintf(wchar_t *buf, size_t n, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, n, fmt, ap); va_end(ap); return r;
}
int furb_swprintf_nc(wchar_t *buf, const wchar_t *fmt, ...) {
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(buf, 1u << 20, fmt, ap); va_end(ap); return r;
}
int furb_fwprintf(FILE *f, const wchar_t *fmt, ...) {
	wchar_t tmp[4096];
	va_list ap; va_start(ap, fmt); int r = furb_vswprintf(tmp, 4096, fmt, ap); va_end(ap);
	fputs(furb_narrow(tmp).c_str(), f);
	return r;
}
int furb_swscanf(const wchar_t *s, const wchar_t *fmt, ...) {
	std::wstring f = furb_fixfmt(fmt);
	va_list ap; va_start(ap, fmt); int r = vswscanf(s, f.c_str(), ap); va_end(ap); return r;
}

// ---------------------------------------------------------------- operator new
// Zero-filled, in the executable and in every pack.  Some constructors leave
// members unset (the CPU and PPU objects NES::CreateMachine allocates are read
// before they are written), so their start state was whatever the heap held
// -- which depends on earlier allocations, down to the length of the paths on
// the command line.  Zeroing makes every run start from the same state.
#include <new>
static void *zalloc(size_t n) {
	void *p = calloc(n ? n : 1, 1);
	if (!p) throw std::bad_alloc();
	return p;
}
void *operator new(size_t n) { return zalloc(n); }
void *operator new[](size_t n) { return zalloc(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return calloc(n ? n : 1, 1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return calloc(n ? n : 1, 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

// ---------------------------------------------------------------- FindFirstFile
struct FindState { std::vector<std::string> names; size_t next; };

static bool fill(FindState *st, WIN32_FIND_DATA *d) {
	if (st->next >= st->names.size()) return false;
	std::string n = st->names[st->next++];
	// report ".so" packs under their Windows ".dll" names; LoadLibrary maps back
	if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) n.replace(n.size() - 3, 3, ".dll");
	memset(d, 0, sizeof *d);
	std::wstring w = furb_widen(n.c_str());
	wcsncpy(d->cFileName, w.c_str(), MAX_PATH - 1);
	return true;
}

HANDLE FindFirstFile(const wchar_t *pattern, WIN32_FIND_DATA *d) {
	std::string p = posix_path(pattern);
	size_t slash = p.find_last_of('/');
	std::string dir = slash == std::string::npos ? "." : p.substr(0, slash);
	std::string pat = slash == std::string::npos ? p : p.substr(slash + 1);
	DIR *dh = opendir(dir.c_str());
	if (!dh) return INVALID_HANDLE_VALUE;
	FindState *st = new FindState{{}, 0};
	while (struct dirent *e = readdir(dh))
		if (!fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD)) st->names.push_back(e->d_name);
	closedir(dh);
	if (!fill(st, d)) { delete st; return INVALID_HANDLE_VALUE; }
	return (HANDLE)st;
}
BOOL FindNextFile(HANDLE h, WIN32_FIND_DATA *d) { return fill((FindState *)h, d); }
BOOL FindClose(HANDLE h) { delete (FindState *)h; return TRUE; }

HMODULE LoadLibrary(const wchar_t *name) {
	void *h = dlopen(posix_path(name).c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h) fprintf(stderr, "furb_cli: dlopen %s: %s\n", posix_path(name).c_str(), dlerror());
	return (HMODULE)h;
}
void *GetProcAddress(HMODULE h, const char *name) { return h ? dlsym((void *)h, name) : NULL; }
BOOL FreeLibrary(HMODULE h) { return h && !dlclose((void *)h); }

// ---------------------------------------------------------------- file system
DWORD GetFileAttributes(LPCTSTR p) {
	struct stat st;
	if (stat(posix_path(p).c_str(), &st)) return (DWORD)INVALID_FILE_ATTRIBUTES;
	return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL CreateDirectory(LPCTSTR p, void *) { return mkdir(posix_path(p).c_str(), 0777) == 0; }
BOOL PathFileExists(LPCTSTR p) { return access(posix_path(p).c_str(), F_OK) == 0; }
BOOL PathAppend(LPTSTR path, LPCTSTR more) {
	size_t n = wcslen(path);
	if (n && path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat(path, L"\\");
	while (*more == L'\\' || *more == L'/') more++;
	wcscat(path, more);
	return TRUE;
}

#ifdef FURB_PACK
// ---------------------------------------------------------------- pack side
// The window system, dialogs, GDI, cursor and threads live in the executable
// (furb_cli's headless ones or the GUI's); each API in winapi.def becomes a
// forwarder that looks the executable's function up once by name.  Resource
// APIs pass this pack's own resource table as the module handle.
#include "furb_rc.h"
static void *host(const char *name) {
	typedef void *(*Lookup)(const char *);
	static Lookup lookup = (Lookup)dlsym(RTLD_DEFAULT, "furb_host_lookup");
	return lookup ? lookup(name) : NULL;
}
#define W(ret, name, params, args) ret name params { \
	typedef ret (*F) params; static F fn_ = (F)host(#name); \
	if (!fn_) return furb_zero<ret>(); return fn_ args; }
#define H W
#define WR(ret, name, params, args) ret name params { \
	typedef ret (*F) params; static F fn_ = (F)host(#name); \
	hinst = (HINSTANCE)&furb_module_resources; \
	if (!fn_) return furb_zero<ret>(); return fn_ args; }
#define HR WR
#include "winapi.def"
#undef W
#undef H
#undef WR
#undef HR
// The packs never touch the registry or DirectSound.
LONG RegOpenKeyEx(HKEY, LPCTSTR, DWORD, DWORD, HKEY *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCreateKeyEx(HKEY, LPCTSTR, DWORD, LPTSTR, DWORD, DWORD, void *, HKEY *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegCloseKey(HKEY) { return 0; }
LONG RegQueryValueEx(HKEY, LPCTSTR, DWORD *, DWORD *, BYTE *, DWORD *) { return ERROR_FILE_NOT_FOUND; }
LONG RegSetValueEx(HKEY, LPCTSTR, DWORD, DWORD, const BYTE *, DWORD) { return ERROR_FILE_NOT_FOUND; }
HRESULT DirectSoundCreate(const GUID *, LPDIRECTSOUND *out, void *) { *out = NULL; return E_FAIL; }
#endif
