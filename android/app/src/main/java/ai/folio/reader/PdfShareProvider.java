package ai.folio.reader;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Read-only, narrow content:// bridge for PDFs staged in Folio's share cache.
 *  No filesystem paths supplied by callers are ever interpreted as provider paths. */
public final class PdfShareProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }

    private File resolve(Uri uri) throws FileNotFoundException {
        if (getContext() == null || uri == null ||
                !getContext().getPackageName().concat(".share").equals(uri.getAuthority()) ||
                uri.getPathSegments().size() != 2 ||
                !"pdf".equals(uri.getPathSegments().get(0)))
            throw new FileNotFoundException("invalid share URI");
        String name = uri.getPathSegments().get(1);
        if (!name.matches("[a-f0-9\\-]{36}\\.pdf"))
            throw new FileNotFoundException("invalid share ID");
        try {
            File dir = new File(getContext().getCacheDir(), "shares").getCanonicalFile();
            File pdf = new File(dir, name).getCanonicalFile();
            if (!dir.equals(pdf.getParentFile()) || !pdf.isFile())
                throw new FileNotFoundException("share expired");
            return pdf;
        } catch (IOException ex) {
            throw new FileNotFoundException("share unavailable");
        }
    }

    @Override public String getType(Uri uri) { return "application/pdf"; }
    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(resolve(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }
    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) {
        try {
            File pdf = resolve(uri);
            if (projection == null) projection = new String[]{OpenableColumns.DISPLAY_NAME,
                                                                OpenableColumns.SIZE};
            MatrixCursor result = new MatrixCursor(projection);
            Object[] row = new Object[projection.length];
            for (int i = 0; i < projection.length; ++i) {
                if (OpenableColumns.DISPLAY_NAME.equals(projection[i])) row[i] = "Folio-annotated.pdf";
                if (OpenableColumns.SIZE.equals(projection[i])) row[i] = pdf.length();
            }
            result.addRow(row);
            return result;
        } catch (FileNotFoundException ex) { return null; }
    }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection,
                                String[] args) { throw new UnsupportedOperationException(); }
}
