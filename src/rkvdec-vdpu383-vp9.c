// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder VP9 driver for VDPU383 (RK3576)
 *
 * Copyright (C) 2026 Symple Solutions Ltd.
 * Author: Simon <simon@symple.nz>
 *
 * Based on rkvdec-vp9.c (RK3399 VP9 backend) by Boris Brezillon.
 * Register programming follows the mainline VDPU383 HEVC/H.264 backends:
 * a struct vdpu383_regs_vp9 is populated in RAM and pushed to MMIO via
 * four rkvdec_memcpy_toio() calls, one per offset region. This matches
 * the IP block's "registers must be written in increasing u32 order"
 * requirement, which individual writel() calls do not satisfy.
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/hw_bitfield.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <media/v4l2-mem2mem.h>
#include <media/v4l2-vp9.h>

#include "rkvdec.h"
#include "rkvdec-rcb.h"
#include "rkvdec-link.h"
#include "rkvdec-vdpu383-regs.h"
#include "rkvdec-vdpu383-vp9-regs.h"

extern int rkvdec_link_mode;
extern int r43_invalidate_probs_on_fail;
extern int rcb_sram_pack;	/* rcb.c: per-region SRAM packing (N1 follow-up) */
extern int r44_dump_altref_regs;
extern int r45_reset_before_altref;
extern int r49_pre_mv_carryforward;
extern int vp9_time;
extern int vp9_skip_tlb_flush;

/*
 * Lead 3 enabler (NEXT_SESSION_LINK_MODE_CLIENT_2026-06-02): when set,
 * dump each frame's v4l2_ctrl_vp9_frame to dmesg as one-line hex
 * ("VP9CTRLDUMP idx=<n> sz=<bytes> <hex>"), in decode order. The
 * multi-buffer replay client reads these (paired 1:1 with IVF frames for
 * ALLKEY) to set V4L2_CID_STATELESS_VP9_FRAME without parsing the VP9
 * uncompressed header itself. Default 0.
 */
int vp9_dump_ctrls;
module_param(vp9_dump_ctrls, int, 0644);
MODULE_PARM_DESC(vp9_dump_ctrls,
		 "Dump v4l2_ctrl_vp9_frame per frame to dmesg as hex (link-mode client capture).");

/* 2026-06-12 fable review: stream-tail sensitivity probe for the tiny-frame
 * partial-decode failure (yt overlay / 35 B skip frames). See the pad block
 * in vdpu383_vp9_build_regs(). 0=off, 1=re-zero pad + cache clean,
 * 2=poison pad 0xAA + cache clean. */
static int vp9_tail_dbg;
module_param(vp9_tail_dbg, int, 0644);
MODULE_PARM_DESC(vp9_tail_dbg,
		 "VP9 stream-tail probe: 0=off 1=zero+sync 2=poison+sync (diagnostic)");

/* 2026-06-12 fable review: the Bug-A tiny-frame (<512 B) LINK_TIMEOUT
 * override — historic value 500000 cycles. The refs-bypassed decode mode's
 * boundary is exactly this <512 B condition; 0 disables the override
 * entirely (resolution-based threshold for every frame). */
static int vp9_tiny_timeout = 500000;
module_param(vp9_tiny_timeout, int, 0644);
MODULE_PARM_DESC(vp9_tiny_timeout,
		 "LINK_TIMEOUT_THRESHOLD override (cycles) for <512B frames; 0=no override (default 500000 = legacy)");

/* 2026-06-12 fable review: arm INT_EN LINE_IRQ (bit1) alongside bit0 at the
 * single-shot kick, matching the BSP's per-task irq_mask 0x30000. Tests
 * whether the line-IRQ enable is load-bearing for the tiny-frame
 * refs-bypass. Default off. */
static int vp9_int_lineirq;
module_param(vp9_int_lineirq, int, 0644);
MODULE_PARM_DESC(vp9_int_lineirq,
		 "Arm INT_EN bits 0+1 (BSP irq_mask) at VP9 kick (0=off,1=on) - diagnostic");

/*
 * 2026-06-04: clear all THREE decoder caches (CACHE0/1/2) like the BSP
 * mpp_service, not just CACHE0. The BSP MMIO trace (PHASE2_MPP_TRACE) clears
 * CACHE0 (0x510/0x51c), CACHE1 (0x550/0x55c) and CACHE2 (0x590/0x59c) — all at
 * device base, i.e. our function base + 0x410/0x45c... (function base =
 * device + 0x100). Compound prediction reads two references through the
 * reference caches; if CACHE1/CACHE2 retain stale lines from a prior decode,
 * compound MC reads stale second-reference data → collapse to alt-only
 * (the yt tiny-compound-frame bug). The prior session-s attempt regressed
 * Fluster because it used BSP DEVICE offsets (0x55x/0x59x) with our
 * FUNCTION-base pointer, landing 0x100 too high. Correct function offsets:
 * CACHE1 size 0x45c / clr 0x450, CACHE2 size 0x49c / clr 0x490.
 */
int vp9_cache_all = 0;
module_param(vp9_cache_all, int, 0644);
MODULE_PARM_DESC(vp9_cache_all,
		 "Clear all 3 decoder caches (BSP-style) not just CACHE0. 1=on.");

/*
 * 2026-06-04: write the register regions in the EXACT order the upstream
 * VDPU383 HEVC driver uses (COMMON, COMMON_ADDR, CODEC_PARAMS, CODEC_ADDR
 * as four separate memcpy_toio bursts), instead of our current
 * COMMON, CODEC_PARAMS, unified-COMMON_ADDR+CODEC_ADDR. The mainline
 * VDPU383 merge documented that "register write ordering matters — random
 * access ordering could break the decoder." HEVC (working) writes
 * common_addr BEFORE params and codec_addr as a separate burst AFTER;
 * our VP9 deviates. 1=use the HEVC order.
 */
int vp9_hevc_order = 0;
module_param(vp9_hevc_order, int, 0644);
MODULE_PARM_DESC(vp9_hevc_order,
		 "Write reg regions in upstream HEVC order (COMMON,COMMON_ADDR,CODEC_PARAMS,CODEC_ADDR). 1=on.");

/*
 * VP9 setup_past_independence: reset HW frame-context prob buffers to spec
 * DEFAULT (not zero) on KEY / error-resilient / intra-reset frames, so the
 * inter-prob region a KEY doesn't adapt is default rather than zero — fixes
 * SELECT/compound INTER desync on high-motion content. Default on; gated
 * for A/B against the prior zero-init behaviour.
 */
/*
 * Default 0: tested 2026-06-03, NO effect on the yt INTER bug (57/60
 * unchanged) — so the SELECT-frame desync is NOT zero-inter-probs in the
 * context (f2 reads prob_default, which already has correct comp probs).
 * Kept gated (spec-correct on its own) for future use; off by default to
 * avoid perturbing working content.
 */
int vp9_ctx_default_on_intra;
module_param(vp9_ctx_default_on_intra, int, 0644);
MODULE_PARM_DESC(vp9_ctx_default_on_intra,
		 "Reset frame contexts to default probs on KEY/intra (1=on, spec-correct; no effect on yt bug).");

/*
 * 2026-06-04 H1-vs-H2 probe: redirect the non-alt compound reference legs
 * (last + golden, header reg170/171 + payload reg195/196) of the DISPLAYED
 * compound frame to a scratch buffer filled with 0x00, leaving alt
 * (reg172/197) intact. Decode the yt_keyf1f2 repro with the param off then
 * on and diff the output:
 *   - output UNCHANGED  -> last/golden are never read; F2 collapsed to
 *                          degenerate single-ref-alt modes (H1, vendor HW).
 *   - output DARKENS    -> compound IS averaging the non-alt leg (H2, a
 *                          reference-pair/setup bug that is ours to fix).
 * Gated; default off so it never perturbs normal content.
 */
int vp9_perturb_refs;
module_param(vp9_perturb_refs, int, 0644);
MODULE_PARM_DESC(vp9_perturb_refs,
		 "Redirect non-alt compound ref legs to a 0x00 scratch buffer for the displayed compound frame (H1/H2 probe). 1=on.");

/*
 * Cat 5.1 — IOMMU access trace (2026-06-09). The rockchip IOMMU logs only
 * page FAULTS, not successful translations, so we cannot passively watch what
 * the HW fetches. This turns "does the HW fetch the last/golden legs of the
 * compound frame?" into a binary fault signal: point reg170/171 (+payload
 * reg195/196) at a deliberately UNMAPPED IOVA sentinel for the displayed
 * compound frame, alt (reg172/197) left intact.
 *   - rk_iommu page fault at the sentinel -> HW DID issue reads to last/golden
 *     -> compound combine/weight is the broken stage, not the fetch.
 *   - NO fault, decode proceeds (single-ref-alt) -> HW never fetches the second
 *     leg -> compound is dead at the bus level inside the HW.
 * Strictly stronger than vp9_perturb_refs (a read-into-discard could defeat the
 * content perturb; an unmapped IOVA cannot be read silently). Default off.
 */
int vp9_iova_fault;
module_param(vp9_iova_fault, int, 0644);
MODULE_PARM_DESC(vp9_iova_fault,
		 "Point compound last/golden legs at UNMAPPED IOVA sentinels to fault-trace HW ref fetches (Cat 5.1). 1=on.");

/*
 * Cat 6.1 reserved-register-bit sweep (2026-06-09). OR a mask into a common
 * control register (index relative to reg008) on the displayed compound frame,
 * immediately before the kick. Purpose: surface an UNDOCUMENTED control bit a
 * value-diff cannot see — MPP sets the same *named* bits we do (Cat 1 matched),
 * so a reserved bit MPP never touches is invisible to any input comparison.
 * Signal: vp9_adapt_dump's adapted comp_mode@116 / whole-prob CRC. Baseline is
 * static (HW decodes single-ref, compound never engages); a fuzz bit that MOVES
 * comp_mode@116 or the CRC affects compound decoding → candidate fix lever.
 * Default off (reg index -1). reg009_important_en = index 1 (the prime target).
 */
static int vp9_regfuzz_reg = -1;
module_param(vp9_regfuzz_reg, int, 0644);
MODULE_PARM_DESC(vp9_regfuzz_reg, "Cat6.1: common-reg index rel reg008 to OR-fuzz on the compound frame; -1=off");
static uint vp9_regfuzz_or;
module_param(vp9_regfuzz_or, uint, 0644);
MODULE_PARM_DESC(vp9_regfuzz_or, "Cat6.1: 32-bit mask OR'd into the vp9_regfuzz_reg register before kick");

/*
 * 2026-06-08 compound "why" probe: the perturb test proved compound doesn't
 * ENGAGE (HW uses alt-only) but not WHY. The HW writes its post-decode adapted
 * probability state to probs[frame_context_idx] (reg185) — and the adapted
 * comp_mode/comp_ref/single_ref probs encode which modes HW actually DECODED.
 * Dump that buffer (CRC + first96 + refmode) post-decode and compare ours-vs-MPP
 * for the failing compound frame (refmode=2=SELECT):
 *   - adapted probs == MPP -> HW decoded the SAME modes (incl compound) yet our
 *     output collapses -> compound EXECUTION erratum (silicon; sharpest evidence).
 *   - adapted probs differ -> HW decoded the modes differently -> the entropy
 *     decode / context is the cause (potentially ours; the "missed" thing).
 * Observe-only, default off.
 */
static int vp9_adapt_dump;
module_param(vp9_adapt_dump, int, 0644);
MODULE_PARM_DESC(vp9_adapt_dump, "VP9 dump HW-adapted prob buffer (probs[ctx]) post-decode + refmode, observe-only (0=off,1=on)");
#define VP9_PERTURB_SZ (3u * 1024 * 1024)
static void *vp9_perturb_cpu;
static dma_addr_t vp9_perturb_dma;

/*
 * Diagnostic: when defined non-zero, dumps the four assembled register
 * substructs to dmesg per frame right before the HW kick. Compare
 * against MPP's regs_full.dat (Vdpu383RegSet) to find layout/value
 * divergences. Frame counter is global, monotonically increasing.
 * Compile-time flag to avoid adding a kernel-exported module_param.
 */
#define VP9_DUMP_REGS 0
#if VP9_DUMP_REGS
static unsigned int vp9_dump_frame_no;
#endif

/* -----------------------------------------------------------------------
 * Constants — identical to rkvdec-vp9.c (hardware-independent)
 */

/* Probability context buffer per frame context (4 contexts × 4864 bytes). */
#define RKVDEC_VP9_PROBE_SIZE		4864
/* Symbol count accumulation buffer for hardware probability update. */
#define RKVDEC_VP9_COUNT_SIZE		13232
/* Segment ID map — one per frame (current + last). */
#define RKVDEC_VP9_MAX_SEGMAP_SIZE	73728

#define VP9_NUM_FRAME_CTX		4

/*
 * VDPU383 global header buffer size. VP9 decode parameters are packed
 * into a DMA buffer at reg131_gbl_base rather than into dedicated param
 * registers. From MPP: GBL_SIZE = 2 * (ALIGN(1299, 128) / 8) = 352 bytes.
 */
#define VDPU383_VP9_GBL_SIZE		352
#define VDPU383_VP9_GBL_LEN		(VDPU383_VP9_GBL_SIZE / 16)	/* 22 */

/* PROB_KF_SIZE: keyframe default probability table (1312 bytes). */
#define RKVDEC_VP9_PROB_KF_SIZE		1312

/* -----------------------------------------------------------------------
 * Per-frame state (mirrors rkvdec_vp9_frame_info in rkvdec-vp9.c)
 */
struct rkvdec_vdpu383_vp9_frame_info {
	u32 valid		: 1;
	u32 segmapid		: 1;
	u32 frame_context_idx	: 2;
	u32 reference_mode	: 2;
	u32 tx_mode		: 3;
	u32 interpolation_filter: 3;
	u32 flags;
	u64 timestamp;
	u16 width;
	u16 height;
	/*
	 * VP9 color_space (3-bit field from the uncompressed header).
	 * Only carried by KEY frames in the bitstream; INTER frames
	 * inherit. MPP packs ls_info.color_space_last into gbl_buf
	 * at the "last color space" slot. V4L2 v4l2_ctrl_vp9_frame
	 * doesn't expose this, so we parse it from the bitstream on
	 * KEY frames and carry forward through `last` on INTER.
	 */
	u8 color_space;
	struct v4l2_vp9_segmentation seg;
	struct v4l2_vp9_loop_filter lf;

	/* Bug-A post-decode dmaengine copy stash (see rkvdec_vdpu383_vp9_done).
	 * When bug_a_copy_sz != 0, vp9_done() does a PL330 memcpy from
	 * bug_a_alt_dma -> bug_a_dst_dma for bug_a_copy_sz bytes, replacing
	 * the zeros that the silicon wrote with valid alt-ref pixels. */
	dma_addr_t bug_a_alt_dma;
	dma_addr_t bug_a_dst_dma;
	size_t bug_a_copy_sz;
};

struct rkvdec_vdpu383_vp9_probs {
	u8 probs[VP9_NUM_FRAME_CTX][RKVDEC_VP9_PROBE_SIZE];
} __aligned(128);

struct rkvdec_vdpu383_vp9_priv_tbl {
	struct rkvdec_vdpu383_vp9_probs probs;		/* 4 × 4864 = 19456 */
	/*
	 * Pristine "defaults" prob buffer. reg184_lastprob_base points
	 * here when prob_ctx_valid[ctx_id]=0 — i.e. when the IP block has
	 * no saved adaptation for this context yet. The IP block writes
	 * its post-decode adapted state to reg185 = probs[ctx_id]; if we
	 * pointed reg184 at the same buffer we're writing through reg185,
	 * the writeback would overwrite our pristine defaults and the
	 * next first-use of any context_idx would read garbage. Mirrors
	 * MPP's hw_ctx->prob_default_base.
	 */
	u8 prob_default[RKVDEC_VP9_PROBE_SIZE] __aligned(128);
	u8 kf_probs[RKVDEC_VP9_PROB_KF_SIZE];		/* 1312 */
	u8 segmap[2][RKVDEC_VP9_MAX_SEGMAP_SIZE];	/* 2 × 73728 */
} __aligned(4096);

struct rkvdec_vdpu383_vp9_ctx {
	struct rkvdec_aux_buf priv_tbl;
	struct rkvdec_aux_buf count_tbl;
	struct rkvdec_aux_buf gbl_buf;

	struct v4l2_vp9_frame_symbol_counts inter_cnts;
	struct v4l2_vp9_frame_symbol_counts intra_cnts;
	struct v4l2_vp9_frame_context probability_tables;
	struct v4l2_vp9_frame_context frame_context[VP9_NUM_FRAME_CTX];

	u8 prob_ctx_valid[VP9_NUM_FRAME_CTX];

	struct rkvdec_vdpu383_vp9_frame_info cur;
	struct rkvdec_vdpu383_vp9_frame_info last;

	/* Register image: populated per-frame, pushed via memcpy_toio. */
	struct vdpu383_regs_vp9 regs;

	/* Phase 2 LINK mode descriptor ring. ctx->link_table points at
	 * this when rkvdec_link_mode is enabled. Allocated unconditionally
	 * in vp9_start (small ~1KB) so the param can be flipped without
	 * a stream restart. */
	struct rkvdec_link_table link_table;

	/*
	 * 2026-05-31 Round 15: ref-frame DMA ring.
	 *
	 * vb2_find_buffer fails for some VP9 INTER content (notably 09-aq2
	 * with 34 fallbacks, 09-lf_deltas with 6, 15-segkey_adpq with 5;
	 * all correlate 3/3 with Fluster failure vs 0 fallbacks on
	 * passing vectors). The existing fallback path returns the CURRENT
	 * dst_buf's DMA — meaning HW reads the buffer it's actively writing
	 * as the reference frame → corrupted output.
	 *
	 * Track the last 8 successfully-decoded frames by timestamp here
	 * so vp9_ref_dma can fall back to a real previous frame address
	 * when vb2 lookup misses.
	 *
	 * Updated at vp9_done after successful decode. Lookup in vp9_run
	 * via vp9_ref_dma. 8 slots is well above VP9's 3 active refs
	 * (last/golden/alt) so transient mismatches still resolve.
	 */
	struct {
		u64 timestamp;
		dma_addr_t dma;
		/*
		 * 2026-06-13 (#5a): coded dims of the frame in this slot, so a
		 * later frame referencing it by timestamp can pack the correct
		 * per-ref dimensions into the GBL (mvscale) — matching MPP's
		 * ref_frame_coded_width[frame_refs[i].Index]. 0 = unknown.
		 */
		u32 width;
		u32 height;
		bool valid;
	} ref_ring[8];
	u32 ref_ring_next;

	/*
	 * 2026-05-31 Round 5: per-context resize-event detection.
	 * Track previous frame dimensions; if subsequent frame differs,
	 * mark the context as resize-tainted and reject all further
	 * frames. Forces userspace to renegotiate via SOURCE_CHANGE,
	 * starting a fresh stream with consistent dimensions.
	 *
	 * Background: 14-resize content (vp90-2-14-resize-*) has frame-
	 * to-frame dimension changes within a stream that trigger HW
	 * address miscomputation (Round 2.8 / 4.3 docs). HW writes
	 * past the RCB mapping → IOMMU page faults → eventual domain
	 * corruption at batch scale. Detecting + rejecting at the
	 * resize boundary prevents the damage.
	 */
	u32 prev_frame_w;
	u32 prev_frame_h;
	bool resize_seen;

	/*
	 * R49 (2026-06-02) — last-shown-INTER colmv DMA, persistent across
	 * frames including alt-refs. Matches MPP HAL's `pre_mv_base_addr`
	 * semantics (hal_vp9d_vdpu383.c:469-472, 522, 527): updated only
	 * when current frame was visible INTER (not intra_only,
	 * not error_resilient, last_show_frame was set), otherwise
	 * carried forward unchanged.
	 *
	 * Use case: alt-ref decode's reg217 (colmv_ref_base) should point
	 * at the LAST SHOWN INTER's colmv even if there's been an alt-ref
	 * between. Our `last` tracking only sees one back, so when last
	 * was alt-ref we self-ref colmv. MPP carries pre_mv across, so
	 * its alt-ref decode points reg217 at the visible-INTER from
	 * before the previous alt-ref.
	 */
	dma_addr_t last_shown_inter_colmv_dma;
	bool last_shown_inter_colmv_valid;
};

/* -----------------------------------------------------------------------
 * Per-context probability table packing (reg184_lastprob_base format).
 *
 * Lays a struct v4l2_vp9_frame_context into the 4864-byte hardware buffer
 * the IP block reads for last-frame probability state. Layout mirrors the
 * vendor driver's probability defaults; the IP block has no public spec.
 */
struct vp9_prob_packer {
	__le64 *buf;
	u32 idx;
};

static void pp_put(struct vp9_prob_packer *p, u64 val, u32 nbits)
{
	while (nbits) {
		u32 word = p->idx >> 6;
		u32 off  = p->idx & 63;
		u32 n    = min_t(u32, nbits, 64 - off);
		u64 mask = n == 64 ? ~0ULL : ((1ULL << n) - 1);

		p->buf[word] |= cpu_to_le64((val & mask) << off);
		val  >>= n;
		nbits -= n;
		p->idx += n;
	}
}

static void pp_align(struct vp9_prob_packer *p, u32 align_bits)
{
	p->idx = ALIGN(p->idx, align_bits);
}

static void pack_vp9_probs_hw(u8 *buf,
			      const struct v4l2_vp9_frame_context *probs)
{
	struct vp9_prob_packer pp = { .buf = (__le64 *)buf, .idx = 0 };
	int i, j, k, m, n;
	int byte_count;

	memset(buf, 0, RKVDEC_VP9_PROBE_SIZE);

	/* Section 1 — 5 × 128 bits: sb info */
	/* partition probs[16][3]. MPP uses pic_param->prob.partition which
	 * carries kf_partition_probs for KEY frames or the saved/forward-
	 * updated INTER table for INTER frames. We pass that through via
	 * the v4l2_vp9_frame_context the caller hands us.
	 */
	for (i = 0; i < 16; i++)
		for (j = 0; j < 3; j++)
			pp_put(&pp, probs->partition[i][j], 8);

	/* Segment-id pred probs (3) + tree probs (7) — packed as zero. */
	for (i = 0; i < 3 + 7; i++)
		pp_put(&pp, 0, 8);

	/* Skip flag probs (3 contexts) */
	for (i = 0; i < 3; i++)
		pp_put(&pp, probs->skip[i], 8);

	/* tx_size probs */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			pp_put(&pp, probs->tx32[i][j], 8);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			pp_put(&pp, probs->tx16[i][j], 8);
	for (i = 0; i < 2; i++)
		pp_put(&pp, probs->tx8[i][0], 8);

	/* intra_inter (4 contexts) */
	for (i = 0; i < 4; i++)
		pp_put(&pp, probs->is_inter[i], 8);

	pp_align(&pp, 128);

	/* Section 2 — 6 × 128 bits: intra y_mode + inter block info */
	for (i = 0; i < 4; i++)
		for (j = 0; j < 9; j++)
			pp_put(&pp, probs->y_mode[i][j], 8);

	for (i = 0; i < 5; i++)
		pp_put(&pp, probs->comp_mode[i], 8);
	for (i = 0; i < 5; i++)
		pp_put(&pp, probs->comp_ref[i], 8);
	for (i = 0; i < 5; i++)
		for (j = 0; j < 2; j++)
			pp_put(&pp, probs->single_ref[i][j], 8);

	for (i = 0; i < 7; i++)
		for (j = 0; j < 3; j++)
			pp_put(&pp, probs->inter_mode[i][j], 8);

	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			pp_put(&pp, probs->interp_filter[i][j], 8);

	pp_align(&pp, 128);

	/* Section 3 — 128 × 128 bits: coef probs (intra, then inter) */
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++) {
			byte_count = 0;
			for (k = 0; k < 6; k++)
				for (m = 0; m < 6; m++)
					for (n = 0; n < 3; n++) {
						pp_put(&pp, probs->coef[i][j][0][k][m][n], 8);
						if (++byte_count == 27) {
							pp_align(&pp, 128);
							byte_count = 0;
						}
					}
			pp_align(&pp, 128);
		}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++) {
			byte_count = 0;
			for (k = 0; k < 6; k++)
				for (m = 0; m < 6; m++)
					for (n = 0; n < 3; n++) {
						pp_put(&pp, probs->coef[i][j][1][k][m][n], 8);
						if (++byte_count == 27) {
							pp_align(&pp, 128);
							byte_count = 0;
						}
					}
			pp_align(&pp, 128);
		}

	/* Section 4 — intra uv_mode, packed in 4 aligned chunks */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 9; j++)
			pp_put(&pp, probs->uv_mode[i][j], 8);
	pp_align(&pp, 128);
	for (i = 3; i < 6; i++)
		for (j = 0; j < 9; j++)
			pp_put(&pp, probs->uv_mode[i][j], 8);
	pp_align(&pp, 128);
	for (i = 6; i < 9; i++)
		for (j = 0; j < 9; j++)
			pp_put(&pp, probs->uv_mode[i][j], 8);
	pp_align(&pp, 128);
	for (i = 9; i < 10; i++)
		for (j = 0; j < 9; j++)
			pp_put(&pp, probs->uv_mode[i][j], 8);
	pp_align(&pp, 128);
	pp_put(&pp, 0, 8);
	pp_align(&pp, 128);

	/* Section 5 — mv probs (6 × 128 bits) */
	for (i = 0; i < 3; i++)
		pp_put(&pp, probs->mv.joint[i], 8);
	for (i = 0; i < 2; i++)
		pp_put(&pp, probs->mv.sign[i], 8);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 10; j++)
			pp_put(&pp, probs->mv.classes[i][j], 8);
	for (i = 0; i < 2; i++)
		pp_put(&pp, probs->mv.class0_bit[i], 8);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 10; j++)
			pp_put(&pp, probs->mv.bits[i][j], 8);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 3; k++)
				pp_put(&pp, probs->mv.class0_fr[i][j][k], 8);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			pp_put(&pp, probs->mv.fr[i][j], 8);
	for (i = 0; i < 2; i++)
		pp_put(&pp, probs->mv.class0_hp[i], 8);
	for (i = 0; i < 2; i++)
		pp_put(&pp, probs->mv.hp[i], 8);

	pp_align(&pp, 128);
	pp_put(&pp, 0, 8);
	pp_align(&pp, 128);
}

/* -----------------------------------------------------------------------
 * KF probability table packing (unchanged from prior version)
 */
static const u8 vdpu383_vp9_kf_partition_probs[16][3] = {
	{ 158,  97,  94 }, {  93,  24,  99 }, {  85, 119,  44 }, {  62,  59,  67 },
	{ 149,  53,  53 }, {  94,  20,  48 }, {  83,  53,  24 }, {  52,  18,  18 },
	{ 150,  40,  39 }, {  78,  12,  26 }, {  67,  33,  11 }, {  24,   7,   5 },
	{ 174,  35,  49 }, {  68,  11,  27 }, {  57,  15,   9 }, {  12,   3,   3 },
};

static void populate_kf_probs(u8 *buf)
{
	int off = 0, i, j, k, bc;

	memset(buf, 0, RKVDEC_VP9_PROB_KF_SIZE);

	for (i = 0; i < 16; i++)
		for (j = 0; j < 3; j++)
			buf[off++] = vdpu383_vp9_kf_partition_probs[i][j];

	for (i = 0; i < 10; i++) {
		bc = 0;
		for (j = 0; j < 10; j++) {
			for (k = 0; k < 9; k++) {
				buf[off++] = v4l2_vp9_kf_y_mode_prob[i][j][k];
				if (++bc == 27) {
					off = ALIGN(off, 16);
					bc = 0;
				}
			}
		}
		if (i < 4) {
			int flat_start = i * 23;
			int flat_end   = (i < 3) ? flat_start + 23 : flat_start + 21;
			int m;

			for (m = flat_start; m < flat_end; m++)
				buf[off + (m - flat_start)] =
					v4l2_vp9_kf_uv_mode_prob[m / 9][m % 9];
		}
		off = ALIGN(off + 23, 16);
	}
}

/* -----------------------------------------------------------------------
 * Default probability table initialisation
 */
static void init_probs(struct rkvdec_vdpu383_vp9_ctx *vp9_ctx)
{
	struct rkvdec_vdpu383_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
	int i;

	/* Initialise all 4 frame contexts to VP9 spec defaults. */
	for (i = 0; i < VP9_NUM_FRAME_CTX; i++)
		vp9_ctx->frame_context[i] = v4l2_vp9_default_probs;

	/*
	 * Pack the spec defaults into the pristine prob_default buffer.
	 * reg184_lastprob_base reads from here when prob_ctx_valid is 0
	 * for a given context_idx. The per-context probs[i] slots get the
	 * same defaults as a starting baseline, but the IP block will
	 * overwrite them via reg185 writeback after each decode using
	 * that context.
	 */
	pack_vp9_probs_hw(tbl->prob_default, &v4l2_vp9_default_probs);
	/* prob_loop_base[i] gets HW writeback after each refresh_frame_context
	 * decode. Initial state should be zero (matches MPP's mpp_buffer_get
	 * which returns zeroed buffers); we previously packed defaults, which
	 * leaves stale prob bytes in regions HW doesn't fully overwrite,
	 * accumulating divergence vs MPP over frames.
	 */
	for (i = 0; i < VP9_NUM_FRAME_CTX; i++)
		memset(tbl->probs.probs[i], 0, sizeof(tbl->probs.probs[i]));

	populate_kf_probs(tbl->kf_probs);
}

/* -----------------------------------------------------------------------
 * V4L2 format adjustment
 */
static int rkvdec_vdpu383_vp9_adjust_fmt(struct rkvdec_ctx *ctx,
					  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *fmt = &f->fmt.pix_mp;

	/* Width 64-align for HW block size. Height: leave EXACTLY as the
	 * coded picture height — HW writes UV plane at byte (Y_stride *
	 * reg70_height) where reg70 uses coded height. Any V4L2 height
	 * alignment makes ffmpeg/hwdownload read UV from a different offset
	 * than HW writes. Tested: ALIGN(16) breaks 1080p / 360p (heights not
	 * 16-aligned); only worked accidentally for 720p (1280×720 — 720 IS
	 * 16-aligned). NO alignment matches every height.
	 */
	fmt->width  = ALIGN(fmt->width,  64);
	/* fmt->height left unchanged */
	return 0;
}

/* -----------------------------------------------------------------------
 * Session start / stop
 */
static int rkvdec_vdpu383_vp9_start(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx;
	int ret;

	rkvdec->accum_vp9_start++;

	/*
	 * 2026-06-01 R24 experiment REVERTED. Per-session re-warmup helped
	 * YouTube 1080p marginally (1.7% → 3.2%) but YouTube content
	 * regardless of tile config (tested 240p untiled = 0.4%) still
	 * fails, confirming it's a content-property issue not tile-specific.
	 * The probe-time R23 warmup is sufficient for Fluster tile content.
	 * YouTube needs separate investigation. Revert here to keep the
	 * simpler R23-only model as production baseline.
	 */

	vp9_ctx = kzalloc(sizeof(*vp9_ctx), GFP_KERNEL);
	if (!vp9_ctx)
		return -ENOMEM;

	vp9_ctx->priv_tbl.size = sizeof(struct rkvdec_vdpu383_vp9_priv_tbl);
	vp9_ctx->priv_tbl.cpu  = dma_alloc_coherent(rkvdec->dev,
						     vp9_ctx->priv_tbl.size,
						     &vp9_ctx->priv_tbl.dma,
						     GFP_KERNEL);
	if (!vp9_ctx->priv_tbl.cpu) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	vp9_ctx->count_tbl.size = RKVDEC_VP9_COUNT_SIZE;
	vp9_ctx->count_tbl.cpu  = dma_alloc_coherent(rkvdec->dev,
						      vp9_ctx->count_tbl.size,
						      &vp9_ctx->count_tbl.dma,
						      GFP_KERNEL);
	if (!vp9_ctx->count_tbl.cpu) {
		ret = -ENOMEM;
		goto err_free_priv_tbl;
	}

	vp9_ctx->gbl_buf.size = VDPU383_VP9_GBL_SIZE;
	vp9_ctx->gbl_buf.cpu  = dma_alloc_coherent(rkvdec->dev,
						    vp9_ctx->gbl_buf.size,
						    &vp9_ctx->gbl_buf.dma,
						    GFP_KERNEL);
	if (!vp9_ctx->gbl_buf.cpu) {
		ret = -ENOMEM;
		goto err_free_count_tbl;
	}

	init_probs(vp9_ctx);

	/* Allocate the LINK descriptor ring. BSP typically runs with
	 * 16-32 slots; HW's prefetcher may stall on a shallow ring. */
	spin_lock_init(&ctx->inflight_lock);
	ctx->inflight_head = 0;
	ctx->inflight_tail = 0;

	ret = rkvdec_link_alloc_table(&vp9_ctx->link_table, rkvdec->dev,
				      &rkvdec_link_vdpu383_info,
				      /* task_capacity */ 16);
	if (ret)
		goto err_free_gbl_buf;
	ctx->link_table = &vp9_ctx->link_table;

	/*
	 * Lead 2 (§7c) NEGATIVE 2026-06-02: pinning decoder clocks to max
	 * (BSP CLK_MODE_ADVANCED analog) made link mode ALL-BAD, both
	 * per-frame and once-per-session. High clock is harmful to the link
	 * state machine here, not the cure. Do not re-add.
	 */

	ctx->priv = vp9_ctx;
	return 0;

err_free_gbl_buf:
	dma_free_coherent(rkvdec->dev, vp9_ctx->gbl_buf.size,
			  vp9_ctx->gbl_buf.cpu, vp9_ctx->gbl_buf.dma);
err_free_count_tbl:
	dma_free_coherent(rkvdec->dev, vp9_ctx->count_tbl.size,
			  vp9_ctx->count_tbl.cpu, vp9_ctx->count_tbl.dma);
err_free_priv_tbl:
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);
err_free_ctx:
	kfree(vp9_ctx);
	return ret;
}

static void rkvdec_vdpu383_vp9_stop(struct rkvdec_ctx *ctx)
{
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct vb2_v4l2_buffer *stuck_src, *stuck_dst;

	rkvdec->accum_vp9_stop++;
	/*
	 * 2026-05-30 Round 2.5: link-register snapshot at vp9_stop.
	 *
	 * Earlier (Round 2.4 retry) we discovered vp9_stop is ALSO in
	 * pm_runtime-suspended state by the time it's called from
	 * userspace stop_streaming ioctl (autosuspend has fired since
	 * the last decode's job_finish). pm_runtime_get_if_in_use
	 * returns 0 here too, so we have to explicitly resume to get
	 * a valid MMIO read window.
	 *
	 * pm_runtime_resume_and_get triggers a resume if currently
	 * suspended, returns 0 on success. Side effect: brief
	 * clock-on cycle (~ms). Acceptable for diagnostic purposes.
	 *
	 * Snapshot of all link registers gives us a per-session HW
	 * end-state baseline on safe content. Non-zero residue here
	 * would point at state that survives the previous decode's
	 * autosuspend cycle — candidate for the actual fp-tile
	 * accumulation-bug mechanism.
	 */
	if (rkvdec->link) {
		int ret = pm_runtime_resume_and_get(rkvdec->dev);

		if (!ret) {
			u32 dec_enable = readl_relaxed(rkvdec->link + 0x040);
			u32 link_cfg   = readl_relaxed(rkvdec->link + 0x004);
			u32 link_mode  = readl_relaxed(rkvdec->link + 0x008);
			u32 link_dnum  = readl_relaxed(rkvdec->link + 0x010);
			u32 link_tnum  = readl_relaxed(rkvdec->link + 0x014);
			u32 link_en    = readl_relaxed(rkvdec->link + 0x018);
			u32 int_en     = readl_relaxed(rkvdec->link + 0x048);
			u32 sta_int    = readl_relaxed(rkvdec->link + 0x04c);
			u32 ip_en      = readl_relaxed(rkvdec->link + 0x058);

			if (dec_enable & VDPU383_DEC_E_BIT)
				rkvdec->accum_busy_at_vp9_stop++;

			dev_info(rkvdec->dev,
				 "link@vp9_stop: dec_en=0x%08x cfg=0x%08x mode=0x%08x dnum=0x%08x tnum=0x%08x link_en=0x%08x int_en=0x%08x sta=0x%08x ip_en=0x%08x\n",
				 dec_enable, link_cfg, link_mode, link_dnum,
				 link_tnum, link_en, int_en, sta_int, ip_en);
			pm_runtime_put_autosuspend(rkvdec->dev);
		} else {
			rkvdec->accum_suspended_at_vp9_stop++;
		}
	}

	/*
	 * Phase 3 v0.3 step 2.4: drain any leftover inflight entries with
	 * VB2_BUF_STATE_ERROR. Detlev patch 6 (vb2_wait_for_all_buffers in
	 * rkvdec_stop_streaming) handles the normal case where HW finishes
	 * everything before teardown, but if userspace closes the fd
	 * mid-decode the inflight ring can still hold a (src, dst) pair
	 * that vp9_run kicked but no IRQ/watchdog ever consumed.
	 */
	while (rkvdec_inflight_depth(ctx) > 0) {
		struct rkvdec_link_inflight e = rkvdec_inflight_pop(ctx);

		if (!e.src || !e.dst)
			break;
		v4l2_m2m_buf_done(e.src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(e.dst, VB2_BUF_STATE_ERROR);
		/*
		 * Model flip (LINK_MODE_PORT_DESIGN_2026-06-01 §6.3): each
		 * submit took a pm ref in rkvdec_device_run that the watchdog
		 * reap normally drops. A task still parked here at teardown
		 * never reached reap, so drop its ref now to keep pm balanced.
		 */
		pm_runtime_put_autosuspend(rkvdec->dev);
	}

	/*
	 * Single-shot path drain: the inflight ring is only populated in
	 * link mode. For the default single-shot path, vp9_run() kicks HW
	 * and waits for IRQ — if HW silently hangs (observed on
	 * vp90-2-08-tile_1x2_frame_parallel and other tile-with-frame-
	 * parallel content 2026-05-30), no IRQ fires, vp9_done is never
	 * called, and m2m's "currently active" job stays open. That makes
	 * the subsequent vb2_wait_for_all_buffers() in rkvdec_stop_streaming
	 * hang in D-state, which in turn blocks rmmod and requires a
	 * power-cycle to recover.
	 *
	 * Force-complete any m2m-tracked source/destination buffer that's
	 * still hanging. v4l2_m2m_next_src_buf / _next_dst_buf return the
	 * current head of the pending queues — when present we mark them
	 * ERROR so vb2's done_wq fires and the wait can complete.
	 */
	while ((stuck_src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(stuck_src, VB2_BUF_STATE_ERROR);
	while ((stuck_dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(stuck_dst, VB2_BUF_STATE_ERROR);

	/*
	 * 2026-05-30 link mode investigation R2: Targeted fix for the
	 * link-mode-only state-accumulation bug. Before freeing the
	 * descriptor table, write 0 to LINK_CFG_ADDR (offset 0x004 in
	 * the link register block, link-mode-only — does NOT overlap
	 * with single-shot's kick registers at 0x40-0x58).
	 *
	 * Hypothesis: HW link block's prefetcher may continue issuing
	 * reads at the old descriptor IOVA after our enqueue completes.
	 * If we dma_free_coherent the descriptor while the prefetcher
	 * is mid-flight, the next ctx gets a new descriptor at a
	 * reused IOVA but HW may still hold stale fetched bytes from
	 * the previous ctx. Writing CFG_ADDR=0 first tells HW to stop
	 * fetching from the old IOVA.
	 *
	 * Single-shot is unaffected: it never writes CFG_ADDR (uses
	 * DEC_E at 0x40 instead). LINK_INT_EN / LINK_IP_ENABLE are
	 * left alone — those are shared between modes.
	 */
	writel(0, rkvdec->link + 0x004);
	wmb();

	/*
	 * 2026-05-30 Round 2.1 attempt: ported soft-reset + iommu_restore
	 * from the IRQ error path here, hoping to fix the accumulation
	 * bug. REVERTED — crashed even on a SINGLE-vector run, not just
	 * the multi-vector reproducer.
	 *
	 * Diagnosis: at clean session close HW is already idle. The
	 * soft-reset poll for STA_INT BIT(11) doesn't fire (no actual
	 * reset to acknowledge), we proceed past it, then iommu_restore's
	 * empty-domain attach detaches the default domain while the
	 * prefetcher / posted-write window is still draining. That
	 * produces the same "page fault while iommu not attached to
	 * domain" oops as the accumulation bug.
	 *
	 * The IRQ-path version (74af425) is safe because soft-reset there
	 * acts on a HW that's actively stuck (real_timeout / silent_
	 * completion), so the poll for BIT(11) succeeds.
	 *
	 * Next attempts will need a different primitive: either a "wait
	 * for HW idle" check before soft-reset, or move the fix
	 * elsewhere (start_streaming, capture-queue stop).
	 */

	ctx->link_table = NULL;
	rkvdec->link_ctx = NULL;
	rkvdec_link_free_table(&vp9_ctx->link_table);
	dma_free_coherent(rkvdec->dev, vp9_ctx->gbl_buf.size,
			  vp9_ctx->gbl_buf.cpu, vp9_ctx->gbl_buf.dma);
	dma_free_coherent(rkvdec->dev, vp9_ctx->count_tbl.size,
			  vp9_ctx->count_tbl.cpu, vp9_ctx->count_tbl.dma);
	dma_free_coherent(rkvdec->dev, vp9_ctx->priv_tbl.size,
			  vp9_ctx->priv_tbl.cpu, vp9_ctx->priv_tbl.dma);
	kfree(vp9_ctx);
	ctx->priv = NULL;
}

/* -----------------------------------------------------------------------
 * Post-frame probability update
 */
static void rkvdec_vdpu383_vp9_done(struct rkvdec_ctx *ctx,
				     struct vb2_v4l2_buffer *src_buf,
				     struct vb2_v4l2_buffer *dst_buf,
				     enum vb2_buffer_state result)
{
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx = ctx->priv;
	const struct v4l2_ctrl_vp9_frame *frame_params;
	struct v4l2_ctrl *ctrl;

	/* 2026-06-08 compound "why" probe: dump HW's post-decode adapted prob
	 * buffer (= what modes HW decoded) for the ours-vs-MPP compound diff. */
	if (vp9_adapt_dump && result == VB2_BUF_STATE_DONE) {
		struct rkvdec_vdpu383_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
		u32 cid = vp9_ctx->cur.frame_context_idx;
		static unsigned int adapt_run;

		if (tbl && cid < VP9_NUM_FRAME_CTX) {
			u8 *ap = tbl->probs.probs[cid];

			/* Adapted inter-mode region: comp_mode@116[5], comp_ref@121[5],
			 * single_ref@126[10] (Section-1=80B + y_mode 36B). These encode
			 * whether HW decoded compound vs single-ref blocks. Compare the
			 * compound frame (refmode=2) vs the KEY (refmode=0, both ctx0):
			 * KEY is intra so doesn't touch inter probs -> its values are the
			 * ctx0 input; the compound frame's delta from it = what HW decoded.
			 */
			pr_info("rkvdec-vp9 adapt-dump: run=%u ctx=%u refmode=%u crc=%08x comp_mode@116=%*ph | comp_ref+single@121=%*ph\n",
				adapt_run, cid, vp9_ctx->cur.reference_mode,
				crc32(0, ap, RKVDEC_VP9_PROBE_SIZE),
				5, ap + 116, 15, ap + 121);
		}
		adapt_run++;
	}

	/*
	 * 2026-05-31 Round 7 diagnostic: dump dst buffer bytes for
	 * tiny-frame (02-size cluster) content.
	 *
	 * The original Round-7 conclusion ("HW decodes 8x8 PERFECTLY, the
	 * 02-size failures are purely a userspace bytesperline-padding
	 * comparator issue") was WRONG — it only inspected the KEY/intra
	 * frame's buffer.
	 *
	 * 2026-06-05 CORRECTION (conformance_tail_2026-06-05.md): decoding
	 * vp90-2-02-size-08x08 and diffing per-frame vs libvpx shows frame 0
	 * (KEY) byte-perfect but frames 1-9 (single-ref INTER, refmode=0,
	 * NO timeout, NOT compound) 79-92/96 bytes WRONG. So there is a real
	 * small-dimension single-ref INTER decode bug: the intra path is
	 * correct, the inter motion-compensation path produces wrong pixels at
	 * small dimensions (BBB single-ref inter at 1080p is byte-perfect).
	 * Leading hypothesis: inter MC reads references with a wrong stride /
	 * edge-extension when HW-aligned bytesperline (e.g. 192 for an 8-px
	 * row) diverges sharply from the visible width. The non-aligned-width
	 * size vectors (34/66-wide) ALSO show output bytesperline padding (a
	 * separate de-stride/crop gap). Both are open driver-side items.
	 *
	 * Diagnostic kept in tree gated on width<=64 + static count<1
	 * so it only fires once per module load when 02-size content
	 * is decoded.
	 */
	if (result == VB2_BUF_STATE_DONE && dst_buf &&
	    (ctx->decoded_fmt.fmt.pix_mp.width <= 64 ||
	     ctx->decoded_fmt.fmt.pix_mp.width == 384 ||
	     ctx->decoded_fmt.fmt.pix_mp.width == 256 ||
	     ctx->decoded_fmt.fmt.pix_mp.width == 448)) {
		dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(
				&dst_buf->vb2_buf, 0);
		size_t plane_sz = vb2_plane_size(&dst_buf->vb2_buf, 0);
		u32 bpl = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		u32 fmth = ctx->decoded_fmt.fmt.pix_mp.height;
		size_t y_sz = (size_t)bpl * fmth;
		size_t need = y_sz + bpl * fmth / 2;
		struct iommu_domain *dom =
			iommu_get_domain_for_dev(ctx->dev->dev);
		phys_addr_t ph = dom ?
			iommu_iova_to_phys(dom, dst_dma) : 0;
		void *cpu = (ph && plane_sz >= need) ?
			memremap(ph, need, MEMREMAP_WB) : NULL;
		static u32 dump_count;

		if (cpu && dump_count < 1) {
			u32 row;
			dump_count++;
			pr_info("R7DUMP iova=%pad fmt=%ux%u bpl=%u y_sz=%zu sz=%zu\n",
				&dst_dma,
				ctx->decoded_fmt.fmt.pix_mp.width,
				fmth, bpl, y_sz, plane_sz);
			/* First 16 bytes of each Y row (up to row 80 = 5 SB-rows) */
			for (row = 0; row < fmth && row < 80; row++) {
				char prefix[16];
				snprintf(prefix, sizeof(prefix), "R7Y r%02u: ", row);
				print_hex_dump(KERN_INFO, prefix,
					       DUMP_PREFIX_NONE, 16, 1,
					       (char *)cpu + row * bpl, 16, false);
			}
			/* First 16 bytes of each UV row (up to row 40 = 5 SB-rows worth) */
			for (row = 0; row < fmth/2 && row < 40; row++) {
				char prefix[16];
				snprintf(prefix, sizeof(prefix), "R7UV r%02u: ", row);
				print_hex_dump(KERN_INFO, prefix,
					       DUMP_PREFIX_NONE, 16, 1,
					       (char *)cpu + y_sz + row * bpl, 16, false);
			}
			memunmap(cpu);
		}
	}

#if VP9_DUMP_REGS
	/*
	 * Post-decode peek: read the first 64 bytes of the dst capture
	 * buffer directly from kernel via memremap. Distinguishes the
	 * silent-fail hypothesis between "HW wrote nothing (buffer is
	 * still zero)" vs "HW wrote actual data that ended up identical
	 * to a previous frame's content" vs "buffer aliases another
	 * physical region".
	 *
	 * Done in vp9_done() AFTER the HW IRQ has fired, so the buffer
	 * content reflects HW's actual writeback (or lack thereof).
	 */
	if (vp9_dump_frame_no >= 1 && vp9_dump_frame_no <= 3 && dst_buf) {
		dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(
				&dst_buf->vb2_buf, 0);
		size_t plane_sz = vb2_plane_size(&dst_buf->vb2_buf, 0);
		struct iommu_domain *dom =
			iommu_get_domain_for_dev(ctx->dev->dev);
		phys_addr_t ph = dom ?
			iommu_iova_to_phys(dom, dst_dma) : 0;
		void *cpu = (ph && plane_sz >= 64) ?
			memremap(ph, 64, MEMREMAP_WB) : NULL;

		if (cpu) {
			pr_info("VP9DUMP Frame%04u post_decode_dst iova=%pad 64B\n",
				vp9_dump_frame_no, &dst_dma);
			print_hex_dump(KERN_INFO,
				       "VP9DUMP-POSTDST: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       cpu, 64, false);
			memunmap(cpu);
		}
	}
#endif

	/*
	 * Always propagate `cur` to `last` so the next frame's setup has
	 * usable previous-frame context, even when the current decode
	 * failed.  Matches MPP's behavior which updates ls_info
	 * unconditionally at the end of `hal_vp9d_vdpu383_gen_regs`.
	 *
	 * Previously this update was gated on `result == DONE`, so on a
	 * failed frame N our `last` would still reflect frame N-1's info
	 * when frame N+1 ran.  V4L2's per-frame control supplies frame N+1
	 * with reference timestamps that point at frame N's output buffer
	 * (now stale or partial), but our internal `last_*` tracking
	 * pointed two frames back — divergence between V4L2 references
	 * and our `last` state.
	 *
	 * The prob_ctx_valid update IS still gated on success, because
	 * we don't want to claim HW wrote adapted probs if it didn't.
	 */

	if (result == VB2_BUF_STATE_DONE && dst_buf) {
		/*
		 * 2026-05-31 Round 15: stash this frame's timestamp + DMA in
		 * our ref_ring so future frames whose v4l2_ctrl_vp9_frame
		 * references this timestamp can recover the address even
		 * when vb2_find_buffer fails. Slot rotates round-robin
		 * through 8 entries — enough for VP9's 3 active refs plus
		 * transient mismatches.
		 */
		u32 slot = vp9_ctx->ref_ring_next %
			   ARRAY_SIZE(vp9_ctx->ref_ring);

		vp9_ctx->ref_ring[slot].timestamp =
			dst_buf->vb2_buf.timestamp;
		vp9_ctx->ref_ring[slot].dma =
			vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
		/* #5a: record the just-decoded frame's coded dims for later
		 * per-ref GBL dimension lookup. */
		vp9_ctx->ref_ring[slot].width  = vp9_ctx->cur.width;
		vp9_ctx->ref_ring[slot].height = vp9_ctx->cur.height;
		vp9_ctx->ref_ring[slot].valid = true;
		vp9_ctx->ref_ring_next++;
	}

	if (result == VB2_BUF_STATE_DONE) {
		ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
				      V4L2_CID_STATELESS_VP9_FRAME);
		if (!WARN_ON(!ctrl)) {
			frame_params = ctrl->p_cur.p;
			if (frame_params->flags &
			    V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX)
				vp9_ctx->prob_ctx_valid[
					vp9_ctx->cur.frame_context_idx] = 1;

			/*
			 * R49 (2026-06-02) — update last-shown-INTER colmv
			 * tracking. MPP HAL pre_mv_base_addr semantics:
			 * carry forward when current is INTER + not
			 * intra_only + not error_resilient + last_show_frame
			 * was set (hal_vp9d_vdpu383.c:469-472). For our
			 * single-shot, the cleanest equivalent is: if
			 * current was visible INTER (SHOW_FRAME set + not
			 * KEY + not INTRA_ONLY), stash its colmv as the
			 * carry-forward value for future alt-refs.
			 */
			if (dst_buf &&
			    (frame_params->flags &
			     V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
			    !(frame_params->flags &
			      V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
			    !(frame_params->flags &
			      V4L2_VP9_FRAME_FLAG_INTRA_ONLY) &&
			    !(frame_params->flags &
			      V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT)) {
				vp9_ctx->last_shown_inter_colmv_dma =
					vb2_dma_contig_plane_dma_addr(
						&dst_buf->vb2_buf, 0) +
					ctx->colmv_offset;
				vp9_ctx->last_shown_inter_colmv_valid = true;
			}
		}
	} else if (r43_invalidate_probs_on_fail) {
		/*
		 * R43 (2026-06-01) — on decode failure (silent hang or
		 * error), HW may have partially written to prob_loop
		 * before stalling. Subsequent frames using that
		 * frame_context_idx would read garbage adapted probs,
		 * propagating the failure.
		 *
		 * Invalidate this ctx's prob_loop so the next decode
		 * using it reads from prob_default (the pristine
		 * defaults) instead of potentially-corrupted prob_loop.
		 *
		 * This is one of MPP HAL's behaviors that we don't
		 * mirror: MPP also invalidates prob_ctx_valid on intra
		 * frames and error_resilient_mode (hal_vp9d_vdpu383.c
		 * line 373-381), preventing prob_loop garbage from
		 * propagating across stream-state changes.
		 */
		vp9_ctx->prob_ctx_valid[vp9_ctx->cur.frame_context_idx] = 0;
	}

	/*
	 * Bug-A post-decode fix: HW has finished by the time done()
	 * runs. If this frame was flagged by run() as a tiny show-alt-ref
	 * candidate (bug_a_copy_sz != 0), the silicon wrote zeros to dst
	 * instead of alt-ref pixels. Overwrite the zeros with alt-ref
	 * content via PL330 hardware DMA.
	 *
	 * Only fires on result==DONE — if the decode itself failed we
	 * shouldn't touch the buffer.
	 */
	if (result == VB2_BUF_STATE_DONE && vp9_ctx->cur.bug_a_copy_sz) {
		int rc = rkvdec_bug_a_copy(ctx->dev,
					   vp9_ctx->cur.bug_a_dst_dma,
					   vp9_ctx->cur.bug_a_alt_dma,
					   vp9_ctx->cur.bug_a_copy_sz);

		/*
		 * NOTE: PL330 has DMA_BIT_MASK(32) hardcoded. RK3576 has
		 * 5GB DRAM (System RAM 0x40200000-0x13fffffff) and V4L2
		 * CAPTURE buffers routinely land above 4GB. rc may report
		 * DMA_COMPLETE while the write silently went nowhere.
		 *
		 * See ISSUE_1_DMAENGINE_VALIDATED_2026-05-24.md for the
		 * full validation log and next-attempt options.
		 */
		pr_info_ratelimited("rkvdec-vp9 bug-a copy(post): copy_sz=%zu rc=%d\n",
				    vp9_ctx->cur.bug_a_copy_sz, rc);
	}

	/*
	 * Phase 12: post-decode full MMIO read-back. Dumps all
	 * registers that HW could touch (reg0-reg359) so we can compare
	 * against MPP's get_regs (which shows post-decode state from
	 * the SAME silicon). If a register HW wrote differs between
	 * MPP and us, that's a HW-state signal we can chase.
	 *
	 * Format matches MPP's `get regs[NN]: XXXXXXXX` so the two
	 * dumps can be directly diffed.
	 */
	/*
	 * Phase 12 (FALSIFIED — CRASHED unit #1): post-decode readback
	 * of full MMIO bank (reg0-359) caused NULL-pointer deref in
	 * vdpu383_irq_handler after module unload, requiring a SysRq
	 * reboot. Most likely cause: reading reg320-359 (statistic_regs)
	 * accesses MMIO offsets that are either reserved/unmapped or
	 * conflict with ongoing HW operations.
	 *
	 * Lesson learned: do NOT read arbitrary MMIO offsets in
	 * vp9_done(). Specifically avoid reg128+ (address regs HW may
	 * be actively using to drain the previous decode's writes) and
	 * reg320+ (statistic regs that might trigger HW reactions).
	 *
	 * If a future phase needs post-decode MMIO inspection, limit
	 * to reg8-30 (ctrl_regs) only and verify safety on a small
	 * test before iterating.
	 */

	/*
	 * Phase 3 v0.3 step 1 (2026-05-27): DPB state-advance moved from
	 * done() to vp9_run end. This is a prerequisite for pipelining —
	 * with depth>1 the next vp9_run runs BEFORE the previous IRQ, so
	 * `last` must reflect the previous frame at vp9_run time, not at
	 * IRQ time. For depth=1 the ordering is unchanged (run -> kick ->
	 * advance -> IRQ -> done -> next-run vs run -> kick -> IRQ -> done
	 * -> advance -> next-run): both end up with `last` = previous
	 * frame by the time the next run() reads it.
	 *
	 * State updates that stayed in done():
	 *   - prob_ctx_valid (gated on DONE result)
	 *   - bug-A copy (modifies the output buffer post-decode)
	 */
}

/* -----------------------------------------------------------------------
 * GBL buffer bit-packing (unchanged from prior version)
 */
struct vdpu383_gbl_packer {
	__le64 *buf;
	u32 idx;
};

static void gbl_put(struct vdpu383_gbl_packer *p, u64 val, u32 nbits)
{
	while (nbits) {
		u32 word = p->idx >> 6;
		u32 off  = p->idx & 63;
		u32 n    = min_t(u32, nbits, 64 - off);
		u64 mask = n == 64 ? ~0ULL : ((1ULL << n) - 1);

		p->buf[word] |= cpu_to_le64((val & mask) << off);
		val  >>= n;
		nbits -= n;
		p->idx += n;
	}
}

static void gbl_align64(struct vdpu383_gbl_packer *p)
{
	p->idx = ALIGN(p->idx, 64);
}

static const u8 vp9_interp_filter_hw[] = { 1, 0, 2, 3 };

static void vdpu383_vp9_config_global_hdr(
	struct rkvdec_ctx *ctx,
	const struct v4l2_ctrl_vp9_frame *fp,
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx)
{
	const struct rkvdec_vdpu383_vp9_frame_info *last = &vp9_ctx->last;
	struct vdpu383_gbl_packer bp = {
		.buf = (__le64 *)vp9_ctx->gbl_buf.cpu,
		.idx = 0,
	};

	(void)ctx;

	/*
	 * 2026-06-01 R33 result (reverted): MPP CLI packs Rick 720p
	 * 298-byte KEY as frame_type=1 / is_intra=0 in GBL. We tried
	 * matching for KEYs with strm_len < 512 — Rick regressed from
	 * 12% → 0% writeback with new silent_err_wb=12 failure mode.
	 * The byte-level diff has 7 mismatches; the bytes are tied
	 * together and a simple frame_type swap on its own breaks
	 * something else. Fluster 1x4 unaffected (its KEY is 62KB so
	 * stays in normal path). Needs more investigation — possibly
	 * the other 5 byte diffs encode "what kind of INTER" (ref
	 * dimensions, mvscale) that we'd need to also flip.
	 */
	bool is_intra = !!(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
			!!(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
	u32 frame_type = (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ? 0 : 1;
	u32 w = fp->frame_width_minus_1 + 1;
	u32 h = fp->frame_height_minus_1 + 1;
	/* Reference frame dimensions per VP9 ref slot (LAST, GOLDEN, ALTREF).
	 * KEY / INTRA_ONLY frames pack zeros for ref dimensions, matching the
	 * vendor driver — packing current-frame dims for unused refs makes the
	 * IP block treat them as live and can trigger spurious chroma-ref
	 * activity.
	 *
	 * #5a fix (2026-06-13): per-ref coded dimensions, looked up by each
	 * reference's timestamp against the ref_ring — the V4L2 analogue of
	 * MPP's ref_frame_coded_width[frame_refs[i].Index]. The old code used
	 * the *current* frame's dims for GOLDEN/ALTREF (and the immediately-
	 * previous frame's dims for LAST), which is correct only when every
	 * reference is the same resolution as the current frame. For
	 * scaled-reference frames that produced a wrong mvscale. The lookup
	 * falls back to the current dims when the ref isn't in the ring (or its
	 * dims are unknown), so same-resolution content is byte-identical to
	 * the old behaviour.
	 */
	u32 ref_w[3], ref_h[3];
	bool use_prev_mvs;
	bool last_wh_eq;
	int i, j, ri;

	for (ri = 0; ri < 3; ri++) {
		u64 ref_ts = (ri == 0) ? fp->last_frame_ts :
			     (ri == 1) ? fp->golden_frame_ts :
					 fp->alt_frame_ts;
		u32 rw = w, rh = h;
		u32 k;

		for (k = 0; k < ARRAY_SIZE(vp9_ctx->ref_ring); k++) {
			if (vp9_ctx->ref_ring[k].valid &&
			    vp9_ctx->ref_ring[k].timestamp == ref_ts &&
			    vp9_ctx->ref_ring[k].width) {
				rw = vp9_ctx->ref_ring[k].width;
				rh = vp9_ctx->ref_ring[k].height;
				break;
			}
		}
		ref_w[ri] = is_intra ? 0 : rw;
		ref_h[ri] = is_intra ? 0 : rh;
	}

	memset(vp9_ctx->gbl_buf.cpu, 0, VDPU383_VP9_GBL_SIZE);

	gbl_put(&bp, frame_type, 1);
	gbl_put(&bp, !!(fp->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT), 1);
	gbl_put(&bp, fp->bit_depth - 8, 3);
	gbl_put(&bp, 1, 2);
	gbl_put(&bp, w, 16);
	gbl_put(&bp, h, 16);

	gbl_put(&bp, is_intra ? 1 : 0, 1);
	/*
	 * V4L2 ref_frame_sign_bias is a BITMAP with:
	 *   V4L2_VP9_SIGN_BIAS_LAST   = 0x1  (bit 0)
	 *   V4L2_VP9_SIGN_BIAS_GOLDEN = 0x2  (bit 1)
	 *   V4L2_VP9_SIGN_BIAS_ALT    = 0x4  (bit 2)
	 *
	 * MPP's reference reads `pp->ref_frame_sign_bias[1..3]` because
	 * DXVA's PicParams uses an ARRAY indexed 1=LAST, 2=GOLDEN,
	 * 3=ALTREF (index 0 = INTRA_FRAME, unused).  When we ported from
	 * MPP we kept the array indices instead of switching to V4L2's
	 * bit positions — that produces wrong sign-bias values for any
	 * frame where ALTREF is sign-flipped (very common in alt-ref-
	 * using content), which manifests as sta=0x02 strm_error.
	 */
	gbl_put(&bp, !!(fp->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_LAST), 1);
	gbl_put(&bp, !!(fp->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_GOLDEN), 1);
	gbl_put(&bp, !!(fp->ref_frame_sign_bias & V4L2_VP9_SIGN_BIAS_ALT), 1);

	gbl_put(&bp, !!(fp->flags & V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV), 1);
	if (!frame_type || is_intra) {
		gbl_put(&bp, 0, 3);
	} else {
		/*
		 * V4L2 `interpolation_filter` is the FILTER VALUE (per VP9
		 * spec section 7.3): 0=EIGHTTAP, 1=EIGHTTAP_SMOOTH,
		 * 2=EIGHTTAP_SHARP, 3=BILINEAR, 4=SWITCHABLE.
		 *
		 * MPP's reference uses `literal_to_filter[pp->interp_filter]`
		 * because DXVA's `pp->interp_filter` carries the raw 2-bit
		 * LITERAL from the bitstream, which VP9 spec then maps via
		 * literal_to_filter = {EIGHTTAP_SMOOTH, EIGHTTAP,
		 * EIGHTTAP_SHARP, BILINEAR} to the filter value.
		 *
		 * V4L2 has already applied that conversion, so we pack the
		 * filter value directly.  Our earlier code re-applied the
		 * literal_to_filter LUT to V4L2's already-converted value,
		 * producing the wrong filter type for any non-SWITCHABLE
		 * frame (e.g. EIGHTTAP_SMOOTH would become EIGHTTAP).
		 * Wrong sub-pixel filter → wrong reconstruction →
		 * sta=0x02 strm_error on the next entropy-coded symbol.
		 */
		gbl_put(&bp, fp->interpolation_filter, 3);
	}
	gbl_put(&bp, !!(fp->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE), 1);
	gbl_put(&bp, !!(fp->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX), 1);

	gbl_put(&bp, fp->lf.level, 6);
	gbl_put(&bp, fp->lf.sharpness, 3);
	gbl_put(&bp, !!(fp->lf.flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED), 1);
	gbl_put(&bp, !!(fp->lf.flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE), 1);
	for (i = 0; i < 4; i++)
		gbl_put(&bp, (u32)(s32)fp->lf.ref_deltas[i] & 0x7f, 7);
	for (i = 0; i < 2; i++)
		gbl_put(&bp, (u32)(s32)fp->lf.mode_deltas[i] & 0x7f, 7);

	gbl_put(&bp, fp->quant.base_q_idx, 8);
	gbl_put(&bp, (u32)(s32)fp->quant.delta_q_y_dc & 0x1f, 5);
	gbl_put(&bp, (u32)(s32)fp->quant.delta_q_uv_dc & 0x1f, 5);
	gbl_put(&bp, (u32)(s32)fp->quant.delta_q_uv_ac & 0x1f, 5);
	gbl_put(&bp,
		(!fp->quant.base_q_idx && !fp->quant.delta_q_y_dc &&
		 !fp->quant.delta_q_uv_dc && !fp->quant.delta_q_uv_ac) ? 1 : 0,
		1);

	/* When segmentation is disabled, V4L2 leaves pred_probs / tree_probs
	 * at 0xff (their reset state); the IP block expects 0 in that case.
	 * Forcing zero matches the vendor driver and avoids INTER-frame
	 * entropy drift.
	 */
	{
		bool seg_on = !!(fp->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED);

		for (i = 0; i < 3; i++)
			gbl_put(&bp, seg_on ? fp->seg.pred_probs[i] : 0, 8);
		for (i = 0; i < 7; i++)
			gbl_put(&bp, seg_on ? fp->seg.tree_probs[i] : 0, 8);
	}
	gbl_put(&bp, !!(fp->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED), 1);
	gbl_put(&bp, !!(fp->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP), 1);
	gbl_put(&bp,
		!!(fp->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE), 1);
	gbl_put(&bp,
		!!(fp->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE),
		1);

	use_prev_mvs = !!(fp->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) == 0 &&
		       w == last->width && h == last->height &&
		       !(last->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
		       !(last->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY) &&
		       !!(last->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
		       last->valid;
	gbl_put(&bp, use_prev_mvs ? 1 : 0, 1);

	for (i = 0; i < 8; i++)
		for (j = 0; j < 4; j++)
			gbl_put(&bp, (fp->seg.feature_enabled[i] >> j) & 1, 1);

	for (i = 0; i < 8; i++) {
		gbl_put(&bp, (u32)(s32)fp->seg.feature_data[i][0] & 0x1ff, 9);
		gbl_put(&bp, (u32)(s32)fp->seg.feature_data[i][1] & 0x7f, 7);
		gbl_put(&bp, fp->seg.feature_data[i][2] & 0x3, 2);
	}

	gbl_put(&bp, fp->compressed_header_size, 16);

	for (i = 0; i < 3; i++) {
		gbl_put(&bp, ref_w[i], 16);
		gbl_put(&bp, ref_h[i], 16);
	}

	for (i = 0; i < 2; i++)
		gbl_put(&bp, (u32)(s32)last->lf.mode_deltas[i] & 0x7f, 7);
	for (i = 0; i < 4; i++)
		gbl_put(&bp, (u32)(s32)last->lf.ref_deltas[i] & 0x7f, 7);
	gbl_put(&bp,
		!!(last->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED), 1);
	gbl_put(&bp, !!(last->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
			last->valid ? 1 : 0, 1);
	gbl_put(&bp, !!(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY), 1);
	last_wh_eq = last->valid && last->width == w && last->height == h;
	gbl_put(&bp, last_wh_eq ? 1 : 0, 1);
	/*
	 * 2026-05-31 R18: pack last frame's color_space (3 bits) here,
	 * matching MPP's ls_info.color_space_last. We previously hardcoded
	 * 0 which was correct only for CS_UNKNOWN content (sintel, most
	 * test vectors). For real-world BT709/BT601/BT2020 (YouTube etc)
	 * this mismatched MPP. See rkvdec_vdpu383_vp9_run for parse.
	 */
	gbl_put(&bp, last->color_space & 0x7, 3);
	gbl_put(&bp,
		(!last->valid || (last->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME))
		? 1 : 0, 1);
	gbl_put(&bp, 0, 1);
	gbl_put(&bp, 1, 1);
	gbl_put(&bp, 1, 1);
	gbl_put(&bp, 1, 1);

	/* mvscale[3][2] — Q14 fixed-point ratio of ref_dim to cur_dim.
	 * MPP packs `pp->mvscale[i][j]` from DXVA, computed per VP9 spec §7.3.4:
	 *   mvscale[i][0] = (ref_width[i]  << 14) / cur_width
	 *   mvscale[i][1] = (ref_height[i] << 14) / cur_height
	 * For unscaled refs (same dim) the value is 0x4000 (= 1.0 Q14).
	 * Packing zeros tells HW "scale by 0" → every motion vector becomes
	 * (0,0), so MC reads from the wrong location and INTER blocks get
	 * the chroma fill colour instead of real reference content.
	 */
	for (i = 0; i < 3; i++) {
		/* KEY frames have no valid refs, so mvscale entries are unused.
		 * The vendor driver fills them with 16 (a sentinel; the IP block
		 * uses this as "no scaling needed"). Packing 0 here makes the IP
		 * block treat motion vectors as scale-by-zero, producing chroma
		 * artefacts on any subsequent INTER decode.
		 */
		u32 sx = (is_intra || !w) ? 16 :
			 (u32)(((u64)ref_w[i] << 14) / w);
		u32 sy = (is_intra || !h) ? 16 :
			 (u32)(((u64)ref_h[i] << 14) / h);

		gbl_put(&bp, sx & 0xffff, 16);
		gbl_put(&bp, sy & 0xffff, 16);
	}

	{
		u32 tile_cols = 1u << fp->tile_cols_log2;
		u32 tile_rows = 1u << fp->tile_rows_log2;
		u32 sb_cols   = ALIGN(w, 64) / 64;
		u32 sb_rows   = ALIGN(h, 64) / 64;

		gbl_put(&bp, tile_cols, 7);
		gbl_put(&bp, tile_rows, 3);

		for (i = 0; i < 64; i++) {
			u32 start = ((u32)i * sb_cols) >> fp->tile_cols_log2;
			u32 end   = ((u32)(i + 1) * sb_cols) >> fp->tile_cols_log2;
			u32 tw    = min(end, sb_cols) - min(start, sb_cols);

			gbl_put(&bp, tw, 10);
		}
		for (j = 0; j < 4; j++) {
			u32 start = ((u32)j * sb_rows) >> fp->tile_rows_log2;
			u32 end   = ((u32)(j + 1) * sb_rows) >> fp->tile_rows_log2;
			u32 th    = min(end, sb_rows) - min(start, sb_rows);

			gbl_put(&bp, th, 10);
		}
	}

	gbl_align64(&bp);
}

/* -----------------------------------------------------------------------
 * Reference-frame DMA lookup by timestamp.
 */
static dma_addr_t vp9_ref_dma(struct rkvdec_ctx *ctx,
			       struct vb2_v4l2_buffer *fallback, u64 ts)
{
	struct vb2_buffer *buf =
		vb2_find_buffer(&ctx->fh.m2m_ctx->cap_q_ctx.q, ts);
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx;
	u32 i;

	ctx->dev->telem_ref_lookup_total++;

	if (buf)
		return vb2_dma_contig_plane_dma_addr(buf, 0);

	/*
	 * 2026-05-31 Round 15.1: vb2 lookup missed. Search our own ring
	 * of last-N decoded frames. Two-stage fallback:
	 *   1. Exact timestamp match in ring → use that
	 *   2. No match → use the MOST RECENT ring entry (better than
	 *      returning dst_buf which would have HW read the current
	 *      decoding buffer as reference)
	 *   3. Empty ring → fall back to dst_buf (legacy bad behaviour,
	 *      only happens on first KEY frame which has no actual refs)
	 */
	vp9_ctx = ctx->priv;
	if (vp9_ctx) {
		u32 most_recent_slot = 0xffffffff;
		u64 most_recent_seq = 0;

		for (i = 0; i < ARRAY_SIZE(vp9_ctx->ref_ring); i++) {
			if (!vp9_ctx->ref_ring[i].valid)
				continue;
			if (vp9_ctx->ref_ring[i].timestamp == ts)
				return vp9_ctx->ref_ring[i].dma;
		}

		/* Find most recent (highest timestamp). */
		for (i = 0; i < ARRAY_SIZE(vp9_ctx->ref_ring); i++) {
			if (vp9_ctx->ref_ring[i].valid &&
			    vp9_ctx->ref_ring[i].timestamp > most_recent_seq) {
				most_recent_seq = vp9_ctx->ref_ring[i].timestamp;
				most_recent_slot = i;
			}
		}

		if (most_recent_slot != 0xffffffff) {
			ctx->dev->telem_ref_lookup_fallback++;
			return vp9_ctx->ref_ring[most_recent_slot].dma;
		}
	}

	/* Empty ring (first KEY frame) → legacy dst_buf fallback. */
	ctx->dev->telem_ref_lookup_fallback++;
	return vb2_dma_contig_plane_dma_addr(&fallback->vb2_buf, 0);
}

/* -----------------------------------------------------------------------
 * Build the four register regions in vp9_ctx->regs.
 *
 * Mirrors the structure of mainline rkvdec-vdpu383-hevc.c::config_registers():
 * memset the whole struct to zero, then populate only the fields VP9 needs.
 * All other registers (rps, scaling, scl_ref strides, ref3..ref7 strides,
 * fbc_e/tile_e in reg009) stay at zero — same as HEVC/H.264 do for the
 * fields they don't use.
 *
 * NOTE: reg009_important_en stays at zero. The pre-refactor workaround
 * (BIT(4) | BIT(7)) was masking incoherent COMMON-region writes from the
 * old individual-writel() approach. Mainline HEVC and H.264 leave reg009
 * at zero and the IP block accepts it.
 */
static void vdpu383_vp9_build_regs(
	struct rkvdec_ctx *ctx,
	const struct v4l2_ctrl_vp9_frame *fp,
	struct vb2_v4l2_buffer *src_buf,
	struct vb2_v4l2_buffer *dst_buf,
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx)
{
	struct vdpu383_regs_vp9 *regs = &vp9_ctx->regs;
	const struct v4l2_pix_format_mplane *pfmt = &ctx->decoded_fmt.fmt.pix_mp;
	dma_addr_t priv_dma = vp9_ctx->priv_tbl.dma;
	dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	dma_addr_t src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	dma_addr_t last_dma, golden_dma, alt_dma, prev_colmv_dma;
	dma_addr_t segmap_last_dma, segmap_cur_dma;
	dma_addr_t lastprob_dma;
	u32 bytesperline = pfmt->plane_fmt[0].bytesperline;
	/*
	 * MPP aligns the coded height to 64 for HW Y-virstride. For
	 * heights ≤ 64 or already multiples of 64 this is a no-op.
	 * For heights like 196/198/210/226 (Fluster 03-size cluster)
	 * MPP rounds up: 196 → 256, 198 → 256, etc. HW writes UV at
	 * offset = aligned_height * bpl, so V4L2 sizeimage must use
	 * the SAME aligned height (handled in rkvdec_fill_decoded_pixfmt
	 * via ALIGN(pix_mp->height, 64)).
	 *
	 * Earlier session-15 comment said "MPP uses raw VP9 coded
	 * height" — that was tested only on 64-aligned content where
	 * raw == aligned. For non-aligned heights MPP rounds up to 64.
	 */
	u32 height       = ALIGN(fp->frame_height_minus_1 + 1, 8);
	u32 hw_hor       = bytesperline >> 4;
	u32 hw_y_stride  = hw_hor * height;

	pr_debug("rkvdec-vp9 fmt: width=%u height=%u bpl=%u sizeimage=%u hw_hor=0x%x hw_y_stride=0x%x\n",
		 pfmt->width, pfmt->height,
		 pfmt->plane_fmt[0].bytesperline,
		 pfmt->plane_fmt[0].sizeimage,
		 hw_hor, hw_y_stride);
	u32 strm_len, uncomp_len;
	u32 dst32        = lower_32_bits(dst_dma);
	u32 colmv_dst32  = lower_32_bits(dst_dma + ctx->colmv_offset);
	/* Use the helper-adjusted ctx idx (KEY frames forced to 0 per spec),
	 * not the raw fp->frame_context_idx — set in run() above.
	 */
	u8  frame_ctx_id = vp9_ctx->cur.frame_context_idx;
	int i;

	memset(regs, 0, sizeof(*regs));

	/* ---- COMMON region (reg8-30) ---- */
	regs->common.reg008_dec_mode = VDPU383_MODE_VP9;

	/* All ten block-gating enables, mirroring HEVC. */
	regs->common.reg010_block_gating_en.strmd_auto_gating_e   = 1;
	regs->common.reg010_block_gating_en.inter_auto_gating_e   = 1;
	regs->common.reg010_block_gating_en.intra_auto_gating_e   = 1;
	regs->common.reg010_block_gating_en.transd_auto_gating_e  = 1;
	regs->common.reg010_block_gating_en.recon_auto_gating_e   = 1;
	regs->common.reg010_block_gating_en.filterd_auto_gating_e = 1;
	regs->common.reg010_block_gating_en.bus_auto_gating_e     = 1;
	regs->common.reg010_block_gating_en.ctrl_auto_gating_e    = 1;
	regs->common.reg010_block_gating_en.rcb_auto_gating_e     = 1;
	regs->common.reg010_block_gating_en.err_prc_auto_gating_e = 1;

	/* Core-timeout threshold. Matches the vendor driver's conservative
	 * value; lower thresholds raise throughput but trigger spurious
	 * watchdogs on heavy-motion content.
	 */
	/*
	 * Core-timeout threshold. Matches the vendor driver's conservative
	 * value; lower thresholds raise throughput but trigger spurious
	 * watchdogs on heavy-motion content.
	 *
	 * R48 (2026-06-02): tested reg13 = 0xFFFFFFDF (matches MPP HAL).
	 * Result: 1/12 perfect vs ~17-25% baseline; some sessions tore
	 * down early. MPP's value HURTS in our context; reverted to
	 * 0xFFFFFF. The reg13 divergence is real but reg13 in isolation
	 * is not the bug — MPP's value is probably part of a larger
	 * coordinated register set we don't fully replicate.
	 * See SESSION_2026-06-02_MPP_HAL_DIFF_START.md.
	 */
	regs->common.reg013_core_timeout_threshold = 0xffffff;
	regs->common.reg016_error_ctrl_set.error_proc_disable = 1;

	/*
	 * Statistic / AXI tuning. Mirrors MPP's vdpu383_setup_statistic().
	 * reg29.addr_align_type=1 sets the address-alignment behaviour for
	 * AXI bursts, which the IP block's INTER-block decode path may
	 * depend on. Mainline HEVC/H.264 don't set these explicitly (they
	 * leave them at zero from memset), and HEVC/H.264 work — but VP9
	 * may differ. Include for parity with MPP.
	 */
	regs->common.reg028_debug_perf_latency_ctrl0.axi_perf_work_e = 1;
	regs->common.reg028_debug_perf_latency_ctrl0.axi_cnt_type    = 1;
	regs->common.reg028_debug_perf_latency_ctrl0.rd_latency_id   = 11;
	regs->common.reg029_debug_perf_latency_ctrl1.addr_align_type    = 1;
	regs->common.reg029_debug_perf_latency_ctrl1.ar_cnt_id_type     = 0;
	regs->common.reg029_debug_perf_latency_ctrl1.aw_cnt_id_type     = 1;
	regs->common.reg029_debug_perf_latency_ctrl1.ar_count_id        = 17;
	regs->common.reg029_debug_perf_latency_ctrl1.aw_count_id        = 0;
	regs->common.reg029_debug_perf_latency_ctrl1.rd_band_width_mode = 0;

	/*
	 * CABAC error-detection masks. Required: with these registers at 0
	 * the IP block treats every block as a CABAC error and fills it with
	 * a default value, leaving only a small region of real decode output
	 * in the top-left of the frame. Mainline HEVC/H.264 don't need these
	 * (their cabac_error masks live in shared common code we don't pull
	 * in); for VP9 we set them per MPP's vdpu383_init_ctrl_regs() values.
	 */
	regs->common.reg020_cabac_error_en_lowbits  = 0xffffffdf;
	regs->common.reg021_cabac_error_en_highbits = 0x3fffffff;

	/* ---- CODEC_PARAMS region (reg64-106) ---- */
	uncomp_len = fp->uncompressed_header_size;
	strm_len   = vb2_get_plane_payload(&src_buf->vb2_buf, 0);

	regs->h26x_params.reg065_strm_start_bit = 8 * (uncomp_len & 0xf);
	regs->h26x_params.reg066_stream_len     = ((strm_len + 15) & ~15u) + 0x80;
	regs->h26x_params.reg067_global_len     = VDPU383_VP9_GBL_LEN;

	/*
	 * Zero the padding region after the bitstream payload.
	 * MPP `hal_vp9d_vdpu383.c::hal_vp9d_vdpu383_gen_regs` does:
	 *   regs->comm_paras.reg66_stream_len = ((stream_len + 15) & ~15) + 0x80;
	 *   memset(bitstream + stream_len, 0, reg66 - stream_len);
	 *
	 * Critical for tiny INTER frames (~24 byte show-alt-ref): without this,
	 * HW reads up to 128 bytes of uninitialised garbage past the real
	 * stream as if it were entropy-coded data. For long streams it usually
	 * doesn't matter (encoder ends with a stop pattern early enough that
	 * HW's own decoder state machine halts before the garbage); for show-
	 * alt-ref the entire decode path is so short the padding noise
	 * dominates and produces uniform fill output.
	 *
	 * V4L2 OUTPUT buffers (dma-contig) have no kernel CPU mapping by
	 * default — vb2_plane_vaddr returns NULL. Resolve dma_addr → phys via
	 * the rkvdec IOMMU domain and memremap for the duration of the
	 * memset. This is safe in run() context (m2m worker thread, sleep-
	 * allowed). Mirrors the AV1 driver's source-copy pattern.
	 */
	{
		u32 padded_len = ((strm_len + 15) & ~15u) + 0x80;
		u32 pad_off    = strm_len;
		u32 pad_len    = padded_len - strm_len;
		size_t plane_sz = vb2_plane_size(&src_buf->vb2_buf, 0);

		/* Only needed for Bug-A tiny frames (<512 B); iommu_iova_to_phys
		 * can return MMIO PAs for large buffers whose IOVA is near
		 * the top of the 32-bit IOVA space (observed crash at frame ~391).
		 */
		if (strm_len < 512 && pad_off + pad_len <= plane_sz) {
			struct rkvdec_dev *rkvdec = ctx->dev;
			struct iommu_domain *domain =
				iommu_get_domain_for_dev(rkvdec->dev);
			phys_addr_t pad_phys = domain ?
				iommu_iova_to_phys(domain, src_dma + pad_off) : 0;
			void *pad_cpu = pad_phys ?
				memremap(pad_phys, pad_len, MEMREMAP_WB) : NULL;

			if (pad_cpu) {
				memset(pad_cpu, 0, pad_len);
				memunmap(pad_cpu);
			}
		}

		/* 2026-06-12 fable review — stream-tail sensitivity probe.
		 * The pad memset above writes through a MEMREMAP_WB alias with
		 * no cache clean, and only for <512 B frames; the HW reads
		 * [strm_base, strm_base + reg66) which extends past the
		 * payload. Probe whether the bytes in that tail reach the
		 * entropy decoder on the failing tiny frames:
		 *   vp9_tail_dbg=1  re-zero the pad page-by-page + cache clean
		 *   vp9_tail_dbg=2  poison the pad with 0xAA + cache clean
		 * Page-by-page iova->phys per [[vdpu383_iommu_scatter_memset_crash]].
		 * Diagnostic only, default off. */
		if (vp9_tail_dbg && pad_off + pad_len <= plane_sz) {
			struct rkvdec_dev *rkvdec = ctx->dev;
			struct iommu_domain *dom =
				iommu_get_domain_for_dev(rkvdec->dev);
			u8 fill = (vp9_tail_dbg == 2) ? 0xAA : 0x00;
			u32 done = 0;

			while (dom && done < pad_len) {
				u32 off = pad_off + done;
				u32 in_page = PAGE_SIZE - ((src_dma + off) &
							   ~PAGE_MASK);
				u32 chunk = min(pad_len - done, in_page);
				phys_addr_t pa = iommu_iova_to_phys(
					dom, (src_dma + off) & PAGE_MASK);
				void *va = pa ? memremap(pa, PAGE_SIZE,
							 MEMREMAP_WB) : NULL;

				if (!va)
					break;
				memset(va + ((src_dma + off) & ~PAGE_MASK),
				       fill, chunk);
				memunmap(va);
				done += chunk;
			}
			dma_sync_single_for_device(rkvdec->dev,
						   src_dma + pad_off, pad_len,
						   DMA_TO_DEVICE);
			pr_info("rkvdec-vp9 tail-dbg: mode=%d strm_len=%u pad=[%u,%u) filled=%u\n",
				vp9_tail_dbg, strm_len, pad_off,
				pad_off + pad_len, done);
		}
	}

	regs->h26x_params.reg068_hor_virstride            = hw_hor;
	regs->h26x_params.reg069_raster_uv_hor_virstride  = hw_hor;
	regs->h26x_params.reg070_y_virstride              = hw_y_stride;

	regs->h26x_params.reg080_error_ref_hor_virstride           = hw_hor;
	regs->h26x_params.reg081_error_ref_raster_uv_hor_virstride = hw_hor;
	regs->h26x_params.reg082_error_ref_virstride               = hw_y_stride;

	/*
	 * Per-ref stride registers reg83-91. The IP block walks
	 * ref-frame pixel data using these strides during INTER decode;
	 * for KEY / INTRA_ONLY frames the refs are unused and the
	 * vendor driver writes zero here (matching ref_frame_idx ==
	 * 0x7f for an IDR). Writing non-zero strides for a phantom ref
	 * makes the IP block walk against whatever reg170-172 points at
	 * (the dst-buffer fallback when no real ref exists); on multi-
	 * tile content (tile_cols > 1) this corrupts every tile after
	 * the first.
	 *
	 * INTER frames always write strides for all three trios — the
	 * V4L2 control timestamps cannot reliably distinguish "unused
	 * ref slot" from "ref decoded but slot 0 of the queue" (e.g.
	 * when the first ref's source buffer carries ts=0 from gst's
	 * vp9parse), so any conditional-zeroing per ref produces worse
	 * regressions on encoders that emit such streams.
	 */
	{
		/*
		 * 2026-06-01 R30 result (reverted): unconditional ref strides
		 * (matching MPP's apparent behaviour from R29 reg dump) made
		 * Rick 720p WORSE (10% → 2.6% writeback) and introduced a new
		 * failure mode irq_nowb=19. The MPP value we observed for
		 * reg83 was from a different frame (not the same V4L2 frame).
		 * Conditional ref strides are correct for our V4L2 path.
		 */
		bool is_intra_frame =
			!!(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
			!!(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
		bool ref_used[3] = {
			!is_intra_frame,
			!is_intra_frame,
			!is_intra_frame,
		};

		regs->h26x_params.reg083_ref0_hor_virstride           = ref_used[0] ? hw_hor      : 0;
		regs->h26x_params.reg084_ref0_raster_uv_hor_virstride = ref_used[0] ? hw_hor      : 0;
		regs->h26x_params.reg085_ref0_virstride               = ref_used[0] ? hw_y_stride : 0;
		regs->h26x_params.reg086_ref1_hor_virstride           = ref_used[1] ? hw_hor      : 0;
		regs->h26x_params.reg087_ref1_raster_uv_hor_virstride = ref_used[1] ? hw_hor      : 0;
		regs->h26x_params.reg088_ref1_virstride               = ref_used[1] ? hw_y_stride : 0;
		regs->h26x_params.reg089_ref2_hor_virstride           = ref_used[2] ? hw_hor      : 0;
		regs->h26x_params.reg090_ref2_raster_uv_hor_virstride = ref_used[2] ? hw_hor      : 0;
		regs->h26x_params.reg091_ref2_virstride               = ref_used[2] ? hw_y_stride : 0;
	}

	/* ---- COMMON_ADDR region (reg128-162) ---- */
	regs->common_addr.reg128_strm_base =
		lower_32_bits(src_dma + (uncomp_len & ~0xfu));
	regs->common_addr.reg131_gbl_base = lower_32_bits(vp9_ctx->gbl_buf.dma);

	/*
	 * RCB layout experiment 2026-05-29: pack stages in MPP's priority
	 * order (FLTD_IN_ROW at offset 0) within a contiguous SRAM region,
	 * matching what MPP's BSP service kernel does via
	 * `vdpu38x_rcb_set_info` + RCB_SET_BY_PRIORITY_MODE.
	 *
	 * The mainline rkvdec_allocate_rcb gives us one contiguous SRAM
	 * region (chunks i=0..10 sit at base + cumulative_size) in DEFINITION
	 * order: strmd, strmd_tile, inter, inter_tile, intra, intra_tile,
	 * filterd, filterd_protect, filterd_tile_row, filterd_tile_col,
	 * upscale.
	 *
	 * MPP's priority order is (most critical first):
	 *   0  FLTD_IN_ROW         (our slot 6)
	 *   1  INTER_IN_ROW        (our slot 2)
	 *   2  INTRA_IN_ROW        (our slot 4)
	 *   3  STRMD_IN_ROW        (our slot 0)
	 *   4  INTER_ON_ROW        (our slot 3)
	 *   5  INTRA_ON_ROW        (our slot 5)
	 *   6  STRMD_ON_ROW        (our slot 1)
	 *   7  FLTD_ON_ROW         (our slot 8)
	 *   8  FLTD_ON_COL         (our slot 9)
	 *   9  FLTD_UPSC_ON_COL    (our slot 10)
	 *  10  FLTD_PROT_IN_ROW    (our slot 7)
	 *
	 * Hypothesis: HW expects FLTD_IN_ROW scratch at the lowest SRAM
	 * offset (cache-line-aligned, prefetcher-friendly). Putting it 6
	 * slots in (our default) might cause cache/latency issues that
	 * corrupt INTER decode on tiny content.
	 */
	{
		static const u8 prio_to_slot[11] = {
			6, 2, 4, 0, 3, 5, 1, 8, 9, 10, 7,
		};
		u32 base = lower_32_bits(rkvdec_rcb_buf_dma_addr(ctx, 0));
		u32 cum_off = 0;
		size_t sz;
		int p, slot;

		if (rcb_sram_pack) {
			/*
			 * 2026-06-09 packed mode: the allocator placed each region
			 * individually (high-priority ones in SRAM, rest DRAM), so
			 * program each slot at its OWN dma — the regions are not a
			 * single contiguous block here. (Priority is already reflected
			 * in which regions the allocator put in SRAM.)
			 */
			for (slot = 0; slot < rkvdec_rcb_buf_count(ctx); slot++) {
				regs->common_addr.reg140_162_rcb_info[slot].offset =
					lower_32_bits(rkvdec_rcb_buf_dma_addr(ctx, slot));
				regs->common_addr.reg140_162_rcb_info[slot].size =
					rkvdec_rcb_buf_size(ctx, slot);
			}
		} else {
			for (p = 0; p < (int)ARRAY_SIZE(prio_to_slot); p++) {
				slot = prio_to_slot[p];
				if (slot >= rkvdec_rcb_buf_count(ctx))
					continue;
				sz = rkvdec_rcb_buf_size(ctx, slot);
				regs->common_addr.reg140_162_rcb_info[slot].offset =
					base + cum_off;
				regs->common_addr.reg140_162_rcb_info[slot].size = sz;
				cum_off += ALIGN(sz, SZ_4K);
			}
		}
	}

	/* ---- CODEC_ADDR (VP9) region (reg168-232) ---- */
	last_dma   = vp9_ref_dma(ctx, dst_buf, fp->last_frame_ts);
	golden_dma = vp9_ref_dma(ctx, dst_buf, fp->golden_frame_ts);
	alt_dma    = vp9_ref_dma(ctx, dst_buf, fp->alt_frame_ts);

	/*
	 * Phase 11 diagnostic (sessions j/k 2026-05-25): read back ref
	 * buffers, source bitstream, prob-ctx state, and colmv-ref content
	 * BEFORE the HW kick, to verify all inputs HW will dereference are
	 * what we expect.
	 *
	 * Cumulative result on sintel: every input at VP9 f3-start
	 * (last/golden/alt pixel data, alt-ref colmv pattern, src
	 * bitstream byte-identical to MPP, adapted prob buffer, colmv-ref
	 * pattern 807d807d... matching MPP convention) is VALID. HW still
	 * writes 100% zero output. Cascade f4+ have zero last_ref/colmv
	 * because f3's failure didn't populate those buffers.
	 *
	 * Session-k phase 13 falsified the "forcing prob_default" angle —
	 * it BROKE VP9 f2 (show-alt-ref display, previously correct), so
	 * adapted probs are LOAD-BEARING, not the cause.
	 *
	 * Conclusion across sessions g/h/i/j/k: bug is in HW behavior or
	 * cross-frame HW state we don't enumerate (CACHE1+, IOMMU
	 * detach/attach lifecycle, statistic regs, IP-block internal
	 * pipeline), not in anything programmed via memcpy_toio.
	 */
	if (vp9_bug_a_phase >= 11 &&
	    ((vb2_get_plane_payload(&src_buf->vb2_buf, 0) < 4096 &&
	      !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
	      !(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY)) ||
	     (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
	     vb2_get_plane_payload(&src_buf->vb2_buf, 0) > 4096)) {
		struct rkvdec_dev *rkvdec_p11 = ctx->dev;
		struct iommu_domain *dom_p11 =
			iommu_get_domain_for_dev(rkvdec_p11->dev);
		struct { dma_addr_t iova; const char *label; } refs[] = {
			{ last_dma,   "LAST  " },
			{ golden_dma, "GOLDEN" },
			{ alt_dma,    "ALT   " },
		};
		size_t k;

		for (k = 0; k < ARRAY_SIZE(refs); k++) {
			phys_addr_t ph = dom_p11 ?
				iommu_iova_to_phys(dom_p11, refs[k].iova) : 0;
			void *cpu = ph ? memremap(ph, 16, MEMREMAP_WB) : NULL;

			if (cpu) {
				u8 *b = cpu;
				pr_info("vp9 p11 %s iova=%pad phys=%pa first16=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
					refs[k].label, &refs[k].iova, &ph,
					b[0],b[1],b[2],b[3], b[4],b[5],b[6],b[7],
					b[8],b[9],b[10],b[11], b[12],b[13],b[14],b[15]);
				memunmap(cpu);
			} else {
				pr_info("vp9 p11 %s iova=%pad MEMREMAP FAILED\n",
					refs[k].label, &refs[k].iova);
			}
		}

		/* Also read back the SOURCE bitstream buffer — verify the
		 * compressed data HW is supposed to decode is actually there.
		 */
		{
			phys_addr_t srcph = dom_p11 ?
				iommu_iova_to_phys(dom_p11, src_dma) : 0;
			void *srccpu = srcph ? memremap(srcph, 32, MEMREMAP_WB) : NULL;
			if (srccpu) {
				u8 *b = srccpu;
				pr_info("vp9 p11 SRC    iova=%pad phys=%pa first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
					&src_dma, &srcph,
					b[0],b[1],b[2],b[3], b[4],b[5],b[6],b[7],
					b[8],b[9],b[10],b[11], b[12],b[13],b[14],b[15],
					b[16],b[17],b[18],b[19], b[20],b[21],b[22],b[23],
					b[24],b[25],b[26],b[27], b[28],b[29],b[30],b[31]);
				memunmap(srccpu);
			}
		}

		/* Frame-context tracking — which ctx slot does this frame use,
		 * which slots are valid, and what's the lastprob buffer it
		 * will read from (= adapted ctx-N probs, or pristine
		 * prob_default).
		 */
		{
			void *lp = vp9_ctx->prob_ctx_valid[frame_ctx_id]
				? &((struct rkvdec_vdpu383_vp9_priv_tbl *)vp9_ctx->priv_tbl.cpu)
					->probs.probs[frame_ctx_id]
				: &((struct rkvdec_vdpu383_vp9_priv_tbl *)vp9_ctx->priv_tbl.cpu)
					->prob_default;
			u8 *lpb = lp;
			pr_info("vp9 p11 PROB   ctx_idx=%u valid[0..3]=%u%u%u%u source=%s lastprob_first16=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
				frame_ctx_id,
				vp9_ctx->prob_ctx_valid[0],
				vp9_ctx->prob_ctx_valid[1],
				vp9_ctx->prob_ctx_valid[2],
				vp9_ctx->prob_ctx_valid[3],
				vp9_ctx->prob_ctx_valid[frame_ctx_id]
					? "adapted" : "default",
				lpb[0],lpb[1],lpb[2],lpb[3],
				lpb[4],lpb[5],lpb[6],lpb[7],
				lpb[8],lpb[9],lpb[10],lpb[11],
				lpb[12],lpb[13],lpb[14],lpb[15]);
		}

		/* Read back colmv-ref content for last_dma — the per-frame
		 * motion-vector buffer that HW reads to MC-predict the
		 * current frame. For Bug-A INTER frames (last_was_inter +
		 * SHOW_FRAME), reg217 = last_dma + ctx->colmv_offset. If the
		 * previous frame (e.g., show-alt-ref display) didn't populate
		 * this colmv area, HW gets zero MVs → no motion prediction
		 * → residual-only decode → may produce near-zero output for
		 * sparse-residual frames.
		 */
		if (last_dma && last_dma != dst_dma) {
			dma_addr_t colmv_ref_iova = last_dma + ctx->colmv_offset;
			phys_addr_t cph = dom_p11 ?
				iommu_iova_to_phys(dom_p11, colmv_ref_iova) : 0;
			void *ccpu = cph ? memremap(cph, 32, MEMREMAP_WB) : NULL;
			if (ccpu) {
				u8 *b = ccpu;
				pr_info("vp9 p11 COLMV  iova=%pad phys=%pa first32=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
					&colmv_ref_iova, &cph,
					b[0],b[1],b[2],b[3], b[4],b[5],b[6],b[7],
					b[8],b[9],b[10],b[11], b[12],b[13],b[14],b[15],
					b[16],b[17],b[18],b[19], b[20],b[21],b[22],b[23],
					b[24],b[25],b[26],b[27], b[28],b[29],b[30],b[31]);
				memunmap(ccpu);
			} else {
				pr_info("vp9 p11 COLMV  iova=%pad MEMREMAP FAILED\n",
					&colmv_ref_iova);
			}
		}
	}

	/*
	 * Bug-A workaround (dmaengine, post-decode pattern). For tiny
	 * show-alt-ref INTER frames the VDPU383 silicon writes zeros to
	 * the destination buffer instead of copying alt-ref pixel data.
	 *
	 * Earlier session-g attempt did the PL330 memcpy here (pre-kick)
	 * — hung the board at every size including 1024 B. Touching the
	 * V4L2 CAPTURE buffer phys while it's queued for the IP block
	 * causes a hang (likely IOMMU/bus contention, or some m2m-worker
	 * lock held by vb2_dma_contig).
	 *
	 * Switched to post-decode pattern: stash alt_dma + copy_sz in
	 * vp9_ctx->cur, do the actual PL330 memcpy in vp9_done() after
	 * the HW has finished. The HW's zeros get overwritten with the
	 * alt-ref pixel data (since show-alt-ref frames should display
	 * exactly the alt-ref content).
	 */
	/*
	 * Trigger threshold: strm_len < 4096. Earlier doc estimate was
	 * <512 (from yt_720p60 / BBB which use 44-byte alt-ref overlays),
	 * but sintel's show-alt-ref display frames are 1400-1700 bytes
	 * and they too cascade to all-zero Y planes — verified with phase
	 * 0 reproducing 100% zero-Y on sintel frame 2-4. 4096 captures
	 * the visible alt-ref-display range across yt/BBB/sintel while
	 * excluding normal INTER frames (typically 5KB+).
	 */
	if (vp9_bug_a_phase >= 4 && vp9_bug_a_phase <= 7 &&
	    alt_dma && alt_dma != dst_dma &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY) &&
	    strm_len < 4096) {
		size_t copy_sz;

		switch (vp9_bug_a_phase) {
		case 4:  copy_sz = 1024;             break;
		case 5:  copy_sz = 16 * 1024;        break;
		case 6:  copy_sz = 256 * 1024;       break;
		default: copy_sz = (size_t)bytesperline * height * 3 / 2; break;
		}

		vp9_ctx->cur.bug_a_alt_dma = alt_dma;
		vp9_ctx->cur.bug_a_dst_dma = dst_dma;
		vp9_ctx->cur.bug_a_copy_sz = copy_sz;
	} else {
		vp9_ctx->cur.bug_a_copy_sz = 0;
	}

	regs->vp9_addr.reg168_decout_base    = dst32;
	regs->vp9_addr.reg169_error_ref_base = dst32;
	regs->vp9_addr.reg170_last_ref_base   = lower_32_bits(last_dma);
	regs->vp9_addr.reg171_golden_ref_base = lower_32_bits(golden_dma);
	regs->vp9_addr.reg172_altref_ref_base = lower_32_bits(alt_dma);

	/* Zero unused ref / payload / colmv slots. MPP's vdpu383 VP9 hal
	 * (DUMP_VDPU383_DATAS regs_full.dat reference, 2026-05-29) writes
	 * these as FD 0 → MPP service kernel writes 0 to the MMIO register.
	 * The earlier "fill with dst" pattern was inherited from a HEVC
	 * misreading; HW interprets non-zero entries here as additional
	 * valid DPB / payload / colmv references, which corrupts INTER
	 * decode on small-dimension content (02-size cluster).
	 *
	 * Memory: leaving these at zero on a write-after-zero sequence
	 * matches the reg168 pre-zero strategy (reg168 alone gives Fluster
	 * +18 vs reverted) — HW expects a clean address-latch reset where
	 * unused slots are 0.
	 */
	memset(regs->vp9_addr.reg173_180_unused_ref, 0,
	       sizeof(regs->vp9_addr.reg173_180_unused_ref));
	memset(regs->vp9_addr.reg198_210_unused_payload, 0,
	       sizeof(regs->vp9_addr.reg198_210_unused_payload));
	memset(regs->vp9_addr.reg218_232_unused_colmv, 0,
	       sizeof(regs->vp9_addr.reg218_232_unused_colmv));
	(void)colmv_dst32;

	if (vp9_ctx->cur.segmapid == 0) {
		segmap_cur_dma  = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						      segmap[0]);
		segmap_last_dma = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						      segmap[1]);
	} else {
		segmap_cur_dma  = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						      segmap[1]);
		segmap_last_dma = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						      segmap[0]);
	}
	regs->vp9_addr.reg181_segid_last_base = lower_32_bits(segmap_last_dma);
	regs->vp9_addr.reg182_segid_cur_base  = lower_32_bits(segmap_cur_dma);

	regs->vp9_addr.reg183_kf_prob_base = lower_32_bits(priv_dma +
		offsetof(struct rkvdec_vdpu383_vp9_priv_tbl, kf_probs));

	/*
	 * reg184 read source: prob_default for fresh contexts, the
	 * per-context buffer for adapted contexts. reg185 always writes
	 * to the per-context buffer. Mirrors MPP: prob_default_base stays
	 * pristine across decodes (HW only reads it), prob_loop_base[idx]
	 * is the writeback target.
	 *
	 * Using a single buffer for both reg184 and reg185 (which we did
	 * before this commit) corrupted the pristine defaults via HW
	 * writeback after the first decode using any context, breaking
	 * every subsequent INTER frame.
	 */
	/*
	 * Phase 13 (FALSIFIED 2026-05-25): forcing prob_default for every
	 * frame broke VP9 f2 (show-alt-ref display) which works correctly
	 * with adapted probs in baseline. Adapted probs are LOAD-BEARING
	 * for f2's correct output — they're not Bug-A's cause.
	 */
	if (vp9_ctx->prob_ctx_valid[frame_ctx_id])
		lastprob_dma = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						   probs.probs[frame_ctx_id]);
	else
		lastprob_dma = priv_dma + offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
						   prob_default);
	regs->vp9_addr.reg184_lastprob_base = lower_32_bits(lastprob_dma);

	/*
	 * 2026-06-01 R27 diagnostic: log per-frame which prob source
	 * reg184 points at (D=prob_default, L=prob_loop[ctx_id]) plus the
	 * frame's key/refresh_ctx flags + frame_context_idx. Lets us see
	 * the cascade pattern on YouTube content. Gated on vp9_bug_a_phase
	 * to avoid noise in normal runs.
	 */
	if (vp9_bug_a_phase >= 27)
		pr_info("vp9 r27 frame: %s ctx=%u refresh=%u valid=[%u%u%u%u] reg184=%c\n",
			(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ? "KEY  " : "INTER",
			frame_ctx_id,
			!!(fp->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX),
			vp9_ctx->prob_ctx_valid[0],
			vp9_ctx->prob_ctx_valid[1],
			vp9_ctx->prob_ctx_valid[2],
			vp9_ctx->prob_ctx_valid[3],
			vp9_ctx->prob_ctx_valid[frame_ctx_id] ? 'L' : 'D');
	regs->vp9_addr.reg185_updateprob_base = lower_32_bits(priv_dma +
		offsetof(struct rkvdec_vdpu383_vp9_priv_tbl,
			 probs.probs[frame_ctx_id]));

	regs->vp9_addr.reg192_payload_cur_base       = dst32;
	regs->vp9_addr.reg194_payload_error_ref_base = dst32;
	regs->vp9_addr.reg195_payload_ref0_base      = lower_32_bits(last_dma);
	regs->vp9_addr.reg196_payload_ref1_base      = lower_32_bits(golden_dma);
	regs->vp9_addr.reg197_payload_ref2_base      = lower_32_bits(alt_dma);

	/*
	 * H1-vs-H2 probe (2026-06-04): see vp9_perturb_refs. Point the
	 * last+golden legs (header reg170/171 + payload reg195/196) at a
	 * 0x00-filled scratch buffer, leaving alt (reg172/197) intact. Only
	 * for the displayed compound frame (SHOW_FRAME, !KEY, !INTRA, small
	 * strm) so we hit F2 and not the hidden alt-ref F1.
	 */
	if ((vp9_perturb_refs || vp9_iova_fault) && alt_dma && alt_dma != dst_dma &&
	    (fp->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY) &&
	    strm_len < 4096) {
		struct rkvdec_dev *rkvdec = ctx->dev;

		/*
		 * Cat 5.1 IOMMU access trace takes precedence over the content
		 * perturb: redirect last/golden to UNMAPPED sentinels so any HW
		 * read of those legs page-faults at a recognisable address.
		 */
		if (vp9_iova_fault) {
			/* distinct sentinels per leg+kind so a fault address
			 * attributes the read: dead=last-pixel, deae=golden-pixel,
			 * dac0=last-payload, dab0=golden-payload */
			u32 last_px = 0xDEAD0000u, gold_px = 0xDEAE0000u;
			u32 last_pl = 0xDAC00000u, gold_pl = 0xDAB00000u;

			regs->vp9_addr.reg170_last_ref_base     = last_px;
			regs->vp9_addr.reg171_golden_ref_base   = gold_px;
			regs->vp9_addr.reg195_payload_ref0_base = last_pl;
			regs->vp9_addr.reg196_payload_ref1_base = gold_pl;
			pr_info("VP9-IOVA-FAULT show-compound: last_px=%#x last_pl=%#x golden_px=%#x golden_pl=%#x alt=%#x kept strm=%u\n",
				last_px, last_pl, gold_px, gold_pl,
				lower_32_bits(alt_dma), (u32)strm_len);
			goto perturb_done;
		}

		if (!vp9_perturb_cpu) {
			vp9_perturb_cpu = dma_alloc_coherent(rkvdec->dev,
				VP9_PERTURB_SZ, &vp9_perturb_dma, GFP_KERNEL);
			if (vp9_perturb_cpu)
				memset(vp9_perturb_cpu, 0x00, VP9_PERTURB_SZ);
		}
		if (vp9_perturb_refs == 4) {
			/* 2026-06-12 fable review — dst-prefill probe: every ref
			 * address register (incl. error-ref) proved output-
			 * irrelevant for the failing frame. Pre-fill the DST
			 * (decout) buffer with 0x55 page-by-page before the kick:
			 *   all-0x55 out      -> HW wrote a different buffer
			 *   bottom 0x55-mixed -> HW predicts in-place from dst
			 *   unchanged          -> full overwrite, source unknown */
			struct rkvdec_dev *rkvdec2 = ctx->dev;
			struct iommu_domain *dom2 =
				iommu_get_domain_for_dev(rkvdec2->dev);
			size_t dsz = vb2_plane_size(&dst_buf->vb2_buf, 0);
			size_t done2 = 0;

			while (dom2 && done2 < dsz) {
				phys_addr_t pa2 = iommu_iova_to_phys(
					dom2, (dst_dma + done2) & PAGE_MASK);
				void *va2 = pa2 ? memremap(pa2, PAGE_SIZE,
							   MEMREMAP_WB) : NULL;
				if (!va2)
					break;
				memset(va2, 0x55, PAGE_SIZE);
				memunmap(va2);
				done2 += PAGE_SIZE;
			}
			dma_sync_single_for_device(rkvdec2->dev, dst_dma, dsz,
						   DMA_TO_DEVICE);
			pr_info("VP9-PERTURB-DSTFILL: filled %zu/%zu of dst %#x with 0x55\n",
				done2, dsz, lower_32_bits(dst_dma));
		} else if (vp9_perturb_cpu && vp9_perturb_refs == 3) {
			/* 2026-06-12 fable review — error-ref probe: all three
			 * real ref legs proved output-irrelevant on the failing
			 * frame (modes 1+2 byte-identical), so test whether the
			 * prediction pixels flow through the ERROR-CONCEALMENT
			 * reference (reg169/reg194, which we point at dst). */
			u32 p32 = lower_32_bits(vp9_perturb_dma);

			regs->vp9_addr.reg169_error_ref_base         = p32;
			regs->vp9_addr.reg194_payload_error_ref_base = p32;
			pr_info("VP9-PERTURB-ERRREF show-compound: error_ref->scratch %#x (refs kept) strm=%u\n",
				p32, (u32)strm_len);
		} else if (vp9_perturb_cpu && vp9_perturb_refs == 2) {
			/* 2026-06-12 fable review — mirror probe: redirect the
			 * ALT leg only (header reg172 + payload reg197), keep
			 * last/golden. If the whole-frame output changes, every
			 * block's prediction flows through the ALT leg (the
			 * "all refs resolve to ALT" routing claim); if only the
			 * truly-alt-coded blocks change, ref routing is fine. */
			u32 p32 = lower_32_bits(vp9_perturb_dma);

			regs->vp9_addr.reg172_altref_ref_base   = p32;
			regs->vp9_addr.reg197_payload_ref2_base = p32;
			pr_info("VP9-PERTURB-ALT show-compound: alt->scratch %#x (last=%#x golden kept) strm=%u\n",
				p32, lower_32_bits(alt_dma), (u32)strm_len);
		} else if (vp9_perturb_cpu) {
			u32 p32 = lower_32_bits(vp9_perturb_dma);

			regs->vp9_addr.reg170_last_ref_base     = p32;
			regs->vp9_addr.reg171_golden_ref_base   = p32;
			regs->vp9_addr.reg195_payload_ref0_base = p32;
			regs->vp9_addr.reg196_payload_ref1_base = p32;
			pr_info("VP9-PERTURB show-compound: last/golden->scratch %#x (alt=%#x kept) strm=%u\n",
				p32, lower_32_bits(alt_dma), (u32)strm_len);
		}
perturb_done:
		(void)rkvdec;
	}

	regs->vp9_addr.reg216_colmv_cur_base = colmv_dst32;

	/*
	 * reg217 colmv-ref base: when no valid previous colmv exists, MPP
	 * falls back to the CURRENT frame's colmv buffer, NOT to a
	 * 0xFFFFFFFF sentinel. See
	 * `mpp/hal/rkdec/vp9d/hal_vp9d_vdpu383.c::hal_vp9d_vdpu383_gen_regs`
	 * lines 522-526:
	 *
	 *     hw_ctx->mv_base_addr = reg216_colmv_cur_base;
	 *     if (hw_ctx->pre_mv_base_addr < 0)
	 *         hw_ctx->pre_mv_base_addr = hw_ctx->mv_base_addr;
	 *     reg217_colmv_ref_base[0] = hw_ctx->pre_mv_base_addr;
	 *
	 * RK3576 HW dereferences reg217 unconditionally even when the GBL
	 * `use_prev_frame_mvs` bit is 0; reading from the 0xFFFFFFFF
	 * sentinel produced IOMMU page faults at iova=0xFFFFFFF0 (HW reads
	 * 16 bytes from the address, hence the -16 offset in the fault
	 * log) and silenced HW completion entirely.
	 *
	 * Pointing reg217 at the current frame's own colmv buffer is safe:
	 * GBL `use_prev_mvs=0` tells HW not to USE the data, but HW is
	 * free to prefetch from a valid address without faulting.
	 *
	 * MPP also gates the "advance" path on the previous frame being
	 * a SHOW_FRAME inter (skips alt-ref / KEY). We do the same here
	 * via `last_was_inter`, falling back to self-ref otherwise.
	 *
	 * Fixing this bug eliminated the IOMMU page-fault storms entirely
	 * (was hundreds per second, now zero) and unblocked frame-pool
	 * progression beyond the first ~10 frames.  Sintel mid 720p went
	 * from "stuck at frames=0" to decoding 78 fps with ~3% strm_error
	 * rate (separate bug).  allkey/noalt/smpte all decode 30/30 clean.
	 */
	{
		bool last_was_inter = vp9_ctx->last.valid &&
			!(vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
			!(vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY) &&
			!!(vp9_ctx->last.flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME);

		regs->vp9_addr.reg217_colmv_ref_base = colmv_dst32;

		/*
		 * R49 (2026-06-02) — MPP carry-forward pre_mv test. When
		 * enabled, reg217 references `last_shown_inter_colmv_dma`
		 * (persistent, carries forward across alt-refs), matching
		 * MPP HAL pre_mv_base_addr semantics. Falls through to
		 * the existing "last_was_inter" path otherwise.
		 *
		 * This differs from the existing path in the alt-ref-after-
		 * alt-ref case: ours self-refs colmv (since last_was_inter
		 * is false), MPP points at LAST SHOWN visible-INTER's
		 * colmv. Previous test (vp9_bug_a_phase=8) used MPP-style
		 * carry-forward computed from last_dma + colmv_offset and
		 * didn't fix all-zero-Y. Re-tests against the HANG signal
		 * with proper persistence tracking.
		 */
		if (r49_pre_mv_carryforward &&
		    vp9_ctx->last_shown_inter_colmv_valid) {
			regs->vp9_addr.reg217_colmv_ref_base =
				lower_32_bits(vp9_ctx->last_shown_inter_colmv_dma);
		} else if (last_was_inter) {
			struct vb2_buffer *prev =
				vb2_find_buffer(&ctx->fh.m2m_ctx->cap_q_ctx.q,
						vp9_ctx->last.timestamp);
			if (prev) {
				prev_colmv_dma =
					vb2_dma_contig_plane_dma_addr(prev, 0)
					+ ctx->colmv_offset;
				regs->vp9_addr.reg217_colmv_ref_base =
					lower_32_bits(prev_colmv_dma);
			}
		}

		/*
		 * Bug-A note (session-h 2026-05-25, falsification record):
		 * Pointing reg217 at MPP's `pre_mv_base` value (the
		 * last-shown frame's colmv, computed as
		 * last_dma + ctx->colmv_offset for the show-alt-ref-after-
		 * hidden-alt-ref case) does NOT fix the all-zero-Y output on
		 * tiny show-alt-ref display frames. Tested under
		 * vp9_bug_a_phase=8; debug confirmed override fired on
		 * every Bug-A-condition sintel frame; output identical to
		 * baseline. The MV-prediction reference pointer is not what
		 * HW is mishandling for these frames. See
		 * docs/rk3576/vp9/ISSUE_1_REG217_FALSIFIED_2026-05-25.md.
		 */
	}
}

/* -----------------------------------------------------------------------
 * Push the populated regs struct to MMIO via four memcpy_toio() bursts.
 *
 * On ARM64 rkvdec_memcpy_toio() == __iowrite32_copy(): strictly increasing
 * u32-by-u32 writes, which is what the IP block requires.
 */
static void vdpu383_vp9_write_regs(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx = ctx->priv;

	/* Write order: ctrl (COMMON, reg8-30), then params (CODEC_PARAMS,
	 * reg64-127), then addresses (COMMON_ADDR + CODEC_ADDR, reg128+).
	 * The IP block expects parameters to settle before any address
	 * register write, since an address write can trigger an action
	 * that uses the parameters.
	 */
	if (vp9_hevc_order) {
		/* Exact upstream VDPU383 HEVC order: COMMON, COMMON_ADDR,
		 * CODEC_PARAMS, CODEC_ADDR — four separate bursts. */
		rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_REGS,
				   &vp9_ctx->regs.common,
				   sizeof(vp9_ctx->regs.common));
		rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_ADDR_REGS,
				   &vp9_ctx->regs.common_addr,
				   sizeof(vp9_ctx->regs.common_addr));
		rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_CODEC_PARAMS_REGS,
				   &vp9_ctx->regs.h26x_params,
				   sizeof(vp9_ctx->regs.h26x_params));
		rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_CODEC_ADDR_REGS,
				   &vp9_ctx->regs.vp9_addr,
				   sizeof(vp9_ctx->regs.vp9_addr));
		return;
	}
	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_REGS,
			   &vp9_ctx->regs.common,
			   sizeof(vp9_ctx->regs.common));
	rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_CODEC_PARAMS_REGS,
			   &vp9_ctx->regs.h26x_params,
			   sizeof(vp9_ctx->regs.h26x_params));
	/*
	 * 2026-05-29: write regs 128-232 as ONE contiguous 420-byte burst
	 * (105 dwords), mimicking MPP's `comm_addrs` wr_cfg ioctl. The
	 * mainline rkvdec_vdpu383 backend splits this into two bursts
	 * (reg128-161 then reg168-232 with a writel(0, reg168) sandwich
	 * in between), but MPP — and the BSP service kernel — never does
	 * that splitting.
	 *
	 * Previous Fluster fiddling (sessions m/n) found that the writel(0,
	 * reg168) reset before the second burst empirically gained +18
	 * tests; but it's a pixel-corruption workaround, not a fix. The
	 * 02-size cluster (0/71) doesn't respond to that workaround. The
	 * actual issue may be that splitting the write into two bursts
	 * (with reg162-167 left stale between them) lets HW snapshot the
	 * common_addr region BEFORE vp9_addr lands, observing an
	 * inconsistent (cur, ref) pair mid-write.
	 *
	 * Assemble the unified 105-dword buffer on the stack: common_addr
	 * (34 dwords) + 6 zeros (regs 162-167) + vp9_addr (65 dwords).
	 */
	{
		u32 unified[105];

		memcpy(unified, &vp9_ctx->regs.common_addr,
		       sizeof(vp9_ctx->regs.common_addr));
		memset(&unified[34], 0, 6 * sizeof(u32));
		memcpy(&unified[40], &vp9_ctx->regs.vp9_addr,
		       sizeof(vp9_ctx->regs.vp9_addr));
		rkvdec_memcpy_toio(rkvdec->regs + VDPU383_OFFSET_COMMON_ADDR_REGS,
				   unified, sizeof(unified));
	}
}

/* -----------------------------------------------------------------------
 * Main decode dispatch. Trigger sequence is identical to mainline HEVC:
 * timeout → IP_CRU → DEC_E. We do NOT touch INT_EN_IRQ — HEVC doesn't
 * either, and our prior INT_EN_IRQ-toggle experiments were chasing a
 * phantom that arose from the individual-writel() configuration approach.
 */
/*
 * Per-frame state setup. Populates vp9_ctx->cur from the V4L2 frame
 * params, handles full_reset / per-context invalidation, builds the GBL
 * header and the descriptor reg-set. Caller is responsible for the
 * MMIO write_regs + HW kick (single-shot) or descriptor pack +
 * link_enqueue (link mode).
 *
 * Phase 3 v0.3 step 2.1: extracted from rkvdec_vdpu383_vp9_run so the
 * batched-fill path (step 2.6) can call it once per task in a loop.
 */
static int vdpu383_vp9_prepare_frame(struct rkvdec_ctx *ctx,
				     struct vb2_v4l2_buffer *src,
				     struct vb2_v4l2_buffer *dst,
				     const struct v4l2_ctrl_vp9_frame *fp)
{
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx = ctx->priv;
	bool full_reset;

	/*
	 * Per V4L2 spec, KEY / intra_only / error_resilient frames must
	 * use frame_context_idx=0 for entropy decoding even if the
	 * bitstream's frame_context_idx field says otherwise.  The V4L2
	 * helper `v4l2_vp9_reset_frame_ctx` returns the effective idx
	 * (forcing 0 for those frames) and, as a side-effect, resets the
	 * appropriate `frame_context[]` entries to spec defaults — which
	 * the mainline `drivers/media/.../rkvdec/rkvdec-vp9.c` driver
	 * uses for its lastprob packing.
	 *
	 * Our older code passed `&vp9_ctx->frame_context[ctx_idx]`
	 * (a pointer into the middle of the array) which the helper
	 * iterates as `for i in 0..3 frame_context[i] = ...`, walking
	 * past the array bound into `prob_ctx_valid` / `cur` / `last`
	 * when ctx_idx > 0. Use the full array as mainline does.
	 */
	vp9_ctx->cur.valid                = 1;
	vp9_ctx->cur.flags                = fp->flags;
	vp9_ctx->cur.frame_context_idx    =
		v4l2_vp9_reset_frame_ctx(fp, vp9_ctx->frame_context);
	vp9_ctx->cur.reference_mode       = fp->reference_mode;
	vp9_ctx->cur.interpolation_filter = fp->interpolation_filter;
	vp9_ctx->cur.width                = fp->frame_width_minus_1 + 1;
	vp9_ctx->cur.height               = fp->frame_height_minus_1 + 1;
	vp9_ctx->cur.timestamp            = src->vb2_buf.timestamp;
	vp9_ctx->cur.seg                  = fp->seg;
	vp9_ctx->cur.lf                   = fp->lf;

	/*
	 * 2026-05-31 R18: parse color_space from VP9 uncompressed header
	 * on KEY frames; inherit from `last` on INTER. MPP packs this as
	 * ls_info.color_space_last into gbl_buf — we were hardcoding 0,
	 * which works for CS_UNKNOWN (test vectors, sintel) but mismatches
	 * MPP for any real-world BT709/BT601/BT2020 content (YouTube etc).
	 *
	 * VP9 spec §6.2.1 uncompressed header layout (KEY only):
	 *   bits 0..1: frame_marker (0b10)
	 *   bits 2..3: profile (2-bit; see VP9 §7.2 for >2 path)
	 *   bit 4: show_existing_frame (=0 here; KEY)
	 *   bit 5: frame_type (=0; KEY)
	 *   bit 6: show_frame
	 *   bit 7: error_resilient_mode
	 *   bits 8..31: frame_sync_code (0x498342)
	 *   for profile<=2: bits 32..34 = color_space; profile>=2 adds 1
	 *     bit_depth bit BEFORE color_space.
	 *
	 * We assume profile 0/1 (8-bit) — YouTube serves Profile 0 for
	 * 8-bit and Profile 2 for 10-bit. fp->profile distinguishes.
	 */
	if (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) {
		u8 cs = 0;
		const u8 *hdr = (const u8 *)vb2_plane_vaddr(&src->vb2_buf, 0);
		size_t hdr_sz = vb2_get_plane_payload(&src->vb2_buf, 0);

		if (hdr && hdr_sz >= 5) {
			/* bit position 32 = byte 4 MSB-first; for Profile 0/1
			 * color_space is bits 32..34 = top 3 bits of byte 4.
			 * For Profile 2/3 there's a 1-bit bit_depth at bit 32,
			 * so color_space is bits 33..35 = byte 4 bits 6..4.
			 */
			u8 b4 = hdr[4];

			if (fp->profile >= 2)
				cs = (b4 >> 4) & 0x7;
			else
				cs = (b4 >> 5) & 0x7;
		}
		vp9_ctx->cur.color_space = cs;
	} else {
		vp9_ctx->cur.color_space = vp9_ctx->last.color_space;
	}

	/*
	 * Context invalidation. With the separate prob_default buffer in
	 * place, we don't need to re-pack defaults into per-context
	 * probs[ctx_id] — when prob_ctx_valid[ctx_id]=0 we read from
	 * prob_default for reg184. The IP block will overwrite probs[ctx_id]
	 * via reg185 writeback, so its current contents don't matter.
	 */
	full_reset = (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
		     ((fp->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
		      fp->reset_frame_context == V4L2_VP9_RESET_FRAME_CTX_ALL);
	if (full_reset) {
		int i;

		memset(vp9_ctx->prob_ctx_valid, 0, sizeof(vp9_ctx->prob_ctx_valid));
		for (i = 0; i < VP9_NUM_FRAME_CTX; i++)
			vp9_ctx->frame_context[i] = v4l2_vp9_default_probs;
	} else if (fp->reset_frame_context == V4L2_VP9_RESET_FRAME_CTX_SPEC) {
		u8 idx = fp->frame_context_idx;

		vp9_ctx->prob_ctx_valid[idx] = 0;
		vp9_ctx->frame_context[idx] = v4l2_vp9_default_probs;
	}

	vdpu383_vp9_config_global_hdr(ctx, fp, vp9_ctx);
	vdpu383_vp9_build_regs(ctx, fp, src, dst, vp9_ctx);

	/*
	 * Phase 10: dump GBL bytes for Bug-A candidate frames (tiny show-
	 * alt-ref INTER). Matches MPP's HAL_VP9D_DBG_REG GBL printf format,
	 * 16 bytes per line, for byte-diff against MPP's dump.
	 *
	 * Output line prefix is identical to MPP's so the diff trivially
	 * lines up: "vp9 gbl[NNN]: hh hh hh ... hh hh".
	 */
	/*
	 * Phase 10 register/GBL dump scaffolding (default off via
	 * vp9_bug_a_phase). Fires for every frame when phase >= 10 — used to
	 * byte-diff against MPP's HAL_VP9D_DBG_REG=4 dump.
	 *
	 * Findings (session-i 2026-05-25):
	 *  - GBL byte-identical to MPP for sintel f2 and f3 (the bug
	 *    frames).
	 *  - All comm_paras match including reg65, reg66, stride regs.
	 *  - Address registers semantically match (V4L2 IOVA vs MPP fd
	 *    encoding).
	 *  - reg70/82/85/88/91 y_virstride was 0xC438 (= 92*546) for us
	 *    vs 0xC660 (= 92*552 = 92*ALIGN(546,8)) for MPP. Tested
	 *    ALIGN(height,8) — KEY/f2 still work but f3+ still 100% zero.
	 *    Reverted; cosmetic-only.
	 *  - HW completes via DEC_RDY_STA, no error bits in reg15.
	 *
	 * Bug-A cause still unknown; next angle is HW state / memory
	 * content (alt-ref pixel data, colmv-ref data) at decode time.
	 */
	if (vp9_bug_a_phase >= 10) {
		struct vdpu383_regs_vp9 *r = &vp9_ctx->regs;
		u8 *g = vp9_ctx->gbl_buf.cpu;
		size_t off;

		pr_info("vp9 p10 GBL bytes=%zu strm=%u\n",
			(size_t)VDPU383_VP9_GBL_SIZE,
			(u32)vb2_get_plane_payload(&src->vb2_buf, 0));
		for (off = 0; off < VDPU383_VP9_GBL_SIZE; off += 16)
			pr_info("vp9 p10 gbl[%03zu]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				off,
				g[off+0], g[off+1], g[off+2], g[off+3],
				g[off+4], g[off+5], g[off+6], g[off+7],
				g[off+8], g[off+9], g[off+10], g[off+11],
				g[off+12], g[off+13], g[off+14], g[off+15]);

		/* 2026-06-01 R22: dump prob_kf + prob_default bytes for byte-diff
		 * vs MPP. Format matches patched MPP HAL (vp9 prob_kf[NNNN]: hh...
		 * and vp9 prob_default[NNNN]: hh...) for trivial diff. */
		{
			struct rkvdec_vdpu383_vp9_priv_tbl *pt = vp9_ctx->priv_tbl.cpu;
			size_t off;

			pr_info("vp9 prob_kf bytes=%zu\n",
				(size_t)RKVDEC_VP9_PROB_KF_SIZE);
			for (off = 0; off < RKVDEC_VP9_PROB_KF_SIZE; off += 16)
				pr_info("vp9 prob_kf[%04zu]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					off,
					pt->kf_probs[off+0],  pt->kf_probs[off+1],
					pt->kf_probs[off+2],  pt->kf_probs[off+3],
					pt->kf_probs[off+4],  pt->kf_probs[off+5],
					pt->kf_probs[off+6],  pt->kf_probs[off+7],
					pt->kf_probs[off+8],  pt->kf_probs[off+9],
					pt->kf_probs[off+10], pt->kf_probs[off+11],
					pt->kf_probs[off+12], pt->kf_probs[off+13],
					pt->kf_probs[off+14], pt->kf_probs[off+15]);

			if (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) {
				pr_info("vp9 prob_default bytes=%zu\n",
					(size_t)RKVDEC_VP9_PROBE_SIZE);
				for (off = 0; off < RKVDEC_VP9_PROBE_SIZE; off += 16)
					pr_info("vp9 prob_default[%04zu]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
						off,
						pt->prob_default[off+0],  pt->prob_default[off+1],
						pt->prob_default[off+2],  pt->prob_default[off+3],
						pt->prob_default[off+4],  pt->prob_default[off+5],
						pt->prob_default[off+6],  pt->prob_default[off+7],
						pt->prob_default[off+8],  pt->prob_default[off+9],
						pt->prob_default[off+10], pt->prob_default[off+11],
						pt->prob_default[off+12], pt->prob_default[off+13],
						pt->prob_default[off+14], pt->prob_default[off+15]);
			}
		}

		/* Address registers — semantic names, easier to diff than raw blobs */
		pr_info("vp9 p10 ADDRS-COM: r128_strm=%08x r131_gbl=%08x\n",
			r->common_addr.reg128_strm_base,
			r->common_addr.reg131_gbl_base);
		pr_info("vp9 p10 ADDRS-CDC: r168_dec=%08x r169_err=%08x r170_last=%08x r171_gold=%08x r172_alt=%08x\n",
			r->vp9_addr.reg168_decout_base,
			r->vp9_addr.reg169_error_ref_base,
			r->vp9_addr.reg170_last_ref_base,
			r->vp9_addr.reg171_golden_ref_base,
			r->vp9_addr.reg172_altref_ref_base);
		pr_info("vp9 p10 ADDRS-PRB: r183_kfp=%08x r184_lastp=%08x r185_updp=%08x r181_seglast=%08x r182_segcur=%08x\n",
			r->vp9_addr.reg183_kf_prob_base,
			r->vp9_addr.reg184_lastprob_base,
			r->vp9_addr.reg185_updateprob_base,
			r->vp9_addr.reg181_segid_last_base,
			r->vp9_addr.reg182_segid_cur_base);
		pr_info("vp9 p10 ADDRS-PAY: r192_cur=%08x r194_err=%08x r195_ref0=%08x r196_ref1=%08x r197_ref2=%08x\n",
			r->vp9_addr.reg192_payload_cur_base,
			r->vp9_addr.reg194_payload_error_ref_base,
			r->vp9_addr.reg195_payload_ref0_base,
			r->vp9_addr.reg196_payload_ref1_base,
			r->vp9_addr.reg197_payload_ref2_base);
		pr_info("vp9 p10 ADDRS-CMV: r216_cur=%08x r217_ref=%08x\n",
			r->vp9_addr.reg216_colmv_cur_base,
			r->vp9_addr.reg217_colmv_ref_base);

		/* Key params: dimensions + strides */
		pr_info("vp9 p10 PARAMS: r66_strm_len=%u r67_gbl_len=%u r68_hor=%u r70_y_str=%u r80_err_hor=%u r82_err_str=%u r83_r0_hor=%u r85_r0_str=%u r86_r1_hor=%u r88_r1_str=%u r89_r2_hor=%u r91_r2_str=%u\n",
			r->h26x_params.reg066_stream_len,
			r->h26x_params.reg067_global_len,
			r->h26x_params.reg068_hor_virstride,
			r->h26x_params.reg070_y_virstride,
			r->h26x_params.reg080_error_ref_hor_virstride,
			r->h26x_params.reg082_error_ref_virstride,
			r->h26x_params.reg083_ref0_hor_virstride,
			r->h26x_params.reg085_ref0_virstride,
			r->h26x_params.reg086_ref1_hor_virstride,
			r->h26x_params.reg088_ref1_virstride,
			r->h26x_params.reg089_ref2_hor_virstride,
			r->h26x_params.reg091_ref2_virstride);
		pr_info("vp9 p10 V4L2-CTRL: alt_ts=%llu last_ts=%llu golden_ts=%llu flags=%08x w=%u h=%u uncomp=%u comp=%u\n",
			fp->alt_frame_ts, fp->last_frame_ts, fp->golden_frame_ts,
			fp->flags,
			(u32)(fp->frame_width_minus_1 + 1),
			(u32)(fp->frame_height_minus_1 + 1),
			(u32)fp->uncompressed_header_size,
			(u32)fp->compressed_header_size);
		{
			const struct v4l2_pix_format_mplane *pfmt =
				&ctx->decoded_fmt.fmt.pix_mp;
			pr_info("vp9 p10 DEC-FMT: w=%u h=%u bpl=%u sz=%u img_fmt=%u\n",
				pfmt->width, pfmt->height,
				pfmt->plane_fmt[0].bytesperline,
				pfmt->plane_fmt[0].sizeimage,
				ctx->image_fmt);
		}

		/*
		 * Dump our four register regions as raw u32 stream, matching
		 * MPP's `get regs[NN]: XXXXXXXX` format so the two can be
		 * compared with diff(1). Indices align with MPP's
		 * Vdpu383RegSet layout:
		 *   [0]     = reg_version (we don't program; skip)
		 *   [1-23]  = ctrl_regs (HW reg8-30)
		 *   [24-66] = comm_paras (HW reg64-106)
		 *   [67-171]= comm_addrs (HW reg128-232)
		 */
		{
			u32 *p;
			size_t n, i;
			u32 idx;

			pr_info("vp9 p10 send regs[00]: 00000000  (reg_version, not programmed)\n");

			idx = 1;
			p = (u32 *)&r->common;
			n = sizeof(r->common) / 4;
			for (i = 0; i < n; i++)
				pr_info("vp9 p10 send regs[%02u]: %08X\n",
					(u32)(idx + i), p[i]);
			idx += n;

			p = (u32 *)&r->h26x_params;
			n = sizeof(r->h26x_params) / 4;
			for (i = 0; i < n; i++)
				pr_info("vp9 p10 send regs[%02u]: %08X\n",
					(u32)(idx + i), p[i]);
			idx += n;

			p = (u32 *)&r->common_addr;
			n = sizeof(r->common_addr) / 4;
			for (i = 0; i < n; i++)
				pr_info("vp9 p10 send regs[%02u]: %08X\n",
					(u32)(idx + i), p[i]);
			idx += n;

			p = (u32 *)&r->vp9_addr;
			n = sizeof(r->vp9_addr) / 4;
			for (i = 0; i < n; i++)
				pr_info("vp9 p10 send regs[%02u]: %08X\n",
					(u32)(idx + i), p[i]);
		}
	}

	return 0;
}

extern int r42_irq_settle_us;
extern int r43_invalidate_probs_on_fail;
extern int r50_skip_altref;

static int rkvdec_vdpu383_vp9_run(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_vdpu383_vp9_ctx *vp9_ctx = ctx->priv;
	struct rkvdec_run run = {};
	const struct v4l2_ctrl_vp9_frame *fp;
	struct vb2_v4l2_buffer *src, *dst;
	struct v4l2_ctrl *ctrl;
	u32 timeout_threshold;
	int ret;

	rkvdec_run_preamble(ctx, &run);
	src = run.bufs.src;
	dst = run.bufs.dst;

	if (WARN_ON(!src || !dst)) {
		rkvdec_run_postamble(ctx, &run);
		return -EINVAL;
	}

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_VP9_FRAME);
	if (WARN_ON(!ctrl)) {
		rkvdec_run_postamble(ctx, &run);
		return -EINVAL;
	}
	fp = ctrl->p_cur.p;

	if (vp9_dump_ctrls) {
		static const char hx[] = "0123456789abcdef";
		static u32 cd_idx;
		const u8 *p = (const u8 *)fp;
		char hb[2 * sizeof(*fp) + 1];
		size_t bi;

		for (bi = 0; bi < sizeof(*fp); bi++) {
			hb[2 * bi]     = hx[p[bi] >> 4];
			hb[2 * bi + 1] = hx[p[bi] & 0xf];
		}
		hb[2 * sizeof(*fp)] = '\0';
		pr_info("VP9CTRLDUMP idx=%u sz=%zu %s\n",
			cd_idx++, sizeof(*fp), hb);
	}

	/*
	 * VP9 setup_past_independence (2026-06-03): on KEY / error-resilient /
	 * intra-only-with-reset frames, reset ALL HW frame-context prob
	 * buffers to spec DEFAULTS. The per-context probs[] buffers are
	 * zero-initialised and HW only writes back the probs a frame actually
	 * adapts; a KEY frame never decodes inter symbols, so the inter-prob
	 * region (comp_mode / comp_ref / single_ref / inter_mode / mv) of a
	 * context it refreshes stays ZERO. Those probs are NOT forward-updated
	 * in the compressed header — HW reads them straight from the context —
	 * so the first SELECT/compound INTER frame reading that context decodes
	 * comp_mode/comp_ref against zero probs and the arithmetic decoder
	 * desyncs (mid-frame, cascades). Per spec the context must be DEFAULT
	 * after a KEY, not zero. Single-ref-only content (BBB/earth) never reads
	 * comp probs so it was unaffected; high-motion SELECT content (yt,
	 * crowd_run) is. Gated for A/B; default on.
	 */
	if (vp9_ctx_default_on_intra) {
		bool key  = fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME;
		bool intra = fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY;
		bool eres = fp->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT;

		if (key || eres || (intra && fp->reset_frame_context == 3)) {
			struct rkvdec_vdpu383_vp9_priv_tbl *tbl =
				vp9_ctx->priv_tbl.cpu;
			int ci;

			for (ci = 0; ci < VP9_NUM_FRAME_CTX; ci++)
				memcpy(tbl->probs.probs[ci], tbl->prob_default,
				       sizeof(tbl->probs.probs[ci]));
		}
	}

	/* 2026-06-08 input-dump: the INPUT context probs HW reads (reg184 ->
	 * probs[ctx]) for this frame, first 160 B (partition..comp..inter_mode).
	 * Compare vs MPP cabac_update to rule out an entropy-context divergence
	 * upstream of the comp_mode decision. Fires at run() before the kick. */
	if (vp9_adapt_dump) {
		struct rkvdec_vdpu383_vp9_priv_tbl *t2 = vp9_ctx->priv_tbl.cpu;
		u32 cid2 = vp9_ctx->cur.frame_context_idx;

		if (t2 && cid2 < VP9_NUM_FRAME_CTX)
			pr_info("rkvdec-vp9 input-dump: ctx=%u refmode=%u first160=%*ph\n",
				cid2, vp9_ctx->cur.reference_mode,
				160, t2->probs.probs[cid2]);
	}

	/*
	 * R50 (2026-06-02) — alt-ref skip path. When enabled and the
	 * current frame is alt-ref (SHOW_FRAME clear, not KEY, not
	 * INTRA_ONLY), short-circuit: skip the HW decode entirely.
	 *
	 * Trade-off: alt-ref CAPTURE buffer contents undefined →
	 * subsequent INTERs reading the alt-ref slot get garbage →
	 * visual artifact. But the alt-ref intermittent hang
	 * (R44/R45 HW-state lottery) is completely prevented.
	 *
	 * Subsequent frames will reference the (un-decoded) alt-ref
	 * buffer via timestamp. We still update vp9_ctx->cur/last so
	 * DPB tracking advances cleanly. ref_ring also stashes this
	 * frame's dst dma so future timestamp lookups resolve to a
	 * real (albeit garbage) buffer rather than NULL.
	 */
	if (r50_skip_altref &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY)) {
		struct vb2_v4l2_buffer *src_buf = src;
		struct vb2_v4l2_buffer *dst_buf = dst;
		u32 slot;

		/* Stash this frame in ref_ring so future frames'
		 * timestamp lookups resolve. */
		slot = vp9_ctx->ref_ring_next % ARRAY_SIZE(vp9_ctx->ref_ring);
		vp9_ctx->ref_ring[slot].timestamp =
			dst_buf->vb2_buf.timestamp;
		vp9_ctx->ref_ring[slot].dma =
			vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
		/* #5a: dims for per-ref GBL dimension lookup (skip-altref path). */
		vp9_ctx->ref_ring[slot].width  = fp->frame_width_minus_1 + 1;
		vp9_ctx->ref_ring[slot].height = fp->frame_height_minus_1 + 1;
		vp9_ctx->ref_ring[slot].valid = true;
		vp9_ctx->ref_ring_next++;

		/* Update DPB state advance. */
		vp9_ctx->cur.flags = fp->flags;
		vp9_ctx->cur.frame_context_idx = fp->frame_context_idx;
		vp9_ctx->cur.timestamp = dst_buf->vb2_buf.timestamp;
		vp9_ctx->cur.width = fp->frame_width_minus_1 + 1;
		vp9_ctx->cur.height = fp->frame_height_minus_1 + 1;
		vp9_ctx->cur.valid = 1;
		vp9_ctx->last = vp9_ctx->cur;

		pr_info_ratelimited(
			"R50 skip-altref f=%u flags=0x%03x payload=%u\n",
			ctx->dev->accum_frames_run++,
			fp->flags,
			(u32)vb2_get_plane_payload(&src_buf->vb2_buf, 0));

		/* Tell m2m the job is done — buffers go through normally. */
		rkvdec_run_postamble(ctx, &run);
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
		v4l2_m2m_job_finish(rkvdec->m2m_dev, ctx->fh.m2m_ctx);
		return 0;
	}

	/*
	 * 2026-05-30 Round 3: graceful reject of frame-parallel + tiles
	 * content. The VDPU383 silicon has a deterministic errata on this
	 * combination — HW miscomputes one of its output IOVAs (lands on
	 * an unmapped page within the SRAM pool's IOMMU domain). See
	 * SESSION_ROUND2_8_2026-05-30.md and Round 2.8 / 2.9 commits
	 * (3f67804 / aa8975b) for the empirical evidence: 1x2 and 1x8
	 * tile vectors both faulted with content-dependent bad IOVAs,
	 * 0/15 frames clean, all real_to or silent.
	 *
	 * Single-vector decodes recover via the IRQ error path, but each
	 * fault degrades the IOMMU domain marginally; enough faults
	 * across a multi-vector batch eventually corrupt the domain and
	 * crash the box (the "accumulation bug" of earlier rounds).
	 *
	 * Reject cleanly with -EINVAL before kicking HW. Userspace gets
	 * a regular decode-failure return (Fluster counts as 0/1, no
	 * crash, no IOMMU damage).
	 *
	 * Detection: both PARALLEL_DEC_MODE flag set AND at least one
	 * of tile_cols_log2 / tile_rows_log2 non-zero (single-tile
	 * frame-parallel — vector 07 — is safe and DOES decode cleanly,
	 * see Round 2.7 / 1c1e0d1).
	 */
	/*
	 * 2026-05-30 Round 3 diagnostic: log flags/tile values for the
	 * first few frames per session so we can see why 14-resize
	 * content faults despite not matching the PARALLEL_DEC_MODE
	 * detection. May be: tiles alone is enough on resize content,
	 * or PARALLEL_DEC_MODE flag isn't being plumbed by gst v4l2sl.
	 */
	{
		static unsigned long last_log_time;
		unsigned long now = jiffies;

		if (time_after(now, last_log_time + HZ / 2)) {
			last_log_time = now;
			dev_info(rkvdec->dev,
				 "vp9 ctrl: flags=0x%03x tile_cols_log2=%u tile_rows_log2=%u\n",
				 fp->flags,
				 fp->tile_cols_log2, fp->tile_rows_log2);
		}
	}

	/*
	 * R39+/R40/R42 (2026-06-01) per-frame pr_info. Originally
	 * diagnostic for the alt-ref hang investigation, but removing
	 * it (along with the per-IRQ pr_info) regressed yt_720p60
	 * perfect rate from ~25% to 0%. Like the IRQ-handler pr_info,
	 * this is now LOAD-BEARING — the printk side-effect provides
	 * timing/lock work the HW state machine appears to need.
	 *
	 * KEEP unless replaced with an explicit out-of-band settle
	 * mechanism that survives a real production environment.
	 */
	{
		u32 idx = ctx->dev->accum_frames_run++;
		u32 payload = (u32)vb2_get_plane_payload(&src->vb2_buf, 0);

		pr_info("vp9-run f=%u flags=0x%03x payload=%u last_ts=%llu golden_ts=%llu alt_ts=%llu profile=%u refmode=%u ctxidx=%u resetctx=%u show=%u\n",
			idx,
			fp->flags,
			payload,
			fp->last_frame_ts,
			fp->golden_frame_ts,
			fp->alt_frame_ts,
			fp->profile,
			fp->reference_mode,
			fp->frame_context_idx,
			fp->reset_frame_context,
			!!(fp->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME));
	}

	/*
	 * 2026-05-30 Round 3.1: broadened from "PARALLEL_DEC_MODE AND tiles"
	 * to "tiles alone" after bisection on the full 08-tile cluster.
	 *
	 * Individual-vector test results (after Round 3 original ship):
	 *   tile_1x2_frame_parallel  → 0 page faults (caught by reject)
	 *   tile_1x2                 → 52 page faults (NOT caught)
	 *   tile_1x4_frame_parallel  → 0
	 *   tile_1x4                 → 58 page faults
	 *   tile_1x8_frame_parallel  → 0
	 *   tile_1x8                 → 124 page faults
	 *   tile-4x1                 → 6 page faults
	 *   tile-4x4                 → 63 page faults
	 *
	 * Plain tile_NxN vectors have tile_cols/rows_log2 set but
	 * PARALLEL_DEC_MODE flag clear. They hit the same HW errata.
	 * Conclusion: the silicon bug is on the tile-decoding path
	 * regardless of frame-parallel mode. Reject any content
	 * with non-zero tile dimensions.
	 *
	 * Confirms 07-frame_parallel (parallel without tiles) was clean
	 * in Round 2.7 — parallel-mode alone isn't the trigger.
	 */
	/*
	 * 2026-05-30 Round 3.2 refinement: data shows tile_rows_log2 == 1
	 * (2 rows) is safe — 09-subpixel-00.ivf decodes correctly with
	 * that config (1/1 PASS prior to Round 3 reject). All other tile
	 * configurations seen so far fault:
	 *   tile_cols_log2 ∈ {1, 2, 3}, tile_rows_log2 == 0 → FAULT
	 *   tile_cols_log2 == 0, tile_rows_log2 ∈ {1, 2} → mixed:
	 *     1 = safe (09-subpixel), 2 = faults (08-tile-4x1)
	 *   tile_cols_log2 == 2, tile_rows_log2 == 2 (4x4) → faults
	 *
	 * Conservative refinement: reject when tile_cols_log2 > 0 OR
	 * tile_rows_log2 > 1. Allows 09-subpixel back.
	 */
	/*
	 * 2026-05-31 Round 16: empirical re-test of Round 3 reject against
	 * YouTube 1080p VP9 (tile_cols_log2=2) on link mode with the
	 * Round 9.1 single-region RCB in place. Lifted reject → 474 decode
	 * attempts ran end-to-end:
	 *   irq_clean=3   silent=235   real_to=236   spurious=61
	 *   silent_clean_wb=12   silent_dec_only=14   silent_no_wb=209
	 *   IOMMU page faults = 0   kernel oops = 0   box crash = 0
	 *
	 * Two facts proven simultaneously:
	 *   (1) Silicon errata is real on this content class. HW state
	 *       machine never advances LINK_DEC_NUM on ~94% of attempts;
	 *       only ~6% produce any writeback at all. Lifting the reject
	 *       does NOT give us valid pixels — the reject is necessary.
	 *   (2) Round 9.1 single-region RCB IS the correct architectural
	 *       fix for accumulation crashes — sustained ~470 attempts on
	 *       broken silicon with zero IOMMU damage. Without R9.1 the
	 *       same test crashed the box within seconds (pre-R9.1 data).
	 *
	 * Conclusion: Round 3 reject stays. VDPU383 VP9 hardware decode is
	 * untiled-content-only; YouTube uses tiles for ≥480p transcodes
	 * so YouTube high-res VP9 cannot be hardware-decoded on RK3576.
	 * The viable YouTube hardware path is H.264 — gated on the
	 * VDPU383 H.264 driver bug fix (see rkvdec_vdpu383_h264_bug_proven
	 * memory). See SESSION_2026-05-31_R16.md for full evidence log.
	 */
	/*
	 * 2026-05-31 R3 reject restored after R19 tile-test.
	 *
	 * Result chain this session on YouTube 1080p VP9 4-col tile content:
	 *   R16 baseline (lifted reject, no other change): 474 attempts,
	 *      29 writebacks (6.1%), 209 silent_no_wb, 0 IOMMU faults
	 *   R18 (color_space_last gbl_buf fix added):     506 attempts,
	 *      16 writebacks (3.2%) — marginally regressed or within noise
	 *   R19 (R18 + BSP-style minimal descriptor refill in rkvdec-link.c
	 *        rkvdec_link_fill_descriptor):            503 attempts,
	 *      16 writebacks (3.2%) — unchanged vs R18
	 *
	 * Net: descriptor write SEQUENCE change has zero impact on tile
	 * decode rate, falsifying the "HW prefetcher races our whole-node
	 * memset" hypothesis. R9.1 single-region RCB protection held
	 * throughout — 1500+ attempts across R16/R18/R19 without crash.
	 *
	 * The rkvdec-link.c BSP-style fill is kept (it's strictly more
	 * MPP-faithful and cheaper than the prior whole-node memset);
	 * just doesn't fix this bug. The R18 color_space_last parse
	 * + use is also kept for future correctness once we're sure
	 * other fields are right.
	 *
	 * Still open: aux-buffer content (prob/segid/kfprob bytes for
	 * tile content vs MPP); GBL byte-diff on tile content (only
	 * confirmed identical for sintel non-tile); deep-queue batching
	 * (task_capacity 1 vs 16); HW reset/init register sequence diff.
	 */
	/*
	 * 2026-05-31 R3 reject restored after R20+R21 tests.
	 *
	 * Full result chain on YouTube 1080p VP9 4-col tile content
	 * (all tests R3 reject lifted, with R9.1 single-region RCB providing
	 * IOMMU-fault containment so HW errata stay non-fatal):
	 *
	 *   R16 baseline:                    474 total / 6.1% writeback
	 *   R18 (color_space_last fix):      506 total / 3.2% writeback
	 *   R19 (R18 + BSP-style fill):      503 total / 3.2% writeback
	 *   R21 (R19 + BATCH_TARGET=8):      509 total / 2.9% writeback
	 *
	 * Plus R20 (GBL byte-diff against MPP on Fluster 08-tile_1x8):
	 *   All 10 frames (KEY + INTER) byte-identical, 0 mismatches.
	 *
	 * Definitively ruled out as bug source for tile content:
	 *   - silicon errata (R17: MPP decodes 1080p60+4K60 tile cleanly)
	 *   - programmed register values (R18 mmio diff)
	 *   - GBL buffer content (R20 byte-diff all 10 frames)
	 *   - descriptor write sequence (R19 BSP-style fill)
	 *   - link-mode batching depth (R21 batch=8 doesn't help)
	 *
	 * Remaining candidates:
	 *   - aux DMA buffer content MPP populates we don't (prob_default
	 *     bytes, kf_probs bytes, segid maps, colmv buffer initial state)
	 *   - HW-internal register state at IP init time MPP sets that we
	 *     miss in rkvdec_link_init / module load path
	 *   - cache coherency of descriptor + aux buffers (dma_alloc_coherent
	 *     might not be honored as uncached on RK3576)
	 *
	 * See SESSION_2026-05-31_R18_REGDIFF.md for full evidence log.
	 */
	/*
	 * 2026-06-01 R3 reject restored after R22 (prob byte-diff) capture.
	 *
	 * R22 result: prob_kf (1312 bytes) AND prob_default (4864 bytes)
	 * both byte-identical to MPP on Fluster 08-tile_1x8 frame 0 (KEY).
	 * Combined with prior R20 (GBL all 10 frames byte-identical) and
	 * R18 (every programmed register byte-identical) — *every* byte
	 * we hand HW for frame 0 KEY matches MPP exactly. The bug is
	 * conclusively in HW state setup / session init register writes
	 * that MPP does and we don't.
	 *
	 * See SESSION_2026-06-01_R22_PROB_DIFF.md.
	 */
	/* 2026-06-01 R23 test: reject lifted. If warmup unblocked tile
	 * decode, telem should jump from R16's 6% writeback to >90%. */
	if (fp->tile_cols_log2 || fp->tile_rows_log2 > 1)
		dev_info_ratelimited(rkvdec->dev,
			"vp9: R23 tile-test tile_cols_log2=%u tile_rows_log2=%u\n",
			fp->tile_cols_log2, fp->tile_rows_log2);

	/*
	 * 2026-05-31 Round 14: sub-100 dimension reject under link mode.
	 *
	 * Discovered when measuring the steady-state LM aggregate
	 * (after Round 12 v3 warmup investigation). On a cold module
	 * load the 02-size cluster (vp90-2-02-size-*, tiny 8-96
	 * pixel-wide frames) decodes 0/71 cleanly — HW writes
	 * partial pixels with no SoC issue. But after the module is
	 * warmed (any prior LM cluster has run), running cluster 02
	 * HARD-HANGS the SoC: no flashing system LED, no SSH, no
	 * kernel oops, full watchdog reboot required.
	 *
	 * Root cause unclear without datasheet, but the boundary is
	 * width < ~100 pixels combined with persistent LM module
	 * state. Mirrors the Round 3 tile-content reject pattern:
	 * graceful -EINVAL prevents the hang while userspace gets
	 * a regular decode-failure signal.
	 *
	 * Only applies under rkvdec_link_mode=1. Single-shot path
	 * decodes 02-size content correctly (per Round 7 finding:
	 * HW writes valid pixels, only userspace pipeline mishandles
	 * the unusual bytesperline=192 alignment).
	 */
	if (rkvdec_link_mode) {
		u32 frame_w = (u32)fp->frame_width_minus_1 + 1;
		u32 frame_h = (u32)fp->frame_height_minus_1 + 1;

		if (frame_w < 100 || frame_h < 100) {
			dev_warn_ratelimited(rkvdec->dev,
				"vp9: LM rejecting sub-100-dim content (warm-state SoC-hang risk; %ux%u)\n",
				frame_w, frame_h);
			rkvdec_run_postamble(ctx, &run);
			return -EINVAL;
		}
	}

	/*
	 * 2026-05-30 Round 4.2: dimension-mismatch reject against
	 * ctx->rcb_config dims (not V4L2 decoded_fmt). The previous
	 * attempt (Round 4.1, reverted in 574f3d2) compared against
	 * decoded_fmt and never fired — by vp9_run time V4L2 has
	 * already renegotiated. But RCB was allocated for whatever
	 * the dimensions were at the LAST allocation time, which on
	 * 14-resize content can lag behind the current frame.
	 */
	{
		struct rkvdec_rcb_config *rcb = ctx->rcb_config;
		u32 frame_w = (u32)fp->frame_width_minus_1 + 1;
		u32 frame_h = (u32)fp->frame_height_minus_1 + 1;

		if (rcb && (frame_w > rcb->width || frame_h > rcb->height)) {
			dev_warn_ratelimited(rkvdec->dev,
				"vp9: rejecting rcb mismatch (frame %ux%u > rcb %ux%u) — userspace needs SOURCE_CHANGE\n",
				frame_w, frame_h, rcb->width, rcb->height);
			rkvdec_run_postamble(ctx, &run);
			return -EINVAL;
		}

		/*
		 * 2026-05-31 Round 5: resize-event reject. Track per-context
		 * frame dimensions; if the current frame's dimensions differ
		 * from the previous frame's, mark the context as resize-
		 * tainted and reject all subsequent frames in this stream.
		 * Forces userspace to renegotiate via SOURCE_CHANGE → fresh
		 * stream with consistent dimensions throughout.
		 *
		 * Goal: turn 14-resize content from "decodes 1/5 frames but
		 * batch-crashes box via IOMMU corruption" into "decodes
		 * 0/N cleanly, no HW damage." Matches the shape of the
		 * Round 3.1/3.2 tile reject — graceful failure instead
		 * of HW errata trigger.
		 */
		if (vp9_ctx->resize_seen) {
			dev_warn_ratelimited(rkvdec->dev,
				"vp9: stream resize-tainted, rejecting (frame %ux%u)\n",
				frame_w, frame_h);
			rkvdec_run_postamble(ctx, &run);
			return -EINVAL;
		}

		/* Round 5 diagnostic: log every frame's dimensions. */
		dev_info_ratelimited(rkvdec->dev,
			"vp9 frame: %ux%u (prev %ux%u render %ux%u fmt %ux%u rcb %ux%u)\n",
			frame_w, frame_h,
			vp9_ctx->prev_frame_w, vp9_ctx->prev_frame_h,
			(u32)fp->render_width_minus_1 + 1,
			(u32)fp->render_height_minus_1 + 1,
			ctx->decoded_fmt.fmt.pix_mp.width,
			ctx->decoded_fmt.fmt.pix_mp.height,
			rcb ? rcb->width : 0, rcb ? rcb->height : 0);

		if (vp9_ctx->prev_frame_w &&
		    (frame_w != vp9_ctx->prev_frame_w ||
		     frame_h != vp9_ctx->prev_frame_h)) {
			vp9_ctx->resize_seen = true;
			dev_warn_ratelimited(rkvdec->dev,
				"vp9: resize event detected (%ux%u -> %ux%u), rejecting and tainting stream\n",
				vp9_ctx->prev_frame_w, vp9_ctx->prev_frame_h,
				frame_w, frame_h);
			rkvdec_run_postamble(ctx, &run);
			return -EINVAL;
		}

		vp9_ctx->prev_frame_w = frame_w;
		vp9_ctx->prev_frame_h = frame_h;
	}

	ret = vdpu383_vp9_prepare_frame(ctx, src, dst, fp);
	if (ret) {
		rkvdec_run_postamble(ctx, &run);
		return ret;
	}

	/* IP-block cache config + clear + max-outstanding-reads, written
	 * before per-frame parameters. The vendor driver clears CLR_CACHE0
	 * here to prevent stale entries from a previous decode contaminating
	 * the next. Offsets are relative to the function-region base
	 * (0x27b00100):
	 *   0x410 = CLR_CACHE0    -> 1 (clear)
	 *   0x418 = MAX_READS     -> 0x1c (28 outstanding reads)
	 *   0x41c = CACHE0_SIZE   -> CACHEABLE | READ_ALLOC | LINE_64
	 *
	 * Session-s (BSP source diff, FALSIFIED) — BSP writes at
	 * different offsets (0x510/0x518/0x51c) AND clears CACHE1
	 * (0x550/0x55c) + CACHE2 (0x590/0x59c). Adding those writes
	 * alongside the existing 0x41x writes REGRESSED Fluster
	 * (89.5 vs 98 with reg168 reset alone). The additional
	 * MMIO writes appear to disrupt HW state in some way.
	 * Kept the historical writes only.
	 */
	/* vp9_cache_all: 0=CACHE0 only (default), 1=all 3 caches (SIZE+CLEAR),
	 * 2=skip all cache writes, 3=enable CACHE1/2 SIZE/permit but do NOT clear
	 *   them (2026-06-10 throughput: warm the reference caches without the
	 *   per-frame cold-clear that doubles 4K decode time). */
	if (vp9_cache_all != 2) {
		writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x41c); /* CACHE0_SIZE */
		if (vp9_cache_all == 1 || vp9_cache_all == 3) {
			writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x45c); /* CACHE1_SIZE */
			writel(0x1u | 0x2u | 0x10u, rkvdec->regs + 0x49c); /* CACHE2_SIZE */
		}
		writel(0x1u, rkvdec->regs + 0x410);                /* CLR_CACHE0 */
		if (vp9_cache_all == 1) {
			writel(0x1u, rkvdec->regs + 0x450);        /* CLR_CACHE1 */
			writel(0x1u, rkvdec->regs + 0x490);        /* CLR_CACHE2 */
		}
		writel(0x1cu, rkvdec->regs + 0x418);               /* MAX_READS */
	}

	/*
	 * R44 (2026-06-01) — alt-ref register dump. When module param
	 * is set, dump each region of vp9_ctx->regs just before MMIO
	 * write, but only for alt-ref / hidden frames (SHOW_FRAME bit
	 * clear). Bistability means we'll get a successful alt-ref
	 * dump and a hung alt-ref dump within a few trials; diffing
	 * them localises the divergence (if any) at register level.
	 */
	/*
	 * R45 (2026-06-01) — soft-reset HW IP before alt-ref decode.
	 * Done BEFORE register write so the post-reset IP picks up our
	 * fresh register state. Replicates the soft-reset sequence from
	 * the IRQ handler error path.
	 */
	if (r45_reset_before_altref &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME)) {
		u32 ip_en = readl(rkvdec->link + VDPU383_LINK_IP_ENABLE);
		int poll;

		writel(ip_en | BIT(15), rkvdec->link + VDPU383_LINK_IP_ENABLE);
		writel(BIT(0), rkvdec->link + 0x44);
		for (poll = 0; poll < 50; poll++) {
			if (readl(rkvdec->link + VDPU383_LINK_STA_INT) & BIT(11))
				break;
			udelay(10);
		}
		writel(FIELD_PREP_WM16(BIT(11), 0),
		       rkvdec->link + VDPU383_LINK_STA_INT);
		writel(FIELD_PREP_WM16(0xffff, 0),
		       rkvdec->link + VDPU383_LINK_INT_EN);
		writel(FIELD_PREP_WM16(VDPU383_STA_INT_ALL, 0),
		       rkvdec->link + VDPU383_LINK_STA_INT);
		writel(ip_en, rkvdec->link + VDPU383_LINK_IP_ENABLE);
	}

	if (r44_dump_altref_regs &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
	    !(fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME)) {
		u32 idx = ctx->dev->accum_frames_run;

		pr_info("R44 altref f=%u flags=0x%03x payload=%u =====\n",
			idx, fp->flags,
			(u32)vb2_get_plane_payload(&src->vb2_buf, 0));
		print_hex_dump(KERN_INFO, "R44 common: ", DUMP_PREFIX_OFFSET,
			       16, 4, &vp9_ctx->regs.common,
			       sizeof(vp9_ctx->regs.common), false);
		print_hex_dump(KERN_INFO, "R44 h26x: ", DUMP_PREFIX_OFFSET,
			       16, 4, &vp9_ctx->regs.h26x_params,
			       sizeof(vp9_ctx->regs.h26x_params), false);
		print_hex_dump(KERN_INFO, "R44 caddr: ", DUMP_PREFIX_OFFSET,
			       16, 4, &vp9_ctx->regs.common_addr,
			       sizeof(vp9_ctx->regs.common_addr), false);
		print_hex_dump(KERN_INFO, "R44 vp9a: ", DUMP_PREFIX_OFFSET,
			       16, 4, &vp9_ctx->regs.vp9_addr,
			       sizeof(vp9_ctx->regs.vp9_addr), false);
	}

	if (vp9_dump_ctrls)
		pr_info("vp9-addr f=%u flags=0x%03x decout=%08x colmv_cur=%08x payload_cur=%08x segid_cur=%08x updprob=%08x last=%08x gold=%08x alt=%08x\n",
			ctx->dev->accum_frames_run, fp->flags,
			vp9_ctx->regs.vp9_addr.reg168_decout_base,
			vp9_ctx->regs.vp9_addr.reg216_colmv_cur_base,
			vp9_ctx->regs.vp9_addr.reg192_payload_cur_base,
			vp9_ctx->regs.vp9_addr.reg182_segid_cur_base,
			vp9_ctx->regs.vp9_addr.reg185_updateprob_base,
			vp9_ctx->regs.vp9_addr.reg170_last_ref_base,
			vp9_ctx->regs.vp9_addr.reg171_golden_ref_base,
			vp9_ctx->regs.vp9_addr.reg172_altref_ref_base);

	vdpu383_vp9_write_regs(ctx);

	/*
	 * Diagnostic dump of the four assembled register substructs. Compares
	 * against MPP's regs_full.dat (Vdpu383RegSet) for layout/value diff.
	 * Substruct ↔ MPP region mapping:
	 *   common      (regs 8-30,   92B) ↔ MPP ctrl_regs       (92B)
	 *   h26x_params (regs 64-106, 172B) ↔ MPP comm_paras      (172B)
	 *   common_addr (regs 128-161, 136B) ↔ MPP comm_addrs[0..136]
	 *   vp9_addr    (regs 168-232, 260B) ↔ MPP comm_addrs[160..420]
	 *     (MPP has 24B for regs 162-167 we don't model.)
	 */
#if VP9_DUMP_REGS
	if (vp9_dump_frame_no <= 2) {
		struct vdpu383_regs_vp9 *r = &vp9_ctx->regs;

		pr_info("VP9DUMP Frame%04u common %zuB\n",
			vp9_dump_frame_no, sizeof(r->common));
		print_hex_dump(KERN_INFO, "VP9DUMP-COMMON: ",
			       DUMP_PREFIX_OFFSET, 16, 4,
			       &r->common, sizeof(r->common), false);
		pr_info("VP9DUMP Frame%04u h26x_params %zuB\n",
			vp9_dump_frame_no, sizeof(r->h26x_params));
		print_hex_dump(KERN_INFO, "VP9DUMP-PARAMS: ",
			       DUMP_PREFIX_OFFSET, 16, 4,
			       &r->h26x_params, sizeof(r->h26x_params), false);
		pr_info("VP9DUMP Frame%04u common_addr %zuB\n",
			vp9_dump_frame_no, sizeof(r->common_addr));
		print_hex_dump(KERN_INFO, "VP9DUMP-COMMADDR: ",
			       DUMP_PREFIX_OFFSET, 16, 4,
			       &r->common_addr, sizeof(r->common_addr), false);
		pr_info("VP9DUMP Frame%04u vp9_addr %zuB\n",
			vp9_dump_frame_no, sizeof(r->vp9_addr));
		print_hex_dump(KERN_INFO, "VP9DUMP-VP9ADDR: ",
			       DUMP_PREFIX_OFFSET, 16, 4,
			       &r->vp9_addr, sizeof(r->vp9_addr), false);
		pr_info("VP9DUMP Frame%04u gbl %dB\n",
			vp9_dump_frame_no, VDPU383_VP9_GBL_SIZE);
		print_hex_dump(KERN_INFO, "VP9DUMP-GBL: ",
			       DUMP_PREFIX_OFFSET, 16, 1,
			       vp9_ctx->gbl_buf.cpu, VDPU383_VP9_GBL_SIZE, false);
		if (vp9_dump_frame_no == 0 && vp9_ctx->priv_tbl.cpu) {
			struct rkvdec_vdpu383_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;

			pr_info("VP9DUMP Frame%04u prob_default %dB\n",
				vp9_dump_frame_no, RKVDEC_VP9_PROBE_SIZE);
			print_hex_dump(KERN_INFO, "VP9DUMP-PROBDEF: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       tbl->prob_default, RKVDEC_VP9_PROBE_SIZE,
				       false);
		}
		/*
		 * Dump the PRE-DECODE state of prob_loop[0] for Frame 1+. This
		 * is what HW will read as reg184_lastprob_base for INTER decode.
		 * It contains whatever the previous frame's HW writeback put
		 * there. If this differs from MPP's prob_loop_ctx0.dat at the
		 * same frame, the HW-side prob update path is divergent.
		 */
		if (vp9_dump_frame_no >= 1 && vp9_dump_frame_no <= 2 &&
		    vp9_ctx->priv_tbl.cpu) {
			struct rkvdec_vdpu383_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;

			pr_info("VP9DUMP Frame%04u prob_loop0_pre %dB\n",
				vp9_dump_frame_no, RKVDEC_VP9_PROBE_SIZE);
			print_hex_dump(KERN_INFO, "VP9DUMP-PROBLOOP0PRE: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       tbl->probs.probs[0], RKVDEC_VP9_PROBE_SIZE,
				       false);
		}
		/*
		 * Segid buffer dumps (pre-decode state for Frame 1+). reg181/
		 * reg182 swap each frame between segmap[0] and segmap[1].
		 * Only dump first 64 B per buffer — first SB-row covers a
		 * 64x64 frame; whole segmap is 73728 B.
		 */
		if (vp9_dump_frame_no == 1 && vp9_ctx->priv_tbl.cpu) {
			struct rkvdec_vdpu383_vp9_priv_tbl *tbl = vp9_ctx->priv_tbl.cpu;
			u32 segdump_len = 64;

			pr_info("VP9DUMP Frame%04u segmap0_pre %uB\n",
				vp9_dump_frame_no, segdump_len);
			print_hex_dump(KERN_INFO, "VP9DUMP-SEGMAP0: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       tbl->segmap[0], segdump_len, false);
			pr_info("VP9DUMP Frame%04u segmap1_pre %uB\n",
				vp9_dump_frame_no, segdump_len);
			print_hex_dump(KERN_INFO, "VP9DUMP-SEGMAP1: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       tbl->segmap[1], segdump_len, false);
		}
		/*
		 * Stream-input dump for Frame 0 and Frame 1 only. Reads the
		 * VP9 bitstream content HW will fetch via reg128_strm_base,
		 * starting at the offset HW actually uses (uncomp_len_aligned).
		 * Compares against MPP's per-frame stream_in.dat.
		 */
		if (vp9_dump_frame_no <= 1) {
			u32 sin_uncomp = fp->uncompressed_header_size;
			u32 sin_comp = fp->compressed_header_size;
			u32 sin_strm = vb2_get_plane_payload(&src->vb2_buf, 0);
			u32 sin_uncomp_aligned = sin_uncomp & ~0xfu;
			u32 sin_dump_len = min_t(u32,
						 ((sin_strm + 15) & ~15u) + 0x80,
						 512);

			pr_info("VP9DUMP Frame%04u v4l2_ctrl uncomp=%u comp=%u strm=%u flags=0x%x ref=%llu/%llu/%llu\n",
				vp9_dump_frame_no, sin_uncomp, sin_comp,
				sin_strm, fp->flags,
				fp->last_frame_ts, fp->golden_frame_ts,
				fp->alt_frame_ts);
			dma_addr_t sin_dma =
				vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
			size_t sin_plane_sz =
				vb2_plane_size(&src->vb2_buf, 0);

			if (sin_uncomp_aligned + sin_dump_len <= sin_plane_sz) {
				struct iommu_domain *sin_dom =
					iommu_get_domain_for_dev(rkvdec->dev);
				phys_addr_t sin_ph = sin_dom ?
					iommu_iova_to_phys(sin_dom,
							   sin_dma + sin_uncomp_aligned) : 0;
				void *sin_cpu = sin_ph ?
					memremap(sin_ph, sin_dump_len,
						 MEMREMAP_WB) : NULL;

				if (sin_cpu) {
					pr_info("VP9DUMP Frame%04u stream_in %uB (strm_len=%u uncomp=%u)\n",
						vp9_dump_frame_no, sin_dump_len,
						sin_strm, sin_uncomp_aligned);
					print_hex_dump(KERN_INFO,
						       "VP9DUMP-STRMIN: ",
						       DUMP_PREFIX_OFFSET, 16, 1,
						       sin_cpu, sin_dump_len,
						       false);
					memunmap(sin_cpu);
				}
			}
		}
		vp9_dump_frame_no++;
	}
#endif

	/* Flush the IOMMU TLB between register writes and the HW kick. The
	 * vendor driver does this too: stale TLB entries from a previous
	 * decode would otherwise translate against now-invalid page-table
	 * entries and the IP block can fetch wrong data.
	 */
	/*
	 * Flush-only-after-restore (2026-06-05 throughput fix). A full per-frame
	 * TLB flush costs ~1.3-2.5 ms/frame and is redundant in steady state
	 * (vb2 IOVAs are stable frame-to-frame); only iommu_restore / queue
	 * (re)start can stale the TLB, and those set iommu_needs_flush.
	 * vp9_skip_tlb_flush overrides: 1=never, 2=always.
	 */
	{
		bool do_flush = (vp9_skip_tlb_flush == 2) ||
				(vp9_skip_tlb_flush != 1 && rkvdec->iommu_needs_flush);

		if (do_flush) {
			struct iommu_domain *dom =
				iommu_get_domain_for_dev(rkvdec->dev);

			if (dom)
				iommu_flush_iotlb_all(dom);
			rkvdec->iommu_needs_flush = false;
		}
	}

	/*
	 * Bug-A HW-state experiments (session-l 2026-05-25, both
	 * FALSIFIED):
	 *
	 * Phase 14 — per-frame IOMMU restore (empty_domain attach/detach)
	 *   before every VP9 INTER decode: no effect. Same Bug-A pattern
	 *   (KEY+f2 correct, f3+ all-zero). Cross-frame IOMMU state isn't
	 *   the missing piece.
	 *
	 * Phase 15 — IP-block soft-reset before every VP9 INTER decode
	 *   (the silent-completion recovery sequence): MADE THINGS WORSE.
	 *   Even VP9 f2 (show-alt-ref display, correct in baseline)
	 *   becomes all-zero. So HW pipeline state IS load-bearing
	 *   across frames and resetting wipes what f2's decode depends
	 *   on. Reset isn't the fix.
	 *
	 * Net finding: HW state at f3-time is BOTH valid (HW reads all
	 * the right things — confirmed sessions j/k) AND too-fragile
	 * to wipe (preserving it is necessary for f1/f2 to decode).
	 * The Bug-A trigger must be something about HOW HW processes f3
	 * specifically, not about state passed into it.
	 */

	pr_debug("rkvdec-vp9: %s frame ctx=%u dst=0x%08x\n",
		 (fp->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ? "KEY" : "INTER",
		 fp->frame_context_idx,
		 vp9_ctx->regs.vp9_addr.reg168_decout_base);

	rkvdec_run_postamble(ctx, &run);

	/* vp9_time diagnostic: stamp the single-shot kick for the DONE IRQ. */
	if (vp9_time)
		ctx->dev->vp9_kick_kt = ktime_get();

	timeout_threshold = vp9_ctx->regs.common.reg013_core_timeout_threshold;
	/* Phase 3 v0.3 step 2.6: stash for IRQ-side reschedule when an
	 * intermediate fire drains only part of a batched kick. */
	ctx->last_watchdog_threshold = timeout_threshold;
	/* Step A v2: reset poll counter and schedule the FIRST watchdog
	 * fire at the short (10 ms) poll interval. Telemetry showed
	 * VP9 link mode tasks silently complete in well under 80 ms;
	 * polling at 10 ms catches them ~6x faster. Watchdog handler
	 * reschedules itself at +10 ms while HW is still mid-decode,
	 * up to RKVDEC_MAX_WATCHDOG_POLLS before escalating to the
	 * full timeout fall-through. */
	ctx->watchdog_poll_count = 0;
	rkvdec_schedule_watchdog_poll(rkvdec);
	/* Preserved for posterity — the original threshold-based
	 * timeout is still computed and saved (above) for any IRQ
	 * path that wants to use it. */
	(void)timeout_threshold;

	/* Bug A: tiny show-alt-ref frames (<512B) always core_timeout on VDPU383.
	 * Override just the HW LINK threshold to ~0.9ms so the core resets fast;
	 * SW watchdog keeps VDPU383_TIMEOUT_4K for IRQ delivery safety margin.
	 *
	 * 2026-06-12 fable review: the <512 B boundary of the refs-bypassed
	 * decode mode (tiny frames perturb-immune, MC ignores all ref address
	 * registers) is EXACTLY this condition — the only stack-specific,
	 * kick-time, link-block write no register comparison ever covered
	 * (MPP has no tiny-frame special case). vp9_tiny_timeout=0 disables
	 * the override (normal resolution-based threshold for all frames)
	 * to test whether the override itself arms the degraded mode. */
	{
		u32 link_timeout = (vp9_tiny_timeout &&
				    vb2_get_plane_payload(&src->vb2_buf, 0) < 512)
			? (u32)vp9_tiny_timeout : timeout_threshold;
		writel(link_timeout, rkvdec->link + VDPU383_LINK_TIMEOUT_THRESHOLD);
	}
	/*
	 * Session-r finding (architectural): BSP rk_vcodec uses LINK
	 * MODE for VDPU383 VP9 decode — confirmed by ftrace on unit #2.
	 * BSP's per-frame call sequence shows only:
	 *   rkvdec2_link_process_task
	 *   rkvdec2_alloc_task
	 *   rkvdec2_link_enqueue.isra.0
	 *   rkvdec_vdpu383_link_irq
	 *   rkvdec2_link_worker
	 *   rkvdec2_link_wait_result
	 *
	 * BSP does NOT call its run function per frame. HW iterates
	 * through a descriptor list (link table) automatically.
	 *
	 * Our mainline V4L2 driver does single-shot per-frame decode
	 * with full register reprogramming + kick + wait. This is a
	 * fundamental architectural difference.
	 *
	 * Phase 16 falsified: skipping per-frame IP_ENABLE write after
	 * the first decode (relying on persistent enable) does NOT fix
	 * Bug-A. So the issue isn't simply about IP_ENABLE staying set;
	 * the LINK mode preserves additional HW state we don't have a
	 * straightforward way to retain in single-shot mode.
	 *
	 * Implementing real LINK mode in mainline V4L2 would be a
	 * substantial driver rewrite (descriptor list, async task
	 * queue, link IRQ handler). Out of scope as a quick fix.
	 */
	/* Phase 2 LINK mode: pack registers into the descriptor and enqueue
	 * via the BSP-style link path. Falls back to the single-shot kick
	 * below when the module param is 0 (default). vdpu383_vp9_write_regs
	 * above already pushed the same register values to MMIO via
	 * memcpy_toio — in link mode those writes are noise (HW reads from
	 * the descriptor) but harmless; keep them for now so a single-shot
	 * fallback after a HW error path doesn't have stale regs. */
	if (rkvdec_link_mode) {
		u32 reg_image[256] = { 0 };
		u32 first_slot = vp9_ctx->link_table.next_slot;
		u32 batched = 0;
		int err;

		/*
		 * Model flip (LINK_MODE_PORT_DESIGN_2026-06-01 §3): stash the
		 * submitting ctx so the completion paths (watchdog / IRQ) can
		 * find it — job_finish-at-submit clears m2m's curr_ctx, so
		 * v4l2_m2m_get_curr_priv() would return NULL when they fire.
		 */
		rkvdec->link_ctx = ctx;

		/* --- Task 1 (the m2m next_*_buf pair already in {src,dst}) --- */
		rkvdec_link_image_pack(reg_image,   8, &vp9_ctx->regs.common,
				       sizeof(vp9_ctx->regs.common));
		rkvdec_link_image_pack(reg_image,  64, &vp9_ctx->regs.h26x_params,
				       sizeof(vp9_ctx->regs.h26x_params));
		rkvdec_link_image_pack(reg_image, 128, &vp9_ctx->regs.common_addr,
				       sizeof(vp9_ctx->regs.common_addr));
		rkvdec_link_image_pack(reg_image, 168, &vp9_ctx->regs.vp9_addr,
				       sizeof(vp9_ctx->regs.vp9_addr));
		rkvdec_link_fill_descriptor(&vp9_ctx->link_table, first_slot,
					    reg_image);

		writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ,
				       VDPU383_INT_EN_IRQ),
		       rkvdec->link + VDPU383_LINK_INT_EN);

		/* Detach + push to inflight BEFORE kicking. If the order
		 * is inverted, the HW IRQ can fire between enqueue and
		 * push — the IRQ-side code then sees an empty inflight
		 * ring and crashes m2m via NULL src in run_preamble. */
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		err = rkvdec_inflight_push(ctx, src, dst, first_slot);
		if (err) {
			pr_warn_ratelimited("rkvdec-vp9: inflight push failed: %d\n",
					    err);
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
			rkvdec_run_postamble(ctx, &run);
			v4l2_m2m_job_finish(rkvdec->m2m_dev,
					    ctx->fh.m2m_ctx);
			return err;
		}
		batched = 1;

		/* Advance DPB state for task 1 — task 2's prepare_frame
		 * reads vp9_ctx->last and must see task 1's frame there. */
		vp9_ctx->last = vp9_ctx->cur;
		vp9_ctx->cur.segmapid ^= 1;

		/* --- Pre-fill additional tasks (depth>1 batching) ---
		 *
		 * For each pair we need to: detach src/dst from m2m,
		 * apply the request's controls (overwrites ctx->ctrl_hdl
		 * with this request's values — that's why we have to
		 * fully build + pack the previous task's descriptor first),
		 * call prepare_frame to build the regs + GBL, pack into
		 * the next descriptor slot, push to inflight, advance DPB
		 * state, and complete the request. HW will iterate
		 * through all packed descriptors on the single kick below.
		 */
		while (batched < RKVDEC_LINK_BATCH_TARGET &&
		       rkvdec_inflight_depth(ctx) <
			       RKVDEC_LINK_BATCH_TARGET &&
		       v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) > 0 &&
		       v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) > 0) {
			struct vb2_v4l2_buffer *next_src, *next_dst;
			struct media_request *next_req;
			struct v4l2_ctrl *next_ctrl;
			const struct v4l2_ctrl_vp9_frame *next_fp;
			u32 next_slot;
			int ret;

			next_src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
			next_dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
			if (!next_src || !next_dst)
				break;

			next_req = next_src->vb2_buf.req_obj.req;
			if (next_req)
				v4l2_ctrl_request_setup(next_req,
							&ctx->ctrl_hdl);

			next_ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
				V4L2_CID_STATELESS_VP9_FRAME);
			if (!next_ctrl) {
				if (next_req)
					v4l2_ctrl_request_complete(
						next_req, &ctx->ctrl_hdl);
				break;
			}
			next_fp = next_ctrl->p_cur.p;

			v4l2_m2m_buf_copy_metadata(next_src, next_dst);

			ret = vdpu383_vp9_prepare_frame(ctx, next_src,
							next_dst, next_fp);
			if (ret) {
				if (next_req)
					v4l2_ctrl_request_complete(
						next_req, &ctx->ctrl_hdl);
				break;
			}

			next_slot = (first_slot + batched) %
				    vp9_ctx->link_table.task_capacity;
			memset(reg_image, 0, sizeof(reg_image));
			rkvdec_link_image_pack(reg_image,   8,
				&vp9_ctx->regs.common,
				sizeof(vp9_ctx->regs.common));
			rkvdec_link_image_pack(reg_image,  64,
				&vp9_ctx->regs.h26x_params,
				sizeof(vp9_ctx->regs.h26x_params));
			rkvdec_link_image_pack(reg_image, 128,
				&vp9_ctx->regs.common_addr,
				sizeof(vp9_ctx->regs.common_addr));
			rkvdec_link_image_pack(reg_image, 168,
				&vp9_ctx->regs.vp9_addr,
				sizeof(vp9_ctx->regs.vp9_addr));
			rkvdec_link_fill_descriptor(&vp9_ctx->link_table,
						    next_slot, reg_image);

			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
			err = rkvdec_inflight_push(ctx, next_src, next_dst,
						   next_slot);
			if (err) {
				pr_warn_ratelimited("rkvdec-vp9: batched inflight push failed: %d\n",
						    err);
				v4l2_m2m_buf_done(next_src,
					VB2_BUF_STATE_ERROR);
				v4l2_m2m_buf_done(next_dst,
					VB2_BUF_STATE_ERROR);
				if (next_req)
					v4l2_ctrl_request_complete(
						next_req, &ctx->ctrl_hdl);
				break;
			}

			vp9_ctx->last = vp9_ctx->cur;
			vp9_ctx->cur.segmapid ^= 1;

			if (next_req)
				v4l2_ctrl_request_complete(next_req,
							   &ctx->ctrl_hdl);

			batched++;
		}

		/*
		 * NB: per-kick iommu_flush_iotlb_all (BSP's en_sw_iommu_zap)
		 * tested 2026-06-01 — did NOT change the bistability (~30% bad
		 * on allkey/noalt/smpte), so the runaway isn't a stale-TLB
		 * descriptor fetch. Not added (heavy, no benefit).
		 */

		/* --- Single kick covers all `batched` descriptors --- */
		err = rkvdec_link_enqueue_vdpu383(&vp9_ctx->link_table,
						   first_slot, batched,
						   rkvdec->link);
		vp9_ctx->link_table.next_slot =
			(first_slot + batched) %
			vp9_ctx->link_table.task_capacity;
		if (err) {
			u32 i;

			pr_warn_ratelimited("rkvdec-vp9: link enqueue failed: %d (batched=%u)\n",
					    err, batched);
			for (i = 0; i < batched; i++) {
				struct rkvdec_link_inflight e =
					rkvdec_inflight_pop(ctx);

				if (e.src && e.dst) {
					v4l2_m2m_buf_done(e.src,
						VB2_BUF_STATE_ERROR);
					v4l2_m2m_buf_done(e.dst,
						VB2_BUF_STATE_ERROR);
				}
			}
			rkvdec_run_postamble(ctx, &run);
			v4l2_m2m_job_finish(rkvdec->m2m_dev,
					    ctx->fh.m2m_ctx);
			return err;
		}

		/*
		 * Model flip (LINK_MODE_PORT_DESIGN_2026-06-01 §3): finish the
		 * m2m job at SUBMIT, not at completion. m2m is now free to
		 * dispatch the next device_run immediately (gated by job_ready
		 * at rkvdec_link_depth tasks in flight), so HW can be fed a new
		 * descriptor via the ADD_MODE append path while still armed on
		 * this one. The buffers parked in the inflight ring are returned
		 * later by the watchdog reap (rkvdec_link_reap); the pm ref this
		 * submit took (in rkvdec_device_run) is held until that reap.
		 */
		v4l2_m2m_job_finish(rkvdec->m2m_dev, ctx->fh.m2m_ctx);
		return 0;
	}

	/* Single-shot path (default; preserved verbatim). */

	/*
	 * R42b (2026-06-01) — process-context pre-kick settle delay.
	 * Empirical observation: pr_info in the IRQ handler is
	 * load-bearing because it provides ~50 us of settle time
	 * between IRQ ack and next kick. Trying udelay in IRQ context
	 * crashed the box (R42). This places the delay safely in
	 * process context, before the kick on the receiving side.
	 *
	 * Module param r42_irq_settle_us controls the delay (default
	 * 50). Set to 0 to disable.
	 */
	if (r42_irq_settle_us > 0)
		usleep_range(r42_irq_settle_us, r42_irq_settle_us + 10);

	/*
	 * 2026-06-09 input-equivalence: CRC the ENTROPY INPUT prob buffer
	 * (probs[frame_context_idx] = what reg184 hands HW) right before the kick,
	 * to byte-compare against MPP's cabac_last/update for the compound frame.
	 * If an earlier prob (coef/skip/tx/inter-mode) differs from MPP, the
	 * arithmetic decoder desyncs before the compound decision. Segment CRCs
	 * localise any divergence. Gated on vp9_adapt_dump.
	 */
	if (vp9_adapt_dump) {
		struct rkvdec_vdpu383_vp9_priv_tbl *t = vp9_ctx->priv_tbl.cpu;
		u32 c = vp9_ctx->cur.frame_context_idx;

		if (t && c < VP9_NUM_FRAME_CTX) {
			u8 *ip = t->probs.probs[c];

			pr_info("rkvdec-vp9 input-crc: refmode=%u ctx=%u all2432=%08x s0=%08x s1=%08x s2=%08x\n",
				vp9_ctx->cur.reference_mode, c,
				crc32(0, ip, 2432),
				crc32(0, ip, 512),
				crc32(0, ip + 512, 512),
				crc32(0, ip + 1024, 1408));
			/* localise the s0 divergence: bytes 48..223 (seg/skip/tx/is_inter/
			 * y_mode/comp_mode/comp_ref/single_ref/inter_mode region) */
			/* one-shot full byte dump of the first compound (refmode==2)
			 * frame's entropy input — for a CRC-independent byte-for-byte
			 * diff against MPP cabac_last */
			{
				static int dumped_compound;
				if (vp9_ctx->cur.reference_mode == 2 && !dumped_compound) {
					dumped_compound = 1;
					print_hex_dump(KERN_INFO, "vp9f2in ",
						DUMP_PREFIX_OFFSET, 32, 1, ip, 2432, false);
				}
			}
		}
	}

	/* Cat 6.1 reserved-bit fuzz: OR a mask into a common register on the
	 * compound frame, last thing before the kick (after all normal writes). */
	if (vp9_regfuzz_reg >= 0 && vp9_ctx->cur.reference_mode == 2) {
		void __iomem *fa = rkvdec->regs + VDPU383_OFFSET_COMMON_REGS +
				   vp9_regfuzz_reg * 4;
		u32 before = readl(fa);

		writel(before | vp9_regfuzz_or, fa);
		pr_info("VP9-REGFUZZ: reg%03d off=%#x before=%08x or=%08x after=%08x\n",
			8 + vp9_regfuzz_reg,
			VDPU383_OFFSET_COMMON_REGS + vp9_regfuzz_reg * 4,
			before, vp9_regfuzz_or, readl(fa));
	}

	/* 2026-06-10 E1: flush the cached RCB to device before the kick (no-op
	 * unless rcb_cached). Mirrors the BSP per-frame dma_sync over its
	 * dma-buf-cache context buffers — VDPU383_LIFECYCLE_MAP §3.2/3.3. */
	rkvdec_rcb_sync_for_device(ctx);

	writel(VDPU383_IP_CRU_MODE, rkvdec->link + VDPU383_LINK_IP_ENABLE);
	/* 2026-06-12 fable review — BSP arms irq_mask 0x30000 (INT_EN bit0 +
	 * LINE_IRQ bit1) for every task in both its modes; our single-shot
	 * inherits whatever INT_EN was left armed (bit0 via ISR re-arm).
	 * Gated test of whether the LINE_IRQ enable is load-bearing for the
	 * VP9 tiny-frame refs-bypass (the last never-compared LINK-surface
	 * difference). */
	if (vp9_int_lineirq)
		writel(FIELD_PREP_WM16(VDPU383_INT_EN_IRQ |
				       VDPU383_INT_EN_LINE_IRQ,
				       VDPU383_INT_EN_IRQ |
				       VDPU383_INT_EN_LINE_IRQ),
		       rkvdec->link + VDPU383_LINK_INT_EN);
	/* wmb() + readl(reg15) before the HW kick: drain posted writes so
	 * the IP block sees all register state in order. A plain mb() is
	 * over-strong here (it can stall the AXI bus); wmb followed by a
	 * read-back of a register the IP block has just been written gives
	 * the same ordering guarantee.
	 */
	wmb();
	(void)readl(rkvdec->regs + VDPU383_OFFSET_COMMON_REGS + 7 * 4);
	writel(VDPU383_DEC_E_BIT,   rkvdec->link + VDPU383_LINK_DEC_ENABLE);

	/* Phase 3 v0.3 step 1: DPB state-advance moved here from done(). */
	vp9_ctx->last = vp9_ctx->cur;
	vp9_ctx->cur.segmapid ^= 1;
	return 0;
}

/* -----------------------------------------------------------------------
 * V4L2 control setup
 */
/* Non-static so rkvdec.c's vdpu383_coded_fmts[] entry can reach it.
 *
 * VP9_FRAME does NOT carry .cfg.ops here even though our get_image_fmt
 * inspects it: the V4L2 framework only delivers s_ctrl callbacks for
 * SET operations, but ffmpeg's v4l2_request hwaccel sets the VP9 FRAME
 * control AFTER CAPTURE buffers are allocated. Auto-switching image_fmt
 * at that point would return -EBUSY (the CAPTURE queue is busy) and the
 * hwaccel gives up entirely.
 *
 * The supported fix path: userspace must select V4L2_PIX_FMT_NV15 on
 * CAPTURE before STREAMON for 10-bit content. ffmpeg v4l2_request would
 * need a small patch to do this; for now NV15 is enumerable and the
 * driver honours an explicit selection. 8-bit content continues to work
 * with the default NV12 path.
 */
const struct rkvdec_ctrls rkvdec_vdpu383_vp9_ctrls = {
	.ctrls = (const struct rkvdec_ctrl_desc[]) {
		{
			.cfg.id = V4L2_CID_STATELESS_VP9_FRAME,
			/* 2026-05-28: wire ctrl_ops so rkvdec_s_ctrl fires
			 * when userspace sets the VP9 FRAME control. Enables
			 * both bit-depth-driven CAPTURE format switch (NV12
			 * for 8-bit, NV15 for 10-bit) and the resize
			 * source_change event.
			 *
			 * Tested 2026-05-28 to confirm this wiring did NOT
			 * cause the single-shot BBB200 regression observed
			 * earlier today (4.5 s -> 14-20 s with real_to=100).
			 * With .cfg.ops reverted, the regression persisted.
			 * Root cause was the Step A watchdog poll being
			 * gated to link mode only; single-shot's HW decodes
			 * fell into the 10 ms race window. Fixed by removing
			 * the link-only gate in the watchdog handler. */
			.cfg.ops = &rkvdec_ctrl_ops,
		},
		{ .cfg.id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR, },
	},
	.num_ctrls = 2,
};

/* Non-static so rkvdec.c can size-extern this array.
 *
 * VP9 Profile 0 produces 4:2:0 8-bit (NV12).
 * VP9 Profile 2 produces 4:2:0 10-bit. The matching V4L2 fourcc on
 * Rockchip hardware is NV15 (10-bit packed luma + interleaved 10-bit
 * packed chroma, 4 pixels per 5 bytes — same convention HEVC and
 * H.264 use on this driver). Selection between the two is made by
 * rkvdec_vp9_get_image_fmt() per frame based on the V4L2 VP9 control
 * `bit_depth` field, and the m2m framework triggers a source_change
 * event when the value changes (e.g. profile transition mid-session).
 */
const struct rkvdec_decoded_fmt_desc rkvdec_vdpu383_vp9_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.image_fmt = RKVDEC_IMG_FMT_420_8BIT,
	},
	{
		/*
		 * 10-bit output. VDPU383 writes 10-bit luma/chroma packed
		 * 4-samples-per-5-bytes (NV15), the Rockchip-vendor 10-bit
		 * NV12-variant. V4L2_PIX_FMT_NV15 is the mainline fourcc
		 * for this layout (4cc 'NV15', added to videodev2.h for
		 * Rockchip MPP). Userspace decoders need explicit support
		 * to consume it (gst-plugins-bad's v4l2 plugin does not
		 * yet advertise NV15 caps; this is a documented userspace
		 * gap, not a driver bug).
		 *
		 * sizeimage computation in rkvdec.c::rkvdec_reset_decoded_fmt
		 * already handles the NV15 byte ratio via the bytesperline
		 * V4L2 default (width * 5 / 4 for NV15 vs width * 1 for
		 * NV12); aligned_bpl * height * 3 / 2 then gives the
		 * correct 3,888,000-byte image-only size for 1920x1080
		 * NV15.
		 */
		.fourcc = V4L2_PIX_FMT_NV15,
		.image_fmt = RKVDEC_IMG_FMT_420_10BIT,
	},
};

/* Bit-depth selector — runs on every V4L2_CID_STATELESS_VP9_FRAME set.
 * The m2m framework compares this against ctx->image_fmt and triggers a
 * source_change event when it changes (handled by the userspace V4L2
 * client). Other control IDs return RKVDEC_IMG_FMT_ANY so the framework
 * keeps the current format.
 */
static enum rkvdec_image_fmt
rkvdec_vdpu383_vp9_get_image_fmt(struct rkvdec_ctx *ctx,
				 struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_vp9_frame *fp;

	if (ctrl->id != V4L2_CID_STATELESS_VP9_FRAME)
		return RKVDEC_IMG_FMT_ANY;

	fp = ctrl->p_new.p_vp9_frame;
	if (fp->bit_depth == 10)
		return RKVDEC_IMG_FMT_420_10BIT;
	return RKVDEC_IMG_FMT_420_8BIT;
}

/*
 * Mid-stream resolution-change detection. Returns true when the
 * just-set VP9 frame control carries dimensions that don't fit the
 * current decoded_fmt. Triggers V4L2_EVENT_SOURCE_CHANGE in the
 * core; userspace stops CAPTURE, re-G_FMTs, and restarts.
 *
 * VP9 carries frame dimensions in every frame header (not just the
 * sequence start), so the encoded stream can resize at any frame.
 * Conformance corpus files like crowd_run_*_frm_resize_l1 exercise
 * this — without the event, the decoder would write at the new size
 * into CAPTURE buffers sized for the old dimensions and produce
 * truncated or overrun output.
 */
static bool rkvdec_vdpu383_vp9_check_source_change(struct rkvdec_ctx *ctx,
						   struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_vp9_frame *fp;
	u32 w, h;

	if (ctrl->id != V4L2_CID_STATELESS_VP9_FRAME)
		return false;

	fp = ctrl->p_new.p_vp9_frame;
	w = fp->frame_width_minus_1 + 1;
	h = fp->frame_height_minus_1 + 1;

	return w != ctx->decoded_fmt.fmt.pix_mp.width ||
	       h != ctx->decoded_fmt.fmt.pix_mp.height;
}

const struct rkvdec_coded_fmt_ops rkvdec_vdpu383_vp9_fmt_ops = {
	.adjust_fmt		= rkvdec_vdpu383_vp9_adjust_fmt,
	.start			= rkvdec_vdpu383_vp9_start,
	.stop			= rkvdec_vdpu383_vp9_stop,
	.run			= rkvdec_vdpu383_vp9_run,
	.done			= rkvdec_vdpu383_vp9_done,
	.get_image_fmt		= rkvdec_vdpu383_vp9_get_image_fmt,
	.check_source_change	= rkvdec_vdpu383_vp9_check_source_change,
};
