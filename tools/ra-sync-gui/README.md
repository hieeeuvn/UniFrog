# UniFrog RA Sync (Windows + Android)

Small offline-first clients that move RetroAchievements data between the
UniFrog SD card and retroachievements.org, without the card being read by the
console at the same time.

They do the same three things as `tools/ra_sync.mjs`:

| Action | Card folder | Talks to RA |
|---|---|---|
| Download definitions | `achievements/queue/<hash>.req` → `achievements/cache/<hash>.json` | `r=patch` |
| Upload unlocks | `achievements/queue/unlocks.jsonl` | `r=awardachievement` |
| List card | counts cache, requests and queued unlocks | nothing |

The firmware writes those files while playing; the apps only read them and
write the results back. Nothing else on `unifrog_data` is touched.

## What you get

- `dist/UniFrog-RA-Sync-32.exe` — Windows 32-bit GUI
- `dist/UniFrog-RA-Sync-64.exe` — Windows 64-bit GUI
- `dist/UniFrog-RA-Sync.apk` — Android 5.0+ app

Both builds are self-contained: the executables need no runtime DLLs beyond
what Windows 10/11 ships (WinHTTP, UCRT), and the APK bundles its own native
code for `arm64-v8a`, `armeabi-v7a` and `x86_64`.

Get your Web API key from retroachievements.org → Settings → Keys. Use the
**Web API Key**, not the account password.

## Windows

1. Unplug the card from the console and mount it on the PC (USB mass storage
   or a card reader). Close any explorer window that has the card open.
2. Run `UniFrog-RA-Sync-64.exe` (or the 32-bit build on older systems).
3. `Browse...` and pick the folder that holds `unifrog_data`
   (`E:\unifrog_data` on most cards). Picking `unifrog_data\achievements`
   also works.
4. Enter username and Web API key. They are remembered in
   `%APPDATA%\UniFrogRASync\settings.ini`.
5. `Sync` does both directions; `Pull definitions`, `Push unlocks` and
   `List card` run a single direction.

The log pane shows one line per game/award, including the server message when
an award is rejected.

## Android

1. Copy the APK to the phone and open it (allow "install unknown apps" for
   your file manager). It is signed with the debug key from `build/android`,
   so Android will warn that it is not from the Play Store.
2. Either use a USB-C card reader, or write the card through a PC first — a
   card mounted over MTP is not visible to the folder picker.
3. Press `Choose unifrog_data folder` and pick `unifrog_data` on the card
   (or its `achievements` folder). The app keeps read/write access to that
   folder across restarts.
4. Enter the same credentials, tick Download/Upload, press `Sync now`.
5. `Card contents` only reads the card; `Clear log` empties the pane.

Credentials are stored in the app's private preferences file. Uninstalling the
app removes them.

## Layout

```
core/       protocol, JSON, MD5 and the sync pass (shared C, host-tested)
tests/      C test for the core + JVM test that drives the JNI bridge
windows/    Win32 GUI, WinHTTP transport, Win32 filesystem
android/    single-activity Java app, SAF filesystem, JNI glue
```

`core/ra_sync_core.c` is the single implementation of the protocol: the
Windows client calls it directly, the Android app calls the same code through
`android/jni/ra_sync_jni.c`. `tools/ra_sync.mjs` remains the reference
implementation for the file formats.

## Building

Nothing here is wired into the firmware Makefile; these are host tools.

```sh
# core + JNI bridge, run on the build machine (the closest check to the APK)
sh tests/jvm/run_jvm_test.sh

# Windows: needs Zig for the mingw-w64 cross build
sh windows/build_windows.sh

# Android: needs JDK 17, SDK platform 34, build-tools 34.0.0 and NDK 26.1
sh android/build_android.sh
```

`build_windows.sh` looks for `.deps/zig-linux-x86_64-0.13.0/zig`; override with
`ZIG=...`. `build_android.sh` looks for `.deps/jdk-17.0.21+9` and
`.deps/android-sdk`; override with `JAVA_HOME`, `ANDROID_SDK_ROOT` and
`ANDROID_NDK_ROOT`.

The Android build creates `build/keystore/debug.keystore` on first use. Keep
that file if you want future APKs to install over the old one; a different key
means "App not installed" until the old app is uninstalled. Use your own
release key for anything published.

Outputs land in `dist/`, which is not committed.

## Notes and limits

- Downloading definitions only runs for hashes the firmware queued in
  `achievements/queue/*.req`; the console produces those when it sees a game
  it does not have cached.
- Uploads are idempotent: the server answers "already unlocked" for repeats,
  and the apps treat that as success. A failed award stays in
  `unlocks.jsonl` for the next run.
- `o=` (seconds since unlock) comes from the card's queue line, so a wildly
  wrong console clock can make the server reject an otherwise valid award.
- Nothing is deleted from `unifrog_data` except `queue/*.req` files that were
  successfully cached and queue lines that were accepted.
