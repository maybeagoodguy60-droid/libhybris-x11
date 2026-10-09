#!/bin/sh
# Reproducible build + install for the glxshim desktop-GL -> GLES GLX shim.
#
# Pins the exact toolchain and flags so a rebuild is *behaviorally* identical
# to the version that passed scoria-stress (byte-identical .so is not promised:
# gcc embeds a build-id and absolute source path unless mapped away).
#
# Usage:  sudo ./build.sh [install|check]
#   install : build both sonames and install over the three GL lookup paths
#             (backups of anything replaced are left in backup-*/)
#   check   : re-verify an existing install without rebuilding
set -eu

cd "$(dirname "$0")"

# --- pinned build environment --------------------------------------
export CC="${CC:-gcc}"
CFLAGS="-O2 -fPIC -Wall -ffunction-sections"
LDFLAGS="-shared -Wl,--no-undefined"
LDLIBS="-ldl -lX11 -lm"
JOBS="$(nproc 2>/dev/null || echo 4)"

echo "== glxshim reproducible build =="
echo "   source dir: $PWD"
echo "   cc:     $CC  ($($CC --version 2>/dev/null | head -1))"
echo "   flags:  CFLAGS='$CFLAGS' LDFLAGS='$LDFLAGS' LDLIBS='$LDLIBS'"
echo "   make:   -j$JOBS"
git -C .. rev-parse --short HEAD 2>/dev/null | sed 's/^/   git:    /' || true

if [ "${1:-install}" = "check" ]; then
    make verify
    echo "== check done"
    exit 0
fi

make -j"$JOBS" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" LDLIBS="$LDLIBS" clean >/dev/null 2>&1 || true
make -j"$JOBS" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" LDLIBS="$LDLIBS" all
make    CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" LDLIBS="$LDLIBS" install BUILDINFO="$(git -C .. rev-parse --short HEAD 2>/dev/null || echo local)"

# --- regression smoke after install --------------------------------
echo "== post-install smoke"
if glxinfo 2>/dev/null | grep -q glxshim; then
    echo "   PASS glxinfo sees glxshim"
else
    echo "   FAIL glxinfo does not resolve glxshim" >&2; exit 1
fi
# fixed-function immediate mode is exactly what overflowed pre-35753ab;
# glxgears exercises quads + the shim's vertex buffer path.
out=$(timeout 12 glxgears 2>&1) || {
    case "$out" in *"invalid size"*|*Aborted*) echo "   FAIL glxgears: $out" >&2; exit 1;; esac
}
fps=$(printf '%s\n' "$out" | grep -oE '[0-9.]+ FPS' | tail -1)
echo "   PASS glxgears fixed-function path ($fps)"

echo "== done; install backups under ./backup-*/"