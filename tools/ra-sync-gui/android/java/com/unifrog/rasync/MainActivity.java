package com.unifrog.rasync;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * One screen: pick the card folder, enter credentials, sync.
 *
 * <p>All protocol work happens in the native core on a worker thread; this
 * activity only owns the UI and the saved settings.</p>
 */
public final class MainActivity extends Activity implements SafCard.Listener {
    private static final int PICK_FOLDER = 1001;
    private static final String PREFS = "sync";

    private final ExecutorService worker = Executors.newSingleThreadExecutor();

    private SharedPreferences prefs;
    private Uri treeUri;
    private boolean running;

    private EditText userField;
    private EditText keyField;
    private TextView folderLabel;
    private TextView statusView;
    private TextView logView;
    private CheckBox pullBox;
    private CheckBox pushBox;
    private Button folderButton;
    private Button syncButton;
    private Button listButton;
    private ProgressBar progress;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        userField = (EditText) findViewById(R.id.user);
        keyField = (EditText) findViewById(R.id.key);
        folderLabel = (TextView) findViewById(R.id.folder);
        statusView = (TextView) findViewById(R.id.status);
        logView = (TextView) findViewById(R.id.log);
        pullBox = (CheckBox) findViewById(R.id.pull);
        pushBox = (CheckBox) findViewById(R.id.push);
        folderButton = (Button) findViewById(R.id.choose);
        syncButton = (Button) findViewById(R.id.sync);
        listButton = (Button) findViewById(R.id.list);
        progress = (ProgressBar) findViewById(R.id.progress);

        userField.setText(prefs.getString("user", ""));
        keyField.setText(prefs.getString("key", ""));
        String savedTree = prefs.getString("tree", "");
        if (savedTree.length() > 0) {
            Uri saved = Uri.parse(savedTree);
            if (hasPermission(saved))
                treeUri = saved;
        }
        updateFolderLabel();

        folderButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                pickFolder();
            }
        });
        syncButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startSync(false);
            }
        });
        listButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startSync(true);
            }
        });
        findViewById(R.id.clear).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                logView.setText("");
            }
        });
    }

    private boolean hasPermission(Uri uri) {
        for (android.content.UriPermission permission
            : getContentResolver().getPersistedUriPermissions()) {
            if (permission.getUri().equals(uri) && permission.isWritePermission())
                return true;
        }
        return false;
    }

    private void pickFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_FOLDER || resultCode != RESULT_OK || data == null)
            return;
        Uri uri = data.getData();
        if (uri == null)
            return;
        try {
            getContentResolver().takePersistableUriPermission(uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (SecurityException error) {
            Toast.makeText(this, "The file manager did not grant write access",
                Toast.LENGTH_LONG).show();
            return;
        }
        treeUri = uri;
        prefs.edit().putString("tree", uri.toString()).apply();
        updateFolderLabel();
        statusView.setText("Folder selected");
    }

    private void updateFolderLabel() {
        if (treeUri == null) {
            folderLabel.setText("No folder selected yet");
            return;
        }
        String shown = treeUri.getLastPathSegment();
        folderLabel.setText(shown != null ? shown : treeUri.toString());
    }

    private void startSync(boolean listOnly) {
        if (running)
            return;
        String user = userField.getText().toString().trim();
        String key = keyField.getText().toString().trim();
        if (treeUri == null) {
            Toast.makeText(this, "Choose the card's unifrog_data folder first",
                Toast.LENGTH_LONG).show();
            return;
        }
        if (!listOnly && (user.length() == 0 || key.length() == 0)) {
            Toast.makeText(this,
                "Enter your RetroAchievements username and Web API key",
                Toast.LENGTH_LONG).show();
            return;
        }
        boolean pull = pullBox.isChecked();
        boolean push = pushBox.isChecked();
        if (!listOnly && !pull && !push) {
            Toast.makeText(this, "Tick download, upload, or both",
                Toast.LENGTH_LONG).show();
            return;
        }
        prefs.edit().putString("user", user).putString("key", key).apply();

        final String userName = user;
        final String apiKey = key;
        final boolean doPull = pull;
        final boolean doPush = push;
        final boolean list = listOnly;
        final Uri tree = treeUri;

        appendLog("");
        appendLog(list ? "== Card contents ==" : "== Sync ==");
        setRunning(true);
        statusView.setText(list ? "Reading the card..." : "Syncing...");
        worker.execute(new Runnable() {
            @Override
            public void run() {
                runSync(userName, apiKey, doPull, doPush, list, tree);
            }
        });
    }

    private void runSync(String user, String key, boolean pull, boolean push,
        boolean list, Uri tree) {
        try {
            SafCard card = SafCard.open(getContentResolver(), tree, this);
            int result = NativeSync.run(user, key, pull, push, list, false, card);
            if (result != 0)
                statusView.post(new Runnable() {
                    @Override
                    public void run() {
                        statusView.setText("Stopped early - see the log");
                    }
                });
        } catch (Throwable error) {
            final String message = error.getClass().getSimpleName()
                + (error.getMessage() != null ? ": " + error.getMessage() : "");
            appendLog(message);
            statusView.post(new Runnable() {
                @Override
                public void run() {
                    statusView.setText(message);
                }
            });
        } finally {
            setRunning(false);
        }
    }

    private void setRunning(final boolean value) {
        running = value;
        statusView.post(new Runnable() {
            @Override
            public void run() {
                syncButton.setEnabled(!value);
                listButton.setEnabled(!value);
                folderButton.setEnabled(!value);
                progress.setVisibility(value ? View.VISIBLE : View.GONE);
            }
        });
    }

    @Override
    public void onLog(final String line) {
        appendLog(line);
    }

    @Override
    public void onStatus(final String summary) {
        statusView.post(new Runnable() {
            @Override
            public void run() {
                statusView.setText(summary);
            }
        });
    }

    private void appendLog(final String line) {
        logView.post(new Runnable() {
            @Override
            public void run() {
                logView.append(line);
                logView.append("\n");
            }
        });
    }

    @Override
    protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}
