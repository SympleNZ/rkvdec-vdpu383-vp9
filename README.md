# rkvdec-vdpu383-vp9 — V4L2 stateless VP9 decoder for Rockchip RK3576 (VDPU383)

A working **V4L2 stateless VP9 decoder** for the Rockchip **RK3576** SoC's
**VDPU383** video IP — the first of its kind. It decodes real-world 8-bit and
10-bit VP9 (Profile 0 and Profile 2) correctly and well above real-time, built
on top of the now-mainline VDPU383 H.264/H.265 `rkvdec` infrastructure.

**One outstanding bug** blocks VP9 *compound (alt-ref) prediction* — and that's
a hardware-execution-level question we cannot resolve from the driver side. This
repo is published downstream-first: working code plus a precise, fully-triaged
question for the people with the hardware documentation (Collabora / the
VDPU383 maintainers). See [The compound-prediction bug](#the-compound-prediction-bug-the-open-question).

> Status as of 2026-06-10. Independent development on the RK3576 VDPU383.

---

## ★ Update 2026-06-10 — the compound bug is proven HW-internal (below everything software-reachable)

A deep follow-up session **definitively localized** the compound bug and closed the
register-level investigation:

- **On-hardware proof it is NOT the register file.** Reading back the full HW register state
  per-decode and correlating with the outcome shows the control/parameter registers are
  **byte-identical run-to-run regardless of whether the frame decodes correctly or not.** Our
  programming is provably deterministic and identical; the divergence is below it.
- **Software, not silicon (confirmed byte-exact).** The vendor MPP stack decodes the same
  compound clip **byte-identical to libvpx** on the same SoC (luma md5 match), while our V4L2
  output diverges on exactly that frame. So the VDPU383 *can* decode it correctly under the
  right software — the fix is reachable in principle, just not from the register/buffer surface.
- **All three software layers walked, all negative.** (1) HAL / register file — byte-identical;
  (2) the per-codec kernel driver — ~20 distinct levers (cache attributes/config, TLB flush,
  register-bit/mode permutations, warmup, kick barriers, write order, link mode, …); (3) the
  core MPP **service** layer (`mpp_service`/`mpp_iommu`) — IOMMU hardware refresh
  (`rockchip_iommu_disable+enable` equivalent) and the cached-dma-buf buffer model, both negative.
  Whatever the vendor stack does to arm the compound decision leaves **no trace** in any register,
  buffer, IOMMU operation, or memory attribute we can observe or control from a V4L2 driver.

**Net:** the open question below is now backed by an exhaustive, layer-by-layer negative result,
not just a register diff — it needs the VDPU383 hardware documentation (the internal entropy /
cross-frame state machine that arms compound prediction). See `docs/COMPOUND_BUG.md`.

---

## TL;DR

| Capability | State |
|---|---|
| 8-bit Profile 0 — intra / KEY | ✅ byte-exact to libvpx |
| 8-bit Profile 0 — single-ref INTER (≥128px) | ✅ byte-exact to libvpx; visually perfect on HDMI |
| 10-bit Profile 2 (4:2:0) | ✅ correct (±1 reconstruction rounding); HDR10+ displays correctly |
| Resolutions 720p / 1080p / 4K | ✅ all decode; well above real-time on clean content |
| **VP9 compound / SELECT (alt-ref) prediction** | ❌ **the open bug** — HW decodes single-ref only; compound never engages |
| Small-dimension (<128px) single-ref INTER | ⚠️ conformance-only defect, zero real-world impact |
| Mid-stream resize | ⚠️ driver signals it; gst-plugins-bad drops frames (downstream) |
| Profile 3 (4:2:2 / 4:4:4), 12-bit | ❌ unsupported by design |

Because most real streaming content (YouTube, etc.) is **compound-heavy**, the
compound bug is the single gate between this driver and general-purpose VP9
hardware decode. Everything *else* works.

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

**8-bit Profile 0.** KEY/intra and single-ref INTER decode **byte-exact to
libvpx** for real-world content (≥128px). Validated visually on HDMI: real
clips with no compound frames (`earth_1080p`, a GoPro promo) display perfectly;
and by Fluster strict-MD5 (the core/quantizer/intra/superframe classes pass).

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

## The compound-prediction bug (the open question)

**This is the centrepiece.** Full triage in
[`docs/COMPOUND_BUG.md`](docs/COMPOUND_BUG.md); summary here.

> **Latest (2026-06-09, [`COMPOUND_BUG.md` §6](docs/COMPOUND_BUG.md)):** three results that
> tighten — but do not change — the conclusion. (1) Input equivalence is now *fully* closed: the
> complete 2432-byte entropy-input prob buffer (incl. the coef table), not just the mode probs,
> is byte-identical to MPP (0 diffs). (2) A deterministic IOMMU access trace shows the HW *does*
> issue pixel-fetch reads to the `last` reference (never `golden`) — so the candidate references
> are *fetched*; this **corrects** the earlier "non-alt references never read" inference (that was
> read off a non-deterministic output). It does **not** mean compound works: `comp_mode` still
> never adapts, so the HW decodes **zero compound blocks** — the divergence is the per-block
> compound/single *decision*, upstream of the (proven-sound) averaging unit. (3) A
> reserved-register-bit sweep finds **no** compound-enable bit — `reference_mode` is
> bitstream-derived, not register-gated.

### Symptom

For VP9 frames coded with **`reference_mode = 2` (SELECT / compound)**, the
VDPU383 under this V4L2 driver produces a **verbatim copy of the alt-ref
reference frame** — as if every block were decoded as `skip, single-ref = ALT,
MV = 0, residual = 0`. The compound average (last + alt) and the coefficient
residual are both effectively not applied. Large-payload compound frames (lots
of explicit coding) escape it; small-payload, prediction-heavy compound frames
collapse wholesale.

On screen: a real clip that is mostly compound (e.g. Big Buck Bunny "Surround",
556/701 compound frames) shows gross corruption, while compound-free clips are
perfect.

### Repro

`KEY + hidden alt-ref (F1, show=0) + shown compound frame (F2)` in a 3-frame
`.ivf`. Our hardware emits `[KEY, copy-of-F1]`; libvpx/MPP emit `[KEY,
correct-compound-F2]`. F2 byte-identical to the alt-ref F1 (`md5 a5eb2e9b60`)
instead of the correct frame (`cfe88b3a13`).

### What has been ruled out (exhaustively)

- **Every programmable input is byte-identical to MPP** for the failing frame:
  the full register descriptor (ctrl/comm_paras/addresses), the GBL header, the
  probability tables (default + adapted `prob_loop`), and both reference
  buffers (proven correct independently). Verified against MPP `DUMP_VDPU383_DATAS`
  dumps on the BSP stack.
- **Firmware:** ran our V4L2 driver on BSP **BL31 v1.17** (vs Armbian v1.20) —
  no change. Not firmware.
- **Decoder cache:** clearing all three caches (CACHE0/1/2) per frame, or none —
  no change.
- **Clocks:** decoder clock tree identical to the working BSP stack.
- **Register write order:** matching the upstream HEVC burst order — no change.
- **IOMMU restore / reset / kick sequence:** the failing frame completes with a
  **clean IRQ** (DEC_RDY), no error, no reset, references resolve correctly.
- **The two-reference averaging hardware path is sound:** HEVC B-frame
  bi-prediction (the H.265 analogue of VP9 compound) is **byte-exact** on this
  exact V4L2 stack. So the averaging unit is not broken — VP9 compound is failing
  to *engage*, not failing to average.
- **Reference-perturbation (refined 2026-06-09):** zeroing the `last` + `golden`
  reference *content* (leaving `alt`) changes the output **by zero bytes** — the
  non-alt references have no effect on the result. *Correction:* this was first
  read as "non-alt references never read", but a deterministic IOMMU access trace
  (below) shows the HW *does* issue pixel-fetch reads to `last` — the references
  are **fetched but have no effect on output**, not skipped. The "unchanged"
  result was measured on the non-deterministic displayed output; the IOMMU trace
  is the reliable signal. Either way compound never *engages*: every block decodes
  single-ref (see the adapted-prob bullet).
- **Continuous-session / power / warm-state HW context (2026-06-08, decisive
  negative):** the most natural remaining hypothesis — that compound needs
  cross-frame HW state which MPP's *continuous* link session preserves and our
  per-frame single-shot submit tears down — is **refuted three ways on the same
  repro**: (1) forcing the decoder's runtime-PM power domain to stay on (no
  per-frame suspend, so no power-cycle between frames), (2) disabling the
  per-resume HW warm-up, and (3) running the repro through a *truly continuous*
  link runtime (the two repro frames batched into **one** link submission so the
  HW is never idled between them) — **all three produce the identical alt-ref
  copy.** For reference, MPP *does* run the whole stream as one continuous link
  session (cache/link configured once on the first task, HW kept live
  frame-to-frame) versus our per-frame `memset`+reprogram — but that device-flow
  difference is **non-causal** for this bug. So it is not power state, not the
  warm-up, and not session/runtime continuity: same input, same completion, same
  wrong output whether the HW is reset between frames or kept continuously live.
- **Decode-side, not execution — and the decision prob matches MPP (2026-06-08):** the
  failing frame's HW-adapted `comp_mode`/`comp_ref` probs come back **unchanged** (zero
  compound decisions counted) while `single_ref` adapts — so the HW decodes **every block as
  single-reference**, it never decodes compound at all (not "decoded compound but
  mis-averaged"). And our **`comp_mode` input prob is byte-identical to MPP** (cross-dumped),
  with HW forward-updates applied correctly and the post-KEY context at correct non-zero
  defaults. Same decision prob as MPP, yet single-ref decode → the per-block compound/single
  choice diverges below MMIO. See [`docs/COMPOUND_BUG.md` §3b](docs/COMPOUND_BUG.md).
- **Full entropy-input equivalence (2026-06-09):** beyond the `comp_mode`/`comp_ref` probs, the
  **complete 2432-byte entropy-input prob buffer** (incl. the coef table, uv_mode, mv probs) is
  **byte-identical to MPP's `cabac_last`** for the failing frame — 0 of 2432 bytes differ. The
  last input that had not been fully byte-compared now matches exactly.
- **IOMMU access trace (2026-06-09):** redirecting each reference leg to a distinct unmapped
  IOVA and reading the fault address shows the HW issues **pixel-fetch reads to `last` and
  `alt`** (never `golden`) for the SELECT frame — so the candidate references are set up at the
  bus level. This is *not* evidence compound engages (it doesn't — `comp_mode` never adapts);
  it corrects the perturbation bullet's "never read" and rules out reference *fetch/selection*
  as the failure site.
- **No compound-enable register (2026-06-09):** OR-sweeping the reserved bitfields of every
  VDPU383 common control register on the failing frame changes the decode in **no** case — most
  reserved bits are unimplemented, the writable few have no effect. There is no settable
  "engage compound" bit (consistent with `reference_mode` being bitstream-derived).

### The ask

Given byte-identical compressed-header bytes and probability tables that MPP
decodes as compound, the VDPU383 under the mainline-V4L2 stack silently
downgrades a SELECT-mode frame's per-block reference modes to single-ref-from-alt.
`reference_mode` is never programmed to a register by **either** driver — the HW
derives it from its own compressed-header bitstream decode. So there is no
"enable compound" register we are missing. The remaining variable is the HW's
internal entropy-decoder / cross-frame context state at the moment it parses the
compound frame — below the register interface. **What does MPP's
device/session setup do that a mainline V4L2 m2m client does not, that makes the
VDPU383 engage compound prediction?**

---

## Known limitations

- **Small-dimension (<128px) single-ref INTER** — KEY frames are byte-perfect
  but inter frames corrupt on sub-128px frames (a single 64×64 superblock or
  smaller). **Zero real-world impact** (real content ≥128px is byte-exact); it
  only affects Fluster `size` conformance vectors. Both cheap hypotheses (stride,
  height-alignment) are disproven — see
  [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md). Same class as the compound bug
  (a tiny-frame HW behaviour not in the visible registers).
- **Mid-stream resolution change** — the driver correctly emits
  `V4L2_EVENT_SOURCE_CHANGE`; gst-plugins-bad's `copy_output_buffer` then drops
  frames on the stride transition. Downstream (gst), not a kernel decode bug.
- **Profile 3 (4:2:2 / 4:4:4) and 12-bit** — unsupported by design (only 4:2:0
  8/10-bit declared).
- **Throughput vs the vendor stack** — MPP/BSP is ~2.3–3× faster; the residual
  gap is the **per-frame HW decode time** (memory-bandwidth-bound), not
  inter-frame submission — RK3576 is single-core, so pipelining can't overlap
  decode (reg13 / cache / clocks / FBC ruled out; flush-only-after-restore is the
  landed win). Still multiples of real-time at 720p/1080p. See
  [`docs/THROUGHPUT.md`](docs/THROUGHPUT.md).

---

## Repository layout

```
src/        The out-of-tree rockchip-vdec.ko module (builds the full VDPU381/383
            H.264 / H.265 / AV1 / VP9 decoder). The VP9 work is
            rkvdec-vdpu383-vp9.c (+ -regs.h) and rkvdec-vp9.c; the rest is the
            shared/mainline infrastructure it builds on.
docs/       COMPOUND_BUG, THROUGHPUT, CONFORMANCE, BUILD_AND_TEST (+ fluster_baseline.csv).
test/       build+smoke script and a conformance smoke harness.
```

## Module parameters of note

- `vp9_skip_tlb_flush` — IOMMU TLB-flush policy: `0` flush-only-after-restore
  (default; the throughput fix), `1` never, `2` always (old per-frame behaviour).
- `vp9_time` — print mean pure-HW decode time every 100 frames (diagnostic).
- `rkvdec_link_mode` — link (batched) vs single-shot submit. Default single-shot.

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
