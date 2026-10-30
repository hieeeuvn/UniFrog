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
     * @return 0 when the pass completed, -1 when it stopped early
     */
    public static native int run(String user, String key, boolean pull,
        boolean push, boolean list, boolean dryRun, Bridge bridge);
}
