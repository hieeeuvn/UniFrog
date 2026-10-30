#!/bin/sh
# Builds the C core plus the JNI glue as a Linux library and drives it through
# the same Java bridge the APK uses. This is the closest possible check of the
# Android build without a device: everything except SAF and HttpURLConnection
# runs for real.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
app_root=$(cd "$here/../.." && pwd)
repo=$(cd "$app_root/../.." && pwd)
JAVA_HOME=${JAVA_HOME:-$repo/.deps/jdk-17.0.21+9}
OUT="$app_root/build/jvm"

[ -x "$JAVA_HOME/bin/javac" ] || {
    echo "error: JDK not found at $JAVA_HOME" >&2
    exit 1
}
PATH="$JAVA_HOME/bin:$PATH"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/lib"

cc -std=c99 -O1 -g -Wall -Wextra -fPIC -shared \
    -I"$app_root/core" -I"$here/../../android/jni" \
    -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
    -o "$OUT/lib/libunifrog_rasync.so" \
    "$app_root/core/ra_sync_core.c" \
    "$app_root/core/ra_md5.c" \
    "$app_root/android/jni/ra_sync_jni.c"

"$JAVA_HOME/bin/javac" -source 8 -target 8 -encoding UTF-8 -Xlint:-options \
    -d "$OUT/classes" \
    "$app_root/android/java/com/unifrog/rasync/NativeSync.java" \
    "$here/RaSyncJvmTest.java"

"$JAVA_HOME/bin/java" -Djava.library.path="$OUT/lib" \
    -cp "$OUT/classes" RaSyncJvmTest
