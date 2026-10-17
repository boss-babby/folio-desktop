package ai.folio.reader;

import android.app.NativeActivity;
import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.content.UriPermission;
import android.system.Os;
import android.system.OsConstants;
import android.app.AlertDialog;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowInsets;
import android.graphics.Color;
import android.widget.FrameLayout;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.text.InputType;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.security.MessageDigest;

/** Android-only bridges for the native Skia UI. No Android view paints the UI. */
public final class FolioActivity extends NativeActivity {
    static { System.loadLibrary("folio-android"); }
    private static final int OPEN_PDF = 41;
    private static final int SAVE_PDF = 42;
    private static final int RELINK_PDF = 43;
    private String pendingRelink;
    private String stagedExport;
    private View keyboardTarget;
    private volatile boolean systemBarsDark;
    // NativeActivity calls Window.takeSurface(): EGL renders into the WINDOW,
    // not into a SurfaceView. A SurfaceView search always returned null, so
    // the old native safe-area callback was never delivered on this phone.
    private FrameLayout nativeContent;
    private View statusBuffer;
    private int childStatusInset;
    private int lastNativeSafeTop = -1;
    private int lastBufferHeight = -1;

    private void updateStatusBuffer() {
        if (nativeContent == null || !nativeContent.isAttachedToWindow()) return;
        final Window window = getWindow();
        final View decor = window.getDecorView();
        final WindowInsets root = decor.getRootWindowInsets();
        // This activity explicitly keeps the bar visible. OEMs can report
        // a consumed root inset (including isVisible=false) while still
        // drawing its icons; trust the window fullscreen flag, not that bit.
        final boolean barVisible = (window.getAttributes().flags &
                WindowManager.LayoutParams.FLAG_FULLSCREEN) == 0;

        int barBottom = 0;
        if (barVisible) {
            barBottom = childStatusInset;
            if (root != null) {
                barBottom = Math.max(barBottom,
                        android.os.Build.VERSION.SDK_INT >= 30
                                ? root.getInsets(WindowInsets.Type.statusBars()).top
                                : root.getSystemWindowInsetTop());
            }
            // Some NativeActivity windows report CONSUMED insets even while
            // Android visibly paints the clock. Use the system's real status
            // bar dimension, never a hard-coded dp guess.
            int id = getResources().getIdentifier("status_bar_height", "dimen", "android");
            if (id != 0) barBottom = Math.max(barBottom,
                    getResources().getDimensionPixelSize(id));
        }

        int[] windowPos = new int[2];
        int[] contentPos = new int[2];
        decor.getLocationOnScreen(windowPos);
        nativeContent.getLocationOnScreen(contentPos);
        // EGL is the decor/window surface, not the child content view.
        // These coordinates refer to screen pixels, NOT dp. If Android has
        // already put the window below the bar, both offsets become zero.
        int nativeTop = SystemBarGeometry.uncovered(barBottom, windowPos[1]);
        int bufferHeight = SystemBarGeometry.uncovered(barBottom, contentPos[1]);
        boolean changed = nativeTop != lastNativeSafeTop ||
                bufferHeight != lastBufferHeight;
        if (nativeTop != lastNativeSafeTop) {
            lastNativeSafeTop = nativeTop;
            nativeOnSafeArea(nativeTop);
        }
        if (bufferHeight != lastBufferHeight) {
            lastBufferHeight = bufferHeight;
            FrameLayout.LayoutParams params = (FrameLayout.LayoutParams)
                    statusBuffer.getLayoutParams();
            params.height = bufferHeight;
            statusBuffer.setLayoutParams(params);
        }
        if (changed) android.util.Log.i("folio", "system buffer=" + bufferHeight +
                "px, native top=" + nativeTop + "px, statusBottom=" +
                barBottom + "px, windowScreenY=" + windowPos[1] +
                "px, contentScreenY=" + contentPos[1]);
    }

    private void installStatusBuffer() {
        View root = findViewById(android.R.id.content);
        if (!(root instanceof FrameLayout)) {
            android.util.Log.e("folio", "NativeActivity content is not a FrameLayout");
            return;
        }
        nativeContent = (FrameLayout) root;
        // A real Android view over the status region. Native rendering also
        // paints this strip before clipping Folio's UI below it; a window
        // surface may not composite ordinary views on every Android version.
        statusBuffer = new View(this);
        statusBuffer.setClickable(true);
        statusBuffer.setBackgroundColor(Color.rgb(237, 234, 227));
        nativeContent.addView(statusBuffer, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, 0, Gravity.TOP));
        nativeContent.setOnApplyWindowInsetsListener((view, insets) -> {
            childStatusInset = android.os.Build.VERSION.SDK_INT >= 30
                    ? insets.getInsets(WindowInsets.Type.statusBars()).top
                    : insets.getSystemWindowInsetTop();
            view.post(this::updateStatusBuffer);
            return insets;
        });
        // NativeActivity's window surface does NOT move when content view
        // padding changes. Global layout catches window/inset/rotation/IME
        // changes even if the content container's own bounds do not change.
        nativeContent.getViewTreeObserver().addOnGlobalLayoutListener(
                this::updateStatusBuffer);
        nativeContent.post(this::updateStatusBuffer);
        nativeContent.requestApplyInsets();
    }

    private void applySystemBars() {
        Window window = getWindow();
        // The old Fullscreen theme suppressed time, notifications and the
        // status-bar icons. Let Android own its inset; Folio paints below it.
        window.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN |
                          WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS |
                          WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS |
                          WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION);
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
        // Ask the decor to fit system bars; the independent window-surface
        // buffer below also handles NativeActivity/OEMs that ignore the fit.
        if (android.os.Build.VERSION.SDK_INT >= 30)
            window.setDecorFitsSystemWindows(true);
        window.getDecorView().setFitsSystemWindows(true);
        final int background = systemBarsDark ? Color.rgb(15, 16, 22)
                                              : Color.rgb(237, 234, 227);
        window.setStatusBarColor(background);
        window.setNavigationBarColor(background);
        if (statusBuffer != null) statusBuffer.setBackgroundColor(background);
        View decor = window.getDecorView();
        int flags = decor.getSystemUiVisibility();
        flags &= ~(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                   View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                   View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                   View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE |
                   View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                   View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR |
                   View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        if (!systemBarsDark)
            flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR |
                     View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        decor.setSystemUiVisibility(flags);
        decor.requestApplyInsets();
        View content = findViewById(android.R.id.content);
        if (content != null) content.requestApplyInsets();
        decor.post(this::updateStatusBuffer);
    }

    /** Called from the native shell only when Folio's light/dark setting changes. */
    public void setSystemBarsThemeColor(int argb, boolean darkIcons) {
        runOnUiThread(() -> {
            Window window = getWindow();
            window.setStatusBarColor(argb);
            window.setNavigationBarColor(argb);
            if (statusBuffer != null) statusBuffer.setBackgroundColor(argb);
            View decor = window.getDecorView();
            int flags = decor.getSystemUiVisibility();
            flags &= ~(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR |
                       View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
            if (!darkIcons)
                flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR |
                         View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            if (flags != decor.getSystemUiVisibility())
                decor.setSystemUiVisibility(flags);
            systemBarsDark = darkIcons;
        });
    }

    public void setSystemBarsDark(boolean dark) {
        systemBarsDark = dark;
        runOnUiThread(this::applySystemBars);
    }

    private static native void nativeOnSafeArea(int topPixels);
    private static native void nativeOnPdfResult(String path);
    // status 1 = saved, 0 = cancelled, -1 = failed; message only on failure.
    private static native void nativeOnSaveResult(int status, String message);
    private static native void nativeOnStorageResult(int status, String message);
    // Takes ownership of the detached descriptor; closes it after a fresh
    // PDFium parse. Called only off the Java UI thread.
    private static native boolean nativeValidatePdfFd(int fd);
    private static native void nativeOnText(String text);
    private static native void nativeOnBackspace();
    private static native void nativeOnEnter();

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        installStatusBuffer();
        applySystemBars();
        keyboardTarget = new View(this) {
            @Override public boolean onCheckIsTextEditor() { return true; }
            @Override public InputConnection onCreateInputConnection(EditorInfo info) {
                info.inputType = InputType.TYPE_CLASS_TEXT;
                info.imeOptions = EditorInfo.IME_ACTION_DONE;
                return new BaseInputConnection(this, true) {
                    @Override public boolean commitText(CharSequence text, int position) {
                        if (text != null && text.length() > 0) nativeOnText(text.toString());
                        return true;
                    }
                    @Override public boolean deleteSurroundingText(int before, int after) {
                        for (int i = 0; i < before; ++i) nativeOnBackspace();
                        return true;
                    }
                    @Override public boolean performEditorAction(int action) {
                        nativeOnEnter();
                        return true;
                    }
                    @Override public boolean sendKeyEvent(android.view.KeyEvent event) {
                        if (event.getAction() == android.view.KeyEvent.ACTION_DOWN) {
                            if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_DEL) {
                                nativeOnBackspace();
                                return true;
                            }
                            if (event.getKeyCode() == android.view.KeyEvent.KEYCODE_ENTER) {
                                nativeOnEnter();
                                return true;
                            }
                        }
                        return super.sendKeyEvent(event);
                    }
                };
            }
        };
        keyboardTarget.setFocusable(true);
        keyboardTarget.setFocusableInTouchMode(true);
        // A 1x1 input connection provides IME composition without covering
        // the NativeActivity surface with an Android text box.
        addContentView(keyboardTarget, new ViewGroup.LayoutParams(1, 1));
        importViewIntent(getIntent());
    }

    @Override protected void onResume() {
        super.onResume();
        applySystemBars();
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        importViewIntent(intent);
    }

    public void setSoftKeyboardVisible(boolean visible) {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (visible) {
                keyboardTarget.requestFocus();
                keyboardTarget.post(() -> imm.showSoftInput(keyboardTarget, InputMethodManager.SHOW_IMPLICIT));
            } else {
                imm.hideSoftInputFromWindow(keyboardTarget.getWindowToken(), 0);
                keyboardTarget.clearFocus();
            }
        });
    }

    public void requestOpenPdf() {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/pdf");
            try {
                startActivityForResult(intent, OPEN_PDF);
            } catch (android.content.ActivityNotFoundException ex) {
                nativeOnPdfResult("");
            }
        });
    }

    public void requestSavePdf(String stagedPath, String suggestedName) {
        runOnUiThread(() -> {
            if (stagedExport != null) {
                nativeOnSaveResult(-1, "Another export is already open");
                return;
            }
            try {
                File staging = new File(stagedPath).getCanonicalFile();
                File privateFiles = getFilesDir().getCanonicalFile();
                if (!staging.getPath().startsWith(privateFiles.getPath() + File.separator) ||
                        !staging.getName().endsWith(".folio-export.pdf") ||
                        !staging.isFile() || staging.length() == 0)
                    throw new IOException("private PDF staging file is missing");
                stagedExport = staging.getPath();
                Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/pdf");
                intent.putExtra(Intent.EXTRA_TITLE, suggestedName);
                startActivityForResult(intent, SAVE_PDF);
            } catch (Exception ex) {
                stagedExport = null;
                android.util.Log.e("folio", "Could not start PDF export", ex);
                nativeOnSaveResult(-1, "Could not open Android document picker");
            }
        });
    }

    /** Export to a private share-cache file and grant ONLY the chosen target
     *  read access to its content URI. Never expose a file:// URI or source. */
    public void requestSharePdf(String stagedPath, String suggestedName) {
        new Thread(() -> {
            File staging = null;
            try {
                staging = new File(stagedPath).getCanonicalFile();
                File files = getFilesDir().getCanonicalFile();
                if (!staging.getPath().startsWith(files.getPath() + File.separator) ||
                        !staging.getName().endsWith(".folio-export.pdf") ||
                        !staging.isFile() || staging.length() == 0)
                    throw new IOException("private PDF staging file is missing");
                File dir = new File(getCacheDir(), "shares");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("share cache unavailable");
                File[] old = dir.listFiles();
                if (old != null) for (File f : old) {
                    if (f.isFile() && f.lastModified() < System.currentTimeMillis() - 86400000L)
                        f.delete();
                }
                String id = UUID.randomUUID().toString() + ".pdf";
                File published = new File(dir, id);
                Files.copy(staging.toPath(), published.toPath());
                Uri uri = new Uri.Builder().scheme("content")
                        .authority(getPackageName() + ".share")
                        .appendPath("pdf").appendPath(id).build();
                runOnUiThread(() -> {
                    try {
                        Intent send = new Intent(Intent.ACTION_SEND);
                        send.setType("application/pdf");
                        send.putExtra(Intent.EXTRA_STREAM, uri);
                        send.setClipData(ClipData.newUri(getContentResolver(),
                                "Folio-annotated.pdf", uri));
                        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        startActivity(Intent.createChooser(send, "Share annotated PDF"));
                        nativeOnSaveResult(1, "");
                    } catch (Exception ex) {
                        published.delete();
                        nativeOnSaveResult(-1, "No app can share this PDF");
                        android.widget.Toast.makeText(this, "No app can share this PDF",
                                android.widget.Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Exception ex) {
                android.util.Log.e("folio", "Could not prepare share", ex);
                runOnUiThread(() -> {
                    nativeOnSaveResult(-1, "Could not prepare PDF to share");
                    android.widget.Toast.makeText(this,
                            "Could not prepare PDF to share", android.widget.Toast.LENGTH_LONG).show();
                });
            } finally {
                if (staging != null) staging.delete();
            }
        }, "folio-pdf-share").start();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SAVE_PDF) {
            String staged = stagedExport;
            stagedExport = null;
            if (staged == null) return;
            if (result != Activity.RESULT_OK || data == null || data.getData() == null) {
                new File(staged).delete();
                nativeOnSaveResult(0, "");
            } else {
                copyExportToProvider(staged, data.getData());
            }
            return;
        }
        if (request == RELINK_PDF) {
            String path = pendingRelink;
            pendingRelink = null;
            if (path != null && result == Activity.RESULT_OK && data != null &&
                    data.getData() != null) relinkInBackground(path, data.getData(), data.getFlags());
            return;
        }
        if (request != OPEN_PDF) return;
        if (result != Activity.RESULT_OK || data == null || data.getData() == null) {
            nativeOnPdfResult("");
            return;
        }
        importUri(data.getData(), data.getFlags());
    }

    private void copyExportToProvider(String staged, Uri uri) {
        // Provider I/O may be remote or slow. Never copy on Android's UI thread.
        new Thread(() -> {
            int status = -1;
            String message = "Could not write PDF to the selected location";
            try (InputStream in = new java.io.FileInputStream(staged);
                 ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "rwt")) {
                if (fd == null) throw new IOException("document provider did not open output");
                // Do not close the stream separately: it owns fd; sync before
                // close so a successful callback never means buffered bytes.
                try (FileOutputStream out = new FileOutputStream(fd.getFileDescriptor())) {
                    byte[] buf = new byte[65536];
                    long copied = 0;
                    int n;
                    while ((n = in.read(buf)) != -1) {
                        out.write(buf, 0, n);
                        copied += n;
                    }
                    if (copied == 0) throw new IOException("staged PDF was empty");
                    out.getFD().sync();
                }
                status = 1;
                message = "";
            } catch (Exception ex) {
                android.util.Log.e("folio", "PDF provider export failed", ex);
                // A failed new document must not masquerade as a successful
                // but corrupt PDF. Deletion is best effort; keep the error.
                try { getContentResolver().delete(uri, null, null); }
                catch (Exception cleanup) {
                    android.util.Log.e("folio", "Could not remove partial export", cleanup);
                    message = "Export failed; check destination for a partial PDF";
                }
            } finally {
                new File(staged).delete();
            }
            nativeOnSaveResult(status, message);
        }, "folio-pdf-export").start();
    }

    private void importViewIntent(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) &&
                intent.getData() != null) importUri(intent.getData(), intent.getFlags());
    }

    private String providerName(Uri uri) {
        String name = null;
        try (Cursor c = getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) name = c.getString(0);
        } catch (Exception ignored) { /* not every provider exposes a name */ }
        if (name == null) name = "Document.pdf";
        StringBuilder safe = new StringBuilder();
        for (int i = 0; i < name.length() && safe.length() < 80; i++) {
            char ch = name.charAt(i);
            safe.append(Character.isISOControl(ch) || "\\/:*?\"<>|".indexOf(ch) >= 0
                    ? '_' : ch);
        }
        String result = safe.toString().trim();
        while (result.startsWith(".")) result = result.substring(1);
        if (result.isEmpty()) result = "Document.pdf";
        if (!result.toLowerCase(java.util.Locale.ROOT).endsWith(".pdf")) result += ".pdf";
        return result;
    }

    private static String uriKey(Uri uri) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(
                uri.toString().getBytes(StandardCharsets.UTF_8));
        StringBuilder key = new StringBuilder();
        for (int i = 0; i < 16; i++)
            key.append(String.format(java.util.Locale.ROOT, "%02x", digest[i] & 0xff));
        return key.toString();
    }

    private boolean hasPersistedRead(Uri uri) {
        for (UriPermission grant : getContentResolver().getPersistedUriPermissions())
            if (grant.isReadPermission() && uri.equals(grant.getUri())) return true;
        return false;
    }

    private boolean persistRead(Uri uri, int flags) {
        if ((flags & Intent.FLAG_GRANT_READ_URI_PERMISSION) == 0 ||
            (flags & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) == 0) return false;
        try {
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException | IllegalArgumentException ex) {
            android.util.Log.i("folio", "Document has no persistent read grant", ex);
        }
        return hasPersistedRead(uri);
    }

    private File importedDirectory(String key) throws IOException {
        File root = new File(getFilesDir(), "imported").getCanonicalFile();
        if (!key.matches("[0-9a-f]{32}")) throw new IOException("invalid document identity");
        File dir = new File(root, key);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("import directory unavailable");
        return dir;
    }

    private File checkedPdf(String path) throws IOException {
        File root = new File(getFilesDir(), "imported").getCanonicalFile();
        File pdf = new File(path).getCanonicalFile();
        File parent = pdf.getParentFile();
        if (parent == null || !root.equals(parent.getParentFile()) ||
                !parent.getName().matches("[0-9a-f]{32}") ||
                !pdf.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".pdf"))
            throw new IOException("not a Folio library PDF");
        return pdf;
    }

    private File marker(File pdf) { return new File(pdf.getPath() + ".folio-link"); }

    private static String digestHex(byte[] digest) {
        StringBuilder out = new StringBuilder();
        for (byte b : digest) out.append(String.format(java.util.Locale.ROOT, "%02x", b & 0xff));
        return out.toString();
    }

    private static String sha256(InputStream in) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) != -1) md.update(buf, 0, n);
        return digestHex(md.digest());
    }

    private static String readSmall(File file) throws IOException {
        if (!file.isFile() || file.length() > 8192) throw new IOException("source metadata unavailable");
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
    }

    private static void atomicText(File file, String value) throws IOException {
        File tmp = new File(file.getPath() + ".part");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(value.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        try {
            Files.move(tmp.toPath(), file.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (java.nio.file.AtomicMoveNotSupportedException e) {
            Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private String expectedDigest(File pdf) throws Exception {
        File digest = new File(pdf.getParentFile(), "source.sha256");
        if (!digest.isFile()) {
            if (!pdf.isFile()) throw new IOException("source identity unavailable; keep your annotations");
            try (InputStream in = new java.io.FileInputStream(pdf))
                { atomicText(digest, sha256(in)); }
        }
        String value = readSmall(digest);
        if (!value.matches("[0-9a-f]{64}")) throw new IOException("source identity invalid");
        return value;
    }

    private Uri savedSource(File pdf) throws IOException {
        Uri uri = Uri.parse(readSmall(new File(pdf.getParentFile(), "source.uri")));
        if (!"content".equals(uri.getScheme()) || !hasPersistedRead(uri))
            throw new IOException("Source grant expired; relink with the PDF picker");
        return uri;
    }

    // Return an owned descriptor to C++'s FileMappedAccess. Never persist the
    // /proc/self/fd number: it is different on every open and process start.
    public int openLinkedPdfFd(String path) {
        try {
            File pdf = checkedPdf(path);
            if (!marker(pdf).isFile()) return -1;
            Uri uri = savedSource(pdf);
            try (ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r")) {
                if (fd == null) return -1;
                android.system.StructStat st = Os.fstat(fd.getFileDescriptor());
                if (!OsConstants.S_ISREG(st.st_mode) || st.st_size <= 0 ||
                    st.st_size > 0xffffffffL ||
                    Os.lseek(fd.getFileDescriptor(), 0, OsConstants.SEEK_CUR) < 0) return -1;
                return fd.detachFd();
            }
        } catch (Exception ex) {
            android.util.Log.w("folio", "Linked source inaccessible", ex);
            return -1;
        }
    }

    private void validateSource(Uri uri, String expected) throws Exception {
        if (!hasPersistedRead(uri)) throw new IOException("Persistent source access unavailable; keep copy instead");
        try (ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r")) {
            if (fd == null) throw new IOException("Source cannot be opened");
            android.system.StructStat st = Os.fstat(fd.getFileDescriptor());
            if (!OsConstants.S_ISREG(st.st_mode) || st.st_size <= 0 ||
                    st.st_size > 0xffffffffL)
                throw new IOException("Provider does not offer a seekable regular PDF; keep copy");
            Os.lseek(fd.getFileDescriptor(), 0, OsConstants.SEEK_SET);
            int owned = ParcelFileDescriptor.dup(fd.getFileDescriptor()).detachFd();
            if (!nativeValidatePdfFd(owned)) throw new IOException("Source PDF failed PDFium reopen; keep copy");
        }
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null || !expected.equals(sha256(in)))
                throw new IOException("PDF differs from the original; annotations were NOT moved");
        }
    }

    private void copyFromSource(File pdf, Uri uri, String expected) throws Exception {
        File incoming = new File(pdf.getParentFile(), "incoming.part");
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = getContentResolver().openInputStream(uri);
                 FileOutputStream dest = new FileOutputStream(incoming)) {
                if (in == null) throw new IOException("source unavailable");
                byte[] buf = new byte[65536]; int n;
                while ((n = in.read(buf)) != -1) { dest.write(buf, 0, n); digest.update(buf, 0, n); }
                dest.getFD().sync();
            }
            if (incoming.length() == 0 || !expected.equals(digestHex(digest.digest())))
                throw new IOException("Source changed; original copy and annotations preserved");
            try (ParcelFileDescriptor fd = ParcelFileDescriptor.open(incoming,
                    ParcelFileDescriptor.MODE_READ_ONLY)) {
                if (!nativeValidatePdfFd(ParcelFileDescriptor.dup(fd.getFileDescriptor()).detachFd()))
                    throw new IOException("Downloaded PDF failed to reopen");
            }
            try {
                Files.move(incoming.toPath(), pdf.toPath(),
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (java.nio.file.AtomicMoveNotSupportedException ex) {
                Files.move(incoming.toPath(), pdf.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally { incoming.delete(); }
    }

    public boolean prefersSourceOnly() {
        return getPreferences(MODE_PRIVATE).getBoolean("prefer_source_only", false);
    }
    public void setPrefersSourceOnly(boolean value) {
        getPreferences(MODE_PRIVATE).edit().putBoolean("prefer_source_only", value).apply();
    }

    public void requestStorageAction(String path, int action) {
        if (action == 3) {
            runOnUiThread(() -> {
                try {
                    File pdf = checkedPdf(path);
                    if (!marker(pdf).isFile()) throw new IOException("Only unavailable linked PDFs need relinking");
                    pendingRelink = pdf.getPath();
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    intent.setType("application/pdf");
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                    startActivityForResult(intent, RELINK_PDF);
                } catch (Exception ex) { storageResult(-1, ex.getMessage()); }
            });
            return;
        }
        if (action == 2) {
            runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("Use source only?")
                .setMessage("Folio will remove its offline PDF after verifying persistent access. " +
                            "If the source moves or its permission expires, this item cannot open " +
                            "until you relink it. Your Folio annotations will remain.")
                .setNegativeButton("Keep copy", (d, which) -> {})
                .setPositiveButton("Use source only", (d, which) -> storageInBackground(path, action))
                .show());
        } else storageInBackground(path, action);
    }

    private void storageResult(int status, String message) {
        nativeOnStorageResult(status, message == null ? "Storage action failed" : message);
    }

    private void storageInBackground(String path, int action) {
        new Thread(() -> {
            try {
                File pdf = checkedPdf(path);
                String expected = expectedDigest(pdf);
                Uri uri = savedSource(pdf);
                if (action == 2) {
                    validateSource(uri, expected);
                    if (!pdf.isFile()) throw new IOException("Offline PDF missing; relink source instead");
                    atomicText(marker(pdf), "source-only\n");
                    if (!pdf.delete()) {
                        marker(pdf).delete();
                        throw new IOException("Could not remove offline copy; it remains safe");
                    }
                    storageResult(1, "Source only · annotations kept locally");
                } else if (action == 1) {
                    copyFromSource(pdf, uri, expected);
                    if (!marker(pdf).delete() && marker(pdf).exists())
                        throw new IOException("Offline copy saved, but storage state could not be updated");
                    storageResult(1, "Offline copy ready");
                } else throw new IOException("Unknown storage action");
            } catch (Exception ex) {
                android.util.Log.e("folio", "Storage action failed", ex);
                storageResult(-1, ex.getMessage());
            }
        }, "folio-pdf-storage").start();
    }

    private void relinkInBackground(String path, Uri uri, int flags) {
        new Thread(() -> {
            try {
                File pdf = checkedPdf(path);
                if (!persistRead(uri, flags)) throw new IOException("Selected provider has no persistent grant");
                validateSource(uri, expectedDigest(pdf));
                atomicText(new File(pdf.getParentFile(), "source.uri"), uri.toString());
                storageResult(1, "PDF relinked; annotations preserved");
            } catch (Exception ex) {
                android.util.Log.e("folio", "Relink failed", ex);
                storageResult(-1, ex.getMessage());
            }
        }, "folio-pdf-relink").start();
    }

    private void importUri(Uri uri, int flags) {
        boolean granted = persistRead(uri, flags);
        new Thread(() -> {
            String path = "";
            File incoming = null;
            try {
                File dir = importedDirectory(uriKey(uri));
                File output = new File(dir, providerName(uri));
                // Preserve the original path on repeat imports: its exact
                // spelling is the existing sidecar/recents identity.
                File[] existing = dir.listFiles((d, name) -> name.endsWith(".pdf"));
                if (existing != null && existing.length > 0) output = existing[0];
                if (marker(output).isFile()) {
                    if (!granted) throw new IOException("Relink required; source grant not persistent");
                    validateSource(uri, expectedDigest(output));
                    atomicText(new File(dir, "source.uri"), uri.toString());
                    path = output.getAbsolutePath();
                } else {
                    incoming = new File(dir, "incoming.part");
                    MessageDigest digest = MessageDigest.getInstance("SHA-256");
                    try (InputStream in = getContentResolver().openInputStream(uri);
                         FileOutputStream dest = new FileOutputStream(incoming)) {
                        if (in == null) throw new IOException("document unavailable");
                        byte[] buf = new byte[65536]; int n;
                        while ((n = in.read(buf)) != -1) {
                            dest.write(buf, 0, n); digest.update(buf, 0, n);
                        }
                        dest.getFD().sync();
                    }
                    if (incoming.length() == 0) throw new IOException("empty PDF");
                    String incomingDigest = digestHex(digest.digest());
                    // Legacy path-only recents have no digest row yet. Hash
                    // their existing private copy BEFORE replacing anything;
                    // never attach its sidecar to changed provider bytes.
                    if (output.isFile() && !expectedDigest(output).equals(incomingDigest))
                        throw new IOException("Original source changed; existing annotations and copy kept");
                    try {
                        Files.move(incoming.toPath(), output.toPath(),
                            StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                    } catch (java.nio.file.AtomicMoveNotSupportedException ex) {
                        Files.move(incoming.toPath(), output.toPath(),
                            StandardCopyOption.REPLACE_EXISTING);
                    }
                    atomicText(new File(dir, "source.sha256"), incomingDigest);
                    if (granted) atomicText(new File(dir, "source.uri"), uri.toString());
                    path = output.getAbsolutePath();
                    if (granted && prefersSourceOnly()) {
                        try {
                            validateSource(uri, expectedDigest(output));
                            atomicText(marker(output), "source-only\n");
                            if (!output.delete()) marker(output).delete();
                        } catch (Exception ex) {
                            marker(output).delete();
                            android.util.Log.i("folio", "Source-only unavailable; keeping private copy", ex);
                        }
                    }
                }
            } catch (Exception ex) {
                android.util.Log.e("folio", "PDF import failed", ex);
                storageResult(-1, ex.getMessage());
            } finally { if (incoming != null) incoming.delete(); }
            nativeOnPdfResult(path);
        }, "folio-pdf-import").start();
    }

    public void setClipboardText(String text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("Folio selection", text));
    }

    public String getClipboardText() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (!cm.hasPrimaryClip() || cm.getPrimaryClip() == null) return "";
        CharSequence text = cm.getPrimaryClip().getItemAt(0).coerceToText(this);
        return text == null ? "" : text.toString();
    }
}
