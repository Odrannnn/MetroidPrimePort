package org.metroidprime.port;

import android.Manifest;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;

import dev.encounter.aurora.AuroraSurface;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MetroidPrimeActivity extends SDLActivity {
    private static final String TAG = "MetroidPrimePort";
    // Distinct from SDL's own dialog request codes, which count up from 0.
    private static final int REQUEST_TEXTURE_PACK = 0x7e57;
    private static final int REQUEST_STORAGE = 0x7e58;
    // The Remastered import's .nsp/.xci (+0) and prod.keys (+1).
    private static final int REQUEST_REMASTERED = 0x7e59;
    private TouchControlsView touchControls;
    private final AtomicBoolean texturePackCopying = new AtomicBoolean();
    // The data folder the texture pack being picked is copied into.
    private volatile String texturePackFolder;

    // Implemented in platform/debug_ui.cpp.
    private static native void nativeTexturePackStatus(String status);
    private static native void nativeTexturePackReady();
    private static native void nativeRemasteredPicked(int which, String uri);

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
    // languages (which 2). None has a MIME type of its own.
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
        if (requestCode >= REQUEST_REMASTERED && requestCode <= REQUEST_REMASTERED + 2) {
            Uri uri = data != null && resultCode == RESULT_OK ? data.getData() : null;
            if (uri != null) {
                rememberRemasteredPick(requestCode - REQUEST_REMASTERED, uri);
            }
            return;
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
