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
