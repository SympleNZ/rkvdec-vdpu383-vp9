# The VP9 compound-prediction bug (VDPU383, mainline-V4L2 stack)

This is the one outstanding correctness bug and the reason this repo is published
downstream-first. It is a **hardware-execution-level** difference between the
mainline-kernel V4L2 stateless stack and the vendor MPP/BSP stack on the **same
VDPU383 silicon** — not a value any driver programs. We have triaged it
exhaustively at the register level; the remaining answer needs the hardware
documentation.

## 1. Symptom — precise

For VP9 frames coded with **`reference_mode = 2` (SELECT / compound)**, the
decode output is a **verbatim copy of the alt-ref reference frame**. Equivalent
to decoding every block as `skip, single-ref = ALT, MV = 0, residual = 0`:

- The **compound average** (½·last + ½·alt) is not applied.
- The **coefficient residual** is not applied.
- Output is purely the alt reference — not `last`, not `average(last, alt)`.

It is **payload-dependent**, not uniformly "all compound frames wrong":

- Small-payload, prediction-heavy compound frames (mostly skip blocks) →
  collapse wholesale to the alt-ref copy.
- Large-payload compound frames (lots of explicit residual coding) → decode
  **correctly**.

So the compound *average + residual* path is silently inert for blocks that rely
on prediction; frames with enough explicit data mask it.

### On-screen / real-content manifestation

Real VP9 (YouTube-style, Big Buck Bunny, etc.) is compound-heavy and corrupts
visibly. Measured on three real clips (per-frame `reference_mode` trace):

| clip | compound (rm=2) frames | single-ref frames | HDMI result |
|---|---|---|---|
| earth_1080p_horizontal | 0 | 192 | perfect |
| GoPro promo (why-hero) | 0 | 712 | perfect |
| Big Buck Bunny Surround | **556** | 145 | gross corruption |

Every clip that is compound-free is perfect; the only one that corrupts is 79%
compound. This is the bug, end to end.

## 2. Reproducer

A minimal 3-frame `.ivf`: `KEY + hidden alt-ref (F1, show_frame=0) + shown
compound frame (F2, reference_mode=2)`.

- libvpx / MPP: `[KEY, correct-compound-F2]` (`F2 md5 cfe88b3a13`).
- This driver: `[KEY, copy-of-F1]` — **F2 is byte-identical to the alt-ref F1**
  (`md5 a5eb2e9b60`).

(For a YouTube superframe the alt-ref is hidden inside the superframe; split it
into its own packet to observe F1's pixels. The bug is intrinsic to F2's compound
decode, not superframe packaging — a standalone 3-frame stream reproduces it.)

## 3. What has been ruled out

Every item below was an explicit experiment. The board is the same VDPU383
silicon throughout; MPP is the ground-truth reference (it decodes the same
streams correctly on the BSP stack).

### Every programmable input is byte-identical to MPP

For the failing compound frame, captured against MPP `DUMP_VDPU383_DATAS` dumps:

- **Full register descriptor** — ctrl_regs (8–30), comm_paras (64–106: all
  decode-control params incl. ref strides 83–91, dims, scaling, tile,
  strm_start_bit), and the address registers (differ only in IOVA encoding,
  semantically correct). 0 diffs.
- **GBL header** (352 B) — byte-identical.
- **Probability tables** — `prob_default` and the adapted `prob_loop[ctx]` the
  HW reads for the compound frame: byte-identical (0/4864 differ).
- **Both reference buffers** — `last`(KEY) and `alt`(F1) proven correct
  independently (F1 decodes byte-identical to libvpx as a standalone frame).
- No register exists in MPP's descriptor that this driver does not also set;
  `reg_version`, `statistic_regs`, and the reg162–167 gap are all zero in both.

### Cross-frame / stream-level state is replicated

MPP's parser + HAL cross-frame records (`ls_info`, `use_prev_frame_mvs`,
`frame_context_idx`, `prob_ctx_valid`, `pre_mv_base_addr`) were enumerated and
checked. `use_prev_frame_mvs` is computed identically and the cumulative effect
is proven by the **byte-identical GBL** (which encodes it). No missed function
call, no un-replicated record.

### Firmware — ruled out

Ran this V4L2 driver on the BSP's **BL31 v1.17** (vs Armbian's v1.20) by
repacking u-boot. Compound still collapses. MPP works on the same BL31 v1.17.
So the difference is the software stack above BL31, not the firmware.

### Decoder cache — ruled out

MPP's live MMIO trace shows it programs the cache regs (CACHE0/1/2) only on the
first task of a session. Tested this driver with per-frame cache clear (one
cache, all three) and skip-all: no change in any configuration.

### Clocks — ruled out

`clk_summary` identical on both stacks (rkvdec_core 594 MHz, hclk 198 MHz,
hevc_cabac 1 GHz, aclk_root 594/500 MHz).

### Register write order — ruled out

Forced the exact upstream HEVC burst order (COMMON, COMMON_ADDR, CODEC_PARAMS,
CODEC_ADDR). No change. The HW latches on the kick (`CMD_SEND` for MPP); there is
no per-register ordering MPP does that we miss. MPP's only "extra" step is the
RCB SRAM info ioctl, and RCB is fully understood and ruled out (our allocation is
≥ MPP's, in SRAM, and single-ref INTER decodes byte-perfect with it).

### Completion is clean — not a silent/error path

The failing compound frame finishes with a **clean DEC_RDY IRQ** (`sta=0x01`),
no error bits, no soft-reset, and all references resolve (`ref_lookup_fallback=0`).
The HW reports full success, yet emits the alt-ref copy. Link mode (continuous)
also fails identically.

### The two-reference averaging hardware path is sound (key triangulation)

HEVC **B-frame bi-prediction** — the H.265 analogue of VP9 compound (average two
references) — is **byte-exact** on this exact V4L2 stack (32-frame x265 stream,
22 B-frames, 0 wrong). So:

- The hardware's two-reference *averaging unit* works.
- HEVC decode (the validated production path) is fully correct on this
  kernel/IOMMU/m2m infrastructure.
- Therefore the bug is **VP9-compound-specific**: compound frames are being
  decoded as **single-ref (alt only)** — the per-block compound mode/reference
  *selection* collapses, not the prediction arithmetic.

### Decisive proof — the non-alt references are never read

Gated experiment (`vp9_perturb_refs`): for the shown compound frame only,
redirect the **`last` + `golden`** reference legs (header reg170/171 + payload
reg195/196) to a 3 MB scratch buffer filled with `0x00`, leaving **`alt`**
(reg172/197) intact. Decode the repro with the param off, then on, and diff.

**Result: 0 / 1382400 bytes differ.** Zeroing both non-alt legs has **zero
effect** on the output. The `last` and `golden` references are **never read**
during the compound frame's decode — it depends only on `alt`.

This confirms the per-block reference modes are all single-ref-from-alt (H1) and
rules out a reference-pair/address/setup bug on our side (that would have
darkened the output). The reference *setup* is correct; the references are
correctly unused *because the decoded modes are single-ref-alt*. There is no
address we can fix.


### RCB placement (SRAM vs DRAM) — ruled out

The Rockchip BSP kernel rewrites the RCB base registers in-kernel at submit
(`mpp_set_rcbbuf`) and, with no `rcb-iova` in the BSP DT, decodes with **DRAM** RCB;
the mainline V4L2 driver always uses **SRAM** RCB — a divergence invisible to the HAL
register dump. Forcing all-DRAM RCB and re-decoding a compound-heavy clip (`sintel`,
1280×546, 789 frames) gives **487/789 frames wrong under both SRAM and DRAM — a
byte-identical outcome.** RCB placement does not affect the compound collapse. (It *does*
move AV1's separate intra-above-row bug, so it was worth excluding here.)

### Power state, warm-up, and session continuity — ruled out (2026-06-08)

The natural reading of "MPP keeps the session continuous, our single-shot tears
it down per frame" was tested directly and refuted **three ways** on the
byte-level repro:

- **Runtime-PM power domain forced on** (`/sys/bus/platform/devices/<dev>/power/control
  = on`) so the decoder never suspends between frames — no change.
- **Per-resume HW warm-up disabled** — no change.
- **Truly continuous link runtime** (`rkvdec_link_mode=1` + `rkvdec_submit_mode=2`
  + `rkvdec_link_depth=2`): the two repro frames batched into a **single** link
  submission so the HW is never idled or re-programmed between them — still the
  identical alt-ref copy (`a5eb2e9b60…` vs the correct `cfe88b3a13…`).

MPP does run the whole stream as one continuous link session (cache/link
programmed once on the first task, HW kept live frame-to-frame), versus our
per-frame `memset`+reprogram. But the three tests above show that difference is
**non-causal**: the compound collapse is byte-identical whether the HW is
power-cycled and fully re-initialised per frame, or kept continuously live across
the GOP. So it is **not** power state, **not** the warm-up, and **not**
session/runtime continuity.

## 3b. Update 2026-06-08 — it is decode-side: HW decodes single-reference, never compound

Dumping the HW-adapted probability buffer (`probs[frame_context_idx]`, the post-decode
backward-adaptation writeback) for the failing compound frame pins the mechanism more
precisely than "collapses to an alt copy":

- After the compound frame decodes, the adapted **`comp_mode` and `comp_ref` probs are
  unchanged** from the context input, while **`single_ref` *does* adapt**. Because the
  adaptation path is demonstrably alive (single_ref moved), `comp_mode`/`comp_ref` staying
  flat means the HW counted **zero compound-mode decisions** — i.e. it decoded **every block
  as single-reference**. So this is **not** "decoded compound but mis-executed the average"
  (that would have moved `comp_mode`); the HW **never decodes compound at all** — it
  mis-resolves the per-block compound/single choice as always-single under
  `reference_mode = SELECT`. (Coherent with the alt-only reference reads in §3 and the clean
  alt-*copy* output: a copy is what single-ref/skip produces, not garbled compound execution.)

- **The `comp_mode` decision prob we feed HW is byte-identical to MPP.** Cross-dumped MPP's
  input prob buffer for the same frame on the BSP (`cabac_update_probe.dat`, text-hex,
  byte-reversed per 16-byte line): `comp_mode` and `comp_ref` match ours exactly. The
  *non-degenerate* default comp_mode probs are in place (the post-KEY context is reset to
  spec defaults, not left zero — a separate desync that is already fixed), so HW *should*
  sometimes pick compound; it never does.

- **HW forward-updates correctly.** MPP pre-applies the compressed-header forward prob
  updates and feeds the result; we feed the saved context and the VDPU383 applies the forward
  updates itself (our post-decode adapted buffer equals MPP's forward-updated values, and
  single-ref content like BBB is byte-perfect). So the forward-update division of labour is
  not the divergence either.

Net: identical `comp_mode` input, correct forward-update handling, correct (non-zero) default
probs — **yet the HW resolves every block to single-reference**. The per-block compound/single
decision diverges **below the MMIO interface**, established far deeper than "the registers
match": the specific decision probability is byte-identical and the entropy/forward-update
path is correct.

## 4. Where it must be / the ask

`reference_mode` is **never programmed to a register or the GBL by either
driver** — the VDPU383 derives it purely from its own compressed-header bitstream
decode (as the VP9 spec mandates). So there is no "enable compound" register we
are failing to set.

Given **byte-identical compressed-header bytes + probability tables (incl.
`comp_mode` / `comp_ref`) that MPP decodes as compound**, the VDPU383 under the
mainline-V4L2 driver silently downgrades the SELECT-mode frame's per-block
reference modes to single-ref-from-alt. Same silicon, same input bytes, different
decode. The only remaining variable is the **hardware's internal
entropy-decoder / cross-frame context state at the moment it parses the compound
frame** — below the MMIO register interface.

**The question for the hardware owners:** what does MPP's device/session
initialisation do — once, outside the per-frame register programming — that
makes the VDPU383 engage VP9 compound (SELECT) prediction, which a mainline V4L2
m2m client does not? Note the obvious candidate is already excluded (§3): it is
**not** session/runtime *continuity* — power-cycling and fully re-initialising the
HW every frame vs keeping it continuously live across the GOP gives byte-identical
wrong output. So the missing piece is some other per-session device-init state the
HW carries into its compressed-header entropy decode, not the fact of a persistent
session.

## 5. Reproduce it yourself

1. Build + load the module (see `BUILD_AND_TEST.md`).
2. Decode any compound-heavy clip (e.g. Big Buck Bunny "Surround") to HDMI and
   observe corruption; decode a compound-free clip and observe correctness.
3. For the byte-level repro, build a 3-frame `KEY + hidden-alt-ref + compound`
   `.ivf` and compare the second output frame to (a) the alt-ref decoded
   standalone and (b) the libvpx reference. Ours matches (a); libvpx is (b).
4. `vp9_perturb_refs=1` then decode the repro: output is unchanged → non-alt legs
   unread.
