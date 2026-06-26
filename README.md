# rkvdec-vdpu383-vp9 — V4L2 stateless VP9 decoder for Rockchip RK3576 (VDPU383)

A working **V4L2 stateless VP9 decoder** for the Rockchip **RK3576** SoC's
**VDPU383** video IP — the first of its kind. It decodes real-world 8-bit and
10-bit VP9 (Profile 0 and Profile 2) correctly and well above real-time, built
on top of the now-mainline VDPU383 H.264/H.265 `rkvdec` infrastructure.

**One outstanding bug** affects VP9 **inter** frames on certain content. After
exhaustive triage it is established to be a **hardware-internal divergence below the
register interface**: every programmable input is byte-identical to the vendor
library (MPP), yet MPP on the *same* VDPU383 silicon decodes the same streams
**bit-exact to the libvpx/ffmpeg software reference** while our V4L2 stack does not.
It is not resolvable from the driver side. This repo is published downstream-first:
working code plus a precise, fully-triaged question for the people with the hardware
documentation (Collabora / the VDPU383 maintainers). See
[The open bug](#the-open-bug-below-the-register-interface).

## Status

The driver decodes 8-bit and 10-bit VP9 (Profiles 0 and 2) at all resolutions to
4K, well above real-time. The one correctness gap is VP9 **inter** decode, which
fails on two kinds of content — both traced to the *same* hardware-internal wall:

- **Reference-bypass on small-MC-footprint frames** (sparse/skip-heavy *or*
  sub-128px): the frame reconstructs from a retained internal copy of the
  most-recent reference instead of the reference at the programmed DRAM address.
- **Sub-pel-precision drift on complex / high-q inter frames**: a subtle per-frame
  error that cascades through references.

Both were triaged to exhaustion and converge on one conclusion: the bug is below
the MMIO register interface, in the hardware's internal execution. Every
programmable input is byte-identical to MPP — the register file (`ctrl_regs` +
`comm_paras`, 0-diff via the offsetof field-map), the GBL header, the full
entropy-probability buffer, and the stream bytes the hardware reads. Config reaches
the hardware the same way MPP delivers it (verified down to the DRAM-descriptor
fetch path); the submission sequence is identical to the *working* HEVC backend; and
a continuously-armed link ring plus the complete per-frame clock / IOMMU / reset /
PM / warmup operation set were replicated from MPP. The output is still wrong, while
MPP — same silicon, same frame — is bit-exact. The decode even **starts** correct
(on the AV1 sibling, frame 0's first 16 bytes are byte-exact to the reference) and
diverges *during* the hardware's own internal pass. There is no remaining
driver-side input or operation that changes the result; the investigation is
**terminal** from the driver side. The sibling **AV1** decoder
(`rkvdec-vdpu383-av1`) reached the identical wall independently — this is one
hardware-internal issue across both VDPU383 codecs.

---

## TL;DR

| Capability | State |
|---|---|
| 8-bit Profile 0 — intra / KEY | ✅ byte-exact to libvpx |
| 8-bit Profile 0 — INTER, large-footprint frames (≥128px, well-coded) | ✅ byte-exact to libvpx; visually perfect on HDMI |
| 8-bit Profile 0 — compound / SELECT (alt-ref) on well-coded frames | ✅ byte-exact (compound prediction works) |
| 10-bit Profile 2 (4:2:0) | ✅ correct (±1 reconstruction rounding); HDR10+ displays correctly |
| Resolutions 720p / 1080p / 4K | ✅ all decode; well above real-time on clean content |
| **INTER frames with a small MC footprint** (sparse/skip-heavy *or* <128px) | ❌ **the open bug** — MC served from stale internal reference |
| Mid-stream resize | ⚠️ driver signals it; gst-plugins-bad drops frames (downstream) |
| Profile 3 (4:2:2 / 4:4:4), 12-bit | ❌ unsupported by design |

Because real streaming content (YouTube, etc.) is full of small prediction-heavy
inter frames (alt-ref overlays, skip-heavy P-frames), this bug is the single gate
between this driver and general-purpose VP9 hardware decode. Large-frame and
compound-on-well-coded content works; everything *else* works.

---

## Hardware & software

- **SoC:** Rockchip RK3576, VDPU383 video decoder IP.
- **Reference board:** NanoPi R76S (also applies to ArmSoM Sige5 — same RK3576).
- **Kernel:** mainline-based 7.0.x (developed/validated on Armbian
  `7.0.1-edge-rockchip64`). Built as an out-of-tree module (`rockchip-vdec.ko`).
- **Userspace (for the GStreamer path):** `gst-plugins-bad` **≥ 1.28**
  (`v4l2slvp9dec`). 1.28 is the first release that submits the HEVC EXT_SPS_RPS
  controls and has a working V4L2 stateless VP9 element; earlier versions are
  not usable here. ffmpeg's `v4l2_request` hwaccel also works for 8-bit.

This driver is built on the VDPU383 H.264/H.265 support merged to mainline Linux
in early 2026 (Collabora, 17-patch series; `rkvdec` de-staged). VP9 and AV1 for
VDPU383 are **not** upstream — VP9 is on the Collabora roadmap as a *future*
task. This is independent development on top of that mainline infrastructure.

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
# 148/305 strict-MD5 (see docs/CONFORMANCE.md for the breakdown). Reboot before
# a fresh baseline — decoder state bleeds across runs.
```

See [`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) for the full loop,
module parameters, and gotchas.

---

## What works (with evidence)

**8-bit Profile 0.** KEY/intra and well-coded INTER frames decode **byte-exact to
libvpx** for real-world content (≥128px), including compound/alt-ref frames with
sufficient explicit coding. Validated visually on HDMI: large-frame clips
(`earth_1080p`, a GoPro promo) display perfectly; and by Fluster strict-MD5 (the
core/quantizer/intra/superframe classes pass). Only inter frames with a small MC
footprint (sparse/skip-heavy or sub-128px) hit the open bug below.

**10-bit Profile 2 (4:2:0).** Decodes correctly and **no longer hangs**. Output
is the Rockchip packed 10-bit layout `V4L2_PIX_FMT_NV15` (= gst `NV12_10LE40`),
consistent with the HEVC/H.264 paths on this IP. Versus libvpx it is **85% of
samples byte-exact, the rest off by exactly +1 (max delta 4)** — reconstruction
rounding exposed at 10-bit precision, not a decode defect (8-bit rounds it away
and is byte-exact). HDR10+ sample content displays correctly on HDMI.

**Resolutions / throughput.** 720p, 1080p and 4K all decode. Pure hardware
decode time on clean (compound-free) content, performance governor:

| resolution | pure-HW per frame | pure-HW fps |
|---|---|---|
| 720p | 1.9 ms | ~517 |
| 1080p | 4.3 ms | ~231 |
| 4K (2160p) | ~16–19 ms | ~52–60 |

i.e. multiples of real-time at 720p/1080p, and real-time at 4K30. See
[`docs/THROUGHPUT.md`](docs/THROUGHPUT.md) (incl. the MPP/BSP native baseline and
the `flush-only-after-restore` optimisation).

---

## The open bug (below the register interface)

**This is the centrepiece.** Full triage in
[`docs/REF_BYPASS_BUG.md`](docs/REF_BYPASS_BUG.md); summary here. The deepest-
characterised manifestation is the **reference-bypass** on small-MC-footprint
frames (detailed below); the **sub-pel-precision drift** on complex/high-q frames
is the same wall on a different surface, and both were proven to sit below the MMIO
register interface (every input byte-identical to MPP, every operation class
replicated, the decode starts correct and diverges internally — see *Status* above
and `docs/REF_BYPASS_BUG.md` §7).

### Symptom (reference-bypass manifestation)

A VP9 **inter** frame with a small motion-compensation footprint reconstructs
using a **retained internal copy of the most-recently-used reference** instead of
the reference frame at the programmed DRAM reference-base addresses. "Small
footprint" arises two ways and both trigger it: a **sparse/skip-heavy** frame (a
few hundred bytes, any resolution) or a **sub-128px** frame. It is
content-modulated: an all-skip frame decodes correctly (it wants the most-recent
reference anyway), a small frame coding real motion decodes wrong (the retained
copy is stale), and a well-coded frame decodes correctly (it fetches DRAM
normally). It is **not** compound-specific — single-reference (`reference_mode=0`)
64×64 inter frames reproduce it.

On screen: real VOD content (full of small alt-ref/skip frames) corrupts visibly;
large-frame compound-free content is perfect.

### Repro

- **Single-ref (cleanest):** a 10-frame 64×64 `testsrc2` clip, `-auto-alt-ref 0`.
  Frame 0 (KEY) byte-exact; frames 1–9 (inter) 9–85% wrong. No compound.
- **Compound / real:** `KEY + hidden alt-ref (F1, show=0) + shown frame (F2,
  reference_mode=2)` in a 3-frame `.ivf`; F2 ≈ half wrong.

### What has been ruled out (exhaustively)

- **VP9-specific (decisive):** H.264 *and* HEVC tiny inter frames decode
  byte-exact on the same driver. HEVC uses the *same* GBL-header-buffer submission
  as VP9, so it is not the submission model — it is the VP9 MC path specifically.
- **Address-invariant (decisive):** the failing frame's output is bit-identical
  with the `last`/`golden`/`alt`/error-ref legs all redirected to a zero page, the
  output buffer pre-filled with a sentinel (which the HW overwrites — it *does*
  write a reconstruction), and CLR_CACHE0/1/2 issued each frame. No programmed
  reference is read. (The older "non-alt legs never read / depends only on alt"
  claim was the single-leg version of this and is superseded.)
- **MPP is address-sensitive on the same frame:** redirecting MPP's reference legs
  (a reverted one-line HAL change on the BSP board) *changes* its output — so the
  vendor path fetches the programmed reference and ours does not.
- **Our addresses are correct, not fallbacks** — `ref_lookup_fallback=0`; every
  reference resolves via the normal by-timestamp path (the same HEVC uses).
- **Every programmable input is byte-identical to MPP** — register descriptor, GBL
  header, the full 2432 B entropy-input probability buffer (0/2432), bitstream.
- **Completion is clean** (DEC_RDY, no error/reset/timeout); INT_EN line-IRQ
  arming, `frame-parallel=0`, `error-resilient=1`, firmware (BL31 v1.17), clocks,
  register write order, and RCB SRAM-vs-DRAM placement all change nothing.
  (Submission model is not the cause — decisively, HEVC and H.264 small inter
  frames are byte-exact under the *same* GBL-header submission path; see the
  VP9-specific bullet above. This driver's experimental link-mode submit does not
  run as a clean continuous node on the mainline stack, so it is not a usable
  comparison point either way.)
- **The two-reference averaging unit is sound:** HEVC B-frame bi-prediction (the
  VP9-compound analogue) is byte-exact on this stack — and HEVC small frames are
  byte-exact — so neither the averaging arithmetic nor small-frame handling in
  general is at fault. It is VP9 MC reference *fetch* specifically.

### The ask

Every programmable input is byte-identical to MPP, every clock / IOMMU / reset /
PM / warmup operation class MPP performs is replicated, the submission sequence
matches the working HEVC backend, and the decode *starts* correct — yet for
small-MC-footprint VP9 inter frames the hardware reconstructs from a retained
internal copy of the most-recent reference instead of the programmed DRAM address,
while MPP (same silicon, same frame) fetches it. **On VDPU383, what internal
per-frame state or init step does VP9 (and AV1) inter decode require that H.264 and
HEVC do not — that arms a real DRAM reference fetch (and, more generally, keeps the
internal decode on-track past the start of the frame) — given our inputs and
operation set match MPP yet ours diverges and MPP is bit-exact?** A pointer to
where the VP9/AV1 inter path differs from HEVC on this IP would be decisive.

---

## Known limitations

- **Small-dimension (<128px) single-ref INTER** — this is **the same bug** as the
  reference-bypass centrepiece above (small dimensions = small MC footprint;
  perturb-immune, proven 2026-06-13), not a separate defect. KEY frames are
  byte-perfect. Real content (≥128px, well-coded) is byte-exact; sub-128px is a
  Fluster `size`-vector stressor. See [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).
- **Mid-stream resolution change** — the driver correctly emits
  `V4L2_EVENT_SOURCE_CHANGE`; gst-plugins-bad's `copy_output_buffer` then drops
  frames on the stride transition. Downstream (gst), not a kernel decode bug.
- **Profile 3 (4:2:2 / 4:4:4) and 12-bit** — unsupported by design (only 4:2:0
  8/10-bit declared).
- **Throughput vs the vendor stack** — MPP/BSP is ~2.3–3× faster (it pipelines
  via link mode; this driver uses per-frame single-shot submit). Still multiples
  of real-time at 720p/1080p on clean content. Real VOD throughput is additionally
  gated by the reference-bypass bug (small frames stall/corrupt). See
  [`docs/THROUGHPUT.md`](docs/THROUGHPUT.md).

---

## Repository layout

```
src/        The out-of-tree rockchip-vdec.ko module (builds the full VDPU381/383
            H.264 / H.265 / AV1 / VP9 decoder). The VP9 work is
            rkvdec-vdpu383-vp9.c (+ -regs.h) and rkvdec-vp9.c; the rest is the
            shared/mainline infrastructure it builds on.
docs/       REF_BYPASS_BUG, THROUGHPUT, CONFORMANCE, BUILD_AND_TEST (+ fluster_baseline.csv).
test/       build+smoke script and a conformance smoke harness.
```

## Module parameters of note

- `vp9_skip_tlb_flush` — IOMMU TLB-flush policy: `0` flush-only-after-restore
  (default; the throughput fix), `1` never, `2` always (old per-frame behaviour).
- `vp9_time` — print mean pure-HW decode time every 100 frames (diagnostic).
- `rkvdec_link_mode` — link (batched) vs single-shot submit. Default single-shot.
- `vp9_perturb_refs` — diagnostic: redirect a small inter frame's non-alt
  reference legs to a scratch buffer; the output is unchanged, proving the
  references are not read (see `docs/REF_BYPASS_BUG.md`).

(Several `r3x`/`r4x` and `vp9_*` parameters are retained inert experimental
probes from the investigation; all default off.)

## Licence

GPL-2.0 (Linux kernel module). Derived from and built on the mainline Rockchip
`rkvdec` driver.

## Acknowledgements

Built on Collabora's mainline VDPU383 H.264/H.265 `rkvdec` work. MPP
(`rockchip-linux/mpp`) was the indispensable ground-truth reference for the
register-level triage.

## Publishing note

This directory is a self-contained snapshot intended to become a standalone
public repository (`git init` here and push). It is staged in a development
tree for snapshotting. Nothing here is auto-published.
