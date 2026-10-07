// Furbtendulator for Android: the activity.  The emulator, the picture, the
// sound and the on-screen controls are native (android/native/main.cpp, on
// SDL 2); this class unpacks the data files, imports games and BIOS files
// through Android's file picker, and shows the menus.  The native side calls
// showMenu() with what is loaded ("key=value;..."), and every choice goes
// back to it as a text command through nativeCommand().
package io.github.gamingfanboy365.furbtendulator;

import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.HapticFeedbackConstants;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class FurbActivity extends SDLActivity {
    static native void nativeCommand(String command);

    private static final int PICK_GAME = 1, PICK_BIOS = 2;
    // what Furbtendulator opens (NES.cpp: OpenFile)
    private static final String[] GAME_EXTENSIONS = {".nes", ".unf", ".unif", ".fds", ".qd", ".nsf", ".tnes", ".stbx", ".bin", ".mfc", ".smc"};

    private Map<String, String> state = new HashMap<>();
    private int openDialogs = 0;
    private boolean picking = false;

    @Override
    protected String[] getLibraries() {
        // libmain.so loads the mapper packs (libfurb_*.so) itself
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Intent start = getIntent();
        // SDL would hand an "open with" intent's path to the native side as a
        // dropped file; content:// URIs have no usable path, so import it here
        setIntent(new Intent(Intent.ACTION_MAIN));
        unpackData();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) return;
        openIntent(start);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        openIntent(intent);
    }

    private void openIntent(Intent intent) {
        if (intent == null || intent.getData() == null || !Intent.ACTION_VIEW.equals(intent.getAction())) return;
        File f = importGame(intent.getData());
        if (f != null) nativeCommand("open " + f.getAbsolutePath());
    }

    // ------------------------------------------------------------ data files
    // Furbtendulator's *.cfg files and the BIOS/ and samples/ folders (with
    // their dir.txt lists) go to the app's files folder, which is the
    // emulator's program folder; again whenever the app is updated.
    private void unpackData() {
        SharedPreferences prefs = getSharedPreferences("furb", MODE_PRIVATE);
        long version;
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = pi.lastUpdateTime;
        } catch (Exception e) {
            version = -1;
        }
        if (prefs.getLong("unpacked", 0) == version) return;
        try {
            copyAssets(getAssets(), "furb", getFilesDir());
            prefs.edit().putLong("unpacked", version).apply();
        } catch (IOException e) {
            Toast.makeText(this, "Could not unpack the data files: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private static void copyAssets(AssetManager am, String path, File to) throws IOException {
        String[] list = am.list(path);
        if (list == null || list.length == 0) {
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(to)) {
                copy(in, out);
            }
            return;
        }
        to.mkdirs();
        for (String name : list) copyAssets(am, path + "/" + name, new File(to, name));
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[65536];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
    }

    // ------------------------------------------------------------ importing
    private String displayName(Uri uri) {
        String name = null;
        if (ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
            try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) name = c.getString(0);
            } catch (Exception ignored) {
            }
        }
        if (name == null) name = uri.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "game.nes";
        return name.replaceAll("[/\\\\:*?\"<>|\\x00-\\x1f]", "_");
    }

    private static boolean isGame(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        for (String e : GAME_EXTENSIONS) if (n.endsWith(e)) return true;
        return false;
    }

    // A picked game is copied into the app (files/roms): the emulator opens
    // files by path, and its battery saves and save states are named after
    // the file.  From a .zip, the first file in it that looks like a game.
    private File importGame(Uri uri) {
        String name = displayName(uri);
        File dir = new File(getFilesDir(), "roms");
        dir.mkdirs();
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("cannot read it");
            if (name.toLowerCase(Locale.ROOT).endsWith(".zip")) {
                try (ZipInputStream zip = new ZipInputStream(in)) {
                    for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                        String inner = new File(e.getName()).getName();
                        if (e.isDirectory() || !isGame(inner)) continue;
                        File out = new File(dir, inner);
                        try (OutputStream o = new FileOutputStream(out)) {
                            copy(zip, o);
                        }
                        return out;
                    }
                }
                throw new IOException("no game file in " + name);
            }
            File out = new File(dir, name);
            try (OutputStream o = new FileOutputStream(out)) {
                copy(in, o);
            }
            return out;
        } catch (Exception e) {
            Toast.makeText(this, "Could not open " + name + ": " + e.getMessage(), Toast.LENGTH_LONG).show();
            return null;
        }
    }

    // BIOS files go to files/BIOS under the name Furbtendulator looks for
    // (BIOS/dir.txt lists them with their CRC32), whatever the picked file is
    // called; a file not in the list keeps its own name.
    private void importBios(Uri uri) {
        String name = displayName(uri);
        File dir = new File(getFilesDir(), "BIOS");
        dir.mkdirs();
        try {
            byte[] data;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("cannot read it");
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                copy(in, b);
                data = b.toByteArray();
            }
            CRC32 crc = new CRC32();
            crc.update(data);
            String want = String.format(Locale.ROOT, "%08X", crc.getValue());
            String known = null;
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(new File(dir, "dir.txt")), "ISO-8859-1"))) {
                for (String line; (line = r.readLine()) != null; ) {
                    String[] f = line.trim().split("\\s+");
                    if (f.length >= 2 && f[1].equalsIgnoreCase(want)) { known = f[0]; break; }
                }
            } catch (IOException ignored) {
            }
            String target = known != null ? known : name;
            try (OutputStream o = new FileOutputStream(new File(dir, target))) {
                o.write(data);
            }
            Toast.makeText(this, known != null ? "Installed " + target : "Copied " + target + " to BIOS (not a known BIOS file)", Toast.LENGTH_LONG).show();
        } catch (Exception e) {
            Toast.makeText(this, "Could not copy " + name + ": " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        picking = true;
        try {
            startActivityForResult(i, what);
        } catch (Exception e) {
            picking = false;
            Toast.makeText(this, "No file picker on this device", Toast.LENGTH_LONG).show();
            closed();
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        picking = false;
        Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri != null && request == PICK_GAME) {
            File f = importGame(uri);
            if (f != null) nativeCommand("open " + f.getAbsolutePath());
        } else if (uri != null && request == PICK_BIOS) {
            importBios(uri);
        }
        closed();
    }

    // ------------------------------------------------------------ called from native code
    public void showMenu(final String s) {
        runOnUiThread(() -> {
            state.clear();
            for (String kv : s.split(";")) {
                int eq = kv.indexOf('=');
                if (eq > 0) state.put(kv.substring(0, eq), kv.substring(eq + 1));
            }
            mainMenu();
        });
    }

    public void buzz(String unused) {
        runOnUiThread(() -> {
            if (mSurface != null) mSurface.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        });
    }

    // ------------------------------------------------------------ menus
    private int num(String key) {
        try {
            return Integer.parseInt(state.get(key));
        } catch (Exception e) {
            return 0;
        }
    }

    // The emulator stays paused while any menu or the file picker is open.
    private void closed() {
        if (openDialogs == 0 && !picking) nativeCommand("resume");
    }

    private AlertDialog.Builder dialog(String title) {
        return new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert).setTitle(title);
    }

    private void show(AlertDialog.Builder b) {
        AlertDialog d = b.create();
        openDialogs++;
        d.setOnDismissListener(x -> {
            openDialogs--;
            // (posted: a click that opens the next menu or the picker does so first)
            ui.post(this::closed);
        });
        d.show();
    }

    private final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());

    private interface Action { void run(); }

    private void menu(String title, List<String> labels, List<Action> actions) {
        show(dialog(title).setItems(labels.toArray(new String[0]), (d, which) -> actions.get(which).run()));
    }

    private void mainMenu() {
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        boolean loaded = num("loaded") != 0;
        l.add("Open game…"); a.add(() -> pick(PICK_GAME));
        if (recentGames().length > 0) { l.add("Recent games…"); a.add(this::recentMenu); }
        if (loaded) {
            l.add("Save state…"); a.add(() -> slotMenu(true));
            l.add("Load state…"); a.add(() -> slotMenu(false));
            l.add("Reset"); a.add(() -> nativeCommand("reset"));
            l.add("Power cycle"); a.add(() -> nativeCommand("hardreset"));
            if (num("fds") > 0) { l.add("Disk…"); a.add(this::diskMenu); }
            if (num("vs") > 0) { l.add("Insert coin"); a.add(() -> nativeCommand("coin1")); }
            if (num("vs") > 1) { l.add("Insert coin (player 2)"); a.add(() -> nativeCommand("coin2")); }
            if (num("nsf") > 0) { l.add("Song…"); a.add(this::songMenu); }
            l.add("Region…"); a.add(this::regionMenu);
        }
        l.add("Settings…"); a.add(this::settingsMenu);
        l.add("Install a BIOS file…"); a.add(() -> pick(PICK_BIOS));
        if (loaded) { l.add("Close game"); a.add(() -> nativeCommand("close")); }
        l.add("Exit"); a.add(() -> nativeCommand("quit"));
        String name = state.get("name");
        menu(loaded && name != null ? name : "Furbtendulator", l, a);
    }

    private File[] recentGames() {
        File[] f = new File(getFilesDir(), "roms").listFiles();
        if (f == null) return new File[0];
        Arrays.sort(f, (x, y) -> Long.compare(y.lastModified(), x.lastModified()));
        return f;
    }

    private void recentMenu() {
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        for (File f : recentGames()) {
            l.add(f.getName());
            a.add(() -> {
                f.setLastModified(System.currentTimeMillis());
                nativeCommand("open " + f.getAbsolutePath());
            });
        }
        l.add("Remove games from this list…"); a.add(this::removeMenu);
        menu("Recent games", l, a);
    }

    // (the copies only: battery saves and save states stay)
    private void removeMenu() {
        File[] games = recentGames();
        String[] names = new String[games.length];
        boolean[] checked = new boolean[games.length];
        for (int i = 0; i < games.length; i++) names[i] = games[i].getName();
        show(dialog("Remove from the list")
            .setMultiChoiceItems(names, checked, (d, i, on) -> checked[i] = on)
            .setPositiveButton("Remove", (d, w) -> {
                for (int i = 0; i < games.length; i++) if (checked[i]) games[i].delete();
            })
            .setNegativeButton("Cancel", null));
    }

    private void slotMenu(boolean save) {
        Map<Integer, Long> used = new HashMap<>();
        String s = state.get("states");
        if (s != null && !s.isEmpty())
            for (String e : s.split(",")) {
                String[] p = e.split(":");
                if (p.length == 2) used.put(Integer.parseInt(p[0]), Long.parseLong(p[1]));
            }
        DateFormat fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);
        List<String> l = new ArrayList<>();
        List<Action> a = new ArrayList<>();
        for (int i = 0; i < 10; i++) {
            Long t = used.get(i);
            if (!save && t == null) continue;
            final int slot = i;
            l.add("Slot " + i + (t != null ? "  —  " + fmt.format(new Date(t * 1000)) : "  —  empty"));
            a.add(() -> nativeCommand((save ? "save " : "load ") + slot));
        }
        if (l.isEmpty()) {
            Toast.makeText(this, "No save states for this game yet", Toast.LENGTH_SHORT).show();
            return;
        }
        menu(save ? "Save state" : "Load state", l, a);
    }

    private void diskMenu() {
        menu("Disk (" + num("fds") + " sides)",
            Arrays.asList("Next side", "Previous side", "Eject", "Insert"),
            Arrays.asList(() -> nativeCommand("fds-next"), () -> nativeCommand("fds-prev"),
                          () -> nativeCommand("fds-eject"), () -> nativeCommand("fds-insert")));
    }

    private void songMenu() {
        int n = num("nsf");
        String[] songs = new String[n];
        for (int i = 0; i < n; i++) songs[i] = "Song " + (i + 1);
        show(dialog("Song").setSingleChoiceItems(songs, num("song") - 1, (d, i) -> {
            nativeCommand("nsf-song " + (i + 1));
            d.dismiss();
        }));
    }

    private void regionMenu() {
        show(dialog("Region").setSingleChoiceItems(new String[] {"NTSC (60 Hz)", "PAL (50 Hz)", "Dendy (50 Hz)"}, num("region"), (d, i) -> {
            nativeCommand("region " + i);
            d.dismiss();
        }));
    }

    private void settingsMenu() {
        String[] names = {"Square pixels (instead of a TV's 8:7)", "Smooth the picture", "On-screen controls",
                          "Vibrate on button presses", "Sound", "Show frames per second"};
        String[] keys = {"aspect", "smooth", "controls", "haptics", "sound", "fps"};
        boolean[] on = new boolean[keys.length];
        for (int i = 0; i < keys.length; i++) on[i] = num(keys[i]) != 0;
        show(dialog("Settings")
            .setMultiChoiceItems(names, on, (d, i, v) -> {
                nativeCommand("pref " + keys[i] + " " + (v ? 1 : 0));
                state.put(keys[i], v ? "1" : "0");
            })
            .setNeutralButton("Controls opacity…", (d, w) -> opacityMenu())
            .setPositiveButton("Done", null));
    }

    private void opacityMenu() {
        final int[] levels = {15, 30, 45, 60, 80};
        String[] names = new String[levels.length];
        int cur = 0;
        for (int i = 0; i < levels.length; i++) {
            names[i] = levels[i] + "%";
            if (Math.abs(levels[i] - num("opacity")) < Math.abs(levels[cur] - num("opacity"))) cur = i;
        }
        show(dialog("On-screen controls opacity").setSingleChoiceItems(names, cur, (d, i) -> {
            nativeCommand("pref opacity " + levels[i]);
            d.dismiss();
        }));
    }
}
