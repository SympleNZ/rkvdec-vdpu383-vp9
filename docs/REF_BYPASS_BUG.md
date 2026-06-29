> **NOTE (2026-06-28): the "reference-bypass" framing in this document is SUPERSEDED.**
> The complex-content failure was definitively isolated to VP9 **compound prediction** and proven
> to be an internal compound-MC issue below every V4L2-touchable interface (driver-addressable in
> principle, not silicon-broken, but not fixable through mainline V4L2). See the README "VP9 compound
> prediction" section for the final, authoritative conclusion. This file is retained as investigation history.

# The VP9 small-footprint reference-bypass bug (VDPU383, mainline-V4L2 stack)

This is the one outstanding correctness bug and the reason this repo is published
downstream-first. It is a **hardware-execution-level** difference between the
mainline-kernel V4L2 stateless stack and the vendor MPP/BSP stack on the **same
VDPU383 silicon** — not a value any driver programs.

> **History / correction (2026-06-13).** Earlier revisions of this repo framed
> this as a *compound-prediction* bug ("SELECT-mode frames collapse to an alt-ref
> copy; compound never engages"). **That framing was wrong** and is retracted
> here. Two later findings overturned it: (1) freshly-encoded compound/alt-ref
> clips decode **byte-exact** on this driver, so compound prediction works; the
> "adapted comp_mode never moves" signal that motivated the old framing came from
> a `frame_parallel_decoding_mode=1` stream, where VP9 backward adaptation is
> disabled *by spec* — the prob write-back cannot move regardless, and ours is in
> fact byte-identical to MPP's forward-updated context. (2) The bug reproduces on
> **single-reference** (`reference_mode=0`) frames too. The real common factor is
> the frame's **motion-compensation footprint**, not compound mode. The older
> "compound" and "small-dimension single-ref inter" bugs this repo described
> separately are **one bug**, characterised below.

## 1. Symptom — precise

A VP9 **inter** frame whose actual motion-compensation reference footprint is
*small* reconstructs with reference pixels served from a **retained internal copy
of the most-recently-used reference**, rather than from the reference frame at the
programmed reference-base addresses in DRAM. "Small footprint" arises two ways,
and both trigger it:

- **Small coded size** — a sparse / skip-heavy inter frame (a few hundred bytes)
  at any resolution.
- **Small dimensions** — a sub-128px frame (a single 64×64 superblock or smaller),
  even when not sparse.

The corruption is **content-modulated, not a clean threshold**:

- An **all-skip** frame (e.g. 31 bytes, zero motion) decodes **correctly** — it
  wants the most-recent reference anyway, so the retained copy happens to be right.
- A small frame that codes **real motion** decodes **wrong** — the retained copy
  is stale relative to the reference it should have fetched.
- A frame with enough explicit coding (larger footprint) decodes **correctly** —
  it fetches the programmed reference from DRAM normally.

So the output looks "reference-derived but stale": plausible image content drawn
from the wrong (most-recent) reference. On a compound frame with real motion the
result is roughly half the frame wrong; on a clean single-ref 64×64 inter frame,
~9–85% of pixels wrong depending on coded density.

### On-screen / real-content manifestation

Real VOD VP9 (YouTube, Big Buck Bunny "Surround", etc.) is dominated by small,
prediction-heavy inter frames (alt-ref overlays, skip-heavy P-frames), so it
corrupts visibly. Compound-free, large-frame content (`earth_1080p`, a GoPro
promo) is perfect end to end. The single discriminator is the per-frame MC
footprint, which correlates with — but is not caused by — compound/alt-ref usage.

## 2. Reproducers

Two minimal repros, both in this repo's evidence set:

- **Single-ref (cleanest):** a 10-frame 64×64 `testsrc2` clip, `-auto-alt-ref 0`
  (`reference_mode=0`, no compound). Frame 0 (KEY) is byte-exact; frames 1–9
  (inter) are 9–85% wrong. No compound, no alt-ref, no superframe — the simplest
  form of the bug.
- **Compound / real-world:** `KEY + hidden alt-ref (F1, show=0) + shown frame
  (F2, reference_mode=2)` in a 3-frame `.ivf` (a single YouTube superframe split
  into separate packets). F2 is ~half wrong. (The superframe packaging is not the
  cause — splitting it into separate frames reproduces identically.)

## 3. The decisive evidence — it is VP9-specific and address-invariant

### Cross-codec: only VP9 is affected

Static-scene clips with tiny inter frames, decoded HW-vs-software on the same
driver and board:

| Codec | Submission model | Tiny (<512 B) inter frames |
|---|---|---|
| H.264 | param registers | **byte-exact** (21 tiny frames, 0 wrong) |
| HEVC | **GBL header buffer** (same as VP9) | **byte-exact** (14 tiny frames, 0 wrong) |
| VP9 | GBL header buffer | **fails** (this bug) |

HEVC uses the *same* global-header-buffer submission model as VP9 and is
unaffected — so this is **not** the submission path, the header-buffer approach,
or a generic small-frame HW behaviour. It is specific to the VP9 reconstruction /
MC path. **HEVC's working tiny-frame path is the control to diff VP9 against.**

### The MC reference is not fetched from the programmed addresses

The shipped `vp9_perturb_refs` gated diagnostic redirects the failing frame's
**non-alt** reference legs (`last` + `golden`, reg170/171 + payload reg195/196)
to a scratch/zero page. On the failing frames the output is **bit-identical**
with it on — those references are not read. A set of further gated redirect
experiments (kept out of this snapshot to keep it lean) extended the same probe
to the remaining sources and found the reconstruction equally invariant to:

- the `alt` leg (reg172/197),
- the error-concealment reference (reg169/194),
- the output buffer pre-filled with a `0x55` sentinel (which the HW then fully
  overwrites with the wrong-but-real content — so the HW *is* writing a
  reconstruction, not skipping the frame),
- `CLR_CACHE0/1/2` issued before every frame.

i.e. the reconstruction's reference content is **invariant to every programmable
reference address and to the documented decoder cache.** The retained copy was
made when that reference was first used; redirecting the address *now* is too
late. (The older revision's "depends only on alt / non-alt refs never read" claim
was the partial, non-alt-only version of this and is superseded — *no* programmed
reference is read for these frames.)

> Confirmed on **both** repro classes: redirecting refs leaves the compound F2 *and*
> the single-ref 64×64 inter frames byte-identical. Same bug.

### The retained reference is seeded by the immediately-preceding decode

If the failing frame's reference comes from internal retention rather than DRAM,
*what* is retained? It is the previous decode's reconstruction. Decoding the
`KEY + hidden-alt-ref(F1) + shown(F2)` repro with F1's decode **skipped** (the
driver's `r50_skip_altref`) changes F2's output (`6b3c7a8a` → `13673718`). Since
F2 ignores its programmed reference addresses (above), the only channel by which
skipping the preceding frame can move F2 is the **internal retained reference** —
so F2's motion comp is served from whatever the immediately-preceding decode left
in the hardware (F1 normally; the KEY when F1 is skipped). The retained copy is
real reconstructed content from the prior frame, not a re-fetch — which is why
redirecting F2's addresses afterwards has no effect.

### MPP decodes it correctly and IS address-sensitive (same silicon)

On the BSP board, MPP decodes the same frame byte-exact to libvpx. A one-line
change in MPP's `hal_vp9d_vdpu383.c` redirecting the same reference legs (reverted
afterwards) **changes MPP's output**. So on identical silicon the vendor path
fetches references from the programmed addresses for this frame and ours does not
— the bypass is an abnormal mode our submission arms, not normal HW behaviour.

## 4. What else has been ruled out

The board is the same VDPU383 silicon throughout; MPP is the ground-truth
reference. All explicit experiments:

- **Every programmable input is byte-identical to MPP** for the failing frame:
  full register descriptor (ctrl/comm_paras/addresses), the 352 B GBL header, the
  2432 B entropy-input probability buffer (0/2432 differ), the bitstream. Verified
  against MPP `DUMP_VDPU383_DATAS` dumps.
- **Our reference addresses are correct, not fallbacks** — `ref_lookup_fallback=0`
  across every lookup on the failing clip; each reg170-172 value resolves via the
  normal by-timestamp `vb2_find_buffer` path, the same path HEVC uses.
- **Buffer address / size alignment** — output buffers are 4 MB-aligned, and the
  cleanest repro (the 64×64 single-ref clip) has a hor-stride **identical to MPP's**
  (192), 8-aligned dimensions, and aligned buffers — and it still bypasses. So
  alignment is not the trigger. (Our hor-align does diverge from MPP at large
  widths — 1472 vs 1280 for a 1280-wide frame — but the small repro that fails has
  the same stride as MPP, so that divergence is a separate matter, not this bug.)
- **Completion is clean** — the failing frame finishes with a clean DEC_RDY IRQ
  (`sta=0x01`), no error bits, no soft-reset, no timeout.
- **Decoder cache** — clearing CACHE0/1/2 per frame, alone or combined with the
  reference redirect, changes nothing.
- **Submission model** — single-shot and link/batched submit both fail identically.
- **IRQ arming** — matching the BSP per-task `irq_mask` (INT_EN bits 0+1) changes
  nothing.
- **Encoder flags** — `frame-parallel=0` and `error-resilient=1` both still fail.
- **Firmware** — ran this driver on BSP BL31 v1.17 (vs Armbian v1.20); no change.
  MPP works on both.
- **Clocks** — `clk_summary` identical to the working BSP stack.
- **Register write order** — forced the upstream HEVC burst order; no change.
- **RCB placement (SRAM vs DRAM)** — forcing all-DRAM RCB gives a byte-identical
  failing outcome (`sintel`, 789 frames). Placement does not affect this bug.
- **The two-reference averaging unit is sound** — HEVC B-frame bi-prediction (the
  H.265 analogue of VP9 compound) is byte-exact on this exact stack, and HEVC's
  small frames are byte-exact, so neither the averaging arithmetic nor small-frame
  handling in general is the issue. It is VP9 MC reference *fetch* specifically.

## 5. Where it must be / the ask

`reference_mode` is never programmed to a register or the GBL by either driver —
the VDPU383 derives it from its own compressed-header decode, so there is no
"enable compound" register, and (as section 1 shows) compound mode is not the
variable anyway. The reference addresses we program are correct and identical in
construction to the working HEVC path. Yet for small-MC-footprint VP9 inter frames
the HW reconstructs from a retained internal copy of the most-recent reference
instead of those addresses, while MPP fetches them.

**The question for the hardware owners:** on VDPU383, what in the **VP9**
reconstruction / motion-comp path arms a real DRAM reference fetch, and why is it
disarmed for small-footprint frames — given H.264 and HEVC small frames are
unaffected, our reference addresses match the working HEVC path, and MPP (on the
same silicon) fetches the programmed reference for the same frame? A pointer to
where VP9 MC reference fetch differs from HEVC on this IP would be decisive.

## 6. Reproduce it yourself

1. Build + load the module (see `BUILD_AND_TEST.md`).
2. **Single-ref repro:** `ffmpeg -f lavfi -i testsrc2=size=64x64:rate=30
   -frames:v 10 -c:v libvpx-vp9 -auto-alt-ref 0 -lag-in-frames 0 -b:v 200k
   -cpu-used 4 c64.ivf`; decode `parsebin ! v4l2slvp9dec` vs `ffmpeg -pix_fmt
   yuv420p`. Frame 0 matches; frames 1–9 differ.
3. **Perturb-immunity:** `vp9_perturb_refs=1`, decode the same clip — output is
   byte-identical to `vp9_perturb_refs=0` (the references are not read).
4. **Compound / real:** decode a compound-heavy clip (Big Buck Bunny "Surround")
   to HDMI and observe corruption; a compound-free large-frame clip is perfect.
5. **Cross-codec control:** the same static scene encoded as H.264 or HEVC (with
   equally tiny inter frames) decodes byte-exact — only VP9 fails.

---

## 7. 2026-06-26 update — exhaustive MPP replication; below-MMIO hardened; open PM/IOMMU lead

Since the 2026-06-13 writeup the triage was pushed to its limit. Summary of what is now
established, and the one dimension still open.

**(a) It is genuinely our-driver-wrong, not a silicon limit — proven against MPP and software.**
On a single md5-identical clip (`vp9fail.ivf`, BBB 640×360 libvpx-vp9 crf50), decoded on the
*same* VDPU383 silicon:
- MPP vs ffmpeg/libvpx (software): **0.000 — bit-exact**, deterministic.
- our V4L2 stack vs both: **0.346** mean abs error on the first INTER frame, cascading.

So MPP and the software reference agree exactly; ours is the outlier. The hardware can decode
this stream correctly — our mainline stack does not.

**(b) A second manifestation — sub-pel precision.** Beyond the gross reference-bypass on tiny
frames, complex / high-q INTER content shows a subtle sub-pel-precision drift (q-sweep: high-q
prediction-dominant frames fail, low-q residual-rich frames pass). Same below-MMIO wall, a
different surface; the residual masks it at low q.

**(c) Every software-reachable input and mechanism replicated to MPP — still wrong.** Using the
BSP `Vdpu383RegSet` offsetof field-map for an apples-to-apples register diff:
- `ctrl_regs` (r8–30) and `comm_paras` (r64–106, incl. reference strides/dims/mv-scaling and
  stream framing) are **byte-identical to MPP** for both the KEY and the failing INTER frame;
- the **stream bytes the HW reads** are byte-identical to the canonical IVF frame;
- RCB buffer sizes differ (over-allocated) but are **output-invariant** (scale 1×==2×);
- `colmv` is **not read** for the first failing frame (`use_prev_mvs=0`, prev is KEY);
- config delivered via the **DRAM-descriptor HW-fetch** path (skipping the redundant MMIO
  register write) produces **byte-identical wrong output** — delivery mechanism is not the cause;
- the per-frame **submission sequence is identical to the working HEVC backend** (and H.264
  uses a different sequence yet also works — sequence does not separate working from failing);
- a **continuously-armed link ring** (append branch confirmed firing: 1 bootstrap + N appends,
  HW armed across KEY→INTER with no disarm, exactly as MPP) — **still 0.346 wrong**.

With all of the above identical to MPP and the output still wrong while MPP is bit-exact on the
same silicon, the divergence is **internal hardware execution/state below every interface the
mainline V4L2 stack can touch** — the same conclusion the AV1 sibling reached independently
(`rkvdec-vdpu383-av1`).

**(d) The last open lead — per-frame PM / IOMMU / clock cycling — tested and REFUTED (2026-06-27).**
A full hardware-access trace of MPP showed it cycles, *per frame*: IOMMU TLB flush, IOMMU
re-initialisation (`rk_iommu_resume`), and decoder-clock gating. We forced our driver to do the
same — genuine per-frame suspend→resume with IOMMU re-init, clock off/on and the RK3576 warmup,
ftrace-confirmed to land *between* the KEY frame and the first INTER frame, escalated 1→12 cycles.
**It changed nothing — byte-identical wrong output (VP9) and 0/39 exact (AV1), every cycling
intensity.** Operation-class coverage is now complete: our driver exercises every clock / IOMMU /
reset / PM / warmup operation class MPP does — no MPP operation class is absent. (A first-pass
source diff also corrected the premise: in MPP's link/CCU mode, PM and clocks are burst-level,
not strictly per-frame; the only true per-frame op is the IOMMU TLB flush + cache-clear, both of
which our driver already performs.)

**(e) The decisive observation — the decode starts correct and diverges mid-frame.** On the AV1
sibling, frame 0's first 16 bytes are **byte-exact to the reference**, yet the frame's Y-MAE is
~90 — the hardware receives correct inputs (proven byte-identical), *begins* decoding correctly,
and diverges during its own internal pass. VP9 fails deterministically, AV1 metastably; same wall.
There is no driver-side input or operation that can alter a computation the hardware performs
correctly at the start of a frame and wrong by the end. **The investigation is terminal.** The one
un-run check (a register-*value* `rwmmio` diff) needs a full Armbian vendor-kernel rebuild (high
cost / per-boot re-brick risk) and is very unlikely to surface anything new, since the decoder
register values are already proven byte-identical and the core clk/IOMMU register values are not
our code.

**The question for the hardware owners:** what internal per-frame state or init step does the
VDPU383 require for VP9 (and AV1) INTER decode that H.264 and HEVC do not — given every
programmable input, the stream bytes, the submission sequence, the continuously-armed ring, *and*
the full clock/IOMMU/PM/warmup operation set are replicated from MPP, the decode *starts* correct,
yet diverges mid-frame while MPP (same silicon) is bit-exact?

**(f) 2026-06-29 — closed from two further directions (cross-codec, applies to this IP).** Run on the
deterministic AV1 repro but excluding the same shared infrastructure VP9 uses:

- **The graft** ran MPP's *actual compiled* `mpp_rkvdec2_link` back-end under the V4L2 front-end and
  **still produced the wrong attractor** — excluding the back-end, MPP's internal buffers and the
  register image (`GRAFT_TERMINAL_2026-06-29.md`).
- **The `mpp_service`-boundary source diff** found the one interface both stacks share **functionally
  identical** (one session per stream, shared worker, device held resumed) and carrying **no process
  context** (`current->`/`tgid`/`mm`/PASID/per-process IOMMU domain all absent in source) — the
  process-context hypothesis is **falsified in source** (`MPP_SERVICE_BOUNDARY_RESULT_2026-06-29.md`).

The residual is now excluded from **four independent directions** (decode-op matching; MPP→ours reverse
bisection; the graft; the boundary diff) and is HW-internal entropy/symbol-decoder state — measurable as
the `cabac_cdf_out` ~68% divergence (byte-identical input CDF). The maintainer-grade package is
`VDPU383_ENTROPY_RESIDUAL_EVIDENCE_BRIEF_2026-06-29.md`.
