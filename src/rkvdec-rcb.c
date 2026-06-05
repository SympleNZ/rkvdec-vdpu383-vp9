// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip video decoder Rows and Cols Buffers manager
 *
 * Copyright (C) 2025 Collabora, Ltd.
 *  Detlev Casanova <detlev.casanova@collabora.com>
 */

#include "rkvdec.h"
#include "rkvdec-rcb.h"

#include <linux/iommu.h>
#include <linux/genalloc.h>
#include <linux/sizes.h>
#include <linux/types.h>

/* struct rkvdec_rcb_config moved to rkvdec-rcb.h so codec backends can
 * read the actual RCB allocation dimensions (e.g. for the Round 4.2
 * dim-mismatch reject in rkvdec_vdpu383_vp9_run). */

static size_t rkvdec_rcb_size(const struct rcb_size_info *size_info,
			      unsigned int width, unsigned int height)
{
	size_t dim_term;

	dim_term = (size_t)size_info->multiplier *
		   (size_info->axis == PIC_HEIGHT ? height : width);
	return max_t(size_t, dim_term, size_info->min_bytes);
}

dma_addr_t rkvdec_rcb_buf_dma_addr(struct rkvdec_ctx *ctx, int id)
{
	return ctx->rcb_config->rcb_bufs[id].dma;
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
	}
	return 0;

err_alloc:
	rkvdec_free_rcb(ctx);

	return ret;
}
