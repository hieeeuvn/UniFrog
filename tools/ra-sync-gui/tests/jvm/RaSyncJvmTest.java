import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;
import com.unifrog.rasync.NativeSync;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URL;
import java.net.URLDecoder;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Host-side test for the Android JNI glue.
 *
 * <p>Builds the same C core used by the APK as a Linux shared library, loads it
 * through {@link NativeSync} exactly like the app does and drives it against a
 * local HTTP server and a temporary card folder. This covers the JNI
 * conversions, the bridge contract and the sync protocol; only the SAF part of
 * the Android build cannot run off-device.</p>
 */
public final class RaSyncJvmTest {
    private static final Charset UTF_8 = Charset.forName("UTF-8");
    private static final String HASH = digest("unifrog-test-game");

    private static int failures;
    private static volatile String mode = "ok";
    private static volatile Map<String, String> lastAward = new HashMap<String, String>();

    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/dorequest.php", new Handler());
        server.start();
        String endpoint = "http://127.0.0.1:" + server.getAddress().getPort()
            + "/dorequest.php";
        Path base = Files.createTempDirectory("unifrog-rasync-");
        System.out.println("card root: " + base);

        testList(base.resolve("list"), endpoint);
        testPull(base.resolve("pull"), endpoint);
        testPushSuccess(base.resolve("push"), endpoint);
        testPushKeepsFailures(base.resolve("push-fail"), endpoint);
        testLoginFailure(base.resolve("login-fail"), endpoint);
        testMissingCredentials(base.resolve("no-cred"), endpoint);

        server.stop(0);
        if (failures > 0) {
            System.out.println(failures + " check(s) failed");
            System.exit(1);
        }
        System.out.println("all JNI checks passed");
    }

    private static void testList(Path card, String endpoint) throws Exception {
        System.out.println("-- list");
        Files.createDirectories(card.resolve("queue"));
        Files.createDirectories(card.resolve("cache"));
        write(card.resolve("cache/abc.json"), "{\"Success\":true}");
        write(card.resolve("queue/" + HASH + ".req"), "{}");
        write(card.resolve("queue/unlocks.jsonl"),
            unlock(1, 5, 0, 60) + unlock(2, 10, 1, 30));
        LocalBridge bridge = new LocalBridge(card, endpoint);
        mode = "ok";
        int rc = NativeSync.run("", "", false, false, true, false, bridge);
        check(rc == 0, "list returns 0");
        check(bridge.logContains("Cached games: 1"), "counts one cached game");
        check(bridge.logContains("Pending requests: 1"), "counts one request");
        check(bridge.logContains("Queued unlocks: 2"), "counts two unlocks");
    }

    private static void testPull(Path card, String endpoint) throws Exception {
        System.out.println("-- pull");
        Files.createDirectories(card.resolve("queue"));
        Files.createDirectories(card.resolve("cache"));
        write(card.resolve("queue/" + HASH + ".req"), "{}");
        LocalBridge bridge = new LocalBridge(card, endpoint);
        mode = "ok";
        int rc = NativeSync.run("tester", "key", true, false, false, false, bridge);
        check(rc == 0, "pull returns 0");
        check(Files.exists(card.resolve("cache/" + HASH + ".json")),
            "definitions written to cache/");
        String cached = read(card.resolve("cache/" + HASH + ".json"));
        check(cached.contains("Test Game"), "cache keeps the raw patch response");
        check(!Files.exists(card.resolve("queue/" + HASH + ".req")),
            "request removed after caching");
        check(bridge.status != null && bridge.status.contains("cached=1"),
            "status reports one cached game: " + bridge.status);
    }

    private static void testPushSuccess(Path card, String endpoint) throws Exception {
        System.out.println("-- push");
        Files.createDirectories(card.resolve("queue"));
        write(card.resolve("queue/unlocks.jsonl"), unlock(11, 5, 0, 45));
        LocalBridge bridge = new LocalBridge(card, endpoint);
        mode = "ok";
        int rc = NativeSync.run("tester", "key", false, true, false, false, bridge);
        check(rc == 0, "push returns 0");
        check(!Files.exists(card.resolve("queue/unlocks.jsonl")),
            "queue file removed once every award was sent");
        check(bridge.status != null && bridge.status.contains("awarded=1"),
            "status reports one award: " + bridge.status);
        check(bridge.logContains("achievement 11: awarded"), "award logged");
        Map<String, String> award = new HashMap<String, String>(lastAward);
        check("awardachievement".equals(award.get("r")), "award request sent");
        check(award.get("v") != null && award.get("v").matches("[0-9a-f]{32}"),
            "award carries a 32 hex signature");
        check(award.get("m") != null && award.get("m").equals(HASH),
            "award carries the game hash");
        check(award.get("o") != null && Integer.parseInt(award.get("o")) >= 40,
            "award carries the seconds since unlock");
    }

    private static void testPushKeepsFailures(Path card, String endpoint)
        throws Exception {
        System.out.println("-- push keeps failed awards");
        Files.createDirectories(card.resolve("queue"));
        String line = unlock(12, 5, 1, 120);
        write(card.resolve("queue/unlocks.jsonl"), line);
        LocalBridge bridge = new LocalBridge(card, endpoint);
        mode = "award-fail";
        int rc = NativeSync.run("tester", "key", false, true, false, false, bridge);
        check(rc == 0, "failed award does not abort the pass");
        check(bridge.status != null && bridge.status.contains("failed=1"),
            "status reports the failure: " + bridge.status);
        check(Files.exists(card.resolve("queue/unlocks.jsonl")),
            "queue kept for the next sync");
        check(read(card.resolve("queue/unlocks.jsonl")).contains("\"a\":12"),
            "kept line is the failed award");
    }

    private static void testLoginFailure(Path card, String endpoint) throws Exception {
        System.out.println("-- login failure");
        Files.createDirectories(card.resolve("queue"));
        write(card.resolve("queue/unlocks.jsonl"), unlock(13, 5, 0, 10));
        LocalBridge bridge = new LocalBridge(card, endpoint);
        mode = "login-fail";
        int rc = NativeSync.run("tester", "bad-key", false, true, false, false,
            bridge);
        check(rc != 0, "bad credentials stop the pass");
        check(bridge.status != null && bridge.status.contains("Login failed"),
            "status reports the login failure: " + bridge.status);
        check(Files.exists(card.resolve("queue/unlocks.jsonl")),
            "queue untouched when login fails");
        mode = "ok";
    }

    private static void testMissingCredentials(Path card, String endpoint)
        throws Exception {
        System.out.println("-- missing credentials");
        Files.createDirectories(card.resolve("queue"));
        LocalBridge bridge = new LocalBridge(card, endpoint);
        int rc = NativeSync.run("", "", true, true, false, false, bridge);
        check(rc != 0, "empty credentials stop the pass");
        check(bridge.status != null && bridge.status.contains("Missing credentials"),
            "status asks for credentials: " + bridge.status);
    }

    /** Local-directory bridge with the same callbacks the app implements. */
    private static final class LocalBridge implements NativeSync.Bridge {
        private final File root;
        private final String endpoint;
        private final List<String> lines = new ArrayList<String>();
        private String lastError;
        volatile String status;

        LocalBridge(Path root, String endpoint) {
            this.root = root.toFile();
            this.endpoint = endpoint;
        }

        private File file(String path) {
            return new File(root, path);
        }

        boolean logContains(String text) {
            synchronized (lines) {
                for (String line : lines) {
                    if (line.contains(text))
                        return true;
                }
            }
            return false;
        }

        @Override
        public void log(String line) {
            synchronized (lines) {
                lines.add(line);
            }
            System.out.println("     core: " + line);
        }

        @Override
        public void status(String summary) {
            status = summary;
        }

        @Override
        public String lastError() {
            return lastError;
        }

        @Override
        public boolean exists(String path) {
            return file(path).exists();
        }

        @Override
        public String[] list(String dir) {
            String[] names = file(dir).list();
            return names;
        }

        @Override
        public byte[] read(String path) {
            try {
                return Files.readAllBytes(file(path).toPath());
            } catch (IOException error) {
                return null;
            }
        }

        @Override
        public boolean write(String path, byte[] data) {
            File target = file(path);
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                return false;
            try {
                Files.write(target.toPath(), data);
                return true;
            } catch (IOException error) {
                return false;
            }
        }

        @Override
        public boolean delete(String path) {
            File target = file(path);
            return !target.exists() || target.delete();
        }

        @Override
        public boolean rename(String from, String to) {
            File source = file(from);
            File target = file(to);
            if (target.exists() && !target.delete())
                return false;
            return source.renameTo(target);
        }

        @Override
        public byte[] post(String body) {
            HttpURLConnection connection = null;
            try {
                byte[] payload = body.getBytes(UTF_8);
                connection = (HttpURLConnection) new URL(endpoint).openConnection();
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                connection.setConnectTimeout(5000);
                connection.setReadTimeout(10000);
                connection.setRequestProperty("Content-Type",
                    "application/x-www-form-urlencoded");
                OutputStream out = connection.getOutputStream();
                try {
                    out.write(payload);
                } finally {
                    out.close();
                }
                int status = connection.getResponseCode();
                InputStream in = status >= 400 ? connection.getErrorStream()
                    : connection.getInputStream();
                ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                if (in != null) {
                    try {
                        byte[] chunk = new byte[4096];
                        int count;
                        while ((count = in.read(chunk)) > 0)
                            buffer.write(chunk, 0, count);
                    } finally {
                        in.close();
                    }
                }
                lastError = null;
                return buffer.toByteArray();
            } catch (IOException error) {
                lastError = error.toString();
                return null;
            } finally {
                if (connection != null)
                    connection.disconnect();
            }
        }
    }

    private static final class Handler implements HttpHandler {
        @Override
        public void handle(HttpExchange exchange) throws IOException {
            byte[] body = readAll(exchange.getRequestBody());
            Map<String, String> form = parse(new String(body, UTF_8));
            String request = form.get("r");
            String response;
            if ("login2".equals(request)) {
                response = "login-fail".equals(mode)
                    ? "{\"Success\":false,\"Error\":\"Invalid API Key. Please check it\"}"
                    : "{\"Success\":true,\"User\":\"tester\",\"Token\":\"abc\"}";
            } else if ("patch".equals(request)) {
                response = "{\"Success\":true,\"PatchData\":{\"ID\":4242,"
                    + "\"Title\":\"Test Game\",\"Achievements\":[]}}";
            } else if ("awardachievement".equals(request)) {
                lastAward = form;
                response = "award-fail".equals(mode)
                    ? "{\"Success\":false,\"Error\":\"Invalid API Key\"}"
                    : "{\"Success\":true,\"AchievementID\":" + form.get("a")
                        + ",\"Score\":5}";
            } else {
                response = "{\"Success\":false,\"Error\":\"unknown request\"}";
            }
            byte[] out = response.getBytes(UTF_8);
            exchange.getResponseHeaders().set("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, out.length);
            OutputStream stream = exchange.getResponseBody();
            try {
                stream.write(out);
            } finally {
                stream.close();
            }
        }
    }

    private static Map<String, String> parse(String body) {
        Map<String, String> form = new LinkedHashMap<String, String>();
        for (String pair : body.split("&")) {
            if (pair.length() == 0)
                continue;
            int equals = pair.indexOf('=');
            String name = equals < 0 ? pair : pair.substring(0, equals);
            String value = equals < 0 ? "" : pair.substring(equals + 1);
            try {
                form.put(URLDecoder.decode(name, "UTF-8"),
                    URLDecoder.decode(value, "UTF-8"));
            } catch (IOException error) {
                // Malformed encoding: keep the raw pair so the server still sees it.
                form.put(name, value);
            }
        }
        return form;
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        byte[] chunk = new byte[4096];
        int count;
        while ((count = in.read(chunk)) > 0)
            buffer.write(chunk, 0, count);
        in.close();
        return buffer.toByteArray();
    }

    private static String unlock(int id, int points, int hardcore, int secondsAgo) {
        long now = System.currentTimeMillis() / 1000L - secondsAgo;
        return "{\"a\":" + id + ",\"p\":" + points + ",\"h\":\"" + HASH
            + "\",\"m\":" + hardcore + ",\"t\":" + now + "}\n";
    }

    private static String digest(String text) {
        try {
            MessageDigest digest = MessageDigest.getInstance("MD5");
            byte[] hash = digest.digest(text.getBytes(UTF_8));
            StringBuilder out = new StringBuilder();
            for (byte value : hash)
                out.append(String.format("%02x", value & 0xff));
            return out.toString();
        } catch (Exception error) {
            throw new IllegalStateException(error);
        }
    }

    private static void write(Path path, String text) throws IOException {
        Files.createDirectories(path.getParent());
        Files.write(path, text.getBytes(UTF_8));
    }

    private static String read(Path path) throws IOException {
        return new String(Files.readAllBytes(path), UTF_8);
    }

    private static void check(boolean condition, String what) {
        if (condition) {
            System.out.println("ok   " + what);
        } else {
            System.out.println("FAIL " + what);
            failures++;
        }
    }

    private RaSyncJvmTest() {
    }
}
