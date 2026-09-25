// furb: Furbtendulator with its own window, menus and dialogs, on Linux.
// Sets up GTK and SDL, the data folder and the settings file, then runs
// Furbtendulator's own WinMain; see gui.h for the Win32 layer underneath.
//
//   furb [ROM] [--data-dir DIR] [--config FILE] [--set Name=Value]...
#include "gui.h"
#include "furb_host.h"
#include <SDL.h>
#include <glib-unix.h>
#include <signal.h>
#include <locale.h>
#include <sys/stat.h>

int APIENTRY _tWinMain(HINSTANCE, HINSTANCE, LPTSTR, int);
std::wstring furb_data_parent;		// SHGetFolderPath's "Application Data"; WinMain adds \Nintendulator

static std::string xdg(const char *var, const char *fallback) {
	const char *v = getenv(var);
	if (v && *v) return v;
	const char *home = getenv("HOME");
	return std::string(home ? home : "/tmp") + "/" + fallback;
}
static void mkdirs(const std::string &p) {
	for (size_t i = 1; i <= p.size(); i++)
		if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777);
}

// Compact widgets: Furbtendulator's dialogs are laid out in dialog units for
// Windows' small controls.
static const char *css =
	".furb-dialog { font-size: 8pt; }\n"
	".furb-dialog button { padding: 0 4px; min-height: 0; min-width: 0; }\n"
	".furb-dialog entry { padding: 0 3px; min-height: 0; }\n"
	".furb-dialog combobox button { padding: 0 2px; }\n"
	".furb-dialog checkbutton, .furb-dialog radiobutton { padding: 0; min-height: 0; }\n"
	".furb-dialog check, .furb-dialog radio { min-width: 12px; min-height: 12px; margin: 0 3px 0 0; }\n"
	".furb-dialog scale { padding: 0; min-height: 0; }\n"
	".furb-dialog treeview { padding: 0; }\n"
	".furb-dialog frame > label { margin: 0 2px; }\n";

int main(int argc, char **argv) {
	// Furbtendulator reads its .cfg files with the wide C I/O functions: UTF-8, whatever the user's locale
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("usage: furb [ROM] [--data-dir DIR] [--config FILE] [--set Name=Value]...\n"
			       "  DIR   data folder (savestates, battery saves, ...: DIR/Nintendulator);\n"
			       "        default %s\n"
			       "  FILE  settings, a regedit export of HKCU\\SOFTWARE\\Nintendulator;\n"
			       "        default %s\n",
			       xdg("XDG_DATA_HOME", ".local/share").append("/furbtendulator").c_str(),
			       xdg("XDG_CONFIG_HOME", ".config").append("/furbtendulator/settings.reg").c_str());
			return 0;
		}
	gtk_disable_setlocale();
	setlocale(LC_ALL, "C.UTF-8");
	gtk_init(&argc, &argv);
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
		fprintf(stderr, "furb: SDL: %s (no joysticks)\n", SDL_GetError());
	SDL_JoystickEventState(SDL_IGNORE);	// polled by DirectInput's Poll()

	std::string rom, data_dir, config = xdg("XDG_CONFIG_HOME", ".config") + "/furbtendulator/settings.reg";
	std::vector<std::string> sets;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if ((a == "--data-dir" || a == "--config" || a == "--set") && i + 1 < argc) {
			std::string v = argv[++i];
			if (a == "--data-dir") data_dir = v;
			else if (a == "--config") config = v;
			else sets.push_back(v);
		} else rom = a;
	}
	if (data_dir.empty()) data_dir = xdg("XDG_DATA_HOME", ".local/share") + "/furbtendulator";
	mkdirs(data_dir);
	furb_data_parent = furb_widen(data_dir.c_str());

	// settings: the registry Furbtendulator's own loader reads
	struct stat st;
	bool first_run = stat(config.c_str(), &st) != 0;
	if (!first_run) {
		std::string err;
		if (!FurbHost::load_config(config, err)) fprintf(stderr, "furb: %s\n", err.c_str());
	} else {
		// keyboard defaults for player 1 (device 0 = keyboard, DirectInput scan codes):
		// A=X B=Z Select=RShift Start=Enter, arrows, turbo A=S turbo B=A
		DWORD pad[10] = {0x2D, 0x2C, 0x36, 0x1C, 0xC8, 0xD0, 0xCB, 0xCD, 0x1F, 0x1E};
		FurbHost::set_binary(L"ButtonsStdController1", pad, sizeof pad);
	}
	for (auto &kv : sets) {
		size_t eq = kv.find('=');
		if (eq == std::string::npos) continue;
		std::string name = kv.substr(0, eq), val = kv.substr(eq + 1);
		if (!val.empty() && val[0] == '"') FurbHost::set_string(furb_widen(name.c_str()), furb_widen(val.substr(1, val.size() - 2).c_str()));
		else FurbHost::set_dword(furb_widen(name.c_str()), (DWORD)strtol(val.c_str(), NULL, 0));
	}

	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	// Ctrl+C / kill: close the window as the user would (settings and battery saves are written)
	for (int sig : {SIGINT, SIGTERM})
		g_unix_signal_add(sig, [](gpointer) -> gboolean {
			if (gui::Wnd *m = gui::main_window()) PostMessage(gui::hwnd(m), WM_CLOSE, 0, 0);
			return G_SOURCE_CONTINUE;
		}, NULL);

	std::wstring cmd;
	if (!rom.empty()) {
		char full[4096];
		cmd = L"\"" + furb_widen(realpath(rom.c_str(), full) ? full : rom.c_str()) + L"\"";
	}
	int r = _tWinMain((HINSTANCE)&furb_module_resources, NULL, &cmd[0], SW_SHOW);

	// NES::Destroy (on WM_CLOSE) wrote the settings to the registry
	size_t slash = config.find_last_of('/');
	if (slash != std::string::npos) mkdirs(config.substr(0, slash));
	if (!FurbHost::save_config(config)) fprintf(stderr, "furb: cannot write %s\n", config.c_str());
	SDL_Quit();
	return r;
}
