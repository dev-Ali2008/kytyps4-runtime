#!/usr/bin/env bash
set -euo pipefail

# One-command build for the rv2ide on-device IDE. Locates the pinned NDK r26
# (26.1.10909125) and the IDE's cmake/ninja inside the IDE sandbox, then runs
# the canonical build-box64.sh. Produces app/src/main/jniLibs/arm64-v8a/libbox64.so

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

SDK_DIR=""
for base in /data/user/0/com.tom.rv2ide /data/data/com.tom.rv2ide; do
  if [ -d "$base/files/home/android-sdk" ]; then
    SDK_DIR="$base/files/home/android-sdk"
    break
  fi
done
if [ -z "$SDK_DIR" ]; then
  echo "android-sdk not found under com.tom.rv2ide (expected files/home/android-sdk)" >&2
  exit 1
fi

NDK="$SDK_DIR/ndk/26.1.10909125"
test -f "$NDK/build/cmake/android.toolchain.cmake" || {
  echo "pinned NDK 26.1.10909125 not found in $SDK_DIR/ndk" >&2
  exit 1
}

CMAKE_BIN="$(ls -d "$SDK_DIR"/cmake/*/bin 2>/dev/null | head -1)"
[ -n "$CMAKE_BIN" ] || {
  echo "no cmake install found under $SDK_DIR/cmake" >&2
  exit 1
}

export ANDROID_NDK_ROOT="$NDK"

SHIM=""
if ! command -v readelf >/dev/null 2>&1; then
  SHIM="$(mktemp -d)"
  for tag in linux-aarch64 linux-x86_64; do
    if [ -x "$NDK/toolchains/llvm/prebuilt/$tag/bin/llvm-readelf" ]; then
      ln -s "$NDK/toolchains/llvm/prebuilt/$tag/bin/llvm-readelf" "$SHIM/readelf"
      break
    fi
  done
fi
if [ -n "$SHIM" ] && [ ! -e "$SHIM/readelf" ]; then
  echo "readelf not available on PATH and no NDK llvm-readelf found" >&2
  exit 1
fi
export PATH="${SHIM:+$SHIM:}$CMAKE_BIN:$PATH"

echo "NDK:  $NDK"
echo "PATH: ${SHIM:+$SHIM:}$CMAKE_BIN"
exec bash "$script_dir/build-box64.sh" "$@"
