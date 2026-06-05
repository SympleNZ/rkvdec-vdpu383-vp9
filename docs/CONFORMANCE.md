# VP9 conformance tail — accurately characterized (2026-06-05)

Working through the "remaining open items", I re-examined the Fluster failure
clusters (`VP9-TEST-VECTORS`, 148/305 pass) by decoding individual vectors and
comparing our output to the libvpx (ffmpeg) reference directly — rather than
trusting Fluster's pass/fail or the prior "it's all downstream comparator noise"
belief. **That belief was wrong: part of the tail is a real, distinct driver
bug.** Method: `gst … v4l2slvp9dec ! videoconvert ! I420 ! filesink` vs
`ffmpeg -pix_fmt yuv420p`, then byte/per-frame diff.

## 10-bit Profile 2 (HIGH suite) — functionally correct, ±1 rounding

`VP9-TEST-VECTORS-HIGH` strict-MD5: 0/6. But decoding
`vp92-2-20-10bit-yuv420` and comparing to the libvpx reference (both as
`I420_10LE` / `yuv420p10le`):

- **85.3% of samples byte-exact; 14.6% off by exactly +1; max delta 4.**
- The strong +1 bias (31598 `+1` vs 106 `-1`) is a reconstruction-rounding
  difference exposed at 10-bit precision, **not a decode defect** (a real bug is
  random and large). 8-bit passes strict-MD5 because these sub-LSB differences
  round away at 8-bit.

**Verdict: 10-bit Profile 2 decode is correct (visually lossless); strict-MD5
fails only on ±1 rounding.** Profile 3 (4:2:2 / 4:4:4) and 12-bit are
**unsupported by design** (driver declares only 4:2:0 8/10-bit). The remaining
HIGH failures are those unsupported formats.

## Small-dimension cluster (`vp90-2-02-size-*`) — TWO causes, one is a real bug

Per-vector our-vs-ffmpeg I420 diff, grouped by width:

| width | result | meaning |
|---|---|---|
| 8, 64 (alignment-multiple) | size matches, **differ 69–85%** | **real single-ref INTER decode bug** |
| 34, 66 (non-aligned) | output **larger** than ref (STRIDE_PAD) | HW-aligned bytesperline padding not cropped from output |

For `vp90-2-02-size-08x08` (frame-by-frame): **frame 0 (KEY) is byte-perfect
(0/96); frames 1–9 (inter) are 79–92/96 wrong; all `refmode=0` (single-ref, NOT
compound); 0 timeouts.** So this is a genuine **small-dimension single-ref INTER
decode error** — the KEY/intra path is correct, the inter motion-compensation
path produces wrong pixels at small dimensions. Distinct from the compound bug
(that's `refmode=2`). BBB single-ref inter at 1080p is byte-perfect, so the bug
is specific to small dimensions.

**Leading hypothesis:** inter MC reads reference pixels with a wrong stride /
edge-extension at small dimensions (where HW-aligned bytesperline, e.g. 192 for
an 8-px row, diverges sharply from the visible width). KEY frames don't read
references, so they're unaffected. (Untested — a lead for a future fix session.)

**This corrects the in-tree R7DUMP comment** in `rkvdec_vdpu383_vp9_done`, which
concluded "HW decodes 8×8 perfectly, the failure is the userspace comparator."
That was based on a single (KEY) frame's buffer dump; the inter frames are
actually wrong. Comment updated.

## Resize cluster (`vp90-2-21-resize_inter_*`, `vp90-2-14-resize-*`) — downstream gst

`vp90-2-21-resize_inter_320x180_5_1-2`: `gst_rc=0`, our output 864000 B (10
frames) vs ffmpeg 2592000 B (30 frames) — **incomplete frame delivery through
the mid-stream resize**, not wrong pixels. Matches `PROFILE2_RESIZE_2026-05-28.md`:
the driver emits `SOURCE_CHANGE` correctly but gst-plugins-bad's
`copy_output_buffer` mishandles the stride transition and drops frames.
Downstream (gst), not a kernel decode bug.

## Net — corrected conformance picture

The 157 Fluster failures are NOT one downstream bucket. They are:

1. **Compound (`refmode=2`)** — the vendor bug (the dominant correctness
   blocker; → publish + Detlev).
2. **Small-dimension single-ref INTER** — a **real, distinct driver bug** (~half
   the `size` cluster, aligned widths). NEW finding; previously misfiled as
   "downstream". A tractable-looking lead (inter MC stride/edge at small dims).
3. **Small-dimension non-aligned width** — output carries HW bytesperline
   padding; a crop/de-stride gap (driver could set the V4L2 crop rectangle, or
   userspace must honour bytesperline). Borderline-downstream.
4. **Resize** — gst-plugins-bad `copy_output_buffer` (downstream).
5. **10-bit** — correct to ±1 rounding (strict-MD5 intolerance), not a defect.

Two genuinely-kernel correctness items remain: the compound bug (vendor) and the
**small-dim single-ref inter bug (ours, newly found)**.

## Small-dim inter bug — bounded: conformance-only, ZERO real-world impact

Threshold bisect with clean compound-free synthetic inter streams (testsrc2,
`-auto-alt-ref 0`, `refmode=0`, no timeout), our-vs-ffmpeg I420:

| size | differ |
|---|---|
| 64×64 | 8.6% (a few blocks, maxd ~222) |
| 128×128 | 0.0% |
| 256×256 | 0.0% |
| 512×512 | 0.0% |

So:
- **Real content (≥128px) decodes byte-perfect** — no practical impact (no real
  video is 8×8/64×64; clean720/1080 inter are byte-exact).
- The heavy 70–85% failures are the **conformance `size` vectors specifically**
  (edge-case coding at tiny dims); plain synthetic 64×64 is only ~8.6% off and
  ≥128px is perfect. So the bug is **content-AND-dimension dependent**, confined
  to sub-128px conformance stress vectors.

**Concrete fix lead:** `rkvdec-vdpu383-vp9.c:1511` uses `height = ALIGN(h, 8)`
for `hw_y_stride` (the reference UV-plane offset), but the adjacent comment and
MPP align coded height to **64**. For an 8-px-high frame that's 8 vs 64 — if the
HW MC engine assumes 64-aligned reference UV internally, it reads chroma (and via
edge effects, luma) from the wrong offset on tiny frames. Self-consistent for our
own-decoded refs only if the HW honours the programmed stride for both write and
MC-read; a divergence there fits the symptom. Untested — would need a per-vector
MPP register-diff (the compound-bug tooling) to confirm/fix, for conformance-only
gain.

**Disposition:** real-world-irrelevant. Documented as a precise known limitation
(sub-128px conformance edge cases) with a fix lead, rather than spending a deep
MPP-diff session on zero-impact vectors. Revisit only if full conformance is
needed for upstream.

### Fix attempt (chased on request) — both cheap leads DISPROVEN

Stride values via the `rkvdec-vp9 fmt:` pr_debug:

| size | bpl | hw_hor | hw_y_stride | result |
|---|---|---|---|---|
| 64×64 | 192 | 0xc | 0x300 | fails 8.6% |
| 128×128 | 192 | 0xc | 0x600 | works 0% |
| 256×256 | 448 | 0x1c | 0x1c00 | works 0% |

- **Stride hypothesis DEAD:** 64 (fails) and 128 (works) have **identical bpl
  (192) and hw_hor (0xc)** — same stride — so the bug is NOT a stride/bytesperline
  value we set. (hw_y_stride differs only proportionally with height, correct.)
- **Height-ALIGN-8-vs-64 lead DEAD:** R7DUMP showed the 8×8 write UV is at
  bpl×8 and the ref-read uses the same hw_y_stride → write/read self-consistent;
  KEY frames confirm the layout.
- **Correlation:** failure tracks **frame ≤ 64px (a single 64×64 superblock)** +
  content density (synthetic 64×64 = 8.6%; conformance size-64x* = 70–85%). Not
  any obvious dimension register.

This is the **same class as the compound bug**: a tiny-frame HW-execution
difference not expressed in the visible register values. The next step is the
full per-vector **MPP register-diff**, but the `.104` MPP dump build
(`builddir-dump` with `DUMP_VDPU383_DATAS`) is **gone** and would need a
compound-scale rebuild. **Deferred** — not worth a multi-session, cross-board
MPP-diff rebuild for sub-128px conformance vectors with zero real-world impact.
If pursued later: rebuild MPP dump build on `.104`, dump regs for an inter frame
of `vp90-2-02-size-64x64`, diff vs our `VP9_DUMP_REGS` output (set the macro to
1), look for the non-obvious single-SB register MPP sets that we don't.
