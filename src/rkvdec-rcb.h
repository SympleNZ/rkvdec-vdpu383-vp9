/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip video decoder Rows and Cols Buffers manager
 *
 * Copyright (C) 2025 Collabora, Ltd.
 *  Detlev Casanova <detlev.casanova@collabora.com>
 */

#include <linux/types.h>

struct rkvdec_ctx;
struct rkvdec_aux_buf;

enum rcb_axis {
	PIC_WIDTH = 0,
	PIC_HEIGHT = 1
};

struct rcb_size_info {
	u16 multiplier;		/* widened from u8 to fit VP9-required values */
	enum rcb_axis axis;
	u32 min_bytes;		/* size = max(multiplier * dim, min_bytes) */
};

/*
 * Exposed so codec backends can read the actual RCB allocation
 * dimensions (Round 4.2 dim-mismatch reject).
 */
struct rkvdec_rcb_config {
	struct rkvdec_aux_buf *rcb_bufs;
	size_t rcb_count;
	u32 width;
	u32 height;

	/*
	 * 2026-06-09 per-region SRAM packing (gated `rcb_sram_pack`, N1 follow-up).
	 * When `packed`, regions are split across TWO underlying blocks — an
	 * iommu-mapped SRAM block (high-priority row contexts) and a DRAM block
	 * (the spill) — freed explicitly from these handles instead of via slot 0.
	 */
	bool packed;
	void *sram_cpu;		/* gen_pool vaddr == iova base; NULL if no SRAM block */
	size_t sram_size;
	void *dram_cpu;
	dma_addr_t dram_dma;
	size_t dram_size;

	/*
	 * 2026-06-10 E1 (gated `rcb_cached`) — VDPU383_LIFECYCLE_MAP §3.2/3.3.
	 * BSP allocates context buffers CACHED (dma-buf-cache) and dma_sync's
	 * them to device every frame (~29/frame measured on .104, Run 2); ours
	 * defaults to uncached dma_alloc_coherent with no per-frame sync. When
	 * `cached`, the whole RCB is ONE cached dma_alloc_noncoherent DRAM block,
	 * synced to device before each kick via rkvdec_rcb_sync_for_device().
	 */
	bool cached;
	void *cached_cpu;
	dma_addr_t cached_dma;
	size_t cached_size;
};

int rkvdec_allocate_rcb(struct rkvdec_ctx *ctx, u32 width, u32 height,
			const struct rcb_size_info *size_info,
			size_t rcb_count);
dma_addr_t rkvdec_rcb_buf_dma_addr(struct rkvdec_ctx *ctx, int id);
void rkvdec_rcb_sync_for_device(struct rkvdec_ctx *ctx);
size_t rkvdec_rcb_buf_size(struct rkvdec_ctx *ctx, int id);
int rkvdec_rcb_buf_count(struct rkvdec_ctx *ctx);
bool rkvdec_rcb_buf_validate_size(struct rkvdec_ctx *ctx);
void rkvdec_free_rcb(struct rkvdec_ctx *ctx);
