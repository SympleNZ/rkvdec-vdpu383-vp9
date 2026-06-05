/* SPDX-License-Identifier: GPL-2.0 */
/*
 * VDPU383 VP9 register layout — VP9-specific addr block.
 *
 * VP9 reuses three of the four mainline VDPU383 register blocks unchanged
 * (common, h26x_params, common_addr from rkvdec-vdpu383-regs.h). The
 * fourth — codec address registers — has a different meaning in VP9 from
 * HEVC/H.264 even though the byte layout is identical:
 *
 *   reg170-172 are the only ref-frame DMAs (last/golden/altref), not 16
 *               contiguous DPB entries.
 *   reg181-185 are VP9-specific: segment-ID-map last/current, keyframe
 *               prob base, last prob base, update prob base. In HEVC's
 *               struct these positions are aliased to ref slots [11..15]
 *               of reg170_185_ref_base.
 *   reg217 is the only colmv-ref slot used (previous frame), not 16
 *               contiguous slots.
 *
 * This header gives those positions VP9-correct names while preserving
 * the exact byte layout of struct vdpu383_regs_h26x_addr, so the four
 * rkvdec_memcpy_toio() writes still hit the same offsets.
 */

#ifndef _RKVDEC_VDPU383_VP9_REGS_H_
#define _RKVDEC_VDPU383_VP9_REGS_H_

#include <linux/build_bug.h>
#include <linux/types.h>

#include "rkvdec-vdpu383-regs.h"

struct vdpu383_regs_vp9_addr {
	u32 reg168_decout_base;			/* 0x2A0 */
	u32 reg169_error_ref_base;		/* 0x2A4 */
	u32 reg170_last_ref_base;		/* 0x2A8 */
	u32 reg171_golden_ref_base;		/* 0x2AC */
	u32 reg172_altref_ref_base;		/* 0x2B0 */
	u32 reg173_180_unused_ref[8];		/* 0x2B4-0x2D0  fill with dst */
	u32 reg181_segid_last_base;		/* 0x2D4 */
	u32 reg182_segid_cur_base;		/* 0x2D8 */
	u32 reg183_kf_prob_base;		/* 0x2DC */
	u32 reg184_lastprob_base;		/* 0x2E0 */
	u32 reg185_updateprob_base;		/* 0x2E4 */
	u32 reg186_191_reserved[6];		/* 0x2E8-0x2FC */
	u32 reg192_payload_cur_base;		/* 0x300 */
	u32 reg193_fbc_payload_offset;		/* 0x304 */
	u32 reg194_payload_error_ref_base;	/* 0x308 */
	u32 reg195_payload_ref0_base;		/* 0x30C  last */
	u32 reg196_payload_ref1_base;		/* 0x310  golden */
	u32 reg197_payload_ref2_base;		/* 0x314  altref */
	u32 reg198_210_unused_payload[13];	/* 0x318-0x348  fill with dst */
	u32 reg211_215_reserved[5];		/* 0x34C-0x35C */
	u32 reg216_colmv_cur_base;		/* 0x360 */
	u32 reg217_colmv_ref_base;		/* 0x364 */
	u32 reg218_232_unused_colmv[15];	/* 0x368-0x3A0  fill with dst+colmv */
};

struct vdpu383_regs_vp9 {
	struct vdpu383_regs_common	common;		/* reg8-30   @ 0x020 */
	struct vdpu383_regs_h26x_params	h26x_params;	/* reg64-106 @ 0x100 */
	struct vdpu383_regs_common_addr	common_addr;	/* reg128-162 @ 0x200 */
	struct vdpu383_regs_vp9_addr	vp9_addr;	/* reg168-232 @ 0x2A0 */
} __packed;

/*
 * The VP9 codec-address block must occupy the same 65 u32 region as the
 * HEVC/H.264 h26x_addr block; the four rkvdec_memcpy_toio() writes assume
 * this exactly.
 */
static_assert(sizeof(struct vdpu383_regs_vp9_addr) == 65 * sizeof(u32));
static_assert(sizeof(struct vdpu383_regs_vp9_addr) ==
	      sizeof(struct vdpu383_regs_h26x_addr));

#endif /* _RKVDEC_VDPU383_VP9_REGS_H_ */
