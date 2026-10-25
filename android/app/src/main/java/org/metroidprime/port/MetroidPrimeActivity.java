package org.metroidprime.port;

import android.Manifest;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.Settings;
import android.util.Log;

import dev.encounter.aurora.AuroraSurface;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MetroidPrimeActivity extends SDLActivity {
    private static final String TAG = "MetroidPrimePort";
    // Distinct from SDL's own dialog request codes, which count up from 0.
    private static final int REQUEST_TEXTURE_PACK = 0x7e57;
    private static final int REQUEST_STORAGE = 0x7e58;
    // The Remastered import's .nsp/.xci (+0) and prod.keys (+1), a PAL disc for its
    // languages (+2) and a disc for the full Flaahgra theme (+3).
    private static final int REQUEST_REMASTERED = 0x7e59;
    // A disc picked after Android destroyed the activity behind the picker
    // reaches a new launch, which drops it: SDL's dialog state died with the old
    // process. The picker's request code is kept here, and such a pick is left
    // in PENDING_DISC_FILE for the native side's next start (platform/main.cpp).
    private static final String DISC_PICK_PREFS = "disc_pick";
    private static final String PENDING_DISC_FILE = "pending_disc_uri";
    private TouchControlsView touchControls;
    private final AtomicBoolean texturePackCopying = new AtomicBoolean();
    // The data folder the texture pack being picked is copied into.
    private volatile String texturePackFolder;

    // Implemented in platform/debug_ui.cpp.
    private static native void nativeTexturePackStatus(String status);
    private static native void nativeTexturePackReady();
    private static native void nativeRemasteredPicked(int which, String uri);
    // Implemented in src/Kyoto/Graphics/DolphinCGraphics.cpp.
    private static native boolean nativeFramePresented();
    // Implemented in platform/port_memory_log.cpp; level -1 is onLowMemory.
    private static native void nativeMemoryWarning(int level, long availableKb, long thresholdKb, boolean low);

    // How the previous run ended, for the log (issue #102): a low-memory kill leaves
    // no crash report, so the system's exit record is the only trace. Read by
    // port_memory_log.cpp once the log runs.
    private static volatile String exitReport = "";

    static String exitReport() {
        return exitReport;
    }

    // ActivityManager.isLowRamDevice() (Android Go and the like), read by
    // port_memory_log.cpp to build pipelines on first use instead of at startup.
    private static volatile boolean lowRamDevice = false;

    static boolean lowRamDevice() {
        return lowRamDevice;
    }

    private String buildExitReport() {
        StringBuilder report = new StringBuilder();
        ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) {
            return "";
        }
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(memory);
        lowRamDevice = manager.isLowRamDevice();
        report.append(String.format(Locale.ROOT,
            "port: device memory: %d MiB, %d MiB available, low-memory threshold %d MiB, app heap class %d MiB",
            memory.totalMem >> 20, memory.availMem >> 20, memory.threshold >> 20, manager.getMemoryClass()));
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            report.append("\nport: previous run: exit reasons need Android 11");
            return report.toString();
        }
        ApplicationExitInfo last = null;
        for (ApplicationExitInfo info : manager.getHistoricalProcessExitReasons(getPackageName(), 0, 8)) {
            if (getPackageName().equals(info.getProcessName())) {
                last = info;
                break;
            }
        }
        if (last == null) {
            report.append("\nport: previous run: no exit record");
            return report.toString();
        }
        report.append(String.format(Locale.ROOT,
            "\nport: previous run: ended %s, %s (status %d, importance %d), memory pss %d MiB rss %d MiB",
            new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new java.util.Date(last.getTimestamp())),
            exitReasonName(last.getReason()), last.getStatus(), last.getImportance(),
            last.getPss() >> 10, last.getRss() >> 10));
        if (last.getDescription() != null) {
            report.append("\nport: previous run: ").append(last.getDescription());
        }
        // An ANR's trace is text (a native crash's is a protobuf tombstone): its head
        // says where the main thread was stuck.
        if (last.getReason() == ApplicationExitInfo.REASON_ANR) {
            try (InputStream trace = last.getTraceInputStream()) {
                if (trace != null) {
                    BufferedReader reader = new BufferedReader(new java.io.InputStreamReader(trace));
                    String line;
                    for (int i = 0; i < 60 && (line = reader.readLine()) != null; ++i) {
                        report.append("\n  ").append(line);
                    }
                }
            } catch (IOException e) {
                Log.w(TAG, "Could not read the ANR trace", e);
            }
        }
        return report.toString();
    }

    @android.annotation.TargetApi(Build.VERSION_CODES.R)
    private static String exitReasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "exited by itself";
            case ApplicationExitInfo.REASON_SIGNALED: return "killed by a signal";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "killed for low memory";
            case ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case ApplicationExitInfo.REASON_ANR: return "not responding (ANR)";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "initialization failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resource usage";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "user requested (force stop)";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency died";
            case ApplicationExitInfo.REASON_OTHER: return "other (system)";
            case 14: return "freezer"; // REASON_FREEZER, API 33
            case 15: return "package state change"; // API 34
            case 16: return "package updated"; // API 34
            default: return "unknown reason " + reason;
        }
    }

    private void reportMemoryWarning(int level) {
        ActivityManager manager = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        if (manager != null) {
            manager.getMemoryInfo(memory);
        }
        try {
            nativeMemoryWarning(level, memory.availMem >> 10, memory.threshold >> 10, memory.lowMemory);
        } catch (UnsatisfiedLinkError e) {
            // The library failed to load: no log to write to.
        }
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        reportMemoryWarning(level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        reportMemoryWarning(-1);
    }

    // The surface stays black until the game presents its first frame, about
    // 2 s after the system splash on a phone (GPU setup, mod scan, pipeline
    // seed). Keeps the app icon up over it until then.
    private void showStartupCover() {
        if (nativeFramePresentedSafe()) {
            return;
        }
        final android.widget.FrameLayout cover = new android.widget.FrameLayout(this);
        cover.setBackgroundColor(0xFF000000);
        android.widget.ImageView icon = new android.widget.ImageView(this);
        icon.setImageResource(R.mipmap.ic_launcher);
        int size = Math.round(160 * getResources().getDisplayMetrics().density);
        cover.addView(icon, new android.widget.FrameLayout.LayoutParams(size, size, android.view.Gravity.CENTER));
        mLayout.addView(cover, new android.widget.RelativeLayout.LayoutParams(
            android.widget.RelativeLayout.LayoutParams.MATCH_PARENT,
            android.widget.RelativeLayout.LayoutParams.MATCH_PARENT));
        cover.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (nativeFramePresentedSafe()) {
                    cover.animate().alpha(0f).setDuration(150).withEndAction(() -> mLayout.removeView(cover));
                } else {
                    cover.postDelayed(this, 30);
                }
            }
        }, 30);
    }

    // The library is loaded by SDLActivity.onCreate; if that failed, there's
    // no native side to wait for.
    private static boolean nativeFramePresentedSafe() {
        try {
            return nativeFramePresented();
        } catch (UnsatisfiedLinkError e) {
            return true;
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"metroid_prime_port"};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new AuroraSurface(context);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            exitReport = buildExitReport();
        } catch (RuntimeException e) {
            Log.w(TAG, "Could not read the previous exit", e);
        }
        try {
            // Cleared first: copying only adds, so an icon a newer APK no longer
            // ships would keep replacing a texture it no longer claims.
            File textures = new File(getFilesDir(), "textures");
            deleteTree(textures);
            copyAssetTree("textures", textures);
            copyAssetFile("initial_pipeline_cache.db", new File(getFilesDir(), "initial_pipeline_cache.db"));
        } catch (IOException e) {
            Log.e(TAG, "Failed to prepare native resources", e);
        }
        super.onCreate(savedInstanceState);
        if (mLayout != null) {
            touchControls = new TouchControlsView(this);
            mLayout.addView(touchControls, new android.widget.RelativeLayout.LayoutParams(
                android.widget.RelativeLayout.LayoutParams.MATCH_PARENT,
                android.widget.RelativeLayout.LayoutParams.MATCH_PARENT));
            showStartupCover();
        }
        preferHighestRefreshRate();
        warnIfDataFolderUnreachable();
    }

    // Android runs an app at 60 Hz on a 90/120 Hz panel unless the window asks
    // for more, and the swapchain is always Fifo here, so without this the
    // uncapped frame rate stops at 60. Picks the fastest mode at the current
    // resolution; the 60 FPS cap still paces frames when it is on.
    private void preferHighestRefreshRate() {
        android.view.Display display = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
            ? getDisplay() : getWindowManager().getDefaultDisplay();
        if (display == null) {
            return;
        }
        android.view.Display.Mode current = display.getMode();
        android.view.Display.Mode best = current;
        for (android.view.Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                && mode.getPhysicalHeight() == current.getPhysicalHeight()
                && mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        android.view.WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(params);
        Log.i(TAG, String.format(Locale.ROOT, "Display mode %d: %dx%d at %.1f Hz (was %.1f Hz)",
            best.getModeId(), best.getPhysicalWidth(), best.getPhysicalHeight(),
            best.getRefreshRate(), current.getRefreshRate()));
    }

    // The data was moved to shared storage (port_paths.h reads the same file),
    // but the permission to reach it is gone - revoked, or the app was
    // reinstalled. The game falls back to app storage; say so before it starts
    // rather than letting the saves look lost.
    private void warnIfDataFolderUnreachable() {
        File marker = new File(getFilesDir(), "data_folder.txt");
        if (!marker.isFile() || hasStorageAccess()) {
            return;
        }
        String folder = "";
        try (BufferedReader reader = new BufferedReader(new FileReader(marker))) {
            String line = reader.readLine();
            folder = line != null ? line.trim() : "";
        } catch (IOException e) {
            Log.w(TAG, "Could not read " + marker, e);
        }
        new AlertDialog.Builder(this)
            .setTitle("Data folder not reachable")
            .setMessage("Your saves and settings are in " + folder + ", which the game may not open "
                + "without the \"All files access\" permission. Until it is allowed and the game "
                + "restarted, the copy in app storage is used.")
            .setPositiveButton("Allow access", (dialog, which) -> requestStorageAccess())
            .setNegativeButton("Not now", null)
            .show();
    }

    // Whether native code may read and write shared storage with plain file
    // calls. Called from port_data_folder.cpp.
    public boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
            == PackageManager.PERMISSION_GRANTED;
    }

    // Opens the system's "All files access" page (Android 11+) or asks for the
    // storage permission (9-10).
    public void requestStorageAccess() {
        runOnUiThread(() -> {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
                requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_STORAGE);
                return;
            }
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } catch (android.content.ActivityNotFoundException e) {
                // Some builds only have the list of all apps.
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (android.content.ActivityNotFoundException e2) {
                    Log.w(TAG, "No settings page for all files access", e2);
                }
            }
        });
    }

    // Starts the game again in a new process (RestartActivity), so the data
    // folder chosen in the overlay is picked up.
    public void restartApp() {
        runOnUiThread(() -> {
            Intent intent = new Intent(this, RestartActivity.class);
            intent.putExtra(RestartActivity.EXTRA_PID, Process.myPid());
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
        });
    }

    @Override
    protected void onPause() {
        if (touchControls != null) {
            touchControls.releaseAll();
        }
        super.onPause();
    }

    // Called from the debug overlay, on the SDL thread, with the data folder
    // the pack goes into.
    public void pickTexturePack(String folder) {
        runOnUiThread(() -> {
            if (texturePackCopying.get()) {
                nativeTexturePackStatus("A texture pack is still being copied.");
                return;
            }
            texturePackFolder = folder;
            try {
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_TEXTURE_PACK);
            } catch (android.content.ActivityNotFoundException e) {
                nativeTexturePackStatus("This device has no folder picker.");
            }
        });
    }

    // Called from the debug overlay, on the SDL thread: picks the Remastered
    // .nsp/.xci (which 0), prod.keys (which 1) or a PAL disc image for its
    // languages (which 2), a disc image for the full Flaahgra theme (which 3) or a Metroid Dread
    // .nsp/.xci for the Dread Samus import (which 4). None has a MIME type of its own.
    public void pickRemasteredFile(int which) {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*");
            try {
                startActivityForResult(intent, REQUEST_REMASTERED + which);
            } catch (android.content.ActivityNotFoundException e) {
                Log.w(TAG, "No document picker", e);
            }
        });
    }

    // Called from photo mode (port_photo.cpp), on the SDL thread, once a photo is
    // written: moves it into the shared Pictures/Metroid Prime folder, where the
    // gallery sees it. Returns where it went, or null (the file is then kept).
    public String exportPhoto(String path) {
        File source = new File(path);
        String folder = Environment.DIRECTORY_PICTURES + "/Metroid Prime";
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ContentResolver resolver = getContentResolver();
                ContentValues values = new ContentValues();
                values.put(MediaStore.Images.Media.DISPLAY_NAME, source.getName());
                values.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
                values.put(MediaStore.Images.Media.RELATIVE_PATH, folder);
                values.put(MediaStore.Images.Media.IS_PENDING, 1);
                Uri uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
                if (uri == null) {
                    return null;
                }
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = resolver.openOutputStream(uri)) {
                    if (output == null) {
                        throw new IOException("No stream for " + uri);
                    }
                    copyStream(input, output);
                } catch (IOException e) {
                    resolver.delete(uri, null, null);
                    throw e;
                }
                values.clear();
                values.put(MediaStore.Images.Media.IS_PENDING, 0);
                resolver.update(uri, values, null, null);
            } else {
                // Android 9 has no MediaStore folders: a plain file, then a scan for the gallery.
                File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
                                    "Metroid Prime");
                if (!dir.isDirectory() && !dir.mkdirs()) {
                    throw new IOException("Failed to create " + dir);
                }
                File destination = new File(dir, source.getName());
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = new FileOutputStream(destination)) {
                    copyStream(input, output);
                }
                MediaScannerConnection.scanFile(this, new String[]{destination.getPath()},
                                                new String[]{"image/png"}, null);
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Could not move " + source + " to " + folder, e);
            return null;
        }
        source.delete();
        return folder + "/" + source.getName();
    }

    private static void copyStream(InputStream input, OutputStream output) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int count;
        while ((count = input.read(buffer)) != -1) {
            output.write(buffer, 0, count);
        }
    }

    // The pick is written down before native hears of it: the game is often
    // killed behind the picker for its memory, and then this runs in a new
    // process whose overlay reads the file (RemasteredPickFile in debug_ui.cpp).
    private void rememberRemasteredPick(int which, Uri uri) {
        persistUri(uri);
        File file = new File(getFilesDir(), "remastered_pick_" + which + ".txt");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write((uri.toString() + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "Could not write " + file, e);
        }
        try {
            nativeRemasteredPicked(which, uri.toString());
        } catch (UnsatisfiedLinkError e) {
            // The library did not load; the file is read once it does.
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_TEXTURE_PACK) {
            Uri tree = data != null && resultCode == RESULT_OK ? data.getData() : null;
            if (tree != null) {
                copyTexturePack(tree);
            }
            return;
        }
        if (requestCode >= REQUEST_REMASTERED && requestCode <= REQUEST_REMASTERED + 4) {
            Uri uri = data != null && resultCode == RESULT_OK ? data.getData() : null;
            if (uri != null) {
                rememberRemasteredPick(requestCode - REQUEST_REMASTERED, uri);
            }
            return;
        }
        android.content.SharedPreferences discPick = getSharedPreferences(DISC_PICK_PREFS, MODE_PRIVATE);
        if (discPick.getInt("request", Integer.MIN_VALUE) == requestCode) {
            discPick.edit().remove("request").commit();
            Uri disc = data != null && resultCode == RESULT_OK ? data.getData() : null;
            if (disc != null && !isFileDialogPending(requestCode)) {
                persistUri(disc);
                try (OutputStream out = new FileOutputStream(new File(getFilesDir(), PENDING_DISC_FILE))) {
                    out.write(disc.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    Log.i(TAG, "Kept a disc picked before the activity was recreated: " + disc);
                } catch (IOException e) {
                    Log.w(TAG, "Could not keep the disc picked before the activity was recreated", e);
                }
                return;
            }
        }
        if (data != null) {
            persistUri(data.getData());
            ClipData clips = data.getClipData();
            if (clips != null) {
                for (int i = 0; i < clips.getItemCount(); ++i) {
                    persistUri(clips.getItemAt(i).getUri());
                }
            }
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // Remembers which dialog is the disc picker (opening, filtered to disc
    // images) in storage that outlives the process. commit, not apply: the
    // process can be gone before an apply is written.
    @Override
    protected void onFileDialogShown(String[] filters, boolean forWrite, int requestCode) {
        boolean disc = false;
        if (!forWrite && filters != null) {
            for (String filter : filters) {
                disc |= filter != null && filter.contains("rvz");
            }
        }
        android.content.SharedPreferences.Editor edit = getSharedPreferences(DISC_PICK_PREFS, MODE_PRIVATE).edit();
        if (disc) {
            edit.putInt("request", requestCode);
        } else {
            edit.remove("request");
        }
        edit.commit();
    }

    private void persistUri(Uri uri) {
        if (uri == null) {
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "Document provider did not grant persistent access to " + uri, e);
        }
    }

    // Copies the picked folder's textures into <data>/user_textures.new, which
    // the native side swaps in for user_textures on its next frame. A copy rather
    // than reading through the URI: the grant can be revoked, and the loader
    // needs real paths. The built-in set is never touched.
    private void copyTexturePack(Uri tree) {
        if (!texturePackCopying.compareAndSet(false, true)) {
            return;
        }
        nativeTexturePackStatus("Copying the texture pack...");
        new Thread(() -> {
            String folder = texturePackFolder;
            File root = folder != null && !folder.isEmpty() ? new File(folder) : getFilesDir();
            File staging = new File(root, "user_textures.partial");
            File ready = new File(root, "user_textures.new");
            try {
                deleteTree(staging);
                int[] copied = {0};
                String rootId = DocumentsContract.getTreeDocumentId(tree);
                copyDocumentTree(tree, rootId, staging, copied);
                if (copied[0] == 0) {
                    deleteTree(staging);
                    nativeTexturePackStatus("No .png or .dds textures in that folder; nothing changed.");
                    return;
                }
                deleteTree(ready);
                if (!staging.renameTo(ready)) {
                    throw new IOException("Failed to rename " + staging + " to " + ready);
                }
                nativeTexturePackStatus(String.format(Locale.ROOT, "Copied %d textures.", copied[0]));
                nativeTexturePackReady();
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Failed to copy the texture pack", e);
                deleteTree(staging);
                nativeTexturePackStatus("Copying the texture pack failed: " + e.getMessage());
            } finally {
                texturePackCopying.set(false);
            }
        }, "texture-pack-copy").start();
    }

    private void copyDocumentTree(Uri tree, String parentId, File destination, int[] copied)
            throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) {
                throw new IOException("Cannot list " + children);
            }
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                // A name from the provider becomes a path component here.
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")
                        || name.contains("/")) {
                    continue;
                }
                File target = new File(destination, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    copyDocumentTree(tree, id, target, copied);
                    continue;
                }
                String lower = name.toLowerCase(Locale.ROOT);
                if (!lower.endsWith(".png") && !lower.endsWith(".dds")) {
                    continue;
                }
                if (!destination.isDirectory() && !destination.mkdirs()) {
                    throw new IOException("Failed to create " + destination);
                }
                Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                try (InputStream input = getContentResolver().openInputStream(document);
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) {
                        throw new IOException("Cannot open " + document);
                    }
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                    }
                }
                if (++copied[0] % 100 == 0) {
                    nativeTexturePackStatus(String.format(Locale.ROOT,
                        "Copying the texture pack... %d textures", copied[0]));
                }
            }
        }
    }

    private void copyAssetTree(String assetPath, File destination) throws IOException {
        AssetManager assets = getAssets();
        String[] children = assets.list(assetPath);
        if (children == null || children.length == 0) {
            copyAssetFile(assetPath, destination);
            return;
        }
        if (!destination.isDirectory() && !destination.mkdirs()) {
            throw new IOException("Failed to create " + destination);
        }
        for (String child : children) {
            copyAssetTree(assetPath + "/" + child, new File(destination, child));
        }
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }

    private void copyAssetFile(String assetPath, File destination) throws IOException {
        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Failed to create " + parent);
        }
        try (InputStream input = getAssets().open(assetPath);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
        }
    }
}
