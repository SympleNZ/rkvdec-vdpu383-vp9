# Build, deploy, and test

## Requirements

- **SoC:** Rockchip RK3576 (VDPU383). Reference board: NanoPi R76S / ArmSoM Sige5.
- **Kernel:** mainline-based 7.0.x with the VDPU383 `rkvdec` infrastructure
  (validated on Armbian `7.0.1-edge-rockchip64`). You need the matching kernel
  build tree / headers to compile the out-of-tree module.
- **Device tree:** the `video-codec@27b00000` node with `function`/`link` reg
  ranges and (ideally) the `sram` node — the RCB row-context buffers go to
  on-chip SRAM when present.
- **GStreamer path:** `gst-plugins-bad` **≥ 1.28** (provides a working
  `v4l2slvp9dec`). Earlier versions will not decode VDPU383 content correctly.

## Build

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
```

The module references vmlinux symbols not in its own `Module.symvers`; for an
out-of-tree build that surfaces as MODPOST "undefined symbol" errors. Silence
them (they resolve at `insmod` against the running kernel):

```sh
KBUILD_MODPOST_WARN=1 make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
```

Output: `src/rockchip-vdec.ko`.

## Load

```sh
sudo rmmod rockchip_vdec 2>/dev/null
sudo insmod src/rockchip-vdec.ko
dmesg | tail
v4l2-ctl -d /dev/video-dec0 --list-formats-out   # expect VP9F, S264, S265
```

`test/build_and_smoke.sh` does build + reload + format check in one step.

## Decode

```sh
# .ivf -> parsebin (NOT ivfparse); .webm -> matroskademux ! vp9parse
gst-launch-1.0 -q filesrc location=clip.ivf ! parsebin ! v4l2slvp9dec ! \
    videoconvert ! video/x-raw,format=I420 ! filesink location=out.yuv

# NV12 frame @1280x720 = 1382400 bytes. 10-bit content negotiates NV15
# (NV12_10LE40); convert to I420_10LE for comparison with yuv420p10le.
```

## Display (direct-to-DRM/KMS)

```sh
gst-launch-1.0 filesrc location=clip.webm ! matroskademux ! vp9parse ! \
    v4l2slvp9dec ! kmssink force-modesetting=true
```

Notes:
- A portrait clip taller than the display (e.g. 1080x1920 on a 1440p panel)
  fails to place in `kmssink` — that's a display-geometry limit, not a decode
  issue (decode-to-`fakesink`/`filesink` succeeds). Add a scaler to letterbox.
- Some `.webm` files trip `matroskademux` in the kmssink pipeline but decode fine
  via `parsebin ! v4l2slvp9dec ! fakesink` — a container/demux quirk, not decode.

## Conformance (Fluster)

```sh
python3 fluster.py run -d GStreamer-VP9-V4L2SL -ts VP9-TEST-VECTORS -j1
# Current: 148/305 strict-MD5. See CONFORMANCE.md for the per-cluster breakdown
# (the misses are dominated by the VP9 compound-prediction gap (below the V4L2
#  interface; see README) + small-dimension size stressors, plus downstream gst/comparator.)
```

**Reboot before a fresh baseline** — decoder/HW state bleeds across runs and can
turn a clean run into a timeout-laden one. Throughput in particular swings wildly
(3.3–177 fps) by CPU-governor / idle state on contaminated content; use the
`performance` governor and compound-free content for stable decode-rate numbers.

## Module parameters

| param | meaning |
|---|---|
| `vp9_skip_tlb_flush` | IOMMU TLB-flush policy: `0` flush-only-after-restore (default, the throughput fix), `1` never (debug), `2` always per-frame (old behaviour / A-B baseline) |
| `vp9_time` | print mean pure-HW decode time (kick→DONE IRQ) every 100 frames |
| `rkvdec_link_mode` | `0` single-shot submit (default), `1` link/batched submit |
| `vp9_perturb_refs` | diagnostic: redirect a small inter frame's non-alt reference legs to a scratch buffer; output unchanged proves the references are not read (see REF_BYPASS_BUG.md) |
| `vp9_dump_ctrls` | dump each frame's `v4l2_ctrl_vp9_frame` as one hex line |

Several other `r3x`/`r4x` and `vp9_*` parameters are retained inert experimental
probes from the investigation; all default off and have no effect at their
defaults.

## Useful diagnostics

- Per-frame decode classification + `reference_mode`: enable the `vp9-run`
  pr_debug (`echo 'file rkvdec-vdpu383-vp9.c +p' >
  /sys/kernel/debug/dynamic_debug/control`) and read `/dev/kmsg` during decode
  (the dmesg ring drops the fast per-frame lines under load).
- Counting compound frames in a clip: decode to `fakesink` with the above
  enabled and `grep -c 'refmode=2'`. (Compound correlates with the failing
  small-footprint frames but is not the cause — single-ref small frames fail too.)
- Pure HW decode rate: `vp9_time=1` + `performance` governor + compound-free
  content.
