// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip video decoder Rows and Cols Buffers manager
 *
 * Copyright (C) 2025 Collabora, Ltd.
 *  Detlev Casanova <detlev.casanova@collabora.com>
 */

#include "rkvdec.h"
#include "rkvdec-rcb.h"

#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/genalloc.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/types.h>

/*
 * 2026-06-09 N1 follow-up — per-region SRAM RCB packing. Default 0 keeps the
 * validated all-or-nothing path untouched. When 1, the allocator splits the RCB
 * into an SRAM block (high-priority row contexts, in MPP RCB_SET_BY_PRIORITY
 * order — FLTD_IN, INTER_IN, INTRA_IN, STRMD_IN, ...) up to the SRAM pool's
 * capacity, and a DRAM block for the spill. Codec backends must read per-region
 * `rkvdec_rcb_buf_dma_addr(ctx, slot)` in this mode (regions are not contiguous).
 * Targets the AV1 partial bug (intra above-row RCB, slot 4, never run in SRAM
 * for real content) and VP9. See PER_REGION_SRAM_RCB_PLAN_2026-06-09.md.
 */
int rcb_sram_pack;
module_param(rcb_sram_pack, int, 0644);
MODULE_PARM_DESC(rcb_sram_pack,
		 "Pack high-priority RCB regions into SRAM, spill rest to DRAM (0=off/all-or-nothing, 1=on)");

/*
 * 2026-06-10 E1 — cached RCB backing + per-frame dma_sync (VDPU383_LIFECYCLE_MAP
 * §3.2/3.3). Default 0 keeps the validated uncached dma_alloc_coherent path.
 * When 1, the whole RCB is ONE cached dma_alloc_noncoherent DRAM block and the
 * codec run paths call rkvdec_rcb_sync_for_device() before each kick — mirroring
 * the BSP's dma-buf-cache + ~29 dma_sync_single_for_device/frame measured on .104
 * (Run 2 ground truth). Placement (SRAM vs DRAM) is closed (N1); the lever under
 * test here is the buffer CACHEABILITY, not its location. Skips the SRAM path.
 */
int rcb_cached;
module_param(rcb_cached, int, 0644);
MODULE_PARM_DESC(rcb_cached,
		 "Allocate RCB as cached DRAM + dma_sync before each kick (0=off uncached-coherent, 1=on)");

/* VDPU383 RCB priority (MPP RCB_SET_BY_PRIORITY): which slots get SRAM first.
 * Slot map (shared vdpu383_rcb_sizes): 0 STRMD_IN 1 STRMD_ON 2 INTER_IN
 * 3 INTER_ON 4 INTRA_IN 5 INTRA_ON 6 FLTD_IN 7 FLTD_PROT_IN 8 FLTD_ON_ROW
 * 9 FLTD_ON_COL 10 FLTD_UPSC. */
static const u8 rkvdec_rcb_prio[11] = { 6, 2, 4, 0, 3, 5, 1, 8, 9, 10, 7 };

/* struct rkvdec_rcb_config moved to rkvdec-rcb.h so codec backends can
 * read the actual RCB allocation dimensions (e.g. for the Round 4.2
 * dim-mismatch reject in rkvdec_vdpu383_vp9_run). */

/*
 * 2026-06-16 (Detlev lead): our vdpu383_rcb_sizes[] is derived from MPP's
 * *VP9* rcb_calc; MPP keeps a separate vdpu383_av1d_rcb_calc with larger
 * per-region sizes (e.g. inter ×2752 vs ×2368, plus nonzero strmd_in and
 * fltd_upsc_on_col, plus AV1 loop-restoration/upscale terms VP9 zeroes).
 * Detlev: "too-small RCB can lead to random results" — the exact AV1
 * non-determinism symptom. Fable only ever *shrank* slot-4 / moved RCB
 * placement; enlarging was never tested. This param uniformly scales every
 * RCB region so we can test "is the AV1 coin-flip RCB-undersize?" without
 * first porting the full AV1 calc: set 2 or 3, re-run the AV1 battery, and
 * see if the wrong/silent rate drops. Default 1 = exact current behaviour
 * (no-op for all codecs). UNTESTED on-board as of staging.
 */
int rcb_size_scale = 1;
module_param(rcb_size_scale, int, 0644);
MODULE_PARM_DESC(rcb_size_scale,
		 "Uniform multiplier on every RCB region size (1=current, 2/3=enlarge for AV1 undersize test)");

static size_t rkvdec_rcb_size(const struct rcb_size_info *size_info,
			      unsigned int width, unsigned int height)
{
	size_t dim_term;
	int scale = rcb_size_scale > 0 ? rcb_size_scale : 1;

	dim_term = (size_t)size_info->multiplier *
		   (size_info->axis == PIC_HEIGHT ? height : width);
	return (size_t)scale * max_t(size_t, dim_term, size_info->min_bytes);
}

dma_addr_t rkvdec_rcb_buf_dma_addr(struct rkvdec_ctx *ctx, int id)
{
	return ctx->rcb_config->rcb_bufs[id].dma;
}

/*
 * 2026-06-10 E1 — flush the cached RCB to device before a kick. No-op unless
 * rcb_cached allocated the buffer cached. Mirrors the BSP per-frame
 * dma_sync_single_for_device over its dma-buf-cache context buffers (Run 2).
 */
void rkvdec_rcb_sync_for_device(struct rkvdec_ctx *ctx)
{
	struct rkvdec_rcb_config *cfg = ctx->rcb_config;

	if (cfg && cfg->cached && cfg->cached_cpu)
		dma_sync_single_for_device(ctx->dev->dev, cfg->cached_dma,
					   cfg->cached_size, DMA_BIDIRECTIONAL);
}

size_t rkvdec_rcb_buf_size(struct rkvdec_ctx *ctx, int id)
{
	return ctx->rcb_config->rcb_bufs[id].size;
}

int rkvdec_rcb_buf_count(struct rkvdec_ctx *ctx)
{
	return ctx->rcb_config->rcb_count;
}

bool rkvdec_rcb_buf_validate_size(struct rkvdec_ctx *ctx)
{
	struct rkvdec_rcb_config *cfg = ctx->rcb_config;

	bool ret = cfg && cfg->height >= ctx->decoded_fmt.fmt.pix_mp.height &&
		   cfg->width >= ctx->decoded_fmt.fmt.pix_mp.width;

	if (!ret && cfg) {
		dev_dbg(ctx->dev->dev, "RCB size %ux%u -> %ux%u\n", cfg->width, cfg->height,
			ctx->decoded_fmt.fmt.pix_mp.width, ctx->decoded_fmt.fmt.pix_mp.height);
	}

	return ret;
}

void rkvdec_free_rcb(struct rkvdec_ctx *ctx)
{
	struct rkvdec_dev *dev = ctx->dev;
	struct rkvdec_rcb_config *cfg = ctx->rcb_config;
	unsigned long virt_addr;
	int i;

	if (!cfg)
		return;

	dev->accum_rcb_free++;

	/*
	 * 2026-06-10 E1 cached mode (rcb_cached): one cached dma_alloc_noncoherent
	 * DRAM block; free via the matching dma_free_noncoherent.
	 */
	if (cfg->cached) {
		if (cfg->cached_cpu)
			dma_free_noncoherent(dev->dev, cfg->cached_size,
					     cfg->cached_cpu, cfg->cached_dma,
					     DMA_BIDIRECTIONAL);
		goto free_meta;
	}

	/*
	 * 2026-06-09 packed mode (rcb_sram_pack): two explicit underlying blocks
	 * (an iommu-mapped SRAM block + a DRAM block); free both from the config
	 * handles, NOT via slot 0 (slots alias into either block).
	 */
	if (cfg->packed) {
		if (cfg->sram_cpu) {
			if (dev->iommu_domain)
				iommu_unmap(dev->iommu_domain,
					    (unsigned long)cfg->sram_cpu,
					    cfg->sram_size);
			gen_pool_free(dev->sram_pool,
				      (unsigned long)cfg->sram_cpu, cfg->sram_size);
		}
		if (cfg->dram_cpu)
			dma_free_coherent(dev->dev, cfg->dram_size,
					  cfg->dram_cpu, cfg->dram_dma);
		goto free_meta;
	}

	/*
	 * 2026-05-31 Round 9: single contiguous allocation. Only slot 0
	 * holds the underlying handle; subsequent slots are aliased
	 * offsets within. Free the whole block via slot 0 and skip the
	 * rest.
	 */
	if (cfg->rcb_count > 0 && cfg->rcb_bufs[0].cpu) {
		size_t total_size = 0;
		void *base_cpu = cfg->rcb_bufs[0].cpu;
		dma_addr_t base_dma = cfg->rcb_bufs[0].dma;

		for (i = 0; i < cfg->rcb_count; i++)
			total_size += cfg->rcb_bufs[i].size;

		switch (cfg->rcb_bufs[0].type) {
		case RKVDEC_ALLOC_SRAM:
			virt_addr = (unsigned long)base_cpu;
			if (dev->iommu_domain)
				iommu_unmap(dev->iommu_domain, virt_addr,
					    total_size);
			gen_pool_free(dev->sram_pool, virt_addr, total_size);
			break;
		case RKVDEC_ALLOC_DMA:
			dma_free_coherent(dev->dev, total_size,
					  base_cpu, base_dma);
			break;
		}
	}

free_meta:
	if (cfg->rcb_bufs)
		devm_kfree(dev->dev, cfg->rcb_bufs);

	devm_kfree(dev->dev, cfg);

	ctx->rcb_config = NULL;
}

int rkvdec_allocate_rcb(struct rkvdec_ctx *ctx, u32 width, u32 height,
			const struct rcb_size_info *size_info,
			size_t rcb_count)
{
	int ret, i;
	struct rkvdec_dev *rkvdec = ctx->dev;
	struct rkvdec_rcb_config *cfg;

	if (!size_info || !rcb_count) {
		ctx->rcb_config = NULL;
		return 0;
	}

	rkvdec->accum_rcb_alloc++;

	ctx->rcb_config = devm_kzalloc(rkvdec->dev, sizeof(*ctx->rcb_config), GFP_KERNEL);
	if (!ctx->rcb_config)
		return -ENOMEM;

	cfg = ctx->rcb_config;

	cfg->rcb_bufs = devm_kzalloc(rkvdec->dev, sizeof(*cfg->rcb_bufs) * rcb_count, GFP_KERNEL);
	if (!cfg->rcb_bufs) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	cfg->width = width;
	cfg->height = height;

	/*
	 * 2026-05-31 Round 9: ONE contiguous SRAM allocation for all RCB
	 * buffers, ONE iommu_map covering the whole region. Per-buffer
	 * slots become offsets within the single allocation.
	 *
	 * Background: MPP+BSP A/B (Round 8) showed vendor decoder produces
	 * the SAME fill output as ours on 14-resize content, BUT with ZERO
	 * IOMMU page faults vs our 138+ per single vector. The difference:
	 * MPP allocates ONE big SRAM region and lays out RCB buffers
	 * within it; HW's wrong-IOVA writes during the fp-tiles resize
	 * failure land WITHIN the mapped region (corrupting the next
	 * buffer's content — output is fill — but never hitting unmapped
	 * memory).
	 *
	 * Our previous per-buffer allocation pattern had gaps between
	 * mappings whenever gen_pool fragmented. The vp9.c HW-config
	 * code in rkvdec_vdpu383_vp9_run writes "base + cum_off" for
	 * each slot's address into reg140_162_rcb_info, assuming
	 * contiguity. When the actual gen_pool allocations weren't
	 * contiguous, HW's slot N reads landed in gap memory or another
	 * slot's region — broken for ALL content actually, but only
	 * 14-resize's wrong-IOVA writes triggered visible page faults.
	 *
	 * The single-region layout matches BSP/MPP semantics and
	 * inherently fixes both the gap-fault issue and the post-Round-4.3
	 * accumulation bug.
	 */
	{
		size_t total_size = 0;
		void *big_cpu = NULL;
		dma_addr_t big_dma = 0;
		unsigned long big_virt = 0;
		size_t cum = 0;
		enum rkvdec_alloc_type alloc_type;
		bool sram_ok = false;

		/* First pass: compute total + per-buffer sizes */
		for (i = 0; i < rcb_count; i++) {
			size_t sz = rkvdec_rcb_size(&size_info[i], width, height);
			/* keep Round 4.3 4KB per-buffer pad as headroom */
			sz += SZ_4K;
			if (rkvdec->iommu_domain)
				sz = ALIGN(sz, SZ_4K);
			cfg->rcb_bufs[i].size = sz;
			total_size += sz;
		}

		/*
		 * 2026-06-10 E1 — cached whole-RCB path (gated rcb_cached). ONE
		 * CACHED DRAM block via dma_alloc_noncoherent; the codec backends
		 * dma_sync it to device before each kick. Skips SRAM (placement
		 * closed, N1); the lever under test is buffer cacheability, per the
		 * Run-2 BSP ground truth (dma-buf-cache + ~29 dma_sync/frame).
		 */
		if (rcb_cached) {
			void *cpu;
			dma_addr_t dma;

			cpu = dma_alloc_noncoherent(rkvdec->dev, total_size, &dma,
						    DMA_BIDIRECTIONAL, GFP_KERNEL);
			if (!cpu) {
				ret = -ENOMEM;
				goto err_alloc;
			}
			memset(cpu, 0, total_size);
			dma_sync_single_for_device(rkvdec->dev, dma, total_size,
						   DMA_BIDIRECTIONAL);

			cfg->cached = true;
			cfg->cached_cpu = cpu;
			cfg->cached_dma = dma;
			cfg->cached_size = total_size;

			for (i = 0; i < rcb_count; i++) {
				cfg->rcb_bufs[i].cpu = (char *)cpu + cum;
				cfg->rcb_bufs[i].dma = dma + cum;
				cfg->rcb_bufs[i].type = RKVDEC_ALLOC_DMA;
				cfg->rcb_count++;
				cum += cfg->rcb_bufs[i].size;
			}

			pr_info("rkvdec-rcb CACHED: %ux%u total=%zu dma=0x%llx slots=%zu\n",
				width, height, total_size,
				(unsigned long long)dma, rcb_count);
			return 0;
		}

		/*
		 * 2026-06-09 N1 follow-up — per-region SRAM packing (gated).
		 * Split the RCB into an SRAM block (high-priority row contexts, in
		 * rkvdec_rcb_prio order) up to the pool's capacity + a DRAM block for
		 * the spill, so the critical FLTD_IN / INTER_IN / INTRA_IN regions land
		 * in fast SRAM even when the total exceeds 512 KB. Per-region addresses;
		 * codec backends read rkvdec_rcb_buf_dma_addr(ctx, slot). Default path
		 * (below) is untouched when rcb_sram_pack == 0.
		 */
		if (rcb_sram_pack && rkvdec->iommu_domain) {
			size_t sram_avail = 0, sram_used = 0, dram_used = 0;
			void *sram_cpu = NULL;
			dma_addr_t sram_dma = 0;
			unsigned long sram_virt = 0;
			void *dram_cpu;
			dma_addr_t dram_dma;
			int pi;

			dram_cpu = dma_alloc_coherent(ctx->dev->dev, total_size,
						      &dram_dma, GFP_KERNEL);
			if (!dram_cpu) {
				ret = -ENOMEM;
				goto err_alloc;
			}
			cfg->packed = true;
			cfg->dram_cpu = dram_cpu;
			cfg->dram_dma = dram_dma;
			cfg->dram_size = total_size;

			if (ctx->dev->sram_pool) {
				sram_avail = gen_pool_avail(ctx->dev->sram_pool);
				if (sram_avail > total_size)
					sram_avail = total_size;
				sram_avail = ALIGN_DOWN(sram_avail, SZ_4K);
				if (sram_avail) {
					sram_cpu = gen_pool_dma_zalloc_align(
						ctx->dev->sram_pool, sram_avail,
						&sram_dma, SZ_4K);
					if (sram_cpu) {
						sram_virt = (unsigned long)sram_cpu;
						ret = iommu_map(rkvdec->iommu_domain,
								sram_virt,
								(phys_addr_t)sram_dma,
								sram_avail,
								IOMMU_READ | IOMMU_WRITE,
								0);
						if (ret) {
							gen_pool_free(ctx->dev->sram_pool,
								      sram_virt, sram_avail);
							sram_cpu = NULL;
						} else {
							cfg->sram_cpu = sram_cpu;
							cfg->sram_size = sram_avail;
						}
					}
				}
			}

			/* Place regions: priority order into SRAM until full, rest DRAM. */
			for (pi = 0; pi < (int)ARRAY_SIZE(rkvdec_rcb_prio); pi++) {
				int slot = rkvdec_rcb_prio[pi];
				size_t sz;

				if (slot >= (int)rcb_count)
					continue;
				sz = cfg->rcb_bufs[slot].size;
				if (sram_cpu && sram_used + sz <= sram_avail) {
					cfg->rcb_bufs[slot].cpu =
						(char *)sram_cpu + sram_used;
					cfg->rcb_bufs[slot].dma = sram_virt + sram_used;
					cfg->rcb_bufs[slot].type = RKVDEC_ALLOC_SRAM;
					sram_used += sz;
				} else {
					cfg->rcb_bufs[slot].cpu =
						(char *)dram_cpu + dram_used;
					cfg->rcb_bufs[slot].dma = dram_dma + dram_used;
					cfg->rcb_bufs[slot].type = RKVDEC_ALLOC_DMA;
					dram_used += sz;
				}
				cfg->rcb_count++;
			}

			pr_info("rkvdec-rcb PACKED: %ux%u total=%zu sram_cap=%zu sram_used=%zu dram_used=%zu slot4_intra=%s\n",
				width, height, total_size,
				sram_cpu ? sram_avail : 0, sram_used, dram_used,
				(rcb_count > 4 &&
				 cfg->rcb_bufs[4].type == RKVDEC_ALLOC_SRAM) ?
				"SRAM" : "DRAM");
			return 0;
		}

		/* Try ONE SRAM allocation for everything */
		if (ctx->dev->sram_pool) {
			big_cpu = gen_pool_dma_zalloc_align(ctx->dev->sram_pool,
							    total_size,
							    &big_dma,
							    SZ_4K);
			if (big_cpu) {
				/* iommu_map the whole block if domain present */
				if (rkvdec->iommu_domain) {
					big_virt = (unsigned long)big_cpu;
					ret = iommu_map(rkvdec->iommu_domain,
							big_virt,
							(phys_addr_t)big_dma,
							total_size,
							IOMMU_READ | IOMMU_WRITE,
							0);
					if (ret) {
						gen_pool_free(ctx->dev->sram_pool,
							      (unsigned long)big_cpu,
							      total_size);
						big_cpu = NULL;
					} else {
						sram_ok = true;
					}
				} else {
					sram_ok = true;
				}
			}
		}

		/* RAM fallback if SRAM path failed */
		if (!sram_ok) {
			big_cpu = dma_alloc_coherent(ctx->dev->dev,
						     total_size, &big_dma,
						     GFP_KERNEL);
			if (!big_cpu) {
				ret = -ENOMEM;
				goto err_alloc;
			}
			big_virt = (unsigned long)big_cpu;
			alloc_type = RKVDEC_ALLOC_DMA;
		} else {
			alloc_type = RKVDEC_ALLOC_SRAM;
		}

		/* Second pass: hand out sub-slices.
		 * Type reflects the ACTUAL allocation path (sram_ok), not
		 * sram_pool presence. Only slot 0 owns the underlying handle;
		 * others are aliased into the same region and freed alongside.
		 */
		for (i = 0; i < rcb_count; i++) {
			cfg->rcb_bufs[i].cpu = (char *)big_cpu + cum;
			cfg->rcb_bufs[i].dma = (alloc_type == RKVDEC_ALLOC_SRAM &&
						rkvdec->iommu_domain) ?
				big_virt + cum : big_dma + cum;
			cfg->rcb_bufs[i].type = alloc_type;
			cfg->rcb_count++;
			cum += cfg->rcb_bufs[i].size;
		}

		/* 2026-06-09 N1: which path did RCB take — on-chip SRAM or the
		 * dma_alloc_coherent DRAM fallback? (sequence-diff: BSP always SRAM
		 * via a permanent probe-time iommu_map; we fall back silently.) */
		pr_info("rkvdec-rcb: %ux%u total=%zu sram_pool=%s -> type=%s dma=0x%llx slots=%zu\n",
			width, height, total_size,
			ctx->dev->sram_pool ? "present" : "absent",
			alloc_type == RKVDEC_ALLOC_SRAM ? "SRAM" : "DRAM",
			(unsigned long long)big_dma, rcb_count);
	}
	return 0;

err_alloc:
	rkvdec_free_rcb(ctx);

	return ret;
}
