// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder LINK mode — Phase 1 scaffold.
 *
 * Codec-agnostic descriptor-table allocator and fill code. Models the
 * BSP rk_vcodec link-mode mechanism (see /tmp/bsp/mpp_rkvdec2_link.c
 * + LINK_MODE_DESIGN_2026-05-25.md).
 *
 * Phase 1 has NO MMIO. Phase 2 adds the enqueue path + IRQ handler.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "rkvdec-link.h"
#include "rkvdec.h"		/* full struct rkvdec_dev for the IRQ-driven warmup */
#include <linux/jiffies.h>	/* msecs_to_jiffies */

/* Module params live in rkvdec.c. */
extern int r38_a_core_work_mode;
extern int r46_link_desc_sync;
extern int r47_link_barrier_strong;

/* ---- BSP cache offsets (mpp_rkvdec2.h:82-88) -------------------- */
#define RKVDEC_BSP_CACHE0_SIZE	0x51c
#define RKVDEC_BSP_CACHE1_SIZE	0x55c
#define RKVDEC_BSP_CACHE2_SIZE	0x59c
#define RKVDEC_BSP_CLR_CACHE0	0x510
#define RKVDEC_BSP_CLR_CACHE1	0x550
#define RKVDEC_BSP_CLR_CACHE2	0x590
#define RKVDEC_BSP_MAX_READS	0x518

#define RKVDEC_CACHE_PERMIT_CACHEABLE_ACCESS	BIT(0)
#define RKVDEC_CACHE_PERMIT_READ_ALLOCATE	BIT(1)
#define RKVDEC_CACHE_LINE_SIZE_64_BYTES		BIT(4)
#define RKVDEC_CACHE_MAX_READS_VAL		0x1c

/* ---- BSP link-mode register offsets (mpp_rkvdec2_link.h) -------- */
#define RKVDEC_BSP_LINK_CFG_ADDR	0x004
#define RKVDEC_BSP_LINK_MODE		0x008
#define RKVDEC_BSP_LINK_ADD_MODE	BIT(31)
#define RKVDEC_BSP_LINK_CFG_CTRL	0x00c
#define RKVDEC_BSP_LINK_CFG_DONE	BIT(0)
#define RKVDEC_BSP_LINK_DEC_NUM		0x010
#define RKVDEC_BSP_LINK_TOTAL_NUM	0x014
#define RKVDEC_BSP_LINK_EN		0x018
#define RKVDEC_BSP_LINK_EN_BIT		BIT(0)

/* BSP comment: "link table address requires 64 align". We use 256 for
 * cache-line headroom; 64 is the lower bound. */
#define RKVDEC_LINK_NODE_ALIGN 256

/* Sentinel pattern written to read-back slots at fill time so we can
 * tell whether HW has updated them post-decode. */
#define RKVDEC_LINK_RBACK_SENTINEL 0xffffffffu

int rkvdec_link_alloc_table(struct rkvdec_link_table *table,
			    struct device *dev,
			    const struct rkvdec_link_info *info,
			    u32 task_capacity)
{
	u32 node_bytes;
	size_t total_bytes;
	dma_addr_t iova;
	void *kva;
	u32 i;

	if (!table || !dev || !info || !task_capacity)
		return -EINVAL;
	if (info->tb_reg_num == 0 || info->tb_reg_num > 1024)
		return -EINVAL;

	node_bytes = ALIGN(info->tb_reg_num * (u32)sizeof(u32),
			   RKVDEC_LINK_NODE_ALIGN);
	total_bytes = (size_t)node_bytes * task_capacity;

	kva = dma_alloc_coherent(dev, total_bytes, &iova, GFP_KERNEL);
	if (!kva)
		return -ENOMEM;

	/* Zero-init the whole ring so unused slots (gaps in part_w
	 * coverage, read-back slots that HW will overwrite) start clean.
	 * dma_alloc_coherent is __GFP_ZERO on most platforms but the API
	 * does not guarantee it — explicit memset is cheap and correct. */
	memset(kva, 0, total_bytes);

	table->dev           = dev;
	table->info          = info;
	table->kva           = kva;
	table->iova          = iova;
	table->total_size    = total_bytes;
	table->task_capacity = task_capacity;
	table->node_size     = node_bytes;
	table->next_slot     = 0;

	/* Pre-wire next_addr AND tb_reg_r for each slot to form a circular
	 * ring with read-back pointers. Matches BSP rkvdec2_link_alloc_table
	 * (mpp_rkvdec2_link.c line 764-772): each slot's tb_reg_r position
	 * holds the IOVA of that slot's own read-back area (the part_r
	 * region), so HW knows where to write per-task status/cycle
	 * counters when the slot's frame completes.
	 *
	 * 2026-05-30 BSP comparison: previously we set only tb_reg_next
	 * here and missed tb_reg_r. fill_descriptor's memset then wiped
	 * the slot, leaving slot[tb_reg_r] = 0 — HW had no valid pointer
	 * to write readback into. We re-set both here AND in fill_descriptor
	 * (after the per-task memset).
	 */
	for (i = 0; i < task_capacity; i++) {
		u32 *node = (u32 *)((u8 *)kva + (size_t)i * node_bytes);
		dma_addr_t slot_iova = iova + (dma_addr_t)i * node_bytes;
		dma_addr_t next_iova = iova +
			(dma_addr_t)((i + 1) % task_capacity) * node_bytes;

		node[info->tb_reg_next] = lower_32_bits(next_iova);

		if (info->tb_reg_r >= 0 &&
		    (u32)info->tb_reg_r < info->tb_reg_num &&
		    info->part_r_num > 0) {
			dma_addr_t r_iova = slot_iova +
				(dma_addr_t)info->part_r[0].tb_reg_off * 4;
			node[info->tb_reg_r] = lower_32_bits(r_iova);
		}
	}

	return 0;
}

void rkvdec_link_free_table(struct rkvdec_link_table *table)
{
	if (!table || !table->kva)
		return;
	dma_free_coherent(table->dev, table->total_size,
			  table->kva, table->iova);
	table->kva = NULL;
	table->iova = 0;
	table->total_size = 0;
	table->task_capacity = 0;
}

static inline u32 *rkvdec_link_slot_ptr(struct rkvdec_link_table *table,
					u32 slot)
{
	return (u32 *)((u8 *)table->kva + (size_t)slot * table->node_size);
}

void rkvdec_link_fill_descriptor(struct rkvdec_link_table *table, u32 slot,
				 const u32 *regs)
{
	const struct rkvdec_link_info *info = table->info;
	u32 *node = rkvdec_link_slot_ptr(table, slot);
	u32 i;

	/*
	 * 2026-05-31 R19 — BSP-faithful minimal fill (mpp_rkvdec2_link.c:565).
	 *
	 * Previous fill memset the whole node (256 u32) then re-wrote next/r
	 * plus self-pointers plus part_w. Between the memset and the
	 * re-writes the descriptor briefly contained zeros for slots HW may
	 * prefetch (next_addr in particular). Even with wmb() before kick the
	 * prefetcher could race the memset.
	 *
	 * BSP approach (rkvdec2_link_prepare):
	 *   1. memcpy part_w sections
	 *   2. memset part_r areas (HW writeback regions)
	 *   3. set tb_reg_debug / seg0 / seg1 / seg2 self-pointers
	 *
	 * No whole-node wipe. tb_reg_next and tb_reg_r are set ONCE at
	 * alloc time (we already do that above in rkvdec_link_alloc_table).
	 * The header area (slots 0..7) holding next/r/debug/seg* stays
	 * stable across reuses; only the per-task data zones get rewritten.
	 *
	 * Hypothesis: HW's link-mode prefetcher reads next_addr early; a
	 * brief zero at that slot (the previous-fill memset window) makes
	 * the prefetcher follow a dangling pointer for some tasks. Tile-
	 * content decodes which are state-heavier are the ones that expose
	 * the race; non-tile content tolerates it.
	 *
	 * R17 MPP A/B and R18 mmio-indexed register diff confirmed the bug
	 * is not in any register VALUE; this changes the WRITE SEQUENCE
	 * only.
	 */

	/* Pack each writable part. */
	for (i = 0; i < info->part_w_num; i++) {
		const struct rkvdec_link_part *p = &info->part_w[i];

		if (p->reg_num == 0)
			continue;
		/* Source range must lie within the caller's flat image. */
		if (WARN_ON_ONCE(p->reg_start + p->reg_num > info->tb_reg_num))
			continue;
		memcpy(&node[p->tb_reg_off], &regs[p->reg_start],
		       p->reg_num * sizeof(u32));
	}

	/*
	 * memset HW writeback regions to zero (BSP rkvdec2_link_prepare
	 * lines 605-611). Each part_r section is where HW writes per-task
	 * status/cycle counters; clearing before each kick gives clean
	 * post-decode reads.
	 */
	for (i = 0; i < info->part_r_num; i++) {
		const struct rkvdec_link_part *p = &info->part_r[i];

		if (p->reg_num == 0)
			continue;
		if (WARN_ON_ONCE(p->tb_reg_off + p->reg_num > info->tb_reg_num))
			continue;
		memset(&node[p->tb_reg_off], 0, p->reg_num * sizeof(u32));
	}

	/* Per BSP rkvdec2_link_prepare (mpp_rkvdec2_link.c:614-621): the
	 * descriptor must hold IOVAs pointing into itself at the read-back
	 * area and at each write-block. Without these, HW dereferences
	 * NULL pointers for where to fetch register chunks from and
	 * page-faults at IOVA 0. */
	{
		dma_addr_t slot_iova = table->iova +
			(dma_addr_t)slot * table->node_size;

		/*
		 * NB 2026-06-01: tested making next_addr SELF-pointing to bound
		 * the ring-runaway — made it WORSE (HW loops forever on the
		 * slot, never completes/IRQs: every frame timed out). HW does
		 * not honor frame_num=1 to stop on a self-loop. Keep the
		 * circular next_addr from alloc_table.
		 */

		if (info->tb_reg_debug >= 0 &&
		    (u32)info->tb_reg_debug < info->tb_reg_num &&
		    info->part_r_num > 0)
			node[info->tb_reg_debug] = lower_32_bits(
				slot_iova + info->part_r[0].tb_reg_off * 4);
		if (info->tb_reg_seg0 >= 0 &&
		    (u32)info->tb_reg_seg0 < info->tb_reg_num &&
		    info->part_w_num > 0)
			node[info->tb_reg_seg0] = lower_32_bits(
				slot_iova + info->part_w[0].tb_reg_off * 4);
		if (info->tb_reg_seg1 >= 0 &&
		    (u32)info->tb_reg_seg1 < info->tb_reg_num &&
		    info->part_w_num > 1)
			node[info->tb_reg_seg1] = lower_32_bits(
				slot_iova + info->part_w[1].tb_reg_off * 4);
		if (info->tb_reg_seg2 >= 0 &&
		    (u32)info->tb_reg_seg2 < info->tb_reg_num &&
		    info->part_w_num > 2)
			node[info->tb_reg_seg2] = lower_32_bits(
				slot_iova + info->part_w[2].tb_reg_off * 4);
	}

	/*
	 * 2026-05-30 BSP-alignment: BSP rkvdec2_link_prepare (line 605-611)
	 * memsets the part_r region to ZERO, not a sentinel. The whole-node
	 * memset at the top of this function already leaves tb_reg_int=0,
	 * so the explicit 0xffffffff sentinel write was an over-divergence
	 * from BSP that we don't actually need: the IRQ handler's
	 * task_done = (status != 0xffffffff && status != 0) check
	 * collapses to (status != 0) when sentinel is 0, which is exactly
	 * what BSP does in rkvdec2_link_try_dequeue line 1119-1120:
	 *   irq_status = tb_reg[info->tb_reg_int];
	 *   task_done = irq_status || timeout_flag || abort_flag;
	 *
	 * Leaving tb_reg_int = 0 (post-memset) is BSP-faithful.
	 */

	/* Ensure CPU writes are visible before HW could read. dma_coherent
	 * memory does not need explicit cache flush, but a wmb() is cheap
	 * and matches BSP's wmb() before its CFG_DONE write. */
	wmb();

	/*
	 * R46 (2026-06-01) — explicit dma_sync_single_for_device on the
	 * descriptor slot. dma_alloc_coherent should produce coherent
	 * memory but rk3576's IOMMU may not fully participate in the
	 * coherency domain. LINK_MODE_BASELINE_2026-05-30 hypothesised
	 * cache-coherency between descriptor write and HW read as the
	 * most likely cause of link mode regression (HW receives
	 * byte-identical regs but produces wrong output).
	 *
	 * Gated on r46_link_desc_sync module param. Sync entire slot range.
	 */
	if (r46_link_desc_sync && table->dev) {
		dma_addr_t slot_iova = table->iova +
				       (dma_addr_t)slot * table->node_size;

		dma_sync_single_for_device(table->dev, slot_iova,
					   table->node_size,
					   DMA_TO_DEVICE);
	}
}

u32 rkvdec_link_read_task_status(struct rkvdec_link_table *table, u32 slot)
{
	const struct rkvdec_link_info *info = table->info;
	const u32 *node = rkvdec_link_slot_ptr(table, slot);

	if (info->tb_reg_int >= info->tb_reg_num)
		return RKVDEC_LINK_RBACK_SENTINEL;
	/* HW writes via DMA; mirror with a read barrier so we don't see
	 * a stale cached value. */
	rmb();
	return node[info->tb_reg_int];
}

/* ----- Diagnostic dump ---------------------------------------------- */

static void rkvdec_link_dump_range(const char *prefix, const char *label,
				   const u32 *base, u32 start, u32 count)
{
	char line[160];
	u32 written = 0;
	u32 i;

	if (count == 0)
		return;
	for (i = 0; i < count; i++) {
		int n;

		if ((i & 0x7) == 0) {
			if (i > 0)
				pr_info("%s%s\n", prefix, line);
			n = scnprintf(line, sizeof(line),
				      "  %s reg%03u: ", label, start + i);
			written = n;
		}
		n = scnprintf(line + written, sizeof(line) - written,
			      "%08x ", base[i]);
		written += n;
	}
	if (written > 0)
		pr_info("%s%s\n", prefix, line);
}

void rkvdec_link_dump_descriptor(struct rkvdec_link_table *table, u32 slot,
				 const char *prefix)
{
	const struct rkvdec_link_info *info = table->info;
	const u32 *node = rkvdec_link_slot_ptr(table, slot);
	dma_addr_t node_iova;
	u32 i;

	if (!prefix)
		prefix = "rkvdec-link:";
	node_iova = table->iova + (dma_addr_t)slot * table->node_size;

	pr_info("%s slot %u iova %pad node_size %u\n",
		prefix, slot, &node_iova, table->node_size);
	pr_info("%s   header  next=%08x rback=%08x int=%08x\n",
		prefix,
		node[info->tb_reg_next],
		node[info->tb_reg_r],
		(info->tb_reg_int < info->tb_reg_num)
			? node[info->tb_reg_int]
			: 0u);

	for (i = 0; i < info->part_w_num; i++) {
		const struct rkvdec_link_part *p = &info->part_w[i];
		char label[16];

		scnprintf(label, sizeof(label), "part_w[%u]", i);
		rkvdec_link_dump_range(prefix, label,
				       &node[p->tb_reg_off],
				       p->reg_start, p->reg_num);
	}
}

/* ----- Phase 2: cache clear ---------------------------------------- *
 *
 * Mirrors BSP rkvdec2_clear_cache (mpp_rkvdec2_link.c:455). Called
 * ONCE per session — when LINK_EN_BASE reads 0, indicating an empty
 * queue. Must not be combined with mainline 0x41x writes.
 */
void rkvdec_link_clear_caches_bsp(void __iomem *link_base,
				  bool cache_line_size_64)
{
	u32 cfg = RKVDEC_CACHE_PERMIT_CACHEABLE_ACCESS |
		  RKVDEC_CACHE_PERMIT_READ_ALLOCATE;

	if (cache_line_size_64)
		cfg |= RKVDEC_CACHE_LINE_SIZE_64_BYTES;

	writel_relaxed(cfg, link_base + RKVDEC_BSP_CACHE0_SIZE);
	writel_relaxed(cfg, link_base + RKVDEC_BSP_CACHE1_SIZE);
	writel_relaxed(cfg, link_base + RKVDEC_BSP_CACHE2_SIZE);

	writel_relaxed(1, link_base + RKVDEC_BSP_CLR_CACHE0);
	writel_relaxed(1, link_base + RKVDEC_BSP_CLR_CACHE1);
	writel_relaxed(1, link_base + RKVDEC_BSP_CLR_CACHE2);

	writel_relaxed(RKVDEC_CACHE_MAX_READS_VAL,
		       link_base + RKVDEC_BSP_MAX_READS);
}

/* ----- Phase 2: enqueue -------------------------------------------- *
 *
 * Implements BSP rkvdec2_link_enqueue (mpp_rkvdec2_link.c:476). For
 * Phase 2 we issue ONE descriptor at a time — the caller flushes the
 * IOMMU TLB before calling this, and the IRQ handler reaps the result.
 */
int rkvdec_link_enqueue_vdpu383(struct rkvdec_link_table *table, u32 slot,
				u32 frame_num, void __iomem *link_base)
{
	const struct rkvdec_link_info *info;
	dma_addr_t slot_iova;
	u32 link_en;
	u32 link_mode;

	if (!table || !table->kva || !link_base)
		return -EINVAL;
	if (slot >= table->task_capacity)
		return -EINVAL;
	if (frame_num == 0 || frame_num > table->task_capacity)
		return -EINVAL;

	info = table->info;
	slot_iova = table->iova + (dma_addr_t)slot * table->node_size;

	/*
	 * 2026-06-01 link-mode-port Phase 1b — real bootstrap-vs-append,
	 * faithful to BSP rkvdec2_link_enqueue (mpp_rkvdec2_link.c:488-529).
	 *
	 * History (2026-05-27, v0.16-v0.19): the always-bootstrap path was a
	 * stopgap because single-task-per-device_run append was "fast but
	 * broken". That diagnosis was a symptom of the wrong submission model,
	 * not the append sequence itself (see LINK_MODE_PORT_DESIGN_2026-06-01
	 * §1). The cure is genuine pipelining: device_run fills one slot and
	 * appends while HW stays armed, so HW transitions frame→frame without
	 * the enable/disable cycle. This function now does the real thing.
	 *
	 * Read LINK_EN and branch:
	 *   - LINK_EN == 0 (empty queue): BOOTSTRAP — clear cache, zero
	 *     LINK_MODE, CFG_DONE, LINK_EN, point CFG_ADDR at this slot,
	 *     LINK_MODE = frame_num.
	 *   - LINK_EN == 1 (HW still running an earlier queue): APPEND —
	 *     LINK_MODE = frame_num | ADD_MODE. Do NOT rewrite CFG_ADDR or
	 *     LINK_EN; HW follows the descriptor ring's next_addr (wired
	 *     circularly at alloc time) into the slot we just filled.
	 *
	 * Dead-code-safe under the current completion-time-job_finish model:
	 * HW drains and clears LINK_EN between frames there, so LINK_EN always
	 * reads 0 and only the bootstrap branch runs — byte-identical to the
	 * old always-bootstrap behaviour. The append branch goes live only
	 * once the model flip (job_finish-at-submit + job_ready depth>1) lands.
	 */
	link_en = readl(link_base + RKVDEC_BSP_LINK_EN);

	if (!link_en) {
		/* First task into an empty queue. Bootstrap sequence per
		 * BSP: zero LINK_MODE, CFG_DONE, LINK_EN, descriptor IOVA.
		 *
		 * 2026-06-01: do NOT call rkvdec_link_clear_caches_bsp here.
		 * BSP clears the cache via its CORE register window
		 * (mpp->reg_base + 0x51x); our equivalent and proven-working
		 * cache config is the UNCONDITIONAL writel(rkvdec->regs +
		 * 0x41c/0x410/0x418) in vp9_run, which already runs for link
		 * mode. The link-window 0x51x writes target the wrong window
		 * and session-s found BSP-style 0x51x cache writes actively
		 * REGRESS Fluster (89.5 vs 98) — they disrupt HW state right
		 * before the kick, undoing the good 0x41x config. Suspected
		 * cause of link-mode silent-wrong-decode (R2/R3).
		 */
		writel(0, link_base + RKVDEC_BSP_LINK_MODE);
		wmb();
		writel(RKVDEC_BSP_LINK_CFG_DONE,
		       link_base + RKVDEC_BSP_LINK_CFG_CTRL);
		wmb();
		writel(RKVDEC_BSP_LINK_EN_BIT,
		       link_base + RKVDEC_BSP_LINK_EN);
		writel_relaxed(lower_32_bits(slot_iova),
			       link_base + RKVDEC_BSP_LINK_CFG_ADDR);
		link_mode = frame_num;
	} else {
		/* HW still armed on an earlier queue — append this slot.
		 * ADD_MODE tells HW to add frame_num to its running total
		 * and chain through next_addr; CFG_ADDR / LINK_EN untouched. */
		link_mode = frame_num | RKVDEC_BSP_LINK_ADD_MODE;
	}

	writel_relaxed(link_mode, link_base + RKVDEC_BSP_LINK_MODE);

	/* IP enable per task (BSP ip_en_base=0x58, ip_en_val=0x01000000). */
	if (info->ip_en_base)
		writel_relaxed(info->ip_en_val,
			       link_base + info->ip_en_base);

	wmb();

	/*
	 * R47 (2026-06-01) — LINK_MODE_BASELINE hypothesis #4:
	 * stronger barrier before CFG_DONE. Single-shot path does
	 * `wmb() + readl(reg15)` before its kick to drain posted
	 * writes. Link mode does only wmb(). Add a readl-back of
	 * a link register we just wrote to flush all posted writes
	 * through the bus before CFG_DONE.
	 */
	if (r47_link_barrier_strong) {
		(void)readl(link_base + info->ip_en_base);
		(void)readl(link_base + RKVDEC_BSP_LINK_MODE);
		mb();
	}

	/*
	 * R38-A (2026-06-01) — write CORE_WORK_MODE | CCU_WORK_MODE
	 * (= 0x30000) to link_base + 0x48 before the kick.
	 *
	 * BSP rkvdec2_link_run does this per task as the first thing
	 * in the run path (mpp_rkvdec2_link.c:1987-1988). Our pre-R38
	 * path never wrote 0x30000 to 0x48 — we only touched 0x48 at
	 * IRQ-ack time to clear pending bits.
	 *
	 * Module-param gated for clean A/B against the 12-trial
	 * bistability harness.
	 */
	if (r38_a_core_work_mode) {
		writel_relaxed(0x30000u, link_base + 0x48);
		wmb();
	}

	/* CFG_DONE — HW commits config. */
	writel(RKVDEC_BSP_LINK_CFG_DONE,
	       link_base + RKVDEC_BSP_LINK_CFG_CTRL);

	/* Only the bootstrap path arms the HW; append leaves LINK_EN set
	 * (BSP mpp_rkvdec2_link.c:524-529). */
	if (!link_en) {
		wmb();
		writel(RKVDEC_BSP_LINK_EN_BIT,
		       link_base + RKVDEC_BSP_LINK_EN);
	}

	return 0;
}

/* ----- Phase 2: link IRQ check ------------------------------------- *
 *
 * Mirrors BSP rkvdec_vdpu383_link_irq (mpp_rkvdec2_link.c:702). Reads
 * the link IRQ status at 0x48 (irq_base) and the per-task status at
 * 0x4c (status_base). Returns true if a link IRQ fired and clears the
 * IRQ + status to acknowledge.
 *
 * NOTE: BSP checks `irq_val & (irq_mask >> 16)` = irq_val & 0x3 — i.e.
 * bits 0,1 of the read register. This contrasts with our mainline
 * single-shot IRQ which checks BIT(0)=DEC_RDY_STA in 0x4c. The two
 * paths look at different aspects of the same register bank: link
 * mode uses the low 2 bits as "completion fired" / "queue empty",
 * single-shot uses the same bits as "decode ready" / "line IRQ".
 */
bool rkvdec_link_vdpu383_irq_check(void __iomem *link_base,
				   u32 *irq_val_out, u32 *status_out)
{
	/* Kept for callers that want raw register state. The real
	 * completion signal in link mode is via LINK_DEC_NUM advance,
	 * not via 0x4c status bits — use rkvdec_link_check_completion()
	 * for that. */
	u32 irq_val = readl_relaxed(link_base + 0x48);
	u32 status  = readl_relaxed(link_base + 0x4c);

	if (irq_val_out)
		*irq_val_out = irq_val;
	if (status_out)
		*status_out = status;

	return (status & 0x3ff) != 0;
}

u32 rkvdec_link_check_completion(struct rkvdec_link_table *table,
				 void __iomem *link_base)
{
	u32 dec_num;

	if (!table || !link_base)
		return 0;

	/* LINK_DEC_NUM_BASE = 0x010 (BSP). HW increments after each task
	 * it finishes. Counter is NOT strictly monotonic on this SoC
	 * variant: HW appears to soft-reset between certain frames
	 * (observed dec_num bouncing 1 -> 0 -> 1 across IRQs on allkey
	 * content). Detect the reset: if HW counter is lower than what
	 * we last saw, treat as "no progress" (dec_num < expected) AND
	 * reset our tracker so the next genuine completion is recorded
	 * correctly. */
	dec_num = readl_relaxed(link_base + 0x010);

	if (dec_num == table->expected_dec_num)
		return 0;

	if (dec_num < table->expected_dec_num) {
		/* HW reset between frames. Re-sync. */
		table->expected_dec_num = dec_num;
		return 0;
	}

	{
		u32 delta = dec_num - table->expected_dec_num;
		table->expected_dec_num = dec_num;
		return delta;
	}
}

/* ----- VDPU383 link info ------------------------------------------- *
 *
 * Mirrors BSP `rkvdec_link_vdpu383_hw_info` from mpp_rkvdec2_link.c
 * line 213. Used by HEVC, H.264, VP9, AV1 on VDPU383 — same shape, the
 * codec-specific bits live in the codec's per-frame regs (which we
 * pack into the descriptor via rkvdec_link_fill_descriptor).
 *
 * The MMIO offsets (irq_base 0x48 / status_base 0x4c / ip_en_base
 * 0x58) align with what our rkvdec-vdpu383-regs.h already defines as
 * VDPU383_LINK_INT_EN / VDPU383_LINK_STA_INT / VDPU383_LINK_IP_ENABLE.
 * The single-shot IRQ handler reads them with mask BIT(0) — link mode
 * uses the upper-16 mask space (0x30000 / 0x3ff0000).
 */
const struct rkvdec_link_info rkvdec_link_vdpu383_info = {
	.tb_reg_num   = 256,
	.tb_reg_next  = 0,
	.tb_reg_r     = 1,
	.tb_reg_int   = 16,
	.tb_reg_debug = 2,	/* slot points at part_r[0] start in descriptor */
	.tb_reg_seg0  = 3,	/* slot points at part_w[0] (reg8-31) start  */
	.tb_reg_seg1  = 4,	/* slot points at part_w[1] (reg64-107) start */
	.tb_reg_seg2  = 5,	/* slot points at part_w[2] (reg128-235) start */

	.part_w_num = 3,
	.part_r_num = 2,
	.part_w[0] = { .tb_reg_off =  80, .reg_start =   8, .reg_num =  24 },
	.part_w[1] = { .tb_reg_off = 104, .reg_start =  64, .reg_num =  44 },
	.part_w[2] = { .tb_reg_off = 148, .reg_start = 128, .reg_num = 108 },
	.part_r[0] = { .tb_reg_off =  16, .reg_start =  15, .reg_num =   1 },
	.part_r[1] = { .tb_reg_off =  20, .reg_start = 320, .reg_num =  40 },

	.next_addr_base = 0x20,
	.ip_reset_base  = 0x44,
	.ip_reset_en    = BIT(0),
	.irq_base       = 0x48,
	.irq_mask       = 0x30000,
	.status_base    = 0x4c,
	.status_mask    = 0x3ff0000,
	.err_mask       = 0x3fe,
	.ip_time_base   = 0x54,
	.en_base        = 0x40,
	.ip_en_base     = 0x58,
	.ip_en_val      = 0x01000000,	/* VDPU383_IP_CRU_MODE = BIT(24) */
	.en_sw_iommu_zap = true,
};

/* =====================================================================
 *  RK3576 HW WARMUP — port of BSP's rk3576_workaround_init + run.
 *
 *  Discovered via disassembly of the BSP kernel's
 *  rk3576_workaround_run (412 B at 0xffffffc00881aa74) and
 *  rk3576_workaround_init (544 B at 0xffffffc00881a810) on R76S #2
 *  (BSP eMMC, kernel 6.1.141). The BSP wires these onto
 *  rkvdec_rk3576_hw_ops as .init/.hack_run and runs them ONCE at
 *  rkvdec2_probe_default — populating + executing a pre-canned test
 *  descriptor through the link-mode pipeline to establish IP-internal
 *  state required for normal decode operation.
 *
 *  Without this warmup the IP comes up in an indeterminate state that
 *  works for simple VP9 content but fails on tile-coded content (the
 *  full body of evidence: R17 / R18 / R20 / R22 byte-identical input
 *  data yet our decode fails).
 *
 *  See SESSION_2026-06-01_R22_PROB_DIFF.md for the discovery trail.
 *  See .local/workaround_run.S + workaround_init.S for the kcore
 *  disassembly this is transcribed from.
 * ===================================================================== */

/*
 * Static header bytes (64 B) copied from the BSP kernel's .rodata
 * (extracted from /proc/kcore at virtual address 0xffffffc00937ac68).
 * The init writes these as 8 consecutive u64s into the fix DMA buffer
 * header region. Treat as opaque IP-init blob.
 */
static const u8 rkvdec_rk3576_warmup_hdr[64] = {
	0x00, 0x00, 0x01, 0x65, 0x88, 0x82, 0x0b, 0x01,
	0x2f, 0x08, 0xc5, 0x00, 0x01, 0x51, 0x78, 0xe0,
	0x00, 0x24, 0xf7, 0x1c, 0x00, 0x04, 0xcc, 0xeb,
	0x89, 0xd7, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x40, 0x26, 0x00, 0x10, 0x04, 0x08, 0x00, 0x08,
	0x80, 0x01, 0x00, 0x00, 0x00, 0x40, 0x01, 0xd8,
	0x07, 0x7c, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Sentinel pattern (0x76543210 / 0x76543212) — "rk" recognition marker
 * the IP looks for in the warmup descriptor. */
#define RK3576_WARMUP_SENTINEL_HI       0x76543210u
#define RK3576_WARMUP_SENTINEL_HI_PLUS2 0x76543212u

/* The fix buffer size — BSP allocates 8 KiB. Descriptor lives at +0x1000. */
#define RK3576_WARMUP_BUF_SIZE          0x2000u
#define RK3576_WARMUP_DESC_OFF          0x1000u

/*
 * Populate the warmup DMA buffer with the test descriptor.
 *
 *   buf  Kernel virtual address of the DMA-coherent buffer (already zeroed)
 *   iova IOMMU IOVA of the same buffer
 *
 * All offsets and constants transcribed verbatim from the
 * rk3576_workaround_init disassembly. fix_iova_lo32 is the low 32 bits
 * of the buffer's IOVA (the BSP stores u32 register values).
 */
static void rkvdec_rk3576_warmup_populate(void *buf, dma_addr_t iova)
{
	u8  *b8  = buf;
	u32 *b32 = buf;
	u32  iova_lo = lower_32_bits(iova);

	/* Header table: bytes 0..31 from rodata, gap at 32..63, then
	 * bytes 32..47 of rodata at fix[64..79], bytes 48..55 at fix[80..87],
	 * bytes 56..59 at fix[88..91]. (BSP disasm lines 38..66.) */
	memcpy(&b8[0],  &rkvdec_rk3576_warmup_hdr[0],  32);
	memcpy(&b8[64], &rkvdec_rk3576_warmup_hdr[32], 16);
	memcpy(&b8[80], &rkvdec_rk3576_warmup_hdr[48],  8);
	memcpy(&b8[88], &rkvdec_rk3576_warmup_hdr[56],  4);

	/* Descriptor body. Offsets in BYTES from fix_vaddr.
	 * b32[N/4] is the u32 at byte offset N. */
	b32[4128 / 4] = 0x00000001u;
	b32[4148 / 4] = 0x0000ffffu;
	b32[4160 / 4] = 0x00000101u;
	/* 64-bit pattern split into two u32 (LE: low half first) */
	b32[4176 / 4] = 0xffffffffu;
	b32[4180 / 4] = 0x3ff3ffffu;

	b32[4352 / 4] = RK3576_WARMUP_SENTINEL_HI;   /* low u32 of 0x76543210 */
	b32[4356 / 4] = 0x00000000u;
	b32[4360 / 4] = 0x00000020u;                  /* low u32 of 0xa800000020 */
	b32[4364 / 4] = 0x000000a8u;                  /* high u32 */
	b32[4368 / 4] = 0x00000002u;                  /* low u32 of 0x200000002 */
	b32[4372 / 4] = 0x00000002u;                  /* high u32 */
	b32[4376 / 4] = 0x00000040u;

	/* IOVA-based descriptor pointers (low 32 bits, BSP only writes u32).
	 * The +0xNNNN are byte offsets into the fix buffer itself, forming
	 * self-pointers HW dereferences. Matches our standard link-table
	 * self-pointer concept (debug/seg0/1/2). */
	b32[4608 / 4] = iova_lo;
	b32[4612 / 4] = iova_lo + 0x140u;
	b32[4620 / 4] = iova_lo + 0x040u;
	b32[4656 / 4] = iova_lo + 0x240u;
	b32[4660 / 4] = 0x000000c0u;
	b32[4688 / 4] = iova_lo + 0x340u;
	b32[4692 / 4] = 0x00000200u;
	b32[4768 / 4] = iova_lo + 0x540u;
	b32[4960 / 4] = iova_lo + 0xb40u;
	b32[4100 / 4] = RK3576_WARMUP_SENTINEL_HI_PLUS2;
	b32[4096 / 4] = iova_lo + 0x540u;
	b32[4104 / 4] = iova_lo + 0x1400u;
	b32[4108 / 4] = iova_lo + 0x1020u;
	b32[4112 / 4] = iova_lo + 0x1100u;
	b32[4116 / 4] = iova_lo + 0x1200u;

	/* Ensure all stores reach RAM before HW kicks. */
	wmb();
}

/*
 * Allocate + populate the warmup buffer. Returns a pointer the caller
 * stashes in rkvdec_dev and frees at module exit via the matching
 * rkvdec_rk3576_warmup_free.
 *
 * Returns 0 on success, negative errno on failure.
 *
 * NB: This function does NOT trigger the HW kick — caller should run
 * rkvdec_rk3576_warmup_run AFTER IRQ is registered.
 */
int rkvdec_rk3576_warmup_alloc(struct device *dev,
			       void **out_cpu, dma_addr_t *out_dma)
{
	void *cpu;
	dma_addr_t dma;

	cpu = dma_alloc_coherent(dev, RK3576_WARMUP_BUF_SIZE, &dma, GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;

	memset(cpu, 0, RK3576_WARMUP_BUF_SIZE);
	rkvdec_rk3576_warmup_populate(cpu, dma);

	*out_cpu = cpu;
	*out_dma = dma;
	return 0;
}

void rkvdec_rk3576_warmup_free(struct device *dev,
			       void *cpu, dma_addr_t dma)
{
	if (cpu)
		dma_free_coherent(dev, RK3576_WARMUP_BUF_SIZE, cpu, dma);
}

#include <linux/delay.h>  /* usleep_range, mdelay */

/*
 * Run the warmup. Writes the BSP rk3576_workaround_run register
 * sequence — kicks the test descriptor and waits for completion.
 *
 *   link_base  MMIO base of the link mode register bank
 *               (same address rkvdec_link_enqueue uses).
 *   buf_iova   IOVA of the buffer populated by warmup_alloc.
 *
 * Returns 0 on clean completion, -EIO if the IP reported an error
 * (status err_mask bits set), -ETIMEDOUT if HW didn't finish.
 *
 * Caller responsibility: clocks + power domain must be ON.
 */
int rkvdec_rk3576_warmup_run(void __iomem *link_base, dma_addr_t buf_iova)
{
	u32 status;
	int i;

	/* Verbatim sequence from rk3576_workaround_run disassembly. */
	writel(0x8000u,      link_base + 0x58); /* ip_en_base, BIT(15) only  */
	writel(0x7ffffu,     link_base + 0x54); /* ip_time_base watchdog     */
	writel(0x10001u,     link_base + 0x00); /* mystery init register     */
	writel(lower_32_bits(buf_iova) + 0x1000u,
	       link_base + 0x04);               /* CFG_ADDR -> descriptor    */
	writel(0x1u, link_base + 0x08);         /* LINK_MODE = 1             */
	writel(0x1u, link_base + 0x18);         /* LINK_EN   = 1             */
	dsb(st);
	dmb(oshst);
	writel(0x1u, link_base + 0x0c);         /* CFG_DONE -> HW commits    */

	/* BSP calls a delay before polling status. Loop until status reports
	 * something or we hit ~21 ms (matches the BSP's msleep target). */
	for (i = 0; i < 200; i++) {
		usleep_range(100, 150);
		status = readl(link_base + 0x4c);
		if (status)
			break;
	}

	/* The disassembly reads status, then `eor x0, x0, x0` (XORs with
	 * itself = 0) then `cbnz x0, .` — an infinite loop only entered if
	 * status was non-zero AND the XOR result was non-zero, which can't
	 * happen. So the post-read is just a memory barrier construct. We
	 * just consume the read here. */
	(void)status;

	/* Cleanup — BSP writes 0xffff0000 to irq + status, zeroes the
	 * mystery reg / LINK_MODE / LINK_EN / ip_en_base. */
	writel(0xffff0000u, link_base + 0x48);
	writel(0xffff0000u, link_base + 0x4c);
	writel(0x0u, link_base + 0x00);
	writel(0x0u, link_base + 0x08);
	writel(0x0u, link_base + 0x18);
	dmb(oshst);
	writel(0x0u, link_base + 0x58);

	if (i >= 200)
		return -ETIMEDOUT;
	if (status & 0x3fe)
		return -EIO;
	return 0;
}

/*
 * 2026-06-08 Variant B — IRQ-driven warmup (Nicolas's lore suggestion: reap by
 * IRQ, not a manual poll). Same kick sequence as rkvdec_rk3576_warmup_run, but
 * arms the link IRQ (INT_EN bit0) and sleeps on rkvdec->warmup_irq_done instead
 * of busy-polling reg 0x4c. The top-level handler (rkvdec_irq_top), guarded by
 * warmup_irq_inflight, disables INT_EN + completes us. The completion TIMEOUT is
 * the can't-hang fallback — if the IRQ never arrives we still tear down and
 * return, exactly like the polled path. Caller: clocks + power must be ON, and
 * the IRQ must be registered (true at runtime_resume; at probe it may time-out
 * and fall back, which is harmless).
 *
 * Returns 0 on clean IRQ completion, -ETIMEDOUT if the IRQ didn't fire in time
 * (warmup HW still ran; teardown done), -EIO on HW error status.
 */
int rkvdec_rk3576_warmup_run_irq(struct rkvdec_dev *rkvdec)
{
	void __iomem *link_base = rkvdec->link;
	dma_addr_t buf_iova = rkvdec->rk3576_warmup_dma;
	unsigned long left;
	u32 status;

	reinit_completion(&rkvdec->warmup_irq_done);
	rkvdec->warmup_irq_inflight = true;

	writel(0x8000u,      link_base + 0x58); /* ip_en_base, BIT(15) only  */
	writel(0x7ffffu,     link_base + 0x54); /* ip_time_base watchdog     */
	writel(0x10001u,     link_base + 0x00); /* mystery init register     */
	writel(lower_32_bits(buf_iova) + 0x1000u,
	       link_base + 0x04);               /* CFG_ADDR -> descriptor    */
	writel(0x1u, link_base + 0x08);         /* LINK_MODE = 1             */
	writel(0x1u, link_base + 0x18);         /* LINK_EN   = 1             */
	/* arm ALL link INT_EN bits (write-mask-16: mask=0xffff, val=0xffff) in
	 * case the warmup completion signals on a bit other than IRQ bit0. */
	writel(0xffffffffu, link_base + 0x048);
	dsb(st);
	dmb(oshst);
	writel(0x1u, link_base + 0x0c);         /* CFG_DONE -> HW commits    */

	left = wait_for_completion_timeout(&rkvdec->warmup_irq_done,
					   msecs_to_jiffies(50));

	/* Read status before teardown (handler leaves 0x4c intact). */
	status = readl(link_base + 0x4c);

	/* If the IRQ never fired, the handler didn't clear inflight — do it. */
	rkvdec->warmup_irq_inflight = false;

	/* Teardown — identical to the polled path. */
	writel(0xffff0000u, link_base + 0x48);
	writel(0xffff0000u, link_base + 0x4c);
	writel(0x0u, link_base + 0x00);
	writel(0x0u, link_base + 0x08);
	writel(0x0u, link_base + 0x18);
	dmb(oshst);
	writel(0x0u, link_base + 0x58);

	if (!left)
		return -ETIMEDOUT;
	if (status & 0x3fe)
		return -EIO;
	return 0;
}
