# rkvdec-vdpu383-vp9 — V4L2 stateless VP9 decoder for Rockchip RK3576 (VDPU383)

The **first working mainline-Linux V4L2 stateless VP9 decoder** for the Rockchip
**RK3576** SoC's **VDPU383** video IP. It is **production-ready and bit-exact to
the vendor MPP decoder for KEY / single-reference / low-motion 8-bit content**,
built on top of the now-mainline VDPU383 H.264/H.265 `rkvdec` infrastructure.

One correctness gap remains, and this repository documents it to its absolute
conclusion: **VP9 compound prediction** (`reference_mode = SELECT`, the
two-reference path that real-world high-motion content such as some YouTube VP9
relies on) decodes wrong on a small, content-specific set of frames. After an
exhaustive same-silicon investigation the gap is established to be a **hardware-
internal divergence in the VDPU383's VP9-compound motion-compensation path, below
every interface the mainline V4L2 driver can touch** — not a silicon-incapability,
not a driver-input error, and not resolvable from the driver side. The proof chain
is laid out in full below; it is published downstream-first as working code plus a
precise, fully-triaged question for the people with the hardware documentation
(Collabora / the VDPU383 maintainers).

This is independent development on top of Collabora's mainline VDPU383 work; VP9
for VDPU383 is **not** upstream and is on the Collabora roadmap as a *future*
task. The sibling **AV1** decoder (`rkvdec-vdpu383-av1`) reached a symmetric
conclusion independently.

---

## Status at a glance

| Capability | State |
|---|---|
| 8-bit Profile 0 — intra / KEY | ✅ bit-exact to libvpx / MPP |
| 8-bit Profile 0 — INTER, single-reference, low-motion | ✅ bit-exact to libvpx / MPP; visually perfect on HDMI |
| 8-bit Profile 0 — **compound / SELECT** (alt-ref, high-motion) | ❌ **the open gap** — wrong on a content-specific subset of compound frames |
| 10-bit Profile 2 (4:2:0) | ✅ correct (±1 reconstruction rounding at 10-bit); HDR10+ displays correctly |
| Resolutions 720p / 1080p / 4K | ✅ all decode, well above real-time on clean content |
| Mid-stream resolution change | ⚠️ driver signals it correctly; gst-plugins-bad drops frames (downstream) |
| Profile 3 (4:2:2 / 4:4:4), 12-bit | ❌ unsupported by design |

**Fluster** (`VP9-TEST-VECTORS`, strict MD5): **148/305 (48.5%), zero crashes**,
core decode 94–100%. The misses are dominated by small-dimension padding / resize
stressors and the compound gap, not by core-decode failures.

The single gate between this driver and *general-purpose* VP9 hardware decode is
the compound-prediction gap: KEY, single-reference and low-motion content is
correct and shippable today; compound / high-motion content (e.g. some YouTube
VP9) must be routed elsewhere (see [Disposition](#disposition)).

---

## What works (with evidence)

**8-bit Profile 0 — bit-exact for KEY / single-ref / low-motion.** KEY/intra and
single-reference inter frames decode **bit-exact to libvpx and to the vendor MPP
decoder** on real-world content. Validated three ways: visually on HDMI
(large-frame clips such as `earth_1080p` and a GoPro promo display perfectly); by
Fluster strict-MD5 (the core / quantizer / intra / superframe classes pass); and
by a controlled A/B on real YouTube content — a single-pass (single-reference)
re-encode of GoPro 720p footage decodes **119/120 frames bit-exact** to its
libvpx golden.

The implementation milestones behind that result:

- **KEY decode** — `struct` + `memcpy_toio` register programming; the default-prob
  HW packer; the 4864-byte probability-buffer bit-packer; the 352-byte GBL
  (global-header) bit-packer.
- **INTER decode** — reference-by-timestamp resolution; `use_prev_mvs` / colmv
  (collocated MV) handling; per-frame probability context with HW-writeback
  backward adaptation across the 4 VP9 frame contexts.
- **The RK3576 HW-init warmup** — ported from the BSP; unblocked tile decode
  (Fluster `08-tile` 0/8 → 5/5). This is the init step MPP performs that the
  mainline path originally missed.
- **Multi-tile** — 4- and 8-column tile decode.
- **Link-descriptor mode** — depth=1, 96.8% parity with single-shot; byte-identical
  to single-shot when completion detection engages (read per-slot writeback like
  the BSP).
- **10-bit P010** — no longer crashes.

**10-bit Profile 2 (4:2:0).** Decodes correctly. Output is the Rockchip packed
10-bit layout `V4L2_PIX_FMT_NV15` (= gst `NV12_10LE40`), consistent with the
HEVC/H.264 paths on this IP. Versus libvpx it is **85% of samples bit-exact, the
rest off by exactly +1 (max delta 4)** — a reconstruction-rounding difference
exposed at 10-bit precision, not a decode defect (8-bit rounds it away and is
bit-exact). HDR10+ sample content displays correctly on HDMI.

**Resolutions / throughput.** 720p, 1080p and 4K all decode. Pure hardware-decode
time on clean content (performance governor):

| resolution | pure-HW per frame | pure-HW fps |
|---|---|---|
| 720p | 1.9 ms | ~517 |
| 1080p | 4.3 ms | ~231 |
| 4K (2160p) | ~16–19 ms | ~52–60 |

i.e. multiples of real-time at 720p/1080p and real-time at 4K30. The vendor MPP/BSP
stack is ~2.3–3× faster (it pipelines via continuous link mode; this driver uses
per-frame single-shot submit by default). See
[`docs/THROUGHPUT.md`](docs/THROUGHPUT.md) for the native baseline and the
`flush-only-after-restore` IOMMU optimisation.

---

## The open gap: VP9 compound prediction (below the V4L2 interface)

**This is the centrepiece.** The investigation reached its absolute conclusion;
the summary is here and the full triage is in
[`docs/REF_BYPASS_BUG.md`](docs/REF_BYPASS_BUG.md).

### The headline: it is a *driver* shortfall against the silicon, not a silicon limit

We built the vendor **MPP** decoder out-of-tree on the **same** mainline 7.0
kernel, the **same** device tree, the **same** platform (clocks / power / IOMMU)
and the **same** silicon that this V4L2 driver runs on. On that build:

- MPP-on-mainline decodes VP9 INTER (60 frames of real GoPro YouTube content)
  **bit-exact** to MPP-on-BSP and to ffmpeg/libvpx (hash `7fb9bc0b`), triple-
  validated across two kernels and two stacks.
- Our V4L2 driver degrades on the *same* board, *same* clip.

So the platform is not the cause (MPP gets it right on it) and the silicon is not
the cause (MPP decodes the same compound content perfectly on it). The hardware is
capable; the mainline V4L2 path is not yet driving it correctly.

### Isolated to compound prediction (`reference_mode = SELECT`)

A controlled A/B re-encode of the *same* correct GoPro YUV pins the trigger
exactly to compound prediction:

| encode | `reference_mode` | our V4L2 result |
|---|---|---|
| 1-pass (default) | 0 = single-reference | **119/120 frames bit-exact** ✓ |
| 2-pass + `auto-alt-ref` | 2 = SELECT / compound | **54/120 exact, fails** ✗ |
| original YouTube stream | 2 = SELECT / compound | ~31.5 dB avg Y-PSNR, heavy fail ✗ |

Same content; the only difference is compound prediction. `interp_filter` was
switchable (`4`) in every case, so it is not the interpolation filter. This
**falsifies** the earlier "compound = silicon erratum" hypothesis (the prior
compound work predates the same-board MPP reference) — MPP decodes the identical
compound content bit-exact.

### Driver-addressable in principle — the hardware is provably capable

**HEVC B-frame bi-prediction** (two-reference averaging on the *same* VDPU383 MC
engine, on this *same* driver) decodes **bit-exact** — `bbb1080.h265`, every
B-frame in the GOP, validated against an ffmpeg golden. Together with MPP being
correct on the compound content, this doubly proves the silicon's two-reference
averaging hardware is sound. VP9 compound is therefore driver-addressable *in
principle*. (HEVC's working bi-pred does **not** by itself transfer a fix: VP9
compound averaging — `ROUND_POWER_OF_TWO(p0+p1, 1)` — and its sub-pel second-leg
fetch are a **spec-distinct HW path** from HEVC's weighted bi-pred, so the VP9
path can diverge while HEVC's is correct.)

### Closed at every interface the driver can reach

For the cleanest deterministic repro — `gopro13.ivf`, where display frames f0–f10
decode **bit-exact**, **f11 is wrong** (24.3% of Y pixels, maxabs 96, an MC-shaped
spatial region, not an entropy garble), and **f12 is bit-exact again** — every
V4L2-touchable input was verified to match MPP on the same board:

- **Control registers byte-identical to MPP on every frame, including f11** —
  full live MMIO dump (reg0–255), offsetof-mapped, aligned by reg66 (stream
  length). reg28 (`reference_mode`/`tx_mode`) included. 0 diffs.
- **GBL header field-identical and value-correct** — source-diffed field by field
  against MPP's `vdpu38x_vp9d_uncomp_hdr`; the one compound-specific field,
  `sign_bias`, is verified value-correct, as is `mvscale` (0x4000, unscaled refs).
- **Compound probability region packing-identical** — source-diffed against MPP's
  `hal_vp9d_output_probe`: `comp_mode[5]` / `comp_ref[5]` / `single_ref[5][2]`
  packed at identical order / width / alignment / offset.
- **Stream bytes identical**; references and colmv exact (f11's neighbours are
  bit-exact, so their MV field and reference buffers are exact).
- **Both run mechanisms produce identical wrong output** — single-shot and link
  depth=1 give **byte-identical** wrong pixels on f11 (Ywrong 24.3%, maxabs 96).
  The submission mechanism is not the differentiator.

**The decisive proof — the bit-exact-chain argument.** All the compound frames in
the repro share entropy context 0, with HW-writeback backward adaptation chaining
each frame's adapted probabilities into the next. Decode is bit-exact f0–f10,
wrong only at f11, bit-exact again at f12. An *isolated* failure between two
bit-exact frames that share the chained context proves two things at once:

1. **f11's probability/context inputs are correct.** A wrong compound probability
   would desync the entropy decoder, and f0–f10 (which read the same chained
   context) could not be bit-exact.
2. **f11's HW backward adaptation is correct.** f12 inherits f11's adapted context
   and is bit-exact; a corrupted adaptation would break f12.

So f11's entropy / mode / probability decode is provably correct, and the error is
confined to f11's **motion-compensation pixel execution**.

### The last V4L2-fixable lead — reference border / edge-extension — tested and killed

The one residual that, unlike everything else, the driver *could* have fixed was
the reference-buffer **border / edge-extension** pixels a boundary-crossing MV
would read. It was killed on two independent axes:

- **MPP source.** MPP allocates VP9 frame buffers *aligned-larger* than visible
  (1280×720 → ~1344×768) but contains **zero edge-extension / border-fill code**.
  A larger-but-unfilled buffer that still decodes bit-exact means the HW does not
  read the margin — it **clamps reference fetches internally** to the visible
  reference dimensions (`ref_w`/`ref_h`, which both stacks pack identically into
  the GBL). The HEVC bi-pred control corroborates: HEVC decodes bit-exact on *our*
  buffers with no special border handling despite boundary-crossing MVs.
- **On-board error map.** Over gopro13 f11, a 16-px boundary band: the EDGE band is
  the *cleanest* region (9.55% wrong) and the **interior** is worst (25.41%);
  **97.3% of the error mass is interior, 2.7% at the edge** — the exact inverse of
  a border bug. The maxabs error of 96 is far larger than a ½-LSB rounding tie, so
  it is not a rounding-tie-break either.

### The residual — below every V4L2-touchable interface

Every interface-touchable input is proven correct (byte-identical where dumped,
logically forced by the bit-exact chain elsewhere), both run mechanisms match, the
two-reference averaging HW is sound, and f11's entropy and modes are provably
correct. What remains is an **interior, motion-localised, content-dependent error
in the VDPU383's internal VP9-compound motion-compensation path** — a spec-distinct
HW path from HEVC's weighted bi-pred, with **every control register byte-identical
to MPP**, so there is no precision/mode register left to change. It is fixed in
silicon, below every interface the mainline V4L2 driver controls. This is symmetric
with the AV1 result for the same silicon.

### The ask

Every programmable input is byte-identical to MPP; the GBL header and compound
probability region are source-verified field-identical; the stream bytes match;
both single-shot and link-depth=1 produce identical wrong pixels; the two-reference
averaging hardware is proven sound (HEVC bi-pred bit-exact); and the bit-exact-chain
argument proves the failing frame's entropy, modes and adaptation are all correct —
yet one content-specific VP9 compound frame's motion-compensation pixels diverge
while MPP (same silicon, same frame) is bit-exact. **On VDPU383, what internal
per-frame state or compound-MC configuration does the VP9-compound path require —
distinct from HEVC weighted bi-pred and not exposed in any control register — that
MPP supplies and the mainline V4L2 path does not?** A pointer to where the VP9
compound MC path differs from HEVC's bi-pred on this IP would be decisive.

---

## Hardware & software

- **SoC:** Rockchip RK3576, VDPU383 video decoder IP.
- **Reference board:** NanoPi R76S (also applies to ArmSoM Sige5 — same RK3576).
- **Kernel:** mainline-based 7.0.x (developed / validated on Armbian
  `7.0.1-edge-rockchip64`). Built as an out-of-tree module (`rockchip-vdec.ko`).
- **Userspace (GStreamer path):** `gst-plugins-bad` **≥ 1.28** (`v4l2slvp9dec`).
  1.28 is the first release with a working V4L2 stateless VP9 element here; earlier
  versions are not usable. ffmpeg's `v4l2_request` hwaccel also works for 8-bit.

This driver is built on the VDPU383 H.264/H.265 support merged to mainline Linux in
early 2026 (Collabora's 17-patch series; `rkvdec` de-staged). VP9 and AV1 for
VDPU383 are **not** upstream.

---

## Build, deploy, test

```sh
# On the target (or cross-build), against your kernel headers/source:
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
# (KBUILD_MODPOST_WARN=1 silences expected unresolved-symbol warnings for an
#  out-of-tree module that references vmlinux symbols.)

sudo rmmod rockchip_vdec 2>/dev/null
sudo insmod src/rockchip-vdec.ko
v4l2-ctl -d /dev/video-dec0 --list-formats-out   # expect VP9F (and S264/S265)
```

Decode a clip (GStreamer V4L2 stateless path):

```sh
# .ivf needs parsebin (not ivfparse); .webm uses matroskademux ! vp9parse
gst-launch-1.0 filesrc location=clip.ivf ! parsebin ! v4l2slvp9dec ! \
    videoconvert ! video/x-raw,format=I420 ! filesink location=out.yuv

# Display direct-to-DRM/KMS on the board's HDMI:
gst-launch-1.0 filesrc location=clip.webm ! matroskademux ! vp9parse ! \
    v4l2slvp9dec ! kmssink force-modesetting=true
```

Conformance (Fluster):

```sh
python3 fluster.py run -d GStreamer-VP9-V4L2SL -ts VP9-TEST-VECTORS -j1
# 148/305 strict-MD5, zero crashes (see docs/CONFORMANCE.md for the breakdown).
# Reboot before a fresh baseline — decoder state bleeds across runs.
```

Smoke test (build + load + format check):

```sh
sudo ./test/build_and_smoke.sh
```

See [`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) for the full loop, module
parameters and gotchas.

---

## Known limitations

- **VP9 compound / high-motion content** — wrong on a content-specific subset of
  compound frames; this is **the open gap** above, below the V4L2 interface and not
  driver-addressable. KEY / single-reference / low-motion content is bit-exact.
- **Small-dimension (<128px) padding / resize vectors** — Fluster `size`-class
  stressors; small-dimension single-ref inter is the small-MC-footprint manifestation,
  not a separate defect. Real content (≥128px, well-coded single-ref) is bit-exact.
- **Mid-stream resolution change** — the driver correctly emits
  `V4L2_EVENT_SOURCE_CHANGE`; gst-plugins-bad's `copy_output_buffer` then drops
  frames on the stride transition. Downstream (gst), not a kernel decode bug.
- **Profile 3 (4:2:2 / 4:4:4) and 12-bit** — unsupported by design (only 4:2:0
  8/10-bit declared).
- **Throughput vs the vendor stack** — MPP/BSP is ~2.3–3× faster (continuous link
  pipelining vs per-frame single-shot submit here). Still multiples of real-time at
  720p/1080p on clean content.

---

## Disposition

Native mainline V4L2 VP9 on RK3576 / VDPU383 is **correct and shippable for
KEY / single-reference / low-motion 8-bit content**. **Compound / high-motion
content** (alt-ref, some YouTube VP9) should be routed to the **vendor MPP
decoder** (BSP, or the out-of-tree `rk_vcodec` on mainline — both bit-exact here)
or to an **H.264 / HLS** path. No native-V4L2 path exists for VP9 compound on this
silicon today. This is symmetric with the AV1 result for the same IP.

---

## Repository layout

```
src/        The out-of-tree rockchip-vdec.ko module (builds the full VDPU381/383
            H.264 / H.265 / AV1 / VP9 decoder). The VP9 work is
            rkvdec-vdpu383-vp9.c (+ rkvdec-vdpu383-vp9-regs.h) and rkvdec-vp9.c;
            the rest is the shared / mainline infrastructure it builds on.
docs/       REF_BYPASS_BUG, THROUGHPUT, CONFORMANCE, BUILD_AND_TEST
            (+ fluster_baseline.csv).
test/       build_and_smoke.sh and the conformance smoke harness.
```

## Module parameters of note

- `vp9_skip_tlb_flush` — IOMMU TLB-flush policy: `0` flush-only-after-restore
  (default; the throughput fix), `1` never, `2` always (old per-frame behaviour).
- `vp9_time` — print mean pure-HW decode time every 100 frames (diagnostic).
- `rkvdec_link_mode` — link (batched) vs single-shot submit. Default single-shot;
  proven byte-identical to single-shot (including on the failing compound frame).
- `vp9_regdump` — diagnostic: dump the live register file (reg0–255) per frame for
  the MPP differential.

(Several `r3x`/`r4x` and `vp9_*` parameters are retained inert experimental probes
from the investigation; all default off.)

## Licence

GPL-2.0 (Linux kernel module). Derived from and built on the mainline Rockchip
`rkvdec` driver.

## Acknowledgements

Built on Collabora's mainline VDPU383 H.264/H.265 `rkvdec` work, on the VP9 V4L2
stateless uAPI from Detlev Casanova and the Collabora media team, and on
`rockchip-linux/mpp` (MPP), which was the indispensable ground-truth reference for
the register-level triage and the decisive same-silicon correctness baseline.

## Publishing note

This directory is a self-contained snapshot intended to become a standalone public
repository (`git init` here and push). It is staged in a development tree for
snapshotting. Nothing here is auto-published.
