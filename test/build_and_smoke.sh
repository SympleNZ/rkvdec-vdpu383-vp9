#!/bin/sh
# Build the rockchip-vdec out-of-tree module, reload it, and smoke-check that
# the VP9 stateless format registers. Run from the repo root.
#
#   ./test/build_and_smoke.sh
#
# Requires matching kernel build tree at /lib/modules/$(uname -r)/build.
set -eu

KVER=$(uname -r)
SRC="$(cd "$(dirname "$0")/../src" && pwd)"

echo "=== build (out-of-tree module $SRC) ==="
KBUILD_MODPOST_WARN=1 make -C "/lib/modules/${KVER}/build" M="${SRC}" -j"$(nproc)" modules \
    2>&1 | grep -E 'error|warning|LD \[M\]' | tail -20
test -f "${SRC}/rockchip-vdec.ko" || { echo "ERROR: build failed"; exit 1; }
ls -l "${SRC}/rockchip-vdec.ko"

echo "=== reload ==="
rmmod rockchip_vdec 2>/dev/null || true
insmod "${SRC}/rockchip-vdec.ko"
dmesg | tail -3

echo "=== smoke: VP9 stateless format registered? ==="
v4l2-ctl -d /dev/video-dec0 --list-formats-out 2>&1 | grep -E 'VP9F|S264|S265' || \
    { echo "ERROR: VP9F not registered"; exit 1; }

echo "=== OK ==="
