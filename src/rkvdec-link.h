/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Video Decoder LINK mode — codec-agnostic descriptor table.
 *
 * Phase 1 (this header): types + alloc + descriptor fill. NO MMIO yet.
 * Phase 2 adds enqueue + IRQ handler.
 *
 * Models the BSP rk_vcodec link-mode descriptor mechanism documented in
 * docs/rk3576/vp9/LINK_MODE_DESIGN_2026-05-25.md.
 *
 * The BSP runs all RK3576/VDPU383 codecs (HEVC, H.264, VP9, AV1) through
 * the same `rkvdec_link_info` shape; only the part_w[]/part_r[] offsets
 * and the ip_en_val differ. We design the same way so AV1 inherits this
 * infrastructure once VP9 validates Phase 2 + Phase 3.
 */
#ifndef _RKVDEC_LINK_H_
#define _RKVDEC_LINK_H_

#include <linux/types.h>
#include <linux/dma-mapping.h>

struct device;

#define RKVDEC_LINK_MAX_WRITE_PART	6
#define RKVDEC_LINK_MAX_READ_PART	2

/*
 * One contiguous register range in the BSP descriptor.
 *
 *   tb_reg_off  — descriptor offset in u32s (NOT bytes)
 *   reg_start   — first MMIO register index (also u32-indexed)
 *   reg_num     — count of registers in this part
 *
 * "part_w" parts: descriptor → HW reads (sender fills, HW consumes).
 * "part_r" parts: HW → descriptor writes (HW writes during decode).
 */
struct rkvdec_link_part {
	u32 tb_reg_off;
	u32 reg_start;
	u32 reg_num;
};

/*
 * Per-codec link info. Mirrors BSP `struct rkvdec_link_info` but trimmed
 * to the fields we use. Codec drivers (vdpu383 vp9 / av1 etc.) provide
 * a const instance.
 *
 * Fields:
 *   tb_reg_num       Total descriptor size in u32 (always 256 for VDPU383).
 *   tb_reg_next      Descriptor slot holding the IOVA of the next entry.
 *   tb_reg_r         Slot HW uses for read-back (usually 1).
 *   tb_reg_debug     Descriptor slot for the read-back-area self-pointer.
 *                    Set to table.iova + part_r[0].tb_reg_off*4. -1 to skip.
 *   tb_reg_seg0/1/2  Descriptor slots for the part_w[0/1/2] self-pointers.
 *                    Set to table.iova + part_w[N].tb_reg_off*4. -1 to skip.
 *   tb_reg_int       Slot HW writes per-task IRQ status into (usually 16).
 *   part_w_num       Number of write parts (typically 3 for VDPU383).
 *   part_r_num       Number of read parts (typically 2 for VDPU383).
 *   part_w/part_r    Range tables.
 *
 *   next_addr_base   MMIO offset for descriptor list next-pointer (0x20).
 *   ip_reset_base    MMIO offset for ip soft-reset (0x44).
 *   ip_reset_en      BIT for soft-reset (BIT(0)).
 *   irq_base         MMIO offset for link IRQ status (0x48).
 *   irq_mask         Mask within `irq_base` upper 16 bits (0x30000).
 *   status_base      MMIO offset for link status (0x4c).
 *   status_mask      Mask within `status_base` upper 16 bits.
 *   err_mask         Mask within status[16:25] indicating per-task error.
 *   ip_time_base     MMIO offset for IP timeout reg (0x54).
 *   en_base          MMIO offset for single-shot DEC_ENABLE (0x40, unused in link mode).
 *   ip_en_base       MMIO offset for per-task IP enable (0x58).
 *   ip_en_val        Value written to ip_en_base each task (0x01000000).
 *   en_sw_iommu_zap  Whether the driver must flush IOMMU TLB per task.
 */
struct rkvdec_link_info {
	u32 tb_reg_num;
	u32 tb_reg_next;
	u32 tb_reg_r;
	u32 tb_reg_int;
	/* -1 = slot unused; non-negative = descriptor index that holds an
	 * IOVA pointing into the descriptor at the relevant sub-region.
	 * VDPU383: debug=2, seg0=3, seg1=4, seg2=5. */
	int tb_reg_debug;
	int tb_reg_seg0;
	int tb_reg_seg1;
	int tb_reg_seg2;

	u32 part_w_num;
	u32 part_r_num;
	struct rkvdec_link_part part_w[RKVDEC_LINK_MAX_WRITE_PART];
	struct rkvdec_link_part part_r[RKVDEC_LINK_MAX_READ_PART];

	u32 next_addr_base;
	u32 ip_reset_base;
	u32 ip_reset_en;
	u32 irq_base;
	u32 irq_mask;
	u32 status_base;
	u32 status_mask;
	u32 err_mask;
	u32 ip_time_base;
	u32 en_base;
	u32 ip_en_base;
	u32 ip_en_val;

	bool en_sw_iommu_zap;
};

/*
 * Per-session link state. Lives on the rkvdec_ctx (one per V4L2 file
 * handle); HW state lives on rkvdec_dev. Phase 1 keeps this minimal —
 * just the descriptor ring. Phase 2 adds the running/pending counters.
 */
struct rkvdec_link_table {
	struct device *dev;
	const struct rkvdec_link_info *info;

	/* Coherent DMA buffer holding the descriptor ring. */
	void *kva;
	dma_addr_t iova;
	size_t total_size;	/* bytes */
	u32 task_capacity;	/* number of descriptors in ring */
	u32 node_size;		/* bytes per descriptor (aligned) */

	/* Ring index for next slot to fill. Phase 1 doesn't really
	 * advance this — it just lets us round-robin descriptors per
	 * frame so dumps show different content over time.
	 */
	u32 next_slot;

	/* HW completion tracking. HW increments LINK_DEC_NUM (0x10)
	 * after each task completes. The IRQ-check compares the latest
	 * HW counter against this value; if it advanced, that's the
	 * link-mode "task done" signal (0x4c status bits stay 0 in
	 * normal completion — BSP relies on the counter for ack). */
	u32 expected_dec_num;
};

/* ----------------------------------------------------------------- */

/*
 * Allocate the descriptor ring.
 *
 *   info            Codec link info (VDPU383 VP9 / AV1 etc.).
 *   task_capacity   Number of descriptors in the ring (≥ 1).
 *                   Phase 1 uses 1; Phase 3 may grow to 2–4.
 *
 * Returns 0 on success. On success, table->kva / iova / etc. are filled.
 * Caller frees with rkvdec_link_free_table().
 *
 * Allocation uses dma_alloc_coherent. The whole table is zero-init so
 * descriptor slots that don't get written (the read-back slots, the
 * gaps in part_w coverage) stay clean.
 *
 * Node size is ALIGN(tb_reg_num * sizeof(u32), 256) — matches BSP
 * "link table address requires 64 align" but rounded to a cache-line-
 * friendly 256.
 */
int rkvdec_link_alloc_table(struct rkvdec_link_table *table,
			    struct device *dev,
			    const struct rkvdec_link_info *info,
			    u32 task_capacity);

void rkvdec_link_free_table(struct rkvdec_link_table *table);

/*
 * Fill descriptor `slot` from a flat 256-u32 register-image array.
 *
 *   slot     0 ≤ slot < table->task_capacity
 *   regs     Pointer to a u32[info->tb_reg_num] register image. Codec
 *            builds this from its existing per-frame regs struct
 *            (e.g. vdpu383_regs_vp9). Indices outside the part_w[]
 *            ranges are ignored. Caller is responsible for placing
 *            reg values at their MMIO-relative indices in `regs`.
 *
 * After fill, the descriptor's next_addr slot is set so the ring
 * loops back to slot 0 (Phase 1: trivial cycle). Phase 3 will rewrite
 * next_addr dynamically as tasks queue.
 *
 * Read-back parts (part_r) are NOT touched — HW writes those. The
 * tb_reg_int slot is cleared to 0xffffffff (sentinel: "HW hasn't
 * written yet"; HW will overwrite with status on completion).
 */
void rkvdec_link_fill_descriptor(struct rkvdec_link_table *table, u32 slot,
				 const u32 *regs);

/*
 * Read back the per-task IRQ status that HW wrote into the descriptor's
 * tb_reg_int slot. Used by the link IRQ handler (Phase 2) to learn the
 * outcome of a completed task.
 *
 * Returns the status word (low 32 bits matching the BSP per-task IRQ
 * status from descriptor offset 16) or 0xffffffff if HW hasn't written
 * yet (still the sentinel from fill).
 */
u32 rkvdec_link_read_task_status(struct rkvdec_link_table *table, u32 slot);

/*
 * Pretty-print one descriptor's contents to dmesg. Diagnostic-only.
 * Format mirrors BSP's `rkvdec_link_node_dump` style so dumps can be
 * directly diffed.
 *
 * Output (illustrative):
 *   rkvdec-link: slot 0 iova 0xff100000 (1024 bytes):
 *   rkvdec-link:   header  [0..15]:    .next=0xff100100 ...
 *   rkvdec-link:   part_w[0] reg008-031: 00000002 00008101 ...
 *   rkvdec-link:   part_w[1] reg064-107: ...
 *   rkvdec-link:   part_w[2] reg128-235: ...
 */
void rkvdec_link_dump_descriptor(struct rkvdec_link_table *table, u32 slot,
				 const char *prefix);

/*
 * Pack a codec's per-frame register sub-structs into a flat u32[256]
 * image laid out at MMIO indices, ready to hand to
 * rkvdec_link_fill_descriptor().
 *
 * This is a thin codec-agnostic helper:
 *   image[base + i] = src[i]   for i in 0..(src_words-1)
 *
 * Callers do one call per sub-struct (common at index 8, h26x_params
 * at 64, common_addr at 128, codec_addr at 168). The image is
 * zero-init by the caller so gaps stay zero.
 */
static inline void rkvdec_link_image_pack(u32 *image, u32 base,
					  const void *src, size_t src_bytes)
{
	memcpy(&image[base], src, src_bytes);
}

extern const struct rkvdec_link_info rkvdec_link_vdpu383_info;

/* ---- Phase 2: MMIO touch (enqueue + IRQ + cache) ----------------- *
 *
 * These functions operate on the codec's link MMIO bank (the same
 * `rkvdec->link` pointer used by single-shot kicks at offsets
 * 0x40/0x58). They are codec-agnostic — the codec's per-frame regs
 * have already been packed into the descriptor by
 * rkvdec_link_fill_descriptor().
 */

/*
 * BSP cache-clear sequence at link offsets 0x510/0x518/0x51c/0x550/
 * 0x55c/0x590/0x59c. Called ONLY when starting a fresh queue (when
 * LINK_EN reads 0). Must NOT be combined with mainline 0x41x cache
 * writes — session-s phase-17 confirmed doing both regresses Fluster
 * by 8.5 (98→89.5).
 *
 * cache_line_size_64: true to use the BSP "64-byte cache line" config
 *                     (matches mpp_debug_unlikely(DEBUG_CACHE_32B)==false).
 */
void rkvdec_link_clear_caches_bsp(void __iomem *link_base,
				  bool cache_line_size_64);

/*
 * RK3576 HW WARMUP — port of BSP's rk3576_workaround_init + run.
 *
 * Allocate a DMA-coherent buffer (8 KiB) and populate it with the
 * pre-canned test descriptor. The returned (cpu, dma) pair is owned
 * by the caller and must be passed to rkvdec_rk3576_warmup_free at
 * tear-down. Does NOT trigger HW; call rkvdec_rk3576_warmup_run
 * separately AFTER IRQ is registered.
 *
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
int rkvdec_rk3576_warmup_alloc(struct device *dev,
			       void **out_cpu, dma_addr_t *out_dma);
void rkvdec_rk3576_warmup_free(struct device *dev,
			       void *cpu, dma_addr_t dma);

/*
 * Execute the warmup descriptor through the link-mode pipeline. Writes
 * the rk3576_workaround_run register sequence to link_base, polls for
 * completion, then clears all link registers to a known-good state.
 *
 * Caller responsibility: clocks + power domain must be ON.
 *
 * Returns 0 on clean completion (HW reported status without errors),
 * -EIO if status reported an error in err_mask, -ETIMEDOUT if HW
 * didn't finish within ~21 ms.
 */
int rkvdec_rk3576_warmup_run(void __iomem *link_base, dma_addr_t buf_iova);

/*
 * 2026-06-08 Variant B — IRQ-driven warmup. Arms the link IRQ and sleeps on
 * rkvdec->warmup_irq_done (reaped by rkvdec_irq_top under warmup_irq_inflight)
 * instead of busy-polling. The completion timeout is the can't-hang fallback.
 * Returns 0 / -ETIMEDOUT (IRQ didn't fire; HW still ran) / -EIO.
 */
struct rkvdec_dev;
int rkvdec_rk3576_warmup_run_irq(struct rkvdec_dev *rkvdec);

/*
 * Enqueue ONE filled descriptor into the link queue. Implements BSP's
 * rkvdec2_link_enqueue (mpp_rkvdec2_link.c:476) including the first-
 * task-of-session bootstrap (cache clear + LINK_EN bring-up) and
 * subsequent-task append (LINK_BIT_ADD_MODE).
 *
 *   table      Descriptor ring (must have slot already filled).
 *   slot       Index of the filled descriptor to kick. Phase 2 single-
 *              task mode always uses slot 0; Phase 3 ring-buffers.
 *   link_base  MMIO bank base (rkvdec->link).
 *
 * Returns 0 on success. Does NOT poll for completion — IRQ handler
 * runs after HW finishes.
 *
 * IOMMU TLB flush is the caller's responsibility (BSP en_sw_iommu_zap
 * does it inline, but mainline our IOMMU flush API needs an
 * iommu_domain* the caller already has via iommu_get_domain_for_dev).
 * Call flush AFTER this function returns the BSP-pre-CFG_DONE wmb
 * but BEFORE the final CFG_DONE write — see implementation comment
 * for the exact sequence. For Phase 2 single-task we just call flush
 * before this function and accept the slight ordering difference.
 */
/*
 * Phase 3 v0.3 step 2.6: `frame_num` is the number of descriptors
 * (starting at `slot`) HW should chain through in this kick. Pass
 * 1 for single-shot link mode (depth=1 behaviour); pass N for
 * batched mode with N pre-filled descriptors at slot..slot+N-1.
 */
int rkvdec_link_enqueue_vdpu383(struct rkvdec_link_table *table, u32 slot,
				u32 frame_num, void __iomem *link_base);

/*
 * VDPU383 link-mode IRQ check. Reads the link IRQ status from offset
 * 0x48 and the per-task status from 0x4c, mirroring BSP's
 * rkvdec_vdpu383_link_irq (mpp_rkvdec2_link.c:702).
 *
 * Returns true if a link IRQ fired (caller should then process the
 * completed task via rkvdec_link_read_task_status). irq_val_out and
 * status_out receive the raw register values for logging.
 *
 * Side effect: clears the IRQ + status registers on the way out
 * (writes 0xffff0000 to both, per BSP).
 */
bool rkvdec_link_vdpu383_irq_check(void __iomem *link_base,
				   u32 *irq_val_out, u32 *status_out);

/*
 * Link-mode completion check (the real BSP path). Reads LINK_DEC_NUM
 * at 0x10. If the counter advanced past `table->expected_dec_num`,
 * one or more tasks have completed since last check; updates the
 * table's counter and returns the number completed (typically 1 in
 * Phase 2 single-task mode).
 *
 * Caller then disables INT_EN_IRQ (via FIELD_PREP_WM16) to ack the
 * IRQ — without that the kernel sees a sticky IRQ and disables it
 * as spurious after a handful of retries.
 */
u32 rkvdec_link_check_completion(struct rkvdec_link_table *table,
				 void __iomem *link_base);

#endif /* _RKVDEC_LINK_H_ */
