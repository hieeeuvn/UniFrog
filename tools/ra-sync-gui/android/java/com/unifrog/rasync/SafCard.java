package com.unifrog.rasync;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * SAF backend for the sync core.
 *
 * <p>The card is picked once with {@code ACTION_OPEN_DOCUMENT_TREE}; the
 * selected tree must be the folder that holds {@code unifrog_data} (the folder
 * that contains {@code achievements}) or {@code unifrog_data} itself. From
 * there every core path such as {@code queue/unlocks.jsonl} is mapped to a
 * document below the {@code achievements} folder.</p>
 */
final class SafCard implements NativeSync.Bridge {
    static final String SYNC_URL = "https://retroachievements.org/dorequest.php";

    private static final String TAG = "UniFrogRASync";
    private static final String MIME_DIR = "vnd.android.document/directory";
    private static final String MIME_FILE = "application/octet-stream";
    private static final Charset UTF_8 = Charset.forName("UTF-8");
    private static final String[] CHILD_PROJECTION = {
        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE,
    };

    interface Listener {
        void onLog(String line);

        void onStatus(String summary);
    }

    private final ContentResolver resolver;
    private final Uri tree;
    private final String rootId;
    private final HashMap<String, String> folders = new HashMap<String, String>();
    private Listener listener;
    private String lastError;

    private SafCard(ContentResolver resolver, Uri tree, String rootId) {
        this.resolver = resolver;
        this.tree = tree;
        this.rootId = rootId;
        folders.put("", rootId);
    }

    /**
     * Opens the card behind {@code tree}. Accepts either the folder that holds
     * {@code achievements} or the achievements folder itself.
     */
    static SafCard open(ContentResolver resolver, Uri tree, Listener listener)
        throws IOException {
        String treeId = DocumentsContract.getTreeDocumentId(tree);
        String root = treeId;
        String achievements = findChild(resolver, tree, treeId, "achievements");
        if (achievements != null && isDirectory(resolver, tree, achievements))
            root = achievements;
        SafCard card = new SafCard(resolver, tree, root);
        card.listener = listener;
        if (!card.hasFolder("queue") && !card.hasFolder("cache")) {
            throw new IOException(
                "That folder is not a UniFrog card: pick unifrog_data or its achievements folder");
        }
        if (card.ensureFolder("queue") == null
            || card.ensureFolder("cache") == null) {
            throw new IOException(
                "Could not open queue/ and cache/ on the card (is it read-only?)");
        }
        return card;
    }

    @Override
    public void log(String line) {
        Log.i(TAG, line);
        if (listener != null)
            listener.onLog(line);
    }

    @Override
    public void status(String summary) {
        if (listener != null)
            listener.onStatus(summary);
    }

    @Override
    public String lastError() {
        return lastError;
    }

    @Override
    public boolean exists(String path) {
        if (isRoot(path))
            return true;
        return fileId(path) != null;
    }

    @Override
    public String[] list(String dir) {
        String parent = folderId(dir);
        if (parent == null)
            return null;
        ArrayList<String> names = new ArrayList<String>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        Cursor cursor = null;
        try {
            cursor = resolver.query(children, CHILD_PROJECTION, null, null, null);
            if (cursor == null)
                return null;
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name != null)
                    names.add(name);
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "list " + dir + " failed", error);
            return null;
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return names.toArray(new String[names.size()]);
    }

    @Override
    public byte[] read(String path) {
        String id = fileId(path);
        if (id == null)
            return null;
        InputStream in = null;
        try {
            in = resolver.openInputStream(documentUri(id));
            if (in == null)
                return null;
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int count;
            while ((count = in.read(buffer)) > 0)
                out.write(buffer, 0, count);
            return out.toByteArray();
        } catch (IOException error) {
            Log.w(TAG, "read " + path + " failed", error);
            return null;
        } catch (RuntimeException error) {
            Log.w(TAG, "read " + path + " failed", error);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    @Override
    public boolean write(String path, byte[] data) {
        String folder = parentOf(path);
        String name = leaf(path);
        String folderId = ensureFolder(folder);
        if (folderId == null)
            return false;
        delete(path);
        Uri created = null;
        try {
            created = DocumentsContract.createDocument(resolver,
                documentUri(folderId), MIME_FILE, name);
            if (created == null) {
                lastError = "could not create " + path;
                return false;
            }
            OutputStream out = resolver.openOutputStream(created, "w");
            if (out == null) {
                lastError = "could not open " + path;
                return false;
            }
            try {
                out.write(data);
                out.flush();
            } finally {
                closeQuietly(out);
            }
            return true;
        } catch (IOException error) {
            lastError = String.valueOf(error.getMessage());
            Log.w(TAG, "write " + path + " failed", error);
            return false;
        } catch (RuntimeException error) {
            lastError = String.valueOf(error.getMessage());
            Log.w(TAG, "write " + path + " failed", error);
            return false;
        }
    }

    @Override
    public boolean delete(String path) {
        String id = fileId(path);
        if (id == null)
            return true;
        try {
            return DocumentsContract.deleteDocument(resolver, documentUri(id));
        } catch (RuntimeException error) {
            Log.w(TAG, "delete " + path + " failed", error);
            return false;
        } catch (IOException error) {
            Log.w(TAG, "delete " + path + " failed", error);
            return false;
        }
    }

    @Override
    public boolean rename(String from, String to) {
        String id = fileId(from);
        if (id == null)
            return false;
        if (!parentOf(from).equals(parentOf(to)))
            return false;
        delete(to);
        try {
            Uri renamed = DocumentsContract.renameDocument(resolver,
                documentUri(id), leaf(to));
            return renamed != null;
        } catch (RuntimeException error) {
            Log.w(TAG, "rename " + from + " failed", error);
            return false;
        } catch (IOException error) {
            Log.w(TAG, "rename " + from + " failed", error);
            return false;
        }
    }

    @Override
    public byte[] post(String body) {
        HttpURLConnection connection = null;
        try {
            byte[] payload = body.getBytes(UTF_8);
            connection = (HttpURLConnection) new URL(SYNC_URL).openConnection();
            connection.setRequestMethod("POST");
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(60000);
            connection.setDoOutput(true);
            connection.setFixedLengthStreamingMode(payload.length);
            connection.setRequestProperty("Content-Type",
                "application/x-www-form-urlencoded");
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("User-Agent", "UniFrog-RA-Sync/1.0");
            OutputStream out = connection.getOutputStream();
            try {
                out.write(payload);
                out.flush();
            } finally {
                closeQuietly(out);
            }
            int status = connection.getResponseCode();
            InputStream in = status >= 400 ? connection.getErrorStream()
                : connection.getInputStream();
            if (in == null)
                in = connection.getErrorStream();
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            if (in != null) {
                try {
                    byte[] chunk = new byte[8192];
                    int count;
                    while ((count = in.read(chunk)) > 0)
                        buffer.write(chunk, 0, count);
                } finally {
                    closeQuietly(in);
                }
            }
            byte[] data = buffer.toByteArray();
            lastError = null;
            if (status == 403) {
                lastError = "server returned HTTP 403 Forbidden "
                    + "(request blocked before reaching the API)";
                return null;
            }
            if (status == 429) {
                lastError = "server returned HTTP 429 (too many requests, "
                    + "wait a minute and try again)";
                return null;
            }
            if (data.length == 0 && status >= 400) {
                lastError = "server returned HTTP " + status;
                return null;
            }
            return data;
        } catch (IOException error) {
            lastError = error.getMessage() != null ? error.getMessage()
                : error.toString();
            Log.w(TAG, "POST failed", error);
            return null;
        } catch (RuntimeException error) {
            lastError = error.getMessage() != null ? error.getMessage()
                : error.toString();
            Log.w(TAG, "POST failed", error);
            return null;
        } finally {
            if (connection != null)
                connection.disconnect();
        }
    }

    private static void closeQuietly(java.io.Closeable stream) {
        if (stream == null)
            return;
        try {
            stream.close();
        } catch (IOException ignored) {
            // Nothing useful to do while unwinding.
        }
    }

    private Uri documentUri(String id) {
        return DocumentsContract.buildDocumentUriUsingTree(tree, id);
    }

    private static boolean isRoot(String path) {
        return path == null || path.length() == 0 || ".".equals(path);
    }

    private static String parentOf(String path) {
        int slash = path.lastIndexOf('/');
        return slash < 0 ? "" : path.substring(0, slash);
    }

    private static String leaf(String path) {
        int slash = path.lastIndexOf('/');
        return slash < 0 ? path : path.substring(slash + 1);
    }

    private boolean hasFolder(String name) {
        return folderId(name) != null;
    }

    private String folderId(String path) {
        if (isRoot(path))
            return rootId;
        String cached = folders.get(path);
        if (cached != null)
            return cached;
        String parent = folderId(parentOf(path));
        if (parent == null)
            return null;
        String child = findChild(resolver, tree, parent, leaf(path));
        if (child == null || !isDirectory(resolver, tree, child))
            return null;
        folders.put(path, child);
        return child;
    }

    private String ensureFolder(String path) {
        String existing = folderId(path);
        if (existing != null)
            return existing;
        if (isRoot(path))
            return rootId;
        String parent = ensureFolder(parentOf(path));
        if (parent == null)
            return null;
        try {
            Uri created = DocumentsContract.createDocument(resolver,
                documentUri(parent), MIME_DIR, leaf(path));
            if (created == null)
                return null;
            String id = DocumentsContract.getDocumentId(created);
            folders.put(path, id);
            return id;
        } catch (RuntimeException error) {
            Log.w(TAG, "create folder " + path + " failed", error);
            return null;
        } catch (IOException error) {
            Log.w(TAG, "create folder " + path + " failed", error);
            return null;
        }
    }

    /** Document id of an existing file, or null. */
    private String fileId(String path) {
        if (isRoot(path))
            return null;
        String parent = folderId(parentOf(path));
        if (parent == null)
            return null;
        return findChild(resolver, tree, parent, leaf(path));
    }

    private static String findChild(ContentResolver resolver, Uri tree,
        String parentId, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
            parentId);
        Cursor cursor = null;
        try {
            cursor = resolver.query(children, CHILD_PROJECTION, null, null, null);
            if (cursor == null)
                return null;
            while (cursor.moveToNext()) {
                String display = cursor.getString(1);
                if (display != null && display.equalsIgnoreCase(name))
                    return cursor.getString(0);
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "lookup " + name + " failed", error);
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return null;
    }

    private static boolean isDirectory(ContentResolver resolver, Uri tree,
        String id) {
        return MIME_DIR.equals(mimeType(resolver, tree, id));
    }

    private static String mimeType(ContentResolver resolver, Uri tree,
        String id) {
        Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, id);
        Cursor cursor = null;
        try {
            cursor = resolver.query(document,
                new String[] {DocumentsContract.Document.COLUMN_MIME_TYPE},
                null, null, null);
            if (cursor != null && cursor.moveToFirst())
                return cursor.getString(0);
        } catch (RuntimeException error) {
            Log.w(TAG, "mime lookup failed", error);
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return null;
    }
}
