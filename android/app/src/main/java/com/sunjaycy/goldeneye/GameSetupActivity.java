package com.sunjaycy.goldeneye;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;

/**
 * First-run game-data setup, and the app's launcher activity.
 *
 * Android 11+ blocks ordinary file managers from writing into
 * Android/data/<pkg>/files, so the old "copy the dump into the app's external
 * files dir" instruction needs a PC (adb push) or root -- see issue #16. This
 * screen removes that requirement: it takes "All files access"
 * (MANAGE_EXTERNAL_STORAGE), lets the user pick the folder their GoldenEye 007
 * XBLA dump already sits in, and records that path for the native side.
 *
 * The dump is then read IN PLACE -- nothing is copied. A copy would mean
 * duplicating ~700 MB / 1800 files through SAF, which is both slow and a waste
 * of storage on a handheld.
 *
 * Contract with native code: the chosen absolute path is written as a single
 * line to files/user/ge_game_path.txt, which GeApp::OnConfigurePaths reads and
 * assigns to PathConfig::game_data_root (src/ge_app.h). An absent or stale file
 * means "use the default", i.e. the app's external files dir -- so installs
 * already staged there by adb keep working with no setup step at all.
 *
 * This activity is the LAUNCHER entry rather than a screen inside
 * GoldenEyeActivity because that one is a NativeActivity: it hands its window
 * surface to native Vulkan rendering and starts android_main from onCreate, so
 * it cannot host ordinary focusable Views. When the data is already usable we
 * forward to it immediately and finish, keeping ourselves off the back stack.
 */
public class GameSetupActivity extends Activity {
    private static final String TAG = "GESETUP";

    /** Path file, relative to getExternalFilesDir(null). Mirrors ge_app.h. */
    private static final String PATH_FILE = "user/ge_game_path.txt";

    /** Cheapest reliable "is this a GoldenEye dump?" probe. */
    private static final String MARKER = "default.xex";

    /**
     * Report the native startup check writes when required files are missing,
     * and deletes on a clean boot (src/ge_asset_check.cpp). Its presence means
     * the previous launch was rejected, so we show the picker instead of
     * forwarding into the game -- otherwise a half-copied dump is a dead end:
     * the native error screen is a non-focusable overlay with no way to choose
     * a different folder.
     */
    private static final String REPORT_FILE = "user/ge_missing_files.txt";

    /**
     * One known file per subtree of the dump, checked when a folder is picked.
     *
     * The authoritative check is native and covers all 1800 files; this is just
     * enough to reject an obviously wrong or half-copied folder at pick time,
     * while the user is still on a screen that can ask them to pick again.
     */
    private static final String[] SAMPLE_FILES = {
        "default.xex",
        "music.xwb",
        "sfx.xwb",
        "files/loc/english/arch/default.str",
        "files/misc/alps3/default.dt",
        "files/new/background/archives/default.bin",
        "files/original/background/archives/default.bin",
        "files/texture/ammo_icon_9mm/default.rba",
    };

    private static final int REQ_ALL_FILES = 1001;
    private static final int REQ_PICK_TREE = 1002;

    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Re-checked on every launch, not just the first: the chosen folder can
        // vanish (SD card pulled, files moved, permission revoked in Settings),
        // and when it does the user should land back here rather than on the
        // native "missing files" error screen with no way to fix it.
        String rejected = readRejectionSummary();
        if (rejected == null && resolveGameRoot() != null) {
            launchGame();
            return;
        }
        buildUi();
        if (rejected != null) {
            setStatus(rejected);
        }
    }

    /**
     * Summary of the last native asset-check failure, or null if the previous
     * boot was clean. Deleted by the native check on success, and by us as soon
     * as a different folder is chosen.
     */
    private String readRejectionSummary() {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            return null;
        }
        File f = new File(ext, REPORT_FILE);
        if (!f.isFile()) {
            return null;
        }
        StringBuilder sb = new StringBuilder("The last launch was stopped: ");
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(new java.io.FileInputStream(f),
                                              StandardCharsets.UTF_8))) {
            String first = r.readLine();          // "<n> required game file(s) missing:"
            sb.append(first != null ? first : "the game folder is incomplete.");
            int shown = 0;
            String line;
            while (shown < 5 && (line = r.readLine()) != null) {
                sb.append('\n').append(line);
                shown++;
            }
        } catch (Throwable t) {
            return "The last launch was stopped: the game folder is incomplete.";
        }
        sb.append("\n\nSelect a complete dump.");
        return sb.toString();
    }

    private void clearRejection() {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            return;
        }
        // Best-effort: a leftover report only costs one extra trip through this
        // screen, and the native check deletes it on the next clean boot.
        new File(ext, REPORT_FILE).delete();
    }

    // --- resolution --------------------------------------------------------

    /**
     * The game data folder to boot from, or null when setup is still needed.
     *
     * Order matters, and MUST match GeApp::ApplyChosenGameRoot (src/ge_app.h):
     * an explicitly chosen folder wins, and the app's own external files dir is
     * the fallback. Disagreeing with native here would mean this screen
     * validates one folder while the guest boots from another.
     *
     * The fallback is what keeps an existing adb-push install working: no saved
     * path, dump already in place, so this screen forwards straight through and
     * never asks for a storage permission.
     */
    private File resolveGameRoot() {
        String saved = readSavedPath();
        if (saved != null) {
            File dir = new File(saved);
            if (hasFile(dir, MARKER) && dir.canRead()) {
                return dir;
            }
            Log.w(TAG, "saved game path no longer usable: " + saved);
        }
        File ext = getExternalFilesDir(null);
        if (ext != null && hasFile(ext, MARKER)) {
            return ext;
        }
        return null;
    }

    /**
     * Does dir contain this relative path, ignoring case?
     *
     * The native check is deliberately case-insensitive, because the guest VFS
     * looks host files up that way (src/ge_asset_check.cpp). Java's File is
     * case-SENSITIVE on ext4/FUSE, so a plain new File(dir, rel).isFile() would
     * reject a dump whose extractor preserved e.g. "Files/" or "DEFAULT.XEX" --
     * a dump the guest would then have loaded perfectly well. Rejecting it here
     * would be a dead end, since this screen is what gates reaching the game.
     *
     * Tries the exact name first, so the ordinary all-lowercase dump costs no
     * directory listings at all.
     */
    private static boolean hasFile(File dir, String relative) {
        File cur = dir;
        for (String part : relative.split("/")) {
            if (part.isEmpty()) {
                continue;
            }
            File exact = new File(cur, part);
            if (exact.exists()) {
                cur = exact;
                continue;
            }
            File[] children = cur.listFiles();
            File hit = null;
            if (children != null) {
                for (File c : children) {
                    if (c.getName().equalsIgnoreCase(part)) {
                        hit = c;
                        break;
                    }
                }
            }
            if (hit == null) {
                return false;
            }
            cur = hit;
        }
        return cur.isFile();
    }

    private String readSavedPath() {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            return null;
        }
        File f = new File(ext, PATH_FILE);
        if (!f.isFile()) {
            return null;
        }
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(new java.io.FileInputStream(f),
                                              StandardCharsets.UTF_8))) {
            String line = r.readLine();
            if (line == null) {
                return null;
            }
            line = line.trim();
            return line.isEmpty() ? null : line;
        } catch (Throwable t) {
            Log.w(TAG, "could not read " + f, t);
            return null;
        }
    }

    private boolean saveChosenPath(File dir) {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            return false;
        }
        File f = new File(ext, PATH_FILE);
        File parent = f.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            Log.e(TAG, "could not create " + parent);
            return false;
        }
        try (OutputStreamWriter w = new OutputStreamWriter(new FileOutputStream(f),
                                                           StandardCharsets.UTF_8)) {
            w.write(dir.getAbsolutePath());
            w.write('\n');
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "could not write " + f, t);
            return false;
        }
    }

    private void launchGame() {
        Intent intent = new Intent(this, GoldenEyeActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();  // never sit on the back stack behind the game
    }

    // --- UI ----------------------------------------------------------------

    private void buildUi() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setBackgroundColor(Color.BLACK);
        int pad = dp(24);
        col.setPadding(pad, pad, pad, pad);

        col.addView(text("GoldenEye 007", 22, Color.WHITE, true));
        col.addView(spacer(dp(12)));
        col.addView(text(
            "No game files found.\n\n"
            + "This app ships no game data — you supply your own GoldenEye 007 "
            + "(Xbox 360 / XBLA) dump.\n\n"
            + "1. Copy the dump anywhere on this device's storage (e.g. a "
            + "\"GoldenEye\" folder in Download). Any file manager will do — it "
            + "does NOT have to go in Android/data.\n"
            + "2. Tap below and select that folder.\n\n"
            + "The folder should contain " + MARKER + ", the music/sfx banks, and a "
            + "\"files\" subfolder. Nothing is copied — the game reads it where it is, "
            + "so keep the folder in place.",
            14, 0xFFCCCCCC, false));
        col.addView(spacer(dp(20)));

        Button choose = new Button(this);
        choose.setText("Select game folder");
        choose.setOnClickListener(v -> onChooseClicked());
        col.addView(choose);

        col.addView(spacer(dp(16)));
        status = text("", 13, 0xFFFFAA55, false);
        col.addView(status);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.BLACK);
        scroll.addView(col, new ScrollView.LayoutParams(
            ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView tv = new TextView(this);
        tv.setText(s);
        tv.setTextColor(color);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold) {
            tv.setTypeface(tv.getTypeface(), android.graphics.Typeface.BOLD);
        }
        tv.setGravity(Gravity.START);
        tv.setLayoutParams(new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return tv;
    }

    private View spacer(int h) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, h));
        return v;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                                                    getResources().getDisplayMetrics()));
    }

    private void setStatus(String s) {
        if (status != null) {
            status.setText(s);
        }
    }

    // --- permission + picker flow ------------------------------------------

    /**
     * Step 1: storage read access. The picker itself only grants access through
     * SAF, but the guest reads the dump with ordinary POSIX calls from native
     * code, so we need real filesystem access to the chosen path.
     */
    private void onChooseClicked() {
        setStatus("");
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            // Android 10 (minSdk 29) has no MANAGE_EXTERNAL_STORAGE, and since we
            // target 34 it gets scoped storage regardless of
            // requestLegacyExternalStorage (ignored above target 30).
            // READ_EXTERNAL_STORAGE would only reach media files, never a .xex or
            // .xwb -- so there is no way to read a dump outside our own folder
            // here. Say so rather than sending the user round a loop that cannot
            // succeed.
            setStatus(adbInstructions("Picking a folder on-device needs Android 11 "
                                      + "or newer."));
            return;
        }
        if (!Environment.isExternalStorageManager()) {
            requestAllFilesAccess();
            return;
        }
        launchFolderPicker();
    }

    private void requestAllFilesAccess() {
        setStatus("Grant “All files access”, then come back and tap the button "
                  + "again.\n\nThe game needs it to read the dump straight from "
                  + "storage instead of copying ~700 MB into its own folder.");
        // The per-app screen deep-links straight to our entry; the generic list
        // is the fallback for devices whose Settings does not handle it.
        Intent perApp = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                   Uri.parse("package:" + getPackageName()));
        Intent generic = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
        if (!startSettings(perApp, REQ_ALL_FILES) && !startSettings(generic, REQ_ALL_FILES)) {
            setStatus(adbInstructions("This device has no “All files access” "
                                     + "settings screen."));
        }
    }

    /** Fallback advice: the adb route needs no permission of any kind. */
    private String adbInstructions(String reason) {
        return reason + "\n\nCopy the dump into the app's own folder from a PC "
             + "instead — that needs no permission:\n\n"
             + "adb push <dump>/. /sdcard/Android/data/" + getPackageName() + "/files/";
    }

    private boolean startSettings(Intent intent, int requestCode) {
        try {
            startActivityForResult(intent, requestCode);
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "settings intent not handled: " + intent.getAction(), t);
            return false;
        }
    }

    private void launchFolderPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        try {
            startActivityForResult(intent, REQ_PICK_TREE);
        } catch (Throwable t) {
            Log.e(TAG, "no folder picker available", t);
            setStatus("No folder picker available on this device.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ALL_FILES) {
            // resultCode is RESULT_CANCELED even on success (the Settings screen
            // returns nothing), so ask the system for the real answer.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && Environment.isExternalStorageManager()) {
                setStatus("");
                launchFolderPicker();
            } else {
                setStatus("“All files access” was not granted. Tap the button to "
                          + "try again.");
            }
            return;
        }
        if (requestCode == REQ_PICK_TREE) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                setStatus("No folder selected.");
                return;
            }
            onFolderPicked(data.getData());
        }
    }

    private void onFolderPicked(Uri treeUri) {
        String path = treeUriToPath(treeUri);
        if (path == null) {
            setStatus("That folder is not on this device's storage.\n\n"
                      + "Pick a folder under Internal storage or an SD card — not "
                      + "Drive, Downloads-provider shortcuts, or another app's "
                      + "content.");
            return;
        }
        File dir = findDumpRoot(new File(path));
        if (dir == null) {
            setStatus("No " + MARKER + " in:\n" + path + "\n\n"
                      + "Select the folder that directly contains " + MARKER + ".");
            return;
        }
        if (!dir.canRead()) {
            setStatus("Cannot read:\n" + dir.getAbsolutePath()
                      + "\n\nCheck that “All files access” is still granted.");
            return;
        }
        String incomplete = firstMissingSample(dir);
        if (incomplete != null) {
            setStatus("That folder is missing " + incomplete + ".\n\n"
                      + dir.getAbsolutePath() + "\n\n"
                      + "It looks like an incomplete dump — copy the whole thing "
                      + "and select it again.");
            return;
        }
        clearRejection();
        if (!saveChosenPath(dir)) {
            setStatus("Could not save the chosen folder.");
            return;
        }
        Log.i(TAG, "game data root set to " + dir);
        launchGame();
    }

    /** First entry of SAMPLE_FILES absent from dir, or null when all are present. */
    private static String firstMissingSample(File dir) {
        for (String rel : SAMPLE_FILES) {
            if (!hasFile(dir, rel)) {
                return rel;
            }
        }
        return null;
    }

    /**
     * Accept either the dump root itself or its immediate parent.
     *
     * Dumps commonly unpack into a wrapper folder ("GoldenEye 007 XBLA"), and
     * picking the wrapper's parent is an easy mistake; descending one level
     * when exactly one child looks like a dump avoids a dead end. Exactly one,
     * so an ambiguous folder full of candidates still reports an error rather
     * than silently guessing.
     */
    private static File findDumpRoot(File picked) {
        if (hasFile(picked, MARKER)) {
            return picked;
        }
        File[] children = picked.listFiles();
        if (children == null) {
            return null;
        }
        File match = null;
        for (File c : children) {
            if (c.isDirectory() && hasFile(c, MARKER)) {
                if (match != null) {
                    return null;  // ambiguous
                }
                match = c;
            }
        }
        return match;
    }

    /**
     * Convert a SAF tree URI into the filesystem path native code will open.
     *
     * Only external-storage documents map to a real path; anything else (Drive,
     * MediaDocuments, another app's provider) has no POSIX path and returns
     * null. Tree document IDs are "<volume>:<relative/path>", where the volume
     * is "primary" for internal storage or a volume UUID for removable media.
     */
    private String treeUriToPath(Uri treeUri) {
        try {
            if (!"com.android.externalstorage.documents".equals(treeUri.getAuthority())) {
                return null;
            }
            String docId = DocumentsContract.getTreeDocumentId(treeUri);
            if (docId == null) {
                return null;
            }
            String[] parts = docId.split(":", 2);
            String volume = parts[0];
            String relative = parts.length > 1 ? parts[1] : "";
            File base;
            if ("primary".equalsIgnoreCase(volume)) {
                base = Environment.getExternalStorageDirectory();
            } else {
                base = removableVolumeDir(volume);
            }
            if (base == null) {
                return null;
            }
            return relative.isEmpty() ? base.getAbsolutePath()
                                      : new File(base, relative).getAbsolutePath();
        } catch (Throwable t) {
            Log.w(TAG, "could not map tree uri " + treeUri, t);
            return null;
        }
    }

    /**
     * Mount point for a removable volume UUID. Ask StorageManager first (it
     * knows the real mount, which is not always /storage/<uuid>) and fall back
     * to the conventional path when the lookup comes up empty.
     */
    private File removableVolumeDir(String uuid) {
        try {
            StorageManager sm = getSystemService(StorageManager.class);
            if (sm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                for (StorageVolume v : sm.getStorageVolumes()) {
                    if (uuid.equalsIgnoreCase(v.getUuid())) {
                        File dir = v.getDirectory();
                        if (dir != null) {
                            return dir;
                        }
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "StorageManager lookup failed for " + uuid, t);
        }
        File fallback = new File("/storage/" + uuid);
        return fallback.isDirectory() ? fallback : null;
    }
}
