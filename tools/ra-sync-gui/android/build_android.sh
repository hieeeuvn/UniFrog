#!/bin/sh
# Builds the Android app without Gradle.
#
# Toolchain: JDK 17, Android SDK build-tools 34.0.0, platform android-34 and
# the NDK (clang) for the native sync core. Override the paths with
# JAVA_HOME, ANDROID_SDK_ROOT and ANDROID_NDK_ROOT when they live elsewhere.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app_root=$(cd "$here/.." && pwd)
repo=$(cd "$app_root/../.." && pwd)

JAVA_HOME=${JAVA_HOME:-$repo/.deps/jdk-17.0.21+9}
SDK=${ANDROID_SDK_ROOT:-$repo/.deps/android-sdk}
NDK=${ANDROID_NDK_ROOT:-$SDK/ndk/26.1.10909125}
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT="$app_root/build/android"
DIST="$app_root/dist"
API=21
PACKAGE_VERSION_NAME=1.0
PACKAGE_VERSION_CODE=1

die() {
    echo "error: $*" >&2
    exit 1
}

[ -x "$JAVA_HOME/bin/javac" ] || die "JDK not found at $JAVA_HOME"
[ -f "$PLATFORM" ] || die "android.jar not found at $PLATFORM (sdkmanager 'platforms;android-34')"
[ -x "$BT/aapt2" ] || die "build-tools 34.0.0 not found at $BT"
[ -d "$TOOLCHAIN" ] || die "Android NDK not found at $NDK (sdkmanager 'ndk;26.1.10909125')"

PATH="$JAVA_HOME/bin:$PATH"
export PATH

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/classes" "$OUT/gen" "$DIST"

echo "== native core"
for abi in arm64-v8a armeabi-v7a x86_64; do
    case $abi in
        arm64-v8a) cc="$TOOLCHAIN/aarch64-linux-android$API-clang" ;;
        armeabi-v7a) cc="$TOOLCHAIN/armv7a-linux-androideabi$API-clang" ;;
        x86_64) cc="$TOOLCHAIN/x86_64-linux-android$API-clang" ;;
    esac
    [ -x "$cc" ] || die "clang for $abi not found: $cc"
    mkdir -p "$OUT/lib/$abi"
    "$cc" -shared -fPIC -O2 -Wall -Wextra \
        -I"$app_root/core" -I"$here/jni" \
        -o "$OUT/lib/$abi/libunifrog_rasync.so" \
        "$app_root/core/ra_sync_core.c" \
        "$app_root/core/ra_md5.c" \
        "$here/jni/ra_sync_jni.c"
    echo "   $abi: $(wc -c < "$OUT/lib/$abi/libunifrog_rasync.so") bytes"
done

echo "== resources"
"$BT/aapt2" compile --dir "$here/res" -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" \
    --manifest "$here/AndroidManifest.xml" \
    --java "$OUT/gen" "$OUT/res.zip" \
    --min-sdk-version $API --target-sdk-version 34 \
    --version-code $PACKAGE_VERSION_CODE \
    --version-name "$PACKAGE_VERSION_NAME"

echo "== java"
find "$here/java" "$OUT/gen" -name '*.java' -print > "$OUT/sources.txt"
"$JAVA_HOME/bin/javac" -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
    -bootclasspath "$PLATFORM" -classpath "$PLATFORM" \
    -d "$OUT/classes" @"$OUT/sources.txt"

echo "== dex"
find "$OUT/classes" -name '*.class' -print > "$OUT/classes.txt"
"$BT/d8" --release --min-api $API --lib "$PLATFORM" \
    --output "$OUT" @"$OUT/classes.txt"

cp "$OUT/base.apk" "$OUT/app.apk"
rm -rf "$OUT/stage"
mkdir -p "$OUT/stage"
cp -R "$OUT/lib" "$OUT/stage/lib"
cp "$OUT/classes.dex" "$OUT/stage/classes.dex"
(cd "$OUT/stage" && zip -q -r "$OUT/app.apk" classes.dex lib)

echo "== sign"
"$BT/zipalign" -f 4 "$OUT/app.apk" "$OUT/app-aligned.apk"
KS="$app_root/build/keystore/debug.keystore"
mkdir -p "$app_root/build/keystore"
if [ ! -f "$KS" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android \
        -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=UniFrog RA Sync, O=UniFrog, C=VN" >/dev/null
    echo "   created $KS (keep it to publish updates under the same signature)"
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android \
    --key-pass pass:android --min-sdk-version $API \
    --v4-signing-enabled false \
    --out "$DIST/UniFrog-RA-Sync.apk" "$OUT/app-aligned.apk"
"$BT/apksigner" verify --min-sdk-version $API "$DIST/UniFrog-RA-Sync.apk"

"$BT/aapt2" dump badging "$DIST/UniFrog-RA-Sync.apk" | sed -n 1,3p
echo "== built $DIST/UniFrog-RA-Sync.apk ($(wc -c < "$DIST/UniFrog-RA-Sync.apk") bytes)"
