#!/usr/bin/env bash
# Build this app into build/<id>.t5app. CI runs exactly this.
#
#   ./build.sh                 fetch the SDK named in tab5os-sdk.txt (once) and build
#   ./build.sh --sign KEY.pem  the same, signed with your key (make one with `t5 keygen`)
#
# The SDK goes into .t5sdk/ and is checked against the release's SHA256SUMS.
# Set T5_SDK to an SDK's sdk/ folder (an unpacked release, or a tab5os
# checkout's sdk/ with a firmware build) to use that one instead.
#
# Needs the ESP-IDF v6.1 RISC-V toolchain (riscv32-esp-elf-gcc), CMake 3.22+,
# Ninja, Python 3.11+ and curl. With ESP-IDF installed, the script sources
# $IDF_PATH/export.sh itself when the compiler isn't on PATH.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VER="$(tr -d '[:space:]' < "$HERE/tab5os-sdk.txt")"
REPO="${T5_SDK_REPO:-tab5os/tab5os}"

if [ -z "${T5_SDK:-}" ]; then
    DIR="$HERE/.t5sdk/tab5os-sdk-$VER"
    if [ ! -f "$DIR/sdk/cmake/t5_app.cmake" ]; then
        echo "fetching Tab5 OS SDK $VER from $REPO"
        tmp="$(mktemp -d)"
        trap 'rm -rf "$tmp"' EXIT
        url="https://github.com/$REPO/releases/download/v$VER"
        curl -fsSL -o "$tmp/sdk.tar.gz" "$url/tab5os-sdk-$VER.tar.gz"
        curl -fsSL -o "$tmp/SHA256SUMS" "$url/SHA256SUMS"
        want="$(awk -v f="tab5os-sdk-$VER.tar.gz" '{ sub(/^\.\//, "", $2) } $2 == f { print $1 }' "$tmp/SHA256SUMS")"
        [ -n "$want" ] || { echo "SHA256SUMS of v$VER doesn't list tab5os-sdk-$VER.tar.gz" >&2; exit 1; }
        if command -v sha256sum >/dev/null; then have="$(sha256sum "$tmp/sdk.tar.gz" | cut -d' ' -f1)"
        else have="$(shasum -a 256 "$tmp/sdk.tar.gz" | cut -d' ' -f1)"; fi
        [ "$want" = "$have" ] || { echo "SDK checksum mismatch: want $want, got $have" >&2; exit 1; }
        mkdir -p "$HERE/.t5sdk"
        tar -C "$HERE/.t5sdk" -xzf "$tmp/sdk.tar.gz"
    fi
    T5_SDK="$DIR/sdk"
fi
T5_SDK="$(cd "$T5_SDK" && pwd)"

if ! command -v riscv32-esp-elf-gcc >/dev/null && [ -n "${IDF_PATH:-}" ]; then
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh" >/dev/null
fi
command -v riscv32-esp-elf-gcc >/dev/null || { echo "riscv32-esp-elf-gcc isn't on PATH: install ESP-IDF v6.1, or its RISC-V toolchain" >&2; exit 1; }

export T5_SDK
exec "$T5_SDK/../tools/t5/t5" app-build "$HERE" "$@"
