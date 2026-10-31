package com.unifrog.rasync;

/**
 * Thin JNI bridge to the C sync engine that also powers the Windows build.
 *
 * <p>The native side only knows about relative paths inside the card's
 * {@code achievements} folder, so {@link SafCard} maps each callback to a
 * SAF document URI.</p>
 */
public final class NativeSync {
    static {
        System.loadLibrary("unifrog_rasync");
    }

    private NativeSync() {
    }

    /** File and network access, implemented on top of Android's SAF. */
    public interface Bridge {
        boolean exists(String path);

        /** Direct children of {@code dir}, or null when the folder is missing. */
        String[] list(String dir);

        byte[] read(String path);

        boolean write(String path, byte[] data);

        boolean delete(String path);

        boolean rename(String from, String to);

        /** POST a form-encoded body; null means the request failed. */
        byte[] post(String body);

        String lastError();

        void log(String line);

        void status(String summary);
    }

    /**
     * Runs one sync pass.
     *
     * @param key a 16-character Connect token, or empty
     * @param password the account password, used once to obtain a Connect token
     * @return the Connect token when the pass completed, null when it stopped
     *     early (the reason is already in the log and status callbacks)
     */
    public static native String run(String user, String key, String password,
        boolean pull, boolean push, boolean list, boolean dryRun, Bridge bridge);
}
