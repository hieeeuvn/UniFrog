#!/bin/sh
#
# Cross-build the Windows clients from Linux with Zig. Zig ships its own
# mingw-w64 headers and import libraries, so no Windows or MSVC install is
# needed. Output goes to tools/ra-sync-gui/dist (gitignored).
#
#   ./build_windows.sh            build 32-bit and 64-bit
#   ZIG=/path/to/zig ./build_windows.sh
#
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
zig=${ZIG:-$root/.deps/zig-linux-x86_64-0.13.0/zig}
out=${OUT:-$here/../dist}

[ -x "$zig" ] || {
   echo "missing zig: $zig (set ZIG=/path/to/zig)" >&2
   exit 1
}

mkdir -p "$out"

sources="
$here/main_win32.c
$here/ra_fs_win32.c
$here/ra_http_winhttp.c
$here/../core/ra_sync_core.c
$here/../core/ra_md5.c
"

libraries="-lwinhttp -lole32 -lshell32 -luser32 -lgdi32"

build() {
   arch=$1
   name=$2

   echo "  CC      $name ($arch-windows-gnu)"
   "$zig" cc -target "$arch-windows-gnu" -O2 -Wall -Wextra \
      -Wl,--subsystem,windows $sources $libraries -o "$out/$name"
}

build x86 "UniFrog-RA-Sync-32.exe"
build x86_64 "UniFrog-RA-Sync-64.exe"

rm -f "$out"/*.pdb
echo "  OK      $out"
ls -l "$out"
