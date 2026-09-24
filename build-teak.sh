#!/bin/bash
#
# Build and install the Teak toolchain (clang + lld) for BlocksDS libteak.
#
# Usage: ./build-teak.sh [configure] [build] [check] [install] [all]
#
# Environment:
#   PREFIX    Install prefix (default: $BLOCKSDSEXT/llvm-teak)
#   BLOCKSDSEXT  BlocksDS external libraries (default: /opt/blocksds/external)
#   BUILDDIR  Build directory (default: build)
#   JOBS      Parallel jobs passed to ninja (default: ninja's own default)

set -euo pipefail

SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-${BLOCKSDSEXT:-/opt/blocksds/external}/llvm-teak}"
BUILDDIR="${BUILDDIR:-$SRCDIR/build}"

COMPONENTS="clang;clang-resource-headers;lld;llvm-ar;llvm-ranlib;llvm-mc;llvm-objcopy;llvm-objdump;llvm-readobj;llvm-nm;llvm-size"

NINJA_ARGS=()
if [ -n "${JOBS:-}" ]; then
    NINJA_ARGS+=(-j "$JOBS")
fi

do_configure() {
    cmake -S "$SRCDIR/llvm" -B "$BUILDDIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="" \
        -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=Teak \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DLLVM_DISTRIBUTION_COMPONENTS="$COMPONENTS"
}

do_build() {
    ninja -C "$BUILDDIR" "${NINJA_ARGS[@]}" distribution
}

do_check() {
    # The lit tools are not part of the distribution target
    ninja -C "$BUILDDIR" "${NINJA_ARGS[@]}" FileCheck count not llvm-config \
        llc teak-roundtrip
    python3 "$BUILDDIR/bin/llvm-lit" -sv \
        "$SRCDIR/llvm/test/MC/Teak" \
        "$SRCDIR/llvm/test/CodeGen/Teak" \
        "$SRCDIR/lld/test/ELF/teak-relocs.s" \
        "$SRCDIR/clang/test/Driver/teak-integrated-as.s" \
        "$SRCDIR/clang/test/CodeGen/teak-pointer-struct.c"
}

do_install() {
    ninja -C "$BUILDDIR" "${NINJA_ARGS[@]}" install-distribution

    local missing=0
    for tool in clang clang++ ld.lld llvm-ar llvm-objdump; do
        if [ ! -x "$PREFIX/bin/$tool" ]; then
            echo "error: $PREFIX/bin/$tool is missing after install" >&2
            missing=1
        fi
    done
    [ "$missing" -eq 0 ]

    echo "Installed Teak toolchain to $PREFIX"
}

if [ $# -eq 0 ]; then
    set -- all
fi

for step in "$@"; do
    case "$step" in
        configure) do_configure ;;
        build)     do_build ;;
        check)     do_check ;;
        install)   do_install ;;
        all)       do_configure; do_build; do_check; do_install ;;
        *)
            echo "usage: $0 [configure] [build] [check] [install] [all]" >&2
            exit 1
            ;;
    esac
done
